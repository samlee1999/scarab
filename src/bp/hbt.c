#include "bp/hbt.h"
#include "globals/global_types.h"
#include "op.h"
#include "bp/bp_conf.h"
#include "bp/bp.param.h"
#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "globals/assert.h"
#include "globals/utils.h"
#include "statistics.h"
#include <stdlib.h>
#include <string.h>

/* TEA paper Table II / Section IV-B: 256 entries, 8-way, allocated on a misprediction with the counter at 1, H2P
   above 1, all counters decremented every 50K retired instructions, counter-0 entries replaced first.  This is the
   table of the test (CLEAR) branch with full tags; the geometry comes from hbt_entries / hbt_assoc.  hbt_assoc 1 is
   the direct-mapped table this branch used before: every H2P decision and counter it reports is the same. */
static HbtEntry* hbt_table = NULL;
static uns       hbt_sets  = 0;
static uns       hbt_ways  = 0;
static Counter   hbt_clock = 0; /* monotonic stamp for the LRU field */

uns64 retired_branch_count          = 0;
uns64 hbt_retired_instruction_count = 0;

static inline uns       hbt_set_of(Addr pc) { return (uns)(pc % hbt_sets); }
static inline uns64     hbt_tag_of(Addr pc) { return (uns64)(pc / hbt_sets); }
static inline HbtEntry* hbt_set_base(Addr pc) { return &hbt_table[(size_t)hbt_set_of(pc) * hbt_ways]; }

/* The way holding this PC, or NULL.  Read-only: it does not age the entry. */
static HbtEntry* hbt_find(Addr pc) {
  if (!hbt_table)
    return NULL;
  HbtEntry* set = hbt_set_base(pc);
  uns64     tag = hbt_tag_of(pc);
  for (uns w = 0; w < hbt_ways; ++w)
    if (set[w].valid && set[w].tag == tag)
      return &set[w];
  return NULL;
}

// 논문 기준: "All counters are decremented by 1 every 50K instructions"
static void hbt_periodic_decrement(uns proc_id) {
  uns     entries = hbt_sets * hbt_ways;
  Counter live = 0, hard = 0;
  for (uns i = 0; i < entries; i++) {
    if (!hbt_table[i].valid)
      continue;
    live++;
    if (hbt_table[i].counter > 0)
      hbt_table[i].counter--;
    /* An entry whose counter reaches 0 stays resident -- it simply becomes the first thing a later allocation will
       replace (paper, Section IV-B). */
    if (hbt_table[i].counter > HBT_H2P_THRESHOLD)
      hard++;
  }
  INC_STAT_EVENT(proc_id, HBT_RESIDENT_ENTRIES_TOTAL, live);
  INC_STAT_EVENT(proc_id, HBT_RESIDENT_HARD_TOTAL, hard);
}

void hbt_init(void) {
  hbt_ways    = HBT_ASSOC ? HBT_ASSOC : 1;
  uns entries = HBT_ENTRIES ? HBT_ENTRIES : hbt_ways;
  ASSERTM(0, entries % hbt_ways == 0, "hbt_entries (%u) must be a multiple of hbt_assoc (%u)\n", entries, hbt_ways);
  hbt_sets = entries / hbt_ways;
  free(hbt_table);
  hbt_table = (HbtEntry*)calloc(entries, sizeof(HbtEntry));
  ASSERTM(0, hbt_table, "Failed to allocate the H2P branch table\n");
  hbt_clock                     = 0;
  retired_branch_count          = 0;
  hbt_retired_instruction_count = 0;
  _DEBUG(0, DEBUG_HBT, "HBT initialized: %u entries, %u-way, %u sets.\n", entries, hbt_ways, hbt_sets);
}

void hbt_retire_instruction_tick(uns proc_id) {
  hbt_retired_instruction_count++;
  STAT_EVENT(proc_id, HBT_RETIRED_INST_TICKS);

  if ((hbt_retired_instruction_count % HBT_DECAY_INTERVAL) == 0) {
    hbt_periodic_decrement(proc_id);
    STAT_EVENT(proc_id, HBT_DECAY_EVENTS);
  }
}

/* Called for every retired branch. */
void hbt_update(Op* op) {
  Addr pc      = op->inst_info->addr;
  Flag mispred = op->oracle_info.mispred | op->oracle_info.misfetch;

  retired_branch_count++;
  STAT_EVENT(op->proc_id, HBT_RETIRED_BRANCH_UPDATES);

  HbtEntry* entry = hbt_find(pc);
  hbt_clock++;

  if (entry) {
    entry->lru = hbt_clock;
    STAT_EVENT(op->proc_id, HBT_UPDATE_HITS);
    if (mispred) {
      /* 예측에 실패했을 때만 카운터를 1 증가시킴 (saturating) */
      entry->counter = SAT_INC(entry->counter, HBT_CTR_MAX);
      STAT_EVENT(op->proc_id, HBT_MISPRED_UPDATES);
    }
    return;
  }

  /* Miss.  The paper allocates only on a misprediction -- a branch that always predicts correctly never takes a
     slot -- and the new entry starts at 1. */
  if (!mispred) {
    STAT_EVENT(op->proc_id, HBT_UPDATE_MISS_NO_ALLOC);
    return;
  }

  HbtEntry* set    = hbt_set_base(pc);
  HbtEntry* victim = NULL;
  for (uns w = 0; w < hbt_ways; ++w) { /* an empty way first */
    if (!set[w].valid) {
      victim = &set[w];
      break;
    }
  }
  if (!victim) { /* then a counter-0 way, least recent */
    for (uns w = 0; w < hbt_ways; ++w)
      if (set[w].counter == 0 && (!victim || set[w].lru < victim->lru))
        victim = &set[w];
    if (victim)
      STAT_EVENT(op->proc_id, HBT_ALLOC_EVICTED_COLD);
  }
  if (!victim) {
    /* Every way holds a branch that has mispredicted recently: this branch is not tracked this time round. */
    STAT_EVENT(op->proc_id, HBT_ALLOC_BLOCKED);
    return;
  }

  victim->valid   = TRUE;
  victim->tag     = hbt_tag_of(pc);
  victim->pc      = pc;
  victim->counter = 1;
  victim->lru     = hbt_clock;
  STAT_EVENT(op->proc_id, HBT_ALLOCATIONS);
}

/* 논문 기준: counter > 1이면 H2P */
Flag hbt_is_hard_branch(Addr pc) {
  const HbtEntry* entry = hbt_find(pc);
  return (entry && entry->counter > HBT_H2P_THRESHOLD) ? TRUE : FALSE;
}

uns32 hbt_get_counter(Addr pc) {
  const HbtEntry* entry = hbt_find(pc);
  return entry ? entry->counter : 0;
}
