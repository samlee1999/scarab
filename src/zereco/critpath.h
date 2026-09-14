/***************************************************************************************
 * File         : zereco/critpath.h
 * Description  : Critical chains of H2P branches (brslice_tab).
 *
 * An instruction with several source operands becomes ready when its LAST
 * source arrives.  Only the producer on that edge -- the Last Producer Register
 * (LPR) -- can move this instruction's ready time, and by induction only the
 * chain of such edges can move an H2P branch's resolution time.  This module
 * finds those chains; ZERECO_CRITPATH_PRIORITY gives their members IQ priority
 * and RFP_TARGET_CRITPATH makes their loads Target Loads.  With neither consumer
 * on, nothing written here is read by scheduling, renaming or the memory system,
 * and a run stays cycle-identical to one with ZERECO_CRITPATH_PROFILE off.
 *
 * Baseline mechanism (defaults in core.param.def, 2026-09-14):
 *
 *   seed         an H2P branch (HBT counter > 1, read when it commits) becomes a
 *                root (depth 0) and re-confirms itself on every such commit.
 *
 *   propagate    at commit, a member adds the producer of its last-arriving
 *                REGISTER source one level up, one level per dynamic instance.
 *                Only members and roots allocate brslice_tab entries (1K,
 *                128 x 8-way).  Propagation stops at an op that waited on no
 *                operand (the frontier).
 *
 *   filter A     a member at depth >= 2 propagates only when the same producer
 *                was its last-arriving one on 4 commits in a row (2-bit counter,
 *                threshold 3); depth 0 and 1 -- the branch and its compare --
 *                always propagate, since the compare is where two inputs compete.
 *
 *   refresh      a 1-bit confirm per entry, set by every propagation that names
 *                the entry (or a root's own H2P commit) and cleared every 10K
 *                committed ops; a member found already clear leaves.  This is what
 *                adapts the chain to phase changes, to a branch leaving H2P, and
 *                to edges that filter A stopped following.
 *
 * Also available for comparison or study: the PUBS-style full slice
 * (ZERECO_CRITPATH_FULL_SLICE), store->load edges (ZERECO_CRITPATH_MEM_EDGE),
 * filters B and C and the hysteresis form of A (not adopted), and the Phase A
 * measurements (slack, edge stability, confirm and owner histograms).
 *
 * Design notes live in src/zereco/zereco_CRITPATH_DESIGN.md.
 ***************************************************************************************/

#ifndef __ZERECO_CRITPATH_H__
#define __ZERECO_CRITPATH_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRITPATH_MAX_DEPTH 63 /* saturating chain-depth counter */

struct Op_struct;

void critpath_init(uns proc_id);
void critpath_reset(uns proc_id);

/* A source operand just woke `dep_op`.  Keep the largest and second largest
   source wake cycle, and remember which source owns the largest. */
void critpath_note_wake(struct Op_struct* src_op, struct Op_struct* dep_op,
                        uns8 rdy_bit);

/* Is this static PC currently a member of some H2P branch's critical chain?
   Read-only: it neither allocates nor ages an entry, so the consumption path
   cannot disturb what the learning path measures.  `max_depth` of 0 accepts any
   depth; otherwise only members within that distance of their branch qualify. */
Flag critpath_is_member(uns proc_id, Addr pc, uns max_depth);

/* An op is committing.  Everything is measured here so that only on-path,
   architecturally executed instructions contribute. */
void critpath_note_retire(struct Op_struct* op);

#ifdef __cplusplus
}
#endif

#endif /* __ZERECO_CRITPATH_H__ */
