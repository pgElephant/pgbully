#!/usr/bin/env bash
#
# integration.sh - self-contained 3-node leader-election test for pgBully.
#
# Unlike the TAP suite (t/001_bully.pl) this script has no Perl dependencies;
# it spins up three throwaway PostgreSQL instances, installs pgBully, and
# asserts the Bully invariants directly with psql.  Intended for quick local
# verification and CI environments without IPC::Run.
#
# Usage:
#   PG_CONFIG=/path/to/pg_config ./test/integration.sh
#
set -euo pipefail

PG_CONFIG="${PG_CONFIG:-pg_config}"
BINDIR="$($PG_CONFIG --bindir)"
export PATH="$BINDIR:$PATH"

BASE="${TMPDIR:-/tmp}/pgbully_it.$$"
USER_NAME="$(id -un)"
BASE_PORT="${BASE_PORT:-55431}"
PASS=0
FAIL=0

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '  \033[1;32mok\033[0m   %s\n' "$*"; PASS=$((PASS+1)); }
nok()  { printf '  \033[1;31mFAIL\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }

cleanup() {
    for n in 1 2 3; do
        pg_ctl -D "$BASE/n$n" -m immediate stop >/dev/null 2>&1 || true
    done
    rm -rf "$BASE"
}
trap cleanup EXIT

port_of() { echo $((BASE_PORT + $1 - 1)); }

q() {  # q <node> <sql>
    psql -h 127.0.0.1 -p "$(port_of "$1")" -U "$USER_NAME" -d postgres -tAc "$2" 2>/dev/null
}

# Wait until node $1 reports leader == $2 (timeout ~20s).
wait_leader() {
    local node="$1" want="$2" got=''
    for _ in $(seq 1 100); do
        got="$(q "$node" "SELECT coalesce(pgbully.leader()::text,'none')" || true)"
        [ "$got" = "$want" ] && return 0
        sleep 0.2
    done
    echo "    (node $node leader='$got', wanted '$want')"
    return 1
}

assert_leader() {  # assert_leader <node> <want> <msg>
    if wait_leader "$1" "$2"; then ok "$3"; else nok "$3"; fi
}

# ---------------------------------------------------------------------------
log "Building membership across nodes 1,2,3"
mkdir -p "$BASE"
NODES=""
for n in 1 2 3; do
    [ -n "$NODES" ] && NODES="$NODES, "
    NODES="$NODES$n: host=127.0.0.1 port=$(port_of "$n") dbname=postgres user=$USER_NAME"
done

log "initdb + configure"
for n in 1 2 3; do
    dd="$BASE/n$n"
    initdb -D "$dd" -U "$USER_NAME" -A trust >/dev/null 2>&1
    cat >> "$dd/postgresql.conf" <<EOF
port = $(port_of "$n")
listen_addresses = '127.0.0.1'
unix_socket_directories = '$BASE'
shared_preload_libraries = 'pgbully'
pgbully.node_id = $n
pgbully.nodes = '$NODES'
pgbully.heartbeat_interval = '300ms'
pgbully.election_timeout = '1500ms'
pgbully.connect_timeout = '1s'
EOF
    pg_ctl -D "$dd" -l "$dd/server.log" -w start >/dev/null 2>&1
    psql -h 127.0.0.1 -p "$(port_of "$n")" -U "$USER_NAME" -d postgres \
         -c "CREATE EXTENSION pgbully;" >/dev/null 2>&1
done

log "Test 1: highest id (3) wins the initial election"
assert_leader 1 3 "node 1 sees leader 3"
assert_leader 2 3 "node 2 sees leader 3"
assert_leader 3 3 "node 3 sees leader 3"
[ "$(q 3 'SELECT pgbully.is_leader()')" = "t" ] && ok "node 3 is_leader()" || nok "node 3 is_leader()"

log "Test 2: failover when the leader dies"
pg_ctl -D "$BASE/n3" -m immediate stop >/dev/null 2>&1
assert_leader 2 2 "node 2 takes over"
assert_leader 1 2 "node 1 follows node 2"

log "Test 3: the bully returns (node 3 reclaims)"
pg_ctl -D "$BASE/n3" -l "$BASE/n3/server.log" -w start >/dev/null 2>&1
psql -h 127.0.0.1 -p "$(port_of 3)" -U "$USER_NAME" -d postgres \
     -c "CREATE EXTENSION IF NOT EXISTS pgbully;" >/dev/null 2>&1
assert_leader 3 3 "node 3 reclaims leadership"
assert_leader 1 3 "node 1 follows reclaimed node 3"

log "Test 4: exactly one leader"
leaders=0
for n in 1 2 3; do
    [ "$(q "$n" 'SELECT pgbully.is_leader()')" = "t" ] && leaders=$((leaders+1))
done
[ "$leaders" = "1" ] && ok "exactly one leader" || nok "expected 1 leader, found $leaders"

echo
log "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
