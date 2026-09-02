# Installation

pgBully builds against PostgreSQL **15, 16, 17 and 18** using the standard
PGXS build system. It must be installed on **every** node that participates in
the cluster.

## Prerequisites

- PostgreSQL 15 or newer, **including the server development headers**
  (`postgresql-server-dev-NN` on Debian/Ubuntu, `postgresqlNN-devel` on
  RHEL/Rocky, or a source build).
- A C compiler (`gcc` or `clang`) and `make`.
- libpq and its headers (bundled with every PostgreSQL installation; pgBully
  links against it for peer communication).

Confirm your `pg_config` points at the right server:

```bash
pg_config --version      # must report 15.x, 16.x, 17.x or 18.x
```

## Build and install

From a checkout of the repository:

```bash
make PG_CONFIG=/path/to/pg_config
sudo make install PG_CONFIG=/path/to/pg_config
```

If `pg_config` is already on your `PATH`, you can omit `PG_CONFIG=...`.

`make install` copies three artifacts into the locations reported by
`pg_config`:

| Artifact | Destination |
|---|---|
| `pgbully.so` / `.dylib` | `$(pg_config --pkglibdir)` |
| `pgbully.control` | `$(pg_config --sharedir)/extension/` |
| `pgbully--1.0.sql` | `$(pg_config --sharedir)/extension/` |

The build refuses to proceed on PostgreSQL older than 15.

## Enable the extension

pgBully runs a background worker and reserves shared memory, so it **must** be
preloaded. Edit `postgresql.conf`:

```ini
shared_preload_libraries = 'pgbully'
```

> If another preloaded library is already listed, append pgBully:
> `shared_preload_libraries = 'pg_stat_statements,pgbully'`.

Then set this node's identity and the cluster membership (see
[configuration.md](configuration.md) for the full reference):

```ini
pgbully.node_id = 1
pgbully.nodes   = '1: host=10.0.0.1 port=5432 dbname=postgres,
                   2: host=10.0.0.2 port=5432 dbname=postgres,
                   3: host=10.0.0.3 port=5432 dbname=postgres'
```

Restart PostgreSQL (a restart, not a reload, is required to load a
`shared_preload_libraries` entry), then register the SQL objects in the
database named by your conninfo:

```sql
CREATE EXTENSION pgbully;
```

Verify:

```sql
SELECT * FROM pgbully.status();
```

## Multi-node checklist

Repeat on every node, taking care that:

- [ ] `shared_preload_libraries` includes `pgbully` and the server was
      **restarted**.
- [ ] `pgbully.node_id` is **unique** per node and matches an entry in
      `pgbully.nodes`.
- [ ] `pgbully.nodes` is **identical** on every node.
- [ ] Each conninfo specifies a `dbname` in which `CREATE EXTENSION pgbully`
      has been run, and an authentication path (`pg_hba.conf`, password, etc.)
      that lets peers connect.
- [ ] `CREATE EXTENSION pgbully` has been executed in that database on the
      node.

## Uninstall

```sql
DROP EXTENSION pgbully;
```

Then remove `pgbully` from `shared_preload_libraries` and restart. To remove
the files from disk:

```bash
sudo make uninstall PG_CONFIG=/path/to/pg_config
```

## Building for multiple PostgreSQL versions

Because pgBully is version-agnostic across 15–18, the only thing that changes
between targets is which `pg_config` you build against:

```bash
make clean && make install PG_CONFIG=/usr/lib/postgresql/15/bin/pg_config
make clean && make install PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config
make clean && make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
make clean && make install PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
```
