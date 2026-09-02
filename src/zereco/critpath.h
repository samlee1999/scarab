/***************************************************************************************
 * File         : zereco/critpath.h
 * Description  : Critical-path slice observation (Phase A -- measurement only).
 *
 * An instruction with several source operands becomes ready when its LAST
 * source arrives.  Only the producer on that edge -- the Last Producer Register
 * (LPR) -- can move this instruction's ready time, and by induction only the
 * chain of such edges can move an H2P branch's resolution time.  This module
 * observes those edges; it never changes them.
 *
 * Everything here is instrumentation.  No field written here is read by
 * scheduling, renaming, or the memory system, so a run with
 * ZERECO_CRITPATH_PROFILE on must stay cycle-identical to one with it off.
 * That identity is the acceptance test for Phase A.
 *
 * What it measures, and what each number decides:
 *
 *   slack = t_last - t_second     how much later the critical operand arrived
 *                                 than the runner-up.  Accelerating the winning
 *                                 chain can only pay off up to this many cycles
 *                                 before the sibling becomes binding, so the
 *                                 distribution sets the Delta-window: below it,
 *                                 both producers are co-critical and both must
 *                                 be tracked.
 *
 *   argmax stability              how often the same static PC picks a different
 *                                 source as its LPR.  Sets how much confirmation
 *                                 a chain member needs before it is trusted.
 *
 *   frontier                      instructions that never waited on an operand.
 *                                 Backward propagation has nothing to gain past
 *                                 them, which is where a walk should stop.
 *
 *   depth                         how many levels a chain reaches, i.e. how long
 *                                 incremental one-level-per-instance learning
 *                                 takes to warm up.
 *
 * Design notes live in src/zereco/zereco_CRITPATH_DESIGN.md.
 ***************************************************************************************/

#ifndef __ZERECO_CRITPATH_H__
#define __ZERECO_CRITPATH_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Observation tables are scaffolding, not modeled hardware, so they are sized
   generously enough that capacity never distorts a measurement.  Phase B
   replaces them with the real, budgeted structures. */
#define CRITPATH_PC_TABLE_ENTRIES (64 * 1024) /* PC -> previous LPR source */
#define CRITPATH_MAX_DEPTH 63                 /* saturating chain-depth counter */

struct Op_struct;

void critpath_init(uns proc_id);
void critpath_reset(uns proc_id);

/* A source operand just woke `dep_op`.  Keep the largest and second largest
   source wake cycle, and remember which source owns the largest. */
void critpath_note_wake(struct Op_struct* src_op, struct Op_struct* dep_op,
                        uns8 rdy_bit);

/* An op is committing.  Everything is measured here so that only on-path,
   architecturally executed instructions contribute. */
void critpath_note_retire(struct Op_struct* op);

#ifdef __cplusplus
}
#endif

#endif /* __ZERECO_CRITPATH_H__ */
