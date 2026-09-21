/***************************************************************************************
 * File         : zereco/critpath.c
 * Description  : Critical chains of H2P branches (brslice_tab).  See critpath.h
 *                for the mechanism and zereco_CRITPATH_DESIGN.md for the design.
 *
 * Two hooks build the chain; the frontend reads it (critpath_is_member):
 *
 *   critpath_note_wake()    at the wakeup logic, keeps the largest and second
 *                           largest source wake cycle plus the owner of the
 *                           largest.  This is the argmax of the max the wakeup
 *                           logic already computes, so it adds no work.
 *
 *   critpath_note_retire()  at commit: seeds H2P roots, lets a member add its
 *                           last-arriving producer one level up (subject to
 *                           filter A), ages members every decay interval, and
 *                           takes the measurements.  Only on-path,
 *                           architecturally executed instructions reach it.
 ***************************************************************************************/

#include "zereco/critpath.h"

#include <stdio.h>
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
  uns8 edge_conf;       /* A: confidence that edge_pc is this PC's critical producer */
  Addr edge_pc;         /* A: the producer that confidence is about */
  uns16 win_exec;       /* B: commits of this PC in the current decay window */
  uns16 win_crit;       /* B: times chosen as a last-arriving producer, same window */
  Counter lru_touch;
} Critpath_PC_Entry;

typedef struct Critpath_Core_State_struct {
  Flag initialized;
  Critpath_PC_Entry* pc_table;
  /* Full-slice mode only: a second table that applies the critical-path rule
     to the same commit stream, so every committed op can be classified as
     "full-slice member that the critical rule would / would not have kept".
     It is never consulted by the frontend; it only measures the filtering. */
  Critpath_PC_Entry* shadow_table;
  uns sets;
  uns assoc;
  uns confirm_max;
  Counter retires;      /* drives the decay interval; also the timeline x-axis */
  /* Timeline dump (ZERECO_CRITPATH_TIMELINE_INTERVAL).  Own counters, so that
     the warm-up stat reset does not zero them. */
  FILE* timeline;
  Counter tl_members;
  Counter tl_roots;
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
static Critpath_PC_Entry* critpath_table_lookup(uns proc_id,
                                                Critpath_Core_State* state,
                                                Critpath_PC_Entry* table,
                                                Addr pc, Flag allocate,
                                                Flag is_main) {
  Critpath_PC_Entry* set = &table[(pc % state->sets) * state->assoc];
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

  if (victim->valid && is_main) {
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

static Critpath_PC_Entry* critpath_pc_lookup(uns proc_id, Addr pc,
                                             Flag allocate) {
  Critpath_Core_State* state = critpath_get_state(proc_id);
  if (!state->initialized)
    return NULL;
  return critpath_table_lookup(proc_id, state, state->pc_table, pc, allocate,
                               TRUE);
}

/* Age every counter, and drop the entries that have stopped being re-derived.
   This is the knob that adapts membership to a program phase change: a chain
   that no longer carries the critical edge simply stops being refreshed and
   falls out on its own, without anything having to detect the phase. */
static void critpath_decay_table(uns proc_id, Critpath_Core_State* state,
                                 Critpath_PC_Entry* table, Flag is_main) {
  uns entries = state->sets * state->assoc;
  Counter live = 0;
  for (uns ii = 0; ii < entries; ++ii) {
    Critpath_PC_Entry* e = &table[ii];
    if (!e->valid)
      continue;
    /* B: a member that commits often but is rarely anyone's last-arriving
       producer is on the slice, not on the critical path.  Roots (depth 0) are
       branches -- nobody's producer -- so they are exempt. */
    if (is_main && ZERECO_CRITPATH_RATIO_MIN && e->in_slice && e->depth > 0 &&
        e->win_exec >= 4 &&
        (uns32)e->win_crit * 100 < (uns32)ZERECO_CRITPATH_RATIO_MIN * e->win_exec) {
      e->in_slice = FALSE;
      e->depth = 0;
      e->owner_pc = 0;
      e->confirm = 0;
      STAT_EVENT(proc_id, CRITPATH_RATIO_DROPPED_MEMBER);
    }
    e->win_exec = 0;
    e->win_crit = 0;
    if (e->confirm) {
      e->confirm--;
    } else if (e->in_slice) {
      /* Nothing re-confirmed it for a whole interval: it leaves the chain. */
      e->in_slice = FALSE;
      e->depth = 0;
      e->owner_pc = 0;
      if (is_main)
        STAT_EVENT(proc_id, CRITPATH_DECAY_DROPPED_MEMBER);
    }
    if (e->in_slice)
      live++;
  }
  /* Live member PCs after the sweep: averaged over sweeps this is the table
     population the real structure must hold (hardware budget, TODO D-6). */
  INC_STAT_EVENT(proc_id, is_main ? CRITPATH_LIVE_MEMBERS_AT_SWEEP
                                  : CRITPATH_SHADOW_LIVE_MEMBERS_AT_SWEEP,
                 live);
}

static Counter critpath_live_members(Critpath_Core_State* state,
                                     Critpath_PC_Entry* table) {
  Counter live = 0;
  uns entries = state->sets * state->assoc;
  for (uns ii = 0; ii < entries; ++ii)
    if (table[ii].valid && table[ii].in_slice)
      live++;
  return live;
}

/* One timeline row.  Called on the main thread's commit stream only. */
static void critpath_timeline_tick(uns proc_id, Critpath_Core_State* state) {
  if (proc_id != 0)
    return;
  if (!state->timeline) {
    state->timeline = fopen("critpath_timeline.csv", "w");
    ASSERTM(proc_id, state->timeline, "cannot open critpath_timeline.csv\n");
    fprintf(state->timeline,
            "committed_ops,committed_insts,cycle,member_commits,root_commits,"
            "live_member_pcs,live_member_pcs_shadow\n");
  }
  fprintf(state->timeline, "%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
          (unsigned long long)state->retires,
          (unsigned long long)inst_count[proc_id],
          (unsigned long long)cycle_count,
          (unsigned long long)state->tl_members,
          (unsigned long long)state->tl_roots,
          (unsigned long long)critpath_live_members(state, state->pc_table),
          (unsigned long long)(state->shadow_table
                                 ? critpath_live_members(state, state->shadow_table)
                                 : 0));
  fflush(state->timeline);
}

static void critpath_decay(uns proc_id) {
  Critpath_Core_State* state = critpath_get_state(proc_id);
  STAT_EVENT(proc_id, CRITPATH_DECAY_SWEEPS);
  critpath_decay_table(proc_id, state, state->pc_table, TRUE);
  if (state->shadow_table)
    critpath_decay_table(proc_id, state, state->shadow_table, FALSE);
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
  ASSERTM(proc_id, ZERECO_CRITPATH_EDGE_CONF_MIN <= 3,
          "zereco_critpath_edge_conf_min must be 0 (off) or 1..3 (2-bit counter)\n");
  ASSERTM(proc_id, ZERECO_CRITPATH_EDGE_CONF_EXEMPT_DEPTH >= -1,
          "zereco_critpath_edge_conf_exempt_depth must be -1 (none) or a depth\n");
  ASSERTM(proc_id, !ZERECO_CRITPATH_RATIO_MIN || ZERECO_CRITPATH_DECAY_INTERVAL,
          "zereco_critpath_ratio_min is evaluated at decay sweeps; set zereco_critpath_decay_interval\n");
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
  if (ZERECO_CRITPATH_FULL_SLICE) {
    state->shadow_table = (Critpath_PC_Entry*)calloc(
      (size_t)state->sets * state->assoc, sizeof(Critpath_PC_Entry));
    ASSERTM(proc_id, state->shadow_table,
            "Failed to allocate the critical-path shadow table\n");
  }
  state->initialized = TRUE;
}

void critpath_reset(uns proc_id) {
  Critpath_Core_State* state = critpath_get_state(proc_id);
  if (!state->initialized)
    return;
  memset(state->pc_table, 0,
         sizeof(Critpath_PC_Entry) * (size_t)state->sets * state->assoc);
  if (state->shadow_table)
    memset(state->shadow_table, 0,
           sizeof(Critpath_PC_Entry) * (size_t)state->sets * state->assoc);
  state->retires = 0;
}

/**************************************************************************************/
/* Consumption path.
 *
 * The frontend asks this whether an instruction belongs to a critical chain, and
 * a load that does is also the one worth prefetching into the register file.
 * It never allocates and never touches confirm or depth -- allocation and aging
 * belong to the commit path.  A hit does refresh the entry's LRU stamp, as a read
 * of a set-associative table would in hardware, so the fetch stream (including
 * wrong-path fetch under zereco_critpath_priority_offpath) does influence which
 * member a later allocation evicts. */

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
  /* Register-only edge set: a store->load forwarding wake has no physical
     register behind it, so the RSE's LPR mux cannot select it.  Keep it aside
     for the statistics only. */
  if (!ZERECO_CRITPATH_MEM_EDGE &&
      dep_op->oracle_info.src_info[rdy_bit].type != REG_DATA_DEP) {
    if (dep_op->critpath_mem_wake_events < 255)
      dep_op->critpath_mem_wake_events++;
    if (arrival > dep_op->critpath_mem_last_cycle)
      dep_op->critpath_mem_last_cycle = arrival;
    return;
  }
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

/* Filter A bookkeeping for one entry on an instance whose last-arriving producer
   is `cur`: returns whether the instance names the producer being tracked, and
   moves the 2-bit confidence.  A different producer either starts it over
   (reset, the baseline) or, with hysteresis, only chips at the confidence of the
   one being tracked.  `seen` says whether the entry has observed an LPR before. */
static Flag critpath_edge_observe(Critpath_PC_Entry* e, Flag seen, Addr cur) {
  if (seen && e->edge_pc == cur) {
    if (e->edge_conf < 3)
      e->edge_conf++;
    return TRUE;
  }
  if (ZERECO_CRITPATH_EDGE_CONF_DECAY && seen && e->edge_conf > 0) {
    e->edge_conf--;
  } else {
    e->edge_pc = cur;
    e->edge_conf = 0;
  }
  return FALSE;
}

/* Filter A: may this entry's edge be followed on this instance?  Only an
   instance that names the tracked producer, and only once that producer has
   proven itself; in reset mode a confident edge always matches.  Members near
   the branch are exempt: a cut there loses everything above it. */
static Flag critpath_edge_passes(const Critpath_PC_Entry* e, Flag match) {
  if (!ZERECO_CRITPATH_EDGE_CONF_MIN)
    return TRUE;
  if ((int)e->depth <= ZERECO_CRITPATH_EDGE_CONF_EXEMPT_DEPTH)
    return TRUE;
  return match && e->edge_conf >= ZERECO_CRITPATH_EDGE_CONF_MIN;
}

/* Filter C: a near-tie means both producers are effectively critical; pushing
   one of them alone buys at most the slack, so neither is followed. */
static Flag critpath_slack_passes(const Op* op) {
  return !(ZERECO_CRITPATH_SLACK_MIN && op->critpath_wake_events >= 2 &&
           op->critpath_last_cycle - op->critpath_second_cycle <
             ZERECO_CRITPATH_SLACK_MIN);
}

/* Make `producer_pc` a member one level below (`my_depth`, `my_owner`).  The
   lookup may evict any entry that shares its set, so callers must have copied
   whatever they still need out of their own entry before calling this. */
static void critpath_propagate_to(uns proc_id, Critpath_Core_State* state,
                                  Critpath_PC_Entry* table, Flag is_main,
                                  Addr producer_pc, uns8 my_depth,
                                  Addr my_owner) {
  Critpath_PC_Entry* producer =
    critpath_table_lookup(proc_id, state, table, producer_pc, TRUE, is_main);
  if (!producer)
    return;
  if (is_main && producer->win_crit < 0xFFFF)
    producer->win_crit++;
  if (!producer->in_slice) {
    producer->in_slice = TRUE;
    producer->depth = my_depth + 1;
    producer->owner_pc = my_owner;
    producer->confirm = 1;
    if (is_main)
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
    if (is_main) {
      STAT_EVENT(proc_id, CRITPATH_OWNER_OVERWRITE);
      if (producer->owner_pc && hbt_is_hard_branch(producer->owner_pc) &&
          !(my_owner && hbt_is_hard_branch(my_owner)))
        STAT_EVENT(proc_id, CRITPATH_OWNER_OVERWRITE_H2P_LOST);
    }
    producer->owner_pc = my_owner;
  } else if (is_main) {
    STAT_EVENT(proc_id, CRITPATH_PROPAGATION_REFRESHED);
  }
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

  /* Decay runs on committed main-thread ops (micro-ops).  The H2P table ages on
     committed x86 instructions instead, so 10K here is about 8K instructions at
     ~1.2 ops per instruction. */
  state->retires++;
  if (ZERECO_CRITPATH_DECAY_INTERVAL &&
      (state->retires % ZERECO_CRITPATH_DECAY_INTERVAL) == 0)
    critpath_decay(proc_id);
  if (ZERECO_CRITPATH_TIMELINE_INTERVAL &&
      (state->retires % ZERECO_CRITPATH_TIMELINE_INTERVAL) == 0)
    critpath_timeline_tick(proc_id, state);

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
  Flag has_lpr = (op->critpath_wake_events > 0);
  Flag edge_match = FALSE;   /* A: this instance names the tracked producer */

  /* Allocate for a member, not for every PC that commits: a miss simply means
     "not a chain member", which is all any consumer asks of this table. */
  Flag allocate = may_seed || !ZERECO_CRITPATH_MEMBER_ONLY_ALLOC;
  Critpath_PC_Entry* entry = critpath_pc_lookup(proc_id, pc, allocate);
  /* Shadow (critical rule) entry, full-slice mode only.  Allocated, seeded and
     aged exactly like the main table; only its propagation rule differs. */
  Critpath_PC_Entry* shadow =
    state->shadow_table
      ? critpath_table_lookup(proc_id, state, state->shadow_table, pc, allocate,
                              FALSE)
      : NULL;

  /* Seed before measuring, not after: an H2P branch is the root of its own
     slice from its very first commit, and taking its slack as non-slice on that
     first pass would bias the slice histograms against the one instruction that
     matters most. */
  if (may_seed) {
    STAT_EVENT(proc_id, CRITPATH_ROOT_COMMITS);
    state->tl_roots++;
    if (entry) {
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
    if (shadow) {
      if (!shadow->in_slice || shadow->owner_pc != pc) {
        shadow->in_slice = TRUE;
        shadow->depth = 0;
        shadow->owner_pc = pc;
      }
      if (shadow->confirm < state->confirm_max)
        shadow->confirm++;
    }
  }
  Flag shadow_member = shadow && shadow->in_slice;

  /* Ops that actually held a priority RS entry -- the bit is cleared when the
     non-stall fallback sends a candidate to a normal entry -- split by whether
     the critical rule would have kept them.  Taken before the main table can
     turn this op away, so a miss there does not silently drop the sample. */
  if (state->shadow_table && op->zereco_iq_priority_bit)
    STAT_EVENT(proc_id, shadow_member ? CRITPATH_PRIORITY_OP_CRITICAL
                                      : CRITPATH_PRIORITY_OP_NONCRITICAL);

  /* The shadow applies the critical rule independently of what the main table
     holds.  Read its fields first: the lookup inside the helper can evict any
     entry sharing the set, including this one. */
  if (shadow_member && has_lpr && !frontier &&
      shadow->depth < CRITPATH_MAX_DEPTH &&
      op->critpath_last_producer_pc != 0) {
    uns8 sh_depth = shadow->depth;
    Addr sh_owner = shadow->owner_pc;
    critpath_propagate_to(proc_id, state, state->shadow_table, FALSE,
                          op->critpath_last_producer_pc, sh_depth, sh_owner);
  }

  if (!entry)
    return;

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
      if (state->shadow_table)
        STAT_EVENT(proc_id, shadow_member ? CRITPATH_TARGET_LOAD_CRITICAL
                                          : CRITPATH_TARGET_LOAD_NONCRITICAL);
    }
    STAT_EVENT(proc_id, CRITPATH_SLICE_OPS);
    state->tl_members++;
    if (entry->win_exec < 0xFFFF)
      entry->win_exec++;
    if (op->table_info->mem_type == MEM_ST)
      STAT_EVENT(proc_id, CRITPATH_SLICE_OPS_STORE);
    if (state->shadow_table)
      STAT_EVENT(proc_id, shadow_member ? CRITPATH_FULL_MEMBER_CRITICAL
                                        : CRITPATH_FULL_MEMBER_NONCRITICAL);
    critpath_record_confirm(proc_id, entry->confirm);
    critpath_record_owner_class(proc_id, entry->owner_pc);
  }

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
  /* "The true last arrival was a store" -- followed when the edge set includes
     memory edges, counted only when it does not. */
  Flag mem_was_last =
    ZERECO_CRITPATH_MEM_EDGE
      ? (has_lpr && op->critpath_last_dep_type == MEM_DATA_DEP)
      : (op->critpath_mem_wake_events > 0 &&
         (!has_lpr || op->critpath_mem_last_cycle > op->critpath_last_cycle));
  if (mem_was_last) {
    STAT_EVENT(proc_id, CRITPATH_LPR_MEM_DEP);
    if (in_slice)
      STAT_EVENT(proc_id, CRITPATH_SLICE_LPR_MEM_DEP);
  } else if (has_lpr) {
    STAT_EVENT(proc_id, CRITPATH_LPR_REG_DEP);
    if (in_slice)
      STAT_EVENT(proc_id, CRITPATH_SLICE_LPR_REG_DEP);
  }

  /* ---- 4. does the critical edge stay the same across instances? ------- */
  if (has_lpr) {
    /* A: the edge to this op's last-arriving producer earns confidence only by
       repeating. */
    edge_match = critpath_edge_observe(entry, entry->has_last,
                                       op->critpath_last_producer_pc);
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
  if (!in_slice)
    return;

  if (ZERECO_CRITPATH_FULL_SLICE) {
    /* PUBS-style: every register producer joins, no frontier stop, no LPR
       needed.  Depth is still bounded and still the shortest distance seen. */
    critpath_record_depth(proc_id, entry->depth);
    if (entry->depth >= CRITPATH_MAX_DEPTH) {
      STAT_EVENT(proc_id, CRITPATH_PROPAGATION_STOPPED_DEPTH);
      return;
    }
    uns8 my_depth = entry->depth;
    Addr my_owner = entry->owner_pc;
    uns num_srcs = op->oracle_info.num_srcs;
    Addr seen[MAX_DEPS];
    uns num_seen = 0;
    for (uns ii = 0; ii < num_srcs; ++ii) {
      Dep_Type t = op->oracle_info.src_info[ii].type;
      if (t != REG_DATA_DEP && !(ZERECO_CRITPATH_MEM_EDGE && t == MEM_DATA_DEP))
        continue;
      Addr producer_pc = op->critpath_src_producer_pc[ii];
      if (producer_pc == 0)
        continue;
      /* One propagation per distinct producer: two operands from the same
         instruction must not confirm it twice per instance. */
      Flag dup = FALSE;
      for (uns jj = 0; jj < num_seen && !dup; ++jj)
        dup = (seen[jj] == producer_pc);
      if (dup)
        continue;
      seen[num_seen++] = producer_pc;
      critpath_propagate_to(proc_id, state, state->pc_table, TRUE,
                            producer_pc, my_depth, my_owner);
    }
    return;
  }

  if (!has_lpr)
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
  if (!critpath_slack_passes(op)) {
    STAT_EVENT(proc_id, CRITPATH_SLACK_BLOCKED);
    return;
  }
  if (!critpath_edge_passes(entry, edge_match)) {
    STAT_EVENT(proc_id, CRITPATH_EDGE_CONF_BLOCKED);
    STAT_EVENT(proc_id, CRITPATH_EDGE_CONF_BLOCKED_D0 + MIN2(entry->depth, 3));
    return;
  }
  /* Read what this op contributes before touching the table again.  The lookup
     inside the helper can evict whichever entry shares its index -- including
     this one -- so `entry` must not be dereferenced afterwards. */
  critpath_propagate_to(proc_id, state, state->pc_table, TRUE,
                        op->critpath_last_producer_pc, entry->depth,
                        entry->owner_pc);
}

/**************************************************************************************/
/* P-IQ in-flight counter.
 *
 * The question this answers is how often no priority op is anywhere between the front end and the reservation
 * station, because that is the condition under which the P-IQ reservation could be handed to the normal stream.  The
 * count starts at the front end rather than at dispatch on purpose: the reservation has to be in place before the op
 * asks for an entry, so the useful signal is the one that arrives earliest.  Nothing reads the value yet, so a run
 * with these counters is cycle-identical to one without them. */

static uns    zereco_piq_in_flight[MAX_NUM_PROCS];
static Counter zereco_piq_zero_run[MAX_NUM_PROCS];

void zereco_piq_inflight_tag(Op* op) {
  if (!op || op->proc_id >= MAX_NUM_PROCS || !op->zereco_iq_priority_bit ||
      op->zereco_piq_inflight_counted)
    return;
  op->zereco_piq_inflight_counted = TRUE;
  op->zereco_piq_tag_cycle = cycle_count;
  zereco_piq_in_flight[op->proc_id]++;
  STAT_EVENT(op->proc_id, ZERECO_PIQ_INFLIGHT_TAGGED_OPS);
}

void zereco_piq_inflight_issued(Op* op) {
  if (!op || op->proc_id >= MAX_NUM_PROCS || !op->zereco_piq_inflight_counted)
    return;
  op->zereco_piq_inflight_counted = FALSE;
  if (zereco_piq_in_flight[op->proc_id] > 0)
    zereco_piq_in_flight[op->proc_id]--;
}

/* An op that never issues -- squashed in the front end, flushed from the node stage, or freed any other way --
 * gives its count back here.  free_op() is the one place every op passes through. */
void zereco_piq_inflight_discarded(Op* op) {
  if (!op || op->proc_id >= MAX_NUM_PROCS || !op->zereco_piq_inflight_counted)
    return;
  op->zereco_piq_inflight_counted = FALSE;
  if (zereco_piq_in_flight[op->proc_id] > 0)
    zereco_piq_in_flight[op->proc_id]--;
  STAT_EVENT(op->proc_id, ZERECO_PIQ_INFLIGHT_SQUASHED_OPS);
}

/* How much warning the counter gives: the front end tags the op here, and it asks for its RS entry that many cycles
 * later. */
void zereco_piq_inflight_admitted_to_rs(Op* op) {
  if (!op || op->proc_id >= MAX_NUM_PROCS || !op->zereco_piq_inflight_counted ||
      !op->zereco_piq_tag_cycle || op->zereco_piq_tag_cycle > cycle_count)
    return;
  INC_STAT_EVENT(op->proc_id, ZERECO_PIQ_PRIORITY_FETCH_TO_RS_TOTAL,
                 cycle_count - op->zereco_piq_tag_cycle);
  STAT_EVENT(op->proc_id, ZERECO_PIQ_PRIORITY_FETCH_TO_RS_OPS);
}

Flag zereco_piq_reservation_engaged(uns proc_id) {
  if (!ZERECO_PIQ_RESERVATION_DYNAMIC || proc_id >= MAX_NUM_PROCS)
    return TRUE;
  if (zereco_piq_in_flight[proc_id])
    return TRUE;
  return zereco_piq_zero_run[proc_id] < (Counter)ZERECO_PIQ_RELEASE_DWELL + 1;
}

void zereco_piq_inflight_sample(uns proc_id) {
  if (proc_id >= MAX_NUM_PROCS)
    return;
  uns v = zereco_piq_in_flight[proc_id];
  INC_STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_TOTAL, v);

  if (v == 0) {
    STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ZERO_CYCLES);
    if (zereco_piq_zero_run[proc_id] == 0)
      STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ZERO_RUNS);
    zereco_piq_zero_run[proc_id]++;
    /* each stretch is counted once, on the cycle it grows past the threshold */
    if (zereco_piq_zero_run[proc_id] == 5)
      STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ZERO_RUNS_ABOVE_4);
    if (zereco_piq_zero_run[proc_id] == 17)
      STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ZERO_RUNS_ABOVE_16);
    if (zereco_piq_zero_run[proc_id] == 65)
      STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ZERO_RUNS_ABOVE_64);
    if (zereco_piq_zero_run[proc_id] == 257)
      STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ZERO_RUNS_ABOVE_256);
    return;
  }

  zereco_piq_zero_run[proc_id] = 0;
  if (v > 1)
    STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ABOVE_1);
  if (v > 2)
    STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ABOVE_2);
  if (v > 4)
    STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ABOVE_4);
  if (v > 8)
    STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ABOVE_8);
  if (v > 16)
    STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ABOVE_16);
  if (v > 32)
    STAT_EVENT(proc_id, ZERECO_PIQ_INFLIGHT_ABOVE_32);
}
