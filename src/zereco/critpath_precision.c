/***************************************************************************************
 * File         : zereco/critpath_precision.c
 * Description  : Per-instance criticality of accelerated ops -- measurement only (see the header).
 ***************************************************************************************/

#include "zereco/critpath_precision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp/hbt.h"
#include "zereco/branch_state.h"
#include "zereco/critpath.h"
#include "core.param.h"
#include "globals/assert.h"
#include "general.param.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "op.h"
#include "statistics.h"

/**************************************************************************************/
/* One record per committed op, in a ring indexed by op_num.  The ring only has to be
   longer than the distance between a chain's producers and the branch they feed. */

#define PREC_RING_BITS 16
#define PREC_RING_SIZE (1u << PREC_RING_BITS)
#define PREC_RING_MASK (PREC_RING_SIZE - 1u)
/* The walk's horizon is the learning rule's: brslice_tab stops propagating at
   CRITPATH_MAX_DEPTH, so no identifier built on it can reach further.  Level 2 is
   "on an H2P branch instance's critical chain within this many steps"; a chain that
   continues past it is counted apart (CRITPATH_PREC_WALK_MAXDEPTH). */
#define PREC_MAX_WALK CRITPATH_MAX_DEPTH

enum {
  PREC_CAND = 1u << 0,     /* carried the priority bit into dispatch */
  PREC_ENTRY = 1u << 1,    /* actually took a priority RS entry (no fallback) */
  PREC_MEMBER = 1u << 2,   /* brslice_tab says this PC is on a chain */
  PREC_L1 = 1u << 3,       /* some consumer named this instance as its last-arriving source */
  PREC_L1M = 1u << 7,      /* ... and that consumer is itself a tracked chain member */
  PREC_L2 = 1u << 4,       /* on the backward chain of an H2P branch instance */
  PREC_L2_MISP = 1u << 5,  /* ... of one that actually mispredicted */
  PREC_VALID = 1u << 6,
  PREC_RFP = 1u << 8,      /* the load's value came from a useful register-file prefetch */
};

typedef struct {
  Counter op_num;
  Counter lpr_op_num; /* the instance that woke it last; 0 = none (frontier) */
  Addr    pc;
  uns32   ord;      /* commit ordinal, the trace's key: the same instance has the same one in every run */
  uns16   lpr_dist; /* commits back to the last-arriving producer when its record was found; 0 = none */
  uns16   flags;
  uns8    depth; /* distance from the branch when PREC_L2 is set, else 0xFF */
} Prec_Record;

static Prec_Record* ring = NULL;

/**************************************************************************************/
/* Per-instance trace (ZERECO_CRITPATH_PRECISION_TRACE).  Two runs of one SimPoint commit
   the same op stream, so the k-th committed op is the same instance in both, and the runs
   can be compared op by op -- e.g. "on the chain without acceleration" against "accelerated,
   and still on the chain" (TODO N-4).  One entry per committed op of the measurement window,
   written when its record leaves the ring, i.e. once its verdict is final; the backward
   link lets the chains be rebuilt offline from any set of branch instances. */

#define PREC_TRACE_BLOCK_BITS 12 /* one PC check value per 4,096 ops */

enum {
  PREC_TR_L2 = 1u << 0,      /* on an H2P branch instance's critical chain in this run */
  PREC_TR_ROOT = 1u << 1,    /* is itself such a branch instance (depth 0) */
  PREC_TR_CAND = 1u << 2,    /* carried the priority bit */
  PREC_TR_ENTRY = 1u << 3,   /* took a priority RS entry */
  PREC_TR_RFP = 1u << 4,     /* got its value from a useful RF prefetch */
  PREC_TR_L2_MISP = 1u << 5, /* on the chain of a mispredicted one (a root: it mispredicted) */
  PREC_TR_L1 = 1u << 6,      /* last-arriving source of its consumer */
  PREC_TR_MEMBER = 1u << 7,  /* brslice_tab tracked the PC */
};

static uns32  commit_ord = 0;  /* ops committed so far */
static uns32  trace_start = 0; /* ordinal of the window's first op; 0 = warm-up not over yet */
static uns8*  tr_flags = NULL;
static uns16* tr_lpr = NULL;
static uns64* tr_pcx = NULL; /* XOR of the PCs of each block: lets two traces prove they are aligned */
static uns32  tr_cap = 0;
static uns32  tr_count = 0; /* one past the highest entry written */

static void prec_trace_grow(uns32 need) {
  uns32 cap = tr_cap ? tr_cap : (1u << 24);
  while (cap < need)
    cap *= 2;
  if (cap == tr_cap)
    return;
  const uns32 old_blocks = tr_cap >> PREC_TRACE_BLOCK_BITS, blocks = cap >> PREC_TRACE_BLOCK_BITS;
  tr_flags = (uns8*)realloc(tr_flags, cap);
  tr_lpr = (uns16*)realloc(tr_lpr, (size_t)cap * sizeof(uns16));
  tr_pcx = (uns64*)realloc(tr_pcx, (size_t)blocks * sizeof(uns64));
  ASSERTM(0, tr_flags && tr_lpr && tr_pcx, "Failed to allocate the criticality trace\n");
  memset(tr_flags + tr_cap, 0, cap - tr_cap);
  memset(tr_lpr + tr_cap, 0, (size_t)(cap - tr_cap) * sizeof(uns16));
  memset(tr_pcx + old_blocks, 0, (size_t)(blocks - old_blocks) * sizeof(uns64));
  tr_cap = cap;
}

/* Little-endian, in this order: "CPTRACE1", uns32 version (1), uns32 ops per PC block,
   uns64 ordinal of the first entry, uns64 entries, uns64 blocks, then uns8 flags[entries],
   uns16 lpr_dist[entries] and uns64 pc_xor[blocks]. */
static void prec_trace_write(void) {
  FILE* f = fopen("critpath_precision_trace.bin", "wb");
  if (!f)
    return;
  const uns32 block = 1u << PREC_TRACE_BLOCK_BITS;
  const uns32 head32[2] = {1, block};
  const uns64 head64[3] = {trace_start, tr_count, (tr_count + block - 1) >> PREC_TRACE_BLOCK_BITS};
  fwrite("CPTRACE1", 1, 8, f);
  fwrite(head32, sizeof(uns32), 2, f);
  fwrite(head64, sizeof(uns64), 3, f);
  fwrite(tr_flags, 1, tr_count, f);
  fwrite(tr_lpr, sizeof(uns16), tr_count, f);
  fwrite(tr_pcx, sizeof(uns64), head64[2], f);
  fclose(f);
}

/**************************************************************************************/
/* One record per static PC, unbounded: criticality may be a property of the PC or of
   the instance, and only the per-PC spread can tell the two apart. */

typedef struct {
  Addr    pc;
  Flag    used;
  Counter commits;
  Counter cand;
  Counter entry;
  Counter l1;
  Counter l2;
  Counter l2_misp;
  Counter cand_l1;
  Counter cand_l2;
} Prec_PC;

static Prec_PC* pc_table = NULL;
static uns      pc_capacity = 0;
static uns      pc_count = 0;
static Flag     prec_emitted = FALSE;

static inline uns prec_hash(Addr pc) {
  uns64 h = (uns64)pc;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 29;
  return (uns)h;
}

static void pc_alloc(uns capacity) {
  pc_table = (Prec_PC*)calloc(capacity, sizeof(Prec_PC));
  ASSERTM(0, pc_table, "Failed to allocate the criticality PC table\n");
  pc_capacity = capacity;
}

static Prec_PC* pc_slot(Prec_PC* table, uns capacity, Addr pc) {
  uns mask = capacity - 1;
  uns i = prec_hash(pc) & mask;
  while (table[i].used && table[i].pc != pc)
    i = (i + 1) & mask;
  return &table[i];
}

static Prec_PC* pc_get(Addr pc) {
  if (!pc_table)
    return NULL;
  if ((pc_count + 1) * 10 >= pc_capacity * 7) {
    Prec_PC* old = pc_table;
    uns      old_cap = pc_capacity;
    pc_alloc(old_cap * 2);
    for (uns i = 0; i < old_cap; ++i) {
      if (old[i].used)
        *pc_slot(pc_table, pc_capacity, old[i].pc) = old[i];
    }
    free(old);
  }
  Prec_PC* r = pc_slot(pc_table, pc_capacity, pc);
  if (!r->used) {
    r->used = TRUE;
    r->pc = pc;
    pc_count++;
  }
  return r;
}

/**************************************************************************************/

void critpath_precision_init(void) {
  free(ring);
  free(pc_table);
  ring = NULL;
  pc_table = NULL;
  pc_capacity = pc_count = 0;
  prec_emitted = FALSE;
  free(tr_flags);
  free(tr_lpr);
  free(tr_pcx);
  tr_flags = NULL;
  tr_lpr = NULL;
  tr_pcx = NULL;
  tr_cap = tr_count = commit_ord = trace_start = 0;
  ASSERTM(0, !ZERECO_CRITPATH_PRECISION_TRACE || ZERECO_CRITPATH_PRECISION_PROFILE,
          "zereco_critpath_precision_trace needs zereco_critpath_precision_profile\n");
  if (!ZERECO_CRITPATH_PRECISION_PROFILE)
    return;
  ring = (Prec_Record*)calloc(PREC_RING_SIZE, sizeof(Prec_Record));
  ASSERTM(0, ring, "Failed to allocate the criticality ring\n");
  pc_alloc(4096);
  if (ZERECO_CRITPATH_PRECISION_TRACE) {
    prec_trace_grow(1);
    if (!FULL_WARMUP)
      trace_start = 1;
  }
}

static void prec_depth_event(uns proc_id, uns8 depth, Flag accelerated) {
  if (depth == 0) {
    STAT_EVENT(proc_id, CRITPATH_PREC_L2_D0);
    if (accelerated)
      STAT_EVENT(proc_id, CRITPATH_PREC_L2_D0_ACCEL);
  } else if (depth == 1) {
    STAT_EVENT(proc_id, CRITPATH_PREC_L2_D1);
    if (accelerated)
      STAT_EVENT(proc_id, CRITPATH_PREC_L2_D1_ACCEL);
  } else if (depth <= 3) {
    STAT_EVENT(proc_id, CRITPATH_PREC_L2_D2_3);
    if (accelerated)
      STAT_EVENT(proc_id, CRITPATH_PREC_L2_D2_3_ACCEL);
  } else if (depth <= 7) {
    STAT_EVENT(proc_id, CRITPATH_PREC_L2_D4_7);
    if (accelerated)
      STAT_EVENT(proc_id, CRITPATH_PREC_L2_D4_7_ACCEL);
  } else {
    STAT_EVENT(proc_id, CRITPATH_PREC_L2_D8P);
    if (accelerated)
      STAT_EVENT(proc_id, CRITPATH_PREC_L2_D8P_ACCEL);
  }
}

/* A record leaves the ring: its verdict is final, so count it. */
static void prec_retire_record(uns proc_id, const Prec_Record* r) {
  if (!(r->flags & PREC_VALID))
    return;
  const Flag cand = (r->flags & PREC_CAND) != 0;
  const Flag entry = (r->flags & PREC_ENTRY) != 0;
  const Flag l1 = (r->flags & PREC_L1) != 0;
  const Flag l1m = (r->flags & PREC_L1M) != 0;
  const Flag l2 = (r->flags & PREC_L2) != 0;
  const Flag l2m = (r->flags & PREC_L2_MISP) != 0;

  if (tr_flags && trace_start && r->ord >= trace_start) {
    const uns32 i = r->ord - trace_start;
    if (i >= tr_cap)
      prec_trace_grow(i + 1);
    uns8 b = 0;
    b |= l2 ? PREC_TR_L2 : 0;
    b |= l2 && r->depth == 0 ? PREC_TR_ROOT : 0;
    b |= cand ? PREC_TR_CAND : 0;
    b |= entry ? PREC_TR_ENTRY : 0;
    b |= (r->flags & PREC_RFP) ? PREC_TR_RFP : 0;
    b |= l2m ? PREC_TR_L2_MISP : 0;
    b |= l1 ? PREC_TR_L1 : 0;
    b |= (r->flags & PREC_MEMBER) ? PREC_TR_MEMBER : 0;
    tr_flags[i] = b;
    tr_lpr[i] = r->lpr_dist;
    tr_pcx[i >> PREC_TRACE_BLOCK_BITS] ^= (uns64)r->pc;
    if (i >= tr_count)
      tr_count = i + 1;
  }

  STAT_EVENT(proc_id, CRITPATH_PREC_OPS);
  if (r->flags & PREC_MEMBER)
    STAT_EVENT(proc_id, CRITPATH_PREC_MEMBER);
  if (cand)
    STAT_EVENT(proc_id, CRITPATH_PREC_CAND);
  if (entry)
    STAT_EVENT(proc_id, CRITPATH_PREC_ENTRY);
  if (l1)
    STAT_EVENT(proc_id, CRITPATH_PREC_L1);
  if (l1m)
    STAT_EVENT(proc_id, CRITPATH_PREC_L1M);
  if (l2)
    STAT_EVENT(proc_id, CRITPATH_PREC_L2);
  if (l2m)
    STAT_EVENT(proc_id, CRITPATH_PREC_L2_MISP);

  /* The 2 x 2s: one per priority definition and strictness. */
  if (cand && l1)
    STAT_EVENT(proc_id, CRITPATH_PREC_CAND_L1);
  if (cand && l1m)
    STAT_EVENT(proc_id, CRITPATH_PREC_CAND_L1M);
  if (cand && l2)
    STAT_EVENT(proc_id, CRITPATH_PREC_CAND_L2);
  if (cand && l2m)
    STAT_EVENT(proc_id, CRITPATH_PREC_CAND_L2_MISP);
  if (entry && l1)
    STAT_EVENT(proc_id, CRITPATH_PREC_ENTRY_L1);
  if (entry && l1m)
    STAT_EVENT(proc_id, CRITPATH_PREC_ENTRY_L1M);
  if (entry && l2)
    STAT_EVENT(proc_id, CRITPATH_PREC_ENTRY_L2);
  if (entry && l2m)
    STAT_EVENT(proc_id, CRITPATH_PREC_ENTRY_L2_MISP);
  if (l2 && !l1 && r->depth != 0) /* must not happen: a chain member is its consumer's LPR */
    STAT_EVENT(proc_id, CRITPATH_PREC_L2_WITHOUT_L1);

  /* Accelerated in fact: a priority RS entry, or a register-file prefetch that delivered the value.  Whether it is
     still on the critical chain afterwards is judged in this same run, like everything above. */
  const Flag rfp = (r->flags & PREC_RFP) != 0;
  if (rfp) {
    STAT_EVENT(proc_id, CRITPATH_PREC_RFP);
    if (l2)
      STAT_EVENT(proc_id, CRITPATH_PREC_RFP_L2);
    if (!cand)
      STAT_EVENT(proc_id, CRITPATH_PREC_RFP_NOT_CAND);
  }
  if (entry || rfp) {
    STAT_EVENT(proc_id, CRITPATH_PREC_ACCEL);
    if (l2)
      STAT_EVENT(proc_id, CRITPATH_PREC_ACCEL_L2);
    /* An H2P branch instance is the root of its own chain (depth 0), so it is on the chain by definition.  Counted
       apart so the share can be read over the chain members alone. */
    if (l2 && r->depth == 0)
      STAT_EVENT(proc_id, CRITPATH_PREC_ACCEL_ROOT);
  }
  if (cand || rfp) {
    STAT_EVENT(proc_id, CRITPATH_PREC_CAND_OR_RFP);
    if (l2)
      STAT_EVENT(proc_id, CRITPATH_PREC_CAND_OR_RFP_L2);
  }
  if (l2)
    prec_depth_event(proc_id, r->depth, cand);

  /* Same window as the counters above: the warm-up dump resets the statistics, so
     only records evicted after warm-up reach the Periodic column.  The per-PC table is
     accumulated by hand and has to apply that rule itself. */
  if (FULL_WARMUP && inst_count[proc_id] < FULL_WARMUP)
    return;
  Prec_PC* e = pc_get(r->pc);
  if (e) {
    e->commits++;
    e->cand += cand;
    e->entry += entry;
    e->l1 += l1;
    e->l2 += l2;
    e->l2_misp += l2m;
    e->cand_l1 += (cand && l1);
    e->cand_l2 += (cand && l2);
  }
}

static inline Prec_Record* prec_find(Counter op_num) {
  if (!op_num)
    return NULL;
  Prec_Record* r = &ring[op_num & PREC_RING_MASK];
  return (r->flags & PREC_VALID) && r->op_num == op_num ? r : NULL;
}

/* What one walk found on its branch instance's chain, the branch itself excluded:
   the micro-ops, and how many of them were accelerated (a priority RS entry or a
   useful RF prefetch), and by which mechanism.  For the age bins. */
typedef struct {
  uns chain;
  uns accel;
  uns entry;
  uns rfp;
} Prec_Walk_Count;

/* From an H2P branch instance, walk last-arriving edges backward and mark what it
   was actually waiting on.  The ring is walked, not the pipeline: by now the
   producers have retired. */
static void prec_walk(uns proc_id, Prec_Record* branch, Flag mispred, Prec_Walk_Count* wc) {
  STAT_EVENT(proc_id, CRITPATH_PREC_WALK_STARTS);
  if (mispred)
    STAT_EVENT(proc_id, CRITPATH_PREC_WALK_STARTS_MISP);
  /* The branch is the root of its own chain: depth 0.  Kept apart in the depth
     breakdown, so the chain members can be read without it. */
  branch->flags |= PREC_L2;
  if (mispred)
    branch->flags |= PREC_L2_MISP;
  branch->depth = 0;
  if (!branch->lpr_op_num) {
    /* Every operand was ready before the branch was dispatched: nothing held it
       back, so this instance has no critical path at all. */
    STAT_EVENT(proc_id, CRITPATH_PREC_WALK_NO_CHAIN);
    return;
  }
  Counter next = branch->lpr_op_num;
  for (uns depth = 1; depth <= PREC_MAX_WALK; ++depth) {
    Prec_Record* r = prec_find(next);
    if (!r) {
      STAT_EVENT(proc_id, CRITPATH_PREC_WALK_MISS);
      return;
    }
    STAT_EVENT(proc_id, CRITPATH_PREC_WALK_STEPS);
    /* This instance waited on it, whether or not an earlier walk got here first. */
    wc->chain++;
    wc->entry += (r->flags & PREC_ENTRY) != 0;
    wc->rfp += (r->flags & PREC_RFP) != 0;
    wc->accel += (r->flags & (PREC_ENTRY | PREC_RFP)) != 0;
    if (depth < r->depth)
      r->depth = (uns8)depth; /* the shortest distance to any branch; unmarked records hold 0xFF */
    r->flags |= PREC_L2;
    if (mispred)
      r->flags |= PREC_L2_MISP;
    if (!r->lpr_op_num) {
      STAT_EVENT(proc_id, CRITPATH_PREC_WALK_FRONTIER);
      return;
    }
    next = r->lpr_op_num;
  }
  STAT_EVENT(proc_id, CRITPATH_PREC_WALK_MAXDEPTH);
}

/* The age bins: file one walked H2P branch instance by how many H2P instances its
   branch has run since its latest promotion (zereco/branch_state.c), so the run
   shows how soon after a promotion its chain is accelerated.  A chain learned
   before the promotion (zereco_critpath_learn_owner_gate 1) should be accelerated
   from the first bin on; one learned only after it (gate 2) should start low.
   The latency is taken here, at the same instance's retire, rather than from the
   ZERECO_H2P_* profiler, which files its event before the branch retires. */
enum {
  PREC_AGE_INST, PREC_AGE_CHAIN, PREC_AGE_ACCEL, PREC_AGE_ENTRY, PREC_AGE_RFP, PREC_AGE_MISP,
  PREC_AGE_MISP_CHAIN, PREC_AGE_MISP_ACCEL, PREC_AGE_MISP_LAT_N, PREC_AGE_MISP_LAT, PREC_AGE_MEASURES
};

static void prec_age_event(uns proc_id, const Op* op, Flag mispred, const Prec_Walk_Count* wc) {
  Counter age = branch_state_h2p_age(op->inst_info->addr);
  if (!age) {
    STAT_EVENT(proc_id, CRITPATH_PREC_AGE_UNKNOWN);
    return;
  }
  uns bin = age <= 16 ? 0 : age <= 64 ? 1 : age <= 256 ? 2 : age <= 1024 ? 3 : 4;
  uns base = CRITPATH_PREC_AGE_A1_16_INST + bin * PREC_AGE_MEASURES;
  STAT_EVENT(proc_id, base + PREC_AGE_INST);
  INC_STAT_EVENT(proc_id, base + PREC_AGE_CHAIN, wc->chain);
  INC_STAT_EVENT(proc_id, base + PREC_AGE_ACCEL, wc->accel);
  INC_STAT_EVENT(proc_id, base + PREC_AGE_ENTRY, wc->entry);
  INC_STAT_EVENT(proc_id, base + PREC_AGE_RFP, wc->rfp);
  if (!mispred)
    return;
  STAT_EVENT(proc_id, base + PREC_AGE_MISP);
  INC_STAT_EVENT(proc_id, base + PREC_AGE_MISP_CHAIN, wc->chain);
  INC_STAT_EVENT(proc_id, base + PREC_AGE_MISP_ACCEL, wc->accel);
  /* Fetch to resolution, as the profiler measures it: the cycle the branch was
     fetched to the cycle it executed and found itself wrong. */
  if (op->oracle_info.recover_at_exec && op->fetch_cycle && op->exec_cycle != MAX_CTR &&
      op->exec_cycle >= op->fetch_cycle) {
    STAT_EVENT(proc_id, base + PREC_AGE_MISP_LAT_N);
    INC_STAT_EVENT(proc_id, base + PREC_AGE_MISP_LAT, op->exec_cycle - op->fetch_cycle);
  }
}

void critpath_precision_note_retire(Op* op) {
  if (!ZERECO_CRITPATH_PRECISION_PROFILE || !ring)
    return;
  if (!op || op->thread_id != 0 || op->off_path || !op->inst_info || !op->table_info)
    return;
  const uns proc_id = op->proc_id;

  Prec_Record* slot = &ring[op->op_num & PREC_RING_MASK];
  prec_retire_record(proc_id, slot); /* whatever it held has had its chance */

  slot->op_num = op->op_num;
  slot->lpr_op_num = op->critpath_wake_events ? op->critpath_last_producer_op_num : 0;
  slot->pc = op->inst_info->addr;
  slot->ord = ++commit_ord;
  slot->lpr_dist = 0;
  slot->depth = 0xFF;
  slot->flags = PREC_VALID;
  if (op->zereco_iq_priority_candidate_bit)
    slot->flags |= PREC_CAND;
  /* op->zereco_piq_entry cannot be used here: it is cleared when the op leaves the RS, so
     that the class counters are decremented correctly, and is therefore always FALSE by
     commit.  zereco_piq_fallback survives, and a candidate either took a priority entry or
     fell back to a normal one -- nothing else clears the bit on the way to dispatch. */
  if (op->zereco_iq_priority_candidate_bit && !op->zereco_piq_fallback)
    slot->flags |= PREC_ENTRY;
  /* Set by rfp_try_validate() when a useful prefetch handed this load its value (every useful one clears
     RFP_COVERED_MIN_SAVED_CYCLES at its default of 1).  The H2P-chain load oracle sets it too, but that model is off
     in every configuration this ledger runs with. */
  if (op->zereco_rf_covered)
    slot->flags |= PREC_RFP;
  if (critpath_pc_critical(proc_id, slot->pc))
    slot->flags |= PREC_MEMBER; /* tracked, whether or not the gate let it through */

  /* Level 1 is the consumer's verdict on its producer, so it is recorded from
     here, where the consumer knows which instance arrived last. */
  if (slot->lpr_op_num) {
    STAT_EVENT(proc_id, CRITPATH_PREC_EDGES);
    Prec_Record* prod = prec_find(slot->lpr_op_num);
    const Flag member_consumer = (slot->flags & PREC_MEMBER) != 0;
    if (member_consumer)
      STAT_EVENT(proc_id, CRITPATH_PREC_EDGES_MEMBER);
    if (prod) {
      /* Both records are in the ring, which spans at most PREC_RING_SIZE commits. */
      slot->lpr_dist = (uns16)MIN2(slot->ord - prod->ord, 0xFFFFu);
      prod->flags |= PREC_L1;
      if (member_consumer)
        prod->flags |= PREC_L1M;
      if (prod->flags & PREC_CAND) {
        STAT_EVENT(proc_id, CRITPATH_PREC_EDGES_ACCEL);
        if (member_consumer)
          STAT_EVENT(proc_id, CRITPATH_PREC_EDGES_MEMBER_ACCEL);
      }
    } else {
      STAT_EVENT(proc_id, CRITPATH_PREC_EDGE_MISS);
    }
  }

  /* An H2P branch instance is where a critical path ends. */
  if (op->table_info->cf_type != NOT_CF && op->oracle_info.hbt_pred_is_hard) {
    Flag mispred = op->oracle_info.mispred | op->oracle_info.misfetch;
    Prec_Walk_Count wc = {0, 0, 0, 0};
    prec_walk(proc_id, slot, mispred, &wc);
    prec_age_event(proc_id, op, mispred, &wc);
  }

  /* The trace's window opens with the first op after the warm-up's last instruction.  It is
     placed by commit count, so it opens on the same instance in every run. */
  if (tr_flags && !trace_start && op->eom && inst_count[proc_id] >= FULL_WARMUP)
    trace_start = slot->ord + 1;
}

/**************************************************************************************/
/* The per-PC spread: is criticality a property of the PC, or of the instance? */

static void prec_pc_bucket(uns proc_id, double rate, Counter ops, Flag level2) {
  if (rate <= 0.0) {
    STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_R0 : CRITPATH_PREC_PC_L1_R0);
    INC_STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_R0_OPS : CRITPATH_PREC_PC_L1_R0_OPS, ops);
  } else if (rate < 10.0) {
    STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_RLT10 : CRITPATH_PREC_PC_L1_RLT10);
    INC_STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_RLT10_OPS : CRITPATH_PREC_PC_L1_RLT10_OPS, ops);
  } else if (rate < 50.0) {
    STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_R10_50 : CRITPATH_PREC_PC_L1_R10_50);
    INC_STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_R10_50_OPS : CRITPATH_PREC_PC_L1_R10_50_OPS, ops);
  } else if (rate < 90.0) {
    STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_R50_90 : CRITPATH_PREC_PC_L1_R50_90);
    INC_STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_R50_90_OPS : CRITPATH_PREC_PC_L1_R50_90_OPS, ops);
  } else {
    STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_RGT90 : CRITPATH_PREC_PC_L1_RGT90);
    INC_STAT_EVENT(proc_id, level2 ? CRITPATH_PREC_PC_L2_RGT90_OPS : CRITPATH_PREC_PC_L1_RGT90_OPS, ops);
  }
}

static void prec_dump_csv(void) {
  FILE* f = fopen("critpath_precision.csv", "w");
  if (!f)
    return;
  fprintf(f, "pc,commits,priority_candidate,priority_entry,level1,level2,level2_mispred,cand_and_level1,"
             "cand_and_level2\n");
  for (uns i = 0; i < pc_capacity; ++i) {
    const Prec_PC* e = &pc_table[i];
    if (!e->used)
      continue;
    fprintf(f, "0x%llx,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)e->pc,
            (unsigned long long)e->commits, (unsigned long long)e->cand, (unsigned long long)e->entry,
            (unsigned long long)e->l1, (unsigned long long)e->l2, (unsigned long long)e->l2_misp,
            (unsigned long long)e->cand_l1, (unsigned long long)e->cand_l2);
  }
  fclose(f);
}

void critpath_precision_done(uns proc_id) {
  if (!ZERECO_CRITPATH_PRECISION_PROFILE || !ring || prec_emitted)
    return;
  prec_emitted = TRUE;
  for (uns i = 0; i < PREC_RING_SIZE; ++i)
    prec_retire_record(proc_id, &ring[i]);

  /* Of the instances this PC had accelerated, how many were really critical?  A PC
     whose rate sits at one end is separable by a static rule; one in the middle is
     critical only on some instances, which no per-PC filter can catch. */
  for (uns i = 0; i < pc_capacity; ++i) {
    const Prec_PC* e = &pc_table[i];
    if (!e->used || !e->cand)
      continue;
    STAT_EVENT(proc_id, CRITPATH_PREC_PC_ACCELERATED);
    prec_pc_bucket(proc_id, 100.0 * (double)e->cand_l1 / (double)e->cand, e->cand, FALSE);
    prec_pc_bucket(proc_id, 100.0 * (double)e->cand_l2 / (double)e->cand, e->cand, TRUE);
  }
  if (ZERECO_CRITPATH_PRECISION_DUMP)
    prec_dump_csv();
  if (tr_flags)
    prec_trace_write();
}
