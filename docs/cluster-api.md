# Cluster-manager API

pgBully's native functions live in the `pgbully` schema and are shaped around
the Bully algorithm — see [api.md](api.md). Alongside them, pgBully implements
the vendor-neutral interface that cluster managers and control planes expect
from any PostgreSQL consensus backend, so an application written against that
interface runs on pgBully without changes.

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
| `pgbully.endpoint_hashkv` | `etcdctl endpoint hashkv` |
| `pgbully.cluster_health` | `etcdctl cluster-health` |
| `pgbully.cluster_info` | cluster identity and size |
| `pgbully.member_details` | per-member detail |
| `pgbully.alarm_list` | alarm listing (always `NONE`) |
| `pgbully.auth_status` | authentication status |
| `pgbully.kv_status` | key/value summary |
| `pgbully.watch_status` | watcher summary |
| `pgbully.snapshot_status` | snapshot summary |

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
| `pgbully.log_status` | Always an empty log — see below |
| `pgbully.kv_store_status` | Raises on select — see below |

Two names need a word of warning:

- **`pgbully.nodes`** is both this view and the membership setting. They live
  in different namespaces and never collide in practice —
  `SELECT * FROM pgbully.nodes` reads the view, `SHOW pgbully.nodes` reads the
  setting — but they are not the same object.
- **`pgbully.kv_status`** is the etcd-style summary above.
  **`pgbully.kv_store_status`** is the key/value store's own health view, which
  raises because pgBully has no key/value store.

---

## Log replication and the key/value store

The Bully algorithm elects a leader. It does not replicate a log and it has no
key/value store, so pgBully cannot implement those parts of the interface.

They are still **declared**, with their full signatures, so that a caller
which probes for them or prepares a statement against them gets a precise,
catchable failure instead of `function does not exist`:

```
ERROR:  pgbully.kv_put() is not supported by pgbully
DETAIL: pgBully elects a leader; it has no replicated log and no key/value store.
HINT:   Applications that need replicated state must use a backend that
        provides it.
```

The SQLSTATE is `0A000` (`feature_not_supported`), so an application can
detect the missing capability without parsing text:

```sql
DO $$
BEGIN
    PERFORM pgbully.kv_put('k', 'v');
EXCEPTION WHEN feature_not_supported THEN
    RAISE NOTICE 'this backend elects a leader only';
END $$;
```

Functions that raise:

- **Log** — `pgbully.log_append`, `pgbully.log_commit`, `pgbully.log_apply`,
  `pgbully.log_get_entry`, `pgbully.log_get_stats`,
  `pgbully.log_get_replication_status`, `pgbully.log_sync_with_leader`,
  `pgbully.replicate_entry`, `pgbully.get_applied_index`,
  `pgbully.record_applied_index`
- **Key/value** — `pgbully.kv_put`, `pgbully.kv_get`, `pgbully.kv_delete`,
  `pgbully.kv_exists`, `pgbully.kv_list_keys`, `pgbully.kv_get_stats`,
  `pgbully.kv_compact`, `pgbully.kv_reset`, `pgbully.kv_put_local`,
  `pgbully.kv_delete_local`

`pgbully.log_status` is the one exception: it reports an empty log rather than
raising, so a monitoring query that selects from it keeps working.

The tables `pgbully.kv`, `pgbully.applied_entries` and
`pgbully.log_index_mapping` exist for schema compatibility and stay empty.

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
