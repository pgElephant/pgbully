# pgBully - distributed leader election (Bully algorithm) for PostgreSQL
# Supports PostgreSQL 15, 16, 17, 18.

EXTENSION   = pgbully
EXTVERSION  = 1.0
PGFILEDESC  = "pgBully - distributed leader election (Bully algorithm) for PostgreSQL"

MODULE_big  = pgbully
OBJS        = src/pgbully.o src/config.o src/shmem.o src/transport.o src/worker.o src/rpc.o \
              src/cluster_api.o

DATA        = sql/pgbully--1.0.sql

# Multi-node leader election cannot be exercised by single-backend pg_regress,
# so the regression suite is TAP-based (PostgreSQL::Test::Cluster).
TAP_TESTS   = 1

PG_CONFIG  ?= pg_config

# libpq is used by the background worker to talk to peer nodes.
PG_CPPFLAGS += -I$(shell $(PG_CONFIG) --includedir) -Isrc
SHLIB_LINK  += -L$(shell $(PG_CONFIG) --libdir) -lpq

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Refuse to build on anything older than PostgreSQL 15.
PG_VERSION_NUM := $(shell $(PG_CONFIG) --version | sed 's/[^0-9]*\([0-9]*\).*/\1/')
ifeq ($(shell test $(PG_VERSION_NUM) -lt 15; echo $$?),0)
$(error pgBully requires PostgreSQL 15 or newer (found $(PG_VERSION_NUM)))
endif
