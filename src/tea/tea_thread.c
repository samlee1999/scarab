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

static int find_next_fetching_chain(uns proc_id) {
  Tea_Thread* tea = tea_threads[proc_id];
  int start = (tea->current_fetch_chain < 0) ? 0 : tea->current_fetch_chain;
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    int idx = (start + 1 + i) % MAX_TEA_CHAINS;
    if (tea->chains[idx].state == CHAIN_FETCHING)
      return idx;
  }
  return -1;
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

  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    memset(&tea->chains[i], 0, sizeof(Tea_H2P_Chain));
    tea->chains[i].state = CHAIN_INACTIVE;
  }
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
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
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
  tea->num_active_chains++;

  /* First active chain: take Shadow RAT snapshot and reset the shared counter */
  if (tea->num_active_chains == 1) {
    shadow_rat_snapshot(proc_id);
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

  /* Update concurrent-chain high watermark stat */
  if (tea->num_active_chains > 1) {
    /* TEA_CHAINS_CONCURRENT_MAX is a high-watermark: we use INC_STAT_EVENT only
     * if new count exceeds previous max (tracked via STAT buckets). */
  }

  _DEBUG(0, DEBUG_TEA, "TEA TRIGGERED slot=%d PC=0x%llx op_num=%llu chain_len=%d cycle=%llu\n",
         slot, (unsigned long long)h2p_pc, (unsigned long long)h2p_op_num,
         chain->chain_length, (unsigned long long)cycle_count);
}

/* terminate_tea_chain: Flush a single chain and reclaim its resources.
 * Called by: exec_stage_bp_resolve (per-chain result), update_tea_thread
 *             (natural completion), recover_tea_on_flush (main recovery). */
void terminate_tea_chain(uns proc_id, int chain_slot) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, chain_slot >= 0 && chain_slot < MAX_TEA_CHAINS);

  Tea_Thread* tea = tea_threads[proc_id];
  Tea_H2P_Chain* c = &tea->chains[chain_slot];

  if (c->state == CHAIN_INACTIVE)
    return;  /* Already terminated */

  uns8 h2p_chain_id = (uns8)(chain_slot + 1);  /* 1-based */

  /* 1. If this chain is currently being fetched, clear it from the fetch stage */
  if (c->state == CHAIN_FETCHING && tea->current_fetch_chain == chain_slot) {
    recover_tea_fetch_stage_by_chain(proc_id, h2p_chain_id);

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
        terminate_tea_chain(proc_id, next);
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

  /* 5. Reset chain state */
  c->state = CHAIN_INACTIVE;
  c->target_h2p_pc = 0;
  c->target_h2p_op_num = 0;
  c->main_h2p_op = NULL;
  c->tea_op_count = 0;
  c->tea_ops_fetched = 0;
  tea->num_active_chains--;

  STAT_EVENT(proc_id, TEA_CHAIN_TERMINATED);

  /* 6. If no chains remain, clean up shared resources */
  if (tea->num_active_chains == 0) {
    reset_tea_preg_pool(proc_id);
    reset_tea_store_buffer(proc_id);
    recover_tea_rename_stage(proc_id);  /* Invalidate Shadow RAT */
    tea->state = TEA_IDLE;
    tea->current_fetch_chain = -1;
  }
}

void terminate_tea_thread(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);

  Tea_Thread* tea = tea_threads[proc_id];

  /* Mark all chains inactive (skip per-chain teardown; full flush follows) */
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    tea->chains[i].state = CHAIN_INACTIVE;
    tea->chains[i].main_h2p_op = NULL;
    tea->chains[i].tea_op_count = 0;
    tea->chains[i].tea_ops_fetched = 0;
  }
  tea->num_active_chains = 0;
  tea->current_fetch_chain = -1;

  /* Full flush of all pipeline stages */
  recover_tea_fetch_stage(proc_id);
  recover_tea_rename_stage(proc_id);
  flush_tea_ops_from_node_stage(proc_id);
  reset_tea_preg_pool(proc_id);
  reset_tea_store_buffer(proc_id);

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
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    Tea_H2P_Chain* c = &tea->chains[i];
    if (c->state == CHAIN_EXECUTING &&
        c->tea_op_count == 0 && c->tea_ops_fetched > 0) {
      terminate_tea_chain(proc_id, i);
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
