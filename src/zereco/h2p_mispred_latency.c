#include "zereco/h2p_mispred_latency.h"

#include <string.h>

#include "globals/global_defs.h"

#include "core.param.h"
#include "op.h"
#include "statistics.h"

typedef struct Zereco_H2P_Mispred_Pending_struct {
  Flag valid;
  Flag mispred;
  Flag misfetch;
  Flag stage_timing_valid;
  Counter recovery_op_num;
  Addr correct_fetch_addr;
  Counter branch_fetch_cycle;
  Counter resolution_cycle;
  Counter recovery_cycle;
  Counter frontend_cycles;
  Counter dependency_cycles;
  Counter scheduler_cycles;
  Counter execution_cycles;
} Zereco_H2P_Mispred_Pending;

static Zereco_H2P_Mispred_Pending pending_recovery[MAX_NUM_PROCS];

static Flag zereco_h2p_mispred_latency_target(Op* op) {
  return op && op->proc_id < MAX_NUM_PROCS && op->thread_id == 0 &&
         !op->off_path && op->table_info &&
         op->table_info->cf_type != NOT_CF &&
         op->oracle_info.hbt_pred_is_hard &&
         op->oracle_info.recover_at_exec &&
         (op->oracle_info.mispred || op->oracle_info.misfetch);
}

void reset_zereco_h2p_mispred_latency_profiler(void) {
  memset(pending_recovery, 0, sizeof(pending_recovery));
}

void zereco_h2p_mispred_latency_begin_recovery(Op* op, Addr correct_fetch_addr,
                                                Counter recovery_cycle) {
  if (!ZERECO_H2P_MISPRED_LATENCY_PROFILE ||
      !zereco_h2p_mispred_latency_target(op))
    return;

  STAT_EVENT(op->proc_id, ZERECO_H2P_EXEC_RECOVERY_CANDIDATES);

  Zereco_H2P_Mispred_Pending* pending = &pending_recovery[op->proc_id];
  if (pending->valid)
    STAT_EVENT(op->proc_id, ZERECO_H2P_PENDING_RECOVERY_OVERWRITTEN);

  memset(pending, 0, sizeof(*pending));

  if (op->fetch_cycle == 0 || op->exec_cycle == MAX_CTR ||
      op->exec_cycle < op->fetch_cycle || recovery_cycle < op->exec_cycle) {
    STAT_EVENT(op->proc_id, ZERECO_H2P_INVALID_TIMING);
    return;
  }

  pending->valid = TRUE;
  pending->mispred = op->oracle_info.mispred;
  pending->misfetch = op->oracle_info.misfetch;
  pending->recovery_op_num = op->op_num;
  pending->correct_fetch_addr = correct_fetch_addr;
  pending->branch_fetch_cycle = op->fetch_cycle;
  pending->resolution_cycle = op->exec_cycle;
  pending->recovery_cycle = recovery_cycle;

  if (op->issue_cycle != MAX_CTR && op->sched_cycle != MAX_CTR &&
      op->issue_cycle >= op->fetch_cycle) {
    Counter ready_cycle = op->rdy_cycle > op->issue_cycle ?
                            op->rdy_cycle : op->issue_cycle;
    if (op->sched_cycle >= ready_cycle && op->exec_cycle >= op->sched_cycle) {
      pending->stage_timing_valid = TRUE;
      pending->frontend_cycles = op->issue_cycle - op->fetch_cycle;
      pending->dependency_cycles = ready_cycle - op->issue_cycle;
      pending->scheduler_cycles = op->sched_cycle - ready_cycle;
      pending->execution_cycles = op->exec_cycle - op->sched_cycle;
    }
  }

  if (!pending->stage_timing_valid)
    STAT_EVENT(op->proc_id, ZERECO_H2P_INVALID_STAGE_TIMING);
}

void zereco_h2p_mispred_latency_record_fetch(Op* op) {
  if (!ZERECO_H2P_MISPRED_LATENCY_PROFILE || !op ||
      op->proc_id >= MAX_NUM_PROCS)
    return;

  Zereco_H2P_Mispred_Pending* pending = &pending_recovery[op->proc_id];
  if (!pending->valid || op->thread_id != 0 || op->off_path ||
      op->op_num <= pending->recovery_op_num)
    return;

  if (op->fetch_cycle < pending->recovery_cycle ||
      op->fetch_cycle < pending->resolution_cycle) {
    STAT_EVENT(op->proc_id, ZERECO_H2P_INVALID_TIMING);
    pending->valid = FALSE;
    return;
  }

  if (op->inst_info && op->inst_info->addr != pending->correct_fetch_addr)
    STAT_EVENT(op->proc_id, ZERECO_H2P_CORRECT_FETCH_ADDR_MISMATCH);

  Counter fetch_to_resolution =
    pending->resolution_cycle - pending->branch_fetch_cycle;
  Counter resolution_to_recovery =
    pending->recovery_cycle - pending->resolution_cycle;
  Counter recovery_to_correct_fetch =
    op->fetch_cycle - pending->recovery_cycle;
  Counter post_resolution =
    op->fetch_cycle - pending->resolution_cycle;
  Counter fetch_to_correct_fetch =
    op->fetch_cycle - pending->branch_fetch_cycle;

  STAT_EVENT(op->proc_id, ZERECO_H2P_TIMELINE_EVENTS);
  if (pending->mispred)
    STAT_EVENT(op->proc_id, ZERECO_H2P_MISPRED_EVENTS);
  if (pending->misfetch)
    STAT_EVENT(op->proc_id, ZERECO_H2P_MISFETCH_EVENTS);

  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_FETCH_TO_RESOLUTION_TOTAL,
                 fetch_to_resolution);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_FETCH_TO_RESOLUTION_AVG,
                 fetch_to_resolution);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_RESOLUTION_TO_RECOVERY_TOTAL,
                 resolution_to_recovery);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_RESOLUTION_TO_RECOVERY_AVG,
                 resolution_to_recovery);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_RECOVERY_TO_CORRECT_FETCH_TOTAL,
                 recovery_to_correct_fetch);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_RECOVERY_TO_CORRECT_FETCH_AVG,
                 recovery_to_correct_fetch);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_POST_RESOLUTION_TOTAL,
                 post_resolution);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_POST_RESOLUTION_AVG,
                 post_resolution);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_FETCH_TO_CORRECT_FETCH_TOTAL,
                 fetch_to_correct_fetch);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_FETCH_TO_CORRECT_FETCH_AVG,
                 fetch_to_correct_fetch);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_RESOLUTION_PORTION,
                 fetch_to_resolution);
  INC_STAT_EVENT(op->proc_id, ZERECO_H2P_POST_RESOLUTION_PORTION,
                 post_resolution);

  if (pending->stage_timing_valid) {
    STAT_EVENT(op->proc_id, ZERECO_H2P_STAGE_TIMING_EVENTS);
    INC_STAT_EVENT(op->proc_id, ZERECO_H2P_FRONTEND_TOTAL,
                   pending->frontend_cycles);
    INC_STAT_EVENT(op->proc_id, ZERECO_H2P_FRONTEND_AVG,
                   pending->frontend_cycles);
    INC_STAT_EVENT(op->proc_id, ZERECO_H2P_DEPENDENCY_TOTAL,
                   pending->dependency_cycles);
    INC_STAT_EVENT(op->proc_id, ZERECO_H2P_DEPENDENCY_AVG,
                   pending->dependency_cycles);
    INC_STAT_EVENT(op->proc_id, ZERECO_H2P_SCHEDULER_TOTAL,
                   pending->scheduler_cycles);
    INC_STAT_EVENT(op->proc_id, ZERECO_H2P_SCHEDULER_AVG,
                   pending->scheduler_cycles);
    INC_STAT_EVENT(op->proc_id, ZERECO_H2P_EXECUTION_TOTAL,
                   pending->execution_cycles);
    INC_STAT_EVENT(op->proc_id, ZERECO_H2P_EXECUTION_AVG,
                   pending->execution_cycles);
  }

  pending->valid = FALSE;
}
