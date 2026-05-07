/* Copyright 2020 HPS/SAFARI Research Groups
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
 * File         : tea/tea_thread.c
 * Author       : TEA Implementation
 * Date         : 2025
 * Description  : TEA Thread state management — multi-H2P chain support
 ***************************************************************************************/

#include "tea/tea_thread.h"
#include "tea/tea_fetch_stage.h"
#include "tea/tea_rename.h"
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "core.param.h"
#include "dependency_chain_cache.h"
#include "node_stage.h"
#include "tea/tea_store_buffer.h"
#include "statistics.h"
#include "debug/debug.param.h"
#include "debug/debug_macros.h"

#include <stdlib.h>
#include <string.h>

/**************************************************************************************/
/* Global Variables */

Tea_Thread** tea_threads = NULL;

/**************************************************************************************/
/* Static helpers */

uns tea_max_chains(uns proc_id) {
  ASSERT(proc_id, TEA_MAX_CHAINS > 0);
  ASSERT(proc_id, TEA_MAX_CHAINS <= MAX_TEA_CHAINS);
  return TEA_MAX_CHAINS;
}

Flag tea_chain_slot_is_valid(uns proc_id, int chain_slot) {
  return chain_slot >= 0 && (uns)chain_slot < tea_max_chains(proc_id);
}

static int find_next_fetching_chain(uns proc_id) {
  Tea_Thread* tea = tea_threads[proc_id];
  int max_chains = (int)tea_max_chains(proc_id);
  int start = (tea->current_fetch_chain < 0) ? 0 : tea->current_fetch_chain;
  for (int i = 0; i < max_chains; i++) {
    int idx = (start + 1 + i) % max_chains;
    if (tea->chains[idx].state == CHAIN_FETCHING)
      return idx;
  }
  return -1;
}

static void count_chain_states(Tea_Thread* tea, int max_chains,
                               uns* active, uns* fetching, uns* executing) {
  *active = 0;
  *fetching = 0;
  *executing = 0;

  for (int i = 0; i < max_chains; i++) {
    switch (tea->chains[i].state) {
      case CHAIN_FETCHING:
        (*active)++;
        (*fetching)++;
        break;
      case CHAIN_EXECUTING:
        (*active)++;
        (*executing)++;
        break;
      case CHAIN_INACTIVE:
      default:
        break;
    }
  }
}

static void record_chain_depth_bucket(uns proc_id, uns active, Flag trigger_bucket) {
  if (trigger_bucket) {
    if      (active == 0) STAT_EVENT(proc_id, TEA_TRIGGER_ACTIVE_CHAINS_0);
    else if (active == 1) STAT_EVENT(proc_id, TEA_TRIGGER_ACTIVE_CHAINS_1);
    else if (active == 2) STAT_EVENT(proc_id, TEA_TRIGGER_ACTIVE_CHAINS_2);
    else if (active == 3) STAT_EVENT(proc_id, TEA_TRIGGER_ACTIVE_CHAINS_3);
    else if (active == 4) STAT_EVENT(proc_id, TEA_TRIGGER_ACTIVE_CHAINS_4);
    else if (active <= 8) STAT_EVENT(proc_id, TEA_TRIGGER_ACTIVE_CHAINS_5_8);
    else if (active <= 12) STAT_EVENT(proc_id, TEA_TRIGGER_ACTIVE_CHAINS_9_12);
    else                  STAT_EVENT(proc_id, TEA_TRIGGER_ACTIVE_CHAINS_13_16);
  } else {
    if      (active == 1) STAT_EVENT(proc_id, TEA_ACTIVE_CHAINS_1);
    else if (active == 2) STAT_EVENT(proc_id, TEA_ACTIVE_CHAINS_2);
    else if (active == 3) STAT_EVENT(proc_id, TEA_ACTIVE_CHAINS_3);
    else if (active == 4) STAT_EVENT(proc_id, TEA_ACTIVE_CHAINS_4);
    else if (active <= 8) STAT_EVENT(proc_id, TEA_ACTIVE_CHAINS_5_8);
    else if (active <= 12) STAT_EVENT(proc_id, TEA_ACTIVE_CHAINS_9_12);
    else                  STAT_EVENT(proc_id, TEA_ACTIVE_CHAINS_13_16);
  }
}

static void update_max_active_chains(uns proc_id, Tea_Thread* tea, uns active) {
  if (active > tea->stat_max_active_chains) {
    INC_STAT_EVENT(proc_id, TEA_CHAINS_CONCURRENT_MAX,
                   active - tea->stat_max_active_chains);
    tea->stat_max_active_chains = active;
  }
}

static void record_trigger_chain_pressure(uns proc_id, Tea_Thread* tea,
                                          int max_chains) {
  uns active, fetching, executing;
  count_chain_states(tea, max_chains, &active, &fetching, &executing);

  INC_STAT_EVENT(proc_id, TEA_TRIGGER_ACTIVE_CHAINS_TOTAL, active);
  INC_STAT_EVENT(proc_id, TEA_TRIGGER_FETCHING_CHAINS_TOTAL, fetching);
  INC_STAT_EVENT(proc_id, TEA_TRIGGER_EXECUTING_CHAINS_TOTAL, executing);
  INC_STAT_EVENT(proc_id, TEA_TRIGGER_FREE_SLOTS_TOTAL, max_chains - active);
  record_chain_depth_bucket(proc_id, active, TRUE);

  if (active >= (uns)max_chains) {
    INC_STAT_EVENT(proc_id, TEA_TRIGGER_FULL_FETCHING_CHAINS_TOTAL, fetching);
    INC_STAT_EVENT(proc_id, TEA_TRIGGER_FULL_EXECUTING_CHAINS_TOTAL, executing);
  }
}

static void record_active_chain_cycle_stats(uns proc_id, Tea_Thread* tea,
                                            int max_chains) {
  uns active, fetching, executing;
  count_chain_states(tea, max_chains, &active, &fetching, &executing);
  if (active == 0)
    return;

  STAT_EVENT(proc_id, TEA_ACTIVE_CYCLES);
  INC_STAT_EVENT(proc_id, TEA_ACTIVE_CHAIN_SLOTS_TOTAL, active);
  INC_STAT_EVENT(proc_id, TEA_FETCHING_CHAIN_SLOTS_TOTAL, fetching);
  INC_STAT_EVENT(proc_id, TEA_EXECUTING_CHAIN_SLOTS_TOTAL, executing);
  record_chain_depth_bucket(proc_id, active, FALSE);
  update_max_active_chains(proc_id, tea, active);
}

static void record_chain_length_bucket(uns proc_id, uns length) {
  INC_STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_TOTAL, length);
  if      (length <= 8)  STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_1_8);
  else if (length <= 16) STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_9_16);
  else if (length <= 32) STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_17_32);
  else if (length <= 64) STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_33_64);
  else                   STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_65_PLUS);
}

static void record_chain_lifetime_bucket(uns proc_id, Counter lifetime) {
  INC_STAT_EVENT(proc_id, TEA_CHAIN_LIFETIME_TOTAL, lifetime);
  if      (lifetime < 10)   STAT_EVENT(proc_id, TEA_CHAIN_LIFETIME_0_9);
  else if (lifetime < 50)   STAT_EVENT(proc_id, TEA_CHAIN_LIFETIME_10_49);
  else if (lifetime < 100)  STAT_EVENT(proc_id, TEA_CHAIN_LIFETIME_50_99);
  else if (lifetime < 500)  STAT_EVENT(proc_id, TEA_CHAIN_LIFETIME_100_499);
  else if (lifetime < 1000) STAT_EVENT(proc_id, TEA_CHAIN_LIFETIME_500_999);
  else                      STAT_EVENT(proc_id, TEA_CHAIN_LIFETIME_1000_PLUS);
}

static void record_termination_reason(uns proc_id,
                                      Tea_Chain_Termination_Reason reason) {
  switch (reason) {
    case TEA_CHAIN_TERM_REASON_NATURAL:
      STAT_EVENT(proc_id, TEA_CHAIN_TERM_NATURAL);
      break;
    case TEA_CHAIN_TERM_REASON_H2P_CORRECT:
      STAT_EVENT(proc_id, TEA_CHAIN_TERM_H2P_CORRECT);
      break;
    case TEA_CHAIN_TERM_REASON_EARLY_FLUSH_CASE1:
      STAT_EVENT(proc_id, TEA_CHAIN_TERM_EARLY_FLUSH_CASE1);
      break;
    case TEA_CHAIN_TERM_REASON_MAIN_RECOVERY:
      STAT_EVENT(proc_id, TEA_CHAIN_TERM_MAIN_RECOVERY);
      break;
    case TEA_CHAIN_TERM_REASON_INVALID_MAIN_H2P:
      STAT_EVENT(proc_id, TEA_CHAIN_TERM_INVALID_MAIN_H2P);
      break;
    case TEA_CHAIN_TERM_REASON_DEP_CHAIN_LOST:
      STAT_EVENT(proc_id, TEA_CHAIN_TERM_DEP_CHAIN_LOST);
      break;
    case TEA_CHAIN_TERM_REASON_FULL_THREAD:
      STAT_EVENT(proc_id, TEA_CHAIN_TERM_FULL_THREAD);
      break;
    case TEA_CHAIN_TERM_REASON_UNKNOWN:
    default:
      STAT_EVENT(proc_id, TEA_CHAIN_TERM_UNKNOWN);
      break;
  }
}

static void record_chain_termination_stats(uns proc_id, Tea_H2P_Chain* c,
                                           Tea_Chain_Termination_Reason reason) {
  record_termination_reason(proc_id, reason);

  if (c->trigger_cycle > 0 && cycle_count >= c->trigger_cycle) {
    Counter lifetime = cycle_count - c->trigger_cycle;
    record_chain_lifetime_bucket(proc_id, lifetime);

    if (c->fetch_done_cycle > 0 && c->fetch_done_cycle >= c->trigger_cycle) {
      INC_STAT_EVENT(proc_id, TEA_CHAIN_FETCH_WAIT_TOTAL,
                     c->fetch_done_cycle - c->trigger_cycle);
      if (cycle_count >= c->fetch_done_cycle) {
        INC_STAT_EVENT(proc_id, TEA_CHAIN_EXEC_WAIT_TOTAL,
                       cycle_count - c->fetch_done_cycle);
      }
    } else if (c->state == CHAIN_FETCHING) {
      INC_STAT_EVENT(proc_id, TEA_CHAIN_FETCH_WAIT_TOTAL, lifetime);
    }
  }
}

/**************************************************************************************/
/* Initialization and Reset */

void init_tea_thread(uns proc_id) {
  if (!tea_threads) {
    tea_threads = (Tea_Thread**)calloc(NUM_CORES, sizeof(Tea_Thread*));
    ASSERT(0, tea_threads);
  }

  ASSERT(proc_id, proc_id < NUM_CORES);

  tea_threads[proc_id] = (Tea_Thread*)calloc(1, sizeof(Tea_Thread));
  ASSERT(proc_id, tea_threads[proc_id]);

  Tea_Thread* tea = tea_threads[proc_id];
  tea_max_chains(proc_id);
  tea->proc_id = proc_id;

  init_tea_store_buffer(proc_id);
  reset_tea_thread(proc_id);
}

void reset_tea_thread(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);

  Tea_Thread* tea = tea_threads[proc_id];

  tea->state = TEA_IDLE;
  tea->num_active_chains = 0;
  tea->current_fetch_chain = -1;
  tea->tea_op_counter = 0x8000000000000000ULL;
  tea->tea_start_cycle = 0;
  tea->stat_max_active_chains = 0;

  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    memset(&tea->chains[i], 0, sizeof(Tea_H2P_Chain));
    tea->chains[i].state = CHAIN_INACTIVE;
  }
  memset(tea->pending_case1_flushes, 0, sizeof(tea->pending_case1_flushes));
}

/**************************************************************************************/
/* TEA Thread Control */

void trigger_tea_thread(uns proc_id, Addr h2p_pc, Counter h2p_op_num, Op* h2p_op) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, h2p_op);

  Tea_Thread* tea = tea_threads[proc_id];
  STAT_EVENT(proc_id, TEA_TRIGGER_ATTEMPTS);

  /* Find an empty chain slot */
  int slot = -1;
  int max_chains = (int)tea_max_chains(proc_id);
  record_trigger_chain_pressure(proc_id, tea, max_chains);
  for (int i = 0; i < max_chains; i++) {
    if (tea->chains[i].state == CHAIN_INACTIVE) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    STAT_EVENT(proc_id, TEA_TRIGGER_SKIP_FULL);
    return;
  }

  /* Check for a dependency chain */
  Dependency_Chain_Cache_Entry* chain = get_dependency_chain(proc_id, h2p_pc);
  if (!chain || !chain->is_valid || chain->chain_length == 0) {
    STAT_EVENT(proc_id, TEA_TRIGGER_SKIP_NO_CHAIN);
    return;
  }
  record_chain_length_bucket(proc_id, chain->chain_length);

  /* [EXPERIMENT: TEA_PERFECT_LOAD] Count load ops in triggered dependency chain */
  for (uns ci = 0; ci < chain->chain_length; ci++) {
    if (chain->chain[ci].table_info &&
        chain->chain[ci].table_info->mem_type == MEM_LD)
      STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_TOTAL);
  }

  /* Fill chain slot */
  Tea_H2P_Chain* c = &tea->chains[slot];
  c->state = CHAIN_FETCHING;
  c->target_h2p_pc = h2p_pc;
  c->target_h2p_op_num = h2p_op_num;
  c->main_h2p_op = h2p_op;
  c->saved_unique_num = h2p_op->unique_num;
  c->h2p_oracle_info = h2p_op->oracle_info;
  c->h2p_recovery_info = h2p_op->recovery_info;
  c->tea_op_count = 0;
  c->tea_ops_fetched = 0;
  c->trigger_cycle = cycle_count;
  c->fetch_done_cycle = 0;
  tea->num_active_chains++;
  update_max_active_chains(proc_id, tea, tea->num_active_chains);

  /* Per-chain Shadow RAT snapshot: each chain gets its own independent mapping
   * from the main thread's current RAT state at trigger time. */
  shadow_rat_snapshot(proc_id, slot);

  /* First active chain: reset shared counters and advance state machine */
  if (tea->num_active_chains == 1) {
    tea->tea_op_counter = 0x8000000000000000ULL;
    tea->state = TEA_FETCHING;
  }

  /* If no chain is currently being fetched, start fetching this one */
  if (tea->current_fetch_chain < 0) {
    tea->current_fetch_chain = slot;
    reset_tea_fetch_stage(proc_id);
    tea_fetch_stages[proc_id]->current_chain_id = slot;
  }

  tea->stat_tea_triggers++;
  STAT_EVENT(proc_id, TEA_TRIGGERS);

  _DEBUG(0, DEBUG_TEA, "TEA TRIGGERED slot=%d PC=0x%llx op_num=%llu chain_len=%d cycle=%llu\n",
         slot, (unsigned long long)h2p_pc, (unsigned long long)h2p_op_num,
         chain->chain_length, (unsigned long long)cycle_count);
}

/* terminate_tea_chain: Flush a single chain and reclaim its resources.
 * Called by: exec_stage_bp_resolve (per-chain result), update_tea_thread
 *             (natural completion), recover_tea_on_flush (main recovery). */
void terminate_tea_chain_with_reason(uns proc_id, int chain_slot,
                                     Tea_Chain_Termination_Reason reason) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, tea_chain_slot_is_valid(proc_id, chain_slot));

  Tea_Thread* tea = tea_threads[proc_id];
  Tea_H2P_Chain* c = &tea->chains[chain_slot];

  if (c->state == CHAIN_INACTIVE)
    return;  /* Already terminated */

  uns8 h2p_chain_id = (uns8)(chain_slot + 1);  /* 1-based */

  /* 1. Flush any ops for this chain still buffered in the fetch stage's SD.
   * Must run unconditionally: the fetch_complete transition (FETCHING→EXECUTING)
   * can fire while ops are still in tf->sd due to a PREG stall in rename.
   * If the chain is then terminated while in CHAIN_EXECUTING, those stale ops
   * would otherwise never be freed. */
  recover_tea_fetch_stage_by_chain(proc_id, h2p_chain_id);

  if (c->state == CHAIN_FETCHING && tea->current_fetch_chain == chain_slot) {
    /* Point current_fetch_chain at the next FETCHING chain, if any */
    int next = find_next_fetching_chain(proc_id);
    tea->current_fetch_chain = next;
    if (next >= 0) {
      /* Setup fetch stage for the next chain */
      Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
      Tea_H2P_Chain* nc = &tea->chains[next];
      Dependency_Chain_Cache_Entry* dep = get_dependency_chain(proc_id, nc->target_h2p_pc);
      if (dep && dep->is_valid && dep->chain_length > 0) {
        tf->active_chain = dep;
        tf->current_chain_idx = 0;
        tf->total_chain_length = dep->chain_length;
        tf->fetch_complete = FALSE;
        tf->current_chain_id = next;
        tf->ops_fetched_this_cycle = 0;
      } else {
        /* Dep chain disappeared; terminate that chain too (recursive) */
        tea->current_fetch_chain = -1;
        terminate_tea_chain_with_reason(proc_id, next,
                                        TEA_CHAIN_TERM_REASON_DEP_CHAIN_LOST);
        /* After recursion, c may still be valid — continue */
      }
    }
  }

  /* 2. Clear any ops for this chain still sitting in the rename stage */
  recover_tea_rename_stage_by_chain(proc_id, h2p_chain_id);

  /* 3. Flush chain's ops from exec/dcache/RS/node table (includes wake-up propagation) */
  flush_tea_ops_by_chain_id(proc_id, h2p_chain_id);

  /* 4. Invalidate this chain's store buffer entries */
  tea_store_buffer_clear_by_chain_id(proc_id, h2p_chain_id);

  record_chain_termination_stats(proc_id, c, reason);

  /* 5. Reset chain state */
  c->state = CHAIN_INACTIVE;
  c->target_h2p_pc = 0;
  c->target_h2p_op_num = 0;
  c->main_h2p_op = NULL;
  c->tea_op_count = 0;
  c->tea_ops_fetched = 0;
  c->trigger_cycle = 0;
  c->fetch_done_cycle = 0;
  tea->num_active_chains--;

  STAT_EVENT(proc_id, TEA_CHAIN_TERMINATED);

  /* 6. Per-chain PREG pool reset: restore this slot's pool independently.
   * Shadow RAT was already invalidated by recover_tea_rename_stage_by_chain (step 2). */
  reset_tea_preg_pool(proc_id, chain_slot);

  /* 7. If no chains remain, clean up shared resources */
  if (tea->num_active_chains == 0) {
    reset_tea_store_buffer(proc_id);
    recover_tea_rename_stage(proc_id);
    tea->state = TEA_IDLE;
    tea->current_fetch_chain = -1;
  }
}

void terminate_tea_chain(uns proc_id, int chain_slot) {
  terminate_tea_chain_with_reason(proc_id, chain_slot,
                                  TEA_CHAIN_TERM_REASON_UNKNOWN);
}

void terminate_tea_thread(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);

  Tea_Thread* tea = tea_threads[proc_id];
  int max_chains = (int)tea_max_chains(proc_id);

  /* Mark all chains inactive (skip per-chain teardown; full flush follows) */
  for (int i = 0; i < max_chains; i++) {
    if (tea->chains[i].state != CHAIN_INACTIVE) {
      record_chain_termination_stats(proc_id, &tea->chains[i],
                                     TEA_CHAIN_TERM_REASON_FULL_THREAD);
      STAT_EVENT(proc_id, TEA_CHAIN_TERMINATED);
    }
    tea->chains[i].state = CHAIN_INACTIVE;
    tea->chains[i].main_h2p_op = NULL;
    tea->chains[i].tea_op_count = 0;
    tea->chains[i].tea_ops_fetched = 0;
    tea->chains[i].trigger_cycle = 0;
    tea->chains[i].fetch_done_cycle = 0;
  }
  tea->num_active_chains = 0;
  tea->current_fetch_chain = -1;

  /* Full flush of all pipeline stages */
  recover_tea_fetch_stage(proc_id);
  recover_tea_rename_stage(proc_id);
  flush_tea_ops_from_node_stage(proc_id);
  for (int i = 0; i < max_chains; i++)
    reset_tea_preg_pool(proc_id, i);
  reset_tea_store_buffer(proc_id);
  memset(tea->pending_case1_flushes, 0, sizeof(tea->pending_case1_flushes));

  tea->state = TEA_IDLE;
}

/**************************************************************************************/
/* State Queries */

Flag tea_is_active(uns proc_id) {
  if (!tea_threads || !tea_threads[proc_id])
    return FALSE;
  return tea_threads[proc_id]->num_active_chains > 0;
}

Tea_State tea_get_state(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  return tea_threads[proc_id]->state;
}

/**************************************************************************************/
/* Per-Cycle Update */

void update_tea_thread(uns proc_id) {
  if (!tea_threads || !tea_threads[proc_id])
    return;

  Tea_Thread* tea = tea_threads[proc_id];
  if (tea->num_active_chains == 0)
    return;

  /* Detect CHAIN_EXECUTING chains whose ops all completed → terminate */
  int max_chains = (int)tea_max_chains(proc_id);
  record_active_chain_cycle_stats(proc_id, tea, max_chains);
  for (int i = 0; i < max_chains; i++) {
    Tea_H2P_Chain* c = &tea->chains[i];
    if (c->state == CHAIN_EXECUTING &&
        c->tea_op_count == 0 && c->tea_ops_fetched > 0) {
      terminate_tea_chain_with_reason(proc_id, i,
                                      TEA_CHAIN_TERM_REASON_NATURAL);
    }
  }
}

/**************************************************************************************/
/* Op Management */

void tea_op_completed(uns proc_id, Op* op) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, op && op->thread_id == 1);

  /* Only mark OS_DONE — tea_op_count is decremented in node_retire_tea_ops()
   * to prevent use-after-free for 0-latency ops. */
  op->state = OS_DONE;
  STAT_EVENT(proc_id, TEA_OPS_EXECUTED);
}

/**************************************************************************************/
/* Case 1 Pending Early Flush */

Flag tea_record_pending_case1_flush(uns proc_id, Op* main_h2p,
                                    Counter detect_cycle,
                                    Tea_Case1_Main_Stage main_stage_at_detect) {
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  Tea_Thread* tea = tea_threads[proc_id];
  int max_chains = (int)tea_max_chains(proc_id);
  for (int i = 0; i < max_chains; i++) {
    if (!tea->pending_case1_flushes[i].valid) {
      tea->pending_case1_flushes[i].valid               = TRUE;
      tea->pending_case1_flushes[i].main_h2p_op         = main_h2p;
      tea->pending_case1_flushes[i].main_h2p_unique_num = main_h2p->unique_num;
      tea->pending_case1_flushes[i].main_h2p_op_num     = main_h2p->op_num;
      tea->pending_case1_flushes[i].detect_cycle        = detect_cycle;
      tea->pending_case1_flushes[i].main_stage_at_detect = main_stage_at_detect;
      return TRUE;
    }
  }
  /* All slots occupied: drop conservatively and expose it in stats. */
  STAT_EVENT(proc_id, TEA_EARLY_FLUSH_CASE1_PENDING_DROPPED);
  return FALSE;
}

void tea_clear_pending_case1_flushes(uns proc_id) {
  if (!tea_threads || !tea_threads[proc_id]) return;
  memset(tea_threads[proc_id]->pending_case1_flushes, 0,
         sizeof(tea_threads[proc_id]->pending_case1_flushes));
}

void tea_selective_clear_pending_case1_flushes(uns proc_id, Counter recovery_op_num) {
  if (!tea_threads || !tea_threads[proc_id]) return;
  Tea_Thread* tea = tea_threads[proc_id];
  int max_chains = (int)tea_max_chains(proc_id);
  for (int i = 0; i < max_chains; i++) {
    Tea_Pending_Case1_Flush* pf = &tea->pending_case1_flushes[i];
    if (pf->valid && pf->main_h2p_op_num >= recovery_op_num)
      pf->valid = FALSE;
  }
}
