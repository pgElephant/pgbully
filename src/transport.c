/*-------------------------------------------------------------------------
 *
 * transport.c
 *      Peer-to-peer message transport for pgBully, over libpq.
 *
 * Each message is delivered as a SQL function call on the receiving node:
 *
 *      ELECTION     -> SELECT pgbully.rpc_election($1)      ($1 = sender id)
 *      COORDINATOR  -> SELECT pgbully.rpc_coordinator($1,$2)(id, term)
 *      HEARTBEAT    -> SELECT pgbully.rpc_heartbeat($1,$2)  (id, term)
 *      PING         -> SELECT pgbully.rpc_ping()
 *
 * Connections are cached per node id for the lifetime of the worker and
 * lazily re-established after a failure.  All calls are bounded by
 * connect_timeout (during connection) and statement_timeout (during the
 * call) so a dead or hung peer cannot wedge the worker.
 *
 * This file is only ever exercised from the single background worker, so the
 * connection cache needs no locking.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "miscadmin.h"

#include "pgbully.h"

typedef struct PgbConn
{
    int32       node_id;
    PGconn     *conn;
} PgbConn;

static PgbConn conn_cache[PGBULLY_MAX_NODES];
static int     conn_cache_n = 0;

/*
 * Drop all cached connections.  Called on configuration reload so that
 * changed conninfo strings take effect.
 */
void
pgbully_transport_reset(void)
{
    int         i;

    for (i = 0; i < conn_cache_n; i++)
    {
        if (conn_cache[i].conn)
            PQfinish(conn_cache[i].conn);
        conn_cache[i].conn = NULL;
    }
    conn_cache_n = 0;
}

static PgbConn *
cache_slot(int32 node_id)
{
    int         i;

    for (i = 0; i < conn_cache_n; i++)
    {
        if (conn_cache[i].node_id == node_id)
            return &conn_cache[i];
    }
    if (conn_cache_n >= PGBULLY_MAX_NODES)
        return NULL;

    conn_cache[conn_cache_n].node_id = node_id;
    conn_cache[conn_cache_n].conn = NULL;
    return &conn_cache[conn_cache_n++];
}

/*
 * Return a live connection to the peer, (re)connecting if necessary.
 * Returns NULL on failure.
 */
static PGconn *
get_conn(const PgbPeer *peer)
{
    PgbConn    *slot = cache_slot(peer->node_id);
    StringInfoData conninfo;
    int         connect_secs;

    if (slot == NULL)
        return NULL;

    if (slot->conn != NULL)
    {
        if (PQstatus(slot->conn) == CONNECTION_OK)
            return slot->conn;
        /* stale -- try to recover the socket before giving up */
        PQreset(slot->conn);
        if (PQstatus(slot->conn) == CONNECTION_OK)
            return slot->conn;
        PQfinish(slot->conn);
        slot->conn = NULL;
    }

    /*
     * libpq's connect_timeout is in whole seconds with an effective floor of
     * 2s, so round our millisecond GUC up to at least 1.  TCP keepalives let
     * us notice a peer that vanishes mid-connection.
     */
    connect_secs = pgbully_connect_timeout_ms / 1000;
    if (connect_secs < 1)
        connect_secs = 1;

    initStringInfo(&conninfo);
    appendStringInfoString(&conninfo, peer->conninfo);
    appendStringInfo(&conninfo,
                     " connect_timeout=%d keepalives=1 keepalives_idle=2"
                     " keepalives_interval=2 keepalives_count=2"
                     " application_name=pgbully",
                     connect_secs);

    slot->conn = PQconnectdb(conninfo.data);
    pfree(conninfo.data);

    if (PQstatus(slot->conn) != CONNECTION_OK)
    {
        PQfinish(slot->conn);
        slot->conn = NULL;
        return NULL;
    }

    /* Bound every subsequent call so a hung server cannot block us. */
    {
        char        cmd[64];
        PGresult   *res;

        snprintf(cmd, sizeof(cmd), "SET statement_timeout = %d",
                 pgbully_connect_timeout_ms);
        res = PQexec(slot->conn, cmd);
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
        {
            /* A connection that cannot even bound its own queries is useless. */
            PQclear(res);
            PQfinish(slot->conn);
            slot->conn = NULL;
            return NULL;
        }
        PQclear(res);
    }

    return slot->conn;
}

/*
 * Run a one-row, one-column query against the peer and hand the textual
 * result (or NULL) to the caller.  Returns a PgbRpcResult.
 */
static PgbRpcResult
call_scalar(const PgbPeer *peer, const char *sql,
            int nparams, const char *const *params,
            char *result_buf, size_t result_buflen)
{
    PGconn     *conn = get_conn(peer);
    PGresult   *res;
    PgbRpcResult rc = PGB_RPC_OK;

    if (conn == NULL)
        return PGB_RPC_UNREACHABLE;

    res = PQexecParams(conn, sql, nparams, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PQclear(res);
        /* connection may be broken; force a fresh one next time */
        {
            PgbConn *slot = cache_slot(peer->node_id);

            if (slot && slot->conn)
            {
                PQfinish(slot->conn);
                slot->conn = NULL;
            }
        }
        return PGB_RPC_UNREACHABLE;
    }

    if (result_buf && result_buflen > 0)
    {
        result_buf[0] = '\0';
        if (PQntuples(res) >= 1 && !PQgetisnull(res, 0, 0))
            strlcpy(result_buf, PQgetvalue(res, 0, 0), result_buflen);
    }

    PQclear(res);
    return rc;
}

PgbRpcResult
pgbully_send_ping(const PgbPeer *peer, int32 *peer_id_out)
{
    char        buf[32];
    PgbRpcResult rc;

    rc = call_scalar(peer, "SELECT pgbully.rpc_ping()", 0, NULL,
                     buf, sizeof(buf));
    if (rc == PGB_RPC_OK && peer_id_out)
        *peer_id_out = (int32) strtol(buf, NULL, 10);
    return rc;
}

PgbRpcResult
pgbully_send_election(const PgbPeer *peer)
{
    char        from[16];
    const char *params[1];

    snprintf(from, sizeof(from), "%d", PgbCtl->my_node_id);
    params[0] = from;

    return call_scalar(peer, "SELECT pgbully.rpc_election($1)",
                       1, params, NULL, 0);
}

PgbRpcResult
pgbully_send_coordinator(const PgbPeer *peer, int32 leader_id, int64 term)
{
    char        a[16];
    char        b[24];
    const char *params[2];

    snprintf(a, sizeof(a), "%d", leader_id);
    snprintf(b, sizeof(b), INT64_FORMAT, term);
    params[0] = a;
    params[1] = b;

    return call_scalar(peer, "SELECT pgbully.rpc_coordinator($1, $2)",
                       2, params, NULL, 0);
}

PgbRpcResult
pgbully_send_heartbeat(const PgbPeer *peer, int32 leader_id, int64 term,
                       int64 kv_version, int64 *peer_term_out)
{
    char        a[16];
    char        b[24];
    char        c[24];
    char        buf[24];
    const char *params[3];
    PgbRpcResult rc;

    snprintf(a, sizeof(a), "%d", leader_id);
    snprintf(b, sizeof(b), INT64_FORMAT, term);
    snprintf(c, sizeof(c), INT64_FORMAT, kv_version);
    params[0] = a;
    params[1] = b;
    params[2] = c;

    rc = call_scalar(peer, "SELECT pgbully.rpc_heartbeat($1, $2, $3)",
                     3, params, buf, sizeof(buf));
    if (rc == PGB_RPC_OK && peer_term_out)
        *peer_term_out = (int64) strtoll(buf, NULL, 10);
    return rc;
}

/* -------------------------------------------------------------------------
 * Key/value replication
 *
 * These two run in an ordinary backend rather than in the worker.  That is
 * safe because the connection cache above is file-static and therefore
 * process-local: a backend that pushes a write simply builds up its own
 * connections, quite separate from the worker's.
 * ------------------------------------------------------------------------- */

/*
 * Push one applied key/value row to a peer.  The peer decides whether to
 * accept it; a stale term or an older version is silently ignored there.
 */
PgbRpcResult
pgbully_send_kv_apply(const PgbPeer *peer, const char *key, const char *value,
                      bool deleted, int64 version, int64 term)
{
    char        v[24];
    char        t[24];
    const char *params[5];

    snprintf(v, sizeof(v), INT64_FORMAT, version);
    snprintf(t, sizeof(t), INT64_FORMAT, term);

    params[0] = key;
    params[1] = value;               /* NULL for a tombstone */
    params[2] = deleted ? "t" : "f";
    params[3] = v;
    params[4] = t;

    return call_scalar(peer,
                       "SELECT pgbully.rpc_kv_apply($1, $2, $3, $4, $5)",
                       5, params, NULL, 0);
}

/*
 * Fetch every row a peer has applied after from_version.  On success
 * *result_out holds a PGresult of (key, value, deleted, version) that the
 * caller must PQclear().
 */
PgbRpcResult
pgbully_fetch_kv_since(const PgbPeer *peer, int64 from_version,
                       void **result_out)
{
    PGconn     *conn;
    PGresult   *res;
    char        v[24];
    const char *params[1];

    *result_out = NULL;

    conn = get_conn(peer);
    if (conn == NULL)
        return PGB_RPC_UNREACHABLE;

    snprintf(v, sizeof(v), INT64_FORMAT, from_version);
    params[0] = v;

    res = PQexecParams(conn,
                       "SELECT key, value, deleted, version"
                       " FROM pgbully.rpc_kv_since($1) ORDER BY version",
                       1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PgbConn    *slot;

        PQclear(res);
        slot = cache_slot(peer->node_id);
        if (slot && slot->conn)
        {
            PQfinish(slot->conn);
            slot->conn = NULL;
        }
        return PGB_RPC_UNREACHABLE;
    }

    *result_out = res;
    return PGB_RPC_OK;
}
