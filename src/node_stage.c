/* Copyright 2020 HPS/SAFARI Research Groups
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
 * File         : node_stage.c
 * Author       : HPS Research Group
 * Date         : 1/28/1999
 * Description  :
 ***************************************************************************************/

#include "node_stage.h"

#include <unistd.h>

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
#include "memory/memory.param.h"

#include "bp/bp.h"
#include "bp/tagescl.h"
#include "frontend/frontend.h"
#include "memory/memory.h"

#include "decoupled_frontend.h"
#include "exec_ports.h"
#include "lsq.h"
#include "fill_buffer.h"
#include "bp/hbt.h"
#include "map.h"
#include "map_rename.h"
#include "node_issue_queue.h"
#include "op_pool.h"
#include "sim.h"
#include "statistics.h"
#include "thread.h"
#include "xed-iclass-enum.h"
#include "log/op_trace_log.h"

#include "log/fill_buffer_log.h"
#include "dependency_chain_cache.h"
#include "tea/tea_thread.h"
#include "tea/tea_rename.h"
#include "cmp_model.h"

/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_NODE_STAGE, ##args)
#define PRINT_RETIRED_UOP(proc_id, args...) _DEBUG_LEAN(proc_id, DEBUG_RETIRED_UOPS, ##args)

#define DEBUG_NODE_WIDTH ISSUE_WIDTH
#define OP_IS_IN_RS(op) (op->state >= OS_IN_RS && op->state < OS_SCHEDULED)

/**************************************************************************************/
/* Global Variables */

Node_Stage* node = NULL;
Rob_Stall_Reason rob_stall_reason = ROB_STALL_NONE;
Rob_Block_Issue_Reason rob_block_issue_reason = ROB_BLOCK_ISSUE_NONE;

/**************************************************************************************/
/* Prototypes */

void debug_print_retired_uop(Op* op);
void flush_ready_list(void);
void flush_scheduling_buffer(void);
void flush_rs(void);
void flush_window(void);
void debug_print_node_table(void);
void debug_print_rs(void);
void debug_print_ready_list(void);
Flag op_not_ready_for_retire(Op* op);
Flag is_node_table_empty(void);
void collect_not_ready_to_retire_stats(Op* op);
Flag is_node_table_full(void);
void collect_node_table_full_stats(Op* op);

void node_fill_rob(Stage_Data*);
void node_retire(void);

void node_precommit_update(void);
void node_precommit_retire(Op* op);

void node_fuse_op(Op* op);

/**************************************************************************************/
/* set_node_stage:*/

void set_node_stage(Node_Stage* new_node) {
  node = new_node;
}

/**************************************************************************************/
/* init_node_stage:*/

void init_node_stage(uns8 proc_id, const char* name) {
  ASSERT(proc_id, node);
  DEBUG(proc_id, "Initializing %s stage\n", name);

  node->proc_id = proc_id;
  node->sd.name = (char*)strdup(name);

  // allocate wires to functional units
  node->sd.max_op_count = NUM_FUS;  // Bandwidth between schedule and FUS
  node->sd.ops = (Op**)malloc(sizeof(Op*) * node->sd.max_op_count);

  reset_node_stage();
}

/**************************************************************************************/
/* reset_node_stage:*/

void reset_node_stage() {
  uns ii;
  for (ii = 0; ii < NUM_FUS; ii++)
    node->sd.ops[ii] = NULL;
  node->sd.op_count = 0;

  node->node_head = NULL;
  node->node_tail = NULL;
  node->rdy_head = NULL;
  node->next_op_into_rs = NULL;

  node->node_count = 0;
  node->ret_op = 1;
  node->last_scheduled_opnum = 0;
  node->mem_blocked = FALSE;
  node->mem_block_length = 0;
  node->ret_stall_length = 0;

  node->node_precommit = NULL;
  node->prev_op_fusable = FALSE;
}

/**************************************************************************************/
/* reset_node_stage:*/
// CMP used for bogus run: may be combined with reset_node_stage
void reset_all_ops_node_stage() {
  /* TODO: re-consider for multi-core mode */
  reset_node_stage();
}

/**************************************************************************************/
/* recover_node_stage:*/

void recover_node_stage() {
  ASSERT(node->proc_id, node->proc_id == bp_recovery_info->proc_id);

  DEBUG(node->proc_id, "Recovering '%s' stage\n", node->sd.name);
  if (ENABLE_GLOBAL_DEBUG_PRINT && DEBUG_NODE_STAGE && DEBUG_RANGE_COND(node->proc_id))
    debug_node_stage();

  flush_ready_list();
  flush_scheduling_buffer();
  flush_rs();
  flush_window();

  // recover last_scheduled_opnum
  if (node->last_scheduled_opnum >= bp_recovery_info->recovery_op_num)
    node->last_scheduled_opnum = bp_recovery_info->recovery_op_num;

  if (ENABLE_GLOBAL_DEBUG_PRINT && DEBUG_NODE_STAGE && DEBUG_RANGE_COND(node->proc_id))
    debug_node_stage();
}

void flush_ready_list() {
  Op* op;
  Op** last;
  for (op = node->rdy_head, last = &node->rdy_head; op; op = op->next_rdy) {
    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    /* Skip TEA ops — their ready list cleanup is handled by
     * flush_tea_ops_from_node_stage() step 1, which also synchronizes
     * RS counters for OS_SCHEDULED/OS_MISS TEA ops. FLUSH_OP() always
     * matches TEA ops (op_num >= 0x8000000000000000 > any recovery_op_num),
     * so without this skip, step 1 would find them already removed. */
    if (op->thread_id == 1) {
      last = &op->next_rdy;
      continue;
    }
    if (FLUSH_OP(op)) {
      ASSERT(node->proc_id, op->op_num > bp_recovery_info->recovery_op_num);
      *last = op->next_rdy;
      op->in_rdy_list = FALSE;
    } else
      last = &op->next_rdy;
  }
}

void flush_scheduling_buffer() {
  uns ii;
  for (ii = 0; ii < node->sd.max_op_count; ii++) {
    Op* op = node->sd.ops[ii];
    if (op && FLUSH_OP(op)) {
      ASSERT(node->proc_id, node->proc_id == op->proc_id);
      ASSERTM(node->proc_id, op->op_num > bp_recovery_info->recovery_op_num, "op_num:%s\n", unsstr64(op->op_num));

      node->sd.ops[ii] = NULL;
      node->sd.op_count--;

      ASSERT(node->proc_id, node->sd.op_count >= 0);
    }
  }
}

void flush_rs() {
  Op* op = node->next_op_into_rs;
  if (op && FLUSH_OP(op)) {
    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    ASSERTM(node->proc_id, op->op_num > bp_recovery_info->recovery_op_num, "op_num:%s\n", unsstr64(op->op_num));
    node->next_op_into_rs = NULL;  // all later ops will also be flushed
  }
}

void flush_window() {
  Op* op;
  Op** last;
  uns flush_ops = 0;
  uns keep_ops = 0;

  node->node_tail = NULL;
  for (op = node->node_head, last = &node->node_head; op; op = *last) {
    ASSERT(node->proc_id, node->proc_id == op->proc_id);

    /* Skip TEA ops - they are handled by flush_tea_ops_from_node_stage().
     * TEA ops use a different op_num counter (tea_op_counter), so FLUSH_OP()
     * comparison with recovery_op_num is not meaningful for them.
     * Also, TEA ops are not counted in node_count via macro_fused handling. */
    if (op->thread_id == 1) {
      last = &op->next_node;
      node->node_tail = op;
      continue;
    }

    if (FLUSH_OP(op)) {
      DEBUG(node->proc_id, "Node flushing  op:%s\n", unsstr64(op->op_num));
      if (!op->macro_fused)
        flush_ops++;
      ASSERT(node->proc_id, op->op_num > bp_recovery_info->recovery_op_num);
      op->in_node_list = FALSE;
      *last = op->next_node;
      if (op->state == OS_IN_RS || op->state == OS_READY || op->state == OS_WAIT_FWD) {
        ASSERT(op->proc_id, node->rs[op->rs_id].rs_op_count > 0);
        node->rs[op->rs_id].rs_op_count--;

        /* Phase 4: Update per-thread counter */
        if (op->thread_id == 1) {
          if (node->rs[op->rs_id].tea_op_count > 0)
            node->rs[op->rs_id].tea_op_count--;
        } else {
          if (node->rs[op->rs_id].main_op_count > 0)
            node->rs[op->rs_id].main_op_count--;
        }

        /* DEBUG: detect divergence */
        ASSERTM(op->proc_id,
                node->rs[op->rs_id].rs_op_count >= node->rs[op->rs_id].main_op_count + node->rs[op->rs_id].tea_op_count,
                "flush_window() divergence: rs=%d rs_op=%d main=%d tea=%d op_num=%s tid=%d C=%llu\n",
                (int)op->rs_id, node->rs[op->rs_id].rs_op_count,
                node->rs[op->rs_id].main_op_count, node->rs[op->rs_id].tea_op_count,
                unsstr64(op->op_num), op->thread_id, cycle_count);
      }
      free_op(op);
    } else {
      /* Keep op */

      if (IS_FLUSHING_OP(op)) {
        /* Mark that the scheduled recovery has occurred */
        op->recovery_scheduled = FALSE;
      }
      DEBUG(node->proc_id, "Node keeping  op:%s node_id:%llu\n", unsstr64(op->op_num), op->node_id);
      if (!op->macro_fused)
        keep_ops++;
      last = &op->next_node;
      node->node_tail = op;
    }
  }

  ASSERT(node->proc_id, flush_ops + keep_ops == node->node_count);
  node->node_count = keep_ops;
  ASSERT(node->proc_id, node->node_count <= NODE_TABLE_SIZE);
}

/**************************************************************************************/
/* debug_node_stage:*/

void debug_node_stage() {
  DPRINTF("# %-10s  node_count:%d\n", node->sd.name, node->node_count);

  debug_print_node_table();
  debug_print_rs();
  debug_print_ready_list();
}

void debug_print_node_table() {
  Op* op;

  Counter row = 0;
  Flag empty = TRUE;
  uns32 slot_num = 0;
  uns printed = 0;

  Op** temp = (Op**)calloc(DEBUG_NODE_WIDTH, sizeof(Op*));

  for (op = node->node_head; op; op = op->next_node, ++row) {
    slot_num = row % DEBUG_NODE_WIDTH;
    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    ASSERT(node->proc_id, temp[slot_num] == NULL);
    temp[slot_num] = op;
    printed++;
    empty = FALSE;

    // we have populated entire row, print and reinitialize
    if (slot_num == DEBUG_NODE_WIDTH - 1) {
      if (!empty) {
        print_open_op_array(GLOBAL_DEBUG_STREAM, temp, DEBUG_NODE_WIDTH, DEBUG_NODE_WIDTH);
      }
      // For some reason this does not zero out the entire array.
      // (Assert fails and verified in gdb).
      // memset(temp, 0, DEBUG_NODE_WIDTH * sizeof(Op*));
      for (int i = 0; i < DEBUG_NODE_WIDTH; i++)
        temp[i] = 0;
      empty = TRUE;
    }
  }

  ASSERTM(node->proc_id, printed == node->node_count, "printed=%d, node_count=%d", printed, node->node_count);

  // If node table is empty, print a blank row. Or if there is a remainder, print that too
  if (printed == 0 || slot_num < DEBUG_NODE_WIDTH - 1)
    print_open_op_array(GLOBAL_DEBUG_STREAM, temp, DEBUG_NODE_WIDTH, DEBUG_NODE_WIDTH);

  print_open_op_array_end(GLOBAL_DEBUG_STREAM, DEBUG_NODE_WIDTH);

  free(temp);
}

void debug_print_rs() {
  Op* op;
  uns printed = 0;
  int32 i, j;

  ASSERT(node->proc_id, node->rs);

  for (i = 0; i < NUM_RS; ++i) {
    Reservation_Station* rs = &node->rs[i];
    printed = 0;
    DPRINTF("%s (%d/%s): ", rs->name, rs->rs_op_count, rs->size == 0 ? "inf" : unsstr64((uns64)rs->size));

    for (j = 0; j < rs->num_fus; ++j) {
      DPRINTF("%s, ", rs->connected_fus[j]->name);
    }

    DPRINTF("\n");

    for (op = node->node_head; op && op != node->next_op_into_rs; op = op->next_node) {
      if (op->rs_id == i && OP_IS_IN_RS(op)) {
        // Op belongs to this RS
        DPRINTF("%lld ", op->op_num);
        printed++;
        if (printed % 8 == 0)
          DPRINTF("\n");
      }
    }

    if (printed % 8)
      DPRINTF("\n");

    ASSERTM(node->proc_id, printed == rs->rs_op_count, "printed=%d, rs_op_count=%d\n", printed, rs->rs_op_count);
  }
}

void debug_print_ready_list() {
  Op* op;

  DPRINTF("Ready list:");

  for (op = node->rdy_head; op; op = op->next_rdy) {
    DPRINTF(" %s", unsstr64(op->op_num));
  }

  DPRINTF("\n");

  print_op_array(GLOBAL_DEBUG_STREAM, node->sd.ops, node->sd.max_op_count, node->sd.max_op_count);
}

/**************************************************************************************/
/* tea_dispatch_to_rs: Dispatch TEA ops to Node Table for RS dispatch
 *   TEA ops use Node Table for issue/wakeup but skip retirement (commit bypass) */

static void tea_dispatch_to_rs(Stage_Data* tea_sd) {
  if (!tea_sd || tea_sd->op_count == 0) {
    return;
  }

  for (uns i = 0; i < tea_sd->max_op_count; i++) {
    Op* op = tea_sd->ops[i];
    if (!op) {
      continue;
    }

    ASSERT(node->proc_id, op->thread_id == 1);  /* Verify TEA op */
    ASSERT(node->proc_id, op->proc_id == node->proc_id);

    /* Check Node Table capacity */
    if (is_node_table_full()) {
      DEBUG(node->proc_id, "Node Table full, TEA op stalled op_num:%s\n",
            unsstr64(op->op_num));
      break;
    }

    /* Remove from TEA rename stage */
    tea_sd->ops[i] = NULL;
    tea_sd->op_count--;

    /* Set op fields (same as node_fill_rob) */
    op->node_id = node->node_count;
    op->issue_cycle = cycle_count;

    /* Add to Node Table linked list */
    ASSERT(node->proc_id, !op->in_node_list);
    if (node->node_tail) {
      node->node_tail->next_node = op;
    }
    if (node->node_head == NULL) {
      node->node_head = op;
    }
    op->next_node = NULL;
    op->in_node_list = TRUE;
    node->node_tail = op;

    /* Independent dispatch: do NOT set next_op_into_rs for TEA ops.
     * next_op_into_rs is Main-only, preventing TEA/Main ordering issues
     * (ASSERT 1&2 root cause fix). */

    /* TEA ops are NOT counted in node_count.
     * They are managed separately and flushed by flush_tea_ops_from_node_stage().
     * This avoids count mismatch in flush_window() ASSERT. */

    /* Direct RS dispatch: bypass node_issue_queue_dispatch() entirely */
    int64 rs_id = node_dispatch_find_emptiest_rs(op);
    if (rs_id != NODE_ISSUE_QUEUE_RS_SLOT_INVALID) {
      Reservation_Station* rs = &node->rs[rs_id];
      op->state = OS_IN_RS;
      op->rs_id = (Counter)rs_id;
      rs->rs_op_count++;
      rs->tea_op_count++;

      /* Register in ready list if all sources are ready */
      if (op->srcs_not_rdy_vector == 0) {
        op->state = (cycle_count + 1 >= op->rdy_cycle ? OS_READY : OS_WAIT_FWD);
        op->next_rdy = node->rdy_head;
        node->rdy_head = op;
        op->in_rdy_list = TRUE;
      }
    } else {
      /* TEA RS partition full — stay in Node Table as OS_IN_ROB.
       * tea_dispatch_retry() will retry after clear() frees RS slots. */
      op->state = OS_IN_ROB;
      STAT_EVENT(node->proc_id, TEA_RS_STALLS);
    }

    DEBUG(node->proc_id, "TEA op to Node Table  op_num:%s rs_id:%lld state:%d\n",
          unsstr64(op->op_num), (long long)rs_id, op->state);

    STAT_EVENT(node->proc_id, TEA_OPS_DISPATCHED);
  }
}

/**************************************************************************************/
/* tea_dispatch_retry: Retry RS dispatch for TEA ops that failed due to RS full.
 *   Called after node_issue_queue_update() so clear() has freed RS slots. */

static void tea_dispatch_retry() {
  for (Op* op = node->node_head; op; op = op->next_node) {
    if (op->thread_id != 1 || op->state != OS_IN_ROB)
      continue;

    int64 rs_id = node_dispatch_find_emptiest_rs(op);
    if (rs_id == NODE_ISSUE_QUEUE_RS_SLOT_INVALID) {
      STAT_EVENT(node->proc_id, TEA_RS_STALLS);
      continue;  /* Still full, try next TEA op */
    }

    Reservation_Station* rs = &node->rs[rs_id];
    op->state = OS_IN_RS;
    op->rs_id = (Counter)rs_id;
    rs->rs_op_count++;
    rs->tea_op_count++;

    if (op->srcs_not_rdy_vector == 0) {
      op->state = (cycle_count + 1 >= op->rdy_cycle ? OS_READY : OS_WAIT_FWD);
      op->next_rdy = node->rdy_head;
      node->rdy_head = op;
      op->in_rdy_list = TRUE;
    }
  }
}

/**************************************************************************************/
/* node_cycle: */

void update_node_stage(Stage_Data* src_sd) {
  Op* op;
  // Update state for all completed ops
  for(op = node->node_head; op; op = op->next_node) {
    if(OP_DONE(op) && op->state != OS_DONE) {
      op->state = OS_DONE;
    }
  }
  DEBUG(node->proc_id, "Beginning '%s' stage\n", node->sd.name);
  STAT_EVENT(node->proc_id, NODE_CYCLE);
  STAT_EVENT(node->proc_id, POWER_CYCLE);

  /* TEA ops: Dispatch to RS first (bypassing ROB) */
  if (TEA_ENABLE && tea_is_active(node->proc_id)) {
    tea_dispatch_to_rs(&tea_rename_stages[node->proc_id]->sd);
  }

  /* insert ops coming from the previous stage*/
  node_fill_rob(src_sd);

  /* update the precommit pointer in the ROB */
  node_precommit_update();

  node_issue_queue_update();

  /* TEA RS dispatch retry: previous cycle's RS-full TEA ops get another chance
   * after clear() has freed RS slots. */
  if (TEA_ENABLE && tea_is_active(node->proc_id)) {
    tea_dispatch_retry();
  }

  /* get rid of the ops that are finished */
  node_retire();

  memview_core_stall(node->proc_id, is_node_stage_stalled(), node->mem_blocked);
}

/**************************************************************************************/
/* node_fill_rob: This function takes ops from the map stage and allocates them into the node table.
 *    Note, this function does not place the Op in the RS, that is done later.*/

void node_fill_rob(Stage_Data* src_sd) {
  Flag on_path = FALSE;
  uns ii;

  /* if nothing to process, return */
  if (src_sd->op_count == 0)
    return;

  // Go through all the ops in the issue buffer and stick them into the Node Table.
  // We will stick them into the RS later
  for (ii = 0; ii < src_sd->max_op_count; ii++) {
    /* if node table is full, stall */
    if (is_node_table_full()) {
      collect_node_table_full_stats(node->node_head);
      rob_block_issue_reason = ROB_BLOCK_ISSUE_FULL;
      return;
    }
    rob_block_issue_reason = ROB_BLOCK_ISSUE_NONE;

    // If it is not full, issue the next op
    Op* op = src_sd->ops[ii];
    if (!op)
      continue;

    if (op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_ST) {
      if (!lsq_available(op)) {
        STAT_EVENT(op->proc_id, LSQ_FULL_TOTAL);
        STAT_EVENT(op->proc_id, LSQ_FULL_TOTAL + op->table_info->mem_type);
        return;
      }

      lsq_dispatch(op);
    }

    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    /* check if it's a synchronizing op that can't issue  */
    if ((op->table_info->bar_type & BAR_ISSUE) && (node->node_count > 0))
      break;

    /* remove op from previous stage */
    src_sd->ops[ii] = NULL;
    src_sd->op_count--;
    ASSERT(node->proc_id, src_sd->op_count >= 0);

    /* set op fields */
    op->node_id = node->node_count;
    op->issue_cycle = cycle_count;

    /* add to node list & update node state*/
    ASSERT(node->proc_id, !op->in_node_list);
    if (node->node_tail)
      node->node_tail->next_node = op;
    if (node->node_head == NULL)
      node->node_head = op;
    op->next_node = NULL;
    op->in_node_list = TRUE;
    node->node_tail = op;

    if (!node->next_op_into_rs)   /* if there are no ops waiting to enter RS */
      node->next_op_into_rs = op; /* this will be the first one */

    // Jump uop after CMP or TEST will be fused into one uop
    node_fuse_op(op);
    if (!op->macro_fused)
      node->node_count++;

    ASSERTM(node->proc_id, node->node_count <= NODE_TABLE_SIZE,
            "node_count: %d src_max_op_count: %d src_op_count: %d\n", node->node_count, src_sd->max_op_count,
            src_sd->op_count);

    on_path |= !op->off_path;

    DEBUG(node->proc_id, "Issuing the op op_num:%s off_path:%d\n", unsstr64(op->op_num), op->off_path);

    op->state = OS_IN_ROB;
    
    log_fill_rob_op(op, cycle_count);

    /* always stop issuing after a synchronizing op */
    if (op->table_info->bar_type & BAR_ISSUE)
      break;
  }
}

/**************************************************************************************/
/* node_retire_tea_ops: Remove completed TEA ops from Node Table
 *   TEA ops skip normal retirement - just remove from list and free */

static void node_retire_tea_ops() {
  Op** last = &node->node_head;
  Op* op = node->node_head;
  Op* new_tail = NULL;

  while (op) {
    /* Retirement gate differs by op type:
     *   mem ops  — OS_DONE only (set by tea_op_completed() called from dcache_stage).
     *              OP_DONE would fire based on exec address-calc latency, one cycle
     *              BEFORE dcache runs and calls tea_op_completed(), causing free_op()
     *              to race ahead and produce a dangling pointer in exec->sd / a ghost
     *              count in tea->tea_op_count.
     *   non-mem  — OP_DONE || OS_DONE (exec sets done_cycle AND calls tea_op_completed
     *              in the same cycle before node runs, so either gate is safe). */
    Flag tea_done;
    if (op->thread_id == 1) {
      if (op->table_info->mem_type != NOT_MEM) {
        tea_done = (op->state == OS_DONE);   /* mem: dcache must have called tea_op_completed */
      } else {
        tea_done = (OP_DONE(op) || op->state == OS_DONE);  /* non-mem: exec latency sufficient */
      }
    } else {
      tea_done = FALSE;
    }
    if (tea_done) {
      /* TEA op done: Remove from Node Table linked list.
       * Note: TEA ops are NOT counted in node_count (not incremented on dispatch,
       * not decremented on retire).
       * recovery_op is always a Main H2P op (never a TEA op), so no guard needed. */

      *last = op->next_node;
      op->in_node_list = FALSE;

      /* Decrement tea_op_count: this is the authoritative decrement point.
       * tea_op_completed() (called from exec/dcache) only sets OS_DONE and
       * records EXECUTED — it no longer decrements the counter.  Moving the
       * decrement here ensures that 0-latency non-mem ops (done_cycle ==
       * issue_cycle, so OP_DONE fires in the same cycle as issuance while
       * exec_stage_clear_fu cannot run until avail_cycle = issue+1) are
       * counted correctly.  Every op that exits via this path decrements
       * exactly once; ops that exit via flush have tea_op_count reset to 0
       * by terminate_tea_thread(). */
      if (tea_threads && tea_threads[node->proc_id]) {
        Tea_Thread* tea_state = tea_threads[node->proc_id];
        int slot = (int)op->h2p_chain_id - 1;  /* 1-based → 0-based */
        if (slot >= 0 && slot < MAX_TEA_CHAINS &&
            tea_state->chains[slot].tea_op_count > 0) {
          tea_state->chains[slot].tea_op_count--;
        }
      }

      DEBUG(node->proc_id, "TEA op retired op_num:%s\n", unsstr64(op->op_num));
      STAT_EVENT(node->proc_id, TEA_OPS_RETIRED);

      /* Record Node residence time: issue_cycle is set on dispatch */
      if (op->issue_cycle > 0) {
        Counter residence = cycle_count - op->issue_cycle;
        INC_STAT_EVENT(node->proc_id, TEA_OP_NODE_CYCLES_TOTAL, residence);
        if      (residence < 10)  STAT_EVENT(node->proc_id, TEA_OP_NODE_CYCLES_0);
        else if (residence < 50)  STAT_EVENT(node->proc_id, TEA_OP_NODE_CYCLES_10);
        else if (residence < 100) STAT_EVENT(node->proc_id, TEA_OP_NODE_CYCLES_50);
        else if (residence < 500) STAT_EVENT(node->proc_id, TEA_OP_NODE_CYCLES_100);
        else                      STAT_EVENT(node->proc_id, TEA_OP_NODE_CYCLES_500);
      }

      /* DEBUG: detect if op is still in rdy_head when being retired.
       * If in_rdy_list is TRUE here, clear() missed it (OS_DONE not in clear() condition)
       * and RS counters (rs_op_count, tea_op_count) were never decremented → RS leak. */
      ASSERTM(0, !op->in_rdy_list,
              "retire_tea: op still in rdy_head! op_num=%s state=%d rs=%d "
              "rs_op=%d main=%d tea=%d C=%llu\n",
              unsstr64(op->op_num), op->state, (int)op->rs_id,
              node->rs[op->rs_id].rs_op_count,
              node->rs[op->rs_id].main_op_count,
              node->rs[op->rs_id].tea_op_count,
              (unsigned long long)cycle_count);

      Op* next = op->next_node;
      free_op(op);
      op = next;
      continue;
    }
    /* Keep this op in list */
    new_tail = op;
    last = &op->next_node;
    op = op->next_node;
  }

  /* Update node_tail */
  node->node_tail = new_tail;
}

/**************************************************************************************/
/* node_retire:*/

void node_retire() {
  uns ret_count = 0;

  // If node table is empty, then there is nothing to retire
  if (is_node_table_empty())
    return;

  /* First pass: Remove completed TEA ops (they skip normal retirement) */
  if (TEA_ENABLE) {
    node_retire_tea_ops();
  }

  /* Second pass: Retire Main thread ops while maintaining linked list integrity.
   * TEA ops stay in the list (they don't participate in in-order retirement).
   * We use pointer-to-pointer to properly remove retired ops from linked list.
   */
  Op** prev_next_ptr = &node->node_head;
  Op* op = node->node_head;
  Op* last_remaining = NULL;

  while (op && ret_count < NODE_RET_WIDTH) {
    Op* next = op->next_node;

    /* Skip TEA ops - they stay in linked list, don't participate in retirement */
    if (op->thread_id == 1) {
      last_remaining = op;
      prev_next_ptr = &op->next_node;
      op = next;
      continue;
    }
    ASSERT(node->proc_id, node->proc_id == op->proc_id);

    // check to see if the head of the node table is ready to retire
    if (op_not_ready_for_retire(op)) {
      // op is not ready to retire
      collect_not_ready_to_retire_stats(op);
      last_remaining = op;  /* This op stays in list */
      break;
    }

    rob_stall_reason = ROB_STALL_NONE;

    /**op is ready to retire**/
    ASSERTM(node->proc_id, op->state != OS_TENTATIVE, "op_num: %llu\n", op->op_num);
    ret_count++;
    DEBUG(node->proc_id, "Retiring op:%llu\n", op->op_num);

    // Debug prints mainly used for testing the uop generation of PIN frontend
    debug_print_retired_uop(op);

    // count number of stall cycles
    STAT_EVENT(node->proc_id, RET_STALL_LENGTH_0 + MIN2(node->ret_stall_length, 5000) / 100);
    if (DIE_ON_RET_STALL_THRESH) {
      // time out code
      if (node->proc_id == DIE_ON_RET_STALL_CORE) {
        ASSERTM(node->proc_id, node->ret_stall_length < DIE_ON_RET_STALL_THRESH,
                "Retire stalled for %u cycles (%llu--%llu)\n", node->ret_stall_length,
                cycle_count - node->ret_stall_length, cycle_count);
      }
    }
    node->ret_stall_length = 0;

    // retire the ops
    Counter real_rdy_cycle = MAX2(op->rdy_cycle, op->issue_cycle);

    ASSERT(node->proc_id, node->proc_id == op->proc_id);
    ASSERT(node->proc_id, op->in_node_list);
    ASSERT(node->proc_id, !op->off_path);
    STAT_EVENT(op->proc_id, OP_WAIT_0 + MIN2(op->sched_cycle - real_rdy_cycle, 31));
    STAT_EVENT(op->proc_id, OP_RETIRED);  // Counts all ops retired, not just those in primary thread

    DEBUG(node->proc_id, "Retiring op_num:%s\n", unsstr64(op->op_num));

    ASSERTM(node->proc_id, op->op_num == node->ret_op, "op_num=%s  ret_op=%s\n", unsstr64(op->op_num),
            unsstr64(node->ret_op));

    if (op->eom) {
      /* We need to retire sys calls, bar fetch instructions, and the last instruction.
       * All other retires are "optional" to release resources in the PIN frontend */
      inst_count[node->proc_id]++;
      STAT_EVENT(op->proc_id, NODE_INST_COUNT);

      if (op->fetched_instruction) {
        inst_count_fetched[node->proc_id]++;
        STAT_EVENT(op->proc_id, NODE_INST_COUNT_FETCHED);
      }

      Flag retire_op = IS_CALLSYS(op->table_info) || op->table_info->bar_type & BAR_FETCH ||
                       (inst_count[node->proc_id] % NODE_RETIRE_RATE == 0);

      if (op->exit) {
        retired_exit[op->proc_id] = TRUE;
        decoupled_fe_retire(op, op->proc_id, -1);
      } else if (retire_op) {
        decoupled_fe_retire(op, op->proc_id, op->inst_uid);
      }
    }
    uop_count[node->proc_id]++;
    STAT_EVENT(op->proc_id, NODE_UOP_COUNT);
    ASSERTM(node->proc_id, uop_count[node->proc_id] == node->ret_op, "%s  %s op_num: %s\n",
            unsstr64(uop_count[node->proc_id]), unsstr64(node->ret_op), unsstr64(op->op_num));

    node->ret_op++;

    STAT_EVENT(op->proc_id, RET_ALL_INST);

    remove_from_seq_op_list(td, op);

    if (op->table_info->cf_type) {
      if (BP_UPDATE_AT_RETIRE) {
        // this code updates the branch prediction structures
        if (op->table_info->cf_type >= CF_IBR)
          bp_target_known_op(g_bp_data, op);

        bp_resolve_op(g_bp_data, op);
      }
      bp_retire_op(g_bp_data, op);
    }

    if (op->table_info->mem_type == MEM_LD && (op->done_cycle - op->sched_cycle) < 5) {
      STAT_EVENT(op->proc_id, LD_EXEC_CYCLES_0 + (op->done_cycle - op->sched_cycle));
    }
    if (op->table_info->mem_type == MEM_LD) {
      STAT_EVENT(op->proc_id, LD_NO_DEPENDENTS + (op->wake_up_head ? 1 : 0));
    }
    STAT_EVENT(op->proc_id, RET_OP_EXEC_COUNT_0 + MIN2(32, op->exec_count));

    op->retire_cycle = cycle_count;

    op->oracle_info.hbt_pred_is_hard = hbt_is_hard_branch(op->inst_info->addr);
    op->oracle_info.hbt_misp_counter = hbt_get_counter(op->inst_info->addr);
    fill_buffer_add(op->proc_id, op);

    // free the previous register entries with same architectural destination
    reg_file_commit(op);

    node_precommit_retire(op);

    if (op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_ST) {
      lsq_commit(op);
    }

    // the fused op does not occupy the ROB entry
    if (!op->macro_fused)
      node->node_count--;

    ASSERT(node->proc_id, node->node_count >= 0);

    /* Remove retired op from linked list */
    *prev_next_ptr = next;
    op->in_node_list = FALSE;
    /* Note: prev_next_ptr is NOT updated since this op is removed */

    if (model->op_retired_hook)
      model->op_retired_hook(op);
    else
      free_op(op);

    op = next;
  }

  if (ret_count > 0) {
    Fill_Buffer* fb = retired_fill_buffers[node->proc_id];
    Backward_Walk_Engine* engine = bw_engines[node->proc_id];
    
    if (fb && fb->count == fb->size && engine->state == BW_IDLE) {
        log_fill_buffer_entry(node->proc_id, fb, cycle_count);
        // --- Snapshot 저장 ---
        engine->state = BW_WALKING;
        engine->walk_cycles_remaining = BACKWARD_WALK_CYCLES;

        engine->snapshot_op_count = fb->count;
        int current_idx = fb->head;
        for (int i = 0; i < fb->count; ++i) {
            engine->snapshot_buffer[i] = fb->entries[current_idx];
            current_idx = (current_idx + 1) % fb->size;
        }
    }
  }

  STAT_EVENT(node->proc_id, ROW_SIZE_0 + ret_count);
  log_retired_ops(cycle_count, ret_count);

  /* node->node_head is already updated through *prev_next_ptr during the loop.
   * Update node_tail based on remaining ops in the linked list.
   */
  if (node->node_head == NULL) {
    node->node_tail = NULL;
    ASSERTM(node->proc_id, node->node_count == 0, "Node table must be empty if head is null!\n");
  } else {
    /* Find the new tail by walking the list */
    Op* tail = node->node_head;
    while (tail->next_node) {
      tail = tail->next_node;
    }
    node->node_tail = tail;
    DEBUG(node->proc_id, "Op op_num:%s is now head of the node table\n", unsstr64(node->node_head->op_num));
  }
}

/**************************************************************************************/
/* is_node_stage_stalled: returns TRUE if node table is full and there are no
 * ready ops */

Flag is_node_stage_stalled() {
  return (node->node_count == NODE_TABLE_SIZE) && /* node table is full */
         !node->rdy_head &&                       /* no ready ops */
         !node->next_op_into_rs;                  /* no ops waiting to enter RS */
}

void debug_print_retired_uop(Op* op) {
  PRINT_RETIRED_UOP(node->proc_id, "============================\n");
  PRINT_RETIRED_UOP(node->proc_id, "EIP: 0x%llx\n", op->inst_info->addr);
  PRINT_RETIRED_UOP(node->proc_id, "Op Type: %s\n", Op_Type_str(op->table_info->op_type));
  PRINT_RETIRED_UOP(node->proc_id, "Mem Type: %d\n", op->table_info->mem_type);
  PRINT_RETIRED_UOP(node->proc_id, "CF Type: %d\n", op->table_info->cf_type);
  PRINT_RETIRED_UOP(node->proc_id, "Barrier Type: %d\n", op->table_info->bar_type);
  PRINT_RETIRED_UOP(node->proc_id, "Is SIMD: %d\n", op->table_info->is_simd);
  PRINT_RETIRED_UOP(node->proc_id, "Srcs: ");
  for (uns i = 0; i < op->table_info->num_src_regs; ++i) {
    PRINT_RETIRED_UOP(node->proc_id, "%s ", disasm_reg(op->inst_info->srcs[i].id));
  }
  PRINT_RETIRED_UOP(node->proc_id, "\n");
  PRINT_RETIRED_UOP(node->proc_id, "Dests: ");
  for (uns i = 0; i < op->table_info->num_dest_regs; ++i) {
    PRINT_RETIRED_UOP(node->proc_id, "%s ", disasm_reg(op->inst_info->dests[i].id));
  }
  PRINT_RETIRED_UOP(node->proc_id, "\n");
}

Flag op_not_ready_for_retire(Op* op) {
  Flag not_done   = !(op->state == OS_DONE || OP_DONE(op));
  Flag blocked    = op->off_path || op->recovery_scheduled || op->redirect_scheduled;
  Flag result     = not_done || blocked;
  return result;
}

Flag is_node_table_empty() {
  if (node->node_count == 0) {
    if (node->node_head != NULL) {
      /* macro_fused ops and TEA ops (thread_id==1) are not counted in node_count */
      ASSERT(node->proc_id, node->node_head->macro_fused || node->node_head->thread_id == 1);
      return FALSE;
    }

    ASSERT(node->proc_id, node->node_head == NULL);
    ASSERT(node->proc_id, node->node_tail == NULL);
    return TRUE;
  }

  ASSERT(node->proc_id, node->node_head != NULL);
  ASSERT(node->proc_id, node->node_tail != NULL);
  return FALSE;
}

void collect_not_ready_to_retire_stats(Op* op) {
  rob_stall_reason = ROB_STALL_OTHER;
  if (op->recovery_scheduled) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_RECOVERY;
  } else if (op->redirect_scheduled) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_REDIRECT;
  }

  if (op->engine_info.l1_miss) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_L1_MISS;
    STAT_EVENT(op->proc_id, RET_BLOCKED_L1_MISS);
    Flag bw_prefetch = !op->engine_info.l1_miss_satisfied &&  // op->req is OK to use
                       op->req->demand_match_prefetch && op->req->bw_prefetch;
    Flag bw_prefetchable = !op->engine_info.l1_miss_satisfied &&  // op->req is OK to use
                           !op->req->demand_match_prefetch && op->req->bw_prefetchable;
    if (bw_prefetch || bw_prefetchable)
      STAT_EVENT(op->proc_id, RET_BLOCKED_L1_MISS_BW_PREF);
  }

  if (op->engine_info.l1_miss || op->state == OS_WAIT_MEM) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_MEMORY;
    STAT_EVENT(op->proc_id, RET_BLOCKED_MEM_STALL);
    if (num_offchip_stall_reqs(op->proc_id) > 0) {
      STAT_EVENT(op->proc_id, RET_BLOCKED_OFFCHIP_DEMAND);
    }
  }

  if (op->engine_info.dcmiss) {
    rob_stall_reason = ROB_STALL_WAIT_FOR_DC_MISS;
    STAT_EVENT(op->proc_id, RET_BLOCKED_DC_MISS);
    if (!op->engine_info.l1_miss)
      STAT_EVENT(op->proc_id, RET_BLOCKED_L1_ACCESS);
  }

  node->ret_stall_length++;
}

Flag is_node_table_full() {
  ASSERT(node->proc_id, node->node_count <= NODE_TABLE_SIZE);
  return (node->node_count == NODE_TABLE_SIZE);
}

void collect_node_table_full_stats(Op* op) {
  if (!(op->state == OS_DONE || OP_DONE(op))) {
    if (op->table_info->op_type == OP_ILD || op->table_info->op_type == OP_IST || op->table_info->op_type == OP_FLD ||
        op->table_info->op_type == OP_FST) {
      STAT_EVENT(node->proc_id, FULL_WINDOW_MEM_OP);
    } else if (op->table_info->op_type >= OP_FCVT && op->table_info->op_type <= OP_FCMOV) {
      STAT_EVENT(node->proc_id, FULL_WINDOW_FP_OP);
    } else {
      STAT_EVENT(node->proc_id, FULL_WINDOW_OTHER_OP);
    }
  }

  STAT_EVENT(node->proc_id, FULL_WINDOW_STALL);
}

/**************************************************************************************/
/* node precommit mechanism */

void node_precommit_update(void) {
  Op* op = node->node_head;
  if (node->node_precommit)
    op = node->node_precommit;

  // scan the node table to update the precommit pointer
  uns precommit_count = 0;
  for (; op != NULL && precommit_count < NODE_RET_WIDTH; op = op->next_node) {
    /* Skip TEA ops - they don't participate in precommit/retirement */
    if (op->thread_id == 1)
      continue;

    // wait until the results usable for branches
    if (op->table_info->cf_type && op->exec_cycle > cycle_count)
      return;

    // wait until looking up the d-cache for memory operands
    if ((op->table_info->mem_type == MEM_LD || op->table_info->mem_type == MEM_ST) && op->dcache_cycle > cycle_count)
      return;

    if (op->off_path)
      return;

    // avoid multiple precommit for the precommit head
    if (op->precommitted)
      continue;

    precommit_count++;
    node->node_precommit = op;
    op->precommitted = TRUE;
    op->precommit_cycle = cycle_count;

    reg_file_precommit(op);
  }
}

void node_precommit_retire(Op* op) {
  ASSERT(node->proc_id, op->precommitted);
  ASSERT(node->proc_id, op->precommit_cycle <= op->retire_cycle);

  if (!node->node_precommit)
    return;
  ASSERT(node->proc_id, node->node_precommit->op_num >= op->op_num);

  /* clear the precommit pointer when it commits, which indicates that
   * the ROB is empty or all in-flight ops are off-path */
  if (node->node_precommit->op_num != op->op_num)
    return;
  node->node_precommit = NULL;
}

/* Marcro-Fusion op */
void node_fuse_op(Op* op) {
  uns16 op_code = op->inst_info->table_info->true_op_type;

  if (op_code == XED_ICLASS_CMP || op_code == XED_ICLASS_TEST) {
    node->prev_op_fusable = TRUE;
    return;
  }

  if (op_code >= XED_ICLASS_JB && op_code <= XED_ICLASS_JZ) {
    if (node->prev_op_fusable) {
      op->macro_fused = TRUE;
      STAT_EVENT(op->proc_id, OP_MACRO_FUSION_ONPATH + op->off_path);
    }
  }

  node->prev_op_fusable = FALSE;
}

/**************************************************************************************/
/* flush_tea_ops_from_node_stage: CRITICAL - Remove all orphan TEA ops on termination */

void flush_tea_ops_from_node_stage(uns proc_id) {
  extern Cmp_Model cmp_model;
  Node_Stage* node_local = &cmp_model.node_stage[proc_id];
  Op* op;
  Op** last;

  /* 0a. Clear exec_stage->sd.ops[] FIRST - prevent use-after-free when ops are freed below */
  Exec_Stage* exec_local = &cmp_model.exec_stage[proc_id];
  for (uns ii = 0; ii < exec_local->sd.max_op_count; ii++) {
    op = exec_local->sd.ops[ii];
    if (op && op->thread_id == 1) {
      exec_local->sd.ops[ii] = NULL;
      exec_local->sd.op_count--;
      /* Don't free here - will be freed from node table below */
    }
  }

  /* 0b. Clear dcache_stage->sd.ops[] FIRST - prevent use-after-free when ops are freed below */
  Dcache_Stage* dc_local = &cmp_model.dcache_stage[proc_id];
  for (uns ii = 0; ii < dc_local->sd.max_op_count; ii++) {
    op = dc_local->sd.ops[ii];
    if (op && op->thread_id == 1) {
      dc_local->sd.ops[ii] = NULL;
      dc_local->sd.op_count--;
      /* Don't free here - will be freed from node table below */
    }
  }

  /* 1. Flush ready list — also decrement RS counter for OS_SCHEDULED/OS_MISS TEA ops.
   *    If found in ready list → clear() has NOT yet processed them (same-cycle case).
   *    In cross-cycle case, clear() already removed them → not found here → safe. */
  for (op = node_local->rdy_head, last = &node_local->rdy_head; op;) {
    if (op->thread_id == 1) {
      /* Remove TEA op from ready list */
      *last = op->next_rdy;
      op->in_rdy_list = FALSE;

      /* RS counter decrement for OS_SCHEDULED/OS_MISS:
       * Normally clear() does this, but we just removed op from ready list
       * so clear() will never see it. Must decrement here to prevent leak. */
      if (op->state == OS_SCHEDULED || op->state == OS_MISS) {
        if (node_local->rs[op->rs_id].rs_op_count > 0)
          node_local->rs[op->rs_id].rs_op_count--;
        if (node_local->rs[op->rs_id].tea_op_count > 0)
          node_local->rs[op->rs_id].tea_op_count--;

        /* DEBUG: detect divergence */
        ASSERTM(0,
                node_local->rs[op->rs_id].rs_op_count >= node_local->rs[op->rs_id].main_op_count + node_local->rs[op->rs_id].tea_op_count,
                "flush_tea step1 divergence: rs=%d rs_op=%d main=%d tea=%d op_num=%s state=%d C=%llu\n",
                (int)op->rs_id, node_local->rs[op->rs_id].rs_op_count,
                node_local->rs[op->rs_id].main_op_count, node_local->rs[op->rs_id].tea_op_count,
                unsstr64(op->op_num), op->state, cycle_count);
      }

      op = op->next_rdy;
      /* Don't free here - will be freed from node table below */
    } else {
      last = &op->next_rdy;
      op = op->next_rdy;
    }
  }

  /* 2. Flush scheduling buffer */
  for (uns ii = 0; ii < node_local->sd.max_op_count; ii++) {
    op = node_local->sd.ops[ii];
    if (op && op->thread_id == 1) {
      node_local->sd.ops[ii] = NULL;
      node_local->sd.op_count--;
      /* Don't free here - will be freed from node table below */
    }
  }

  /* 3. Flush next_op_into_rs — if it points to a TEA op, advance to
   * the next non-TEA op so main ops can still dispatch. */
  op = node_local->next_op_into_rs;
  if (op && op->thread_id == 1) {
    Op* next = op->next_node;
    while (next && next->thread_id == 1)
      next = next->next_node;
    node_local->next_op_into_rs = next;  /* NULL if no main ops follow */
    /* Don't free here - will be freed from node table below */
  }

  /* 4. Flush node table (window) - TEA ops bypass ROB but check for safety */
  node_local->node_tail = NULL;
  for (op = node_local->node_head, last = &node_local->node_head; op;) {
    if (op->thread_id == 1) {
      /* Remove TEA op from node table */
      *last = op->next_node;
      op->in_node_list = FALSE;

      /* Update RS count: only for pre-scheduling states where clear() has
       * NOT yet decremented the counter. After scheduling, clear() already
       * decremented for OS_SCHEDULED/OS_MISS, and post-scheduling states
       * (OS_WAIT_DCACHE, OS_WAIT_MEM, OS_DONE, etc.) also don't need
       * decrement. Use whitelist matching flush_window(). */
      if (op->state == OS_IN_RS || op->state == OS_READY ||
          op->state == OS_WAIT_FWD || op->state == OS_SLEEP ||
          op->state == OS_LOW_PRIORITY || op->state == OS_TENTATIVE) {
        if (node_local->rs[op->rs_id].rs_op_count > 0) {
          node_local->rs[op->rs_id].rs_op_count--;
        }
        if (node_local->rs[op->rs_id].tea_op_count > 0) {
          node_local->rs[op->rs_id].tea_op_count--;
        }

        /* DEBUG: detect divergence */
        ASSERTM(0,
                node_local->rs[op->rs_id].rs_op_count >= node_local->rs[op->rs_id].main_op_count + node_local->rs[op->rs_id].tea_op_count,
                "flush_tea step4 divergence: rs=%d rs_op=%d main=%d tea=%d op_num=%s state=%d C=%llu\n",
                (int)op->rs_id, node_local->rs[op->rs_id].rs_op_count,
                node_local->rs[op->rs_id].main_op_count, node_local->rs[op->rs_id].tea_op_count,
                unsstr64(op->op_num), op->state, cycle_count);
      }

      Op* next = op->next_node;
      free_op(op);
      STAT_EVENT(proc_id, TEA_OPS_FLUSHED);
      op = next;
    } else {
      /* Keep main thread op */
      last = &op->next_node;
      node_local->node_tail = op;
      op = op->next_node;
    }
  }
}

/**************************************************************************************/
/* flush_tea_ops_by_chain_id: Selective flush for one TEA chain */

void flush_tea_ops_by_chain_id(uns proc_id, uns8 chain_id) {
  extern Cmp_Model cmp_model;
  Node_Stage* node_local = &cmp_model.node_stage[proc_id];
  Op* op;
  Op** last;

  /* 0a. exec_stage: remove ops from this chain */
  Exec_Stage* exec_local = &cmp_model.exec_stage[proc_id];
  for (uns ii = 0; ii < exec_local->sd.max_op_count; ii++) {
    op = exec_local->sd.ops[ii];
    if (op && op->h2p_chain_id == chain_id) {
      exec_local->sd.ops[ii] = NULL;
      exec_local->sd.op_count--;
    }
  }

  /* 0b. dcache_stage: remove ops from this chain */
  Dcache_Stage* dc_local = &cmp_model.dcache_stage[proc_id];
  for (uns ii = 0; ii < dc_local->sd.max_op_count; ii++) {
    op = dc_local->sd.ops[ii];
    if (op && op->h2p_chain_id == chain_id) {
      dc_local->sd.ops[ii] = NULL;
      dc_local->sd.op_count--;
    }
  }

  /* 1. Ready list: remove this chain's ops and sync RS counters */
  for (op = node_local->rdy_head, last = &node_local->rdy_head; op;) {
    if (op->h2p_chain_id == chain_id) {
      *last = op->next_rdy;
      op->in_rdy_list = FALSE;
      if (op->state == OS_SCHEDULED || op->state == OS_MISS) {
        if (node_local->rs[op->rs_id].rs_op_count > 0)
          node_local->rs[op->rs_id].rs_op_count--;
        if (node_local->rs[op->rs_id].tea_op_count > 0)
          node_local->rs[op->rs_id].tea_op_count--;
      } else {
        /* DEBUG: log unexpected TEA op states in rdy_head during flush.
         * OS_DONE here means node_retire_tea_ops() freed this op without
         * removing it from rdy_head first — confirms RS counter leak path. */
        _DEBUG(proc_id, DEBUG_TEA,
               "flush_by_chain step1: unexpected state op_num=%s state=%d "
               "rs=%d rs_op=%d main=%d tea=%d C=%llu\n",
               unsstr64(op->op_num), op->state, (int)op->rs_id,
               node_local->rs[op->rs_id].rs_op_count,
               node_local->rs[op->rs_id].main_op_count,
               node_local->rs[op->rs_id].tea_op_count,
               (unsigned long long)cycle_count);
      }
      op = op->next_rdy;
    } else {
      last = &op->next_rdy;
      op = op->next_rdy;
    }
  }

  /* 2. Scheduling buffer */
  for (uns ii = 0; ii < node_local->sd.max_op_count; ii++) {
    op = node_local->sd.ops[ii];
    if (op && op->h2p_chain_id == chain_id) {
      node_local->sd.ops[ii] = NULL;
      node_local->sd.op_count--;
    }
  }

  /* 3. next_op_into_rs: advance past this chain's op if needed */
  op = node_local->next_op_into_rs;
  if (op && op->h2p_chain_id == chain_id) {
    Op* next = op->next_node;
    while (next && next->h2p_chain_id == chain_id)
      next = next->next_node;
    node_local->next_op_into_rs = next;
  }

  /* 4. Node table: remove this chain's ops, propagate wake-ups, free */
  node_local->node_tail = NULL;
  for (op = node_local->node_head, last = &node_local->node_head; op;) {
    if (op->h2p_chain_id == chain_id) {
      *last = op->next_node;
      op->in_node_list = FALSE;

      /* RS counter: only pre-scheduling states need adjustment */
      if (op->state == OS_IN_RS || op->state == OS_READY ||
          op->state == OS_WAIT_FWD || op->state == OS_SLEEP ||
          op->state == OS_LOW_PRIORITY || op->state == OS_TENTATIVE) {
        if (node_local->rs[op->rs_id].rs_op_count > 0)
          node_local->rs[op->rs_id].rs_op_count--;
        if (node_local->rs[op->rs_id].tea_op_count > 0)
          node_local->rs[op->rs_id].tea_op_count--;
      }

      /* Wake-up propagation: clear not-rdy bits in ops that were waiting
       * on this flushed producer.  Surviving chain ops or main-thread ops
       * may be blocked; unblocking them lets execution continue.
       * Skip same-chain dependents — they are freed in this same loop pass
       * and re-adding them to rdy_head would create a dangling pointer. */
      for (Wake_Up_Entry* we = op->wake_up_head; we; we = we->next) {
        Op* dep_op = we->op;
        if (!dep_op || !dep_op->op_pool_valid ||
            dep_op->unique_num != we->unique_num)
          continue;
        if (dep_op->h2p_chain_id == chain_id)
          continue;
        clear_not_rdy_bit(dep_op, we->rdy_bit);
        if (dep_op->srcs_not_rdy_vector == 0x0 &&
            dep_op->state == OS_IN_RS && !dep_op->in_rdy_list) {
          dep_op->next_rdy = node_local->rdy_head;
          node_local->rdy_head = dep_op;
          dep_op->in_rdy_list = TRUE;
        }
      }

      Op* next = op->next_node;
      free_op(op);
      STAT_EVENT(proc_id, TEA_OPS_FLUSHED);
      op = next;
    } else {
      last = &op->next_node;
      node_local->node_tail = op;
      op = op->next_node;
    }
  }
}
