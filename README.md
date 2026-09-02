<div align="center">

# pgBully

**Distributed leader election for PostgreSQL, using the Bully algorithm.**

[![CI](https://github.com/pgElephant/pgbully/actions/workflows/ci.yml/badge.svg)](https://github.com/pgElephant/pgbully/actions/workflows/ci.yml)
[![PostgreSQL 15–18](https://img.shields.io/badge/PostgreSQL-15%20%7C%2016%20%7C%2017%20%7C%2018-336791)](https://www.postgresql.org/)
[![License](https://img.shields.io/badge/license-PostgreSQL-blue.svg)](LICENSE)

</div>

`pgBully` turns a set of independent PostgreSQL servers into a cluster that
**always agrees on exactly one leader**. Each server runs a background worker
that talks to its peers and continuously runs the classic *Bully* leader
election algorithm: the reachable node with the highest id wins, and when it
fails the next-highest takes over automatically — no external coordinator,
no etcd, no ZooKeeper.

```
        ┌──────────┐        ┌──────────┐        ┌──────────┐
        │  node 1  │◀──────▶│  node 2  │◀──────▶│  node 3  │
        │ follower │        │ follower │        │  LEADER  │   ← highest id wins
        └──────────┘        └──────────┘        └──────────┘
              ▲___________________│___________________▲
                    heartbeats + elections over libpq
```

---

## Why pgBully?

- **Self-contained.** The whole protocol runs *inside* PostgreSQL as a
  background worker. The only dependency is libpq, which every PostgreSQL
  install already ships.
- **Deterministic.** The highest-id reachable node always converges to
  leadership — no quorum arithmetic, no surprise winners. A network partition
  can still leave one leader per side; a monotonic term resolves it the moment
  the partition heals. See
  [the limits](docs/algorithm.md#guarantees-and-limits).
- **Observable.** Leadership, term, peer reachability and counters are all
  exposed through plain SQL functions and a view.
- **Portable.** A single codebase builds cleanly on PostgreSQL **15, 16, 17
  and 18**.
- **Tested.** Ships with TAP suites and a dependency-free shell integration
  test that stand up real multi-node clusters and exercise election, failover
  and reclaim.

## What it is *not*

pgBully elects a leader; it does **not** move data, fence writes, or perform
failover of replication on its own. It is a building block: your application,
connection router, or replication tooling consumes `pgbully.is_leader()` /
`pgbully.leader()` and decides what to do. See
[docs/operations.md](docs/operations.md) for integration patterns and the
consistency caveats of a timeout-based algorithm.

---

## Quick start

> Every node in the cluster must install the extension and be told about all
> the other nodes. See the [topology note](#topology-every-node-runs-it).

**1. Build and install** (on each node):

```bash
make PG_CONFIG=/path/to/pg_config
sudo make install PG_CONFIG=/path/to/pg_config
```

**2. Configure** `postgresql.conf` on each node — identical `pgbully.nodes`,
unique `pgbully.node_id`:

```ini
# --- node 3's postgresql.conf ---
shared_preload_libraries = 'pgbully'
pgbully.node_id = 3
pgbully.nodes = '1: host=10.0.0.1 port=5432 dbname=postgres,
                 2: host=10.0.0.2 port=5432 dbname=postgres,
                 3: host=10.0.0.3 port=5432 dbname=postgres'
pgbully.heartbeat_interval = '1s'
pgbully.election_timeout   = '5s'
```

**3. Restart** PostgreSQL (required — `shared_preload_libraries`), then create
the extension once per node:

```sql
CREATE EXTENSION pgbully;
```

**4. Observe** the cluster:

```sql
SELECT * FROM pgbully.status();
```

```
 node_id │  state   │ is_leader │ leader_id │ term │ heartbeat_age_ms │ num_nodes
─────────┼──────────┼───────────┼───────────┼──────┼──────────────────┼───────────
       3 │ leader   │ t         │         3 │    1 │                0 │         3
```

```sql
SELECT node_id, is_leader, reachable, last_seen FROM pgbully.cluster;
```

---

## Topology: every node runs it

**Yes — the extension must be installed and configured on _every_ PostgreSQL
instance that participates.** There is no central server. Each node:

1. loads `pgbully` via `shared_preload_libraries`;
2. has its own unique `pgbully.node_id`;
3. shares the *same* `pgbully.nodes` membership list;
4. runs one background worker that connects to the **other** nodes over libpq
   and exchanges `ELECTION`, `COORDINATOR`, `HEARTBEAT` and `PING` messages.

Those messages are delivered as ordinary SQL calls (`pgbully.rpc_*`) on the
receiving node, so all traffic uses the normal PostgreSQL port and
authentication. A node that cannot be reached is simply treated as down.

---

## SQL surface

| Function | Returns | Description |
|---|---|---|
| `pgbully.node_id()` | `int` | This node's configured id |
| `pgbully.leader()` | `int` | Current leader's id (`NULL` if none yet) |
| `pgbully.is_leader()` | `bool` | Is *this* node the leader? |
| `pgbully.state()` | `text` | `follower` / `candidate` / `waiting` / `leader` |
| `pgbully.term()` | `bigint` | Current election term/epoch |
| `pgbully.status()` | `record` | One-row summary + counters |
| `pgbully.peers()` | `setof record` | Per-node membership & reachability |
| `pgbully.cluster` | view | Friendly view over `peers()` |
| `pgbully.force_election()` | `void` | Manually trigger an election |

Full reference: [docs/api.md](docs/api.md).

### Cluster-manager API

Alongside the native functions above, pgBully implements the vendor-neutral
interface a cluster manager expects from any consensus backend, so an
application written against that interface runs on pgBully unchanged:

```sql
SELECT * FROM pgbully.get_cluster_status();
SELECT * FROM pgbully.get_nodes();
SELECT * FROM pgbully.member_list;      -- etcdctl-style member listing
SELECT pgbully.is_leader(), pgbully.get_leader(), pgbully.get_term();
```

It also brings a small replicated key/value store for cluster-scoped
configuration — writes on the leader, local reads everywhere, and a node that
was down heals itself when it returns:

```sql
SELECT pgbully.kv_put('app/mode', 'active');   -- leader only
SELECT pgbully.kv_get('app/mode');             -- any node, local read
```

It is not a quorum store: a write is durable on the leader and best effort on
the followers, so a partition can strand recent writes. The
[guarantees are spelled out](docs/cluster-api.md#what-it-guarantees-and-what-it-does-not)
in full.

Full reference: [docs/cluster-api.md](docs/cluster-api.md).

## Configuration

| GUC | Default | Description |
|---|---|---|
| `pgbully.node_id` | `0` | Unique id for this node (**required**, `POSTMASTER`) |
| `pgbully.nodes` | `''` | `id:conninfo` membership list (`SIGHUP`) |
| `pgbully.heartbeat_interval` | `1s` | Leader heartbeat cadence (`SIGHUP`) |
| `pgbully.election_timeout` | `5s` | Silence before a follower calls an election (`SIGHUP`) |
| `pgbully.connect_timeout` | `2s` | Per-peer contact timeout (`SIGHUP`) |
| `pgbully.enabled` | `on` | Participate in elections (`SIGHUP`) |

Tuning guidance: [docs/configuration.md](docs/configuration.md).

---

## Testing

```bash
# Dependency-free: spins up a real 3-node cluster and asserts the invariants
PG_CONFIG=/path/to/pg_config ./test/integration.sh

# Canonical TAP suite (needs the IPC::Run perl module)
make installcheck PG_CONFIG=/path/to/pg_config
```

## Documentation

| Document | Contents |
|---|---|
| [docs/index.md](docs/index.md) | Documentation home |
| [docs/installation.md](docs/installation.md) | Build & install on PG 15–18 |
| [docs/configuration.md](docs/configuration.md) | Every GUC, with tuning advice |
| [docs/architecture.md](docs/architecture.md) | Workers, shared memory, transport |
| [docs/algorithm.md](docs/algorithm.md) | The Bully algorithm, term extension, proofs of intent |
| [docs/api.md](docs/api.md) | Complete SQL reference |
| [docs/cluster-api.md](docs/cluster-api.md) | The cluster-manager interface |
| [docs/operations.md](docs/operations.md) | Monitoring, failover, consistency caveats, troubleshooting |
| [docs/faq.md](docs/faq.md) | Frequently asked questions |

## Compatibility

| PostgreSQL | Status |
|---|---|
| 18 | ✅ Supported (CI) |
| 17 | ✅ Supported (CI) |
| 16 | ✅ Supported (CI) |
| 15 | ✅ Supported (CI) |
| ≤ 14 | ❌ Not supported (`shmem_request_hook` required) |

## Repository layout

| Path | Contents |
|---|---|
| `src/` | C sources: module init, GUCs, shared memory, worker, transport, SQL functions |
| `include/` | `pgbully.h` (shared state and prototypes) and `compat.h` (PG 15–18 shims) |
| `sql/` | The extension install script |
| `t/` | TAP suites, run by `make installcheck` |
| `test/` | Dependency-free shell integration test |
| `docs/` | Documentation, indexed by [docs/index.md](docs/index.md) |
| `.github/` | CI workflow, issue and pull-request templates |

## Contributing

Bug reports, ideas and pull requests are welcome — see
[CONTRIBUTING.md](CONTRIBUTING.md). Please also read our
[Code of Conduct](CODE_OF_CONDUCT.md) and the [security policy](SECURITY.md).
For help using pgBully, start with [SUPPORT.md](SUPPORT.md).

## License

Released under the [PostgreSQL License](LICENSE).
