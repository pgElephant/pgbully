/*-------------------------------------------------------------------------
 *
 * pgbully.c
 *      Module entry point: GUC + shared-memory + background-worker setup.
 *
 * Must be loaded via shared_preload_libraries so that _PG_init() runs in the
 * postmaster, where it can reserve shared memory and register the background
 * worker.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/guc.h"

#include "pgbully.h"

PG_MODULE_MAGIC;

void _PG_init(void);

/* saved hook chain links */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/*
 * shmem_request_hook: reserve our shared memory and a named LWLock.
 */
static void
pgbully_shmem_request_cb(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();

    pgbully_shmem_request();
}

/*
 * shmem_startup_hook: actually create / attach the control block.
 */
static void
pgbully_shmem_startup_cb(void)
{
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    pgbully_shmem_startup();
}

/*
 * Translate a state enum into a stable, lower-case name used by both the
 * monitoring SQL functions and the worker's log messages.
 */
const char *
pgbully_state_name(PgbState s)
{
    switch (s)
    {
        case PGB_FOLLOWER:
            return "follower";
        case PGB_CANDIDATE:
            return "candidate";
        case PGB_WAITING:
            return "waiting";
        case PGB_LEADER:
            return "leader";
    }
    return "unknown";
}

void
_PG_init(void)
{
    /*
     * GUCs can be defined outside shared_preload_libraries, but the worker
     * and shared memory cannot be set up later, so insist on preloading.
     */
    pgbully_define_gucs();

    if (!process_shared_preload_libraries_in_progress)
    {
        ereport(LOG,
                (errmsg("pgbully: not loaded via shared_preload_libraries; "
                        "background election worker is disabled"),
                 errhint("Add \"pgbully\" to shared_preload_libraries and "
                         "restart the server.")));
        return;
    }

    /* Reserve shared memory via the request hook (PG15+). */
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = pgbully_shmem_request_cb;

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = pgbully_shmem_startup_cb;

    /* Register the per-node election worker. */
    pgbully_register_worker();

    ereport(LOG, (errmsg("pgbully: initialized")));
}
