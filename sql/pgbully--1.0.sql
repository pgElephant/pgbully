/* pgbully--1.0.sql */

-- Guard against loading the bare script.
\echo Use "CREATE EXTENSION pgbully" to load this file. \quit

CREATE SCHEMA IF NOT EXISTS pgbully;

-- ---------------------------------------------------------------------------
-- Inbound RPC handlers (called by peer nodes over libpq).
-- These are intentionally not exposed beyond the cluster operator / peers.
-- ---------------------------------------------------------------------------
CREATE FUNCTION pgbully.rpc_ping()
    RETURNS integer
    AS 'MODULE_PATHNAME', 'pgbully_rpc_ping'
    LANGUAGE C;

CREATE FUNCTION pgbully.rpc_election(from_id integer)
    RETURNS integer
    AS 'MODULE_PATHNAME', 'pgbully_rpc_election'
    LANGUAGE C;

CREATE FUNCTION pgbully.rpc_coordinator(leader_id integer, term bigint)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pgbully_rpc_coordinator'
    LANGUAGE C;

CREATE FUNCTION pgbully.rpc_heartbeat(leader_id integer, term bigint)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pgbully_rpc_heartbeat'
    LANGUAGE C;

-- ---------------------------------------------------------------------------
-- Public monitoring / control surface.
-- ---------------------------------------------------------------------------
CREATE FUNCTION pgbully.node_id()
    RETURNS integer
    AS 'MODULE_PATHNAME', 'pgbully_my_node_id'
    LANGUAGE C STABLE;

CREATE FUNCTION pgbully.leader()
    RETURNS integer
    AS 'MODULE_PATHNAME', 'pgbully_leader'
    LANGUAGE C STABLE;

CREATE FUNCTION pgbully.is_leader()
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_is_leader'
    LANGUAGE C STABLE;

CREATE FUNCTION pgbully.term()
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pgbully_term'
    LANGUAGE C STABLE;

CREATE FUNCTION pgbully.state()
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_state'
    LANGUAGE C STABLE;

CREATE FUNCTION pgbully.force_election()
    RETURNS void
    AS 'MODULE_PATHNAME', 'pgbully_force_election'
    LANGUAGE C;

CREATE FUNCTION pgbully.status(
    OUT node_id            integer,
    OUT state              text,
    OUT is_leader          boolean,
    OUT leader_id          integer,
    OUT term               bigint,
    OUT heartbeat_age_ms   bigint,
    OUT num_nodes          integer,
    OUT elections_started  bigint,
    OUT elections_won      bigint,
    OUT heartbeats_recv    bigint,
    OUT coordinators_recv  bigint)
    RETURNS record
    AS 'MODULE_PATHNAME', 'pgbully_status'
    LANGUAGE C STABLE;

CREATE FUNCTION pgbully.peers(
    OUT node_id    integer,
    OUT conninfo   text,
    OUT is_self    boolean,
    OUT is_leader  boolean,
    OUT reachable  boolean,
    OUT last_seen  timestamptz)
    RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'pgbully_peers'
    LANGUAGE C STABLE;

-- Convenience view over the cluster membership.
CREATE VIEW pgbully.cluster AS
    SELECT node_id, is_self, is_leader, reachable, last_seen, conninfo
    FROM pgbully.peers()
    ORDER BY node_id;

-- ---------------------------------------------------------------------------
-- Lock down the RPC handlers: only superusers (and peers connecting as the
-- replication/cluster role) should invoke them directly.  Monitoring
-- functions remain available to all.
-- ---------------------------------------------------------------------------
REVOKE ALL ON FUNCTION pgbully.rpc_ping() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.rpc_election(integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.rpc_coordinator(integer, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.rpc_heartbeat(integer, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.force_election() FROM PUBLIC;

-- ===========================================================================
-- Cluster-manager API
--
-- Everything above is pgBully's native surface, shaped around the Bully
-- algorithm.  What follows is the vendor-neutral function and view set that
-- cluster managers and control planes expect from any PostgreSQL consensus
-- backend, so an application written against that interface runs on pgBully
-- without changes:
--
--      pgbully.get_cluster_status()   one row of cluster state
--      pgbully.get_nodes()            membership as (id, address, port)
--      pgbully.member_list            etcd-style member listing
--      pgbully.cluster_state          shared-memory state as a view
--
-- The interface also covers log replication and a key/value store, neither of
-- which the Bully algorithm provides.  Those functions are still declared,
-- with their full signatures, so that a caller which probes for them or
-- prepares a statement against them gets a precise, catchable error
-- (ERRCODE_FEATURE_NOT_SUPPORTED) instead of "function does not exist".
-- Every one of them raises.
-- ===========================================================================

-- ===========================================================================
-- Replication tables
--
-- A backend with a replicated log populates these from its apply loop.
-- pgBully has no apply loop, so they exist for schema compatibility and
-- stay empty.
-- ===========================================================================

CREATE TABLE IF NOT EXISTS pgbully.kv (
    key         TEXT PRIMARY KEY,
    value       TEXT NOT NULL,
    version     BIGINT NOT NULL DEFAULT 1,
    created_at  TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    updated_at  TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS pgbully.applied_entries (
    raft_index  BIGINT PRIMARY KEY,
    raft_term   BIGINT NOT NULL,
    entry_type  INTEGER NOT NULL,
    applied_at  TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS pgbully.log_index_mapping (
    raft_index      BIGINT PRIMARY KEY,
    operation_type  TEXT NOT NULL,
    target_table    TEXT,
    operation_data  JSONB,
    applied_at      TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

-- ===========================================================================
-- Core cluster functions
-- ===========================================================================

-- Initialize pgbully from its GUCs (re-reads pgbully.nodes, wakes the worker).
CREATE FUNCTION pgbully.init()
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_compat_init'
    LANGUAGE C;

/*
 * Add a node to the cluster.  The address and port are turned into a libpq
 * conninfo, reusing the dbname/user already in effect for the cluster.
 *
 * The change is applied to shared memory at once but is not persistent:
 * pgbully.nodes remains the source of truth and a configuration reload
 * restores it.  Update the GUC as well to make the change permanent.
 */
CREATE FUNCTION pgbully.add_node(node_id integer, address text, port integer)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_add_node'
    LANGUAGE C;

-- Remove a node from the cluster (runtime only; see pgbully.add_node).
CREATE FUNCTION pgbully.remove_node(node_id integer)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_remove_node'
    LANGUAGE C;

-- Cluster status, one row.
CREATE FUNCTION pgbully.get_cluster_status()
    RETURNS TABLE(
        node_id             integer,
        current_term        bigint,
        leader_id           bigint,
        state               text,
        num_nodes           integer,
        messages_processed  bigint,
        heartbeats_sent     bigint,
        elections_triggered bigint)
    AS 'MODULE_PATHNAME', 'pgbully_get_cluster_status_table'
    LANGUAGE C;

-- Membership, one row per node.  address and port come from each peer's
-- conninfo, with libpq's own defaults applied.
CREATE FUNCTION pgbully.get_nodes()
    RETURNS TABLE(
        node_id    integer,
        address    text,
        port       integer,
        is_leader  boolean)
    AS 'MODULE_PATHNAME', 'pgbully_get_nodes_table'
    LANGUAGE C;

-- Membership as JSON, in the shape the etcd-compatible views consume.
-- pgbully.get_nodes_json() is the preferred spelling; the _from_raft() name
-- is kept because the interface specifies it, and is the same function.
CREATE FUNCTION pgbully.get_nodes_from_raft()
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_get_nodes_json'
    LANGUAGE C;

CREATE FUNCTION pgbully.get_nodes_json()
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_get_nodes_json'
    LANGUAGE C;

-- Background worker state: RUNNING or STOPPED.
CREATE FUNCTION pgbully.get_worker_state()
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_get_worker_state'
    LANGUAGE C STABLE;

-- Version string, e.g. "pgbully-1.1".
CREATE FUNCTION pgbully.get_version()
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_get_version'
    LANGUAGE C IMMUTABLE;

-- Self-check: shared memory attached, node id set, node listed in the cluster.
CREATE FUNCTION pgbully.test()
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_compat_test'
    LANGUAGE C;

-- Verbose worker logging on/off.
CREATE FUNCTION pgbully.set_debug(enabled boolean)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_set_debug'
    LANGUAGE C;

-- Current leader id; 0 when none is known, as the interface specifies.
-- The native pgbully.leader() returns NULL in that case instead.
CREATE FUNCTION pgbully.get_leader()
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pgbully_get_leader_id'
    LANGUAGE C STABLE;

-- Current term.
CREATE FUNCTION pgbully.get_term()
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pgbully_term'
    LANGUAGE C STABLE;

-- Pending configuration commands.  pgBully applies membership changes
-- synchronously, so this is always empty.
CREATE FUNCTION pgbully.get_queue_status()
    RETURNS TABLE(
        cmd_position  integer,
        command_type  integer,
        node_id       integer,
        address       text,
        port          integer,
        log_data      text)
    AS 'MODULE_PATHNAME', 'pgbully_get_queue_status'
    LANGUAGE C;

-- ===========================================================================
-- Log replication -- not implemented by the Bully algorithm
--
-- Declared with their full signatures; every one raises
-- ERRCODE_FEATURE_NOT_SUPPORTED naming the function that was called.
-- ===========================================================================

CREATE FUNCTION pgbully.log_append(term bigint, data text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.log_commit(index bigint)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.log_apply(index bigint)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.log_get_entry(index bigint)
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.log_get_stats()
    RETURNS TABLE(
        log_size      bigint,
        last_index    bigint,
        commit_index  bigint,
        last_applied  bigint,
        replicated    bigint,
        committed     bigint,
        applied       bigint,
        errors        bigint)
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.log_get_replication_status()
    RETURNS TABLE(
        log_size      bigint,
        last_index    bigint,
        commit_index  bigint,
        last_applied  bigint,
        replicated    bigint,
        committed     bigint,
        applied       bigint,
        errors        bigint)
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.log_sync_with_leader()
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.replicate_entry(entry_data text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.get_applied_index()
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.record_applied_index(index bigint)
    RETURNS void
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

-- ===========================================================================
-- Key/value store -- not implemented by the Bully algorithm
-- ===========================================================================

CREATE FUNCTION pgbully.kv_put(key text, value text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_get(key text)
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_delete(key text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_exists(key text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_list_keys()
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_get_stats()
    RETURNS TABLE(
        num_entries         integer,
        total_operations    bigint,
        last_applied_index  bigint,
        puts                bigint,
        deletes             bigint,
        gets                bigint,
        active_entries      integer,
        deleted_entries     integer)
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_compact()
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_reset()
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_put_local(key text, value text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_delete_local(key text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_unsupported'
    LANGUAGE C;

-- ===========================================================================
-- etcd-compatible views
-- ===========================================================================

-- 'etcdctl member list'
CREATE VIEW pgbully.member_list AS
SELECT
    (node->>'id')::text AS "memberID",
    (node->>'address')::text AS "peerURLs",
    (node->>'address')::text AS "clientURLs",
    CASE
        WHEN (node->>'active')::boolean = false THEN 'unavailable'
        WHEN (node->>'id')::int = pgbully.get_leader() THEN 'leader'
        ELSE 'follower'
    END AS "status"
FROM json_array_elements(pgbully.get_nodes_from_raft()::json) AS node
ORDER BY (node->>'id')::int;

-- Same, built from the table-returning function instead of the JSON one.
CREATE VIEW pgbully.member_list_legacy AS
SELECT
    node_id::text AS "memberID",
    address || ':' || port::text AS "peerURLs",
    address || ':' || port::text AS "clientURLs",
    CASE WHEN is_leader THEN 'leader' ELSE 'follower' END AS "status"
FROM pgbully.get_nodes()
ORDER BY node_id;

-- 'etcdctl endpoint status'
CREATE VIEW pgbully.endpoint_status AS
SELECT
    n.address || ':' || n.port::text AS "endpoint",
    CASE WHEN n.is_leader THEN 'true' ELSE 'false' END AS "isLeader",
    c.current_term::text AS "raftTerm",
    c.messages_processed::text AS "raftIndex",
    c.messages_processed::text AS "raftAppliedIndex",
    '0' AS "dbSize",
    c.leader_id::text AS "leader",
    c.elections_triggered::text AS "leaderIndex",
    current_setting('pgbully.heartbeat_interval', true) AS "uptime"
FROM pgbully.get_nodes() n,
     LATERAL (SELECT * FROM pgbully.get_cluster_status()) c
ORDER BY n.node_id;

-- 'etcdctl endpoint health'
CREATE VIEW pgbully.endpoint_health AS
SELECT
    address || ':' || port::text AS "endpoint",
    CASE WHEN is_leader THEN 'true' ELSE 'false' END AS "health",
    CASE WHEN is_leader THEN 'true' ELSE 'false' END AS "took"
FROM pgbully.get_nodes()
ORDER BY node_id;

-- 'etcdctl cluster-health'
CREATE VIEW pgbully.cluster_health AS
SELECT
    'cluster is healthy' AS "member",
    CASE WHEN is_leader THEN 'true' ELSE 'false' END AS "isLeader",
    'false' AS "isLearner",
    'true' AS "health"
FROM pgbully.get_nodes();

-- etcd-style cluster information
CREATE VIEW pgbully.cluster_info AS
SELECT
    c.current_term::text AS "clusterID",
    c.num_nodes::text AS "memberCount",
    c.leader_id::text AS "leader",
    c.state AS "raftTerm",
    c.messages_processed::text AS "raftIndex",
    c.heartbeats_sent::text AS "raftAppliedIndex"
FROM pgbully.get_cluster_status() c;

CREATE VIEW pgbully.kv_status AS
SELECT
    'pgbully' AS "key",
    'PostgreSQL Bully Leader Election Extension' AS "value",
    '0' AS "version",
    '0' AS "create_revision",
    '0' AS "mod_revision"
WHERE pgbully.is_leader();

-- 'etcdctl endpoint hashkv'
CREATE VIEW pgbully.endpoint_hashkv AS
SELECT
    address || ':' || port::text AS "endpoint",
    '0' AS "hash",
    '0' AS "hash_revision"
FROM pgbully.get_nodes()
ORDER BY node_id;

CREATE VIEW pgbully.watch_status AS
SELECT
    'pgbully.watch' AS "watcher_id",
    'true' AS "is_active",
    '0' AS "watch_count",
    '0' AS "watch_pending"
WHERE pgbully.is_leader();

CREATE VIEW pgbully.member_details AS
SELECT
    node_id::text AS "ID",
    'pgbully' AS "Name",
    address || ':' || port::text AS "PeerURLs",
    address || ':' || port::text AS "ClientURLs",
    CASE WHEN is_leader THEN 'true' ELSE 'false' END AS "IsLeader",
    'false' AS "IsLearner"
FROM pgbully.get_nodes()
ORDER BY node_id;

CREATE VIEW pgbully.auth_status AS
SELECT
    'true' AS "enabled",
    'false' AS "revision"
WHERE pgbully.is_leader();

CREATE VIEW pgbully.alarm_list AS
SELECT
    'NONE' AS "alarm",
    node_id::text AS "memberID"
FROM pgbully.get_nodes();

CREATE VIEW pgbully.snapshot_status AS
SELECT
    '0' AS "hash",
    '0' AS "revision",
    '0' AS "total_key",
    '0' AS "total_size",
    'true' AS "version"
WHERE pgbully.is_leader();

-- ===========================================================================
-- Unqualified views
-- ===========================================================================

-- Core cluster state, read from shared memory.
CREATE VIEW pgbully.cluster_state AS
SELECT
    c.leader_id,
    c.current_term,
    c.state,
    c.num_nodes,
    c.messages_processed,
    c.heartbeats_sent,
    c.elections_triggered,
    n.node_id,
    n.address,
    n.port,
    pgbully.get_worker_state() AS worker_status,
    (pgbully.get_worker_state() = 'RUNNING') AS initialized,
    (c.leader_id = n.node_id) AS is_leader
FROM pgbully.get_cluster_status() c
JOIN pgbully.get_nodes() n ON n.node_id = c.node_id;

CREATE VIEW pgbully.worker_status AS
SELECT
    pgbully.get_worker_state() AS worker_state,
    (pgbully.get_worker_state() = 'RUNNING') AS is_running;

CREATE VIEW pgbully.cluster_overview AS
SELECT
    c.worker_status AS worker_state,
    c.leader_id,
    c.current_term,
    c.state,
    c.is_leader,
    c.num_nodes,
    c.messages_processed
FROM pgbully.cluster_state c;

CREATE VIEW pgbully.nodes AS
SELECT
    n.node_id,
    n.address,
    n.port,
    n.is_leader,
    c.current_term,
    c.state
FROM pgbully.get_nodes() n,
     LATERAL (SELECT * FROM pgbully.get_cluster_status()) c;

-- No replicated log: reported as an empty log rather than an error, so that
-- a monitoring query over this view keeps working.
CREATE VIEW pgbully.log_status AS
SELECT
    c.node_id,
    0::bigint AS log_size,
    0::bigint AS last_index,
    0::bigint AS commit_index,
    0::bigint AS last_applied,
    false AS replicated,
    false AS committed,
    false AS applied,
    0::bigint AS errors
FROM pgbully.get_cluster_status() c;

-- Selecting from this view raises: pgBully has no key/value store.
CREATE VIEW pgbully.kv_store_status AS
SELECT
    s.num_entries,
    s.active_entries,
    s.deleted_entries,
    s.total_operations,
    s.puts,
    s.gets,
    s.deletes,
    s.last_applied_index,
    CASE
        WHEN s.num_entries = 0 THEN 'EMPTY'
        WHEN s.deleted_entries > s.active_entries THEN 'NEEDS_COMPACTION'
        ELSE 'HEALTHY'
    END AS status
FROM pgbully.kv_get_stats() s;

-- ===========================================================================
-- Privileges
--
-- PostgreSQL grants EXECUTE to PUBLIC by default, which would leave cluster
-- membership rewritable by any logged-in role.  Revoke everything, then hand
-- back only the read-only introspection functions.
-- ===========================================================================

REVOKE ALL ON FUNCTION pgbully.init() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.add_node(integer, text, integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.remove_node(integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.set_debug(boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.test() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.log_append(bigint, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.log_commit(bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.log_apply(bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.log_sync_with_leader() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.replicate_entry(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.record_applied_index(bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_put(text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_delete(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_put_local(text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_delete_local(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_compact() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_reset() FROM PUBLIC;

REVOKE ALL ON FUNCTION pgbully.get_cluster_status() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_nodes() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_nodes_from_raft() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_nodes_json() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_worker_state() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_version() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_leader() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_term() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_queue_status() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_applied_index() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.log_get_entry(bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.log_get_stats() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.log_get_replication_status() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_get(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_exists(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_list_keys() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_get_stats() FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pgbully.get_cluster_status() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_nodes() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_nodes_from_raft() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_nodes_json() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_worker_state() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_version() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_leader() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_term() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_queue_status() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_applied_index() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.log_get_entry(bigint) TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.log_get_stats() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.log_get_replication_status() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.kv_get(text) TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.kv_exists(text) TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.kv_list_keys() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.kv_get_stats() TO PUBLIC;

GRANT SELECT ON pgbully.member_list TO PUBLIC;
GRANT SELECT ON pgbully.member_list_legacy TO PUBLIC;
GRANT SELECT ON pgbully.endpoint_status TO PUBLIC;
GRANT SELECT ON pgbully.endpoint_health TO PUBLIC;
GRANT SELECT ON pgbully.cluster_health TO PUBLIC;
GRANT SELECT ON pgbully.cluster_info TO PUBLIC;
GRANT SELECT ON pgbully.kv_status TO PUBLIC;
GRANT SELECT ON pgbully.endpoint_hashkv TO PUBLIC;
GRANT SELECT ON pgbully.watch_status TO PUBLIC;
GRANT SELECT ON pgbully.member_details TO PUBLIC;
GRANT SELECT ON pgbully.auth_status TO PUBLIC;
GRANT SELECT ON pgbully.alarm_list TO PUBLIC;
GRANT SELECT ON pgbully.snapshot_status TO PUBLIC;

GRANT SELECT ON pgbully.cluster_state TO PUBLIC;
GRANT SELECT ON pgbully.worker_status TO PUBLIC;
GRANT SELECT ON pgbully.cluster_overview TO PUBLIC;
GRANT SELECT ON pgbully.nodes TO PUBLIC;
GRANT SELECT ON pgbully.log_status TO PUBLIC;
GRANT SELECT ON pgbully.kv_store_status TO PUBLIC;

GRANT SELECT ON pgbully.kv TO PUBLIC;
GRANT SELECT ON pgbully.applied_entries TO PUBLIC;
GRANT SELECT ON pgbully.log_index_mapping TO PUBLIC;
