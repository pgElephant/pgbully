# Changelog

All notable changes to pgBully are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-06-03

Initial release.

### Added
- Bully leader-election algorithm implemented as a per-node background worker.
- Term-based extension to the classic algorithm: monotonic epoch that fences
  out stale leaders and triggers voluntary step-down.
- Automatic failover to the highest-id surviving node, and reclaim by a
  recovered higher-id node ("the bully returns").
- libpq-based peer transport (messages delivered as `pgbully.rpc_*` SQL calls)
  with cached connections, connect/statement timeouts, and TCP keepalives.
- GUCs: `pgbully.node_id`, `pgbully.nodes`, `pgbully.heartbeat_interval`,
  `pgbully.election_timeout`, `pgbully.connect_timeout`, `pgbully.enabled`.
- Monitoring/control SQL surface: `node_id()`, `leader()`, `is_leader()`,
  `state()`, `term()`, `status()`, `peers()`, the `cluster` view, and
  `force_election()`.
- Cluster-manager API: the vendor-neutral function and view set a control
  plane expects from any consensus backend, so an application written against
  that interface runs on pgBully unchanged. Functions are unqualified
  (`pgbully.get_cluster_status()`, `pgbully.get_nodes()`, `pgbully.is_leader()`,
  `pgbully.get_leader()`, `pgbully.get_term()`, `pgbully.init()`,
  `pgbully.add_node()`, `pgbully.remove_node()`, `pgbully.get_worker_state()`,
  `pgbully.get_version()`, `pgbully.test()`, `pgbully.set_debug()`,
  `pgbully.get_queue_status()`, `pgbully.get_nodes_json()`); the etcd-style
  views (`pgbully.member_list`, `pgbully.endpoint_status`, ...) live in the
  `pgbully` schema, alongside unqualified `pgbully.cluster_state`,
  `pgbully.cluster_overview`, `pgbully.worker_status`, `pgbully.nodes` and
  `pgbully.log_status`.
- `pgbully.add_node()` / `pgbully.remove_node()` change membership in shared
  memory at once; `pgbully.nodes` stays the source of truth and a reload
  restores it.
- `pgbully.set_debug()` and per-iteration worker state logging.
- Log-replication and key/value entry points are declared with their full
  signatures and raise `feature_not_supported` (`0A000`), so a caller probing
  for them gets a precise error instead of "function does not exist".
- Support for PostgreSQL 15, 16, 17 and 18.
- TAP regression suites (`t/001_bully.pl` for election and failover,
  `t/002_cluster_api.pl` for the cluster-manager API) and a dependency-free
  shell integration test (`test/integration.sh`).
- Full documentation set under `docs/`.

[Unreleased]: https://github.com/pgElephant/pgbully/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/pgElephant/pgbully/releases/tag/v1.0.0
