#ifndef FROSTBYTE_PRIOQ_H
#define FROSTBYTE_PRIOQ_H

#include "types.h"
#include "rbt.h"

/*
 * Prioq: an ephemeral min-vruntime priority queue built on top of the
 * intrusive red-black tree (rbt.h) -- the "run queue" half of a CFS-lite
 * (Completely Fair Scheduler, weighted-fair) scheduler. The caller owns the
 * PrioQNode storage (typically embedded in a scheduler's thread struct,
 * next to the vruntime counter it mirrors) and is expected to rebuild the
 * queue from scratch every time a scheduling decision is needed: insert
 * every currently-eligible candidate, pop the minimum, discard the rest.
 * That "ephemeral" usage pattern (rather than a queue that persists node
 * membership across calls) sidesteps a real hazard for a preemptive kernel
 * scheduler: a thread can become eligible again from code paths other than
 * the scheduler itself (e.g. a wait-queue wakeup), and keeping a
 * long-lived queue in sync with every one of those paths is exactly the
 * kind of dual-source-of-truth bug this design avoids by never trying.
 *
 * Ordering key is (vruntime, id): id is a mandatory, unique tiebreaker
 * because rbtInsert silently rejects a node whose key compares equal to one
 * already present (no duplicates in the underlying tree) -- two candidates
 * that happen to share a vruntime would otherwise cause one of them to be
 * dropped from the queue entirely rather than merely deprioritised.
 */

typedef struct PrioQNode {
    RbtNode node;
    uint64 vruntime;
    uint32 id;
} PrioQNode;

typedef struct PrioQueue {
    RbTree tree;
} PrioQueue;

void prioqInit(PrioQueue* q);

/* (Re)inserts n into the queue, keyed by (vruntime, id). n's storage is
   owned by the caller and may be reused across unrelated queue lifetimes --
   every field the tree needs is reset unconditionally by the underlying
   rbtInsert. id must be unique among nodes simultaneously in the same
   queue. */
void prioqInsert(PrioQueue* q, PrioQNode* n, uint64 vruntime, uint32 id);

/* Returns the minimum-vruntime node without removing it, or nullptr if the
   queue is empty. */
PrioQNode* prioqPeekMin(const PrioQueue* q);

/* Removes and returns the minimum-vruntime node, or nullptr if the queue is
   empty. */
PrioQNode* prioqPopMin(PrioQueue* q);

/* Removes an arbitrary node that is currently a member of q. */
void prioqRemove(PrioQueue* q, PrioQNode* n);

usize prioqCount(const PrioQueue* q);
bool prioqIsEmpty(const PrioQueue* q);

#endif
