---
name: Bug report
about: Report a problem with pgBully
title: "[bug] "
labels: bug
assignees: ''
---

## Description

A clear and concise description of what the bug is.

## Environment

- pgBully version:
- PostgreSQL version (`SELECT version();`):
- Operating system / architecture:
- Number of nodes in the cluster:

## Configuration

Relevant `pgbully.*` settings from each node (redact hostnames/credentials):

```ini
pgbully.node_id =
pgbully.nodes =
pgbully.heartbeat_interval =
pgbully.election_timeout =
```

## Steps to reproduce

1.
2.
3.

## Expected behavior

What you expected to happen.

## Actual behavior

What actually happened. Include the output of `SELECT * FROM pgbully.status();`
from each node if relevant.

## Logs

Any `pgbully:`-prefixed lines from the PostgreSQL server log:

```
```

## Additional context

Anything else that might help.
