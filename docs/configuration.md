# Configuration

All configuration is done through GUC parameters in `postgresql.conf` (or via
`ALTER SYSTEM`). pgBully reserves the `pgbully.` prefix.

## Parameters

### `pgbully.node_id`

- **Type:** integer · **Default:** `0` · **Context:** `POSTMASTER` (restart)
- The unique identity of this node within the cluster. Must be greater than
  `0` and must match exactly one entry in `pgbully.nodes`.
- **Higher ids win elections.** Choose ids deliberately: the node you most
  want to be leader under normal conditions should have the highest id.

### `pgbully.nodes`

- **Type:** string · **Default:** `''` · **Context:** `SIGHUP` (reload)
- The full cluster membership, as a comma-separated list of
  `id: conninfo` entries:

  ```ini
  pgbully.nodes = '1: host=10.0.0.1 port=5432 dbname=postgres user=repl,
                   2: host=10.0.0.2 port=5432 dbname=postgres user=repl,
                   3: host=10.0.0.3 port=5432 dbname=postgres user=repl'
  ```

- The `conninfo` is a standard libpq connection string. It **should** include
  `dbname` (the database where `CREATE EXTENSION pgbully` was run) and enough
  to authenticate (e.g. `user`, and a `.pgpass`/`password` as appropriate).
- The same list must be configured on **every** node. The entry whose id
  equals `pgbully.node_id` describes this node and is never contacted.
- Changing this value and issuing `SELECT pg_reload_conf()` re-reads the
  membership and drops cached peer connections — no restart needed.
- Limits: up to 64 nodes; each conninfo up to 511 bytes.

### `pgbully.heartbeat_interval`

- **Type:** integer (ms) · **Default:** `1000` (`1s`) · **Range:** `50` – `600000` · **Context:** `SIGHUP`
- How often the leader sends a heartbeat to every peer. Smaller values detect
  a dead leader faster but generate more traffic.

### `pgbully.election_timeout`

- **Type:** integer (ms) · **Default:** `5000` (`5s`) · **Range:** `100` – `3600000` · **Context:** `SIGHUP`
- How long a follower will go without hearing from the leader before it starts
  an election. **Must be comfortably larger than `heartbeat_interval`** — a
  common rule of thumb is 3–5× — so that a single dropped heartbeat does not
  trigger an unnecessary election.

### `pgbully.connect_timeout`

- **Type:** integer (ms) · **Default:** `2000` (`2s`) · **Range:** `100` – `60000` · **Context:** `SIGHUP`
- Upper bound on the time spent contacting a single peer (used both as libpq's
  `connect_timeout` and as a server-side `statement_timeout` on the RPC call).
  Keep it well below `election_timeout`, since an election may contact several
  peers in sequence.

### `pgbully.enabled`

- **Type:** boolean · **Default:** `on` · **Context:** `SIGHUP`
- When `off`, the worker stops participating: it relinquishes any leadership,
  reports `follower` with no leader, and ignores election timers. Useful for
  draining a node for maintenance without removing it from `pgbully.nodes`.

All three timeouts carry PostgreSQL's millisecond unit, so `'250ms'`, `'2s'`
and `'1min'` are all accepted and `SHOW` reports them in the friendliest unit.
A value outside the range above is rejected when the configuration is loaded.

## Tuning the timeouts

The two timers determine how quickly the cluster reacts to a failed leader and
how tolerant it is of transient network blips. The trade-off:

```
detection time after a leader dies  ≈  election_timeout
                                        + (time to ping higher nodes)
```

Guidelines:

| Goal | Setting |
|---|---|
| Fast failover (LAN, reliable) | `heartbeat_interval = 250ms`, `election_timeout = 1s` |
| Balanced (default) | `heartbeat_interval = 1s`, `election_timeout = 5s` |
| Tolerant (WAN, jittery) | `heartbeat_interval = 2s`, `election_timeout = 15s` |

Always keep the inequality:

```
connect_timeout  <  heartbeat_interval  <<  election_timeout
```

Setting `election_timeout` too close to `heartbeat_interval` causes spurious
elections (and term churn) whenever a heartbeat is briefly delayed. Setting it
too high delays failover.

## Applying changes

| Parameter | How to apply |
|---|---|
| `pgbully.node_id` | Edit `postgresql.conf`, **restart** |
| everything else | Edit / `ALTER SYSTEM`, then `SELECT pg_reload_conf()` |

After a reload, pgBully re-parses `pgbully.nodes` and discards cached peer
connections so new conninfo strings take effect immediately.

Two things are not GUCs. `pgbully.set_debug()` toggles verbose worker logging
at runtime, and `pgbully.add_node()` / `pgbully.remove_node()` change
membership in shared memory without a reload — but `pgbully.nodes` remains the
source of truth, so the next reload overwrites them. See
[cluster-api.md](cluster-api.md).

## Example: three-node cluster

```ini
# ===== common to all three nodes =====
shared_preload_libraries = 'pgbully'
pgbully.nodes = '1: host=db1 port=5432 dbname=postgres,
                 2: host=db2 port=5432 dbname=postgres,
                 3: host=db3 port=5432 dbname=postgres'
pgbully.heartbeat_interval = '1s'
pgbully.election_timeout   = '5s'

# ===== per node (only this line differs) =====
pgbully.node_id = 1      # db1
pgbully.node_id = 2      # db2
pgbully.node_id = 3      # db3
```
