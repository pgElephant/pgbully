# Cluster-manager API

pgBully's native functions live in the `pgbully` schema and are shaped around
the Bully algorithm — see [api.md](api.md). Alongside them, pgBully implements
the vendor-neutral interface that cluster managers and control planes expect
from any PostgreSQL consensus backend, so an application that only needs to
know who leads runs on pgBully without changes.

That interface also covers a replicated key/value store, and pgBully provides
one — with weaker guarantees than a quorum system can offer, spelled out in
[the key/value store](#the-keyvalue-store) below. It does not provide the
log-replication functions: without a quorum there is nothing meaningful to put
behind them.

Everything lives in the `pgbully` schema — functions, views and tables alike.
The schema *is* the namespace, so nothing carries a redundant `pgbully_`
prefix: it is `pgbully.get_cluster_status()`, never
`pgbully.pgbully_get_cluster_status()`.

---

## Cluster lifecycle

### `pgbully.init() → boolean`

Re-reads `pgbully.nodes` into shared memory and wakes the election worker.
Returns `false` with a warning — rather than raising — if the extension is not
in `shared_preload_libraries` or `pgbully.node_id` is unset.

Calling it is optional: the worker loads the membership itself at startup and
on every `SIGHUP`.

### `pgbully.add_node(node_id integer, address text, port integer) → boolean`

Adds a member, or re-points an existing one. The address and port are turned
into a libpq conninfo, carrying over the `dbname` and `user` already in use by
the cluster:

```sql
SELECT pgbully.add_node(4, '10.0.0.4', 5432);
-- conninfo becomes: host='10.0.0.4' port=5432 dbname='app' user='repl'
```

An election is triggered afterwards, since a new higher-id member should take
over immediately.

> **The change is not persistent.** `pgbully.nodes` remains the source of
> truth, and the next configuration reload restores it. Add the node to the
> GUC as well to make it permanent. A `NOTICE` says so on every call.

Raises on a non-positive id, an empty address, a port outside 1–65535, or a
cluster already holding the maximum of 64 nodes.

### `pgbully.remove_node(node_id integer) → boolean`

Drops a member from shared memory. Returns `false` with a warning if the node
was not a member; raises if you try to remove the local node. If the removed
node was the leader, an election starts at once. Not persistent, as above.

---

## Introspection

### `pgbully.get_cluster_status() → table`

One row describing this node's view of the cluster.

| Column | Type | Notes |
|---|---|---|
| `node_id` | `integer` | This node's id |
| `current_term` | `bigint` | Current term/epoch |
| `leader_id` | `bigint` | Current leader; `0` when none is known |
| `state` | `text` | `leader`, `follower` or `candidate` |
| `num_nodes` | `integer` | Configured members |
| `messages_processed` | `bigint` | Bully messages handled, both directions |
| `heartbeats_sent` | `bigint` | Heartbeats this node has sent as leader |
| `elections_triggered` | `bigint` | Elections this node has started |

`state` reports only the three standard names. pgBully's internal `waiting`
state — ELECTION sent to higher-id peers, no winner announced yet — is an
election in progress and reports as `candidate`. Use `pgbully.state()` when
you want the exact internal state.

### `pgbully.get_nodes() → table`

One row per configured member.

| Column | Type | Notes |
|---|---|---|
| `node_id` | `integer` | Member id |
| `address` | `text` | Host from the member's conninfo |
| `port` | `integer` | Port from the member's conninfo |
| `is_leader` | `boolean` | Is this member the current leader? |

`address` and `port` are recovered from each member's conninfo through libpq,
so an entry that omits the port still reports the port the worker actually
dials. `hostaddr` is used when no `host` is given. If a conninfo cannot be
parsed at all, `address` reports it verbatim and `port` is `0`.

### `pgbully.get_nodes_json() → text`

The membership as a JSON array:

```json
[{"id":1,"address":"10.0.0.1:5432","port":5432,"active":true},
 {"id":2,"address":"10.0.0.2:5432","port":5432,"active":false}]
```

`active` is reachability as last observed by the worker; the local node is
always active. This is what the etcd-style views below read.

`pgbully.get_nodes_from_raft()` is the same function under the name the
interface specifies, and is kept so that code written against the interface
works unmodified.

### `pgbully.is_leader() → boolean`

Whether this node currently holds leadership. Same as `pgbully.is_leader()`.

### `pgbully.get_leader() → bigint`

The current leader's id, or **`0`** when none is known. Note the difference
from `pgbully.leader()`, which returns `NULL` in that case.

### `pgbully.get_term() → bigint`

The current term. Same as `pgbully.term()`.

### `pgbully.get_worker_state() → text`

`RUNNING` when the election worker is up and `pgbully.enabled` is on,
`STOPPED` otherwise.

### `pgbully.get_version() → text`

`pgbully-<version>`, for example `pgbully-1.0`.

### `pgbully.test() → boolean`

A self-check: shared memory is attached, `pgbully.node_id` is set, and that id
appears in `pgbully.nodes`. Emits a warning explaining which check failed.

### `pgbully.set_debug(enabled boolean) → boolean`

Turns per-iteration state logging in the worker on or off, cluster-wide within
this instance. Returns the setting now in effect. Output looks like:

```
LOG:  pgbully: state=follower term=7 leader=3 heartbeat_age=214ms election=no
```

### `pgbully.get_queue_status() → table`

Always empty. The interface allows a backend to queue configuration commands
for an asynchronous applier; pgBully applies membership changes synchronously.
Columns are `cmd_position`, `command_type`, `node_id`, `address`, `port`,
`log_data`.

---

## Views

### etcd-style

| View | Mirrors |
|---|---|
| `pgbully.member_list` | `etcdctl member list` |
| `pgbully.member_list_legacy` | the same, built from `pgbully.get_nodes()` |
| `pgbully.endpoint_status` | `etcdctl endpoint status` |
| `pgbully.endpoint_health` | `etcdctl endpoint health` |
| `pgbully.cluster_health` | `etcdctl cluster-health` |
| `pgbully.cluster_info` | cluster identity and size |
| `pgbully.member_details` | per-member detail |
| `pgbully.alarm_list` | alarm listing (always `NONE`) |
| `pgbully.auth_status` | authentication status |
| `pgbully.kv_status` | key/value summary |

`pgbully.member_list` is the useful one day to day:

```sql
SELECT * FROM pgbully.member_list;

 memberID |    peerURLs     |   clientURLs    |   status
----------+-----------------+-----------------+-------------
 1        | 10.0.0.1:5432   | 10.0.0.1:5432   | follower
 2        | 10.0.0.2:5432   | 10.0.0.2:5432   | unavailable
 3        | 10.0.0.3:5432   | 10.0.0.3:5432   | leader
```

### Cluster state

| View | Contents |
|---|---|
| `pgbully.cluster_state` | Cluster status joined with this node's address and worker state |
| `pgbully.cluster_overview` | The same, trimmed to the columns a dashboard needs |
| `pgbully.worker_status` | `worker_state`, `is_running` |
| `pgbully.nodes` | `pgbully.get_nodes()` plus the current term and state |
| `pgbully.kv_store_status` | key/value store health: entry counts, operation counters, version |

One name needs a word of warning: **`pgbully.nodes`** is both this view and
the membership setting. They live in different namespaces and never collide in
practice — `SELECT * FROM pgbully.nodes` reads the view, `SHOW pgbully.nodes`
reads the setting — but they are not the same object.

---

## The key/value store

A small replicated store for cluster-scoped configuration — which node is
active, where a job should run — that any node can read locally without a
round trip.

### What it guarantees, and what it does not

pgBully elects a leader. It has no quorum and no replicated log, so the store
cannot offer what a Raft-backed one does. It offers this instead:

| | |
|---|---|
| **Writes** | Accepted **only on the leader**. A write on a follower raises, naming the leader, rather than being forwarded — so you always know which node took your data. |
| **Ordering** | Every write takes the next version from a counter the leader owns and carries the term it was written under. A peer applies a row only if the term is at least its own and the version beats what it holds, so a deposed leader's late writes are dropped on arrival. |
| **Replication** | The writing backend pushes the row to every reachable peer before returning. Best effort: an unreachable peer does not fail the write. |
| **Catch-up** | The leader advertises its version on every heartbeat. A follower that is behind pulls what it missed before answering a read; a follower that is *ahead* — because it led while the current leader was down — pushes what the leader is missing. A node that was down heals in both directions. |
| **Reads** | Local, and so may be stale by up to one heartbeat. |

> **This is not a quorum store.** A write is durable on the leader and best
> effort everywhere else, so a network partition can strand recent writes on
> the side that loses the election. Do not put anything in it that you cannot
> afford to lose.

### `pgbully.kv_put(key text, value text) → boolean`

Store a key. Leader only.

```sql
SELECT pgbully.kv_put('app/mode', 'active');
```

On a follower:

```
ERROR:  cannot store a key: this node is not the leader
DETAIL: Node 2 currently leads the cluster.
HINT:   Key/value writes must be issued on the leader.
```

A `NULL` value is stored as `NULL`, which is distinct from the key being
absent — use `pgbully.kv_exists()` to tell them apart.

### `pgbully.kv_get(key text) → text`

Read a key from this node, or `NULL` if it is not there. Works on any node,
and catches up from the leader first if this node has fallen behind.

### `pgbully.kv_delete(key text) → boolean`

Delete a key. Leader only. Returns `false` if the key was not there.

The row becomes a tombstone rather than disappearing, because a row that is
gone cannot be replicated — a follower would otherwise never learn of the
deletion. `pgbully.kv_compact()` clears them.

### `pgbully.kv_exists(key text) → boolean`

Whether the key is present on this node. Catches up first, like `kv_get()`.

### `pgbully.kv_list_keys() → text`

The live keys as a JSON array, in key order:

```sql
SELECT pgbully.kv_list_keys();
 ["app/mode","app/owner"]
```

### `pgbully.kv_get_stats() → record`

| Column | Type | Notes |
|---|---|---|
| `num_entries` | `integer` | Rows in the table, tombstones included |
| `total_operations` | `bigint` | Puts + deletes + gets since this node started |
| `last_applied_index` | `bigint` | The store's version on this node |
| `puts` / `deletes` / `gets` | `bigint` | Per-operation counters since startup |
| `active_entries` | `integer` | Live keys |
| `deleted_entries` | `integer` | Tombstones awaiting compaction |

The counters live in shared memory and reset when PostgreSQL restarts;
`last_applied_index` does not, because it is re-seeded from the table.

### `pgbully.kv_compact() → boolean`

Drop tombstones, which is what keeps a delete-heavy store from growing
forever. Leader only. Only rows below the current version go, so the version
never moves backwards.

A `NOTICE` names the version compacted to: a peer that has not caught up past
it will never learn of those deletions. Compact when the cluster is healthy,
not while a node is down.

### `pgbully.kv_reset() → boolean`

Empty the store on this node and every reachable peer. Leader only,
superuser-only, and exactly as blunt as it sounds.

A reset is the leader declaring a new baseline, so it takes precedence: every
peer adopts the leader's version rather than keeping its own. Without that, a
node holding a higher version would decide the leader was behind and push the
emptied rows straight back.

### `pgbully.kv_sync() → bigint`

Pull from the leader now instead of waiting for the next read, and return the
version this node ends up at. Useful right after a node rejoins, and in tests
that cannot wait a heartbeat.

### The table

The rows live in `pgbully.kv`, which you can read directly:

```sql
SELECT key, value, version, term, updated_at
FROM   pgbully.kv
WHERE  NOT deleted
ORDER  BY key;
```

It is marked as extension configuration, so `pg_dump` includes its contents.
Write to it only through the functions above — a direct `INSERT` is not
replicated and does not move the version counter.

---

## What is not here

The interface also covers log replication: `log_append()`, `log_commit()`,
`log_apply()`, `log_get_stats()`, `replicate_entry()` and applied-index
tracking. pgBully does not implement them, and does not declare them either.
Calling one is a plain `undefined_function` (SQLSTATE `42883`).

There is nothing to put behind them. A replicated log is only worth the name
if a quorum agrees on its order, and the Bully algorithm establishes which
node leads, not what a majority has durably accepted. The key/value store
above is what can honestly be built on leader election alone; an application
that needs a real log needs a consensus backend.

---

## Permissions

Read-only introspection is granted to `PUBLIC`. Everything else is revoked and
left to superusers: the functions that change cluster state (`pgbully.init`,
`pgbully.add_node`, `pgbully.remove_node`), the two that change how the node
behaves or reports (`pgbully.set_debug`, `pgbully.test`), and the log and
key/value writers. Grant explicitly if your control plane connects as a
non-superuser:

```sql
GRANT EXECUTE ON FUNCTION pgbully.add_node(integer, text, integer) TO ctrl;
GRANT EXECUTE ON FUNCTION pgbully.remove_node(integer)             TO ctrl;
```

---

## Differences from the native API, at a glance

| | Native | Cluster-manager API |
|---|---|---|
| No leader | `pgbully.leader()` → `NULL` | `pgbully.get_leader()` → `0` |
| Internal `waiting` state | `pgbully.state()` → `waiting` | `state` → `candidate` |
| Membership shape | `id:conninfo` | `(node_id, address, port)` |
| Membership changes | edit `pgbully.nodes`, reload | `pgbully.add_node()`, non-persistent |
