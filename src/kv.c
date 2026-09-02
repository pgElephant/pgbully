/*-------------------------------------------------------------------------
 *
 * kv.c
 *      A leader-owned key/value store for pgBully.
 *
 * The Bully algorithm settles which node leads and stops there: there is no
 * quorum and no replicated log to build a consensus store on.  What it does
 * give us is a single agreed leader and a monotonic term, and that is enough
 * for a store with clearly stated -- and clearly weaker -- guarantees:
 *
 *   writes       are accepted only on the leader.  A write on a follower is
 *                refused rather than forwarded, so a caller always knows
 *                which node its data went to.
 *
 *   ordering     every accepted write takes the next version from a counter
 *                the leader owns, and carries the term it was written under.
 *                A peer applies a row only if the term is at least its own
 *                and the version is newer than what it already holds, so a
 *                deposed leader's late writes are dropped on arrival.
 *
 *   replication  the writing backend pushes the row to every reachable peer
 *                before returning.  Best effort: an unreachable peer does not
 *                fail the write.
 *
 *   catch-up     the leader advertises its version on every heartbeat.  A
 *                follower that finds itself behind pulls the rows it missed
 *                from the leader before answering a read, so a peer that was
 *                down or partitioned heals itself.
 *
 *   reads        are local, and therefore may be stale by up to one
 *                heartbeat.
 *
 * What this is not: quorum-replicated.  A write is durable on the leader and
 * best-effort everywhere else, so a partition can strand recent writes on the
 * side that loses the election.  Do not use it for anything that must not be
 * lost.  It is meant for cluster-scoped configuration -- which node is
 * active, where a job should run -- that any node can read without a round
 * trip.
 *
 * The rows live in the pgbully.kv table.  The version counter lives in shared
 * memory, re-seeded from the table on first use after a restart, so the
 * worker can advertise it without holding a database connection of its own.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/json.h"

#include "compat.h"
#include "pgbully.h"

/* public surface */
PG_FUNCTION_INFO_V1(pgbully_kv_put);
PG_FUNCTION_INFO_V1(pgbully_kv_get);
PG_FUNCTION_INFO_V1(pgbully_kv_delete);
PG_FUNCTION_INFO_V1(pgbully_kv_exists);
PG_FUNCTION_INFO_V1(pgbully_kv_list_keys);
PG_FUNCTION_INFO_V1(pgbully_kv_get_stats);
PG_FUNCTION_INFO_V1(pgbully_kv_compact);
PG_FUNCTION_INFO_V1(pgbully_kv_reset);
PG_FUNCTION_INFO_V1(pgbully_kv_sync);

/* inbound replication handlers, called by the leader over libpq */
PG_FUNCTION_INFO_V1(pgbully_rpc_kv_apply);
PG_FUNCTION_INFO_V1(pgbully_rpc_kv_since);

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static void
require_shmem(void)
{
    if (!pgbully_attach_shmem())
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pgbully is not active"),
                 errhint("Add \"pgbully\" to shared_preload_libraries and "
                         "restart the server.")));
}

/* True if this node currently holds leadership. */
static bool
is_leader(void)
{
    bool        leader;

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    leader = (PgbCtl->state == PGB_LEADER &&
              PgbCtl->leader_id == PgbCtl->my_node_id);
    LWLockRelease(PgbCtl->lock);

    return leader;
}

/*
 * Refuse a write on a follower, naming the leader so the caller knows where
 * to send it.
 */
static void
require_leader(const char *what)
{
    int32       leader;

    if (is_leader())
        return;

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    leader = PgbCtl->leader_id;
    LWLockRelease(PgbCtl->lock);

    if (leader == PGBULLY_NO_LEADER)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cannot %s: this node is not the leader and no "
                        "leader is known", what),
                 errhint("Wait for an election to settle, then retry.")));

    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot %s: this node is not the leader", what),
             errdetail("Node %d currently leads the cluster.", leader),
             errhint("Key/value writes must be issued on the leader.")));
}

/*
 * The highest version stored in the table.  Used once per backend to seed the
 * shared counter after a restart, since the worker cannot read the table.
 */
static int64
max_stored_version(void)
{
    int64       v = 0;
    int         rc;

    rc = SPI_execute("SELECT coalesce(max(version), 0) FROM pgbully.kv", true, 1);
    if (rc == SPI_OK_SELECT && SPI_processed == 1)
    {
        bool        isnull;
        Datum       d = SPI_getbinval(SPI_tuptable->vals[0],
                                      SPI_tuptable->tupdesc, 1, &isnull);

        if (!isnull)
            v = DatumGetInt64(d);
    }
    return v;
}

/*
 * Re-seed the shared version counter from the table.
 *
 * Shared memory does not survive a restart but the table does, so the first
 * backend to touch the store after startup has to put the counter back.  It
 * matters beyond bookkeeping: until this runs the leader would advertise
 * version 0 on its heartbeats, and every follower would conclude it was
 * already up to date.  Caller must be inside SPI.
 */
static void
ensure_seeded(void)
{
    int64       seed;
    bool        needed;

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    needed = !PgbCtl->kv_seeded;
    LWLockRelease(PgbCtl->lock);

    if (!needed)
        return;

    seed = max_stored_version();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    if (!PgbCtl->kv_seeded)
    {
        if (seed > PgbCtl->kv_version)
            PgbCtl->kv_version = seed;
        PgbCtl->kv_seeded = true;
    }
    LWLockRelease(PgbCtl->lock);
}

/*
 * Hand out the next version for a write on the leader.  Caller must be inside
 * SPI.
 */
static int64
next_version(void)
{
    int64       v;

    ensure_seeded();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    v = ++PgbCtl->kv_version;
    LWLockRelease(PgbCtl->lock);

    return v;
}

/* Record that this node has applied everything up to version v. */
static void
note_applied(int64 v)
{
    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    if (v > PgbCtl->kv_version)
        PgbCtl->kv_version = v;
    PgbCtl->kv_seeded = true;
    LWLockRelease(PgbCtl->lock);
}

/*
 * Write one row into pgbully.kv, keeping the newer version when a row is
 * already there.  Caller must be inside SPI.  Returns true if the row landed.
 */
static bool
store_row(const char *key, const char *value, bool deleted,
          int64 version, int64 term)
{
    Oid         types[5] = {TEXTOID, TEXTOID, BOOLOID, INT8OID, INT8OID};
    Datum       values[5];
    char        nulls[5] = {' ', ' ', ' ', ' ', ' '};
    int         rc;

    values[0] = CStringGetTextDatum(key);
    if (value == NULL)
    {
        values[1] = (Datum) 0;
        nulls[1] = 'n';
    }
    else
        values[1] = CStringGetTextDatum(value);
    values[2] = BoolGetDatum(deleted);
    values[3] = Int64GetDatum(version);
    values[4] = Int64GetDatum(term);

    rc = SPI_execute_with_args(
        "INSERT INTO pgbully.kv AS k (key, value, deleted, version, term, updated_at)"
        " VALUES ($1, $2, $3, $4, $5, now())"
        " ON CONFLICT (key) DO UPDATE"
        "    SET value = excluded.value,"
        "        deleted = excluded.deleted,"
        "        version = excluded.version,"
        "        term = excluded.term,"
        "        updated_at = excluded.updated_at"
        "  WHERE excluded.version > k.version",
        5, types, values, nulls, false, 0);

    if (rc != SPI_OK_INSERT && rc != SPI_OK_INSERT_RETURNING)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pgbully: could not store key \"%s\"", key)));

    return SPI_processed > 0;
}

/*
 * Push one row to every reachable peer.  Best effort by design: a peer that
 * cannot be reached will pull the row when it next reads.  Returns how many
 * peers took it.
 */
static int
replicate_row(const char *key, const char *value, bool deleted,
              int64 version, int64 term)
{
    PgbPeer     peers[PGBULLY_MAX_NODES];
    int         npeers;
    int32       my_id;
    int         acked = 0;
    int         i;

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    npeers = PgbCtl->npeers;
    my_id = PgbCtl->my_node_id;
    memcpy(peers, PgbCtl->peers, sizeof(PgbPeer) * npeers);
    LWLockRelease(PgbCtl->lock);

    for (i = 0; i < npeers; i++)
    {
        if (!peers[i].in_use || peers[i].node_id == my_id)
            continue;

        CHECK_FOR_INTERRUPTS();

        if (pgbully_send_kv_apply(&peers[i], key, value, deleted,
                                  version, term) == PGB_RPC_OK)
            acked++;
    }

    return acked;
}

/*
 * Pull whatever this node is missing from the leader.  Called before a read
 * on a follower that has seen the leader advertise a higher version.  Caller
 * must be inside SPI.  Silently does nothing if the leader is unreachable --
 * a stale answer beats an error, and the next read tries again.
 */
static void
catch_up(void)
{
    PgbPeer     leader_peer;
    int64       from;
    int64       advertised;
    int32       leader_id;
    bool        found = false;
    void       *raw = NULL;
    PGresult   *res;
    int         n;
    int         i;
    int64       highest = 0;

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    leader_id = PgbCtl->leader_id;
    from = PgbCtl->kv_version;
    advertised = PgbCtl->leader_kv_version;
    if (leader_id != PGBULLY_NO_LEADER && leader_id != PgbCtl->my_node_id)
    {
        for (i = 0; i < PgbCtl->npeers; i++)
        {
            if (PgbCtl->peers[i].in_use &&
                PgbCtl->peers[i].node_id == leader_id)
            {
                leader_peer = PgbCtl->peers[i];
                found = true;
                break;
            }
        }
    }
    LWLockRelease(PgbCtl->lock);

    if (!found || advertised <= from)
        return;

    if (pgbully_fetch_kv_since(&leader_peer, from, &raw) != PGB_RPC_OK)
        return;

    res = (PGresult *) raw;
    n = PQntuples(res);

    for (i = 0; i < n; i++)
    {
        const char *key = PQgetvalue(res, i, 0);
        const char *value = PQgetisnull(res, i, 1) ? NULL : PQgetvalue(res, i, 1);
        bool        deleted = (PQgetvalue(res, i, 2)[0] == 't');
        int64       version = strtoll(PQgetvalue(res, i, 3), NULL, 10);

        store_row(key, value, deleted, version, 0);
        if (version > highest)
            highest = version;
    }

    PQclear(res);

    if (highest > 0)
    {
        note_applied(highest);
        ereport(DEBUG1,
                (errmsg("pgbully: caught up %d key(s) from node %d",
                        n, leader_id)));
    }
}

/*
 * Offer this node's newer rows to a leader that is behind.
 *
 * The bully rule is "highest reachable id leads", not "most up to date node
 * leads", so a node that was down while the cluster kept writing can come
 * back, win the election on its id alone, and start serving a stale store.
 * Nothing pulls in that direction -- the leader never catches up from anyone
 * -- so without this those writes are simply lost.
 *
 * Every heartbeat carries the leader's version.  A follower that sees it is
 * ahead pushes what the leader is missing, stamped with the current term so
 * the leader accepts it.  Once the leader has caught up it advertises the
 * higher version and this stops firing.
 *
 * Called from the heartbeat handler, which is an ordinary backend, so SPI and
 * libpq are both available.
 */
void
pgbully_kv_offer_to_leader(int32 leader_id, int64 leader_version)
{
    PgbPeer     leader_peer;
    int64       ours;
    int64       term;
    bool        found = false;
    uint64      i;
    int         rc;

    if (!pgbully_attach_shmem())
        return;

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    ours = PgbCtl->kv_version;
    term = PgbCtl->term;
    if (leader_id != PGBULLY_NO_LEADER && leader_id != PgbCtl->my_node_id)
    {
        int         p;

        for (p = 0; p < PgbCtl->npeers; p++)
        {
            if (PgbCtl->peers[p].in_use &&
                PgbCtl->peers[p].node_id == leader_id)
            {
                leader_peer = PgbCtl->peers[p];
                found = true;
                break;
            }
        }
    }
    LWLockRelease(PgbCtl->lock);

    if (!found || ours <= leader_version)
        return;

    SPI_connect();
    ensure_seeded();

    {
        Oid         types[1] = {INT8OID};
        Datum       args[1];

        args[0] = Int64GetDatum(leader_version);
        rc = SPI_execute_with_args(
            "SELECT key, value, deleted, version FROM pgbully.kv"
            " WHERE version > $1 ORDER BY version",
            1, types, args, NULL, true, 0);
    }

    if (rc == SPI_OK_SELECT && SPI_processed > 0)
    {
        uint64      n = SPI_processed;
        SPITupleTable *tt = SPI_tuptable;

        ereport(LOG,
                (errmsg("pgbully: leader %d is behind at version " INT64_FORMAT
                        ", offering " UINT64_FORMAT " row(s)",
                        leader_id, leader_version, n)));

        for (i = 0; i < n; i++)
        {
            char       *key = SPI_getvalue(tt->vals[i], tt->tupdesc, 1);
            char       *value = SPI_getvalue(tt->vals[i], tt->tupdesc, 2);
            char       *del = SPI_getvalue(tt->vals[i], tt->tupdesc, 3);
            char       *ver = SPI_getvalue(tt->vals[i], tt->tupdesc, 4);

            if (key == NULL || ver == NULL)
                continue;

            pgbully_send_kv_apply(&leader_peer, key, value,
                                  del != NULL && del[0] == 't',
                                  strtoll(ver, NULL, 10), term);
        }
    }

    SPI_finish();
}

/* Run catch_up() before a read, when this node is a follower that is behind. */
static void
freshen(void)
{
    bool        behind;

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    behind = (PgbCtl->state != PGB_LEADER &&
              PgbCtl->leader_kv_version > PgbCtl->kv_version);
    LWLockRelease(PgbCtl->lock);

    if (behind)
        catch_up();
}

static void
bump(int64 *counter)
{
    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    (*counter)++;
    LWLockRelease(PgbCtl->lock);
}

/* -------------------------------------------------------------------------
 * Writes -- leader only
 * ------------------------------------------------------------------------- */

/* kv_put(key, value) -> boolean */
Datum
pgbully_kv_put(PG_FUNCTION_ARGS)
{
    text       *key_txt;
    text       *val_txt;
    char       *key;
    char       *value;
    int64       version;
    int64       term;

    require_shmem();

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("key must not be null")));

    require_leader("store a key");

    key_txt = PG_GETARG_TEXT_PP(0);
    key = text_to_cstring(key_txt);

    if (key[0] == '\0')
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("key must not be empty")));

    if (PG_ARGISNULL(1))
        value = NULL;
    else
    {
        val_txt = PG_GETARG_TEXT_PP(1);
        value = text_to_cstring(val_txt);
    }

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    term = PgbCtl->term;
    LWLockRelease(PgbCtl->lock);

    SPI_connect();
    version = next_version();
    store_row(key, value, false, version, term);
    SPI_finish();

    note_applied(version);
    bump(&PgbCtl->kv_puts);

    replicate_row(key, value, false, version, term);

    PG_RETURN_BOOL(true);
}

/* kv_delete(key) -> boolean : true if the key existed */
Datum
pgbully_kv_delete(PG_FUNCTION_ARGS)
{
    char       *key;
    int64       version;
    int64       term;
    bool        existed = false;
    int         rc;

    require_shmem();

    if (PG_ARGISNULL(0))
        PG_RETURN_BOOL(false);

    require_leader("delete a key");

    key = text_to_cstring(PG_GETARG_TEXT_PP(0));

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    term = PgbCtl->term;
    LWLockRelease(PgbCtl->lock);

    SPI_connect();

    {
        Oid         types[1] = {TEXTOID};
        Datum       values[1];

        values[0] = CStringGetTextDatum(key);
        rc = SPI_execute_with_args(
            "SELECT 1 FROM pgbully.kv WHERE key = $1 AND NOT deleted",
            1, types, values, NULL, true, 1);
        existed = (rc == SPI_OK_SELECT && SPI_processed == 1);
    }

    if (existed)
    {
        /*
         * A tombstone, not a DELETE: the row has to reach the peers, and a
         * missing row cannot be replicated.  kv_compact() clears them later.
         */
        version = next_version();
        store_row(key, NULL, true, version, term);
    }

    SPI_finish();

    if (!existed)
        PG_RETURN_BOOL(false);

    note_applied(version);
    bump(&PgbCtl->kv_deletes);

    replicate_row(key, NULL, true, version, term);

    PG_RETURN_BOOL(true);
}

/*
 * kv_compact() -> boolean
 *
 * Drop tombstones, which is what keeps a delete-heavy store from growing
 * forever.  Only rows strictly below the current version go, so the version
 * the table reports never moves backwards.
 */
Datum
pgbully_kv_compact(PG_FUNCTION_ARGS)
{
    int64       upto;
    Oid         types[1] = {INT8OID};
    Datum       values[1];
    uint64      removed;

    require_shmem();
    require_leader("compact the store");

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    upto = PgbCtl->kv_version;
    LWLockRelease(PgbCtl->lock);

    values[0] = Int64GetDatum(upto);

    SPI_connect();
    SPI_execute_with_args("DELETE FROM pgbully.kv"
                          " WHERE deleted AND version < $1",
                          1, types, values, NULL, false, 0);
    removed = SPI_processed;
    SPI_finish();

    ereport(NOTICE,
            (errmsg("pgbully: compacted " UINT64_FORMAT " tombstone(s)",
                    removed),
             errhint("A peer that has not caught up past version "
                     INT64_FORMAT " will not learn of those deletions.",
                     upto)));

    PG_RETURN_BOOL(true);
}

/*
 * kv_reset() -> boolean : empty the store on this node and every reachable
 * peer.  Deliberately blunt, and superuser-only through the SQL grants.
 */
Datum
pgbully_kv_reset(PG_FUNCTION_ARGS)
{
    PgbPeer     peers[PGBULLY_MAX_NODES];
    int         npeers;
    int32       my_id;
    int64       version;
    int64       term;
    int         i;

    require_shmem();
    require_leader("reset the store");

    SPI_connect();
    ensure_seeded();
    SPI_execute("DELETE FROM pgbully.kv", false, 0);
    SPI_finish();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    PgbCtl->kv_version++;       /* so followers see a change, never a rewind */
    PgbCtl->kv_seeded = true;
    version = PgbCtl->kv_version;
    term = PgbCtl->term;
    npeers = PgbCtl->npeers;
    my_id = PgbCtl->my_node_id;
    memcpy(peers, PgbCtl->peers, sizeof(PgbPeer) * npeers);
    LWLockRelease(PgbCtl->lock);

    for (i = 0; i < npeers; i++)
    {
        if (!peers[i].in_use || peers[i].node_id == my_id)
            continue;
        CHECK_FOR_INTERRUPTS();
        /*
         * An empty key is the agreed "drop everything" marker.  It has to
         * carry the current term and the bumped version, or the peer fences
         * it out exactly as it would a deposed leader's write.
         */
        pgbully_send_kv_apply(&peers[i], "", NULL, true, version, term);
    }

    PG_RETURN_BOOL(true);
}

/* -------------------------------------------------------------------------
 * Reads -- any node
 * ------------------------------------------------------------------------- */

/* kv_get(key) -> text, NULL if absent */
Datum
pgbully_kv_get(PG_FUNCTION_ARGS)
{
    Oid         types[1] = {TEXTOID};
    Datum       values[1];
    char       *result = NULL;
    int         rc;

    require_shmem();

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    SPI_connect();
    ensure_seeded();
    freshen();

    values[0] = PG_GETARG_DATUM(0);
    rc = SPI_execute_with_args(
        "SELECT value FROM pgbully.kv WHERE key = $1 AND NOT deleted",
        1, types, values, NULL, true, 1);

    if (rc == SPI_OK_SELECT && SPI_processed == 1)
    {
        bool        isnull;
        Datum       d = SPI_getbinval(SPI_tuptable->vals[0],
                                      SPI_tuptable->tupdesc, 1, &isnull);

        if (!isnull)
            result = text_to_cstring(DatumGetTextPP(d));
    }

    SPI_finish();
    bump(&PgbCtl->kv_gets);

    if (result == NULL)
        PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* kv_exists(key) -> boolean */
Datum
pgbully_kv_exists(PG_FUNCTION_ARGS)
{
    Oid         types[1] = {TEXTOID};
    Datum       values[1];
    bool        found;
    int         rc;

    require_shmem();

    if (PG_ARGISNULL(0))
        PG_RETURN_BOOL(false);

    SPI_connect();
    ensure_seeded();
    freshen();

    values[0] = PG_GETARG_DATUM(0);
    rc = SPI_execute_with_args(
        "SELECT 1 FROM pgbully.kv WHERE key = $1 AND NOT deleted",
        1, types, values, NULL, true, 1);
    found = (rc == SPI_OK_SELECT && SPI_processed == 1);

    SPI_finish();
    bump(&PgbCtl->kv_gets);

    PG_RETURN_BOOL(found);
}

/* kv_list_keys() -> text : a JSON array of the live keys, in order */
Datum
pgbully_kv_list_keys(PG_FUNCTION_ARGS)
{
    StringInfoData buf;
    uint64      i;
    int         rc;

    require_shmem();

    SPI_connect();
    ensure_seeded();
    freshen();

    rc = SPI_execute("SELECT key FROM pgbully.kv"
                     " WHERE NOT deleted ORDER BY key", true, 0);

    initStringInfo(&buf);
    appendStringInfoChar(&buf, '[');

    if (rc == SPI_OK_SELECT)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            char       *k = SPI_getvalue(SPI_tuptable->vals[i],
                                         SPI_tuptable->tupdesc, 1);

            if (i > 0)
                appendStringInfoChar(&buf, ',');
            escape_json(&buf, k ? k : "");
        }
    }

    appendStringInfoChar(&buf, ']');
    SPI_finish();

    PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/*
 * kv_get_stats() -> one row.
 *
 * last_applied_index is the store's version counter: the same number the
 * leader advertises on its heartbeats.
 */
Datum
pgbully_kv_get_stats(PG_FUNCTION_ARGS)
{
    TupleDesc   tupdesc;
    HeapTuple   tuple;
    Datum       values[8];
    bool        nulls[8];
    int64       active = 0;
    int64       tombstones = 0;
    PgbShared   snap;
    int         rc;

    require_shmem();

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context "
                        "that cannot accept type record")));
    tupdesc = BlessTupleDesc(tupdesc);

    SPI_connect();
    ensure_seeded();
    rc = SPI_execute("SELECT count(*) FILTER (WHERE NOT deleted),"
                     "       count(*) FILTER (WHERE deleted)"
                     " FROM pgbully.kv", true, 1);
    if (rc == SPI_OK_SELECT && SPI_processed == 1)
    {
        bool        isnull;

        active = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                             SPI_tuptable->tupdesc, 1, &isnull));
        tombstones = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
                                                 SPI_tuptable->tupdesc, 2, &isnull));
    }
    SPI_finish();

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    snap = *PgbCtl;
    LWLockRelease(PgbCtl->lock);

    memset(nulls, 0, sizeof(nulls));
    values[0] = Int32GetDatum((int32) (active + tombstones));
    values[1] = Int64GetDatum(snap.kv_puts + snap.kv_deletes + snap.kv_gets);
    values[2] = Int64GetDatum(snap.kv_version);
    values[3] = Int64GetDatum(snap.kv_puts);
    values[4] = Int64GetDatum(snap.kv_deletes);
    values[5] = Int64GetDatum(snap.kv_gets);
    values[6] = Int32GetDatum((int32) active);
    values[7] = Int32GetDatum((int32) tombstones);

    tuple = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * kv_sync() -> bigint
 *
 * Pull from the leader now rather than waiting for the next read, and report
 * the version this node ends up at.  Useful after a node rejoins, and in
 * tests.
 */
Datum
pgbully_kv_sync(PG_FUNCTION_ARGS)
{
    int64       v;

    require_shmem();

    SPI_connect();
    ensure_seeded();
    catch_up();
    SPI_finish();

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    v = PgbCtl->kv_version;
    LWLockRelease(PgbCtl->lock);

    PG_RETURN_INT64(v);
}

/* -------------------------------------------------------------------------
 * Inbound replication
 * ------------------------------------------------------------------------- */

/*
 * rpc_kv_apply(key, value, deleted, version, term) -> bigint
 *
 * The leader pushing one row.  Returns the version this node holds after the
 * call, so the sender can see whether it was taken.
 *
 * A row from a stale term is refused: a deposed leader must not be able to
 * write through a node that has already moved on.
 */
Datum
pgbully_rpc_kv_apply(PG_FUNCTION_ARGS)
{
    char       *key;
    char       *value = NULL;
    bool        deleted;
    int64       version;
    int64       term;
    int64       our_term;
    int64       now_at;

    require_shmem();

    key = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (!PG_ARGISNULL(1))
        value = text_to_cstring(PG_GETARG_TEXT_PP(1));
    deleted = PG_GETARG_BOOL(2);
    version = PG_GETARG_INT64(3);
    term = PG_GETARG_INT64(4);

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    our_term = PgbCtl->term;
    LWLockRelease(PgbCtl->lock);

    if (term < our_term)
    {
        ereport(DEBUG1,
                (errmsg("pgbully: ignoring key \"%s\" from term "
                        INT64_FORMAT ", we are at " INT64_FORMAT,
                        key, term, our_term)));
        LWLockAcquire(PgbCtl->lock, LW_SHARED);
        now_at = PgbCtl->kv_version;
        LWLockRelease(PgbCtl->lock);
        PG_RETURN_INT64(now_at);
    }

    SPI_connect();
    ensure_seeded();

    /* The empty key is kv_reset()'s "drop everything" marker. */
    if (key[0] == '\0' && deleted)
    {
        SPI_execute("DELETE FROM pgbully.kv", false, 0);
        SPI_finish();

        /*
         * Take the leader's version rather than bumping our own.  A reset is
         * the leader declaring a new baseline, and it has to win outright: a
         * node left holding a higher version would otherwise decide the
         * leader was behind and push the emptied rows straight back.
         */
        LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
        PgbCtl->kv_version = version;
        PgbCtl->leader_kv_version = version;
        PgbCtl->kv_seeded = true;
        now_at = PgbCtl->kv_version;
        LWLockRelease(PgbCtl->lock);

        PG_RETURN_INT64(now_at);
    }

    store_row(key, value, deleted, version, term);
    SPI_finish();

    note_applied(version);

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    now_at = PgbCtl->kv_version;
    LWLockRelease(PgbCtl->lock);

    PG_RETURN_INT64(now_at);
}

/*
 * rpc_kv_since(from_version) -> setof (key, value, deleted, version)
 *
 * Everything this node has applied after from_version, for a peer that is
 * catching up.
 */
Datum
pgbully_rpc_kv_since(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    int64       from = PG_GETARG_INT64(0);
    Oid         types[1] = {INT8OID};
    Datum       args[1];
    uint64      i;
    int         rc;

    require_shmem();

    InitMaterializedSRF(fcinfo, 0);

    args[0] = Int64GetDatum(from);

    SPI_connect();
    ensure_seeded();
    rc = SPI_execute_with_args(
        "SELECT key, value, deleted, version FROM pgbully.kv"
        " WHERE version > $1 ORDER BY version",
        1, types, args, NULL, true, 0);

    if (rc == SPI_OK_SELECT)
    {
        for (i = 0; i < SPI_processed; i++)
        {
            HeapTuple   tup = SPI_tuptable->vals[i];
            TupleDesc   desc = SPI_tuptable->tupdesc;
            Datum       values[4];
            bool        nulls[4];
            bool        isnull;
            int         c;

            for (c = 0; c < 4; c++)
            {
                Datum       d = SPI_getbinval(tup, desc, c + 1, &isnull);

                nulls[c] = isnull;
                values[c] = isnull ? (Datum) 0 : d;
            }

            /*
             * The tuplestore outlives SPI_finish(), so the text Datums have
             * to be copied out of the SPI context first.
             */
            if (!nulls[0])
                values[0] = PointerGetDatum(PG_DETOAST_DATUM_COPY(values[0]));
            if (!nulls[1])
                values[1] = PointerGetDatum(PG_DETOAST_DATUM_COPY(values[1]));

            tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
                                 values, nulls);
        }
    }

    SPI_finish();

    return (Datum) 0;
}
