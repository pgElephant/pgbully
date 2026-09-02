/*-------------------------------------------------------------------------
 *
 * compat.h
 *      Cross-version compatibility shims for PostgreSQL 15/16/17/18.
 *
 * pgBully targets a narrow band of modern PostgreSQL releases that share a
 * very similar extension API.  This header collects the handful of points
 * where the API drifted so the rest of the code can stay version-agnostic.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGBULLY_COMPAT_H
#define PGBULLY_COMPAT_H

#include "postgres.h"

#if PG_VERSION_NUM < 150000
#error "pgBully requires PostgreSQL 15 or newer"
#endif

/*
 * shmem_request_hook was introduced in PG15, which is our floor, so every
 * supported version uses RequestAddinShmemSpace()/RequestNamedLWLockTranche()
 * from inside the hook.  Nothing to shim there.
 *
 * InitMaterializedSRF() exists from PG15 onward, so set-returning functions
 * use it uniformly.
 */

/*
 * Wait-event definitions (PG_WAIT_EXTENSION and friends) were split out of
 * pgstat.h into their own header in PG16.  Include whichever one this server
 * has so callers can just include compat.h.
 */
#if PG_VERSION_NUM >= 160000
#include "utils/wait_event.h"
#else
#include "pgstat.h"
#endif

/*
 * TimestampDifferenceMilliseconds() has existed since PG13, but its return
 * type widened to a 64-bit value in PG15+.  We always treat it as a signed
 * 64-bit count of milliseconds, which is correct for all supported versions.
 */
static inline int64
pgb_ts_diff_ms(TimestampTz start, TimestampTz stop)
{
    /* TimestampTz is microseconds since the epoch. */
    return (int64) (stop - start) / 1000;
}

#endif                          /* PGBULLY_COMPAT_H */
