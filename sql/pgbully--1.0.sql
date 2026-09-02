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
