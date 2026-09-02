# The Bully algorithm

pgBully implements the leader-election algorithm introduced by Hector
Garcia-Molina in *"Elections in a Distributed Computing System"* (IEEE
Transactions on Computers, 1982), with a small, practical extension: a
monotonic **term** that hardens it against stale messages and clock effects.

## The textbook algorithm

Every process has a unique numeric id. The **highest-id live process is always
the coordinator (leader)**. The algorithm defines three messages:

- **ELECTION** — sent to processes with a *higher* id to ask "are you there?".
- **ANSWER / OK** — the reply that says "yes, I'll take over, stand down".
- **COORDINATOR** — broadcast by the winner to announce it is now leader.

When a process `P` notices the leader is gone (or it just started up):

1. `P` sends **ELECTION** to every process with a higher id.
2. If **nobody answers**, `P` wins: it becomes coordinator and sends
   **COORDINATOR** to all lower-id processes.
3. If **someone answers**, `P` gives up the election and waits for a
   **COORDINATOR** message. (If none arrives within a timeout, it restarts the
   election.)

When a process receives an **ELECTION** from a lower id, it returns **OK** and
starts its own election. The recursion drives the election "up" to the highest
live id, which wins. A recovering high-id process re-runs the election and
*bullies* the incumbent out of leadership — hence the name.

## How pgBully maps onto it

| Textbook concept | pgBully implementation |
|---|---|
| Process id | `pgbully.node_id` |
| ELECTION message | `pgbully.rpc_election(from)` over libpq |
| OK / ANSWER | the libpq call simply *succeeding* (the peer is reachable) |
| COORDINATOR message | `pgbully.rpc_coordinator(id, term)` |
| "leader is gone" detection | `election_timeout` elapses with no `HEARTBEAT` |
| recovering process re-elects | worker raises `election_requested` on startup |

pgBully adds a periodic **HEARTBEAT** (`rpc_heartbeat`) so followers can detect
a dead leader by silence rather than by probing, and so a returning higher-id
node is noticed quickly.

### Worker states

```
        ┌─────────────────────────────────────────────────────────┐
        │                                                         (heard higher term)
        ▼                                                             │
   ┌──────────┐  timeout / asked   ┌───────────┐  no higher answers  ┌────────┐
   │ FOLLOWER │ ─────────────────▶ │ CANDIDATE │ ──────────────────▶ │ LEADER │
   └──────────┘                    └─────┬─────┘                     └────────┘
        ▲                                │ a higher node answers          │
        │ COORDINATOR/HEARTBEAT          ▼                                │
        │                          ┌───────────┐                         │
        └───────────────────────── │  WAITING  │ ◀───────────────────────┘
                  (winner found)   └───────────┘   (timeout → re-elect)
```

- **FOLLOWER** — following a known leader, or waiting for one to appear.
- **CANDIDATE** — actively probing higher-id peers right now.
- **WAITING** — probed higher peers, at least one is alive, expecting it to
  win; re-runs the election if no coordinator arrives within
  `election_timeout`.
- **LEADER** — this node is the coordinator and is sending heartbeats.

## The term extension

The textbook algorithm assumes reliable messaging and no stale state. In a
real cluster, a deposed leader can come back and send an outdated COORDINATOR,
or two partitions can briefly each elect a leader. pgBully attaches a
monotonically increasing **term** to leadership:

- A node becoming leader **increments** the term and stamps its COORDINATOR and
  HEARTBEAT messages with it.
- A COORDINATOR/HEARTBEAT is **accepted only if its term ≥ the receiver's
  term**; older announcements are ignored.
- Every HEARTBEAT reply carries the receiver's current term. If a leader sees a
  **higher** term in a reply, it knows a newer leader exists and **steps down**.

This means a stale leader emerging from a partition is harmless: its term is
behind, its messages are rejected, and its own heartbeat replies tell it to
resign.

## The bully reasserts itself

pgBully is faithful to the algorithm's defining property — the highest live id
*always* leads:

- On startup or restart, a node raises `election_requested` and runs an
  election immediately.
- A follower that receives a COORDINATOR or HEARTBEAT from a **lower-id** leader
  raises `election_requested` to take over.

So when a higher-id node returns from the dead, it reclaims leadership within
about one election cycle, exactly as Garcia-Molina described.

## Guarantees and limits

**What it guarantees** (under a correct, fully-connected configuration):

- The highest reachable id converges to leadership.
- After the leader fails, the next-highest reachable id takes over within
  roughly `election_timeout`.
- The term is monotonic, so stale leaders are fenced out by message rejection.

**What it does not guarantee:**

- It is **not** a quorum/consensus protocol like Raft or Paxos. It does not use
  majority voting and does not by itself prevent two nodes in *different*
  network partitions from each believing they lead their side. The term
  mechanism resolves this the moment the partition heals (the lower-term leader
  steps down), but during a partition each side may act as leader.
- It does **not** fence I/O or move data. If you need write fencing or
  split-brain protection for application correctness, combine
  `pgbully.is_leader()` with an external fencing mechanism. See
  [operations.md](operations.md#consistency-caveats).

For most "one active node at a time" coordination tasks on a reliable network
— scheduled jobs, a singleton writer, choosing a replication source — the
Bully algorithm is simple, fast, and dependency-free.

## Complexity

For `N` nodes, the worst case (the lowest id detecting failure) is `O(N²)`
messages; the best case (the highest live id detecting failure) is `O(N)`.
Because pgBully drives elections from heartbeat timeouts and probes only
higher ids, steady-state traffic is just `O(N)` heartbeats per interval.
