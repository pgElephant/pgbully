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
- Support for PostgreSQL 15, 16, 17 and 18.
- TAP regression suite (`t/001_bully.pl`) and a dependency-free shell
  integration test (`test/integration.sh`).
- Full documentation set under `docs/`.

[Unreleased]: https://github.com/pgedge/pgBully/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/pgedge/pgBully/releases/tag/v1.0.0
