/*-------------------------------------------------------------------------
 *
 * rpc.c
 *      SQL-callable functions for pgBully.
 *
 * Two groups of functions live here:
 *
 *   rpc_*    inbound message handlers invoked by peer nodes over libpq.
 *            They mutate shared state and wake the local worker.
 *
 *   public   monitoring and control functions for human/operator use
 *            (leader, state, status, peers, force_election, ...).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "compat.h"
#include "pgbully.h"

/* inbound handlers */
PG_FUNCTION_INFO_V1(pgbully_rpc_ping);
PG_FUNCTION_INFO_V1(pgbully_rpc_election);
PG_FUNCTION_INFO_V1(pgbully_rpc_coordinator);
PG_FUNCTION_INFO_V1(pgbully_rpc_heartbeat);

/* public surface */
PG_FUNCTION_INFO_V1(pgbully_leader);
PG_FUNCTION_INFO_V1(pgbully_is_leader);
PG_FUNCTION_INFO_V1(pgbully_term);
PG_FUNCTION_INFO_V1(pgbully_state);
PG_FUNCTION_INFO_V1(pgbully_my_node_id);
PG_FUNCTION_INFO_V1(pgbully_force_election);
PG_FUNCTION_INFO_V1(pgbully_status);
PG_FUNCTION_INFO_V1(pgbully_peers);

/* Ensure shared memory is attached, or raise a clear error. */
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

/* Wake the worker if it has registered a latch. */
static void
wake_worker(void)
{
    if (PgbCtl->worker_latch != NULL)
        SetLatch(PgbCtl->worker_latch);
}

/*
 * Record that we just heard from a peer.
 *
 * The worker only learns a peer is alive by probing it, which a follower
 * never does -- it just listens.  A message arriving from a peer is equally
 * good evidence, and without this a follower reports the leader whose
 * heartbeats it is happily receiving as unreachable.
 *
 * Caller must hold PgbCtl->lock exclusively.
 */
static void
mark_peer_seen(int32 node_id)
{
    int         i;

    if (node_id <= 0 || node_id == PgbCtl->my_node_id)
        return;

    for (i = 0; i < PgbCtl->npeers; i++)
    {
        if (PgbCtl->peers[i].in_use && PgbCtl->peers[i].node_id == node_id)
        {
            PgbCtl->peers[i].reachable = true;
            PgbCtl->peers[i].last_seen = GetCurrentTimestamp();
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Inbound RPC handlers
 * ------------------------------------------------------------------------- */

/* rpc_ping() -> this node's id (pure reachability probe) */
Datum
pgbully_rpc_ping(PG_FUNCTION_ARGS)
{
    int32       id;

    require_shmem();
    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    id = PgbCtl->my_node_id;
    LWLockRelease(PgbCtl->lock);

    PG_RETURN_INT32(id);
}

/*
 * rpc_election(from) -> this node's id.
 *
 * A lower-id node told us it is starting an election.  By answering at all we
 * tell it to back off; we then start our own election so the highest live
 * node ends up as leader.
 */
Datum
pgbully_rpc_election(PG_FUNCTION_ARGS)
{
    int32       from = PG_GETARG_INT32(0);
    int32       id;

    require_shmem();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    id = PgbCtl->my_node_id;
    mark_peer_seen(from);
    if (from < id)
        PgbCtl->election_requested = true;
    LWLockRelease(PgbCtl->lock);

    wake_worker();
    PG_RETURN_INT32(id);
}

/*
 * rpc_coordinator(leader, term) -> our (possibly updated) term.
 *
 * The authoritative "I won" announcement.  Accept it if its term is at least
 * ours, recording the new leader and becoming a follower.
 */
Datum
pgbully_rpc_coordinator(PG_FUNCTION_ARGS)
{
    int32       leader = PG_GETARG_INT32(0);
    int64       term = PG_GETARG_INT64(1);
    int64       cur;

    require_shmem();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    mark_peer_seen(leader);
    if (term >= PgbCtl->term && leader != PgbCtl->my_node_id)
    {
        PgbCtl->term = term;
        PgbCtl->leader_id = leader;
        PgbCtl->state = PGB_FOLLOWER;
        PgbCtl->last_heartbeat = GetCurrentTimestamp();
        PgbCtl->coordinators_recv++;

        /*
         * A lower-id node should never lead while we are alive: bully it by
         * starting our own election.
         */
        if (leader < PgbCtl->my_node_id)
            PgbCtl->election_requested = true;
    }
    cur = PgbCtl->term;
    LWLockRelease(PgbCtl->lock);

    wake_worker();
    PG_RETURN_INT64(cur);
}

/*
 * rpc_heartbeat(leader, term) -> our (possibly updated) term.
 *
 * Periodic liveness from the current leader.  If the sender's term is newer,
 * follow it; if it equals ours and we are not ourselves leader, refresh the
 * timer.  The returned term lets the sender notice if WE hold a higher term
 * and must step down.
 */
Datum
pgbully_rpc_heartbeat(PG_FUNCTION_ARGS)
{
    int32       leader = PG_GETARG_INT32(0);
    int64       term = PG_GETARG_INT64(1);
    int64       cur;

    require_shmem();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    PgbCtl->heartbeats_recv++;
    mark_peer_seen(leader);

    if (term > PgbCtl->term)
    {
        /* A newer leader exists -- follow it unconditionally. */
        PgbCtl->term = term;
        PgbCtl->leader_id = leader;
        PgbCtl->state = PGB_FOLLOWER;
        PgbCtl->last_heartbeat = GetCurrentTimestamp();
    }
    else if (term == PgbCtl->term && PgbCtl->state != PGB_LEADER)
    {
        PgbCtl->leader_id = leader;
        PgbCtl->state = PGB_FOLLOWER;
        PgbCtl->last_heartbeat = GetCurrentTimestamp();
    }

    /* Reassert dominance over any lower-id leader. */
    if (leader < PgbCtl->my_node_id)
        PgbCtl->election_requested = true;

    cur = PgbCtl->term;
    LWLockRelease(PgbCtl->lock);

    wake_worker();
    PG_RETURN_INT64(cur);
}

/* -------------------------------------------------------------------------
 * Public monitoring / control
 * ------------------------------------------------------------------------- */

Datum
pgbully_leader(PG_FUNCTION_ARGS)
{
    int32       leader;

    require_shmem();
    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    leader = PgbCtl->leader_id;
    LWLockRelease(PgbCtl->lock);

    if (leader == PGBULLY_NO_LEADER)
        PG_RETURN_NULL();
    PG_RETURN_INT32(leader);
}

Datum
pgbully_is_leader(PG_FUNCTION_ARGS)
{
    bool        is_leader;

    require_shmem();
    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    is_leader = (PgbCtl->state == PGB_LEADER &&
                 PgbCtl->leader_id == PgbCtl->my_node_id);
    LWLockRelease(PgbCtl->lock);

    PG_RETURN_BOOL(is_leader);
}

Datum
pgbully_term(PG_FUNCTION_ARGS)
{
    int64       term;

    require_shmem();
    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    term = PgbCtl->term;
    LWLockRelease(PgbCtl->lock);

    PG_RETURN_INT64(term);
}

Datum
pgbully_state(PG_FUNCTION_ARGS)
{
    PgbState    s;

    require_shmem();
    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    s = PgbCtl->state;
    LWLockRelease(PgbCtl->lock);

    PG_RETURN_TEXT_P(cstring_to_text(pgbully_state_name(s)));
}

Datum
pgbully_my_node_id(PG_FUNCTION_ARGS)
{
    int32       id;

    require_shmem();
    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    id = PgbCtl->my_node_id;
    LWLockRelease(PgbCtl->lock);

    if (id <= 0)
        PG_RETURN_NULL();
    PG_RETURN_INT32(id);
}

/* force_election() -> void : ask the worker to run an election now. */
Datum
pgbully_force_election(PG_FUNCTION_ARGS)
{
    require_shmem();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    PgbCtl->election_requested = true;
    LWLockRelease(PgbCtl->lock);

    wake_worker();
    PG_RETURN_VOID();
}

/*
 * status() -> one composite row summarizing this node.  Column set is
 * declared in the SQL file (OUT parameters).
 */
Datum
pgbully_status(PG_FUNCTION_ARGS)
{
    TupleDesc   tupdesc;
    Datum       values[11];
    bool        nulls[11];
    HeapTuple   tuple;
    PgbShared   snap;
    TimestampTz now = GetCurrentTimestamp();

    require_shmem();

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context "
                        "that cannot accept type record")));
    tupdesc = BlessTupleDesc(tupdesc);

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    snap = *PgbCtl;
    LWLockRelease(PgbCtl->lock);

    memset(nulls, 0, sizeof(nulls));

    /* node_id */
    if (snap.my_node_id > 0)
        values[0] = Int32GetDatum(snap.my_node_id);
    else
        nulls[0] = true;

    /* state */
    values[1] = CStringGetTextDatum(pgbully_state_name(snap.state));

    /* is_leader */
    values[2] = BoolGetDatum(snap.state == PGB_LEADER &&
                             snap.leader_id == snap.my_node_id);

    /* leader_id */
    if (snap.leader_id == PGBULLY_NO_LEADER)
        nulls[3] = true;
    else
        values[3] = Int32GetDatum(snap.leader_id);

    /* term */
    values[4] = Int64GetDatum(snap.term);

    /* heartbeat_age_ms */
    if (snap.last_heartbeat == 0)
        nulls[5] = true;
    else
        values[5] = Int64GetDatum(pgb_ts_diff_ms(snap.last_heartbeat, now));

    /* num_nodes */
    values[6] = Int32GetDatum(snap.npeers);

    /* elections_started / won, heartbeats_recv, coordinators_recv */
    values[7] = Int64GetDatum(snap.elections_started);
    values[8] = Int64GetDatum(snap.elections_won);
    values[9] = Int64GetDatum(snap.heartbeats_recv);
    values[10] = Int64GetDatum(snap.coordinators_recv);

    tuple = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * peers() -> set of rows, one per configured node.
 */
Datum
pgbully_peers(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    PgbPeer     peers[PGBULLY_MAX_NODES];
    int         npeers;
    int32       my_id;
    int32       leader_id;
    TimestampTz now = GetCurrentTimestamp();
    int         i;

    require_shmem();

    InitMaterializedSRF(fcinfo, 0);

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    npeers = PgbCtl->npeers;
    my_id = PgbCtl->my_node_id;
    leader_id = PgbCtl->leader_id;
    memcpy(peers, PgbCtl->peers, sizeof(PgbPeer) * npeers);
    LWLockRelease(PgbCtl->lock);

    for (i = 0; i < npeers; i++)
    {
        Datum       values[6];
        bool        nulls[6];

        if (!peers[i].in_use)
            continue;

        memset(nulls, 0, sizeof(nulls));

        values[0] = Int32GetDatum(peers[i].node_id);
        values[1] = CStringGetTextDatum(peers[i].conninfo);
        values[2] = BoolGetDatum(peers[i].node_id == my_id);
        values[3] = BoolGetDatum(peers[i].node_id == leader_id);

        if (peers[i].node_id == my_id)
        {
            /* We never probe ourselves; report as reachable "now". */
            values[4] = BoolGetDatum(true);
            values[5] = TimestampTzGetDatum(now);
        }
        else
        {
            values[4] = BoolGetDatum(peers[i].reachable);
            if (peers[i].last_seen == 0)
                nulls[5] = true;
            else
                values[5] = TimestampTzGetDatum(peers[i].last_seen);
        }

        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    }

    return (Datum) 0;
}
