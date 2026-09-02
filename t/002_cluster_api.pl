# 002_cluster_api.pl - the standard cluster-management API.
#
# Besides its native functions, pgBully exposes the vendor-neutral interface a
# cluster manager expects from any consensus backend.  This test checks that
# the interface reports the same facts as pgBully's native functions, that it
# keeps reporting them across a failover, and that the log and key/value entry
# points fail with a precise error rather than a missing-function one.
#
# Run with:  make installcheck   (requires the IPC::Run perl module)

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my @nodes;
my @ports;

# Poll until $query on $node returns $want, or give up.
sub wait_for
{
    my ($node, $query, $want, $label) = @_;
    my $got = '';
    foreach (1 .. 100)            # ~20s at 200ms
    {
        $got = $node->safe_psql('postgres', $query);
        return 1 if $got eq "$want";
        usleep(200_000);
    }
    diag("$label: expected '$want', last saw '$got'");
    return 0;
}

# Run $sql expecting it to fail, and hand back the SQLSTATE.
sub sqlstate_of
{
    my ($node, $sql) = @_;
    my ($rc, $out, $err) = $node->psql('postgres',
        "DO \$\$ BEGIN $sql; EXCEPTION WHEN others THEN RAISE NOTICE '%', SQLSTATE; END \$\$");
    return $1 if $err =~ /NOTICE:\s+(\w{5})/;
    return "no-error";
}

# ---------------------------------------------------------------------------
# Two nodes, ids 1 and 2.  Node 2 wins.
# ---------------------------------------------------------------------------

for my $i (0 .. 1)
{
    my $node = PostgreSQL::Test::Cluster->new("pgbc$i");
    $node->init;
    push @nodes, $node;
    push @ports, $node->port;
}

my $membership = join(', ',
    map { ($_ + 1) . ": host=127.0.0.1 port=$ports[$_] dbname=postgres user=$ENV{USER}" }
      (0 .. 1));

for my $i (0 .. 1)
{
    my $id = $i + 1;
    $nodes[$i]->append_conf('postgresql.conf', <<"EOC");
listen_addresses = '127.0.0.1'
shared_preload_libraries = 'pgbully'
pgbully.node_id = $id
pgbully.nodes = '$membership'
pgbully.heartbeat_interval = '300ms'
pgbully.election_timeout = '1500ms'
pgbully.connect_timeout = '1s'
EOC
}

$_->start for @nodes;
$_->safe_psql('postgres', 'CREATE EXTENSION pgbully') for @nodes;

my ($n1, $n2) = @nodes;

# ---------------------------------------------------------------------------
# 1. The extension installs cleanly
# ---------------------------------------------------------------------------
is($n1->safe_psql('postgres',
        "SELECT extversion FROM pg_extension WHERE extname = 'pgbully'"),
    '1.0', 'pgbully installs at 1.0');

like($n1->safe_psql('postgres', 'SELECT pgbully.get_version()'),
    qr/^pgbully-\d/, 'pgbully.get_version() is <name>-<version>');

# ---------------------------------------------------------------------------
# 2. The interface agrees with pgBully's native functions
# ---------------------------------------------------------------------------
ok(wait_for($n2, 'SELECT pgbully.is_leader()', 't', 'initial'),
    'node 2 wins the election');
ok(wait_for($n1, 'SELECT pgbully.get_leader()', '2', 'initial'),
    'node 1 sees node 2 as leader through pgbully.get_leader()');

is($n1->safe_psql('postgres',
        'SELECT pgbully.get_leader() = pgbully.leader()'),
    't', 'pgbully.get_leader() matches pgbully.leader()');
is($n1->safe_psql('postgres',
        'SELECT pgbully.get_term() = pgbully.term()'),
    't', 'pgbully.get_term() matches pgbully.term()');
is($n1->safe_psql('postgres', 'SELECT pgbully.get_worker_state()'),
    'RUNNING', 'worker reports RUNNING');
is($n1->safe_psql('postgres', 'SELECT pgbully.test()'),
    't', 'pgbully.test() passes on a configured node');

# One status row, with the interface's column names and state vocabulary.
my $status = $n2->safe_psql('postgres', <<'SQL');
SELECT node_id || '|' || state || '|' || num_nodes || '|' || (leader_id = 2)
FROM pgbully.get_cluster_status()
SQL
is($status, '2|leader|2|true', 'pgbully.get_cluster_status() row is correct');

# A follower reports "follower", never pgBully's internal "waiting".
like($n1->safe_psql('postgres', 'SELECT state FROM pgbully.get_cluster_status()'),
    qr/^(follower|candidate|leader)$/,
    'state uses the three standard names only');

# ---------------------------------------------------------------------------
# 3. Membership: address and port are recovered from each peer's conninfo
# ---------------------------------------------------------------------------
is($n1->safe_psql('postgres', 'SELECT count(*) FROM pgbully.get_nodes()'),
    '2', 'pgbully.get_nodes() lists both nodes');
is($n1->safe_psql('postgres',
        "SELECT address || ':' || port FROM pgbully.get_nodes() WHERE node_id = 2"),
    "127.0.0.1:$ports[1]", 'address and port come from the conninfo');

is($n1->safe_psql('postgres',
        "SELECT json_array_length(pgbully.get_nodes_from_raft()::json)"),
    '2', 'pgbully.get_nodes_from_raft() returns a JSON array of members');

is($n1->safe_psql('postgres',
        qq{SELECT "status" FROM pgbully.member_list WHERE "memberID" = '2'}),
    'leader', 'pgbully.member_list marks node 2 as leader');

# ---------------------------------------------------------------------------
# 4. Every view is selectable
# ---------------------------------------------------------------------------
foreach my $view (
    qw(pgbully.member_list pgbully.member_list_legacy pgbully.endpoint_status
       pgbully.endpoint_health pgbully.cluster_health pgbully.cluster_info
       pgbully.kv_status pgbully.kv_store_status pgbully.member_details
       pgbully.auth_status pgbully.alarm_list pgbully.cluster_state
       pgbully.worker_status pgbully.cluster_overview pgbully.nodes))
{
    my ($rc, $out, $err) = $n1->psql('postgres', "SELECT * FROM $view");
    is($rc, 0, "$view is selectable");
}

# ---------------------------------------------------------------------------
# 5. Runtime membership changes
# ---------------------------------------------------------------------------
is($n1->safe_psql('postgres', "SELECT pgbully.add_node(7, '10.99.0.7', 5432)"),
    't', 'pgbully.add_node() accepts a new member');
is($n1->safe_psql('postgres',
        'SELECT address || $$:$$ || port FROM pgbully.get_nodes() WHERE node_id = 7'),
    '10.99.0.7:5432', 'the synthesized conninfo round-trips');
is($n1->safe_psql('postgres', 'SELECT pgbully.remove_node(7)'),
    't', 'pgbully.remove_node() drops it again');
is($n1->safe_psql('postgres', 'SELECT count(*) FROM pgbully.get_nodes()'),
    '2', 'membership is back to two nodes');

# The local node may not remove itself.
isnt(sqlstate_of($n1, 'PERFORM pgbully.remove_node(1)'), 'no-error',
    'a node cannot remove itself');

# ---------------------------------------------------------------------------
# 6. The key/value store
# ---------------------------------------------------------------------------

# n2 leads, so n1 must refuse a write and say who to ask.
my ($rc6, $out6, $err6) = $n1->psql('postgres', "SELECT pgbully.kv_put('a', '1')");
isnt($rc6, 0, 'a write on a follower is refused');
like($err6, qr/not the leader/, 'the refusal says this node is not the leader');
like($err6, qr/Node 2 currently leads/, 'and names the leader');

is($n2->safe_psql('postgres', "SELECT pgbully.kv_put('app/mode', 'active')"),
    't', 'the leader accepts a write');
is($n2->safe_psql('postgres', "SELECT pgbully.kv_get('app/mode')"),
    'active', 'the leader reads it back');

# The write is pushed before kv_put() returns, so it is already on n1.
is($n1->safe_psql('postgres', "SELECT pgbully.kv_get('app/mode')"),
    'active', 'the follower has it without waiting');
is($n1->safe_psql('postgres', "SELECT pgbully.kv_exists('app/mode')"),
    't', 'kv_exists() agrees');
is($n1->safe_psql('postgres', "SELECT pgbully.kv_exists('nope')"),
    'f', 'and says so for a key that is not there');

$n2->safe_psql('postgres', "SELECT pgbully.kv_put('app/owner', 'node2')");
is($n1->safe_psql('postgres', 'SELECT pgbully.kv_list_keys()'),
    '["app/mode","app/owner"]', 'kv_list_keys() returns a JSON array, in order');

is($n2->safe_psql('postgres', "SELECT pgbully.kv_delete('app/owner')"),
    't', 'the leader deletes a key');
is($n2->safe_psql('postgres', "SELECT pgbully.kv_delete('app/owner')"),
    'f', 'deleting it again reports it was not there');
is($n1->safe_psql('postgres',
        "SELECT coalesce(pgbully.kv_get('app/owner'), 'gone')"),
    'gone', 'the deletion reached the follower');

# A row from a term older than ours is a deposed leader talking; drop it.
#
# Probe the leader, not a follower, and take the next version rather than
# inventing a large one: a follower left holding a version the leader has not
# reached would correctly decide the leader is behind and push the row back,
# which is the reconciliation working, not a fencing failure.
my $term = $n2->safe_psql('postgres', 'SELECT pgbully.term()');
my $next = $n2->safe_psql('postgres',
    'SELECT last_applied_index + 1 FROM pgbully.kv_get_stats()');

$n2->safe_psql('postgres',
    "SELECT pgbully.rpc_kv_apply('stale', 'ghost', false, $next, 0)");
is($n2->safe_psql('postgres', "SELECT coalesce(pgbully.kv_get('stale'), 'gone')"),
    'gone', 'a write stamped with a stale term is fenced out');

$n2->safe_psql('postgres',
    "SELECT pgbully.rpc_kv_apply('fenced', 'ok', false, $next, $term)");
is($n2->safe_psql('postgres', "SELECT coalesce(pgbully.kv_get('fenced'), 'gone')"),
    'ok', 'the same write in the current term is accepted');

my $stats = $n2->safe_psql('postgres',
    'SELECT active_entries > 0 AND puts > 0 FROM pgbully.kv_get_stats()');
is($stats, 't', 'kv_get_stats() reports real numbers');

isnt($n2->safe_psql('postgres',
        'SELECT last_applied_index FROM pgbully.kv_get_stats()'),
    '0', 'the store has a non-zero version');

# Reset clears every reachable node, not just the leader, and stays cleared:
# a node left holding a higher version would otherwise push the rows back.
$n2->safe_psql('postgres', 'SELECT pgbully.kv_reset()');
ok(wait_for($n2, 'SELECT pgbully.kv_list_keys()', '[]', 'kv_reset leader'),
    'kv_reset() empties the leader, and it stays empty');
ok(wait_for($n1, 'SELECT pgbully.kv_list_keys()', '[]', 'kv_reset follower'),
    'and the follower');

# The log half of the interface is absent, not stubbed.
foreach my $fn (
    'pgbully.log_append(bigint, text)', 'pgbully.log_commit(bigint)',
    'pgbully.log_apply(bigint)', 'pgbully.log_get_stats()',
    'pgbully.replicate_entry(text)', 'pgbully.get_applied_index()')
{
    is($n1->safe_psql('postgres',
            "SELECT to_regprocedure('$fn') IS NULL"),
        't', "$fn is not declared");
}

# ---------------------------------------------------------------------------
# 6b. A node that was down does not take the cluster backwards
# ---------------------------------------------------------------------------
$n2->safe_psql('postgres', "SELECT pgbully.kv_put('k1', 'v1')");

$n2->stop('immediate');
ok(wait_for($n1, 'SELECT pgbully.is_leader()', 't', 'kv failover'),
    'node 1 takes over so it can write');
$n1->safe_psql('postgres', "SELECT pgbully.kv_put('k2', 'written-while-2-was-down')");

$n2->start;
ok(wait_for($n2, 'SELECT pgbully.is_leader()', 't', 'kv reclaim'),
    'node 2 bullies its way back to leader');

# Node 2 won on its id, not on how current its store was. Node 1 must hand
# back what it is missing, or k2 is silently lost.
ok(wait_for($n2, "SELECT coalesce(pgbully.kv_get('k2'), 'LOST')",
        'written-while-2-was-down', 'kv reconcile'),
    'the write made while node 2 was down survives its return');
is($n2->safe_psql('postgres', "SELECT pgbully.kv_get('k1')"),
    'v1', 'and the older key is still there');

# ---------------------------------------------------------------------------
# 7. Failover is visible through the interface
# ---------------------------------------------------------------------------
$n2->stop('immediate');

ok(wait_for($n1, 'SELECT pgbully.is_leader()', 't', 'failover'),
    'node 1 takes over');
is($n1->safe_psql('postgres', 'SELECT pgbully.get_leader()'),
    '1', 'pgbully.get_leader() follows the failover');
is($n1->safe_psql('postgres', 'SELECT state FROM pgbully.get_cluster_status()'),
    'leader', 'cluster status follows the failover');
is($n1->safe_psql('postgres',
        qq{SELECT "status" FROM pgbully.member_list WHERE "memberID" = '2'}),
    'unavailable', 'the stopped node shows as unavailable');

$n1->stop('immediate');

done_testing();
