# Contributing to pgBully

Thanks for your interest in improving pgBully! This document explains how to
build, test, and submit changes.

## Code of conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). By
participating you agree to uphold it.

## Getting started

```bash
git clone https://github.com/pgElephant/pgbully
cd pgBully
make PG_CONFIG=/path/to/pg_config
sudo make install PG_CONFIG=/path/to/pg_config
```

Run the dependency-free integration test before and after your change:

```bash
PG_CONFIG=/path/to/pg_config ./test/integration.sh
```

And, if you have the `IPC::Run` Perl module, the TAP suite:

```bash
make installcheck PG_CONFIG=/path/to/pg_config
```

## Project layout

| Path | Purpose |
|---|---|
| `src/pgbully.c` | module init, hooks, worker registration |
| `src/config.c` | GUCs and `pgbully.nodes` parsing |
| `src/shmem.c` | shared-memory control block lifecycle |
| `src/transport.c` | libpq peer transport |
| `src/worker.c` | background worker + Bully state machine |
| `src/rpc.c` | SQL-callable RPC handlers + monitoring functions |
| `src/cluster_api.c` | the cluster-manager API |
| `include/pgbully.h` | shared state, GUCs, cross-module prototypes |
| `include/compat.h` | cross-version (PG 15–18) shims |
| `sql/` | extension install script |
| `t/` | TAP suites (`make installcheck`) |
| `test/` | dependency-free shell integration test |
| `docs/` | documentation |

## Coding standards

- Follow the surrounding style: four spaces per indent level, braces on their
  own lines, `/* ... */` comments, and the declaration block at the top of each
  function. `.editorconfig` encodes the whitespace rules.
- Keep the extension buildable on **all** of PostgreSQL 15, 16, 17 and 18.
  Hide any version differences behind `include/compat.h`.
- Follow the
  [PostgreSQL error message style guide](https://www.postgresql.org/docs/current/error-style-guide.html):
  primary messages are lowercase and have no trailing period; detail and hint
  messages are complete, capitalized sentences. Use an appropriate `errcode()`
  for errors.
- The build runs with `-Wall`; submissions must compile cleanly with no new
  warnings.
- Guard all shared-memory access with `PgbShared->lock`.

## Submitting changes

1. Fork and create a topic branch.
2. Make your change with a clear, focused commit history.
3. Add or update a test that demonstrates the change.
4. Update `docs/` and `CHANGELOG.md` (the `[Unreleased]` section) as needed.
5. Open a pull request describing the motivation and approach. Fill out the PR
   template.

## Reporting bugs

Use the GitHub issue tracker and the bug-report template. Please include your
PostgreSQL version, OS, the relevant `pgbully.*` settings, and any
`pgbully:`-prefixed log lines.

## Security issues

Do **not** open a public issue for a security vulnerability. Follow
[SECURITY.md](SECURITY.md).
