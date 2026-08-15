/***************************************************************************************
 * File         : zereco/rfp.c
 * Description  : Timed Register File Prefetching for ZERECO.  See rfp.h for the
 *                datapath overview and zereco_RFP_IMPLEMENTATION_PLAN.md for how
 *                each piece maps onto the RFP paper (ISCA'22).
 *
 * Phase 1 scope: the Prefetch Table trains and predicts, but nothing reaches the
 * memory system and no load's latency changes.  Prediction accuracy, coverage
 * headroom, and the store-abstain population are all measurable here, and a run
 * must stay cycle-identical to the baseline whether rfp_enable is on or off.
 * Phase 2 adds the request queue and real L1 probes; Phase 3 lets a covered load
 * skip its cache access.
 ***************************************************************************************/

#include "zereco/rfp.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "core.param.h"
#include "memory/memory.h"
#include "op.h"
#include "statistics.h"

/**************************************************************************************/
/* Per-core state */

static RFP_Core_State rfp_state[MAX_NUM_PROCS];

static inline RFP_Core_State* rfp_get_state(uns proc_id) {
  ASSERT(proc_id, proc_id < MAX_NUM_PROCS);
  return &rfp_state[proc_id];
}

/**************************************************************************************/
/* Deterministic randomness.
 *
 * The paper increments a Prefetch Table entry's confidence with probability
 * 1/16 on a repeated stride (§3.1).  Scarab runs must stay reproducible, so
 * draw from a per-core LCG seeded by proc_id instead of a library RNG. */

static inline Flag rfp_rand_probability(RFP_Core_State* state, uns log2_prob) {
  state->rand_state =
    state->rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
  if (log2_prob == 0)
    return TRUE;
  uns64 draw = (state->rand_state >> 33) & ((1ULL << log2_prob) - 1);
  return draw == 0;
}

/**************************************************************************************/
/* Prefetch Table.
 *
 * Set-associative and indexed by the static load PC.  x86 instructions are
 * unaligned, so the low PC bits carry enough entropy to index directly -- the
 * same convention the Block Cache uses. */

static inline RFP_PT_Entry* rfp_pt_set(RFP_Core_State* state, Addr load_pc) {
  return &state->pt[(load_pc % state->pt_sets) * RFP_PT_ASSOC];
}

static RFP_PT_Entry* rfp_pt_lookup(uns proc_id, Addr load_pc) {
  RFP_Core_State* state = rfp_get_state(proc_id);
  if (!state->initialized)
    return NULL;

  RFP_PT_Entry* set = rfp_pt_set(state, load_pc);
  for (uns way = 0; way < RFP_PT_ASSOC; ++way) {
    if (set[way].valid && set[way].tag == load_pc) {
      set[way].lru_touch = cycle_count;
      return &set[way];
    }
  }
  return NULL;
}

/* Allocate an entry for load_pc, evicting the least useful way (breaking ties
   by LRU).  Utility is what the paper ages entries by: a PC whose stride keeps
   changing never builds utility and loses its way to a steadier one. */
static RFP_PT_Entry* rfp_pt_allocate(uns proc_id, Addr load_pc) {
  RFP_Core_State* state = rfp_get_state(proc_id);
  if (!state->initialized)
    return NULL;

  RFP_PT_Entry* set = rfp_pt_set(state, load_pc);
  RFP_PT_Entry* victim = NULL;

  for (uns way = 0; way < RFP_PT_ASSOC; ++way) {
    if (!set[way].valid) {
      victim = &set[way];
      break;
    }
    if (!victim || set[way].utility < victim->utility ||
        (set[way].utility == victim->utility &&
         set[way].lru_touch < victim->lru_touch))
      victim = &set[way];
  }
  ASSERT(proc_id, victim);

  if (victim->valid) {
    STAT_EVENT(proc_id, RFP_PT_EVICTIONS);
    /* An evicted entry still owning in-flight instances loses the counter that
       their retires would have decremented.  Those retires find a tag mismatch
       and skip, so nothing underflows, but the event is worth watching. */
    if (victim->inflight)
      STAT_EVENT(proc_id, RFP_PT_EVICT_WITH_INFLIGHT);
  }

  memset(victim, 0, sizeof(*victim));
  victim->valid = TRUE;
  victim->tag = load_pc;
  victim->lru_touch = cycle_count;
  return victim;
}

/**************************************************************************************/
/* Init / reset */

void rfp_init(uns proc_id) {
  ASSERT(proc_id, proc_id < MAX_NUM_PROCS);
  RFP_Core_State* state = rfp_get_state(proc_id);
  memset(state, 0, sizeof(*state));

  if (!RFP_ENABLE)
    return;

  /* The oracle and the timed model both hook the load's first dcache attempt,
     and they answer the same question with different fidelity.  Running both
     would make the reported coverage meaningless. */
  ASSERTM(proc_id, !H2P_CHAIN_PERFECT_LOAD,
          "rfp_enable and h2p_chain_perfect_load are mutually exclusive\n");
  ASSERTM(proc_id, !TEA_ENABLE,
          "ZERECO timed RFP is a main-thread-only experiment; disable TEA\n");
  ASSERTM(proc_id, RFP_SCOPE <= 1, "rfp_scope must be 0 or 1\n");
  ASSERTM(proc_id, RFP_L1_MISS_POLICY <= 1,
          "rfp_l1_miss_policy must be 0 (drop) or 1 (continue to lower levels)\n");
  ASSERTM(proc_id, RFP_PT_ASSOC > 0 && RFP_PT_ENTRIES >= RFP_PT_ASSOC &&
                     RFP_PT_ENTRIES % RFP_PT_ASSOC == 0,
          "rfp_pt_entries (%u) must be a non-zero multiple of rfp_pt_assoc (%u)\n",
          RFP_PT_ENTRIES, RFP_PT_ASSOC);
  ASSERTM(proc_id, RFP_CONF_BITS > 0 && RFP_CONF_BITS <= 8,
          "rfp_conf_bits must be between 1 and 8\n");
  ASSERTM(proc_id, RFP_CONF_INC_PROB_LOG2 < 16,
          "rfp_conf_inc_prob_log2 is a log2 probability; keep it under 16\n");
  ASSERTM(proc_id, RFP_QUEUE_ENTRIES > 0, "rfp_queue_entries must be non-zero\n");
  ASSERTM(proc_id, RFP_DRAIN_WIDTH > 0, "rfp_drain_width must be non-zero\n");
  ASSERTM(proc_id, RFP_HIT_LATENCY > 0, "rfp_hit_latency must be non-zero\n");

  state->pt_sets = RFP_PT_ENTRIES / RFP_PT_ASSOC;
  state->pt = (RFP_PT_Entry*)calloc(RFP_PT_ENTRIES, sizeof(RFP_PT_Entry));
  state->queue =
    (RFP_Queue_Entry*)calloc(RFP_QUEUE_ENTRIES, sizeof(RFP_Queue_Entry));
  ASSERTM(proc_id, state->pt && state->queue,
          "Failed to allocate RFP Prefetch Table / request queue\n");

  state->queue_head = 0;
  state->queue_count = 0;
  state->rand_state = 0x9E3779B97F4A7C15ULL ^ ((uns64)proc_id + 1);
  state->initialized = TRUE;
}

void rfp_reset(uns proc_id) {
  RFP_Core_State* state = rfp_get_state(proc_id);
  if (!state->initialized)
    return;

  memset(state->pt, 0, sizeof(RFP_PT_Entry) * RFP_PT_ENTRIES);
  memset(state->queue, 0, sizeof(RFP_Queue_Entry) * RFP_QUEUE_ENTRIES);
  state->queue_head = 0;
  state->queue_count = 0;
}

/**************************************************************************************/
/* Shared predicates */

static inline Flag rfp_active(void) {
  return RFP_ENABLE;
}

/* Only main-thread loads participate; TEA ops and stores never do. */
static inline Flag rfp_load_candidate(Op* op) {
  return rfp_active() && op && op->thread_id == 0 && op->table_info &&
         op->table_info->mem_type == MEM_LD && op->inst_info;
}

static inline uns rfp_conf_max(void) {
  return (1u << RFP_CONF_BITS) - 1u;
}

static inline Flag rfp_entry_eligible(RFP_PT_Entry* entry) {
  return entry->has_base && entry->has_stride &&
         entry->confidence >= rfp_conf_max();
}

/* A load with a true store dependence is left entirely on the demand path.  The
   paper would instead forward the store's data into the prefetch (§3.2.1), so
   this population is what the conservative model gives up -- report it rather
   than hide it.  `all_data_ready` separates the store whose data is already
   available (case a, forwarding possible now) from one still executing. */
static Flag rfp_has_store_dependence(Op* op, Flag* all_data_ready) {
  Flag found = FALSE;
  Flag ready = TRUE;

  for (uns ii = 0; ii < op->oracle_info.num_srcs; ++ii) {
    Src_Info* src = &op->oracle_info.src_info[ii];
    if (src->type != MEM_DATA_DEP)
      continue;
    found = TRUE;

    /* The op pool recycles entries, so the producer pointer is only meaningful
       when its identity still matches -- the same guard wake_up_ops() uses. */
    Op* store = src->op;
    Flag store_done = store && store->op_pool_valid &&
                      store->unique_num == src->unique_num &&
                      store->done_cycle != MAX_CTR &&
                      store->done_cycle <= cycle_count;
    if (!store_done)
      ready = FALSE;
  }

  *all_data_ready = found && ready;
  return found;
}

/**************************************************************************************/
/* Backward-walk hook: which PCs are Target Loads */

void rfp_note_target_load(uns proc_id, Addr load_pc, Addr va) {
  if (!rfp_active() || RFP_SCOPE != 0)
    return;
  (void)va; /* the walk decides membership only; addresses are tracked at retire */

  RFP_PT_Entry* entry = rfp_pt_lookup(proc_id, load_pc);
  if (entry) {
    /* Still a Target Load, so protect the entry from eviction.  This is the
       only aging signal a PC gets while its stride stays steady. */
    if (entry->utility < RFP_UTILITY_MAX)
      entry->utility++;
    STAT_EVENT(proc_id, RFP_PT_REFRESHED_BY_WALK);
    return;
  }

  if (rfp_pt_allocate(proc_id, load_pc))
    STAT_EVENT(proc_id, RFP_PT_ALLOCATED_BY_WALK);
}

/**************************************************************************************/
/* Retire hook: address training */

void rfp_retire_train(Op* op) {
  if (!rfp_load_candidate(op))
    return;

  uns proc_id = op->proc_id;
  Addr pc = op->inst_info->addr;
  RFP_PT_Entry* entry = rfp_pt_lookup(proc_id, pc);

  if (!entry) {
    /* Scope 0 reserves allocation for the backward walk, so an unknown PC here
       simply is not a Target Load.  Scope 1 (vanilla RFP) tracks every load. */
    if (RFP_SCOPE == 1) {
      entry = rfp_pt_allocate(proc_id, pc);
      if (entry)
        STAT_EVENT(proc_id, RFP_PT_ALLOCATED_BY_RETIRE);
    }
    if (!entry) {
      /* The entry this instance counted against was evicted before it retired.
         Its in-flight contribution is lost with the entry; nothing underflows,
         but a steady stream here means the table is thrashing. */
      if (op->rfp_pt_counted) {
        op->rfp_pt_counted = FALSE;
        STAT_EVENT(proc_id, RFP_PT_EVICT_WITH_INFLIGHT);
      }
      return;
    }
  }

  /* Release this instance's in-flight count: it has committed, so the next
     prediction should measure distance from this address, not through it. */
  if (op->rfp_pt_counted) {
    if (entry->inflight > 0)
      entry->inflight--;
    else
      STAT_EVENT(proc_id, RFP_INFLIGHT_UNDERFLOW);
    op->rfp_pt_counted = FALSE;
  }

  Addr va = op->oracle_info.va;
  if (!entry->has_base) {
    entry->has_base = TRUE;
    entry->base_va = va;
    return; /* no previous address, so no delta to learn yet */
  }

  int64 delta = (int64)va - (int64)entry->base_va;
  if (entry->has_stride && entry->stride == delta) {
    /* Paper §3.1 raises confidence only with probability 1/16, so an entry needs
       a long run of identical strides before it may launch. */
    if (entry->confidence < rfp_conf_max() &&
        rfp_rand_probability(rfp_get_state(proc_id), RFP_CONF_INC_PROB_LOG2)) {
      entry->confidence++;
      if (entry->confidence == rfp_conf_max())
        STAT_EVENT(proc_id, RFP_PT_CONF_SATURATED);
    }
    if (entry->utility < RFP_UTILITY_MAX)
      entry->utility++;
  } else {
    entry->stride = delta;
    entry->has_stride = TRUE;
    entry->confidence = 0;
    entry->utility = 0;
    STAT_EVENT(proc_id, RFP_PT_STRIDE_RESETS);
  }

  entry->base_va = va;
}

/**************************************************************************************/
/* Rename hook: count the instance and form the prediction */

void rfp_rename_launch(Op* op) {
  if (!rfp_load_candidate(op))
    return;

  uns proc_id = op->proc_id;

  /* Funnel statistics describe useful work, so they follow on-path loads only.
     Off-path loads still launch, because real hardware cannot tell them apart
     and their prefetches consume the same L1 bandwidth.  Counting every on-path
     load here makes RFP_CANDIDATE_LOADS the "all loads" denominator the paper
     reports against, and RFP_PT_HIT_PCT the share that are Target Loads. */
  Flag on_path = !op->off_path;
  if (on_path)
    STAT_EVENT(proc_id, RFP_CANDIDATE_LOADS);

  RFP_PT_Entry* entry = rfp_pt_lookup(proc_id, op->inst_info->addr);
  if (!entry)
    return; /* PT membership is the scope filter (§3.7) */

  if (on_path) {
    STAT_EVENT(proc_id, RFP_PT_HIT);
    STAT_EVENT(proc_id, RFP_PT_HIT_PCT);
  }

  /* Count every dynamic instance, including off-path ones and those about to
     abstain.  The counter measures distance from the last retired address, so
     skipping any instance would bias every later prediction for this PC. */
  if (entry->inflight < RFP_INFLIGHT_MAX) {
    entry->inflight++;
    op->rfp_pt_counted = TRUE;
  }

  Flag store_data_ready = FALSE;
  if (rfp_has_store_dependence(op, &store_data_ready)) {
    if (on_path) {
      STAT_EVENT(proc_id, RFP_ABSTAIN_STORE_DEP);
      STAT_EVENT(proc_id, RFP_ABSTAIN_STORE_DEP_PCT);
      STAT_EVENT(proc_id, store_data_ready ?
                            RFP_ABSTAIN_STORE_DEP_FWD_READY :
                            RFP_ABSTAIN_STORE_DEP_FWD_WAIT);
    }
    return;
  }

  if (!rfp_entry_eligible(entry))
    return;
  if (on_path)
    STAT_EVENT(proc_id, RFP_ELIGIBLE);

  /* base_va is the last retired address and inflight counts the instances
     between it and this one, so this lands on the address this load will
     compute -- if the stride held (paper §3.1). */
  op->rfp_pred_va = (Addr)((int64)entry->base_va +
                           entry->stride * (int64)entry->inflight);
  op->rfp_launched = TRUE;

  /* Phase 1 has no request queue, so injection cannot fail yet; Phase 2 makes
     this subject to queue capacity and turns the gap into RFP_DROP_QUEUE_FULL. */
  if (on_path) {
    STAT_EVENT(proc_id, RFP_INJECTED);
    STAT_EVENT(proc_id, RFP_INJECTED_PCT);
  }
}

/**************************************************************************************/
/* Dcache hooks */

void rfp_queue_drain(uns proc_id) {
  if (!rfp_active())
    return;
  (void)proc_id;
  /* Phase 2: drain up to RFP_DRAIN_WIDTH packets using read ports the demand
     loads left unused this cycle. */
}

Flag rfp_try_validate(Op* op) {
  if (!rfp_load_candidate(op))
    return FALSE;

  uns proc_id = op->proc_id;

  /* A load that loses the port arbitration re-enters the dcache stage next
     cycle, so this runs more than once per load by design; the flag keeps the
     decision itself to exactly one evaluation.  The re-entry count is a useful
     read on dcache port pressure, not an error. */
  if (op->rfp_validated) {
    STAT_EVENT(proc_id, RFP_VALIDATE_REENTRY);
    return FALSE;
  }
  op->rfp_validated = TRUE;

  if (op->off_path)
    return FALSE;

  if (!op->rfp_launched) {
    STAT_EVENT(proc_id, RFP_NOT_PREDICTED);
    return FALSE;
  }

  if (op->rfp_pred_va != op->oracle_info.va) {
    STAT_EVENT(proc_id, RFP_WRONG_ADDR);
    STAT_EVENT(proc_id, RFP_WRONG_ADDR_PCT);
    return FALSE;
  }

  /* An older store still draining to memory can hold data this address needs,
     and the prefetch read the cache instead.  Same rule the oracle path uses. */
  if (scan_stores(op->oracle_info.va, op->oracle_info.mem_size)) {
    STAT_EVENT(proc_id, RFP_VAL_STORE_CONFLICT);
    return FALSE;
  }

  /* Phase 1 stops here: the address was right and nothing forbade the prefetch,
     but no probe ran, so there is no data and no latency to save.  This counts
     the coverage a perfectly timely, bandwidth-free RFP would reach -- the
     ceiling Phase 2/3 are measured against. */
  STAT_EVENT(proc_id, RFP_NOT_EXECUTED);
  return FALSE;
}

/**************************************************************************************/
/* Op-pool hook: squashed loads give back their in-flight count */

void rfp_note_op_freed(Op* op) {
  if (!rfp_active() || !op || !op->rfp_pt_counted)
    return;

  /* Retired loads clear the flag during training, so reaching here means the op
     was squashed (paper §3.1: the counter is decremented for each squashed
     load).  free_op() is the single point every op passes through, which is what
     makes the decrement exactly-once without tracking each flush path. */
  uns proc_id = op->proc_id;
  op->rfp_pt_counted = FALSE;
  if (!op->inst_info)
    return;

  RFP_PT_Entry* entry = rfp_pt_lookup(proc_id, op->inst_info->addr);
  if (!entry) {
    STAT_EVENT(proc_id, RFP_PT_EVICT_WITH_INFLIGHT);
    return;
  }
  if (entry->inflight > 0)
    entry->inflight--;
  else
    STAT_EVENT(proc_id, RFP_INFLIGHT_UNDERFLOW);
}
