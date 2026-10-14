/***************************************************************************************
 * File         : zereco/branch_state.c
 * Description  : Per-branch H2P state ledger -- measurement only (see the header).
 ***************************************************************************************/

#include "zereco/branch_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp/hbt.h"
#include "core.param.h"
#include "globals/assert.h"
#include "globals/global_types.h"
#include "globals/utils.h"
#include "op.h"
#include "statistics.h"

/**************************************************************************************/
/* One record per static branch PC. */

#define BS_CTR_SLOTS (HBT_CTR_MAX + 1)

typedef struct Branch_Record_struct {
  Addr    pc;
  Flag    used;

  Counter commits;                      /* retired instances of this branch */
  Counter mispredicts;

  /* The real HBT (256 entries, 8-way).  A branch with no entry reads 0. */
  uns8    real_counter;                 /* value carried forward after the last update */
  uns8    real_max;                     /* highest value this branch ever held */
  Counter commits_by_counter[BS_CTR_SLOTS]; /* the value that governed each instance */
  Counter commits_h2p;                  /* instances governed by counter > 1 */
  Counter promotions;                   /* crossings up over the H2P threshold */
  Counter demotions;
  Flag    was_h2p;
  Counter state_start_ops;              /* global retired ops when this state began */

  /* Lead time: instances the branch runs while tracked (counter >= 1) but not
     yet H2P, before its first promotion.  This is the warm-up a low admission
     threshold would hand to the chain for free. */
  Flag    seen_tracked;
  Flag    lead_done;
  Counter lead_instances;

  /* The current stretch at counter 1 (Table 6): whether it is open, whether it
     was entered from H2P (a cool-down) or from 0 (a run-up), the instances run
     in it, and the learning brslice_tab did for this branch's chain meanwhile. */
  Flag    prep_open;
  Flag    prep_from_h2p;
  Counter prep_instances;
  Counter prep_counts;
  Counter prep_regs;

  /* H2P instances since the latest promotion, this one included (1 = the
     instance that crossed the threshold); read by the ledger's age bins. */
  Counter h2p_age;

  /* Shadow HBT: same rules, unbounded. */
  Flag    shadow_valid;
  uns8    shadow_counter;
  uns8    shadow_max;
  Counter commits_shadow_h2p;
} Branch_Record;

/**************************************************************************************/
/* Open-addressing table, power-of-two capacity, linear probing. */

static Branch_Record* bs_table    = NULL;
static uns            bs_capacity = 0;
static uns            bs_count    = 0;
static Counter        bs_ops      = 0;  /* retired instructions, the dwell clock */
static Counter        bs_ticks    = 0;  /* for the shadow decay (hbt_decay_interval) */

/* Accumulated here rather than through STAT_EVENT as they happen: the statistics
   counters are reset at every periodic dump, while the ledger spans the whole run.
   Emitting all five tables together at the end keeps them on one time base. */
static Counter bs_dwell_h2p[4];   /* completed H2P intervals,  < 1K / 1K-10K / 10K-100K / 100K+ */
static Counter bs_dwell_cold[4];  /* completed non-H2P intervals, same buckets */
static Counter bs_promotions;
static Counter bs_demotions;
static Counter bs_open_h2p;       /* intervals still open when the run ended (censored) */
static Counter bs_open_cold;
static Counter bs_decays;
static Flag    bs_emitted;  /* the ledger is global; emit it once even on several cores */

/* Table 6: stretches at counter 1 by how they began (0 = from 0, a run-up; 1 = from
   H2P, a cool-down) and how they ended (0 = promoted, 1 = lapsed, 2 = open at the
   end), and per class: stretches, instances, counts, registrations. */
enum { BS_PREP_N, BS_PREP_INST, BS_PREP_COUNTS, BS_PREP_REGS, BS_PREP_MEASURES };
static Counter bs_prep[2][3][BS_PREP_MEASURES];

static inline uns bs_hash(Addr pc) {
  uns64 h = (uns64)pc;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 29;
  return (uns)h;
}

static void bs_alloc(uns capacity) {
  bs_table    = (Branch_Record*)calloc(capacity, sizeof(Branch_Record));
  ASSERTM(0, bs_table, "Failed to allocate the branch-state ledger\n");
  bs_capacity = capacity;
}

static Branch_Record* bs_slot(Branch_Record* table, uns capacity, Addr pc) {
  uns mask = capacity - 1;
  uns i    = bs_hash(pc) & mask;
  while (table[i].used && table[i].pc != pc)
    i = (i + 1) & mask;
  return &table[i];
}

static void bs_grow(void) {
  Branch_Record* old      = bs_table;
  uns            old_cap  = bs_capacity;
  bs_alloc(old_cap * 2);
  for (uns i = 0; i < old_cap; ++i) {
    if (old[i].used)
      *bs_slot(bs_table, bs_capacity, old[i].pc) = old[i];
  }
  free(old);
}

static Branch_Record* bs_get(Addr pc) {
  if (!bs_table)
    return NULL;
  if ((bs_count + 1) * 10 >= bs_capacity * 7)
    bs_grow();
  Branch_Record* r = bs_slot(bs_table, bs_capacity, pc);
  if (!r->used) {
    r->used = TRUE;
    r->pc   = pc;
    r->state_start_ops = bs_ops;
    bs_count++;
  }
  return r;
}

/**************************************************************************************/

void branch_state_init(void) {
  free(bs_table);
  bs_table    = NULL;
  bs_capacity = 0;
  bs_count    = 0;
  bs_ops      = 0;
  bs_ticks    = 0;
  memset(bs_dwell_h2p, 0, sizeof(bs_dwell_h2p));
  memset(bs_dwell_cold, 0, sizeof(bs_dwell_cold));
  bs_promotions = bs_demotions = bs_open_h2p = bs_open_cold = 0;
  bs_decays = 0;
  memset(bs_prep, 0, sizeof(bs_prep));
  bs_emitted = FALSE;
  if (!ZERECO_BRANCH_STATE_PROFILE)
    return;
  bs_alloc(4096);
}

/* Dwell of one state, in retired instructions, so it compares directly with the
   brslice_tab refresh window (10K committed ops). */
static uns bs_dwell_bucket(Counter ops) {
  if (ops < 1000)
    return 0;
  if (ops < 10000)
    return 1;
  if (ops < 100000)
    return 2;
  return 3;
}

static void bs_record_dwell(Flag was_h2p, Counter ops) {
  if (was_h2p)
    bs_dwell_h2p[bs_dwell_bucket(ops)]++;
  else
    bs_dwell_cold[bs_dwell_bucket(ops)]++;
}

/* Close the branch's stretch at counter 1 with `outcome` (0 promoted, 1 lapsed,
   2 open at the end) and file it under Table 6. */
static void bs_close_prep(Branch_Record* r, uns outcome) {
  Counter* c = bs_prep[r->prep_from_h2p ? 1 : 0][outcome];
  c[BS_PREP_N]++;
  c[BS_PREP_INST] += r->prep_instances;
  c[BS_PREP_COUNTS] += r->prep_counts;
  c[BS_PREP_REGS] += r->prep_regs;
  r->prep_open = FALSE;
}

void branch_state_note_retire(Op* op, uns32 hbt_counter) {
  if (!ZERECO_BRANCH_STATE_PROFILE || !op || op->thread_id != 0 || op->off_path)
    return;
  Branch_Record* r = bs_get(op->inst_info->addr);
  if (!r)
    return;
  const Flag  mispred = op->oracle_info.mispred | op->oracle_info.misfetch;
  const uns32 ctr = hbt_counter > HBT_CTR_MAX ? HBT_CTR_MAX : hbt_counter;

  r->commits++;
  if (mispred)
    r->mispredicts++;
  r->commits_by_counter[ctr]++;
  if (ctr > HBT_H2P_THRESHOLD)
    r->commits_h2p++;

  /* The shadow HBT: identical rules, no capacity limit.  Updated before it is read,
     the same order the real table is updated (bp_retire_op) and read (two lines
     later in node_stage.c) in. */
  if (mispred) {
    if (!r->shadow_valid) {
      r->shadow_valid   = TRUE;
      r->shadow_counter = 1;
    } else {
      r->shadow_counter = SAT_INC(r->shadow_counter, HBT_CTR_MAX);
    }
    if (r->shadow_counter > r->shadow_max)
      r->shadow_max = r->shadow_counter;
  }
  if (r->shadow_valid && r->shadow_counter > HBT_H2P_THRESHOLD)
    r->commits_shadow_h2p++;

  /* Table 6: a stretch at counter 1 ends when the counter leaves 1 -- promoted
     if it rose past the H2P threshold, lapsed if it fell to 0 (decayed, or the
     branch lost its HBT entry) -- and a new one starts whenever it reaches 1. */
  const uns8 prev = r->real_counter;
  if (r->prep_open && ctr != 1)
    bs_close_prep(r, ctr > HBT_H2P_THRESHOLD ? 0 : 1);
  if (ctr == 1 && !r->prep_open) {
    r->prep_open      = TRUE;
    r->prep_from_h2p  = prev > HBT_H2P_THRESHOLD;
    r->prep_instances = r->prep_counts = r->prep_regs = 0;
  }
  if (r->prep_open)
    r->prep_instances++;

  r->real_counter = (uns8)ctr;
  if (r->real_counter > r->real_max)
    r->real_max = r->real_counter;
  if (r->real_counter >= 1)
    r->seen_tracked = TRUE;
  /* Instances the branch runs tracked (counter >= 1) but not yet H2P, up to its
     first crossing: the warm-up a low admission threshold would hand the chain. */
  if (r->seen_tracked && !r->lead_done && ctr <= HBT_H2P_THRESHOLD)
    r->lead_instances++;

  Flag now_h2p = ctr > HBT_H2P_THRESHOLD;
  if (now_h2p != r->was_h2p) {
    bs_record_dwell(r->was_h2p, bs_ops - r->state_start_ops);
    r->state_start_ops = bs_ops;
    r->was_h2p         = now_h2p;
    if (now_h2p) {
      r->promotions++;
      r->lead_done = TRUE;
      bs_promotions++;
      r->h2p_age = 0;
    } else {
      r->demotions++;
      bs_demotions++;
    }
  }
  if (now_h2p)
    r->h2p_age++;
}

Counter branch_state_h2p_age(Addr branch_pc) {
  if (!ZERECO_BRANCH_STATE_PROFILE || !bs_table)
    return 0;
  const Branch_Record* r = bs_slot(bs_table, bs_capacity, branch_pc);  /* look up, never allocate */
  return (r->used && r->was_h2p) ? r->h2p_age : 0;
}

void branch_state_note_prep(Addr branch_pc, uns counts, uns regs) {
  if (!ZERECO_BRANCH_STATE_PROFILE || !bs_table || !branch_pc)
    return;
  Branch_Record* r = bs_slot(bs_table, bs_capacity, branch_pc);  /* look up, never allocate */
  if (!r->used || !r->prep_open)
    return;
  r->prep_counts += counts;
  r->prep_regs += regs;
}

void branch_state_retire_tick(uns proc_id) {
  (void)proc_id;
  if (!ZERECO_BRANCH_STATE_PROFILE || !bs_table)
    return;
  bs_ops++;
  bs_ticks++;
  if (bs_ticks % HBT_DECAY_INTERVAL)
    return;
  /* The shadow table decays exactly like the real one. */
  for (uns i = 0; i < bs_capacity; ++i) {
    Branch_Record* r = &bs_table[i];
    if (!r->used || !r->shadow_valid || r->shadow_counter == 0)
      continue;
    r->shadow_counter--;
  }
  bs_decays++;
}

/**************************************************************************************/
/* The population tables, emitted once at the end of the run so that they land in
   the same statistics window as the Periodic region the analysis reads. */

static void bs_emit_max_counter(uns proc_id, const Branch_Record* r) {
  Counter c = r->commits, h = r->commits_h2p;
  switch (r->real_max) {
    case 0:
      STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR0_BRANCHES);
      INC_STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR0_COMMITS, c);
      break;
    case 1:
      STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR1_BRANCHES);
      INC_STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR1_COMMITS, c);
      break;
    case 2:
      STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR2_BRANCHES);
      INC_STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR2_COMMITS, c);
      INC_STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR2_H2P_COMMITS, h);
      break;
    case 3:
      STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR3_BRANCHES);
      INC_STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR3_COMMITS, c);
      INC_STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR3_H2P_COMMITS, h);
      break;
    default:
      STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR4_7_BRANCHES);
      INC_STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR4_7_COMMITS, c);
      INC_STAT_EVENT(proc_id, BRANCH_STATE_MAXCTR4_7_H2P_COMMITS, h);
      break;
  }
}

static void bs_emit_share(uns proc_id, const Branch_Record* r) {
  double share = r->commits ? 100.0 * (double)r->commits_h2p / (double)r->commits : 0.0;
  Counter c = r->commits;
  if (r->commits_h2p == 0) {
    STAT_EVENT(proc_id, BRANCH_STATE_SHARE_0_BRANCHES);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_SHARE_0_COMMITS, c);
  } else if (share < 1.0) {
    STAT_EVENT(proc_id, BRANCH_STATE_SHARE_LT1_BRANCHES);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_SHARE_LT1_COMMITS, c);
  } else if (share < 10.0) {
    STAT_EVENT(proc_id, BRANCH_STATE_SHARE_1_10_BRANCHES);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_SHARE_1_10_COMMITS, c);
  } else if (share < 50.0) {
    STAT_EVENT(proc_id, BRANCH_STATE_SHARE_10_50_BRANCHES);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_SHARE_10_50_COMMITS, c);
  } else if (share < 99.5) {
    STAT_EVENT(proc_id, BRANCH_STATE_SHARE_50_99_BRANCHES);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_SHARE_50_99_COMMITS, c);
  } else {
    STAT_EVENT(proc_id, BRANCH_STATE_SHARE_100_BRANCHES);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_SHARE_100_COMMITS, c);
  }
}

static void bs_emit_lead(uns proc_id, const Branch_Record* r) {
  if (!r->promotions)   /* only branches that actually crossed have a lead time */
    return;
  Counter n = r->lead_instances;
  STAT_EVENT(proc_id, BRANCH_STATE_LEAD_BRANCHES);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_LEAD_INSTANCES_TOTAL, n);
  if (n < 10)
    STAT_EVENT(proc_id, BRANCH_STATE_LEAD_LT10);
  else if (n < 100)
    STAT_EVENT(proc_id, BRANCH_STATE_LEAD_10_100);
  else if (n < 1000)
    STAT_EVENT(proc_id, BRANCH_STATE_LEAD_100_1K);
  else if (n < 10000)
    STAT_EVENT(proc_id, BRANCH_STATE_LEAD_1K_10K);
  else
    STAT_EVENT(proc_id, BRANCH_STATE_LEAD_10K_PLUS);
}

static void bs_emit_flips(uns proc_id, const Branch_Record* r) {
  Counter f = r->promotions;
  if (f == 0)
    STAT_EVENT(proc_id, BRANCH_STATE_FLIPS_0);
  else if (f == 1)
    STAT_EVENT(proc_id, BRANCH_STATE_FLIPS_1);
  else if (f <= 3)
    STAT_EVENT(proc_id, BRANCH_STATE_FLIPS_2_3);
  else if (f <= 7)
    STAT_EVENT(proc_id, BRANCH_STATE_FLIPS_4_7);
  else
    STAT_EVENT(proc_id, BRANCH_STATE_FLIPS_8_PLUS);
}

static void bs_emit_shadow(uns proc_id, const Branch_Record* r) {
  Flag real_h2p   = r->real_max > HBT_H2P_THRESHOLD;
  Flag shadow_h2p = r->shadow_max > HBT_H2P_THRESHOLD;
  INC_STAT_EVENT(proc_id, BRANCH_STATE_SHADOW_H2P_COMMITS, r->commits_shadow_h2p);
  if (shadow_h2p && real_h2p) {
    STAT_EVENT(proc_id, BRANCH_STATE_XTAB_BOTH);
  } else if (shadow_h2p) {
    /* The real 256-entry table lost this branch to a conflict: a true H2P that
       nothing gated on H2P status can see. */
    STAT_EVENT(proc_id, BRANCH_STATE_XTAB_SHADOW_ONLY);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_XTAB_SHADOW_ONLY_COMMITS, r->commits);
  } else if (real_h2p) {
    STAT_EVENT(proc_id, BRANCH_STATE_XTAB_REAL_ONLY);
  } else {
    STAT_EVENT(proc_id, BRANCH_STATE_XTAB_NEITHER);
  }
}

static void bs_dump_csv(void) {
  FILE* f = fopen("branch_state.csv", "w");
  if (!f)
    return;
  fprintf(f,
          "pc,commits,mispredicts,real_max,real_counter_end,commits_h2p,promotions,demotions,"
          "lead_instances,shadow_max,commits_shadow_h2p,"
          "c0,c1,c2,c3,c4,c5,c6,c7\n");
  for (uns i = 0; i < bs_capacity; ++i) {
    const Branch_Record* r = &bs_table[i];
    if (!r->used)
      continue;
    fprintf(f, "0x%llx,%llu,%llu,%u,%u,%llu,%llu,%llu,%llu,%u,%llu",
            (unsigned long long)r->pc, (unsigned long long)r->commits,
            (unsigned long long)r->mispredicts, r->real_max, r->real_counter,
            (unsigned long long)r->commits_h2p, (unsigned long long)r->promotions,
            (unsigned long long)r->demotions, (unsigned long long)r->lead_instances,
            r->shadow_max, (unsigned long long)r->commits_shadow_h2p);
    for (uns c = 0; c < BS_CTR_SLOTS; ++c)
      fprintf(f, ",%llu", (unsigned long long)r->commits_by_counter[c]);
    fprintf(f, "\n");
  }
  fclose(f);
}

void branch_state_done(uns proc_id) {
  if (!ZERECO_BRANCH_STATE_PROFILE || !bs_table || bs_emitted)
    return;
  bs_emitted = TRUE;
  for (uns i = 0; i < bs_capacity; ++i) {
    const Branch_Record* r = &bs_table[i];
    if (!r->used)
      continue;
    STAT_EVENT(proc_id, BRANCH_STATE_BRANCHES_TOTAL);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_COMMITS_TOTAL, r->commits);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_MISPREDICTS_TOTAL, r->mispredicts);
    INC_STAT_EVENT(proc_id, BRANCH_STATE_H2P_COMMITS_TOTAL, r->commits_h2p);
    bs_emit_max_counter(proc_id, r);
    bs_emit_share(proc_id, r);
    bs_emit_lead(proc_id, r);
    bs_emit_flips(proc_id, r);
    bs_emit_shadow(proc_id, r);
    /* The interval the branch is in when the run ends never completed, so it is
       counted apart rather than biasing the dwell histogram upwards. */
    if (r->was_h2p)
      bs_open_h2p++;
    else
      bs_open_cold++;
    /* Likewise a stretch at counter 1 still running: its outcome is unknown. */
    if (r->prep_open)
      bs_close_prep(&bs_table[i], 2);
  }
  static const uns prep_stat[2][3] = {
    {BRANCH_STATE_RUNUP_PROMOTED, BRANCH_STATE_RUNUP_LAPSED, BRANCH_STATE_RUNUP_OPEN},
    {BRANCH_STATE_COOLDOWN_PROMOTED, BRANCH_STATE_COOLDOWN_LAPSED, BRANCH_STATE_COOLDOWN_OPEN}};
  for (uns from = 0; from < 2; ++from)
    for (uns outcome = 0; outcome < 3; ++outcome)
      for (uns m = 0; m < BS_PREP_MEASURES; ++m)  /* the four stats of a class are consecutive */
        INC_STAT_EVENT(proc_id, prep_stat[from][outcome] + m, bs_prep[from][outcome][m]);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_PROMOTIONS, bs_promotions);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_DEMOTIONS, bs_demotions);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_H2P_DWELL_LT1K, bs_dwell_h2p[0]);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_H2P_DWELL_1K_10K, bs_dwell_h2p[1]);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_H2P_DWELL_10K_100K, bs_dwell_h2p[2]);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_H2P_DWELL_100K_PLUS, bs_dwell_h2p[3]);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_COLD_DWELL_LT1K, bs_dwell_cold[0]);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_COLD_DWELL_1K_10K, bs_dwell_cold[1]);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_COLD_DWELL_10K_100K, bs_dwell_cold[2]);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_COLD_DWELL_100K_PLUS, bs_dwell_cold[3]);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_H2P_OPEN_AT_END, bs_open_h2p);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_COLD_OPEN_AT_END, bs_open_cold);
  INC_STAT_EVENT(proc_id, BRANCH_STATE_SHADOW_DECAYS, bs_decays);
  if (ZERECO_BRANCH_STATE_DUMP)
    bs_dump_csv();
}
