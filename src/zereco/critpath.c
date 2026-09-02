/***************************************************************************************
 * File         : zereco/critpath.c
 * Description  : Critical-path slice observation (Phase A -- measurement only).
 *                See critpath.h for what each measurement decides, and
 *                zereco_CRITPATH_DESIGN.md for the design it feeds.
 *
 * Two hooks, and nothing else touches the machine:
 *
 *   critpath_note_wake()    at the wakeup logic, keeps the largest and second
 *                           largest source wake cycle plus the owner of the
 *                           largest.  This is the argmax of the max the wakeup
 *                           logic already computes, so it adds no work.
 *
 *   critpath_note_retire()  at commit, where every measurement is taken.  Doing
 *                           it here means only on-path, architecturally executed
 *                           instructions contribute -- wrong-path work cannot
 *                           reach the counters.
 ***************************************************************************************/

#include "zereco/critpath.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "bp/hbt.h"
#include "core.param.h"
#include "op.h"
#include "statistics.h"

/**************************************************************************************/
/* Observation table.
 *
 * One direct-mapped, tagged entry per static PC.  It carries three unrelated
 * measurements that all happen to be keyed by PC, so they share a table rather
 * than paying for three:
 *
 *   last_src / last_producer_pc  previous LPR, to detect how often the critical
 *                                edge of a static instruction changes
 *   depth                        distance from the H2P branch, propagated one
 *                                level per dynamic instance exactly as the real
 *                                mechanism would
 *   owner_pc                     which H2P branch claimed this PC, to count how
 *                                often two branches fight over one entry
 */

typedef struct Critpath_PC_Entry_struct {
  Flag valid;
  Addr tag;             /* full PC; the table is scaffolding, so no hashing */
  Flag has_last;        /* a previous LPR observation exists */
  uns8 last_src;        /* previous LPR source index */
  Addr last_producer_pc;/* previous LPR producer PC */
  Flag in_slice;        /* reached by propagation from some H2P branch */
  uns8 depth;           /* distance from that branch, saturating */
  Addr owner_pc;        /* the H2P branch that claimed it */
} Critpath_PC_Entry;

typedef struct Critpath_Core_State_struct {
  Flag initialized;
  Critpath_PC_Entry* pc_table;
} Critpath_Core_State;

static Critpath_Core_State critpath_state[MAX_NUM_PROCS];

static inline Critpath_Core_State* critpath_get_state(uns proc_id) {
  ASSERT(proc_id, proc_id < MAX_NUM_PROCS);
  return &critpath_state[proc_id];
}

static inline Critpath_PC_Entry* critpath_pc_lookup(uns proc_id, Addr pc,
                                                    Flag allocate) {
  Critpath_Core_State* state = critpath_get_state(proc_id);
  if (!state->initialized)
    return NULL;

  Critpath_PC_Entry* e =
    &state->pc_table[pc % CRITPATH_PC_TABLE_ENTRIES];
  if (e->valid && e->tag == pc)
    return e;
  if (!allocate)
    return NULL;

  if (e->valid)
    STAT_EVENT(proc_id, CRITPATH_PC_TABLE_CONFLICTS);
  memset(e, 0, sizeof(*e));
  e->valid = TRUE;
  e->tag = pc;
  return e;
}

/**************************************************************************************/
/* Init / reset */

void critpath_init(uns proc_id) {
  ASSERT(proc_id, proc_id < MAX_NUM_PROCS);
  Critpath_Core_State* state = critpath_get_state(proc_id);
  memset(state, 0, sizeof(*state));

  if (!ZERECO_CRITPATH_PROFILE)
    return;

  state->pc_table = (Critpath_PC_Entry*)calloc(CRITPATH_PC_TABLE_ENTRIES,
                                               sizeof(Critpath_PC_Entry));
  ASSERTM(proc_id, state->pc_table,
          "Failed to allocate the critical-path observation table\n");
  state->initialized = TRUE;
}

void critpath_reset(uns proc_id) {
  Critpath_Core_State* state = critpath_get_state(proc_id);
  if (!state->initialized)
    return;
  memset(state->pc_table, 0,
         sizeof(Critpath_PC_Entry) * CRITPATH_PC_TABLE_ENTRIES);
}

/**************************************************************************************/
/* Wakeup hook */

void critpath_note_wake(Op* src_op, Op* dep_op, uns8 rdy_bit) {
  if (!ZERECO_CRITPATH_PROFILE)
    return;
  /* Main thread only; a TEA op's wakeups describe a different instruction
     stream and would mix two populations into one histogram. */
  if (!src_op || !dep_op || dep_op->thread_id != 0)
    return;
  if (rdy_bit >= dep_op->oracle_info.num_srcs)
    return;

  Counter arrival = src_op->wake_cycle;
  if (dep_op->critpath_wake_events < 255)
    dep_op->critpath_wake_events++;

  if (arrival > dep_op->critpath_last_cycle) {
    /* New latest source: the old winner becomes the runner-up. */
    dep_op->critpath_second_cycle = dep_op->critpath_last_cycle;
    dep_op->critpath_last_cycle = arrival;
    dep_op->critpath_last_src = rdy_bit;
    dep_op->critpath_last_dep_type =
      (uns8)dep_op->oracle_info.src_info[rdy_bit].type;
    dep_op->critpath_last_producer_pc =
      src_op->inst_info ? src_op->inst_info->addr : 0;
  } else if (arrival > dep_op->critpath_second_cycle) {
    dep_op->critpath_second_cycle = arrival;
  }
}

/**************************************************************************************/
/* Retire hook -- every measurement is taken here */

static void critpath_record_slack(uns proc_id, Counter slack, Flag in_slice) {
  INC_STAT_EVENT(proc_id, CRITPATH_SLACK_TOTAL, slack);
  INC_STAT_EVENT(proc_id, CRITPATH_SLACK_AVG, slack);

  if (slack == 0)
    STAT_EVENT(proc_id, CRITPATH_SLACK_0);
  else if (slack == 1)
    STAT_EVENT(proc_id, CRITPATH_SLACK_1);
  else if (slack == 2)
    STAT_EVENT(proc_id, CRITPATH_SLACK_2);
  else if (slack <= 4)
    STAT_EVENT(proc_id, CRITPATH_SLACK_3_4);
  else if (slack <= 8)
    STAT_EVENT(proc_id, CRITPATH_SLACK_5_8);
  else if (slack <= 16)
    STAT_EVENT(proc_id, CRITPATH_SLACK_9_16);
  else if (slack <= 32)
    STAT_EVENT(proc_id, CRITPATH_SLACK_17_32);
  else
    STAT_EVENT(proc_id, CRITPATH_SLACK_33_PLUS);

  if (!in_slice)
    return;

  INC_STAT_EVENT(proc_id, CRITPATH_SLICE_SLACK_TOTAL, slack);
  INC_STAT_EVENT(proc_id, CRITPATH_SLICE_SLACK_AVG, slack);
  if (slack == 0)
    STAT_EVENT(proc_id, CRITPATH_SLICE_SLACK_0);
  else if (slack == 1)
    STAT_EVENT(proc_id, CRITPATH_SLICE_SLACK_1);
  else if (slack == 2)
    STAT_EVENT(proc_id, CRITPATH_SLICE_SLACK_2);
  else if (slack <= 4)
    STAT_EVENT(proc_id, CRITPATH_SLICE_SLACK_3_4);
  else if (slack <= 8)
    STAT_EVENT(proc_id, CRITPATH_SLICE_SLACK_5_8);
  else
    STAT_EVENT(proc_id, CRITPATH_SLICE_SLACK_9_PLUS);
}

static void critpath_record_depth(uns proc_id, uns8 depth) {
  if (depth == 0)
    STAT_EVENT(proc_id, CRITPATH_DEPTH_0);
  else if (depth == 1)
    STAT_EVENT(proc_id, CRITPATH_DEPTH_1);
  else if (depth == 2)
    STAT_EVENT(proc_id, CRITPATH_DEPTH_2);
  else if (depth <= 4)
    STAT_EVENT(proc_id, CRITPATH_DEPTH_3_4);
  else if (depth <= 8)
    STAT_EVENT(proc_id, CRITPATH_DEPTH_5_8);
  else if (depth <= 16)
    STAT_EVENT(proc_id, CRITPATH_DEPTH_9_16);
  else
    STAT_EVENT(proc_id, CRITPATH_DEPTH_17_PLUS);
}

void critpath_note_retire(Op* op) {
  if (!ZERECO_CRITPATH_PROFILE)
    return;
  if (!op || op->thread_id != 0 || op->off_path)
    return;
  if (!op->inst_info || !op->table_info)
    return;

  uns proc_id = op->proc_id;
  Critpath_Core_State* state = critpath_get_state(proc_id);
  if (!state->initialized)
    return;

  Addr pc = op->inst_info->addr;
  STAT_EVENT(proc_id, CRITPATH_OPS);

  /* ---- 1. did this op ever wait on an operand? ------------------------- */
  /* Sources whose producer had already left the machine never signal a wake,
     so an op with no wake events had all its inputs long since available. */
  Flag frontier = (op->issue_cycle != MAX_CTR && op->rdy_cycle <= op->issue_cycle);
  if (frontier)
    STAT_EVENT(proc_id, CRITPATH_FRONTIER_OPS);

  if (op->critpath_wake_events == 0)
    STAT_EVENT(proc_id, CRITPATH_OPS_NO_WAKE);
  else if (op->critpath_wake_events == 1)
    STAT_EVENT(proc_id, CRITPATH_OPS_ONE_WAKE);
  else
    STAT_EVENT(proc_id, CRITPATH_OPS_MULTI_WAKE);

  Critpath_PC_Entry* entry = critpath_pc_lookup(proc_id, pc, TRUE);
  if (!entry)
    return;
  Flag in_slice = entry->in_slice;
  if (in_slice)
    STAT_EVENT(proc_id, CRITPATH_SLICE_OPS);

  Flag has_lpr = (op->critpath_wake_events > 0);

  /* ---- 2. slack between the critical operand and the runner-up --------- */
  /* Only meaningful with two competing arrivals.  With a single wake event the
     other operands were ready before this op was even renamed, so there is no
     sibling close enough to contend for criticality. */
  if (op->critpath_wake_events >= 2) {
    Counter slack = op->critpath_last_cycle - op->critpath_second_cycle;
    critpath_record_slack(proc_id, slack, in_slice);
  } else if (has_lpr) {
    STAT_EVENT(proc_id, CRITPATH_SINGLE_ARRIVAL);
    if (in_slice)
      STAT_EVENT(proc_id, CRITPATH_SLICE_SINGLE_ARRIVAL);
  }

  /* ---- 3. what kind of edge is critical ------------------------------- */
  /* A register edge can be followed backward by a physical-register scheme; a
     store-to-load edge cannot.  This is the cost of that blindness. */
  if (has_lpr) {
    if (op->critpath_last_dep_type == MEM_DATA_DEP) {
      STAT_EVENT(proc_id, CRITPATH_LPR_MEM_DEP);
      if (in_slice)
        STAT_EVENT(proc_id, CRITPATH_SLICE_LPR_MEM_DEP);
    } else {
      STAT_EVENT(proc_id, CRITPATH_LPR_REG_DEP);
      if (in_slice)
        STAT_EVENT(proc_id, CRITPATH_SLICE_LPR_REG_DEP);
    }
  }

  /* ---- 4. does the critical edge stay the same across instances? ------- */
  if (has_lpr) {
    if (!entry->has_last) {
      STAT_EVENT(proc_id, CRITPATH_LPR_FIRST_OBSERVATION);
    } else {
      if (entry->last_src == op->critpath_last_src)
        STAT_EVENT(proc_id, CRITPATH_LPR_SRC_STABLE);
      else
        STAT_EVENT(proc_id, CRITPATH_LPR_SRC_FLIPPED);

      if (entry->last_producer_pc == op->critpath_last_producer_pc)
        STAT_EVENT(proc_id, CRITPATH_LPR_PRODUCER_STABLE);
      else
        STAT_EVENT(proc_id, CRITPATH_LPR_PRODUCER_FLIPPED);

      if (in_slice) {
        if (entry->last_producer_pc == op->critpath_last_producer_pc)
          STAT_EVENT(proc_id, CRITPATH_SLICE_LPR_PRODUCER_STABLE);
        else
          STAT_EVENT(proc_id, CRITPATH_SLICE_LPR_PRODUCER_FLIPPED);
      }
    }
    entry->has_last = TRUE;
    entry->last_src = op->critpath_last_src;
    entry->last_producer_pc = op->critpath_last_producer_pc;
  }

  /* ---- 5. seed and propagate one level, as the real mechanism would ---- */
  Flag is_h2p_branch = (op->table_info->cf_type != NOT_CF) &&
                       op->oracle_info.hbt_pred_is_hard;

  if (is_h2p_branch) {
    /* The branch itself is the root of its own slice. */
    if (!entry->in_slice || entry->owner_pc != pc) {
      entry->in_slice = TRUE;
      entry->depth = 0;
      entry->owner_pc = pc;
      STAT_EVENT(proc_id, CRITPATH_SEEDS);
    }
    in_slice = TRUE;
  }

  if (!in_slice || !has_lpr)
    return;
  STAT_EVENT(proc_id, CRITPATH_SLICE_OPS_WITH_LPR);
  critpath_record_depth(proc_id, entry->depth);

  if (frontier) {
    /* Nothing upstream of this op is delaying it, so a backward walk has
       nothing left to gain here.  This is where propagation should stop. */
    STAT_EVENT(proc_id, CRITPATH_PROPAGATION_STOPPED_FRONTIER);
    return;
  }
  if (entry->depth >= CRITPATH_MAX_DEPTH) {
    STAT_EVENT(proc_id, CRITPATH_PROPAGATION_STOPPED_DEPTH);
    return;
  }
  if (op->critpath_last_producer_pc == 0)
    return;

  Critpath_PC_Entry* producer =
    critpath_pc_lookup(proc_id, op->critpath_last_producer_pc, TRUE);
  if (!producer)
    return;

  if (!producer->in_slice) {
    producer->in_slice = TRUE;
    producer->depth = entry->depth + 1;
    producer->owner_pc = entry->owner_pc;
    STAT_EVENT(proc_id, CRITPATH_PROPAGATIONS);
    return;
  }

  /* Already a member.  Keep the shortest distance seen, and count how often two
     H2P branches claim the same instruction -- with one owner field per entry
     they overwrite each other, and a stale owner can cost the other branch its
     chain member. */
  if (producer->depth > entry->depth + 1)
    producer->depth = entry->depth + 1;

  if (producer->owner_pc != entry->owner_pc) {
    STAT_EVENT(proc_id, CRITPATH_OWNER_OVERWRITE);
    if (hbt_is_hard_branch(producer->owner_pc) &&
        !hbt_is_hard_branch(entry->owner_pc))
      STAT_EVENT(proc_id, CRITPATH_OWNER_OVERWRITE_H2P_LOST);
    producer->owner_pc = entry->owner_pc;
  } else {
    STAT_EVENT(proc_id, CRITPATH_PROPAGATION_REFRESHED);
  }
}
