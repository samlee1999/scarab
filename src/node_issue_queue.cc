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

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "core.param.h"

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

static inline Flag node_issue_queue_rs_supports_op(
  const Reservation_Station* rs, const Op* op) {
  uns64 op_fu_type = get_fu_type(op->table_info->op_type,
                                 op->table_info->is_simd);
  for (uns32 i = 0; i < rs->num_fus; ++i)
    if (op_fu_type & rs->connected_fus[i]->type)
      return TRUE;
  return FALSE;
}

static inline Flag node_issue_queue_piq_partition_mismatch(
  const Reservation_Station* rs) {
  return rs->main_op_count != rs->zereco_priority_op_count +
                                rs->zereco_normal_op_count ||
         rs->zereco_priority_op_count > rs->zereco_priority_rs_limit ||
         rs->zereco_normal_op_count > rs->zereco_normal_rs_limit ||
         rs->zereco_priority_rs_limit + rs->zereco_normal_rs_limit !=
           rs->main_rs_limit;
}

static void node_issue_queue_check_piq_partition(
  const Reservation_Station* rs, uns rs_id, const Op* op) {
  if (!ZERECO_PIQ_ENABLE)
    return;
  Flag mismatch = node_issue_queue_piq_partition_mismatch(rs);
  INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_PARTITION_INTEGRITY_MISMATCHES,
                 mismatch);
  ASSERTM(node->proc_id, !mismatch,
          "P-IQ counter divergence: rs=%u main=%u piq=%u/%u normal=%u/%u op_num=%s C=%llu\n",
          rs_id, rs->main_op_count, rs->zereco_priority_op_count,
          rs->zereco_priority_rs_limit, rs->zereco_normal_op_count,
          rs->zereco_normal_rs_limit,
          op ? unsstr64(op->op_num) : "none", cycle_count);
}

static inline void node_issue_queue_check_physical_entry_accounting(
  const Reservation_Station* rs, uns rs_id, const Op* op) {
  Flag mismatch = !rs->size || !rs->entry_status || !rs->free_entry_ids ||
                  rs->free_entry_count > rs->size ||
                  rs->rs_op_count != rs->size - rs->free_entry_count;
  INC_STAT_EVENT(node->proc_id,
                 ZERECO_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node->proc_id, !mismatch,
          "Physical IQ entry divergence: rs=%u occupied=%u free=%u/%u op_num=%s C=%llu\n",
          rs_id, rs->rs_op_count, rs->free_entry_count, rs->size,
          op ? unsstr64(op->op_num) : "none", cycle_count);
}

void node_issue_queue_allocate_rs_entry(Node_Stage* node_local, Op* op,
                                        uns rs_id) {
  ASSERT(0, node_local && op);
  ASSERT(node_local->proc_id, rs_id < NUM_RS);
  Reservation_Station* rs = &node_local->rs[rs_id];
  Flag mismatch = !rs->size || !rs->entry_status || !rs->free_entry_ids ||
                  !rs->free_entry_count || op->rs_entry_id != MAX_CTR;
  INC_STAT_EVENT(node_local->proc_id,
                 ZERECO_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Cannot allocate physical IQ entry: rs=%u free=%u/%u old_entry=%s op_num=%s C=%llu\n",
          rs_id, rs->free_entry_count, rs->size,
          unsstr64(op->rs_entry_id), unsstr64(op->op_num), cycle_count);

  uns32 entry_id = rs->free_entry_ids[rs->free_entry_head];
  rs->free_entry_head = (rs->free_entry_head + 1) % rs->size;
  rs->free_entry_count--;

  mismatch = entry_id >= rs->size;
  INC_STAT_EVENT(node_local->proc_id,
                 ZERECO_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Physical IQ free list returned invalid entry: rs=%u entry=%u size=%u op_num=%s C=%llu\n",
          rs_id, entry_id, rs->size, unsstr64(op->op_num), cycle_count);
  uns32 word = entry_id / 64;
  uint64_t mask = 1ull << (entry_id % 64);
  mismatch = rs->entry_status[word] & mask;
  INC_STAT_EVENT(node_local->proc_id,
                 ZERECO_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Allocated occupied physical IQ entry: rs=%u entry=%u op_num=%s C=%llu\n",
          rs_id, entry_id, unsstr64(op->op_num), cycle_count);
  rs->entry_status[word] |= mask;
  op->rs_entry_id = entry_id;
}

void node_issue_queue_release_rs_entry(Node_Stage* node_local, Op* op) {
  ASSERT(0, node_local && op);
  ASSERT(node_local->proc_id, op->rs_id < NUM_RS);
  Reservation_Station* rs = &node_local->rs[op->rs_id];
  Flag mismatch = !rs->size || !rs->entry_status || !rs->free_entry_ids ||
                  op->rs_entry_id >= rs->size ||
                  rs->free_entry_count >= rs->size;
  INC_STAT_EVENT(node_local->proc_id,
                 ZERECO_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Cannot release physical IQ entry: rs=%s entry=%s free=%u/%u op_num=%s C=%llu\n",
          unsstr64(op->rs_id), unsstr64(op->rs_entry_id),
          rs->free_entry_count, rs->size, unsstr64(op->op_num), cycle_count);

  uns32 entry_id = (uns32)op->rs_entry_id;
  uns32 word = entry_id / 64;
  uint64_t mask = 1ull << (entry_id % 64);
  mismatch = !(rs->entry_status[word] & mask);
  INC_STAT_EVENT(node_local->proc_id,
                 ZERECO_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Released free physical IQ entry: rs=%s entry=%u op_num=%s C=%llu\n",
          unsstr64(op->rs_id), entry_id, unsstr64(op->op_num), cycle_count);

  rs->entry_status[word] &= ~mask;
  rs->free_entry_ids[rs->free_entry_tail] = entry_id;
  rs->free_entry_tail = (rs->free_entry_tail + 1) % rs->size;
  rs->free_entry_count++;
  op->rs_entry_id = MAX_CTR;

  mismatch = rs->rs_op_count != rs->size - rs->free_entry_count;
  INC_STAT_EVENT(node_local->proc_id,
                 ZERECO_IQ_PHYSICAL_ENTRY_INTEGRITY_MISMATCHES, mismatch);
  ASSERTM(node_local->proc_id, !mismatch,
          "Physical IQ count mismatch after release: rs=%s occupied=%u free=%u/%u op_num=%s C=%llu\n",
          unsstr64(op->rs_id), rs->rs_op_count, rs->free_entry_count,
          rs->size, unsstr64(op->op_num), cycle_count);
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
static int64 node_dispatch_find_emptiest_rs_for_piq_class(
  Op* op, Flag use_priority_partition) {
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

    if (!node_issue_queue_rs_supports_op(rs, op))
      continue;

    /* Phase 4: Check per-thread and optional P-IQ partition limits. */
    uns num_empty_slots;
    if (is_tea_op) {
      if (rs->tea_op_count >= rs->tea_rs_limit)
        continue;
      num_empty_slots = rs->tea_rs_limit - rs->tea_op_count;
    } else if (ZERECO_PIQ_ENABLE) {
      if (use_priority_partition) {
        if (rs->zereco_priority_op_count >=
            rs->zereco_priority_rs_limit)
          continue;
        num_empty_slots = rs->zereco_priority_rs_limit -
                          rs->zereco_priority_op_count;
      } else {
        if (rs->zereco_normal_op_count >= rs->zereco_normal_rs_limit)
          continue;
        num_empty_slots = rs->zereco_normal_rs_limit -
                          rs->zereco_normal_op_count;
      }
    } else {
      if (rs->main_op_count >= rs->main_rs_limit)
        continue;
      num_empty_slots = rs->main_rs_limit - rs->main_op_count;
    }

    if (num_empty_slots == 0)
      continue;

    // find the emptiest compatible partition
    if (emptiest_rs_slots < num_empty_slots) {
      emptiest_rs_id = rs_id;
      emptiest_rs_slots = num_empty_slots;
    }
  }

  return emptiest_rs_id;
}

int64 node_dispatch_find_emptiest_rs(Op* op) {
  return node_dispatch_find_emptiest_rs_for_piq_class(
    op, op->zereco_iq_priority_bit);
}

/**************************************************************************************/
/* Schedulers:
 *      The interface to the schedule functions is that Scarab will pass the
 * function the ready op, and the scheduler will return the selected ops in
 * node->sd. See OLDEST_FIRST_SCHED for an example. Note, it is not necessary
 * to look at FU availability in this stage, if the FU is busy, then the op
 * will be ignored and available to schedule again in the next stage.
 */

static inline bool node_issue_queue_precedes(const Op* lhs, const Op* rhs,
                                             Flag use_zereco_priority) {
  if (use_zereco_priority && ZERECO_IQ_PRIORITY_SCHEDULE_ENABLE) {
    bool lhs_priority = lhs->thread_id == 0 && lhs->zereco_iq_priority_bit;
    bool rhs_priority = rhs->thread_id == 0 && rhs->zereco_iq_priority_bit;
    if (lhs_priority != rhs_priority)
      return lhs_priority;
  }

  switch (NODE_ISSUE_QUEUE_SCHEDULE_SCHEME) {
    case NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_OLDEST_FIRST:
      return lhs->op_num < rhs->op_num;

    case NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_RANDOM_PHYSICAL:
      ASSERT(node->proc_id, lhs->rs_id < NUM_RS && rhs->rs_id < NUM_RS);
      ASSERT(node->proc_id, lhs->rs_entry_id < node->rs[lhs->rs_id].size);
      ASSERT(node->proc_id, rhs->rs_entry_id < node->rs[rhs->rs_id].size);
      /* Golden Cove connects each FU to one RS.  Keep an RS-ID tie-breaker
       * for other legal configurations where compatible candidates from
       * different RSs may reach the same FU. */
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

/* Select according to the configured baseline policy.  ZERECO Priority ops
 * form a higher class; within each class use either age or physical position. */
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

    if (!node_issue_queue_precedes(op, s_op, TRUE)) {
      continue;
    }

    // The slot is occupied by a lower-ranked candidate.
    if (replace_slot_op_id == NODE_ISSUE_QUEUE_FU_SLOT_INVALID) {
      replace_slot_op_id = fu_id;
      continue;
    }

    // Keep the worst replaceable candidate: normal before priority, then young.
    Op* replace_op = node->sd.ops[replace_slot_op_id];
    if (node_issue_queue_precedes(replace_op, s_op, TRUE)) {
      replace_slot_op_id = fu_id;
    }
  }

  /* Did not find an empty slot or a slot that is younger than me, do nothing */
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

    Flag priority_admission_candidate =
      ZERECO_PIQ_ENABLE && op->zereco_iq_priority_bit;
    int64 rs_id = dispatch_func_table[NODE_ISSUE_QUEUE_DISPATCH_SCHEME](op);

    /* PUBS non-stall policy: a Priority candidate that cannot enter any
     * compatible Priority partition may use a compatible Normal entry.  The
     * fallback op receives normal scheduling priority for the rest of its
     * life; its original candidate membership remains recorded separately. */
    if (rs_id == NODE_ISSUE_QUEUE_RS_SLOT_INVALID &&
        priority_admission_candidate &&
        ZERECO_PIQ_DISPATCH_POLICY == 1) {
      rs_id = node_dispatch_find_emptiest_rs_for_piq_class(op, FALSE);
      if (rs_id != NODE_ISSUE_QUEUE_RS_SLOT_INVALID) {
        op->zereco_iq_priority_bit = FALSE;
        op->zereco_piq_fallback = TRUE;
        STAT_EVENT(node->proc_id,
                   ZERECO_PIQ_PRIORITY_TO_NORMAL_FALLBACK_OPS);
        STAT_EVENT(node->proc_id,
                   ZERECO_PIQ_PRIORITY_TO_NORMAL_FALLBACK_PCT);
      }
    }

    if (rs_id == NODE_ISSUE_QUEUE_RS_SLOT_INVALID) {
      if (ZERECO_PIQ_ENABLE) {
        Counter unused_other_slots = 0;
        for (uns candidate_rs_id = 0; candidate_rs_id < NUM_RS;
             ++candidate_rs_id) {
          Reservation_Station* candidate_rs = &node->rs[candidate_rs_id];
          if (!node_issue_queue_rs_supports_op(candidate_rs, op))
            continue;
          node_issue_queue_check_piq_partition(candidate_rs,
                                               candidate_rs_id, op);
          if (op->zereco_iq_priority_bit)
            unused_other_slots += candidate_rs->zereco_normal_rs_limit -
                                  candidate_rs->zereco_normal_op_count;
          else
            unused_other_slots += candidate_rs->zereco_priority_rs_limit -
                                  candidate_rs->zereco_priority_op_count;
        }

        Flag nonstall_mismatch =
          ZERECO_PIQ_DISPATCH_POLICY == 1 &&
          op->zereco_iq_priority_bit && unused_other_slots;
        INC_STAT_EVENT(node->proc_id,
                       ZERECO_PIQ_NONSTALL_INTEGRITY_MISMATCHES,
                       nonstall_mismatch);
        ASSERTM(node->proc_id, !nonstall_mismatch,
                "P-IQ non-stall missed compatible Normal capacity: op_num=%s unused_normal=%llu C=%llu\n",
                unsstr64(op->op_num), unused_other_slots, cycle_count);

        op->zereco_piq_dispatch_wait_cycles++;
        if (op->zereco_iq_priority_bit) {
          STAT_EVENT(node->proc_id,
                     ZERECO_PIQ_PRIORITY_DISPATCH_STALL_CYCLES);
          if (unused_other_slots) {
            STAT_EVENT(node->proc_id,
                       ZERECO_PIQ_PRIORITY_STALL_WITH_UNUSED_NORMAL_CYCLES);
            INC_STAT_EVENT(
              node->proc_id,
              ZERECO_PIQ_UNUSED_NORMAL_SLOTS_DURING_PRIORITY_STALL,
              unused_other_slots);
          }
        } else {
          STAT_EVENT(node->proc_id,
                     ZERECO_PIQ_NORMAL_DISPATCH_STALL_CYCLES);
          if (unused_other_slots) {
            STAT_EVENT(node->proc_id,
                       ZERECO_PIQ_NORMAL_STALL_WITH_UNUSED_PRIORITY_CYCLES);
            INC_STAT_EVENT(
              node->proc_id,
              ZERECO_PIQ_UNUSED_PRIORITY_SLOTS_DURING_NORMAL_STALL,
              unused_other_slots);
          }
        }
      }
      break;
    }
    ASSERT(node->proc_id, rs_id >= 0 && rs_id < NUM_RS);

    Reservation_Station* rs = &node->rs[rs_id];
    ASSERTM(node->proc_id, !rs->size || rs->rs_op_count < rs->size,
            "There must be at least one free space in selected RS!\n");

    ASSERT(node->proc_id, op->state == OS_IN_ROB);
    op->state = OS_IN_RS;
    op->rs_id = (Counter)rs_id;
    node_issue_queue_allocate_rs_entry(node, op, (uns)rs_id);
    rs->rs_op_count++;
    rs->main_op_count++;
    if (ZERECO_PIQ_ENABLE) {
      op->zereco_piq_entry = op->zereco_iq_priority_bit;
      Flag fallback_mismatch =
        op->zereco_piq_fallback &&
        (!priority_admission_candidate || op->zereco_piq_entry ||
         op->zereco_iq_priority_bit);
      INC_STAT_EVENT(node->proc_id,
                     ZERECO_PIQ_NONSTALL_INTEGRITY_MISMATCHES,
                     fallback_mismatch);
      ASSERTM(node->proc_id, !fallback_mismatch,
              "P-IQ fallback retained Priority state: op_num=%s candidate=%u entry=%u priority=%u C=%llu\n",
              unsstr64(op->op_num), priority_admission_candidate,
              op->zereco_piq_entry, op->zereco_iq_priority_bit,
              cycle_count);
      if (priority_admission_candidate)
        STAT_EVENT(node->proc_id,
                   ZERECO_PIQ_PRIORITY_ADMISSION_CANDIDATE_OPS);
      if (op->zereco_piq_entry) {
        rs->zereco_priority_op_count++;
        STAT_EVENT(node->proc_id, ZERECO_PIQ_PRIORITY_DISPATCHED_OPS);
        if (op->zereco_piq_dispatch_wait_cycles) {
          STAT_EVENT(node->proc_id,
                     ZERECO_PIQ_PRIORITY_DISPATCH_WAITED_OPS);
          INC_STAT_EVENT(node->proc_id,
                         ZERECO_PIQ_PRIORITY_DISPATCH_WAIT_TOTAL,
                         op->zereco_piq_dispatch_wait_cycles);
          INC_STAT_EVENT(node->proc_id,
                         ZERECO_PIQ_PRIORITY_DISPATCH_WAIT_AVG,
                         op->zereco_piq_dispatch_wait_cycles);
        }
      } else {
        rs->zereco_normal_op_count++;
        STAT_EVENT(node->proc_id, ZERECO_PIQ_NORMAL_DISPATCHED_OPS);
        if (op->zereco_piq_dispatch_wait_cycles) {
          STAT_EVENT(node->proc_id, ZERECO_PIQ_NORMAL_DISPATCH_WAITED_OPS);
          INC_STAT_EVENT(node->proc_id,
                         ZERECO_PIQ_NORMAL_DISPATCH_WAIT_TOTAL,
                         op->zereco_piq_dispatch_wait_cycles);
          INC_STAT_EVENT(node->proc_id,
                         ZERECO_PIQ_NORMAL_DISPATCH_WAIT_AVG,
                         op->zereco_piq_dispatch_wait_cycles);
        }
      }
      op->zereco_piq_dispatch_wait_cycles = 0;
      node_issue_queue_check_piq_partition(rs, (uns)rs_id, op);
    }
    node_issue_queue_check_physical_entry_accounting(rs, (uns)rs_id, op);

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

static inline void node_issue_queue_collect_piq_rs_stats(
  uns rs_id, const Reservation_Station* rs) {
  switch (rs_id) {
    case 0:
      INC_STAT_EVENT(node->proc_id,
                     ZERECO_PIQ_RS0_PRIORITY_OCCUPANCY_TOTAL,
                     rs->zereco_priority_op_count);
      INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_RS0_NORMAL_OCCUPANCY_TOTAL,
                     rs->zereco_normal_op_count);
      if (rs->zereco_priority_op_count == rs->zereco_priority_rs_limit)
        STAT_EVENT(node->proc_id, ZERECO_PIQ_RS0_PRIORITY_FULL_CYCLES);
      if (rs->zereco_normal_op_count == rs->zereco_normal_rs_limit)
        STAT_EVENT(node->proc_id, ZERECO_PIQ_RS0_NORMAL_FULL_CYCLES);
      break;
    case 1:
      INC_STAT_EVENT(node->proc_id,
                     ZERECO_PIQ_RS1_PRIORITY_OCCUPANCY_TOTAL,
                     rs->zereco_priority_op_count);
      INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_RS1_NORMAL_OCCUPANCY_TOTAL,
                     rs->zereco_normal_op_count);
      if (rs->zereco_priority_op_count == rs->zereco_priority_rs_limit)
        STAT_EVENT(node->proc_id, ZERECO_PIQ_RS1_PRIORITY_FULL_CYCLES);
      if (rs->zereco_normal_op_count == rs->zereco_normal_rs_limit)
        STAT_EVENT(node->proc_id, ZERECO_PIQ_RS1_NORMAL_FULL_CYCLES);
      break;
    case 2:
      INC_STAT_EVENT(node->proc_id,
                     ZERECO_PIQ_RS2_PRIORITY_OCCUPANCY_TOTAL,
                     rs->zereco_priority_op_count);
      INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_RS2_NORMAL_OCCUPANCY_TOTAL,
                     rs->zereco_normal_op_count);
      if (rs->zereco_priority_op_count == rs->zereco_priority_rs_limit)
        STAT_EVENT(node->proc_id, ZERECO_PIQ_RS2_PRIORITY_FULL_CYCLES);
      if (rs->zereco_normal_op_count == rs->zereco_normal_rs_limit)
        STAT_EVENT(node->proc_id, ZERECO_PIQ_RS2_NORMAL_FULL_CYCLES);
      break;
    default:
      break;
  }
}

static void node_issue_queue_collect_zereco_piq_occupancy(void) {
  if (!ZERECO_PIQ_ENABLE)
    return;

  Counter priority_capacity = 0;
  Counter normal_capacity = 0;
  Counter priority_occupancy = 0;
  Counter normal_occupancy = 0;
  Counter priority_full_rs = 0;
  Counter normal_full_rs = 0;

  for (uns rs_id = 0; rs_id < NUM_RS; ++rs_id) {
    Reservation_Station* rs = &node->rs[rs_id];
    node_issue_queue_check_piq_partition(rs, rs_id, NULL);
    priority_capacity += rs->zereco_priority_rs_limit;
    normal_capacity += rs->zereco_normal_rs_limit;
    priority_occupancy += rs->zereco_priority_op_count;
    normal_occupancy += rs->zereco_normal_op_count;
    priority_full_rs +=
      rs->zereco_priority_op_count == rs->zereco_priority_rs_limit;
    normal_full_rs +=
      rs->zereco_normal_op_count == rs->zereco_normal_rs_limit;
    node_issue_queue_collect_piq_rs_stats(rs_id, rs);
  }

  STAT_EVENT(node->proc_id, ZERECO_PIQ_CYCLES);
  INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_PRIORITY_CAPACITY_SLOT_CYCLES,
                 priority_capacity);
  INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_NORMAL_CAPACITY_SLOT_CYCLES,
                 normal_capacity);
  INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_PRIORITY_OCCUPANCY_SLOT_CYCLES,
                 priority_occupancy);
  INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_NORMAL_OCCUPANCY_SLOT_CYCLES,
                 normal_occupancy);
  INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_PRIORITY_OCCUPANCY_PCT,
                 priority_occupancy);
  INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_NORMAL_OCCUPANCY_PCT,
                 normal_occupancy);
  if (priority_full_rs)
    STAT_EVENT(node->proc_id, ZERECO_PIQ_ANY_PRIORITY_FULL_CYCLES);
  if (normal_full_rs)
    STAT_EVENT(node->proc_id, ZERECO_PIQ_ANY_NORMAL_FULL_CYCLES);
  INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_PRIORITY_FULL_RS_CYCLES,
                 priority_full_rs);
  INC_STAT_EVENT(node->proc_id, ZERECO_PIQ_NORMAL_FULL_RS_CYCLES,
                 normal_full_rs);
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

static inline Flag node_issue_queue_ops_share_fu(Op* lhs, Op* rhs) {
  Reservation_Station* lhs_rs = &node->rs[lhs->rs_id];
  Reservation_Station* rhs_rs = &node->rs[rhs->rs_id];
  uns64 lhs_type = get_fu_type(lhs->table_info->op_type,
                               lhs->table_info->is_simd);
  uns64 rhs_type = get_fu_type(rhs->table_info->op_type,
                               rhs->table_info->is_simd);
  for (uns ii = 0; ii < lhs_rs->num_fus; ++ii) {
    Func_Unit* lhs_fu = lhs_rs->connected_fus[ii];
    if (!(lhs_type & lhs_fu->type))
      continue;
    for (uns jj = 0; jj < rhs_rs->num_fus; ++jj) {
      Func_Unit* rhs_fu = rhs_rs->connected_fus[jj];
      if (lhs_fu->fu_id == rhs_fu->fu_id && (rhs_type & rhs_fu->type))
        return TRUE;
    }
  }
  return FALSE;
}

static inline uns64 node_issue_queue_op_fu_mask(const Op* op) {
  ASSERTM(node->proc_id, NUM_FUS <= 64,
          "ZERECO FU competition mask supports at most 64 FUs\n");
  Reservation_Station* rs = &node->rs[op->rs_id];
  uns64 op_type = get_fu_type(op->table_info->op_type,
                              op->table_info->is_simd);
  uns64 mask = 0;
  for (uns ii = 0; ii < rs->num_fus; ++ii) {
    Func_Unit* fu = rs->connected_fus[ii];
    if (op_type & fu->type)
      mask |= 1ull << fu->fu_id;
  }
  return mask;
}

static inline Flag node_issue_queue_selection_contains(
  Op* const* selected, uns selected_count, Op* target) {
  for (uns i = 0; i < selected_count; ++i)
    if (selected[i] == target)
      return TRUE;
  return FALSE;
}

static void node_issue_queue_shadow_consider_baseline(
  Op* op, Op** selected, uns selected_count) {
  int32 replace_fu_id = NODE_ISSUE_QUEUE_FU_SLOT_INVALID;
  Reservation_Station* rs = &node->rs[op->rs_id];

  for (uns32 i = 0; i < rs->num_fus; ++i) {
    Func_Unit* fu = rs->connected_fus[i];
    if (!(get_fu_type(op->table_info->op_type,
                      op->table_info->is_simd) & fu->type))
      continue;

    uns32 fu_id = fu->fu_id;
    ASSERT(node->proc_id, fu_id < selected_count);
    Op* selected_op = selected[fu_id];
    if (!selected_op) {
      selected[fu_id] = op;
      return;
    }
    if (!node_issue_queue_precedes(op, selected_op, FALSE))
      continue;

    if (replace_fu_id == NODE_ISSUE_QUEUE_FU_SLOT_INVALID ||
        node_issue_queue_precedes(selected[replace_fu_id], selected_op,
                                  FALSE))
      replace_fu_id = fu_id;
  }

  if (replace_fu_id != NODE_ISSUE_QUEUE_FU_SLOT_INVALID)
    selected[replace_fu_id] = op;
}

static void node_issue_queue_collect_zereco_shadow_stats(void) {
  if (!ZERECO_IQ_PRIORITY_POLICY)
    return;

  ASSERTM(node->proc_id, node->sd.max_op_count <= 64,
          "ZERECO shadow scheduler supports at most 64 FUs\n");
  Op* shadow_selected[64] = {NULL};
  uns selected_count = (uns)node->sd.max_op_count;
  for (Op* op = node->rdy_head; op; op = op->next_rdy) {
    if (op->thread_id != 0 || !node_issue_queue_op_can_schedule(op))
      continue;
    node_issue_queue_shadow_consider_baseline(op, shadow_selected,
                                              selected_count);
  }

  if (!ZERECO_IQ_PRIORITY_SCHEDULE_ENABLE) {
    Counter mismatches = 0;
    for (uns i = 0; i < selected_count; ++i) {
      Op* op = shadow_selected[i];
      if (op && !node_issue_queue_selection_contains(
                  node->sd.ops, selected_count, op))
        mismatches++;
    }
    for (uns i = 0; i < selected_count; ++i) {
      Op* op = node->sd.ops[i];
      if (op && op->thread_id == 0 &&
          !node_issue_queue_selection_contains(shadow_selected,
                                                selected_count, op))
        mismatches++;
    }
    INC_STAT_EVENT(node->proc_id, ZERECO_IQ_SHADOW_SELECTION_MISMATCHES,
                   mismatches);
    return;
  }

  Counter displaced_normal_ops = 0;
  for (uns i = 0; i < selected_count; ++i) {
    Op* shadow_op = shadow_selected[i];
    if (!shadow_op || shadow_op->thread_id != 0 || shadow_op->off_path ||
        shadow_op->zereco_iq_priority_bit ||
        node_issue_queue_selection_contains(node->sd.ops, selected_count,
                                            shadow_op))
      continue;
    displaced_normal_ops++;
    shadow_op->zereco_iq_normal_displaced_cycles++;
  }

  Counter promoted_priority_ops = 0;
  for (uns i = 0; i < selected_count; ++i) {
    Op* actual_op = node->sd.ops[i];
    if (!actual_op || actual_op->thread_id != 0 || actual_op->off_path ||
        !actual_op->zereco_iq_priority_bit ||
        node_issue_queue_selection_contains(shadow_selected, selected_count,
                                            actual_op))
      continue;
    promoted_priority_ops++;
  }

  if (displaced_normal_ops)
    STAT_EVENT(node->proc_id,
               ZERECO_IQ_NORMAL_DISPLACED_BY_PRIORITY_CYCLES);
  INC_STAT_EVENT(node->proc_id,
                 ZERECO_IQ_NORMAL_DISPLACED_BY_PRIORITY_OP_CYCLES,
                 displaced_normal_ops);
  INC_STAT_EVENT(node->proc_id, ZERECO_IQ_PRIORITY_PROMOTED_OP_CYCLES,
                 promoted_priority_ops);
}

static inline void node_issue_queue_collect_zereco_ready_stats(void) {
  if (!ZERECO_IQ_PRIORITY_POLICY)
    return;
  Counter ready_priority = 0;
  for (Op* op = node->rdy_head; op; op = op->next_rdy) {
    if (op->thread_id == 0 && op->zereco_iq_priority_bit &&
        node_issue_queue_op_can_schedule(op))
      ready_priority++;
  }
  STAT_EVENT(node->proc_id, ZERECO_IQ_READY_CYCLES);
  INC_STAT_EVENT(node->proc_id, ZERECO_IQ_READY_PRIORITY_TOTAL,
                 ready_priority);
  INC_STAT_EVENT(node->proc_id, ZERECO_IQ_READY_PRIORITY_AVG,
                 ready_priority);
}

static inline void node_issue_queue_collect_zereco_contention_stats(void) {
  if (!ZERECO_IQ_PRIORITY_POLICY ||
      !ZERECO_IQ_PRIORITY_SCHEDULE_ENABLE)
    return;

  uns64 normal_fu_mask = 0;
  for (Op* normal = node->rdy_head; normal; normal = normal->next_rdy) {
    if (normal->thread_id != 0 || normal->zereco_iq_priority_bit ||
        !node_issue_queue_op_can_schedule(normal))
      continue;
    normal_fu_mask |= node_issue_queue_op_fu_mask(normal);
  }

  Counter competing_priority_ops = 0;
  for (Op* priority = node->rdy_head; priority;
       priority = priority->next_rdy) {
    if (priority->thread_id == 0 && priority->zereco_iq_priority_bit &&
        node_issue_queue_op_can_schedule(priority) &&
        (node_issue_queue_op_fu_mask(priority) & normal_fu_mask))
      competing_priority_ops++;
  }
  if (competing_priority_ops) {
    STAT_EVENT(node->proc_id,
               ZERECO_IQ_PRIORITY_NORMAL_COMPETITION_CYCLES);
    INC_STAT_EVENT(node->proc_id,
                   ZERECO_IQ_PRIORITY_NORMAL_COMPETING_PRIORITY_OPS,
                   competing_priority_ops);
  }

  Counter blocked_priority_ops = 0;
  for (Op* op = node->rdy_head; op; op = op->next_rdy) {
    if (op->thread_id != 0 || !op->zereco_iq_priority_bit ||
        !node_issue_queue_op_can_schedule(op))
      continue;
    bool selected = false;
    for (uns fu_id = 0; fu_id < (uns)node->sd.max_op_count; ++fu_id)
      selected |= node->sd.ops[fu_id] == op;
    if (selected)
      continue;

    bool blocked_by_priority = false;
    for (uns fu_id = 0; fu_id < (uns)node->sd.max_op_count; ++fu_id) {
      Op* winner = node->sd.ops[fu_id];
      if (winner && winner->thread_id == 0 &&
          winner->zereco_iq_priority_bit &&
          node_issue_queue_ops_share_fu(op, winner)) {
        blocked_by_priority = true;
        break;
      }
    }
    blocked_priority_ops += blocked_by_priority;
  }
  if (blocked_priority_ops) {
    STAT_EVENT(node->proc_id, ZERECO_IQ_PRIORITY_CONTENTION_CYCLES);
    INC_STAT_EVENT(node->proc_id, ZERECO_IQ_PRIORITY_CONTENTION_PAIRS,
                   blocked_priority_ops);
  }

  Counter overtakes = 0;
  for (uns fu_id = 0; fu_id < (uns)node->sd.max_op_count; ++fu_id) {
    Op* winner = node->sd.ops[fu_id];
    if (!winner || winner->thread_id != 0 ||
        !winner->zereco_iq_priority_bit)
      continue;
    for (Op* normal = node->rdy_head; normal; normal = normal->next_rdy) {
      if (normal->thread_id != 0 || normal->zereco_iq_priority_bit ||
          normal->op_num >= winner->op_num ||
          !node_issue_queue_op_can_schedule(normal) ||
          !node_issue_queue_ops_share_fu(winner, normal))
        continue;
      bool normal_selected = false;
      for (uns normal_fu = 0; normal_fu < (uns)node->sd.max_op_count;
           ++normal_fu)
        normal_selected |= node->sd.ops[normal_fu] == normal;
      if (!normal_selected) {
        overtakes++;
        break;
      }
    }
  }
  INC_STAT_EVENT(node->proc_id, ZERECO_IQ_PRIORITY_OVERTAKES_NORMAL,
                 overtakes);
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
  node_issue_queue_collect_zereco_ready_stats();

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
  node_issue_queue_collect_zereco_contention_stats();
  node_issue_queue_collect_zereco_shadow_stats();
}

/**************************************************************************************/
/* External Function */

void node_issue_queue_update() {
  /* remove scheduled ops from RS and ready list */
  node_issue_queue_clear();

  /* fill RS with oldest ops waiting for it */
  node_issue_queue_dispatch();
  node_issue_queue_collect_zereco_piq_occupancy();

  /* first schedule 1 ready op per NUM_FUS  */
  node_issue_queue_schedule();
}
