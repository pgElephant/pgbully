# Getting help

## Documentation first

Most questions are answered in the docs:

| Question | Document |
|---|---|
| How do I build and install it? | [docs/installation.md](docs/installation.md) |
| What does each setting do? | [docs/configuration.md](docs/configuration.md) |
| What SQL can I call? | [docs/api.md](docs/api.md) |
| How do I drive it from a cluster manager? | [docs/cluster-api.md](docs/cluster-api.md) |
| How does the algorithm behave? | [docs/algorithm.md](docs/algorithm.md) |
| It is not electing a leader | [docs/operations.md](docs/operations.md#troubleshooting) |
| Something more general | [docs/faq.md](docs/faq.md) |

## Asking a question

Open a [discussion](https://github.com/pgElephant/pgbully/discussions) for
usage questions, design questions, and "is this the right tool for my case".

## Reporting a bug

Open an [issue](https://github.com/pgElephant/pgbully/issues/new/choose). The
report is much easier to act on with:

- your PostgreSQL version and platform;
- the `pgbully.*` settings from every node;
- `SELECT * FROM pgbully.status();` and `SELECT * FROM pgbully.peers();` from
  each node;
- the server log lines matching `pgbully:` from around the time of the problem
  (`SELECT pgbully.set_debug(true);` makes the worker much more talkative).

## Reporting a vulnerability

Please do **not** open a public issue. See [SECURITY.md](SECURITY.md).

## Commercial support

pgBully is maintained by pgEdge, Inc. For commercial support, contact
<support@pgedge.com>.
