/*-------------------------------------------------------------------------
 *
 * worker.c
 *      The pgBully background worker: a per-node state machine that runs the
 *      Bully leader-election algorithm against the configured peers.
 *
 * One worker runs on every node.  It owns all outbound communication; RPC
 * backends only mutate shared state and wake the worker via its latch.
 *
 * State transitions
 * -----------------
 *   FOLLOWER  --(election timeout, or asked by a lower node)--> CANDIDATE
 *   CANDIDATE --(no higher node answers)--> LEADER
 *   CANDIDATE --(a higher node answers)--> WAITING
 *   WAITING   --(COORDINATOR/HEARTBEAT arrives)--> FOLLOWER
 *   WAITING   --(timeout, no winner)--> CANDIDATE (re-run)
 *   LEADER    --(heard a higher term)--> FOLLOWER (step down)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

#include "compat.h"
#include "pgbully.h"

/* timestamp of our last heartbeat broadcast (worker-local) */
static TimestampTz last_hb_sent = 0;

/* whether we have already warned that this node is misconfigured */
static bool member_warned = false;

static void pgbully_main_loop(void);
static void start_election(void);
static void become_leader(void);
static void send_heartbeats(void);
static void update_peer_status(int32 node_id, bool reachable);

/* -------------------------------------------------------------------------
 * Worker registration
 * ------------------------------------------------------------------------- */
void
pgbully_register_worker(void)
{
    BackgroundWorker worker;

    memset(&worker, 0, sizeof(worker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 5;        /* seconds; restart if it crashes */
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pgbully");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgbully_worker_main");
    snprintf(worker.bgw_name, BGW_MAXLEN, "pgbully election worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "pgbully");

    RegisterBackgroundWorker(&worker);
}

/* -------------------------------------------------------------------------
 * Worker entry point
 * ------------------------------------------------------------------------- */
void
pgbully_worker_main(Datum main_arg)
{
    /* Standard signal wiring: SIGHUP -> reload, SIGTERM -> exit. */
    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();

    if (!pgbully_attach_shmem())
    {
        ereport(LOG,
                (errmsg("pgbully: shared memory not available; worker exiting")));
        return;
    }

    /* Publish our latch/pid so RPC backends can wake us. */
    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    PgbCtl->worker_latch = MyLatch;
    PgbCtl->worker_pid = MyProcPid;
    PgbCtl->state = PGB_FOLLOWER;
    PgbCtl->leader_id = PGBULLY_NO_LEADER;
    PgbCtl->last_heartbeat = GetCurrentTimestamp();
    /*
     * Classic Bully behavior: a node that (re)joins initiates an election so
     * that, if it outranks the incumbent, it reclaims leadership at once.
     */
    PgbCtl->election_requested = true;
    LWLockRelease(PgbCtl->lock);

    pgbully_load_peers();

    ereport(LOG, (errmsg("pgbully: election worker started for node %d",
                         pgbully_node_id)));

    pgbully_main_loop();

    /* Unreachable in practice (die() longjmps), but tidy up regardless. */
    pgbully_transport_reset();
}

/*
 * Confirm this node is actually a configured member.  Returns true and the
 * count of higher-id peers if so.
 */
static bool
node_is_member(int *n_higher_out)
{
    int         i;
    bool        member = false;
    int         higher = 0;

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    for (i = 0; i < PgbCtl->npeers; i++)
    {
        if (!PgbCtl->peers[i].in_use)
            continue;
        if (PgbCtl->peers[i].node_id == PgbCtl->my_node_id)
            member = true;
        else if (PgbCtl->peers[i].node_id > PgbCtl->my_node_id)
            higher++;
    }
    LWLockRelease(PgbCtl->lock);

    if (n_higher_out)
        *n_higher_out = higher;
    return member;
}

static void
pgbully_main_loop(void)
{
    for (;;)
    {
        TimestampTz now;
        PgbState    state;
        int64       elapsed_hb;
        int64       term;
        int32       leader_id;
        bool        debug;
        bool        do_election = false;
        long        naptime;
        int         rc;

        CHECK_FOR_INTERRUPTS();

        if (ConfigReloadPending)
        {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
            pgbully_load_peers();
            pgbully_transport_reset();
            /* Let a corrected (or freshly broken) membership warn again. */
            member_warned = false;
        }

        now = GetCurrentTimestamp();

        if (!pgbully_enabled)
        {
            /* Stand down completely while disabled. */
            LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
            PgbCtl->state = PGB_FOLLOWER;
            PgbCtl->leader_id = PGBULLY_NO_LEADER;
            PgbCtl->election_requested = false;
            LWLockRelease(PgbCtl->lock);
            goto wait;
        }

        if (PgbCtl->my_node_id <= 0 || !node_is_member(NULL))
        {
            if (!member_warned)
            {
                ereport(WARNING,
                        (errmsg("pgbully: node %d is not listed in pgbully.nodes; "
                                "worker is idle", PgbCtl->my_node_id)));
                member_warned = true;
            }
            goto wait;
        }

        /* Snapshot the bits of state we need to make a decision. */
        LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
        state = PgbCtl->state;
        elapsed_hb = pgb_ts_diff_ms(PgbCtl->last_heartbeat, now);
        term = PgbCtl->term;
        leader_id = PgbCtl->leader_id;
        debug = PgbCtl->debug;
        if (PgbCtl->election_requested)
        {
            PgbCtl->election_requested = false;
            do_election = true;
        }
        LWLockRelease(PgbCtl->lock);

        if (debug)
            ereport(LOG,
                    (errmsg("pgbully: state=%s term=" INT64_FORMAT " leader=%d "
                            "heartbeat_age=" INT64_FORMAT "ms election=%s",
                            pgbully_state_name(state), term, leader_id,
                            elapsed_hb, do_election ? "pending" : "no")));

        if (do_election)
        {
            start_election();
        }
        else
        {
            switch (state)
            {
                case PGB_LEADER:
                    if (last_hb_sent == 0 ||
                        pgb_ts_diff_ms(last_hb_sent, now) >= pgbully_heartbeat_interval_ms)
                    {
                        send_heartbeats();
                        last_hb_sent = now;
                    }
                    break;

                case PGB_FOLLOWER:
                case PGB_WAITING:
                    if (elapsed_hb >= pgbully_election_timeout_ms)
                        start_election();
                    break;

                case PGB_CANDIDATE:
                    /* Stuck mid-election (e.g. after a crash); retry. */
                    start_election();
                    break;
            }
        }

wait:
        /*
         * Sleep until the next deadline or until woken.  We keep the nap well
         * below the heartbeat/election intervals so timers stay responsive.
         */
        naptime = pgbully_heartbeat_interval_ms;
        if (pgbully_election_timeout_ms / 4 < naptime)
            naptime = pgbully_election_timeout_ms / 4;
        if (naptime < 50)
            naptime = 50;

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       naptime,
                       PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);

        if (rc & WL_LATCH_SET)
            CHECK_FOR_INTERRUPTS();
    }
}

/* -------------------------------------------------------------------------
 * Algorithm steps
 * ------------------------------------------------------------------------- */

/*
 * Run one round of the Bully election: probe every higher-id peer.  If any of
 * them is reachable, defer to it; otherwise declare victory.
 */
static void
start_election(void)
{
    PgbPeer     peers[PGBULLY_MAX_NODES];
    int         npeers;
    int32       my_id;
    bool        higher_alive = false;
    int         i;

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    PgbCtl->state = PGB_CANDIDATE;
    PgbCtl->election_started = GetCurrentTimestamp();
    PgbCtl->elections_started++;
    my_id = PgbCtl->my_node_id;
    npeers = PgbCtl->npeers;
    memcpy(peers, PgbCtl->peers, sizeof(PgbPeer) * npeers);
    LWLockRelease(PgbCtl->lock);

    ereport(LOG, (errmsg("pgbully: node %d starting election", my_id)));

    for (i = 0; i < npeers; i++)
    {
        PgbRpcResult rc;

        if (!peers[i].in_use || peers[i].node_id <= my_id)
            continue;

        CHECK_FOR_INTERRUPTS();

        rc = pgbully_send_election(&peers[i]);
        update_peer_status(peers[i].node_id, rc == PGB_RPC_OK);
        if (rc == PGB_RPC_OK)
            higher_alive = true;
    }

    if (!higher_alive)
    {
        become_leader();
    }
    else
    {
        /*
         * A higher node is alive and will (or already did) take over.  Wait
         * for its COORDINATOR/HEARTBEAT; reset the timer so we re-run the
         * election if nothing arrives within election_timeout.
         */
        LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
        PgbCtl->state = PGB_WAITING;
        PgbCtl->last_heartbeat = GetCurrentTimestamp();
        LWLockRelease(PgbCtl->lock);

        ereport(LOG,
                (errmsg("pgbully: node %d deferring to a higher-id peer", my_id)));
    }
}

/*
 * Become leader: bump the term, then announce ourselves to every peer.
 */
static void
become_leader(void)
{
    PgbPeer     peers[PGBULLY_MAX_NODES];
    int         npeers;
    int32       my_id;
    int64       term;
    int         i;

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    PgbCtl->term++;
    PgbCtl->state = PGB_LEADER;
    PgbCtl->leader_id = PgbCtl->my_node_id;
    PgbCtl->leader_since = GetCurrentTimestamp();
    PgbCtl->last_heartbeat = PgbCtl->leader_since;
    PgbCtl->elections_won++;
    my_id = PgbCtl->my_node_id;
    term = PgbCtl->term;
    npeers = PgbCtl->npeers;
    memcpy(peers, PgbCtl->peers, sizeof(PgbPeer) * npeers);
    LWLockRelease(PgbCtl->lock);

    last_hb_sent = GetCurrentTimestamp();

    ereport(LOG,
            (errmsg("pgbully: node %d won election, becoming leader (term " INT64_FORMAT ")",
                    my_id, term)));

    for (i = 0; i < npeers; i++)
    {
        PgbRpcResult rc;

        if (!peers[i].in_use || peers[i].node_id == my_id)
            continue;

        CHECK_FOR_INTERRUPTS();

        rc = pgbully_send_coordinator(&peers[i], my_id, term);
        update_peer_status(peers[i].node_id, rc == PGB_RPC_OK);
    }
}

/*
 * Leader duty: send a heartbeat to every peer.  If any peer reports a higher
 * term than ours, a newer leader exists, so step down.
 */
static void
send_heartbeats(void)
{
    PgbPeer     peers[PGBULLY_MAX_NODES];
    int         npeers;
    int32       my_id;
    int64       term;
    bool        step_down = false;
    int64       observed_term = 0;
    int         sent = 0;
    int         i;

    LWLockAcquire(PgbCtl->lock, LW_SHARED);
    my_id = PgbCtl->my_node_id;
    term = PgbCtl->term;
    npeers = PgbCtl->npeers;
    memcpy(peers, PgbCtl->peers, sizeof(PgbPeer) * npeers);
    LWLockRelease(PgbCtl->lock);

    for (i = 0; i < npeers; i++)
    {
        PgbRpcResult rc;
        int64       peer_term = 0;

        if (!peers[i].in_use || peers[i].node_id == my_id)
            continue;

        CHECK_FOR_INTERRUPTS();

        rc = pgbully_send_heartbeat(&peers[i], my_id, term, &peer_term);
        update_peer_status(peers[i].node_id, rc == PGB_RPC_OK);

        if (rc == PGB_RPC_OK)
        {
            sent++;
            if (peer_term > term)
            {
                step_down = true;
                if (peer_term > observed_term)
                    observed_term = peer_term;
            }
        }
    }

    if (sent > 0)
    {
        LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
        PgbCtl->heartbeats_sent += sent;
        LWLockRelease(PgbCtl->lock);
    }

    if (step_down)
    {
        LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
        /* Re-check we are still leader and the term is still stale. */
        if (PgbCtl->state == PGB_LEADER && observed_term > PgbCtl->term)
        {
            PgbCtl->state = PGB_FOLLOWER;
            PgbCtl->leader_id = PGBULLY_NO_LEADER;
            PgbCtl->term = observed_term;
            PgbCtl->last_heartbeat = GetCurrentTimestamp();
        }
        LWLockRelease(PgbCtl->lock);

        ereport(LOG,
                (errmsg("pgbully: node %d stepping down; observed higher term "
                        INT64_FORMAT, my_id, observed_term)));
    }
}

/*
 * Record the outcome of contacting a peer in shared memory.
 */
static void
update_peer_status(int32 node_id, bool reachable)
{
    int         i;
    TimestampTz now = GetCurrentTimestamp();

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    for (i = 0; i < PgbCtl->npeers; i++)
    {
        if (PgbCtl->peers[i].node_id == node_id)
        {
            PgbCtl->peers[i].reachable = reachable;
            if (reachable)
                PgbCtl->peers[i].last_seen = now;
            break;
        }
    }
    LWLockRelease(PgbCtl->lock);
}
