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
 * That identity is the acceptance test.
 *
 * Phase A measured the edges themselves: the slack between the critical operand
 * and the runner-up (which sets the window below which two producers are
 * co-critical), how stable the critical edge is per static PC, where a walk
 * naturally terminates, and how far chains reach.  It also showed the problem
 * Phase A2 exists to solve -- following only critical edges still leaves two
 * thirds of committed instructions inside some chain, so selectivity has to come
 * from somewhere else.
 *
 * Phase A2 adds the three knobs that can supply it, and measures all of their
 * settings at once rather than sweeping them:
 *
 *   confirmation counter   how often a PC has been re-derived as a critical
 *                          producer.  Bucketing member commits by their counter
 *                          yields the population for EVERY threshold from one
 *                          run: population(>=T) is the tail sum above T.
 *
 *   decay                  periodically ages every counter, so a PC that stops
 *                          being re-confirmed leaves the chain on its own.  This
 *                          is what adapts membership to a phase change, and the
 *                          only knob that needs separate runs.
 *
 *   insertion gate         which branches may seed a chain at all.  Bucketing by
 *                          the owner's current misprediction counter yields the
 *                          population under every gate from one run.
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

/* An op is committing.  Everything is measured here so that only on-path,
   architecturally executed instructions contribute. */
void critpath_note_retire(struct Op_struct* op);

#ifdef __cplusplus
}
#endif

#endif /* __ZERECO_CRITPATH_H__ */
