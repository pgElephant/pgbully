/*-------------------------------------------------------------------------
 *
 * cluster_api.c
 *      The standard cluster-management API for pgBully.
 *
 * pgBully's native surface (pgbully.leader(), pgbully.status(), ...) is
 * shaped around the Bully algorithm.  This file adds the vendor-neutral
 * function set that cluster managers and control planes expect from any
 * PostgreSQL consensus backend, so that an application written against that
 * interface runs on pgBully unchanged:
 *
 *      pgbully.init()                  bring the node up from its GUCs
 *      pgbully.add_node() / .remove_node()
 *      pgbully.get_cluster_status()    one row of cluster state
 *      pgbully.get_nodes()             membership as (id, address, port)
 *      pgbully.get_leader() / .get_term()
 *      pgbully.get_worker_state() / .get_version() / .test() / .set_debug()
 *
 * Two details differ from pgBully's native functions and are deliberate:
 *
 *   node identity   the standard interface describes a member as
 *                   (node_id, address, port), while pgbully.nodes describes
 *                   it as "id:conninfo".  The address and port columns are
 *                   derived from each peer's conninfo, and
 *                   pgbully.add_node() synthesizes a conninfo from the
 *                   address and port it is handed.
 *
 *   state names     the standard interface knows leader, follower and
 *                   candidate.  pgBully's internal PGB_WAITING is an
 *                   election in progress, so it reports as "candidate" here;
 *                   pgbully.state() still reports the real one.
 *
 *   no leader       this API reports leader id 0 when no leader is known,
 *                   where pgbully.leader() returns NULL.
 *
 * The interface also covers log replication and a key/value store.  pgBully
 * elects a leader and nothing else, so it carries neither.  Those functions
 * are absent rather than present-and-refusing: a caller that needs replicated
 * state is better served by finding out at CREATE EXTENSION time than by a
 * function that exists only to raise.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/timestamp.h"

#include "compat.h"
#include "pgbully.h"

/* cluster / membership */
PG_FUNCTION_INFO_V1(pgbully_compat_init);
PG_FUNCTION_INFO_V1(pgbully_add_node);
PG_FUNCTION_INFO_V1(pgbully_remove_node);
PG_FUNCTION_INFO_V1(pgbully_get_cluster_status_table);
PG_FUNCTION_INFO_V1(pgbully_get_nodes_table);
PG_FUNCTION_INFO_V1(pgbully_get_nodes_json);
PG_FUNCTION_INFO_V1(pgbully_get_leader_id);
PG_FUNCTION_INFO_V1(pgbully_get_worker_state);
PG_FUNCTION_INFO_V1(pgbully_get_version);
PG_FUNCTION_INFO_V1(pgbully_get_queue_status);
PG_FUNCTION_INFO_V1(pgbully_compat_test);
PG_FUNCTION_INFO_V1(pgbully_set_debug);

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/*
 * Attach to shared memory or raise the same clear error the rest of the
 * extension raises.
 */
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

/*
 * Map a pgBully state onto the three names the standard interface knows.
 * PGB_WAITING -- ELECTION sent to higher-id peers, winner not yet announced
 * -- is still an election in progress, so it reports as "candidate".
 */
static const char *
compat_state_name(PgbState s)
{
    switch (s)
    {
        case PGB_LEADER:
            return "leader";
        case PGB_CANDIDATE:
        case PGB_WAITING:
            return "candidate";
        case PGB_FOLLOWER:
            return "follower";
    }
    return "follower";
}

/*
 * Pull the effective host and port out of a libpq conninfo string.
 *
 * PQconninfoParse() applies environment variables and built-in defaults, so
 * an entry that omits the port still reports the port the worker would
 * actually dial.  On a parse failure we fall back to reporting the conninfo
 * verbatim, which is more useful to an operator than an empty column.
 */
static void
conninfo_host_port(const char *conninfo, char **host_out, int *port_out)
{
    PQconninfoOption *opts;
    PQconninfoOption *opt;
    char       *errmsg = NULL;
    const char *host = NULL;
    const char *hostaddr = NULL;
    const char *port = NULL;

    *host_out = pstrdup(conninfo);
    *port_out = 0;

    opts = PQconninfoParse(conninfo, &errmsg);
    if (opts == NULL)
    {
        if (errmsg)
            PQfreemem(errmsg);
        return;
    }

    for (opt = opts; opt->keyword != NULL; opt++)
    {
        if (opt->val == NULL || opt->val[0] == '\0')
            continue;

        if (strcmp(opt->keyword, "host") == 0)
            host = opt->val;
        else if (strcmp(opt->keyword, "hostaddr") == 0)
            hostaddr = opt->val;
        else if (strcmp(opt->keyword, "port") == 0)
            port = opt->val;
    }

    /* hostaddr wins only when no host name was given, as libpq does. */
    if (host != NULL)
        *host_out = pstrdup(host);
    else if (hostaddr != NULL)
        *host_out = pstrdup(hostaddr);

    if (port != NULL)
        *port_out = atoi(port);

    PQconninfoFree(opts);
}

/*
 * Return the value libpq would use for one conninfo keyword, or NULL.  Used
 * to carry dbname/user across from an existing peer when synthesizing a
 * conninfo in pgbully.add_node().
 */
static char *
conninfo_keyword(const char *conninfo, const char *keyword)
{
    PQconninfoOption *opts;
    PQconninfoOption *opt;
    char       *errmsg = NULL;
    char       *result = NULL;

    opts = PQconninfoParse(conninfo, &errmsg);
    if (opts == NULL)
    {
        if (errmsg)
            PQfreemem(errmsg);
        return NULL;
    }

    for (opt = opts; opt->keyword != NULL; opt++)
    {
        if (strcmp(opt->keyword, keyword) == 0)
        {
            if (opt->val != NULL && opt->val[0] != '\0')
                result = pstrdup(opt->val);
            break;
        }
    }

    PQconninfoFree(opts);
    return result;
}

/*
 * Append "keyword='value'" to a conninfo under construction, escaping the
 * two characters libpq treats specially inside a quoted value.
 */
static void
append_conninfo_kv(StringInfo buf, const char *keyword, const char *value)
{
    const char *p;

    if (buf->len > 0)
        appendStringInfoChar(buf, ' ');

    appendStringInfoString(buf, keyword);
    appendStringInfoString(buf, "='");
    for (p = value; *p != '\0'; p++)
    {
        if (*p == '\\' || *p == '\'')
            appendStringInfoChar(buf, '\\');
        appendStringInfoChar(buf, *p);
    }
    appendStringInfoChar(buf, '\'');
}

/* Wake the worker so a membership change takes effect immediately. */
static void
wake_worker(void)
{
    if (PgbCtl->worker_latch != NULL)
        SetLatch(PgbCtl->worker_latch);
}

/* -------------------------------------------------------------------------
 * Cluster lifecycle
 * ------------------------------------------------------------------------- */

/*
 * pgbully.init() -> boolean
 *
 * Brings the node up from its configuration: re-reads pgbully.nodes into
 * shared memory and kicks the worker.  Returns false rather than raising, as
 * the interface requires.
 */
Datum
pgbully_compat_init(PG_FUNCTION_ARGS)
{
    if (!pgbully_attach_shmem())
    {
        ereport(WARNING,
                (errmsg("pgbully: not loaded via shared_preload_libraries"),
                 errhint("Add \"pgbully\" to shared_preload_libraries and "
                         "restart the server.")));
        PG_RETURN_BOOL(false);
    }

    if (pgbully_node_id <= 0)
    {
        ereport(WARNING,
                (errmsg("pgbully: pgbully.node_id is not set")));
        PG_RETURN_BOOL(false);
    }

    pgbully_load_peers();
    wake_worker();

    PG_RETURN_BOOL(true);
}

/*
 * pgbully.add_node(node_id, address, port) -> boolean
 *
 * Adds (or re-points) a peer in shared memory, synthesizing a conninfo from
 * the address and port.  Any dbname/user already in use for the cluster is
 * carried over so the new entry is dialable without further configuration.
 *
 * The change takes effect at once but is *not* persistent: pgbully.nodes
 * remains the source of truth and a configuration reload restores it.  A
 * NOTICE says so rather than leaving the operator to discover it.
 */
Datum
pgbully_add_node(PG_FUNCTION_ARGS)
{
    int32       node_id = PG_GETARG_INT32(0);
    text       *address_txt = PG_GETARG_TEXT_PP(1);
    int32       port = PG_GETARG_INT32(2);
    char       *address = text_to_cstring(address_txt);
    StringInfoData conninfo;
    char       *template = NULL;
    char       *dbname = NULL;
    char       *user = NULL;
    int         slot = -1;
    int         i;
    bool        replaced = false;

    require_shmem();

    if (node_id <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("node id must be positive")));
    if (address[0] == '\0')
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("node address must not be empty")));
    if (port <= 0 || port > 65535)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("port %d is out of range 1..65535", port)));

    /*
     * Borrow dbname/user from a configured peer so the synthesized entry is
     * dialable.  Copy the template out from under the lock first: parsing a
     * conninfo goes through libpq and mallocs, which has no business
     * happening while an LWLock is held.
     */
    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    for (i = 0; i < PgbCtl->npeers; i++)
    {
        if (PgbCtl->peers[i].in_use)
        {
            template = pstrdup(PgbCtl->peers[i].conninfo);
            break;
        }
    }
    LWLockRelease(PgbCtl->lock);

    if (template != NULL)
    {
        dbname = conninfo_keyword(template, "dbname");
        user = conninfo_keyword(template, "user");
        pfree(template);
    }

    initStringInfo(&conninfo);
    append_conninfo_kv(&conninfo, "host", address);
    appendStringInfo(&conninfo, " port=%d", port);
    if (dbname != NULL)
        append_conninfo_kv(&conninfo, "dbname", dbname);
    if (user != NULL)
        append_conninfo_kv(&conninfo, "user", user);

    if (conninfo.len >= PGBULLY_CONNINFO_LEN)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("synthesized conninfo for node %d is too long (max %d)",
                        node_id, PGBULLY_CONNINFO_LEN - 1)));

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);

    for (i = 0; i < PgbCtl->npeers; i++)
    {
        if (PgbCtl->peers[i].in_use && PgbCtl->peers[i].node_id == node_id)
        {
            slot = i;
            replaced = true;
            break;
        }
    }

    if (slot < 0)
    {
        /* Reuse a freed slot before growing the array. */
        for (i = 0; i < PgbCtl->npeers; i++)
        {
            if (!PgbCtl->peers[i].in_use)
            {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0)
    {
        if (PgbCtl->npeers >= PGBULLY_MAX_NODES)
        {
            LWLockRelease(PgbCtl->lock);
            ereport(ERROR,
                    (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
                     errmsg("cannot add node %d: cluster already has the "
                            "maximum of %d nodes",
                            node_id, PGBULLY_MAX_NODES)));
        }
        slot = PgbCtl->npeers++;
    }

    memset(&PgbCtl->peers[slot], 0, sizeof(PgbPeer));
    PgbCtl->peers[slot].node_id = node_id;
    strlcpy(PgbCtl->peers[slot].conninfo, conninfo.data, PGBULLY_CONNINFO_LEN);
    PgbCtl->peers[slot].in_use = true;
    PgbCtl->peers[slot].reachable = false;
    PgbCtl->peers[slot].last_seen = 0;

    /* A new member may outrank the incumbent, so re-run the election. */
    PgbCtl->election_requested = true;

    LWLockRelease(PgbCtl->lock);

    pfree(conninfo.data);
    wake_worker();

    ereport(NOTICE,
            (errmsg("pgbully: node %d %s in shared memory",
                    node_id, replaced ? "updated" : "added"),
             errhint("Add the node to pgbully.nodes as well; this change does "
                     "not survive a configuration reload.")));

    PG_RETURN_BOOL(true);
}

/*
 * pgbully.remove_node(node_id) -> boolean
 *
 * Drops a peer from shared memory.  Like pgbully.add_node(), this is a
 * runtime change only.  Returns false if the node was not a member.
 */
Datum
pgbully_remove_node(PG_FUNCTION_ARGS)
{
    int32       node_id = PG_GETARG_INT32(0);
    bool        found = false;
    int         i;

    require_shmem();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);

    if (node_id == PgbCtl->my_node_id)
    {
        LWLockRelease(PgbCtl->lock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot remove node %d: it is the local node",
                        node_id)));
    }

    for (i = 0; i < PgbCtl->npeers; i++)
    {
        if (PgbCtl->peers[i].in_use && PgbCtl->peers[i].node_id == node_id)
        {
            memset(&PgbCtl->peers[i], 0, sizeof(PgbPeer));
            found = true;
            break;
        }
    }

    /* Shrink the array if the tail is now unused. */
    while (PgbCtl->npeers > 0 && !PgbCtl->peers[PgbCtl->npeers - 1].in_use)
        PgbCtl->npeers--;

    /* Losing the leader means we need a new one immediately. */
    if (found && node_id == PgbCtl->leader_id)
    {
        PgbCtl->leader_id = PGBULLY_NO_LEADER;
        PgbCtl->election_requested = true;
    }

    LWLockRelease(PgbCtl->lock);

    if (!found)
    {
        ereport(WARNING,
                (errmsg("pgbully: node %d is not a member", node_id)));
        PG_RETURN_BOOL(false);
    }

    wake_worker();

    ereport(NOTICE,
            (errmsg("pgbully: node %d removed from shared memory", node_id),
             errhint("Remove the node from pgbully.nodes as well; this change "
                     "does not survive a configuration reload.")));

    PG_RETURN_BOOL(true);
}

/* -------------------------------------------------------------------------
 * Introspection
 * ------------------------------------------------------------------------- */

/*
 * pgbully.get_cluster_status() -> one row
 *
 * Columns:
 *   node_id, current_term, leader_id, state, num_nodes,
 *   messages_processed, heartbeats_sent, elections_triggered
 *
 * messages_processed counts every Bully message this node has handled in
 * either direction.
 */
Datum
pgbully_get_cluster_status_table(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    Datum       values[8];
    bool        nulls[8];
    PgbShared   snap;

    require_shmem();

    InitMaterializedSRF(fcinfo, 0);

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    snap = *PgbCtl;
    LWLockRelease(PgbCtl->lock);

    memset(nulls, 0, sizeof(nulls));

    values[0] = Int32GetDatum(snap.my_node_id);
    values[1] = Int64GetDatum(snap.term);
    values[2] = Int64GetDatum((int64) snap.leader_id);
    values[3] = CStringGetTextDatum(compat_state_name(snap.state));
    values[4] = Int32GetDatum(snap.npeers);
    values[5] = Int64GetDatum(snap.heartbeats_sent + snap.heartbeats_recv +
                              snap.coordinators_recv + snap.elections_started);
    values[6] = Int64GetDatum(snap.heartbeats_sent);
    values[7] = Int64GetDatum(snap.elections_started);

    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

    return (Datum) 0;
}

/*
 * pgbully.get_nodes() -> set of (node_id, address, port, is_leader)
 */
Datum
pgbully_get_nodes_table(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    PgbPeer     peers[PGBULLY_MAX_NODES];
    int         npeers;
    int32       leader_id;
    int         i;

    require_shmem();

    InitMaterializedSRF(fcinfo, 0);

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    npeers = PgbCtl->npeers;
    leader_id = PgbCtl->leader_id;
    memcpy(peers, PgbCtl->peers, sizeof(PgbPeer) * npeers);
    LWLockRelease(PgbCtl->lock);

    for (i = 0; i < npeers; i++)
    {
        Datum       values[4];
        bool        nulls[4];
        char       *host;
        int         port;

        if (!peers[i].in_use)
            continue;

        conninfo_host_port(peers[i].conninfo, &host, &port);

        memset(nulls, 0, sizeof(nulls));
        values[0] = Int32GetDatum(peers[i].node_id);
        values[1] = CStringGetTextDatum(host);
        values[2] = Int32GetDatum(port);
        values[3] = BoolGetDatum(peers[i].node_id == leader_id);

        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

        pfree(host);
    }

    return (Datum) 0;
}

/*
 * pgbully.get_nodes_json() -> text
 *
 * The membership as a JSON array, in the shape the etcd-compatible views
 * consume: [{"id":1,"address":"host:port","port":5432,"active":true}].
 * "active" is peer reachability as last observed by the worker; the local
 * node is always active.
 */
Datum
pgbully_get_nodes_json(PG_FUNCTION_ARGS)
{
    StringInfoData buf;
    PgbPeer     peers[PGBULLY_MAX_NODES];
    int         npeers;
    int32       my_id;
    int         i;
    bool        first = true;

    require_shmem();

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    npeers = PgbCtl->npeers;
    my_id = PgbCtl->my_node_id;
    memcpy(peers, PgbCtl->peers, sizeof(PgbPeer) * npeers);
    LWLockRelease(PgbCtl->lock);

    initStringInfo(&buf);
    appendStringInfoChar(&buf, '[');

    for (i = 0; i < npeers; i++)
    {
        char       *host;
        int         port;
        StringInfoData addr;

        if (!peers[i].in_use)
            continue;

        conninfo_host_port(peers[i].conninfo, &host, &port);

        initStringInfo(&addr);
        appendStringInfo(&addr, "%s:%d", host, port);

        if (!first)
            appendStringInfoChar(&buf, ',');
        first = false;

        appendStringInfo(&buf, "{\"id\":%d,\"address\":", peers[i].node_id);
        escape_json(&buf, addr.data);
        appendStringInfo(&buf, ",\"port\":%d,\"active\":%s}",
                         port,
                         (peers[i].node_id == my_id || peers[i].reachable)
                         ? "true" : "false");

        pfree(addr.data);
        pfree(host);
    }

    appendStringInfoChar(&buf, ']');

    PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/*
 * pgbully.get_leader() -> bigint
 *
 * Reports 0 -- not NULL -- when no leader is known, which is what the
 * standard interface specifies.  pgbully.leader() returns NULL instead.
 */
Datum
pgbully_get_leader_id(PG_FUNCTION_ARGS)
{
    int32       leader;

    require_shmem();
    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    leader = PgbCtl->leader_id;
    LWLockRelease(PgbCtl->lock);

    PG_RETURN_INT64((int64) leader);
}

/*
 * pgbully.get_worker_state() -> text
 *
 * RUNNING or STOPPED.
 */
Datum
pgbully_get_worker_state(PG_FUNCTION_ARGS)
{
    pid_t       pid;

    require_shmem();
    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    pid = PgbCtl->worker_pid;
    LWLockRelease(PgbCtl->lock);

    if (pid != 0 && pgbully_enabled)
        PG_RETURN_TEXT_P(cstring_to_text("RUNNING"));

    PG_RETURN_TEXT_P(cstring_to_text("STOPPED"));
}

/* pgbully.get_version() -> text, in "<name>-<version>" form. */
Datum
pgbully_get_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("pgbully-" PGBULLY_VERSION));
}

/*
 * pgbully.get_queue_status() -> always empty.
 *
 * The interface allows a backend to queue configuration commands for an
 * asynchronous applier.  pgBully applies membership changes synchronously,
 * so the queue exists only as an empty result set.
 */
Datum
pgbully_get_queue_status(PG_FUNCTION_ARGS)
{
    require_shmem();
    InitMaterializedSRF(fcinfo, 0);
    return (Datum) 0;
}

/*
 * pgbully.test() -> boolean
 *
 * A self-check: shared memory is attached, a node id is configured, and that
 * id is actually listed in the membership.
 */
Datum
pgbully_compat_test(PG_FUNCTION_ARGS)
{
    bool        member = false;
    int32       my_id;
    int         i;

    if (!pgbully_attach_shmem())
    {
        ereport(WARNING,
                (errmsg("pgbully: shared memory is not available")));
        PG_RETURN_BOOL(false);
    }

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    my_id = PgbCtl->my_node_id;
    for (i = 0; i < PgbCtl->npeers; i++)
    {
        if (PgbCtl->peers[i].in_use && PgbCtl->peers[i].node_id == my_id)
        {
            member = true;
            break;
        }
    }
    LWLockRelease(PgbCtl->lock);

    if (my_id <= 0)
    {
        ereport(WARNING, (errmsg("pgbully: pgbully.node_id is not set")));
        PG_RETURN_BOOL(false);
    }
    if (!member)
    {
        ereport(WARNING,
                (errmsg("pgbully: node %d is not listed in pgbully.nodes",
                        my_id)));
        PG_RETURN_BOOL(false);
    }

    PG_RETURN_BOOL(true);
}

/*
 * pgbully.set_debug(enabled) -> boolean
 *
 * Turns cluster-wide-visible debug logging in the worker on or off.  Returns
 * the setting that is now in effect.
 */
Datum
pgbully_set_debug(PG_FUNCTION_ARGS)
{
    bool        enabled = PG_GETARG_BOOL(0);

    require_shmem();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    PgbCtl->debug = enabled;
    LWLockRelease(PgbCtl->lock);

    wake_worker();

    PG_RETURN_BOOL(enabled);
}
