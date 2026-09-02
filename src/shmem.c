/*-------------------------------------------------------------------------
 *
 * shmem.c
 *      Allocation and lifecycle of the pgBully shared control block.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

#include "pgbully.h"

PgbShared *PgbCtl = NULL;

#define PGBULLY_LWLOCK_TRANCHE  "pgbully"

Size
pgbully_shmem_size(void)
{
    return MAXALIGN(sizeof(PgbShared));
}

/*
 * Called from the shmem_request_hook (postmaster, PG15+).
 */
void
pgbully_shmem_request(void)
{
    RequestAddinShmemSpace(pgbully_shmem_size());
    RequestNamedLWLockTranche(PGBULLY_LWLOCK_TRANCHE, 1);
}

/*
 * Called from the shmem_startup_hook.  Initializes the control block the
 * first time and simply attaches on subsequent (re)attaches.
 */
void
pgbully_shmem_startup(void)
{
    bool        found;

    PgbCtl = NULL;

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    PgbCtl = (PgbShared *) ShmemInitStruct("pgbully",
                                           pgbully_shmem_size(),
                                           &found);

    if (!found)
    {
        memset(PgbCtl, 0, sizeof(PgbShared));
        PgbCtl->magic = PGBULLY_SHMEM_MAGIC;
        PgbCtl->lock = &(GetNamedLWLockTranche(PGBULLY_LWLOCK_TRANCHE))->lock;
        PgbCtl->state = PGB_FOLLOWER;
        PgbCtl->term = 0;
        PgbCtl->leader_id = PGBULLY_NO_LEADER;
        PgbCtl->last_heartbeat = 0;
        PgbCtl->worker_latch = NULL;
        PgbCtl->worker_pid = 0;
    }

    LWLockRelease(AddinShmemInitLock);
}

/*
 * Attach to the already-initialized control block from a normal backend
 * (e.g. when an RPC or monitoring SQL function is invoked).  Returns false
 * if the extension was not preloaded.
 */
bool
pgbully_attach_shmem(void)
{
    bool        found;

    if (PgbCtl != NULL && PgbCtl->magic == PGBULLY_SHMEM_MAGIC)
        return true;

    LWLockAcquire(AddinShmemInitLock, LW_SHARED);
    PgbCtl = (PgbShared *) ShmemInitStruct("pgbully",
                                           pgbully_shmem_size(),
                                           &found);
    LWLockRelease(AddinShmemInitLock);

    if (!found || PgbCtl == NULL || PgbCtl->magic != PGBULLY_SHMEM_MAGIC)
    {
        PgbCtl = NULL;
        return false;
    }
    return true;
}
