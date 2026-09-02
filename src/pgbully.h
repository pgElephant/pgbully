/*-------------------------------------------------------------------------
 *
 * pgbully.h
 *      Shared definitions for the pgBully distributed leader-election
 *      extension.
 *
 * pgBully implements the classic "Bully" leader-election algorithm
 * (Garcia-Molina, 1982) for a cluster of PostgreSQL nodes.  Every node runs
 * a background worker that exchanges messages with its peers over libpq.
 * The node with the highest id that is currently reachable always wins the
 * election -- it "bullies" the lower-numbered nodes into submission.
 *
 * Message types (all delivered as SQL function calls on the receiver):
 *
 *   ELECTION(from)         a lower node announces it is starting an election.
 *                          The receiver answers simply by being reachable
 *                          (the libpq call succeeds), and itself starts an
 *                          election because a higher node should take over.
 *
 *   COORDINATOR(id, term)  the new leader announces itself to everyone.
 *
 *   HEARTBEAT(id, term)    the current leader periodically proves liveness.
 *
 *   PING()                 a pure reachability probe.
 *
 * A monotonically increasing "term" is layered on top of the textbook
 * algorithm so that stale COORDINATOR/HEARTBEAT messages from a deposed
 * leader are ignored, and so a leader that discovers a higher term steps
 * down cleanly.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGBULLY_H
#define PGBULLY_H

#include "postgres.h"

#include "datatype/timestamp.h"
#include "storage/latch.h"
#include "storage/lwlock.h"

#define PGBULLY_MAX_NODES       64
#define PGBULLY_CONNINFO_LEN    512
#define PGBULLY_SHMEM_MAGIC     0x42554C4C  /* "BULL" */

/* Node id used to mean "no leader is currently known". */
#define PGBULLY_NO_LEADER       0

/*
 * Role of this node in the cluster.
 */
typedef enum PgbState
{
    PGB_FOLLOWER = 0,   /* following a known leader (or waiting for one) */
    PGB_CANDIDATE,      /* actively running an election right now */
    PGB_WAITING,        /* sent ELECTION to higher nodes, awaiting a winner */
    PGB_LEADER          /* this node is the elected coordinator */
} PgbState;

/*
 * A configured peer.  The conninfo and node_id come from the
 * pgbully.nodes GUC; the runtime fields are maintained by the worker.
 */
typedef struct PgbPeer
{
    int32           node_id;
    char            conninfo[PGBULLY_CONNINFO_LEN];
    bool            in_use;

    /* runtime state, protected by PgbShared->lock */
    bool            reachable;          /* last contact attempt succeeded */
    TimestampTz     last_seen;          /* last successful contact */
} PgbPeer;

/*
 * The single shared-memory control block.  All mutable fields are protected
 * by ->lock except where noted.
 */
typedef struct PgbShared
{
    uint32          magic;              /* sanity / re-init detection */
    LWLock         *lock;               /* protects everything below */

    int32           my_node_id;         /* this node's id (from GUC) */
    PgbState        state;
    int64           term;               /* current term/epoch */
    int32           leader_id;          /* current leader, 0 if none */

    TimestampTz     last_heartbeat;     /* last heartbeat heard from leader */
    TimestampTz     election_started;   /* when the current election began */
    TimestampTz     leader_since;       /* when we became leader (if leader) */

    Latch          *worker_latch;       /* worker's latch, for wakeups */
    pid_t           worker_pid;

    /*
     * Inbox flags raised by RPC backends and consumed by the worker.  The
     * worker is woken via worker_latch whenever one is set.
     */
    bool            election_requested; /* a lower node asked us to run one */

    int32           npeers;
    PgbPeer         peers[PGBULLY_MAX_NODES];

    /* counters (monitoring only) */
    int64           elections_started;
    int64           elections_won;
    int64           heartbeats_sent;
    int64           heartbeats_recv;
    int64           coordinators_recv;
} PgbShared;

/* ---- GUC-backed configuration (defined in config.c) ---- */
extern int      pgbully_node_id;
extern char    *pgbully_nodes_raw;
extern int      pgbully_heartbeat_interval_ms;
extern int      pgbully_election_timeout_ms;
extern int      pgbully_connect_timeout_ms;
extern bool     pgbully_enabled;

/* ---- global shared state pointer (defined in shmem.c) ---- */
extern PgbShared *PgbCtl;

/* ---- shmem.c ---- */
extern Size pgbully_shmem_size(void);
extern void pgbully_shmem_request(void);
extern void pgbully_shmem_startup(void);
extern bool pgbully_attach_shmem(void);     /* attach from a normal backend */

/* ---- config.c ---- */
extern void pgbully_define_gucs(void);
extern int  pgbully_parse_nodes(const char *raw, PgbPeer *out, int max,
                                char **errmsg);
extern void pgbully_load_peers(void);       /* (re)load peers into shmem */

/* ---- transport.c ---- */
typedef enum PgbRpcResult
{
    PGB_RPC_OK = 0,
    PGB_RPC_UNREACHABLE,        /* could not connect / call failed */
    PGB_RPC_BADCONFIG
} PgbRpcResult;

extern PgbRpcResult pgbully_send_election(const PgbPeer *peer);
extern PgbRpcResult pgbully_send_coordinator(const PgbPeer *peer,
                                             int32 leader_id, int64 term);
extern PgbRpcResult pgbully_send_heartbeat(const PgbPeer *peer,
                                           int32 leader_id, int64 term,
                                           int64 *peer_term_out);
extern PgbRpcResult pgbully_send_ping(const PgbPeer *peer, int32 *peer_id_out);
extern void pgbully_transport_reset(void);  /* drop all cached connections */

/* ---- worker.c ---- */
extern void pgbully_register_worker(void);
PGDLLEXPORT void pgbully_worker_main(Datum main_arg);

/* ---- helpers shared between modules ---- */
extern const char *pgbully_state_name(PgbState s);

#endif                          /* PGBULLY_H */
