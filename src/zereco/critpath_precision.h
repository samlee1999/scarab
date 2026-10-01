/***************************************************************************************
 * File         : zereco/critpath_precision.h
 * Description  : Is an accelerated instruction actually on the critical path?  Measurement only.
 *
 * Membership in brslice_tab is a property of a static PC, learned from past
 * instances where that PC was the last-arriving producer.  Acceleration is
 * granted per dynamic instance.  The two are not the same thing: the critical
 * producer of a consumer changes from instance to instance, so an accelerated
 * instance may or may not be the one that actually holds the branch back.  This
 * module measures that gap directly, at two strictnesses:
 *
 *   Level 1  this instance was the last-arriving source of its consumer.
 *            One edge.  Necessary for Level 2, not sufficient.
 *
 *   Level 2  this instance is reachable from an H2P branch instance by walking
 *            last-arriving edges backward -- the real dynamic critical path.
 *            Recorded twice: over every H2P branch instance (what the mechanism
 *            aims at) and over the mispredicted ones alone (where the benefit
 *            actually lands).
 *
 * From the two populations come precision (of what we accelerate, how much is
 * critical -- the headroom a better identifier could claim) and recall (of what
 * is critical, how much we accelerate -- what the identifier misses).  Neither
 * is meaningful alone: accelerating almost nothing makes precision perfect.
 *
 * "Accelerated" comes in three widths: carried the priority bit (CAND), actually
 * took a priority RS entry (ENTRY), and ENTRY or a load whose value came from a
 * useful register-file prefetch (ACCEL) -- the last is what the machine sped up.
 *
 * THE TIME PROBLEM.  Retirement is in program order, so a chain's producers
 * commit and are freed BEFORE the branch they feed commits -- walking backward
 * from the branch through live ops is not possible.  A ring of committed-op
 * records indexed by op_num bridges the gap: every commit writes a record, the
 * H2P branch walks the ring instead of the pipeline, and a record is counted
 * when it is evicted, by which time the walk has had its chance to mark it.
 * CRITPATH_PREC_WALK_MISS says how often the ring was too short.
 *
 * Nothing here is read back into the machine, so a run with
 * ZERECO_CRITPATH_PRECISION_PROFILE on is cycle-identical to one without it.
 ***************************************************************************************/

#ifndef __ZERECO_CRITPATH_PRECISION_H__
#define __ZERECO_CRITPATH_PRECISION_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Op_struct;

void critpath_precision_init(void);

/* An op is committing.  Call after critpath_note_retire() so the membership and
   priority fields describe the same instance. */
void critpath_precision_note_retire(struct Op_struct* op);

/* Drain the ring and emit the population tables.  Called before the final
   dump_stats(), from cmp_per_core_done(). */
void critpath_precision_done(uns proc_id);

#ifdef __cplusplus
}
#endif

#endif /* __ZERECO_CRITPATH_PRECISION_H__ */
