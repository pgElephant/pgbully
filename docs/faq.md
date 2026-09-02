# FAQ

### Does every PostgreSQL node need the extension installed?

**Yes.** pgBully has no central coordinator. Every participating node must:

1. load `pgbully` via `shared_preload_libraries`,
2. have a unique `pgbully.node_id`,
3. share the same `pgbully.nodes` membership list, and
4. have `CREATE EXTENSION pgbully` run in the database its peers connect to.

Each node's background worker connects to the *other* nodes over libpq to run
the election. A node that is missing the extension simply looks unreachable to
its peers.

### How do nodes talk to each other?

Over libpq, on the normal PostgreSQL port. Each message is just a SQL function
call (`pgbully.rpc_election`, `rpc_coordinator`, `rpc_heartbeat`, `rpc_ping`)
executed on the receiving node. This means peer traffic uses your existing
authentication, TLS, and `pg_hba.conf` rules — there is no separate listener.

### Which node becomes the leader?

The reachable node with the **highest `pgbully.node_id`**. Assign the highest
id to the node you most want to lead under normal conditions.

### What happens when the leader fails?

Followers stop receiving heartbeats. After `pgbully.election_timeout`, the
highest-id surviving node wins a fresh election and announces itself. Typical
failover time ≈ `election_timeout` plus a brief probe of higher-id peers.

### What happens when a failed (higher-id) leader comes back?

It re-runs an election on startup and **reclaims** leadership — this is the
defining "bully" behavior. If you prefer a stable leader that is *not*
preempted, that would require a different policy; pgBully implements the
classic algorithm.

### Is this Raft / Paxos? Is it safe against split-brain?

No — it is the Bully *election* algorithm, not a quorum consensus protocol. It
does not use majority voting. During a network partition each side may elect
its own leader; the monotonic **term** makes the lower-term leader step down
automatically once connectivity is restored. If two simultaneous leaders would
be unsafe for your workload, gate leader-only actions behind an external fence.
See [operations.md](operations.md#consistency-caveats) and
[algorithm.md](algorithm.md#guarantees-and-limits).

### Can I use it from something that expects a generic consensus backend?

Yes. Besides its native functions, pgBully implements the vendor-neutral
cluster-manager interface — `pgbully.get_cluster_status()`,
`pgbully.get_nodes()`, `pgbully.is_leader()`, `pgbully.member_list` and the
rest — so an application written against that interface runs on pgBully
unchanged. See [cluster-api.md](cluster-api.md).

The parts of that interface covering a replicated log and a key/value store
have no equivalent in the Bully algorithm. Those functions exist, so a caller
probing for them gets a definite answer, but every one raises
`feature_not_supported` (SQLSTATE `0A000`) rather than pretending to work. An
application can catch that and fall back:

```sql
DO $$
BEGIN
    PERFORM pgbully.kv_put('k', 'v');
EXCEPTION WHEN feature_not_supported THEN
    RAISE NOTICE 'this backend elects a leader only';
END $$;
```

### Does pgBully move data, promote replicas, or fence writes?

No. It elects a leader and tells you who it is via `pgbully.is_leader()` /
`pgbully.leader()`. Acting on that (promotion, routing, fencing, running a
singleton job) is up to your application or tooling. It is a coordination
primitive, not a full HA stack.

### Does it require synchronized clocks?

No. It measures elapsed time intervals locally; it does not compare wall-clock
timestamps across nodes.

### How many nodes are supported?

Up to 64. Practical clusters are usually 3–7.

### Can I take a node out without removing it from the config?

Yes: set `pgbully.enabled = off` and reload. The node stops participating and
gives up any leadership while remaining in `pgbully.nodes`. Re-enable with `on`.

### Do I need to restart to change configuration?

Only for `pgbully.node_id` (and `shared_preload_libraries`). All other GUCs —
including `pgbully.nodes` — are `SIGHUP`: edit and `SELECT pg_reload_conf()`.

### Which PostgreSQL versions are supported?

15, 16, 17 and 18. Earlier versions lack the `shmem_request_hook` the extension
relies on and are not supported.

### How do I test it locally?

Run the bundled, dependency-free integration test:

```bash
PG_CONFIG=/path/to/pg_config ./test/integration.sh
```

It spins up three throwaway clusters and asserts election, failover, and
reclaim. The TAP suite (`make installcheck`) does the same via
`PostgreSQL::Test::Cluster` (requires the `IPC::Run` Perl module).

### What is a "term"?

A monotonically increasing epoch number, incremented each time a node becomes
leader. It lets nodes ignore stale messages from a deposed leader and lets a
superseded leader notice it must step down. See
[algorithm.md](algorithm.md#the-term-extension).

### The worker logs a warning that my node "is not listed in pgbully.nodes".

`pgbully.node_id` does not match any id in `pgbully.nodes`. Fix one or the
other so this node's id appears exactly once in the membership list.
