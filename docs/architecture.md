# Architecture

pgBully is deliberately small. It has four moving parts: a background worker,
a shared-memory control block, a set of SQL-callable RPC handlers, and a libpq
transport layer. This document explains how they fit together.

```
   ┌─────────────────────────── one PostgreSQL node ───────────────────────────┐
   │                                                                            │
   │   ┌────────────────────┐         shared memory          ┌──────────────┐  │
   │   │  election worker   │◀──────── PgbShared ───────────▶│ RPC backends │  │
   │   │  (background)      │   state, term, leader, peers,  │ (rpc_* funcs)│  │
   │   │                    │   inbox flags, counters, latch │              │  │
   │   └─────────┬──────────┘                                └──────▲───────┘  │
   │             │ libpq (outbound)                        SQL calls │ inbound  │
   └─────────────┼──────────────────────────────────────────────────┼─────────┘
                 │                                                    │
                 ▼ ELECTION / COORDINATOR / HEARTBEAT / PING          │
        ┌──────────────────┐                              ┌──────────────────┐
        │   peer node B    │                              │   peer node C    │
        └──────────────────┘                              └──────────────────┘
```

## 1. The background worker (`src/worker.c`)

One worker runs per node, registered from `_PG_init()` and started after
recovery finishes. It is the only component that performs **outbound**
communication. Its main loop:

1. processes pending `SIGHUP` (config reload) and `SIGTERM` (clean exit);
2. takes a snapshot of the relevant shared state under the control lock;
3. acts on its current role:
   - **leader** — sends heartbeats every `heartbeat_interval`; steps down if a
     peer reports a higher term;
   - **follower / waiting** — starts an election if `election_timeout` has
     elapsed with no heartbeat;
   - **candidate** — (transient) runs the election;
4. sleeps on its latch until the next deadline or until an RPC backend wakes
   it.

The worker holds `BGWORKER_SHMEM_ACCESS` only — it needs no database
connection of its own, because all peer communication is outbound libpq and
all local state lives in shared memory.

## 2. Shared memory (`src/shmem.c`, `struct PgbShared`)

A single fixed-size control block, allocated through the PG15+
`shmem_request_hook` / `shmem_startup_hook` pair and protected by one named
LWLock. It holds:

- the node's role (`state`), current `term`, and known `leader_id`;
- timing: `last_heartbeat`, `election_started`, `leader_since`;
- the parsed peer table (`peers[]`, with per-peer reachability);
- **inbox flags** raised by RPC backends (e.g. `election_requested`) and the
  worker's latch pointer, so backends can hand work to the worker;
- monitoring counters.

Everything mutable is guarded by `PgbShared->lock`. Both the worker and any
backend (running an RPC or a monitoring query) attach to the same block.

## 3. RPC handlers (`src/rpc.c`)

Peer messages are not a bespoke wire protocol — they are ordinary SQL function
calls, executed by normal backends on the receiving node:

| Message | SQL entry point | Effect on receiver |
|---|---|---|
| `PING` | `pgbully.rpc_ping()` | returns this node's id (liveness only) |
| `ELECTION` | `pgbully.rpc_election(from)` | raises `election_requested`; returns this node's id |
| `COORDINATOR` | `pgbully.rpc_coordinator(id, term)` | adopts the new leader/term |
| `HEARTBEAT` | `pgbully.rpc_heartbeat(id, term)` | refreshes the leader timer |

Each handler updates `PgbShared` under the lock and sets the worker's latch so
the worker reacts promptly. Reusing SQL-over-libpq means pgBully inherits
PostgreSQL's authentication, TLS, and connection handling for free.

The same file exposes the read-only monitoring functions
(`status`, `peers`, `leader`, `state`, …) and the `force_election()` control.

## 4. Transport (`src/transport.c`)

The worker's outbound side. It keeps one cached libpq connection per peer for
the lifetime of the worker, reconnecting lazily after a failure and dropping
all connections on config reload. Every call is bounded by:

- libpq `connect_timeout` (derived from `pgbully.connect_timeout`),
- TCP keepalives, and
- a server-side `statement_timeout`,

so a dead or hung peer can never wedge the worker. A failed call simply marks
the peer unreachable for that round.

## Configuration plumbing (`src/config.c`)

Defines the GUCs and parses `pgbully.nodes` into the peer table. Parsing
happens in a GUC check hook (for validation) and in `pgbully_load_peers()`
(which publishes the result into shared memory at startup and after each
reload).

## Why this shape?

- **No external dependencies.** Consensus systems usually bolt on etcd,
  ZooKeeper, or Consul. pgBully needs only what PostgreSQL already provides.
- **Security for free.** Peer RPC rides the normal PostgreSQL port and auth;
  there is no second listener to secure.
- **Crash-friendly.** The worker has `bgw_restart_time = 5`, so PostgreSQL
  restarts it automatically; on restart it re-derives all state from shared
  memory and the peer probes.

## Lifecycle of an election (sequence)

```
follower F           higher peer H
   │  (election_timeout elapses, no heartbeat)
   │── rpc_election(F) ─────────────▶│   H is alive → answers
   │◀──────────── H.node_id ─────────│   F sees a higher node lives → WAITING
   │                                 │── (H runs its own election) ─▶ ...
   │◀── rpc_coordinator(H, term) ────│   eventually a winner announces
   │   adopt H as leader, FOLLOWER   │
```

If no higher peer answers, `F` calls `become_leader()`, bumps the term, and
broadcasts `COORDINATOR` to everyone.
