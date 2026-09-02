# pgBully documentation

`pgBully` is a PostgreSQL extension that elects a single leader among a set of
PostgreSQL nodes using the classic **Bully** algorithm. Every node runs a
background worker that exchanges messages with its peers over libpq; the
reachable node with the highest id is always the leader, and failover to the
next-highest node is automatic.

## Start here

- **New to pgBully?** Read the [README](../README.md) for the elevator pitch
  and a quick start, then come back here.
- **Installing?** [installation.md](installation.md).
- **Configuring a cluster?** [configuration.md](configuration.md).

## Table of contents

| Document | What it covers |
|---|---|
| [installation.md](installation.md) | Building and installing on PostgreSQL 15–18 |
| [configuration.md](configuration.md) | Every GUC, defaults, and how to tune timeouts |
| [architecture.md](architecture.md) | Background worker, shared memory, libpq transport, message flow |
| [algorithm.md](algorithm.md) | The Bully algorithm and pgBully's term-based extension |
| [api.md](api.md) | Complete SQL function/view reference |
| [operations.md](operations.md) | Monitoring, manual failover, consistency caveats, troubleshooting |
| [faq.md](faq.md) | Frequently asked questions |

## Mental model in one paragraph

A cluster is a fixed list of `(node_id, conninfo)` pairs, identical on every
node (`pgbully.nodes`). Each node knows which entry is itself
(`pgbully.node_id`). The current leader periodically sends heartbeats to all
peers. If a follower hears nothing for `election_timeout`, it starts an
**election**: it pings every node with a *higher* id. If any answer, it steps
back and waits for one of them to win. If none answer, it declares itself
leader and announces this to everyone. Because the highest reachable id always
wins, the outcome is deterministic.

## Conventions used in these docs

- *Node id* — the integer in `pgbully.node_id`; higher ids win.
- *Term* — a monotonically increasing epoch number used to discard stale
  messages from a deposed leader.
- *Leader / follower / candidate / waiting* — the four worker states; see
  [algorithm.md](algorithm.md).
