/*
 * Copyright 2020 HPS/SAFARI Research Groups
 * Copyright 2025 Litz Lab
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : exec_stage.c
 * Author       : HPS Research Group, Litz Lab
 * Date         : 1/27/1999, 4/12/2025
 * Description  :
 ***************************************************************************************/

#include "exec_stage.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "debug/memview.h"

#include "bp/bp.param.h"
#include "core.param.h"
#include "general.param.h"
#include "memory/memory.param.h"

#include "bp/bp.h"
#include "dvfs/perf_pred.h"

#include "cmp_model.h"
#include "exec_ports.h"
#include "map.h"
#include "map_rename.h"
#include "op_pool.h"
#include "statistics.h"

#include "log/recovery_log.h"

#include "tea/tea_thread.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_EXEC_STAGE, ##args)

/**************************************************************************************/
/* Global Variables */

Exec_Stage* exec = NULL;
int op_type_delays[NUM_OP_TYPES];
int exec_off_path;

/**************************************************************************************/
/* Prototypes */

static inline void init_op_type_delays();
static inline void exec_stage_inc_power_stats(Op* op);

static inline int exec_stage_check_fu_available(int ii);
static inline void exec_stage_reject_op(Stage_Data* src_sd, int ii, int event);
static inline void exec_stage_clear_fu(int ii);

static inline void exec_stage_dep_wakeup(Op* op);
static inline void exec_stage_process_op(Op* op);
static inline void exec_stage_bp_resolve(Op* op);

/**************************************************************************************/
/* set_exec_stage: */

void set_exec_stage(Exec_Stage* new_exec) {
  exec = new_exec;
}

/**************************************************************************************/
// init_op_type_delays

static inline void init_op_type_delays() {
  uns ii;

  if (UNIFORM_OP_DELAY) {
    for (ii = 0; ii < NUM_OP_TYPES; ii++)
      op_type_delays[ii] = UNIFORM_OP_DELAY;
    return;
  }

#define OP_TYPE_DELAY_INIT(op_type) op_type_delays[OP_##op_type] = OP_##op_type##_DELAY;
  OP_TYPE_LIST(OP_TYPE_DELAY_INIT);
#undef OP_TYPE_DELAY_INIT

  /* make sure all op_type_delays were set */
  for (ii = 0; ii < NUM_OP_TYPES; ii++)
    ASSERT(td->proc_id, op_type_delays[ii] != 0);
}

/**************************************************************************************/
/* init_exec_stage: */

void init_exec_stage(uns8 proc_id, const char* name) {
  ASSERT(proc_id, exec);
  DEBUG(proc_id, "Initializing %s stage\n", name);

  memset(exec, 0, sizeof(Exec_Stage));

  exec->proc_id = proc_id;

  exec->sd.name = (char*)strdup(name);
  exec->sd.max_op_count = NUM_FUS;
  exec->sd.ops = (Op**)malloc(sizeof(Op*) * NUM_FUS);
  exec->fus_busy = 0;

  reset_exec_stage();

  init_op_type_delays();
}

/**************************************************************************************/
/* reset_exec_stage: */

void reset_exec_stage() {
  uns ii;
  exec->sd.op_count = 0;
  for (ii = 0; ii < NUM_FUS; ii++) {
    exec->sd.ops[ii] = NULL;
  }
}

/**************************************************************************************/
/* recover_exec_stage: */

void recover_exec_stage() {
  uns ii;
  exec_off_path = 0;
  for (ii = 0; ii < NUM_FUS; ii++) {
    Func_Unit* fu = &exec->fus[ii];
    Op* op = exec->sd.ops[ii];
    if (!op) continue;
    /* TEA ops are managed by flush_tea_ops_by_chain_id() via terminate_tea_chain().
     * Skipping here prevents surviving chains from losing their exec-stage ops. */
    if (TEA_ENABLE && op->thread_id == 1) continue;
    if (op->op_num > bp_recovery_info->recovery_op_num) {
      exec->sd.ops[ii] = NULL;
      exec->sd.op_count--;
      fu->avail_cycle = cycle_count + 1;
      fu->idle_cycle = cycle_count + 1;
    }
  }
}

/**************************************************************************************/
/* debug_exec_stage: */

void debug_exec_stage() {
  int ii;
  DPRINTF("# %-10s  op_count:%d  busy:", exec->sd.name, exec->sd.op_count);
  for (ii = 0; ii < NUM_FUS; ii++) {
    Func_Unit* fu = &exec->fus[ii];
    if (ii % 4 == 0)
      DPRINTF(" ");
    DPRINTF("%d", fu->idle_cycle > cycle_count);
  }
  DPRINTF("  mem_stalls:");
  for (ii = 0; ii < NUM_FUS; ii++) {
    Func_Unit* fu = &exec->fus[ii];
    if (ii % 4 == 0)
      DPRINTF(" ");
    DPRINTF("%d", fu->held_by_mem);
  }
  DPRINTF("\n");
  print_op_array(GLOBAL_DEBUG_STREAM, exec->sd.ops, NUM_FUS, NUM_FUS);
}

/**************************************************************************************/
/* exec_cycle: */

void update_exec_stage(Stage_Data* src_sd) {
  ASSERT(exec->proc_id, exec->sd.op_count <= exec->sd.max_op_count);

  if (!exec_off_path) {
    if (!exec->sd.op_count)
      STAT_EVENT(exec->proc_id, EXEC_STAGE_STARVED);
    else
      STAT_EVENT(exec->proc_id, EXEC_STAGE_NOT_STARVED);
  } else {
    STAT_EVENT(exec->proc_id, EXEC_STAGE_OFF_PATH);
  }

  for (uns ii = 0; ii < src_sd->max_op_count; ii++) {
    if (src_sd->ops[ii] && src_sd->ops[ii]->off_path)
      exec_off_path = 1;
  }

  /* phase 1 - success/failure of latching and wake up of dependent ops */
  for (uns ii = 0; ii < src_sd->max_op_count; ii++) {
    /* do issue availability checking before latching */
    Op* op = src_sd->ops[ii];
    int ret = exec_stage_check_fu_available(ii);
    if (ret != 0) {
      // remove the op from the stage_data if it cannot be issued to make it get scheduled again
      if (op != NULL) {
        exec_stage_reject_op(src_sd, ii, ret);
      }
      ASSERT(exec->proc_id, !src_sd->ops[ii]);
      continue;
    }

    // remove the fop from the FU if the FU is not busy and the instruction is ready to be issued
    Op* fop = exec->sd.ops[ii];
    if (fop) {
      exec_stage_clear_fu(ii);
    }

    // check register file (late allocation) if op can issue
    if (!reg_file_issue(op)) {
      exec_stage_reject_op(src_sd, ii, FU_OTHER_UNAVAILABLE);
      STAT_EVENT(exec->proc_id, FU_STARVED);
      continue;
    }

    // increase the starved counters after the FU checking
    if (!op) {
      STAT_EVENT(exec->proc_id, FU_STARVED);
      continue;
    }

    ASSERTM(exec->proc_id, OP_SRCS_RDY(op), "op_num:%s\n", unsstr64(op->op_num));
    ASSERT(exec->proc_id, get_fu_type(op->table_info->op_type, op->table_info->is_simd) & exec->fus[ii].type);

    /* if we get to here, then it means the op is going into the functional unit. */
    op->sched_cycle = cycle_count;
    DEBUG(exec->proc_id, "op_num:%s fu_num:%d sched_cycle:%s off_path:%d\n", unsstr64(op->op_num), op->fu_num,
          unsstr64(op->sched_cycle), op->off_path);

    // consume the src register values
    reg_file_consume(op);

    /*
     * We need to perform wake-ups of all the dependent ops.
     * This is done before the actual latching of the ops into the execute stage,
     * in order to make sure ops flushed or replayed during the current cycle do not sneak into the execute stage
     * because they happened to be processed before the op causing the recovery or replay.
     */
    exec_stage_dep_wakeup(op);

    // PMU stat counters update
    exec_stage_inc_power_stats(op);
  }

  /* phase 2 - actual latching of instructions and setting of state */
  for (uns ii = 0; ii < src_sd->max_op_count; ii++) {
    Func_Unit* fu = &exec->fus[ii];
    Op* op = src_sd->ops[ii];

    // if there is still an op in the fu, the fu is still busy and there is nothing to latch
    Op* fop = exec->sd.ops[ii];
    if (fop) {
      /*
       * For each op, if its target FU is busy, one of the following must happen:
       * 1. The op is NULL. No op scheduled for this FU.
       * 2. The op is removed becase the FU is not yet ready.
       * 3. The op is removed becase of a memory stall.
       */
      ASSERT(exec->proc_id, !op);

      STAT_EVENT(exec->proc_id, FU_BUSY_0 + ii);
      STAT_EVENT(exec->proc_id, FUS_BUSY_ON_PATH + fop->off_path);
      if (fop->table_info->mem_type) {
        fu->held_by_mem = TRUE;
        STAT_EVENT(exec->proc_id, FU_BUSY_MEM_STALL);
      }
      continue;
    }

    fu->held_by_mem = FALSE;

    if (!op) {
      STAT_EVENT(exec->proc_id, FUS_EMPTY);
      continue;  // there is nothing to latch from the previous stage
    }

    STAT_EVENT(exec->proc_id, FU_BUSY_0 + ii);
    STAT_EVENT(exec->proc_id, FUS_BUSY_ON_PATH + op->off_path);

    /* remove the op from the "schedule" list */
    src_sd->ops[ii] = NULL;
    src_sd->op_count--;
    ASSERT(exec->proc_id, src_sd->op_count >= 0);

    /* busy the functional unit */
    exec->fus_busy++;
    exec->sd.ops[ii] = op;
    exec->sd.op_count++;
    ASSERT(exec->proc_id, exec->sd.op_count <= exec->sd.max_op_count);
    /*
     * The op's latency is assigned a negative value if it is not pipelined.
     * If the op is not pipelined, keep the functional unit busy for the full duration.
     */
    int latency = op->inst_info->latency;
    fu->avail_cycle = cycle_count + (latency < 0 ? -latency : 1);
    fu->idle_cycle = cycle_count + (latency < 0 ? -latency : latency);

    /* update op status and execution metadata; increment PMU counters */
    exec_stage_process_op(op);

    /* branch recovery/resolution */
    Flag is_replay = FALSE;  // TODO: check if this val is needed
    if (op->table_info->cf_type && !is_replay) {
      /*
       * branch recovery currently does not like to be done more than 1 time.
       * since we don't have any way to know if an op is going to be replayed,
       * we have to go with the first recovery (even though it is improper) for the time being
       */
      exec_stage_bp_resolve(op);
    }

    /* value prediction recovery/resolution code  */
    // if we know the value at this point if not ? then we need to wait.
  }

  exec->fus_busy = 0;
  for (uns ii = 0; ii < src_sd->max_op_count; ii++) {
    Func_Unit* fu = &exec->fus[ii];
    /*
     * a functional unit is busy if there's an op in any stage
     * of its pipeline unless it's stalled by memory
     */
    if (fu->idle_cycle > cycle_count && !fu->held_by_mem) {
      exec->fus_busy++;
    }
  }

  memview_fus_busy(exec->proc_id, exec->fus_busy);
}

/**************************************************************************************/
/* Inline Methods */

static inline void exec_stage_inc_power_stats(Op* op) {
  STAT_EVENT(op->proc_id, POWER_ROB_READ);
  STAT_EVENT(op->proc_id, POWER_ROB_WRITE);

  STAT_EVENT(op->proc_id, POWER_OP);

  if (op->table_info->op_type > OP_NOP && op->table_info->op_type < OP_FLD) {
    STAT_EVENT(op->proc_id, POWER_INT_OP);
  } else if (op->table_info->op_type >= OP_FLD) {
    STAT_EVENT(op->proc_id, POWER_FP_OP);
  }

  if (op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_PF) {
    STAT_EVENT(op->proc_id, POWER_LD_OP);
  } else if (op->table_info->mem_type == MEM_ST) {
    STAT_EVENT(op->proc_id, POWER_ST_OP);
  }

  if (!op->off_path) {
    STAT_EVENT(op->proc_id, POWER_COMMITTED_OP);

    if (op->table_info->op_type > OP_NOP && op->table_info->op_type < OP_FLD) {
      STAT_EVENT(op->proc_id, POWER_COMMITTED_INT_OP);
    } else {
      STAT_EVENT(op->proc_id, POWER_COMMITTED_FP_OP);
    }
  }

  if (op->table_info->cf_type == CF_CALL || op->table_info->cf_type == CF_ICALL) {
    STAT_EVENT(op->proc_id, POWER_FUNCTION_CALL);
  }

  if (op->table_info->cf_type > NOT_CF) {
    STAT_EVENT(op->proc_id, POWER_BRANCH_OP);
  }

  if (power_get_fu_type(op->table_info->op_type, op->table_info->is_simd) != POWER_FU_FPU) {
    /* Integer instructions */
    INC_STAT_EVENT(op->proc_id, POWER_RENAME_READ, 2);
    STAT_EVENT(op->proc_id, POWER_RENAME_WRITE);

    STAT_EVENT(op->proc_id, POWER_INST_WINDOW_READ);
    STAT_EVENT(op->proc_id, POWER_INST_WINDOW_WRITE);
    STAT_EVENT(op->proc_id, POWER_INST_WINDOW_WAKEUP_ACCESS);

    INC_STAT_EVENT(op->proc_id, POWER_INT_REGFILE_READ, op->table_info->num_src_regs);
    INC_STAT_EVENT(op->proc_id, POWER_INT_REGFILE_WRITE, op->table_info->num_dest_regs);

    if (power_get_fu_type(op->table_info->op_type, op->table_info->is_simd) == POWER_FU_MUL_DIV) {
      INC_STAT_EVENT(op->proc_id, POWER_MUL_ACCESS, abs(op_type_delays[op->table_info->type]));
      STAT_EVENT(op->proc_id, POWER_CDB_MUL_ACCESS);
    } else {
      INC_STAT_EVENT(op->proc_id, POWER_IALU_ACCESS, abs(op_type_delays[op->table_info->type]));
      STAT_EVENT(op->proc_id, POWER_CDB_IALU_ACCESS);
    }
  } else {
    /*Floating Point instructions*/
    STAT_EVENT(op->proc_id, POWER_FP_RENAME_WRITE);
    INC_STAT_EVENT(op->proc_id, POWER_FP_RENAME_READ, 2);

    STAT_EVENT(op->proc_id, POWER_FP_INST_WINDOW_READ);
    STAT_EVENT(op->proc_id, POWER_FP_INST_WINDOW_WRITE);
    STAT_EVENT(op->proc_id, POWER_FP_INST_WINDOW_WAKEUP_ACCESS);

    INC_STAT_EVENT(op->proc_id, POWER_FP_REGFILE_READ, op->table_info->num_src_regs);
    INC_STAT_EVENT(op->proc_id, POWER_FP_REGFILE_WRITE, op->table_info->num_dest_regs);

    INC_STAT_EVENT(op->proc_id, POWER_FPU_ACCESS, abs(op_type_delays[op->table_info->type]));
    STAT_EVENT(op->proc_id, POWER_CDB_FPU_ACCESS);
  }

  if (op->table_info->mem_type == MEM_ST) {
    STAT_EVENT(op->proc_id, POWER_DTLB_ACCESS);
  }
}

static inline void exec_stage_dep_wakeup(Op* op) {
  Counter exec_cycle = cycle_count + abs(op->inst_info->latency);

  // non-memory ops will always distribute their results after the op's latency
  if (op->table_info->mem_type == NOT_MEM) {
    op->wake_cycle = exec_cycle;
    wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
    return;
  }

  // stores have their addresses computed in this cycle and also write their data into the store buffer
  if (op->table_info->mem_type == MEM_ST) {
    // only wake up if this is the first time this op executes
    if (op->exec_count == 0) {
      op->wake_cycle = exec_cycle;
      wake_up_ops(op, MEM_ADDR_DEP, model->wake_hook);
      wake_up_ops(op, MEM_DATA_DEP, model->wake_hook);
    }
    return;
  }

  // all other ops (loads) will be handled by the memory system
}

static inline void exec_stage_reject_op(Stage_Data* src_sd, int ii, int event) {
  Op* op = src_sd->ops[ii];

  op->delay_bit = 1;
  src_sd->ops[ii] = NULL;
  src_sd->op_count--;

  ASSERT(exec->proc_id, event == FU_UNAVAILABLE || event == FU_MEM_UNAVAILABLE || event == FU_OTHER_UNAVAILABLE);
  STAT_EVENT(exec->proc_id, event);

  if (event == FU_OTHER_UNAVAILABLE)
    return;

  int simd_stat_base = op->table_info->is_simd ? FU_REJECTED_OP_INV_SIMD : FU_REJECTED_OP_INV_NOT_SIMD;
  STAT_EVENT(exec->proc_id, simd_stat_base + op->table_info->op_type);
}

static inline void exec_stage_clear_fu(int ii) {
  Op* op = exec->sd.ops[ii];

  /* TEA op completion: mark done, will be freed in node_retire() */
  if (op && TEA_ENABLE && op->thread_id == 1) {
    /* TEA memory ops: don't clear - let dcache_stage handle them for done_cycle */
    if (op->table_info->mem_type != NOT_MEM) {
      return;  /* Memory ops need dcache processing to set done_cycle */
    }

    /* TEA non-memory ops: clear from exec->sd.
     * 0-latency ops have OS_DONE already set by exec_stage_process_op() (called
     * in Phase 2 of the same cycle).  Latency>0 ops need tea_op_completed() here
     * (Phase 1 of N+avail, before node runs in same cycle). */
    if (op->state != OS_DONE) {
      DEBUG(exec->proc_id, "TEA op completed op_num:%s\n", unsstr64(op->op_num));
      tea_op_completed(exec->proc_id, op);
    }

    exec->sd.ops[ii] = NULL;
    exec->sd.op_count--;
    ASSERT(exec->proc_id, exec->sd.op_count >= 0);

    /* Don't free here - TEA ops are freed in node_retire() after removal from Node Table */
    return;
  }

  /* Main thread ops: normal clear */
  exec->sd.ops[ii] = NULL;
  exec->sd.op_count--;
  ASSERT(exec->proc_id, exec->sd.op_count >= 0);
}

static inline int exec_stage_check_fu_available(int ii) {
  /* check whether the functional unit is busy first */
  // if the FU is not available, then nullify node stage entry to make instruction get scheduled again
  Func_Unit* fu = &exec->fus[ii];
  if (cycle_count < fu->avail_cycle) {
    return FU_UNAVAILABLE;
  }

  /* if the functional unit is not busy, check the op assigned to it */
  // if no operation is occupying the FU, return that the FU is available
  Op* fop = exec->sd.ops[ii];
  if (!fop) {
    return 0;
  }

  // remove non-mem op currently in the FU
  if (!fop->table_info->mem_type) {
    return 0;
  }

  // need to kill it if it is a simultaneous replay
  if (fop->replay && fop->replay_cycle == cycle_count) {
    STAT_EVENT(exec->proc_id, FU_REPLAY);
    return 0;
  }

  // memory stall
  return FU_MEM_UNAVAILABLE;
}

static inline void exec_stage_process_op(Op* op) {
  // set the op's state to reflect it's execution
  if (op->table_info->mem_type == NOT_MEM || STALL_ON_WAIT_MEM) {
    op->state = OS_SCHEDULED;
  } else {
    // mem op may fail if it misses and can't get a mem req buffer
    op->state = OS_TENTATIVE;
  }

  op->exec_cycle = cycle_count + abs(op->inst_info->latency);
  op->exec_count++;
  if (op->table_info->mem_type == NOT_MEM)
    op->done_cycle = op->exec_cycle;

  STAT_EVENT(op->proc_id, EXEC_ON_PATH_INST + op->off_path);
  STAT_EVENT(op->proc_id, EXEC_ON_PATH_INST_MEM + (op->table_info->mem_type == NOT_MEM) + 2 * op->off_path);
  STAT_EVENT(op->proc_id, EXEC_ALL_INST);

  /* TEA non-mem 0-latency: complete immediately.
   * done_cycle == cycle_count means OP_DONE fires in the same cycle this op is
   * latched (Phase 2).  exec_stage_clear_fu runs in Phase 1 of the NEXT cycle,
   * which is after node has already retired (and freed) this op — use-after-free.
   * Fix: call tea_op_completed() here (Phase 2, op still live) so OS_DONE is set
   * before node runs.  exec_stage_clear_fu will see OS_DONE and skip the call. */
  if (TEA_ENABLE && op->thread_id == 1 &&
      op->table_info->mem_type == NOT_MEM &&
      op->inst_info->latency == 0) {
    tea_op_completed(op->proc_id, op);
  }

  DEBUG(exec->proc_id, "op_num:%s fu_num:%d exec_cycle:%s done_cycle:%s off_path:%d\n", unsstr64(op->op_num),
        op->fu_num, unsstr64(op->exec_cycle), unsstr64(op->done_cycle), op->off_path);
}

static inline void exec_stage_bp_resolve(Op* op) {
  /* TEA H2P branch: early misprediction detection and per-chain flush */
  if (TEA_ENABLE && op->thread_id == 1) {
    STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);

    if (!tea_is_active(op->proc_id))
      return;

    Tea_Thread* tea = tea_threads[op->proc_id];

    /* Identify which chain this op belongs to */
    int chain_slot = (int)op->h2p_chain_id - 1;  /* 1-based → 0-based */
    if (chain_slot < 0 || chain_slot >= MAX_TEA_CHAINS)
      return;

    Tea_H2P_Chain* c = &tea->chains[chain_slot];
    if (c->state == CHAIN_INACTIVE)
      return;  /* Chain already terminated; residual op finishing exec */

    /* Only the H2P branch (whose addr matches target_h2p_pc) triggers early flush */
    if (op->inst_info->addr != c->target_h2p_pc)
      return;

    if (op->oracle_info.mispred || op->oracle_info.misfetch) {
      DEBUG(op->proc_id, "TEA early flush: chain=%d H2P mispred op_num:%s\n",
            chain_slot, unsstr64(op->op_num));

      Op* main_h2p = c->main_h2p_op;

      /* Validate: main_h2p may have been retired/freed */
      if (!main_h2p || !main_h2p->op_pool_valid ||
          main_h2p->unique_num != c->saved_unique_num) {
        terminate_tea_chain(op->proc_id, chain_slot);
        return;
      }

      /* Guard: if Main H2P is off-path an earlier recovery handles it */
      if (main_h2p->off_path)
        return;

      if (!main_h2p->oracle_info.recovery_sch) {
        if (reg_file_checkpoint_is_valid()) {
          /* Case 2: SRT checkpoint present → schedule recovery at Main H2P */
          bp_sched_recovery(bp_recovery_info, main_h2p, op->exec_cycle,
                            FALSE, FALSE, EXTRA_LATE_RECOVERY_CYCLES);
          if (main_h2p->oracle_info.recovery_sch)
            main_h2p->recovery_scheduled = TRUE;
          main_h2p->oracle_info.recover_at_exec = FALSE;
          STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE2_WITH_CHKPT);
          /* cmp_recover() → recover_tea_on_flush(proc_id, recovery_op_num)
           * will terminate chains with target_h2p_op_num >= recovery_op_num. */
        } else {
          /* Case 1: No SRT checkpoint — terminate only this chain immediately */
          if (main_h2p->decode_cycle)
            STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_NO_CHKPT);
          else
            STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_DECODE);
          terminate_tea_chain(op->proc_id, chain_slot);
        }
        STAT_EVENT(op->proc_id, TEA_EARLY_FLUSHES);
      }
    } else {
      /* Correct prediction: TEA precomputation done, terminate this chain */
      STAT_EVENT(op->proc_id, TEA_H2P_CORRECT);
      terminate_tea_chain(op->proc_id, chain_slot);
    }
    return;
  }

  /* Main thread branch resolution */
  if (!BP_UPDATE_AT_RETIRE) {
    // this code updates the branch prediction structures
    if (op->table_info->cf_type >= CF_IBR)
      bp_target_known_op(g_bp_data, op);

    bp_resolve_op(g_bp_data, op);
  }

  if(op->oracle_info.recover_at_exec) { //(op->oracle_info.mispred || op->oracle_info.misfetch)
    bp_sched_recovery(bp_recovery_info, op, op->exec_cycle, FALSE, FALSE, EXTRA_LATE_RECOVERY_CYCLES);
    log_misprediction_detection_at_exec(op, node, cycle_count, bp_recovery_info); // 추가
    if (!op->off_path)
      op->recovery_scheduled = TRUE;

    // stats for the reason of resteer
    if (op->oracle_info.mispred)
      STAT_EVENT(op->proc_id, RESTEER_MISPRED_NOT_CF + op->table_info->cf_type);
    else
      STAT_EVENT(op->proc_id, RESTEER_MISFETCH_NOT_CF + op->table_info->cf_type);
  }

#if 0
  if (op->table_info->cf_type >= CF_IBR && op->oracle_info.no_target) {
    ASSERT(bp_recovery_info->proc_id, bp_recovery_info->proc_id == op->proc_id);
    bp_sched_redirect(bp_recovery_info, op, op->exec_cycle);
    // stats for the reason of resteer
    STAT_EVENT(op->proc_id, RESTEER_NO_TARGET_CF_IBR + op->table_info->cf_type - CF_IBR);
    ASSERT(0, 0);
  }
#endif
}
