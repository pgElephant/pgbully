# 001_bully.pl - end-to-end test of pgBully leader election and failover.
#
# Builds a three-node cluster (ids 1,2,3) wired together through pgbully.nodes
# and asserts the core invariants of the Bully algorithm:
#
#   * the highest reachable id is always the leader;
#   * killing the leader triggers failover to the next-highest node;
#   * a recovered higher-id node reclaims leadership ("the bully returns");
#   * exactly one leader exists at a time.
#
# Run with:  make installcheck   (requires the IPC::Run perl module)

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

my @nodes;       # index 0..2 -> node ids 1..3
my @ports;

# Build the shared pgbully.nodes membership string once we know the ports.
sub nodes_guc
{
    my @parts;
    for my $i (0 .. 2)
    {
        my $id = $i + 1;
        push @parts,
          "$id: host=127.0.0.1 port=$ports[$i] dbname=postgres user=$ENV{USER}";
    }
    return join(', ', @parts);
}

# Poll a node's reported leader id until it matches $want or we time out.
sub wait_for_leader
{
    my ($node, $want, $label) = @_;
    my $got = '';
    foreach (1 .. 100)            # ~20s at 200ms
    {
        $got = $node->safe_psql('postgres',
            'SELECT coalesce(pgbully.leader()::text, $$none$$)');
        return 1 if $got eq "$want";
        usleep(200_000);
    }
    diag("$label: expected leader $want, last saw '$got'");
    return 0;
}

# ---------------------------------------------------------------------------
# Set up three nodes
# ---------------------------------------------------------------------------

for my $i (0 .. 2)
{
    my $node = PostgreSQL::Test::Cluster->new("pgb$i");
    $node->init;
    push @nodes, $node;
    push @ports, $node->port;
}

my $membership = nodes_guc();

for my $i (0 .. 2)
{
    my $id   = $i + 1;
    my $node = $nodes[$i];
    $node->append_conf('postgresql.conf', <<"EOC");
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

# ---------------------------------------------------------------------------
# 1. Highest id wins the initial election
# ---------------------------------------------------------------------------
ok(wait_for_leader($nodes[0], 3, 'initial'), 'node 1 sees node 3 as leader');
ok(wait_for_leader($nodes[1], 3, 'initial'), 'node 2 sees node 3 as leader');
ok(wait_for_leader($nodes[2], 3, 'initial'), 'node 3 sees node 3 as leader');

is($nodes[2]->safe_psql('postgres', 'SELECT pgbully.is_leader()'),
    't', 'node 3 reports itself leader');
is($nodes[0]->safe_psql('postgres', 'SELECT pgbully.state()'),
    'follower', 'node 1 is a follower');

# status() returns a populated row
my $num = $nodes[2]->safe_psql('postgres',
    'SELECT num_nodes FROM pgbully.status()');
is($num, '3', 'status() reports three nodes');

# ---------------------------------------------------------------------------
# 2. Failover: kill the leader, node 2 should take over
# ---------------------------------------------------------------------------
$nodes[2]->stop('immediate');

ok(wait_for_leader($nodes[1], 2, 'failover'), 'node 2 becomes leader');
ok(wait_for_leader($nodes[0], 2, 'failover'), 'node 1 follows node 2');
is($nodes[1]->safe_psql('postgres', 'SELECT pgbully.is_leader()'),
    't', 'node 2 reports itself leader after failover');

# ---------------------------------------------------------------------------
# 3. The bully returns: restart node 3, it reclaims leadership
# ---------------------------------------------------------------------------
$nodes[2]->start;

ok(wait_for_leader($nodes[2], 3, 'reclaim'), 'node 3 reclaims leadership');
ok(wait_for_leader($nodes[0], 3, 'reclaim'), 'node 1 follows reclaimed node 3');
ok(wait_for_leader($nodes[1], 3, 'reclaim'), 'node 2 follows reclaimed node 3');

# ---------------------------------------------------------------------------
# 4. Exactly one leader across the cluster
# ---------------------------------------------------------------------------
my $leaders = 0;
for my $node (@nodes)
{
    $leaders++
      if $node->safe_psql('postgres', 'SELECT pgbully.is_leader()') eq 't';
}
is($leaders, 1, 'exactly one leader exists');

$_->stop('immediate') for @nodes;

done_testing();
