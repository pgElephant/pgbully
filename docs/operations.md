# Operations

This guide covers running pgBully in production: monitoring, integrating
leadership into your application, handling failover, the consistency caveats of
a timeout-based algorithm, and troubleshooting.

## Monitoring

The first stop is always `pgbully.status()`:

```sql
SELECT * FROM pgbully.status();
```

Useful derived checks:

```sql
-- Is this node the leader?
SELECT pgbully.is_leader();

-- Which nodes are currently unreachable from here?
SELECT node_id, last_seen
FROM   pgbully.peers()
WHERE  NOT reachable AND NOT is_self;

-- How stale is our view of the leader? (followers only)
SELECT heartbeat_age_ms FROM pgbully.status();
```

### Suggested alerts

| Condition | Query | Meaning |
|---|---|---|
| No leader | `pgbully.leader() IS NULL` for > a few `election_timeout`s | cluster cannot converge |
| Heartbeat stale on a follower | `heartbeat_age_ms > 3 × election_timeout` | leader may be gone / unreachable |
| Election churn | `elections_started` climbing steadily | timeouts too tight, or flapping network |
| Peer down | `peers().reachable = false` persists | investigate that node / network path |

### Logs

pgBully logs state transitions at `LOG` level, e.g.:

```
LOG:  pgbully: election worker started for node 3
LOG:  pgbully: node 2 starting election
LOG:  pgbully: node 3 won election, becoming leader (term 4)
LOG:  pgbully: node 2 deferring to a higher-id peer
LOG:  pgbully: node 5 stepping down; observed higher term 7
```

Grepping `grep pgbully: $PGDATA/log/*.log` gives a full election history.

## Using leadership in your application

pgBully reports who leads; *acting* on it is your responsibility. Common
patterns:

**Singleton scheduled job** — only the leader runs it:

```sql
SELECT run_job() WHERE pgbully.is_leader();
```

**Connection routing** — a proxy/health check that only reports a node healthy
when it leads:

```sql
-- health endpoint returns 200 only on the leader
SELECT pgbully.is_leader();
```

**Choosing a write target** — application picks the node where
`pgbully.is_leader()` is true.

## Manual operations

### Force an election

```sql
SELECT pgbully.force_election();
```

### Drain a node for maintenance

Set `pgbully.enabled = off` and reload — the node relinquishes leadership and
stops participating without being removed from `pgbully.nodes`:

```sql
ALTER SYSTEM SET pgbully.enabled = off;
SELECT pg_reload_conf();
```

Re-enable with `on` + reload.

### Add or remove a node

1. Update `pgbully.nodes` **on every node** to the new membership.
2. `SELECT pg_reload_conf();` on every node.
3. On a new node, also set its `pgbully.node_id` (restart, since it is
   `POSTMASTER`-context) and run `CREATE EXTENSION pgbully`.

Because the membership list is static config, keep it under configuration
management so all nodes stay in sync.

## Failover behavior

| Event | Result |
|---|---|
| Leader process/host dies | followers time out after `election_timeout`; the highest remaining id becomes leader |
| Dead leader returns (higher id) | it re-runs an election on startup and reclaims leadership |
| Network blip shorter than `election_timeout` | no election; heartbeats resume |
| Follower dies | no election; it is just marked unreachable |

Expected detection time after a leader failure is roughly `election_timeout`
plus the time to probe higher-id peers.

## Consistency caveats

pgBully is a **timeout-based** election, not a quorum consensus protocol. Be
aware of the following before relying on it for correctness:

- **Partitions can produce two leaders, temporarily.** If the network splits,
  each side may elect a leader within its partition. The monotonic **term**
  resolves this automatically when the partition heals — the lower-term leader
  observes a higher term and steps down — but *during* the split both sides may
  believe they lead. If two simultaneous writers would corrupt your data, add
  an external fence (e.g. a shared lock, a lease in a quorum store, or storage
  fencing) and gate it on `pgbully.is_leader()`.
- **Leadership is advisory.** pgBully never blocks writes or moves data. A node
  reporting `is_leader() = true` is only telling you the *election* result.
- **Clock independence.** pgBully uses elapsed-time intervals, not wall-clock
  comparisons between nodes, so it does not require synchronized clocks — but
  very large scheduling stalls (e.g. a VM frozen longer than
  `election_timeout`) can still trigger an election.

For the algorithmic details behind these points, see
[algorithm.md](algorithm.md#guarantees-and-limits).

## Troubleshooting

### `ERROR: pgbully is not active`

The extension is not in `shared_preload_libraries`, or the server was not
restarted after adding it. Fix the setting and restart.

### No leader is ever elected

- Check `pgbully.node_id` is set, `> 0`, and present in `pgbully.nodes` on
  every node (the worker logs a warning and idles otherwise).
- Verify peers can actually connect: from one node,
  `psql 'host=… port=… dbname=…'` using the *exact* conninfo from
  `pgbully.nodes`. Check `pg_hba.conf` and credentials.
- Confirm `CREATE EXTENSION pgbully` ran in the `dbname` each conninfo targets;
  otherwise `rpc_*` calls fail and peers look unreachable.

### Constant elections / term keeps climbing

`election_timeout` is too close to `heartbeat_interval`, or the network is
dropping heartbeats. Increase `election_timeout` (rule of thumb: ≥ 3–5×
`heartbeat_interval`) and ensure `connect_timeout < heartbeat_interval`.

### A node won't give up leadership

Another node has an equal or lower id and cannot see the higher-id leader.
Check reachability with `pgbully.peers()` from each side; a one-way network
path (A can reach B but not vice versa) is a common culprit.

### Worker not running

Look for `pgbully: election worker started` in the log. If absent, the library
failed to preload — check `shared_preload_libraries` and the server log for
load errors. The worker auto-restarts 5 seconds after a crash.
