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
#include "zereco/rfp.h"
#include "statistics.h"

/**************************************************************************************/
/* Observation table.
 *
 * One tagged, set-associative entry per static PC, carrying measurements that
 * are all keyed by PC and so share a table rather than paying for several:
 *
 *   last_src / last_producer_pc  previous LPR, to detect how often the critical
 *                                edge of a static instruction changes
 *   depth                        distance from the H2P branch, propagated one
 *                                level per dynamic instance exactly as the real
 *                                mechanism would
 *   owner_pc                     which H2P branch claimed this PC, to count how
 *                                often two branches fight over one entry
 *   confirm                      how often this PC has been re-derived as a
 *                                critical producer; the histogram of this field
 *                                over member commits is the population curve for
 *                                every retention threshold at once
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
  uns8 confirm;         /* times re-derived as a critical producer, saturating */
  Counter lru_touch;
} Critpath_PC_Entry;

typedef struct Critpath_Core_State_struct {
  Flag initialized;
  Critpath_PC_Entry* pc_table;
  uns sets;
  uns assoc;
  uns confirm_max;
  Counter retires;      /* drives the decay interval */
} Critpath_Core_State;

static Critpath_Core_State critpath_state[MAX_NUM_PROCS];

static inline Critpath_Core_State* critpath_get_state(uns proc_id) {
  ASSERT(proc_id, proc_id < MAX_NUM_PROCS);
  return &critpath_state[proc_id];
}

/* Set-associative rather than direct-mapped: at Phase A's 64K direct-mapped
   entries, conflict evictions outnumbered the propagations they were supposed
   to be recording, which distorts any population or depth number read off the
   table.  Associativity also matches the shape the real structure will have. */
static Critpath_PC_Entry* critpath_pc_lookup(uns proc_id, Addr pc,
                                             Flag allocate) {
  Critpath_Core_State* state = critpath_get_state(proc_id);
  if (!state->initialized)
    return NULL;

  Critpath_PC_Entry* set =
    &state->pc_table[(pc % state->sets) * state->assoc];
  for (uns way = 0; way < state->assoc; ++way) {
    if (set[way].valid && set[way].tag == pc) {
      set[way].lru_touch = cycle_count;
      return &set[way];
    }
  }
  if (!allocate)
    return NULL;

  /* Prefer an invalid way, then the least recently used one.  A chain member
     is not protected: letting one age out is exactly what the measurement is
     meant to expose. */
  Critpath_PC_Entry* victim = NULL;
  for (uns way = 0; way < state->assoc; ++way) {
    if (!set[way].valid) {
      victim = &set[way];
      break;
    }
    if (!victim || set[way].lru_touch < victim->lru_touch)
      victim = &set[way];
  }
  ASSERT(proc_id, victim);

  if (victim->valid) {
    STAT_EVENT(proc_id, CRITPATH_PC_TABLE_CONFLICTS);
    if (victim->in_slice)
      STAT_EVENT(proc_id, CRITPATH_PC_TABLE_EVICT_MEMBER);
  }
  memset(victim, 0, sizeof(*victim));
  victim->valid = TRUE;
  victim->tag = pc;
  victim->lru_touch = cycle_count;
  return victim;
}

/* Age every counter, and drop the entries that have stopped being re-derived.
   This is the knob that adapts membership to a program phase change: a chain
   that no longer carries the critical edge simply stops being refreshed and
   falls out on its own, without anything having to detect the phase. */
static void critpath_decay(uns proc_id) {
  Critpath_Core_State* state = critpath_get_state(proc_id);
  uns entries = state->sets * state->assoc;
  STAT_EVENT(proc_id, CRITPATH_DECAY_SWEEPS);

  for (uns ii = 0; ii < entries; ++ii) {
    Critpath_PC_Entry* e = &state->pc_table[ii];
    if (!e->valid)
      continue;
    if (e->confirm) {
      e->confirm--;
    } else if (e->in_slice) {
      /* Nothing re-confirmed it for a whole interval: it leaves the chain. */
      e->in_slice = FALSE;
      e->depth = 0;
      e->owner_pc = 0;
      STAT_EVENT(proc_id, CRITPATH_DECAY_DROPPED_MEMBER);
    }
  }
}

/**************************************************************************************/
/* Init / reset */

void critpath_init(uns proc_id) {
  ASSERT(proc_id, proc_id < MAX_NUM_PROCS);
  Critpath_Core_State* state = critpath_get_state(proc_id);
  memset(state, 0, sizeof(*state));

  if (!ZERECO_CRITPATH_PROFILE)
    return;

  ASSERTM(proc_id, ZERECO_CRITPATH_TABLE_SETS > 0 && ZERECO_CRITPATH_TABLE_ASSOC > 0,
          "critpath table geometry must be non-zero\n");
  ASSERTM(proc_id, ZERECO_CRITPATH_CONFIRM_BITS > 0 &&
                     ZERECO_CRITPATH_CONFIRM_BITS <= 8,
          "zereco_critpath_confirm_bits must be between 1 and 8\n");
  ASSERTM(proc_id, ZERECO_CRITPATH_INSERT_GATE <= 2,
          "zereco_critpath_insert_gate must be 0 (all branches), "
          "1 (mispredicted at least once), or 2 (H2P)\n");

  state->sets = ZERECO_CRITPATH_TABLE_SETS;
  state->assoc = ZERECO_CRITPATH_TABLE_ASSOC;
  state->confirm_max = (1u << ZERECO_CRITPATH_CONFIRM_BITS) - 1u;
  state->retires = 0;
  state->pc_table = (Critpath_PC_Entry*)calloc(
    (size_t)state->sets * state->assoc, sizeof(Critpath_PC_Entry));
  ASSERTM(proc_id, state->pc_table,
          "Failed to allocate the critical-path observation table\n");
  state->initialized = TRUE;
}

void critpath_reset(uns proc_id) {
  Critpath_Core_State* state = critpath_get_state(proc_id);
  if (!state->initialized)
    return;
  memset(state->pc_table, 0,
         sizeof(Critpath_PC_Entry) * (size_t)state->sets * state->assoc);
  state->retires = 0;
}

/**************************************************************************************/
/* Consumption path.
 *
 * The frontend asks this whether an instruction belongs to a critical chain, and
 * a load that does is also the one worth prefetching into the register file.
 * Both are reads: allocation and aging belong to the commit path alone. */

Flag critpath_is_member(uns proc_id, Addr pc, uns max_depth) {
  if (!ZERECO_CRITPATH_PROFILE)
    return FALSE;
  Critpath_PC_Entry* e = critpath_pc_lookup(proc_id, pc, FALSE);
  if (!e || !e->in_slice)
    return FALSE;
  if (max_depth && e->depth > max_depth)
    return FALSE;
  return TRUE;
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

/* Member commits bucketed by how often that PC has been re-derived as critical.
   The tail sum above any T is the population a retention threshold of T would
   admit, so one run answers the question for every threshold. */
static void critpath_record_confirm(uns proc_id, uns8 confirm) {
  if (confirm == 0)
    STAT_EVENT(proc_id, CRITPATH_CONFIRM_0);
  else if (confirm == 1)
    STAT_EVENT(proc_id, CRITPATH_CONFIRM_1);
  else if (confirm == 2)
    STAT_EVENT(proc_id, CRITPATH_CONFIRM_2);
  else if (confirm == 3)
    STAT_EVENT(proc_id, CRITPATH_CONFIRM_3);
  else if (confirm <= 5)
    STAT_EVENT(proc_id, CRITPATH_CONFIRM_4_5);
  else if (confirm <= 7)
    STAT_EVENT(proc_id, CRITPATH_CONFIRM_6_7);
  else
    STAT_EVENT(proc_id, CRITPATH_CONFIRM_8_PLUS);
}

/* Member commits bucketed by how hard-to-predict the owning branch is right
   now.  The tail sums give the population under each insertion gate. */
static void critpath_record_owner_class(uns proc_id, Addr owner_pc) {
  if (!owner_pc) {
    STAT_EVENT(proc_id, CRITPATH_OWNER_UNKNOWN);
    return;
  }
  uns32 c = hbt_get_counter(owner_pc);
  if (c > 1)
    STAT_EVENT(proc_id, CRITPATH_OWNER_H2P);
  else if (c == 1)
    STAT_EVENT(proc_id, CRITPATH_OWNER_MISP_ONCE);
  else
    STAT_EVENT(proc_id, CRITPATH_OWNER_COLD);
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

  /* Decay runs on committed instructions, the same clock the H2P table ages on,
     so the interval means the same thing in both structures. */
  state->retires++;
  if (ZERECO_CRITPATH_DECAY_INTERVAL &&
      (state->retires % ZERECO_CRITPATH_DECAY_INTERVAL) == 0)
    critpath_decay(proc_id);

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

  /* Seed before measuring, not after: an H2P branch is the root of its own
     slice from its very first commit, and taking its slack as non-slice on that
     first pass would bias the slice histograms against the one instruction that
     matters most. */
  /* Which branches may start a chain.  Gate 2 is the strictest -- only a branch
     the H2P table has already convicted -- and gates 1 and 0 loosen it, letting
     a chain warm up while its branch is still earning that status. */
  Flag branch = (op->table_info->cf_type != NOT_CF);
  Flag may_seed = branch &&
                  ((ZERECO_CRITPATH_INSERT_GATE == 0) ||
                   (ZERECO_CRITPATH_INSERT_GATE == 1 &&
                    op->oracle_info.hbt_misp_counter >= 1) ||
                   (ZERECO_CRITPATH_INSERT_GATE == 2 &&
                    op->oracle_info.hbt_pred_is_hard));

  if (may_seed) {
    if (!entry->in_slice || entry->owner_pc != pc) {
      entry->in_slice = TRUE;
      entry->depth = 0;
      entry->owner_pc = pc;
      STAT_EVENT(proc_id, CRITPATH_SEEDS);
    }
    /* A root re-confirms itself every time it commits. */
    if (entry->confirm < state->confirm_max)
      entry->confirm++;
  }

  Flag in_slice = entry->in_slice;
  if (in_slice) {
    /* A load on a critical chain is a Target Load.  Under RFP_TARGET_CRITPATH
       this commit is what grants it a Prefetch Table entry, replacing the
       backward walk that used to own that decision. */
    if (RFP_TARGET_CRITPATH && op->table_info->mem_type == MEM_LD &&
        (!ZERECO_CRITPATH_PRIORITY_MAX_DEPTH ||
         entry->depth <= ZERECO_CRITPATH_PRIORITY_MAX_DEPTH)) {
      rfp_note_target_load(proc_id, pc, op->oracle_info.va);
      STAT_EVENT(proc_id, CRITPATH_TARGET_LOADS);
    }
    STAT_EVENT(proc_id, CRITPATH_SLICE_OPS);
    critpath_record_confirm(proc_id, entry->confirm);
    critpath_record_owner_class(proc_id, entry->owner_pc);
  }

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

  /* ---- 5. propagate one level, as the real mechanism would -------------- */
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

  /* Read what this op contributes before touching the table again.  The lookup
     below can evict whichever entry shares its index -- including this one --
     so `entry` must not be dereferenced afterwards. */
  uns8 my_depth = entry->depth;
  Addr my_owner = entry->owner_pc;

  Critpath_PC_Entry* producer =
    critpath_pc_lookup(proc_id, op->critpath_last_producer_pc, TRUE);
  if (!producer)
    return;

  if (!producer->in_slice) {
    producer->in_slice = TRUE;
    producer->depth = my_depth + 1;
    producer->owner_pc = my_owner;
    producer->confirm = 1;
    STAT_EVENT(proc_id, CRITPATH_PROPAGATIONS);
    return;
  }

  /* Already a member.  Keep the shortest distance seen -- this also absorbs a
     self-dependence, where an induction variable is its own last producer and
     would otherwise deepen by one every iteration. */
  if (producer->depth > my_depth + 1)
    producer->depth = my_depth + 1;

  /* Count how often two H2P branches claim the same instruction: with one owner
     field per entry they overwrite each other, and a stale owner can cost the
     other branch a member of its chain. */
  if (producer->confirm < state->confirm_max)
    producer->confirm++;

  if (producer->owner_pc != my_owner) {
    STAT_EVENT(proc_id, CRITPATH_OWNER_OVERWRITE);
    if (producer->owner_pc && hbt_is_hard_branch(producer->owner_pc) &&
        !(my_owner && hbt_is_hard_branch(my_owner)))
      STAT_EVENT(proc_id, CRITPATH_OWNER_OVERWRITE_H2P_LOST);
    producer->owner_pc = my_owner;
  } else {
    STAT_EVENT(proc_id, CRITPATH_PROPAGATION_REFRESHED);
  }
}
