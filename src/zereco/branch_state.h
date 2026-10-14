/***************************************************************************************
 * File         : zereco/branch_state.h
 * Description  : Per-branch H2P state ledger -- measurement only.
 *
 * Question it answers: which branches are worth keeping a critical slice for?
 * CLEAR seeds a chain when a branch is H2P (HBT counter > 1) and accelerates its
 * slice for as long as it stays H2P.  PUBS instead builds a slice for EVERY
 * branch and decides at use time.  Between those lies a cheaper rule: admit a
 * branch's slice to brslice_tab at a LOW threshold on the same HBT counter and
 * accelerate at the high one -- one counter, two thresholds, no new structure.
 * Choosing the low threshold needs the population: how many branches never
 * mispredict at all, how many mispredict but never reach H2P, how long a branch
 * sits one step below H2P before it crosses (what the low threshold buys as
 * pre-built chain), and how long the states last (what a refresh window must
 * cover).
 *
 * This module keeps one record per static branch PC in an unbounded table that
 * exists only in the simulator.  Nothing reads it back into the machine, so a
 * run with ZERECO_BRANCH_STATE_PROFILE on is cycle-identical to one with it off.
 *
 * It also keeps a SHADOW HBT: the same 3-bit counter rules (allocate on a
 * misprediction at 1, H2P above 1, every counter decremented each hbt_decay_interval (50K) retired
 * instructions) with no capacity limit at all.  The real table is 256 entries,
 * 8-way; the difference between the two is the set of branches the real HBT
 * loses to conflicts -- which is exactly the false-negative bound of anything
 * gated on H2P status.
 ***************************************************************************************/

#ifndef __ZERECO_BRANCH_STATE_H__
#define __ZERECO_BRANCH_STATE_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Op_struct;

void branch_state_init(void);

/* A branch retired.  hbt_counter is the value the HBT holds for it AFTER its own
   update -- exactly what op->oracle_info.hbt_misp_counter carries and what the
   critical-path seed gate tests a few lines later in node_stage.c, so the
   population tables and the gate classify an instance the same way. */
void branch_state_note_retire(struct Op_struct* op, uns32 hbt_counter);

/* brslice_tab did learning for this branch's chain while the branch sat at HBT
   counter 1 (critpath.c): `counts` commits that updated filter A's counters and
   `regs` members or tracked entries registered.  Credited to the branch's open
   stretch at counter 1, if any, whose outcome -- promoted or lapsed -- Table 6
   reports.  Looks the branch up, never adds it. */
void branch_state_note_prep(Addr branch_pc, uns counts, uns regs);

/* How many H2P instances this branch has run since its latest promotion, the one
   retiring now included -- 1 for the instance that crossed the threshold.  0 when
   the branch is not H2P at its latest retire, or the ledger is off.  Call after
   branch_state_note_retire() for the same instance. */
Counter branch_state_h2p_age(Addr branch_pc);

/* One retired instruction.  Drives the shadow HBT's decay (hbt_decay_interval) and the global
   op clock the dwell histograms are measured in. */
void branch_state_retire_tick(uns proc_id);

/* Walk the ledger once at the end of the run and emit the population tables. */
void branch_state_done(uns proc_id);

#ifdef __cplusplus
}
#endif

#endif /* __ZERECO_BRANCH_STATE_H__ */
