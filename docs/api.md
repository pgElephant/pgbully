# SQL API reference

This page covers pgBully's native surface, which lives in the `pgbully`
schema. Functions fall into two groups: **monitoring/control** (for operators
and applications) and **RPC handlers** (called by peer nodes; you normally
never invoke these directly).

pgBully also implements the vendor-neutral cluster-manager interface —
`pgbully.get_cluster_status()`, `pgbully.member_list` and the rest. That is
documented separately in [cluster-api.md](cluster-api.md).

Every function raises an error if the extension is not active (not present in
`shared_preload_libraries`):

```
ERROR:  pgbully is not active
HINT:   Add "pgbully" to shared_preload_libraries and restart the server.
```

---

## Monitoring & control

### `pgbully.node_id() → integer`

This node's configured id, or `NULL` if `pgbully.node_id` is unset. `STABLE`.

```sql
SELECT pgbully.node_id();   -- e.g. 3
```

### `pgbully.leader() → integer`

The id of the node currently believed to be leader, or `NULL` if none is known
yet (e.g. during the first election). `STABLE`.

```sql
SELECT pgbully.leader();    -- e.g. 3
```

### `pgbully.is_leader() → boolean`

`true` if *this* node is the current leader. The canonical guard for
"should I do the leader-only work?". `STABLE`.

```sql
SELECT pgbully.is_leader();
```

```sql
-- Run a singleton job only on the leader:
DO $$
BEGIN
  IF pgbully.is_leader() THEN
    PERFORM run_scheduled_maintenance();
  END IF;
END $$;
```

### `pgbully.state() → text`

This node's role: `follower`, `candidate`, `waiting`, or `leader`. `STABLE`.

### `pgbully.term() → bigint`

The current election term/epoch. Increases by one each time a node assumes
leadership. `STABLE`.

### `pgbully.status() → record`

A single-row summary of this node. `STABLE`. Columns:

| Column | Type | Meaning |
|---|---|---|
| `node_id` | `integer` | this node's id |
| `state` | `text` | `follower` / `candidate` / `waiting` / `leader` |
| `is_leader` | `boolean` | whether this node leads |
| `leader_id` | `integer` | current leader (`NULL` if none) |
| `term` | `bigint` | current term |
| `heartbeat_age_ms` | `bigint` | ms since the last heartbeat from the leader |
| `num_nodes` | `integer` | configured cluster size |
| `elections_started` | `bigint` | elections this node initiated |
| `elections_won` | `bigint` | elections this node won |
| `heartbeats_recv` | `bigint` | heartbeats received |
| `coordinators_recv` | `bigint` | coordinator announcements received |

```sql
SELECT * FROM pgbully.status();
```

### `pgbully.peers() → setof record`

One row per configured node, with live reachability. `STABLE`. Columns:

| Column | Type | Meaning |
|---|---|---|
| `node_id` | `integer` | the peer's id |
| `conninfo` | `text` | its libpq connection string |
| `is_self` | `boolean` | is this the local node? |
| `is_leader` | `boolean` | is this the current leader? |
| `reachable` | `boolean` | was the peer alive at the last exchange? |
| `last_seen` | `timestamptz` | time of the last exchange |

An exchange is either direction: a probe this node sent that the peer
answered, or a message the peer sent us. That matters for a follower, which
never probes anyone — it learns the leader is alive from the heartbeats it
receives.

### `pgbully.cluster` (view)

A convenience view over `pgbully.peers()`, ordered by `node_id`:

```sql
SELECT * FROM pgbully.cluster;
```

```
 node_id │ is_self │ is_leader │ reachable │          last_seen          │     conninfo
─────────┼─────────┼───────────┼───────────┼─────────────────────────────┼──────────────────
       1 │ t       │ f         │ t         │ 2026-06-03 22:38:05.79+05    │ host=db1 port=5432
       2 │ f       │ t         │ t         │ 2026-06-03 22:38:03.20+05    │ host=db2 port=5432
       3 │ f       │ f         │ f         │ 2026-06-03 22:37:41.58+05    │ host=db3 port=5432
```

### `pgbully.force_election() → void`

Asks the local worker to start an election immediately, regardless of timers.
Useful for testing or to hasten convergence after a configuration change.
Restricted to superusers by default.

```sql
SELECT pgbully.force_election();
```

---

## RPC handlers (peer-to-peer)

These are invoked by other nodes' workers over libpq. They are documented for
completeness and debugging; **applications and operators should not call them
directly**. They are revoked from `PUBLIC` on install.

| Function | Signature | Returns | Purpose |
|---|---|---|---|
| `pgbully.rpc_ping` | `()` | `integer` | liveness probe → this node's id |
| `pgbully.rpc_election` | `(from_id integer)` | `integer` | a lower node is electing; triggers our own election |
| `pgbully.rpc_coordinator` | `(leader_id integer, term bigint)` | `bigint` | leader announcement → our (possibly updated) term |
| `pgbully.rpc_heartbeat` | `(leader_id integer, term bigint, kv_version bigint)` | `bigint` | leader heartbeat → our (possibly updated) term |
| `pgbully.rpc_kv_apply` | `(key text, value text, deleted boolean, version bigint, term bigint)` | `bigint` | one replicated key/value row → our store version |
| `pgbully.rpc_kv_since` | `(from_version bigint)` | `setof record` | rows applied after `from_version`, for a peer catching up |

The `bigint` returned by `rpc_coordinator` / `rpc_heartbeat` is the receiver's
current term, which lets a leader detect that it has been superseded and step
down.

`rpc_heartbeat` also carries the leader's key/value version. A follower that
is behind pulls what it missed with `rpc_kv_since`; a follower that is *ahead*
— because it led while this leader was down — pushes what the leader is
missing with `rpc_kv_apply`. See
[cluster-api.md](cluster-api.md#the-keyvalue-store).

---

## Permissions

On install, the following are revoked from `PUBLIC` (superuser-only):

- `rpc_ping`, `rpc_election`, `rpc_coordinator`, `rpc_heartbeat`,
  `rpc_kv_apply`, `rpc_kv_since`
- `force_election`
- `kv_put`, `kv_delete`, `kv_compact`, `kv_reset`

The read-only monitoring functions (`node_id`, `leader`, `is_leader`, `state`,
`term`, `status`, `peers`) and the `cluster` view remain available to all
roles. Grant the RPC functions to the dedicated role your peers connect as if
that role is not a superuser:

```sql
GRANT EXECUTE ON FUNCTION pgbully.rpc_ping()                       TO repl;
GRANT EXECUTE ON FUNCTION pgbully.rpc_election(integer)            TO repl;
GRANT EXECUTE ON FUNCTION pgbully.rpc_coordinator(integer, bigint) TO repl;
GRANT EXECUTE ON FUNCTION pgbully.rpc_heartbeat(integer, bigint, bigint) TO repl;
GRANT EXECUTE ON FUNCTION pgbully.rpc_kv_apply(text, text, boolean, bigint, bigint) TO repl;
GRANT EXECUTE ON FUNCTION pgbully.rpc_kv_since(bigint)             TO repl;
```
