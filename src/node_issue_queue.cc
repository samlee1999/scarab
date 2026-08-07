/*
 * Copyright 2025 University of California Santa Cruz
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
 * File         : node_issue_queue.cc
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 4/15/2025
 * Description  :
 ***************************************************************************************/

#include "node_issue_queue.h"

#include <cstring>

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "memory/memory.h"

#include "exec_ports.h"
#include "node_stage.h"
#include "statistics.h"
#include "tea/tea_thread.h"
}

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_NODE_STAGE, ##args)

/**************************************************************************************/
/* Prototypes */

int64 node_dispatch_find_emptiest_rs(Op*);
void node_schedule_policy_sched(Op*);

/**************************************************************************************/
/* Physical RS-entry lifecycle */

static inline void node_issue_queue_assert_physical_accounting(
  Node_Stage* node_local, Reservation_Station* rs, uns rs_id,
  const Op* op) {
  Flag mismatch = !rs->size || !rs->entry_status || !rs->free_entry_ids ||
                  rs->free_entry_count > rs->size ||
                  rs->rs_op_count != rs->size - rs->free_entry_count;
  INC_STAT_EVENT(node_local->proc_id,
                 TEA_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Physical IQ entry divergence: rs=%u occupied=%u free=%u/%u op_num=%s C=%llu\n",
          rs_id, rs->rs_op_count, rs->free_entry_count, rs->size,
          op ? unsstr64(op->op_num) : "none", cycle_count);
}

void node_issue_queue_allocate_rs_entry(Node_Stage* node_local, Op* op,
                                        uns rs_id) {
  ASSERT(0, node_local && op);
  ASSERT(node_local->proc_id, rs_id < NUM_RS);
  Reservation_Station* rs = &node_local->rs[rs_id];
  node_issue_queue_assert_physical_accounting(node_local, rs, rs_id, op);

  Flag mismatch = !rs->free_entry_count || op->rs_entry_id != MAX_CTR;
  INC_STAT_EVENT(node_local->proc_id,
                 TEA_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Cannot allocate physical IQ entry: rs=%u free=%u/%u old_entry=%s op_num=%s C=%llu\n",
          rs_id, rs->free_entry_count, rs->size,
          unsstr64(op->rs_entry_id), unsstr64(op->op_num), cycle_count);

  uns32 entry_id = rs->free_entry_ids[rs->free_entry_head];
  rs->free_entry_head = (rs->free_entry_head + 1) % rs->size;
  rs->free_entry_count--;

  mismatch = entry_id >= rs->size;
  INC_STAT_EVENT(node_local->proc_id,
                 TEA_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Physical IQ free list returned invalid entry: rs=%u entry=%u size=%u op_num=%s C=%llu\n",
          rs_id, entry_id, rs->size, unsstr64(op->op_num), cycle_count);

  uns32 word = entry_id / 64;
  uint64_t mask = 1ull << (entry_id % 64);
  mismatch = rs->entry_status[word] & mask;
  INC_STAT_EVENT(node_local->proc_id,
                 TEA_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Allocated occupied physical IQ entry: rs=%u entry=%u op_num=%s C=%llu\n",
          rs_id, entry_id, unsstr64(op->op_num), cycle_count);

  rs->entry_status[word] |= mask;
  rs->rs_op_count++;
  op->rs_entry_id = entry_id;
  node_issue_queue_assert_physical_accounting(node_local, rs, rs_id, op);
}

void node_issue_queue_release_rs_entry(Node_Stage* node_local, Op* op) {
  ASSERT(0, node_local && op);
  ASSERT(node_local->proc_id, op->rs_id < NUM_RS);
  Reservation_Station* rs = &node_local->rs[op->rs_id];
  node_issue_queue_assert_physical_accounting(
    node_local, rs, (uns)op->rs_id, op);

  Flag mismatch = op->rs_entry_id >= rs->size ||
                  !rs->rs_op_count || rs->free_entry_count >= rs->size;
  INC_STAT_EVENT(node_local->proc_id,
                 TEA_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Cannot release physical IQ entry: rs=%s entry=%s free=%u/%u op_num=%s C=%llu\n",
          unsstr64(op->rs_id), unsstr64(op->rs_entry_id),
          rs->free_entry_count, rs->size, unsstr64(op->op_num), cycle_count);

  uns32 entry_id = (uns32)op->rs_entry_id;
  uns32 word = entry_id / 64;
  uint64_t mask = 1ull << (entry_id % 64);
  mismatch = !(rs->entry_status[word] & mask);
  INC_STAT_EVENT(node_local->proc_id,
                 TEA_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Released free physical IQ entry: rs=%s entry=%u op_num=%s C=%llu\n",
          unsstr64(op->rs_id), entry_id, unsstr64(op->op_num), cycle_count);

  rs->entry_status[word] &= ~mask;
  rs->free_entry_ids[rs->free_entry_tail] = entry_id;
  rs->free_entry_tail = (rs->free_entry_tail + 1) % rs->size;
  rs->free_entry_count++;
  rs->rs_op_count--;
  op->rs_entry_id = MAX_CTR;
  node_issue_queue_assert_physical_accounting(
    node_local, rs, (uns)op->rs_id, op);
}

void node_issue_queue_reset_rs_entries(Node_Stage* node_local) {
  if (!node_local || !node_local->rs)
    return;

  for (uns rs_id = 0; rs_id < NUM_RS; ++rs_id) {
    Reservation_Station* rs = &node_local->rs[rs_id];
    if (!rs->size || !rs->entry_status || !rs->free_entry_ids)
      continue;

    std::memset(rs->entry_status, 0,
                ((rs->size + 63) / 64) * sizeof(uint64_t));
    for (uns32 entry_id = 0; entry_id < rs->size; ++entry_id)
      rs->free_entry_ids[entry_id] = entry_id;
    rs->free_entry_head = 0;
    rs->free_entry_tail = 0;
    rs->free_entry_count = rs->size;
    rs->rs_op_count = 0;
    rs->main_op_count = 0;
    rs->tea_op_count = 0;
    node_issue_queue_assert_physical_accounting(
      node_local, rs, rs_id, NULL);
  }
}

/**************************************************************************************/
/* Issuers:
 *      The interface to the issue functions is that Scarab will pass the
 * function the op to be issued, and the issuer will return the RS id that the
 * op should be issued to, or -1 meaning that there is no RS for the op to
 * be issued to.
 */

/*
 * FIND_EMPTIEST_RS: will always select the RS with the most empty slots
 *
 * Phase 4: Thread-aware dispatch - checks per-thread partition limits
 */
int64 node_dispatch_find_emptiest_rs(Op* op) {
  int64 emptiest_rs_id = NODE_ISSUE_QUEUE_RS_SLOT_INVALID;
  uns emptiest_rs_slots = 0;

  Flag is_tea_op = (op->thread_id == 1);

  /*
   * Iterate through RSs looking for an available RS that is connected to
   * an FU that can execute the OP.
   */
  for (int64 rs_id = 0; rs_id < NUM_RS; ++rs_id) {
    Reservation_Station* rs = &node->rs[rs_id];
    ASSERT(node->proc_id, !rs->size || rs->rs_op_count <= rs->size);

    /* TODO: support infinite RS for upper-bound expr */
    ASSERTM(node->proc_id, rs->size, "Infinite RS not suppoted by node_dispatch_find_emptiest_rs issuer.");

    for (uns32 i = 0; i < rs->num_fus; ++i) {
      // find the FU that can execute this op
      Func_Unit* fu = rs->connected_fus[i];
      if (!(get_fu_type(op->table_info->op_type, op->table_info->is_simd) & fu->type)) {
        continue;
      }

      /* Phase 4: Check per-thread partition limits */
      uns num_empty_slots;
      if (is_tea_op) {
        /* TEA op: Check TEA partition availability */
        if (rs->tea_op_count >= rs->tea_rs_limit) {
          continue;  /* TEA partition full */
        }
        num_empty_slots = rs->tea_rs_limit - rs->tea_op_count;
      } else {
        /* Main op: Check Main partition availability */
        if (rs->main_op_count >= rs->main_rs_limit) {
          continue;  /* Main partition full */
        }
        num_empty_slots = rs->main_rs_limit - rs->main_op_count;
      }

      if (num_empty_slots == 0) {
        continue;
      }

      // find the emptiest RS
      if (emptiest_rs_slots < num_empty_slots) {
        emptiest_rs_id = rs_id;
        emptiest_rs_slots = num_empty_slots;
      }
    }
  }

  return emptiest_rs_id;
}

/**************************************************************************************/
/* Schedulers:
 *      The interface to the schedule functions is that Scarab will pass the
 * function the ready op, and the scheduler will return the selected ops in
 * node->sd. See OLDEST_FIRST_SCHED for an example. Note, it is not necessary
 * to look at FU availability in this stage, if the FU is busy, then the op
 * will be ignored and available to schedule again in the next stage.
 */

static inline bool node_issue_queue_precedes(const Op* lhs, const Op* rhs) {
  /* Preserve the existing TEA-first two-pass contract. A Main op examined in
   * pass 2 must not evict a TEA op already selected in pass 1. */
  if (lhs->thread_id != rhs->thread_id)
    return lhs->thread_id == 1;

  switch (NODE_ISSUE_QUEUE_SCHEDULE_SCHEME) {
    case NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_OLDEST_FIRST:
      return lhs->op_num < rhs->op_num;

    case NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_RANDOM_PHYSICAL:
      ASSERT(node->proc_id, lhs->rs_id < NUM_RS && rhs->rs_id < NUM_RS);
      ASSERT(node->proc_id,
             lhs->rs_entry_id < node->rs[lhs->rs_id].size);
      ASSERT(node->proc_id,
             rhs->rs_entry_id < node->rs[rhs->rs_id].size);
      if (lhs->rs_id != rhs->rs_id)
        return lhs->rs_id < rhs->rs_id;
      if (lhs->rs_entry_id != rhs->rs_entry_id)
        return lhs->rs_entry_id < rhs->rs_entry_id;
      ASSERTM(node->proc_id, lhs == rhs,
              "Two ops own the same physical IQ entry: rs=%s entry=%s lhs=%s rhs=%s C=%llu\n",
              unsstr64(lhs->rs_id), unsstr64(lhs->rs_entry_id),
              unsstr64(lhs->op_num), unsstr64(rhs->op_num), cycle_count);
      return false;

    default:
      ASSERTM(node->proc_id, FALSE,
              "Unknown node issue queue schedule scheme %u\n",
              NODE_ISSUE_QUEUE_SCHEDULE_SCHEME);
      return false;
  }
}

/* Select according to the configured baseline policy. */
void node_schedule_policy_sched(Op* op) {
  int32 replace_slot_op_id = NODE_ISSUE_QUEUE_FU_SLOT_INVALID;

  // Iterate through the FUs that this RS is connected to.
  Reservation_Station* rs = &node->rs[op->rs_id];
  for (uns32 i = 0; i < rs->num_fus; ++i) {
    // check if this op can be executed by this FU
    Func_Unit* fu = rs->connected_fus[i];
    if (!(get_fu_type(op->table_info->op_type, op->table_info->is_simd) & fu->type)) {
      continue;
    }

    uns32 fu_id = fu->fu_id;
    Op* s_op = node->sd.ops[fu_id];

    // nobody has been scheduled to this FU yet
    if (!s_op) {
      DEBUG(node->proc_id, "Scheduler selecting    op_num:%s  fu_id:%d op:%s l1:%d\n", unsstr64(op->op_num), fu_id,
            disasm_op(op, TRUE), op->engine_info.l1_miss);
      ASSERT(node->proc_id, fu_id < (uns32)node->sd.max_op_count);
      op->fu_num = fu_id;
      node->sd.ops[op->fu_num] = op;
      node->last_scheduled_opnum = op->op_num;
      node->sd.op_count += !s_op;
      ASSERT(node->proc_id, node->sd.op_count <= node->sd.max_op_count);
      return;
    }

    if (!node_issue_queue_precedes(op, s_op)) {
      continue;
    }

    // The slot is occupied by a lower-ranked candidate.
    if (replace_slot_op_id == NODE_ISSUE_QUEUE_FU_SLOT_INVALID) {
      replace_slot_op_id = fu_id;
      continue;
    }

    // Keep the lowest-ranked compatible candidate as the replacement target.
    Op* replace_op = node->sd.ops[replace_slot_op_id];
    if (node_issue_queue_precedes(replace_op, s_op))
      replace_slot_op_id = fu_id;
  }

  /* Did not find an empty slot or a lower-ranked slot. */
  if (replace_slot_op_id == NODE_ISSUE_QUEUE_FU_SLOT_INVALID) {
    return;
  }

  /* Replace the lowest-ranked compatible candidate. */
  uns32 fu_id = replace_slot_op_id;
  DEBUG(node->proc_id, "Scheduler selecting    op_num:%s  fu_id:%d op:%s l1:%d\n", unsstr64(op->op_num), fu_id,
        disasm_op(op, TRUE), op->engine_info.l1_miss);
  ASSERT(node->proc_id, fu_id < (uns32)node->sd.max_op_count);
  op->fu_num = fu_id;
  node->sd.ops[op->fu_num] = op;
  node->last_scheduled_opnum = op->op_num;
  node->sd.op_count += 0;  // replacing an op, not adding a new one.
  ASSERT(node->proc_id, node->sd.op_count <= node->sd.max_op_count);
}

/**************************************************************************************/
/* Driven Table */

using Dispatch_Func = int64 (*)(Op*);
Dispatch_Func dispatch_func_table[NODE_ISSUE_QUEUE_DISPATCH_SCHEME_NUM] = {
    [NODE_ISSUE_QUEUE_DISPATCH_SCHEME_FIND_EMPTIEST_RS] = {node_dispatch_find_emptiest_rs},
};

using Schedule_Func = void (*)(Op*);
Schedule_Func schedule_func_table[NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_NUM] = {
    [NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_OLDEST_FIRST] = {node_schedule_policy_sched},
    [NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_RANDOM_PHYSICAL] = {node_schedule_policy_sched},
};

/**************************************************************************************/

/*
 * Memory is blocked when there are no more MSHRs in the L1 Q
 * (i.e., there is no way to handle a D-Cache miss).
 * This function checks to see if any of the L1 MSHRs have become available.
 */
void node_issue_queue_check_mem() {
  /* if we are stalled due to lack of MSHRs to the L1, check to see if there is space now. */
  if (node->mem_blocked && mem_can_allocate_req_buffer(node->proc_id, MRT_DFETCH, FALSE)) {
    node->mem_blocked = FALSE;
    STAT_EVENT(node->proc_id, MEM_BLOCK_LENGTH_0 + MIN2(node->mem_block_length, 5000) / 100);
    if (DIE_ON_MEM_BLOCK_THRESH) {
      if (node->proc_id == DIE_ON_MEM_BLOCK_CORE) {
        ASSERTM(node->proc_id, node->mem_block_length < DIE_ON_MEM_BLOCK_THRESH,
                "Core blocked on memory for %u cycles (%llu--%llu)\n", node->mem_block_length,
                cycle_count - node->mem_block_length, cycle_count);
      }
    }
    node->mem_block_length = 0;
  }
  INC_STAT_EVENT(node->proc_id, CORE_MEM_BLOCKED, node->mem_blocked);
  node->mem_block_length += node->mem_blocked;
}

/*
 * Remove scheduled ops (i.e., going from RS to FUs) from the RS and ready queue
 */
void node_issue_queue_clear() {
  // TODO: make this traversal more efficient since we know what ops we tried to schedule last cycle
  Op** last = &node->rdy_head;
  for (Op* op = node->rdy_head; op;) {
    Op* next = op->next_rdy;
    if (!node_ready_op_should_clear_rs(op)) {
      last = &op->next_rdy;
      op = next;
      continue;
    }

    DEBUG(node->proc_id, "Removing from RS (and ready list)  op_num:%s op:%s l1:%d\n", unsstr64(op->op_num),
          disasm_op(op, TRUE), op->engine_info.l1_miss);
    Flag tea_done_clear = (op->thread_id == 1 && op->state == OS_DONE);
    *last = next;
    op->next_rdy = NULL;
    op->in_rdy_list = FALSE;
    node_decrement_rs_counters_for_clear(node, op, TRUE);
    if (tea_done_clear)
      STAT_EVENT(node->proc_id, TEA_READY_LIST_DONE_CLEARED);

    STAT_EVENT(node->proc_id, OP_ISSUED);
    op = next;
  }
}

/*
 * Fill the scheduling window (RS) with oldest available ops.
 * For each available op:
 *  - Allocate it to its designated reservation station.
 *  - If all source operands are ready, insert it into the ready list.
 */
void node_issue_queue_dispatch() {
  Op* op = NULL;
  uns32 num_fill_rs = 0;

  /* Scan through dispatched nodes in node table that have not been filled to RS yet.
   * TEA ops are dispatched independently by tea_dispatch_to_rs() and tea_dispatch_retry()
   * in node_stage.c — they never enter this loop. */
  for (op = node->next_op_into_rs; op; op = op->next_node) {
    /* Skip TEA ops — they are dispatched directly by tea_dispatch_to_rs().
     * TEA ops in Node Table have state != OS_IN_ROB (already in RS) or
     * OS_IN_ROB (awaiting tea_dispatch_retry). Either way, skip here. */
    if (op->thread_id == 1)
      continue;

    int64 rs_id = dispatch_func_table[NODE_ISSUE_QUEUE_DISPATCH_SCHEME](op);
    if (rs_id == NODE_ISSUE_QUEUE_RS_SLOT_INVALID)
      break;
    ASSERT(node->proc_id, rs_id >= 0 && rs_id < NUM_RS);

    Reservation_Station* rs = &node->rs[rs_id];
    ASSERTM(node->proc_id, !rs->size || rs->rs_op_count < rs->size,
            "There must be at least one free space in selected RS!\n");

    ASSERT(node->proc_id, op->state == OS_IN_ROB);
    op->state = OS_IN_RS;
    op->rs_id = (Counter)rs_id;
    node_issue_queue_allocate_rs_entry(node, op, (uns)rs_id);
    rs->main_op_count++;

    num_fill_rs++;

    DEBUG(node->proc_id, "Filling %s with op_num:%s (%d)\n", rs->name, unsstr64(op->op_num), rs->rs_op_count);

    if (op->srcs_not_rdy_vector == 0) {
      /* op is ready to issue right now */
      DEBUG(node->proc_id, "Adding to ready list  op_num:%s op:%s l1:%d\n", unsstr64(op->op_num), disasm_op(op, TRUE),
            op->engine_info.l1_miss);
      op->state = (cycle_count + 1 >= op->rdy_cycle ? OS_READY : OS_WAIT_FWD);
      op->next_rdy = node->rdy_head;
      node->rdy_head = op;
      op->in_rdy_list = TRUE;
    }

    // maximum number of operations to fill into the RS per cycle (0 = unlimited)
    if (RS_FILL_WIDTH && (num_fill_rs == RS_FILL_WIDTH)) {
      op = op->next_node;
      break;
    }
  }

  // mark the next node to continue filling in the next cycle.
  node->next_op_into_rs = op;
}

/*
 * Helper function to check if an op can be scheduled this cycle.
 * Returns TRUE if the op should be considered for scheduling, FALSE otherwise.
 */
static inline Flag node_issue_queue_op_can_schedule(Op* op) {
  ASSERT(node->proc_id, node->proc_id == op->proc_id);
  ASSERTM(node->proc_id, op->in_rdy_list, "op_num %llu\n", op->op_num);

  if (op->state == OS_WAIT_MEM) {
    if (node->mem_blocked)
      return FALSE;
    else
      op->state = OS_READY;
  }

  if (op->state == OS_TENTATIVE || op->state == OS_WAIT_DCACHE)
    return FALSE;

  ASSERTM(node->proc_id, op->state == OS_IN_RS || op->state == OS_READY || op->state == OS_WAIT_FWD,
          "op_num: %llu, op_state: %s\n", op->op_num, Op_State_str(op->state));

  /* op will be ready next cycle, try to schedule */
  if (cycle_count >= op->rdy_cycle - 1) {
    ASSERT(node->proc_id, op->srcs_not_rdy_vector == 0x0);
    return TRUE;
  }

  return FALSE;
}

/*
 * Schedule ready ops (ops that are currently in the ready list).
 *
 * Input:  node->rdy_head, containing all ops that are ready to issue from each of the RSs.
 * Output: node->sd, containing ops thats being passed to the FUs.
 *
 * If a functional unit is available, it will accept the scheduled operation,
 * which is then removed from the ready list. If no FU is available, the
 * operation remains in the ready list to be considered in the next
 * scheduling cycle.
 *
 * TEA Priority Scheduling (Phase 3):
 * When TEA is active, uses two-pass scheduling:
 *   Pass 1: Schedule TEA ops (thread_id=1) first
 *   Pass 2: Schedule Main ops (thread_id=0) to remaining FU slots
 */
void node_issue_queue_schedule() {
  /*
   * the next stage is supposed to clear them out,
   * regardless of whether they are actually sent to a functional unit
   */
  ASSERT(node->proc_id, node->sd.op_count == 0);

  // Check to see if the L1 Q is (still) full
  node_issue_queue_check_mem();

  Flag tea_active = TEA_ENABLE && tea_is_active(node->proc_id);

  /*
   * Pass 1: TEA ops first (when TEA is active)
   * This gives TEA ops priority access to FU slots.
   */
  if (tea_active) {
    for (Op* op = node->rdy_head; op; op = op->next_rdy) {
      if (op->thread_id != 1)
        continue;  /* Skip Main thread ops in this pass */

      if (!node_issue_queue_op_can_schedule(op))
        continue;

      DEBUG(node->proc_id, "TEA Scheduler examining    op_num:%s op:%s l1:%d st:%s rdy:%s\n",
            unsstr64(op->op_num), disasm_op(op, TRUE), op->engine_info.l1_miss,
            Op_State_str(op->state), unsstr64(op->rdy_cycle));
      DEBUG(node->proc_id, "TEA Scheduler considering  op_num:%s op:%s l1:%d\n",
            unsstr64(op->op_num), disasm_op(op, TRUE), op->engine_info.l1_miss);

      int prev_count = node->sd.op_count;
      schedule_func_table[NODE_ISSUE_QUEUE_SCHEDULE_SCHEME](op);

      /* Track if TEA op was actually scheduled */
      if (node->sd.op_count > prev_count) {
        STAT_EVENT(node->proc_id, TEA_OPS_ISSUED);
      }
    }
  }

  /*
   * Pass 2: Main thread ops (always executed)
   * Main ops get scheduled to remaining FU slots.
   */
  for (Op* op = node->rdy_head; op; op = op->next_rdy) {
    /* Skip TEA ops - they were handled in Pass 1 */
    if (tea_active && op->thread_id == 1)
      continue;

    if (!node_issue_queue_op_can_schedule(op))
      continue;

    DEBUG(node->proc_id, "Scheduler examining    op_num:%s op:%s l1:%d st:%s rdy:%s exec:%s done:%s\n",
          unsstr64(op->op_num), disasm_op(op, TRUE), op->engine_info.l1_miss, Op_State_str(op->state),
          unsstr64(op->rdy_cycle), unsstr64(op->exec_cycle), unsstr64(op->done_cycle));
    DEBUG(node->proc_id, "Scheduler considering  op_num:%s op:%s l1:%d\n", unsstr64(op->op_num), disasm_op(op, TRUE),
          op->engine_info.l1_miss);

    schedule_func_table[NODE_ISSUE_QUEUE_SCHEDULE_SCHEME](op);
  }
}

/**************************************************************************************/
/* External Function */

void node_issue_queue_update() {
  /* remove scheduled ops from RS and ready list */
  node_issue_queue_clear();

  /* fill RS with oldest ops waiting for it */
  node_issue_queue_dispatch();

  /* first schedule 1 ready op per NUM_FUS  */
  node_issue_queue_schedule();
}
