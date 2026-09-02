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

CREATE FUNCTION pgbully.rpc_heartbeat(leader_id integer, term bigint,
                                      kv_version bigint DEFAULT 0)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pgbully_rpc_heartbeat'
    LANGUAGE C;

CREATE FUNCTION pgbully.rpc_kv_apply(key text, value text, deleted boolean,
                                     version bigint, term bigint)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pgbully_rpc_kv_apply'
    LANGUAGE C;

CREATE FUNCTION pgbully.rpc_kv_since(from_version bigint,
    OUT key      text,
    OUT value    text,
    OUT deleted  boolean,
    OUT version  bigint)
    RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'pgbully_rpc_kv_since'
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
REVOKE ALL ON FUNCTION pgbully.rpc_heartbeat(integer, bigint, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.rpc_kv_apply(text, text, boolean, bigint, bigint) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.rpc_kv_since(bigint) FROM PUBLIC;
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
-- The interface also covers log replication and a key/value store.  pgBully
-- elects a leader and nothing else, so it does not carry those: there is no
-- pgbully.kv_put(), no pgbully.log_append(), and no tables behind them.  A
-- caller that needs replicated state wants a different backend, and finding
-- out at CREATE EXTENSION time beats finding out from a function that exists
-- only to refuse.
-- ===========================================================================

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


-- ===========================================================================
-- Key/value store
--
-- Leader-owned and best-effort replicated: writes are accepted only on the
-- leader, stamped with the current term and the next version, and pushed to
-- every reachable peer before the call returns.  A follower that fell behind
-- pulls what it missed before answering a read, so a node that was down heals
-- itself once it is back.
--
-- This is not a quorum store.  A write is durable on the leader and best
-- effort everywhere else, so a partition can strand recent writes on the side
-- that loses the election.  It is meant for cluster-scoped configuration that
-- every node should be able to read locally -- not for data you cannot lose.
-- ===========================================================================

CREATE TABLE pgbully.kv (
    key         text PRIMARY KEY,
    value       text,
    deleted     boolean     NOT NULL DEFAULT false,
    version     bigint      NOT NULL,
    term        bigint      NOT NULL DEFAULT 0,
    updated_at  timestamptz NOT NULL DEFAULT now()
);

-- Catch-up reads the table in version order.
CREATE INDEX kv_version_idx ON pgbully.kv (version);

SELECT pg_catalog.pg_extension_config_dump('pgbully.kv', '');

COMMENT ON TABLE pgbully.kv IS
    'Replicated key/value rows. Tombstones (deleted = true) are kept so that '
    'deletions replicate; pgbully.kv_compact() clears them.';

-- Store a key.  Leader only; raises on a follower, naming the leader.
CREATE FUNCTION pgbully.kv_put(key text, value text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_kv_put'
    LANGUAGE C;

-- Read a key from this node.  NULL if absent.
CREATE FUNCTION pgbully.kv_get(key text)
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_kv_get'
    LANGUAGE C;

-- Delete a key.  Leader only.  Returns false if it was not there.
CREATE FUNCTION pgbully.kv_delete(key text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_kv_delete'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_exists(key text)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_kv_exists'
    LANGUAGE C;

-- The live keys, as a JSON array.
CREATE FUNCTION pgbully.kv_list_keys()
    RETURNS text
    AS 'MODULE_PATHNAME', 'pgbully_kv_list_keys'
    LANGUAGE C;

CREATE FUNCTION pgbully.kv_get_stats(
    OUT num_entries         integer,
    OUT total_operations    bigint,
    OUT last_applied_index  bigint,
    OUT puts                bigint,
    OUT deletes             bigint,
    OUT gets                bigint,
    OUT active_entries      integer,
    OUT deleted_entries     integer)
    RETURNS record
    AS 'MODULE_PATHNAME', 'pgbully_kv_get_stats'
    LANGUAGE C;

-- Drop tombstones. Leader only.
CREATE FUNCTION pgbully.kv_compact()
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_kv_compact'
    LANGUAGE C;

-- Empty the store on every reachable node. Leader only.
CREATE FUNCTION pgbully.kv_reset()
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pgbully_kv_reset'
    LANGUAGE C;

-- Pull from the leader now instead of waiting for the next read.  Returns the
-- version this node ends up at.
CREATE FUNCTION pgbully.kv_sync()
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pgbully_kv_sync'
    LANGUAGE C;

-- Store health, in the shape the cluster-manager interface expects.
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

-- etcd-style one-row summary.
CREATE VIEW pgbully.kv_status AS
SELECT
    'pgbully' AS "key",
    s.active_entries::text AS "value",
    s.last_applied_index::text AS "version",
    '0' AS "create_revision",
    s.last_applied_index::text AS "mod_revision"
FROM pgbully.kv_get_stats() s;

-- ---------------------------------------------------------------------------
-- Privileges: reads for everyone, writes for superusers, replication handlers
-- for the peers only.
-- ---------------------------------------------------------------------------
REVOKE ALL ON FUNCTION pgbully.kv_put(text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_delete(text) FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_compact() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.kv_reset() FROM PUBLIC;

GRANT SELECT ON pgbully.kv TO PUBLIC;
GRANT SELECT ON pgbully.kv_store_status TO PUBLIC;
GRANT SELECT ON pgbully.kv_status TO PUBLIC;

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

REVOKE ALL ON FUNCTION pgbully.get_cluster_status() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_nodes() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_nodes_from_raft() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_nodes_json() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_worker_state() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_version() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_leader() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_term() FROM PUBLIC;
REVOKE ALL ON FUNCTION pgbully.get_queue_status() FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pgbully.get_cluster_status() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_nodes() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_nodes_from_raft() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_nodes_json() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_worker_state() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_version() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_leader() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_term() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pgbully.get_queue_status() TO PUBLIC;

GRANT SELECT ON pgbully.member_list TO PUBLIC;
GRANT SELECT ON pgbully.member_list_legacy TO PUBLIC;
GRANT SELECT ON pgbully.endpoint_status TO PUBLIC;
GRANT SELECT ON pgbully.endpoint_health TO PUBLIC;
GRANT SELECT ON pgbully.cluster_health TO PUBLIC;
GRANT SELECT ON pgbully.cluster_info TO PUBLIC;
GRANT SELECT ON pgbully.member_details TO PUBLIC;
GRANT SELECT ON pgbully.auth_status TO PUBLIC;
GRANT SELECT ON pgbully.alarm_list TO PUBLIC;

GRANT SELECT ON pgbully.cluster_state TO PUBLIC;
GRANT SELECT ON pgbully.worker_status TO PUBLIC;
GRANT SELECT ON pgbully.cluster_overview TO PUBLIC;
GRANT SELECT ON pgbully.nodes TO PUBLIC;

