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

#include "model.h"

#include "core.param.h"
#include "dcache_stage.h"
#include "libs/cache_lib.h"
#include "libs/port_lib.h"
#include "memory/memory.h"
#include "memory/memory.param.h"
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
  ASSERTM(proc_id, RFP_COVERED_MIN_SAVED_CYCLES > 0 &&
                     RFP_COVERED_MIN_SAVED_CYCLES <= DCACHE_CYCLES,
          "rfp_covered_min_saved_cycles must be between 1 and dcache_cycles\n");
  ASSERTM(proc_id, RFP_PORT_FAIL_POLICY <= 2,
          "rfp_port_fail_policy must be 0 (wait), 1 (drop), or 2 (skip)\n");
  ASSERTM(proc_id, RFP_PORT_PRIORITY <= 2,
          "rfp_port_priority must be 0 (spare), 1 (dedicated), or 2 (prefetch first)\n");
  ASSERTM(proc_id, RFP_PORT_PRIORITY != 1 || RFP_DEDICATED_READ_PORTS > 0,
          "rfp_port_priority 1 needs at least one dedicated read port\n");
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

  if (RFP_PORT_PRIORITY == 1) {
    state->dedicated_ports = (Ports*)calloc(DCACHE_BANKS, sizeof(Ports));
    ASSERTM(proc_id, state->dedicated_ports,
            "Failed to allocate RFP dedicated dcache ports\n");
    for (uns bank = 0; bank < DCACHE_BANKS; ++bank) {
      char name[MAX_STR_LENGTH + 1];
      snprintf(name, MAX_STR_LENGTH, "RFP DCACHE BANK %u PORTS", bank);
      init_ports(&state->dedicated_ports[bank], name, RFP_DEDICATED_READ_PORTS,
                 0, FALSE);
    }
  }

  state->pending =
    (RFP_Pending_Fill*)calloc(RFP_QUEUE_ENTRIES, sizeof(RFP_Pending_Fill));
  ASSERTM(proc_id, state->pending,
          "Failed to allocate RFP pending-fill tracking\n");

  state->queue_head = 0;
  state->queue_count = 0;
  state->pending_count = 0;
  state->port_cycle = MAX_CTR;
  state->ports_taken_this_cycle = 0;
  state->rand_state = 0x9E3779B97F4A7C15ULL ^ ((uns64)proc_id + 1);
  state->initialized = TRUE;
}

void rfp_reset(uns proc_id) {
  RFP_Core_State* state = rfp_get_state(proc_id);
  if (!state->initialized)
    return;

  memset(state->pt, 0, sizeof(RFP_PT_Entry) * RFP_PT_ENTRIES);
  memset(state->queue, 0, sizeof(RFP_Queue_Entry) * RFP_QUEUE_ENTRIES);
  memset(state->pending, 0, sizeof(RFP_Pending_Fill) * RFP_QUEUE_ENTRIES);
  state->queue_head = 0;
  state->queue_count = 0;
  state->pending_count = 0;
  state->port_cycle = MAX_CTR;
  state->ports_taken_this_cycle = 0;
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
  Addr pred_va = (Addr)((int64)entry->base_va +
                        entry->stride * (int64)entry->inflight);

  /* Oracle that bounds what a confidence-based launch gate could recover.  It
     suppresses only the request, never the in-flight bookkeeping above: real
     hardware counts wrong-path allocations too, and skipping them here would
     shift every later prediction instead of isolating the bandwidth effect. */
  if (!on_path && !RFP_LAUNCH_OFFPATH) {
    STAT_EVENT(proc_id, RFP_OFFPATH_LAUNCH_SUPPRESSED);
    return;
  }

  RFP_Core_State* state = rfp_get_state(proc_id);
  if (state->queue_count >= RFP_QUEUE_ENTRIES) {
    /* The queue is the packet's only path to the L1, so a full queue is a lost
       prefetch -- not a stalled one.  The load proceeds normally. */
    if (on_path)
      STAT_EVENT(proc_id, RFP_DROP_QUEUE_FULL);
    return;
  }

  RFP_Queue_Entry* slot =
    &state->queue[(state->queue_head + state->queue_count) % RFP_QUEUE_ENTRIES];
  slot->op = op;
  slot->op_num = op->op_num;
  slot->unique_num = op->unique_num;
  slot->pred_va = pred_va;
  slot->mem_size = op->oracle_info.mem_size;
  slot->launch_cycle = cycle_count;
  state->queue_count++;

  op->rfp_pred_va = pred_va;
  op->rfp_launched = TRUE;
  if (on_path) {
    STAT_EVENT(proc_id, RFP_INJECTED);
    STAT_EVENT(proc_id, RFP_INJECTED_PCT);
  }
}

/**************************************************************************************/
/* Dcache hooks */

/**************************************************************************************/
/* Probes that continue past the L1 (RFP_L1_MISS_POLICY 1) */

/* Register the probe so the fill callback can find its load, then issue the
   request.  Returns FALSE when no MSHR or tracking slot was available, in which
   case the prefetch is simply abandoned and the load fetches the line itself. */
static Flag rfp_send_to_lower_levels(uns proc_id, Op* op, Addr line_addr) {
  RFP_Core_State* state = rfp_get_state(proc_id);

  /* Reclaim slots whose fill will never arrive.  A request that coalesced with
     one already in flight never reaches our callback, and a squashed or
     already-validated load has no use for the data -- without reclamation those
     slots would accumulate until the table wedged shut. */
  RFP_Pending_Fill* slot = NULL;
  for (uns ii = 0; ii < RFP_QUEUE_ENTRIES; ++ii) {
    RFP_Pending_Fill* p = &state->pending[ii];
    if (p->valid) {
      Op* owner = p->op;
      if (!owner->op_pool_valid || owner->unique_num != p->unique_num ||
          owner->op_num != p->op_num || owner->rfp_validated) {
        p->valid = FALSE;
        state->pending_count--;
        STAT_EVENT(proc_id, RFP_PENDING_FILL_RECLAIMED);
      }
    }
    if (!p->valid && !slot)
      slot = p;
  }
  if (!slot)
    return FALSE;

  if (!mem_can_allocate_req_buffer(proc_id, MRT_DPRF, FALSE))
    return FALSE;

  /* Requests are line-granular, like every other miss the memory system sees --
     the load's own data size is not a legal request size.  No op back-pointer
     either: the memory system would otherwise treat this as the load's demand
     request and complete it a second time. */
  if (!new_mem_req(MRT_DPRF, proc_id, line_addr, DCACHE_LINE_SIZE, 0, NULL,
                   rfp_fill_done, op->unique_num, NULL))
    return FALSE;

  slot->valid = TRUE;
  slot->line_addr = line_addr;
  slot->op = op;
  slot->op_num = op->op_num;
  slot->unique_num = op->unique_num;
  slot->issue_cycle = cycle_count;
  state->pending_count++;
  STAT_EVENT(proc_id, RFP_LOWER_LEVEL_REQUESTS);
  return TRUE;
}

Flag rfp_fill_done(Mem_Req* req) {
  /* Fill the line the normal way first; a failure here means the fill must be
     retried, so nothing else may be consumed yet. */
  Flag filled = dcache_fill_line(req);
  if (filled != SUCCESS)
    return filled;

  uns proc_id = req->proc_id;
  RFP_Core_State* state = rfp_get_state(proc_id);
  if (!state->initialized)
    return SUCCESS;

  STAT_EVENT(proc_id, RFP_LOWER_LEVEL_FILLS);
  for (uns ii = 0; ii < RFP_QUEUE_ENTRIES; ++ii) {
    RFP_Pending_Fill* p = &state->pending[ii];
    if (!p->valid || p->unique_num != req->unique_num)
      continue;

    Op* op = p->op;
    p->valid = FALSE;
    state->pending_count--;

    /* The load may have been squashed, or may have already given up waiting and
       fetched the line itself, while the fill was in flight. */
    if (!op->op_pool_valid || op->unique_num != p->unique_num ||
        op->op_num != p->op_num || op->rfp_validated) {
      STAT_EVENT(proc_id, RFP_LOWER_LEVEL_FILLS_WASTED);
      STAT_EVENT(proc_id, RFP_LOWER_LEVEL_FILLS_WASTED_PCT);
      return SUCCESS;
    }

    op->rfp_data_ready_cycle = cycle_count;
    STAT_EVENT(proc_id, RFP_LOWER_LEVEL_FILLS_USEFUL);
    return SUCCESS;
  }

  /* No owner left: its tracking slot was reclaimed, or the request coalesced
     with one the hardware prefetcher had already issued. */
  STAT_EVENT(proc_id, RFP_COALESCED_WITH_INFLIGHT);
  return SUCCESS;
}

/**************************************************************************************/
/* Request queue */

/* A queue slot is live while it still points at its op; dropping a packet just
   clears the pointer, and the head walks past the tombstones. */
static inline void rfp_queue_compact(RFP_Core_State* state) {
  while (state->queue_count && !state->queue[state->queue_head].op) {
    state->queue_head = (state->queue_head + 1) % RFP_QUEUE_ENTRIES;
    state->queue_count--;
  }
}

void rfp_queue_drain(uns proc_id, Dcache_Stage* dcache, Flag before_demand) {
  if (!rfp_active())
    return;
  /* Exactly one of the two call sites does the work, chosen by the policy:
     priority 2 probes ahead of the demand loop, everything else after it. */
  if (before_demand != (RFP_PORT_PRIORITY == 2))
    return;

  RFP_Core_State* state = rfp_get_state(proc_id);
  if (!state->initialized)
    return;

  if (state->port_cycle != cycle_count) {
    state->port_cycle = cycle_count;
    state->ports_taken_this_cycle = 0;
  }

  rfp_queue_compact(state);
  INC_STAT_EVENT(proc_id, RFP_QUEUE_OCCUPANCY_TOTAL, state->queue_count);
  if (state->queue_count >= RFP_QUEUE_ENTRIES)
    STAT_EVENT(proc_id, RFP_QUEUE_FULL_CYCLES);

  uns probes = 0;
  for (uns ii = 0; ii < state->queue_count && probes < RFP_DRAIN_WIDTH; ++ii) {
    RFP_Queue_Entry* e =
      &state->queue[(state->queue_head + ii) % RFP_QUEUE_ENTRIES];
    Op* op = e->op;
    if (!op)
      continue;

    /* Funnel counters follow on-path work, but an off-path prefetch still costs
       real L1 bandwidth, so the resource counters below stay ungated. */
    Flag on_path = !op->off_path;

    /* The op pool recycles entries after a squash, so the packet is only
       meaningful while the op it named still exists. */
    if (!op->op_pool_valid || op->unique_num != e->unique_num ||
        op->op_num != e->op_num) {
      if (on_path)
        STAT_EVENT(proc_id, RFP_DROP_STALE);
      e->op = NULL;
      continue;
    }

    /* The load reached the dcache stage first, so there is no latency left to
       hide (paper §3.3 drops the prefetch in exactly this case). */
    if (op->rfp_validated) {
      if (on_path)
        STAT_EVENT(proc_id, RFP_DROP_LOAD_FIRST);
      e->op = NULL;
      continue;
    }

    if (RFP_QUEUE_MAX_WAIT_CYCLES &&
        cycle_count - e->launch_cycle > RFP_QUEUE_MAX_WAIT_CYCLES) {
      if (on_path)
        STAT_EVENT(proc_id, RFP_DROP_WAIT_TIMEOUT);
      e->op = NULL;
      continue;
    }

    /* An older store still draining to memory can own this address (§2.3). */
    if (scan_stores(e->pred_va, e->mem_size)) {
      if (on_path)
        STAT_EVENT(proc_id, RFP_DROP_STORE_CONFLICT);
      e->op = NULL;
      continue;
    }

    uns bank = (uns)((e->pred_va >> dcache->dcache.shift_bits) &
                     N_BIT_MASK(LOG2(DCACHE_BANKS)));
    Ports* ports = (RFP_PORT_PRIORITY == 1) ? &state->dedicated_ports[bank]
                                            : &dcache->ports[bank];
    if (!get_read_port(ports)) {
      STAT_EVENT(proc_id, RFP_PORT_DENIED_CYCLES);
      if (RFP_PORT_FAIL_POLICY == 1) {
        if (on_path)
          STAT_EVENT(proc_id, RFP_DROP_PORT_UNAVAILABLE);
        e->op = NULL;
        continue;
      }
      if (RFP_PORT_FAIL_POLICY == 2) {
        STAT_EVENT(proc_id, RFP_SKIPPED_PAST_HEAD);
        continue; /* let a younger packet use the bandwidth instead */
      }
      /* Policy 0 keeps the paper's oldest-first order and retries next cycle. */
      STAT_EVENT(proc_id, RFP_HEAD_OF_LINE_BLOCKED_CYCLES);
      break;
    }

    if (RFP_PORT_PRIORITY == 1)
      STAT_EVENT(proc_id, RFP_DEDICATED_PORT_CYCLES_USED);
    else
      state->ports_taken_this_cycle++;
    probes++;

    /* Both averages divide by on-path counters (RFP_EXECUTED, RFP_INJECTED), so
       the numerators have to follow the same population -- otherwise off-path
       waits inflate the reported wait by the off-path probe ratio. */
    Counter waited = cycle_count - e->launch_cycle;
    if (on_path) {
      INC_STAT_EVENT(proc_id, RFP_LAUNCH_TO_PROBE_TOTAL, waited);
      INC_STAT_EVENT(proc_id, RFP_LAUNCH_TO_PROBE_AVG, waited);
      INC_STAT_EVENT(proc_id, RFP_QUEUE_WAIT_TOTAL, waited);
      INC_STAT_EVENT(proc_id, RFP_QUEUE_WAIT_AVG, waited);
    }
    STAT_EVENT(proc_id, RFP_PROBE_L1_ACCESSES);
    if (!on_path)
      STAT_EVENT(proc_id, RFP_OFFPATH_PROBES);

    Addr line_addr;
    Dcache_Data* line = (Dcache_Data*)cache_access(
      &dcache->dcache, e->pred_va, &line_addr, RFP_PROBE_UPDATES_REPL);
    /* Record the probe whether it hit or missed: the port was spent either way,
       which is what the wasted-bandwidth accounting needs. */
    op->rfp_probe_cycle = cycle_count;
    if (line) {
      /* The line is in the L1, so the value reaches the register file after the
         usual access latency.  Whether that beats the load's own access is
         decided at validation. */
      op->rfp_data_ready_cycle = cycle_count + DCACHE_CYCLES;
      if (on_path) {
        STAT_EVENT(proc_id, RFP_EXECUTED);
        STAT_EVENT(proc_id, RFP_EXECUTED_PCT);
      }
    } else if (RFP_L1_MISS_POLICY == 1) {
      /* Continue past the L1 like a demand miss.  This is the only policy that
         can pollute the cache or hold an MSHR, so those costs land in the
         measurements rather than being assumed away. */
      if (!rfp_send_to_lower_levels(proc_id, op, line_addr) && on_path)
        STAT_EVENT(proc_id, RFP_DROP_MSHR_UNAVAILABLE);
    } else if (on_path) {
      /* Policy 0 abandons a missing prefetch and lets the load fetch the line
         itself. */
      STAT_EVENT(proc_id, RFP_DROP_L1_MISS);
    }
    e->op = NULL;
  }

  rfp_queue_compact(state);
}

void rfp_account_dcache_ports(uns proc_id, Dcache_Stage* dcache) {
  if (!rfp_active())
    return;
  RFP_Core_State* state = rfp_get_state(proc_id);
  if (!state->initialized)
    return;

  uns used = 0;
  for (uns bank = 0; bank < DCACHE_BANKS; ++bank) {
    Ports* p = &dcache->ports[bank];
    if (p->read_last_cycle == cycle_count)
      used += p->read_ports_in_use;
  }

  uns available = DCACHE_BANKS * DCACHE_READ_PORTS;
  uns by_prefetch =
    (state->port_cycle == cycle_count) ? state->ports_taken_this_cycle : 0;
  /* Dedicated ports live outside the demand pool, so they never show up in
     `used`; keep the two accounted separately. */
  uns by_demand = used > by_prefetch ? used - by_prefetch : 0;

  INC_STAT_EVENT(proc_id, RFP_PORT_CYCLES_AVAILABLE, available);
  INC_STAT_EVENT(proc_id, RFP_PORT_CYCLES_USED_BY_DEMAND, by_demand);
  INC_STAT_EVENT(proc_id, RFP_PORT_CYCLES_USED_BY_PREFETCH, by_prefetch);
  INC_STAT_EVENT(proc_id, RFP_PORT_UTILIZATION_BY_PREFETCH_PCT, by_prefetch);
  INC_STAT_EVENT(proc_id, RFP_PORT_CYCLES_IDLE,
                 available > used ? available - used : 0);
}

void rfp_note_demand_port_denied(uns proc_id) {
  if (!rfp_active())
    return;
  RFP_Core_State* state = rfp_get_state(proc_id);
  if (!state->initialized)
    return;
  /* Only prefetches that actually took a demand port this cycle can be blamed.
     Under the default priority the drain runs after every demand load, so this
     must stay at zero -- that is the check that "RFP never delays demand". */
  if (state->port_cycle == cycle_count && state->ports_taken_this_cycle) {
    STAT_EVENT(proc_id, RFP_DEMAND_DELAYED_BY_PREFETCH_OPS);
    INC_STAT_EVENT(proc_id, RFP_DEMAND_DELAYED_BY_PREFETCH_CYCLES, 1);
  }
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
    /* A wrong prefetch that already probed spent L1 bandwidth for nothing --
       the only bandwidth RFP adds over the baseline (paper §3). */
    if (op->rfp_probe_cycle != MAX_CTR)
      STAT_EVENT(proc_id, RFP_WRONG_PROBE_ACCESSES);
    return FALSE;
  }

  /* An older store still draining to memory can hold data this address needs,
     and the prefetch read the cache instead.  Same rule the oracle path uses. */
  if (scan_stores(op->oracle_info.va, op->oracle_info.mem_size)) {
    STAT_EVENT(proc_id, RFP_VAL_STORE_CONFLICT);
    return FALSE;
  }

  /* The address was right and nothing forbade the prefetch.  What remains is
     whether the data actually got here in time to save anything. */
  if (op->rfp_data_ready_cycle == MAX_CTR) {
    STAT_EVENT(proc_id, RFP_NOT_EXECUTED);
    return FALSE;
  }

  /* Earliest the register file could supply the value, and what the load would
     otherwise pay.  The prefetch hit the L1, so the demand access would hit
     too -- DCACHE_CYCLES is the right comparison, not a miss latency. */
  Counter ready = op->rfp_data_ready_cycle;
  Counter deliver = MAX2(ready, cycle_count + RFP_HIT_LATENCY);
  Counter demand_done =
    cycle_count + DCACHE_CYCLES + op->inst_info->extra_ld_latency;

  if (deliver >= demand_done) {
    /* The prefetch completed, but not early enough to beat the load's own
       access, so it saves nothing. */
    STAT_EVENT(proc_id, RFP_USEFUL_LATE);
    INC_STAT_EVENT(proc_id, RFP_LATE_CYCLES_TOTAL, deliver - demand_done);
    return FALSE;
  }

  Counter saved = demand_done - deliver;

  if (ready <= cycle_count) {
    STAT_EVENT(proc_id, RFP_USEFUL_FULL);
    STAT_EVENT(proc_id, RFP_LEAD_TIME_SAMPLES);
    INC_STAT_EVENT(proc_id, RFP_LEAD_TIME_TOTAL, cycle_count - ready);
    INC_STAT_EVENT(proc_id, RFP_LEAD_TIME_AVG, cycle_count - ready);
  } else {
    STAT_EVENT(proc_id, RFP_USEFUL_PARTIAL);
    INC_STAT_EVENT(proc_id, RFP_LATE_CYCLES_TOTAL, ready - cycle_count);
    INC_STAT_EVENT(proc_id, RFP_LATE_CYCLES_AVG, ready - cycle_count);
  }

  STAT_EVENT(proc_id, RFP_USEFUL);
  STAT_EVENT(proc_id, RFP_USEFUL_PCT);
  STAT_EVENT(proc_id, RFP_USEFUL_OF_TARGET_PCT);
  INC_STAT_EVENT(proc_id, RFP_SAVED_CYCLES_TOTAL, saved);
  INC_STAT_EVENT(proc_id, RFP_SAVED_CYCLES_AVG, saved);

  /* The value is in (or on its way to) the destination physical register, so the
     load completes without touching the cache -- the probe already spent that
     access.  Dependents wake off `deliver` instead of the full access latency,
     which is the whole point of prefetching into the register file. */
  op->state = OS_SCHEDULED;
  op->dcache_cycle = cycle_count;
  op->done_cycle = deliver;
  op->wake_cycle = deliver;
  op->oracle_info.dcmiss = FALSE;
  op->engine_info.dcmiss = FALSE;

  /* Claiming RF coverage is a separate question from taking the speedup.  The
     data is already in the register file either way, so the load always keeps
     what the prefetch bought it; but a load helped only marginally is still
     sitting on the branch's critical path, and RF coverage is what strips the
     rest of its slice of IQ priority (zereco_ARCHITECTURE.md §7.1).  Only claim
     it once the saving clears the configured bar.  Consumed after retire, so it
     never reorders the current occurrence. */
  if (saved >= RFP_COVERED_MIN_SAVED_CYCLES)
    op->zereco_rf_covered = TRUE;
  else
    STAT_EVENT(proc_id, RFP_USEFUL_BELOW_THRESHOLD);

  wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
  return TRUE;
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
