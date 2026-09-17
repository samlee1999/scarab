/* Resolution timeline of mispredicted main-thread H2P branches.
 *
 * A branch counts when it is on the main thread's correct path, the HBT marked
 * it hard at prediction, and it mispredicted (direction or target).  It is
 * resolved either by its own execution or by the TEA H2P execution that
 * scheduled an early flush for it; decode-time and late-BP recoveries are left
 * out.  The timeline runs from the cycle the branch was predicted to the
 * resolution, to the recovery, and to the first correct-path fetch after it.
 *
 * Fetch to resolution matches ZERECO_H2P_FETCH_TO_RESOLUTION on the test
 * branch (branch fetch to its execution) and averages over every event: a TEA
 * early flush can resolve a branch still in the FTQ, before it is fetched, and
 * such a branch counts as 0 cycles.
 *
 * Collected with TEA on and off (no flag), so a TEA run and a TEA-off run of
 * this branch see the same H2P definition (HBT_SIZE entries). */

#include "tea/main_h2p_resolution.h"

#include <string.h>

#include "globals/global_defs.h"

#include "core.param.h"
#include "statistics.h"

typedef struct Main_H2P_Pending_struct {
  Flag valid;
  Flag by_tea;
  Counter recovery_op_num;
  Counter predict_cycle;
  Counter fetch_cycle;
  Counter resolution_cycle;
  Counter recovery_cycle;
} Main_H2P_Pending;

static Main_H2P_Pending pending_recovery[MAX_NUM_PROCS];

void reset_main_h2p_resolution_profiler(void) {
  memset(pending_recovery, 0, sizeof(pending_recovery));
}

void main_h2p_resolution_begin_recovery(Op* op, Flag late_bp_recovery,
                                        Counter recovery_cycle) {
  if (!op || !op->op_pool_valid || op->proc_id >= MAX_NUM_PROCS ||
      op->thread_id != 0 || op->off_path || late_bp_recovery ||
      !op->table_info || op->table_info->cf_type == NOT_CF ||
      !op->oracle_info.hbt_pred_is_hard ||
      !(op->oracle_info.mispred || op->oracle_info.misfetch))
    return;

  /* cmp_recover() calls this before it clears tea_case1_pending_recovery */
  Flag by_tea = op->tea_early_flush_detected || op->tea_case1_pending_recovery;
  if (!by_tea && !op->oracle_info.recover_at_exec)
    return;

  Main_H2P_Pending* pending = &pending_recovery[op->proc_id];
  if (pending->valid)
    STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_PENDING_OVERWRITTEN);
  memset(pending, 0, sizeof(*pending));

  Counter resolution_cycle = op->exec_cycle;
  if (by_tea)
    resolution_cycle = op->tea_h2p_exec_cycle != MAX_CTR ?
                         op->tea_h2p_exec_cycle :
                         op->tea_case1_detect_cycle;

  Counter predict_cycle = op->recovery_info.predict_cycle;
  if (resolution_cycle == MAX_CTR || resolution_cycle < predict_cycle ||
      recovery_cycle < resolution_cycle) {
    STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_INVALID_TIMING);
    return;
  }

  pending->valid            = TRUE;
  pending->by_tea           = by_tea;
  pending->recovery_op_num  = op->op_num;
  pending->predict_cycle    = predict_cycle;
  /* fetch_cycle is 0 while the branch sits in the FTQ; MAX_CTR marks a branch
   * that was not fetched before it was resolved */
  pending->fetch_cycle      = (op->fetch_cycle && op->fetch_cycle != MAX_CTR &&
                          op->fetch_cycle <= resolution_cycle) ?
                               op->fetch_cycle :
                               MAX_CTR;
  pending->resolution_cycle = resolution_cycle;
  pending->recovery_cycle   = recovery_cycle;
}

void main_h2p_resolution_record_fetch(Op* op) {
  if (!op || op->proc_id >= MAX_NUM_PROCS)
    return;

  Main_H2P_Pending* pending = &pending_recovery[op->proc_id];
  if (!pending->valid || op->thread_id != 0 || op->off_path ||
      op->op_num <= pending->recovery_op_num)
    return;

  pending->valid = FALSE;
  if (op->fetch_cycle < pending->recovery_cycle) {
    STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_INVALID_TIMING);
    return;
  }

  Counter predict_to_resolution = pending->resolution_cycle -
                                  pending->predict_cycle;
  Counter resolution_to_recovery = pending->recovery_cycle -
                                   pending->resolution_cycle;
  Counter recovery_to_correct_fetch = op->fetch_cycle -
                                      pending->recovery_cycle;
  Counter predict_to_correct_fetch = op->fetch_cycle - pending->predict_cycle;

  STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_EVENTS);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_PREDICT_TO_RESOLUTION_TOTAL,
                 predict_to_resolution);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_PREDICT_TO_RESOLUTION_AVG,
                 predict_to_resolution);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_RESOLUTION_TO_RECOVERY_TOTAL,
                 resolution_to_recovery);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_RESOLUTION_TO_RECOVERY_AVG,
                 resolution_to_recovery);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_RECOVERY_TO_CORRECT_FETCH_TOTAL,
                 recovery_to_correct_fetch);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_RECOVERY_TO_CORRECT_FETCH_AVG,
                 recovery_to_correct_fetch);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_PREDICT_TO_CORRECT_FETCH_TOTAL,
                 predict_to_correct_fetch);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_PREDICT_TO_CORRECT_FETCH_AVG,
                 predict_to_correct_fetch);

  if (pending->by_tea) {
    STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_RESOLVED_BY_TEA);
    INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_TEA_PREDICT_TO_RESOLUTION_TOTAL,
                   predict_to_resolution);
    INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_TEA_PREDICT_TO_RESOLUTION_AVG,
                   predict_to_resolution);
  } else {
    STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_RESOLVED_BY_EXEC);
    INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_EXEC_PREDICT_TO_RESOLUTION_TOTAL,
                   predict_to_resolution);
    INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_EXEC_PREDICT_TO_RESOLUTION_AVG,
                   predict_to_resolution);
  }

  Counter fetch_to_resolution = 0;
  if (pending->fetch_cycle != MAX_CTR)
    fetch_to_resolution = pending->resolution_cycle - pending->fetch_cycle;
  else
    STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_RESOLVED_BEFORE_FETCH);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_FETCH_TO_RESOLUTION_TOTAL,
                 fetch_to_resolution);
  INC_STAT_EVENT(op->proc_id, MAIN_H2P_MISPRED_FETCH_TO_RESOLUTION_AVG,
                 fetch_to_resolution);
}
