/*-------------------------------------------------------------------------
 *
 * config.c
 *      GUC definitions and parsing of the pgbully.nodes membership list.
 *
 * The cluster membership is described by a single GUC, pgbully.nodes, of the
 * form:
 *
 *      <id>:<conninfo> [, <id>:<conninfo> ]...
 *
 * e.g.  pgbully.nodes = '1: host=10.0.0.1 port=5432 dbname=app,
 *                        2: host=10.0.0.2 port=5432 dbname=app,
 *                        3: host=10.0.0.3 port=5432 dbname=app'
 *
 * The entry whose id equals pgbully.node_id describes this node and is used
 * only to validate membership; the worker never calls itself.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/guc.h"
#include "storage/lwlock.h"

#include "pgbully.h"

int     pgbully_node_id = 0;
char   *pgbully_nodes_raw = NULL;
int     pgbully_heartbeat_interval_ms = 1000;
int     pgbully_election_timeout_ms = 5000;
int     pgbully_connect_timeout_ms = 2000;
bool    pgbully_enabled = true;

/*
 * Trim leading and trailing ASCII whitespace in place, returning a pointer
 * to the first non-space character.  Modifies the buffer.
 */
static char *
trim(char *s)
{
    char       *end;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    if (*s == '\0')
        return s;

    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';

    return s;
}

/*
 * Parse the pgbully.nodes string into an array of PgbPeer entries.
 *
 * Returns the number of peers parsed, or -1 on error (with *errmsg set to a
 * palloc'd message in the current memory context).  A return of 0 with no
 * error simply means an empty configuration.
 */
int
pgbully_parse_nodes(const char *raw, PgbPeer *out, int max, char **errmsg)
{
    char       *work;
    char       *saveptr = NULL;
    char       *tok;
    int         n = 0;

    if (errmsg)
        *errmsg = NULL;

    if (raw == NULL || raw[0] == '\0')
        return 0;

    work = pstrdup(raw);

    for (tok = strtok_r(work, ",", &saveptr);
         tok != NULL;
         tok = strtok_r(NULL, ",", &saveptr))
    {
        char       *entry = trim(tok);
        char       *colon;
        char       *idstr;
        char       *conn;
        long        id;
        char       *parse_end;
        int         i;

        if (*entry == '\0')
            continue;           /* tolerate trailing/extra commas */

        colon = strchr(entry, ':');
        if (colon == NULL)
        {
            if (errmsg)
                *errmsg = psprintf("missing ':' in node entry \"%s\"", entry);
            pfree(work);
            return -1;
        }

        *colon = '\0';
        idstr = trim(entry);
        conn = trim(colon + 1);

        errno = 0;
        id = strtol(idstr, &parse_end, 10);
        if (*parse_end != '\0' || idstr == parse_end || id <= 0 || id > PG_INT32_MAX)
        {
            if (errmsg)
                *errmsg = psprintf("invalid node id \"%s\"", idstr);
            pfree(work);
            return -1;
        }

        if (*conn == '\0')
        {
            if (errmsg)
                *errmsg = psprintf("empty conninfo for node %ld", id);
            pfree(work);
            return -1;
        }

        if (strlen(conn) >= PGBULLY_CONNINFO_LEN)
        {
            if (errmsg)
                *errmsg = psprintf("conninfo for node %ld is too long (max %d)",
                                   id, PGBULLY_CONNINFO_LEN - 1);
            pfree(work);
            return -1;
        }

        /* reject duplicate ids */
        for (i = 0; i < n; i++)
        {
            if (out[i].node_id == (int32) id)
            {
                if (errmsg)
                    *errmsg = psprintf("duplicate node id %ld", id);
                pfree(work);
                return -1;
            }
        }

        if (n >= max)
        {
            if (errmsg)
                *errmsg = psprintf("too many nodes (max %d)", max);
            pfree(work);
            return -1;
        }

        memset(&out[n], 0, sizeof(PgbPeer));
        out[n].node_id = (int32) id;
        strlcpy(out[n].conninfo, conn, PGBULLY_CONNINFO_LEN);
        out[n].in_use = true;
        out[n].reachable = false;
        out[n].last_seen = 0;
        n++;
    }

    pfree(work);
    return n;
}

/*
 * GUC check hook for pgbully.nodes: validate without mutating shared state.
 */
static bool
check_nodes(char **newval, void **extra, GucSource source)
{
    PgbPeer    *tmp;
    char       *err = NULL;
    int         rc;

    if (*newval == NULL || (*newval)[0] == '\0')
        return true;

    tmp = (PgbPeer *) palloc0(sizeof(PgbPeer) * PGBULLY_MAX_NODES);
    rc = pgbully_parse_nodes(*newval, tmp, PGBULLY_MAX_NODES, &err);
    pfree(tmp);

    if (rc < 0)
    {
        GUC_check_errdetail("%s", err ? err : "invalid pgbully.nodes value");
        return false;
    }
    return true;
}

/*
 * Re-parse pgbully.nodes and publish the result into shared memory.  Called
 * by the worker at startup and after a configuration reload.
 */
void
pgbully_load_peers(void)
{
    PgbPeer    *parsed;
    char       *err = NULL;
    int         n;

    if (PgbCtl == NULL)
        return;

    parsed = (PgbPeer *) palloc0(sizeof(PgbPeer) * PGBULLY_MAX_NODES);
    n = pgbully_parse_nodes(pgbully_nodes_raw, parsed, PGBULLY_MAX_NODES, &err);

    if (n < 0)
    {
        ereport(WARNING,
                (errmsg("pgbully: ignoring invalid pgbully.nodes: %s",
                        err ? err : "parse error")));
        pfree(parsed);
        return;
    }

    LWLockAcquire(PgbCtl->lock, LW_EXCLUSIVE);
    memset(PgbCtl->peers, 0, sizeof(PgbCtl->peers));
    memcpy(PgbCtl->peers, parsed, sizeof(PgbPeer) * n);
    PgbCtl->npeers = n;
    PgbCtl->my_node_id = pgbully_node_id;
    LWLockRelease(PgbCtl->lock);

    pfree(parsed);

    ereport(LOG, (errmsg("pgbully: loaded %d cluster node(s), this node is %d",
                         n, pgbully_node_id)));
}

void
pgbully_define_gucs(void)
{
    DefineCustomIntVariable("pgbully.node_id",
                            "Unique identifier of this node in the cluster.",
                            "Higher ids win elections. Must be > 0 and match an "
                            "entry in pgbully.nodes.",
                            &pgbully_node_id,
                            0, 0, PG_INT32_MAX,
                            PGC_POSTMASTER, 0,
                            NULL, NULL, NULL);

    DefineCustomStringVariable("pgbully.nodes",
                               "Cluster membership as \"id:conninfo, ...\".",
                               "Each entry maps a node id to a libpq conninfo "
                               "used to reach that node.",
                               &pgbully_nodes_raw,
                               "",
                               PGC_SIGHUP, 0,
                               check_nodes, NULL, NULL);

    DefineCustomIntVariable("pgbully.heartbeat_interval",
                            "Milliseconds between leader heartbeats.",
                            NULL,
                            &pgbully_heartbeat_interval_ms,
                            1000, 50, 600000,
                            PGC_SIGHUP, GUC_UNIT_MS,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("pgbully.election_timeout",
                            "Milliseconds without a heartbeat before a follower "
                            "starts an election.",
                            NULL,
                            &pgbully_election_timeout_ms,
                            5000, 100, 3600000,
                            PGC_SIGHUP, GUC_UNIT_MS,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("pgbully.connect_timeout",
                            "Milliseconds to wait when contacting a peer.",
                            NULL,
                            &pgbully_connect_timeout_ms,
                            2000, 100, 60000,
                            PGC_SIGHUP, GUC_UNIT_MS,
                            NULL, NULL, NULL);

    DefineCustomBoolVariable("pgbully.enabled",
                             "Whether the election worker actively participates.",
                             "When off, the worker idles and holds no leadership.",
                             &pgbully_enabled,
                             true,
                             PGC_SIGHUP, 0,
                             NULL, NULL, NULL);

    MarkGUCPrefixReserved("pgbully");
}
