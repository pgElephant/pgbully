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
       pgbully.kv_status pgbully.endpoint_hashkv pgbully.watch_status
       pgbully.member_details pgbully.auth_status pgbully.alarm_list
       pgbully.snapshot_status pgbully.cluster_state pgbully.worker_status
       pgbully.cluster_overview pgbully.nodes pgbully.log_status))
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
# 6. Log and key/value: present, and refusing clearly
# ---------------------------------------------------------------------------
foreach my $call (
    "PERFORM pgbully.log_append(1, 'x')",
    "PERFORM pgbully.log_commit(1)",
    "PERFORM pgbully.log_apply(1)",
    "PERFORM pgbully.log_get_entry(1)",
    "PERFORM * FROM pgbully.log_get_stats()",
    "PERFORM * FROM pgbully.log_get_replication_status()",
    "PERFORM pgbully.log_sync_with_leader()",
    "PERFORM pgbully.replicate_entry('x')",
    "PERFORM pgbully.get_applied_index()",
    "PERFORM pgbully.record_applied_index(1)",
    "PERFORM pgbully.kv_put('k', 'v')",
    "PERFORM pgbully.kv_get('k')",
    "PERFORM pgbully.kv_delete('k')",
    "PERFORM pgbully.kv_exists('k')",
    "PERFORM pgbully.kv_list_keys()",
    "PERFORM * FROM pgbully.kv_get_stats()",
    "PERFORM pgbully.kv_compact()",
    "PERFORM pgbully.kv_reset()",
    "PERFORM pgbully.kv_put_local('k', 'v')",
    "PERFORM pgbully.kv_delete_local('k')")
{
    my ($fn) = $call =~ /(pgbully\.\w+)/;
    is(sqlstate_of($n1, $call), '0A000', "$fn raises feature_not_supported");
}

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
