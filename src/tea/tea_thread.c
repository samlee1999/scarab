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
  tea->current_fetch_chain = 0;

  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    tea->chains[i].state = CHAIN_INACTIVE;
    tea->chains[i].target_h2p_pc = 0;
    tea->chains[i].target_h2p_op_num = 0;
    tea->chains[i].main_h2p_op = NULL;
    tea->chains[i].saved_unique_num = 0;
    tea->chains[i].tea_op_count = 0;
    tea->chains[i].tea_ops_fetched = 0;
  }

  tea->tea_op_counter = 0x8000000000000000ULL;
  tea->tea_start_cycle = 0;

  /* Don't reset statistics */
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

  /* Check dependency chain exists */
  Dependency_Chain_Cache_Entry* chain = get_dependency_chain(proc_id, h2p_pc);
  if (!chain || !chain->is_valid || chain->chain_length == 0) {
    STAT_EVENT(proc_id, TEA_TRIGGER_SKIP_NO_CHAIN);
    return;
  }

  /* Fill the slot */
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
  tea->state = TEA_FETCHING;

  /* First active chain: take Shadow RAT snapshot and start fetch */
  if (tea->num_active_chains == 1) {
    shadow_rat_snapshot(proc_id);
    reset_tea_fetch_stage(proc_id);
    tea->current_fetch_chain = slot;
    setup_fetch_for_chain(proc_id, slot, chain);
    tea->tea_start_cycle = cycle_count;
  }
  /* Otherwise: this chain waits as CHAIN_FETCHING; update_tea_fetch_stage()
   * picks it up via find_next_fetching_chain() after the current chain completes. */

  tea->stat_tea_triggers++;
  STAT_EVENT(proc_id, TEA_TRIGGERS);

  _DEBUG(0, DEBUG_TEA, "TEA TRIGGERED slot=%d: PC=0x%llx op_num=%llu chain_len=%d cycle=%llu\n",
         slot, (unsigned long long)h2p_pc, (unsigned long long)h2p_op_num,
         chain->chain_length, (unsigned long long)cycle_count);
}

/* terminate_tea_chain: Terminate a single H2P chain, flush its ops.
 * If it was the last active chain, also reset shared resources. */
void terminate_tea_chain(uns proc_id, int chain_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, chain_id >= 0 && chain_id < MAX_TEA_CHAINS);

  Tea_Thread* tea = tea_threads[proc_id];
  Tea_H2P_Chain* c = &tea->chains[chain_id];
  uns8 h2p_chain_id = (uns8)(chain_id + 1);  /* 1-based */

  ASSERT(proc_id, c->state != CHAIN_INACTIVE);

  /* Flush fetch stage if currently fetching this chain */
  if (c->state == CHAIN_FETCHING &&
      tea->current_fetch_chain == (uns)chain_id) {
    recover_tea_fetch_stage_by_chain(proc_id, h2p_chain_id);
  }

  /* Flush rename stage output for this chain */
  recover_tea_rename_stage_by_chain(proc_id, h2p_chain_id);

  /* Flush node/exec/dcache/ready/sched for this chain */
  flush_tea_ops_by_chain_id(proc_id, h2p_chain_id);

  /* Invalidate store buffer entries for this chain */
  tea_store_buffer_clear_by_chain_id(proc_id, h2p_chain_id);

  /* Reset chain slot */
  c->state = CHAIN_INACTIVE;
  c->target_h2p_pc = 0;
  c->target_h2p_op_num = 0;
  c->main_h2p_op = NULL;
  c->saved_unique_num = 0;
  c->tea_op_count = 0;
  c->tea_ops_fetched = 0;
  tea->num_active_chains--;
  STAT_EVENT(proc_id, TEA_CHAIN_TERMINATED);

  /* When all chains are done, reset shared resources */
  if (tea->num_active_chains == 0) {
    reset_tea_preg_pool(proc_id);
    reset_tea_store_buffer(proc_id);
    recover_tea_rename_stage(proc_id);
    tea->state = TEA_IDLE;
  }
}

/* terminate_tea_thread: Terminate ALL active chains at once (fast path). */
void terminate_tea_thread(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);

  Tea_Thread* tea = tea_threads[proc_id];

  /* Mark all chains inactive (skip individual per-chain cleanup) */
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    if (tea->chains[i].state != CHAIN_INACTIVE) {
      tea->chains[i].state = CHAIN_INACTIVE;
      tea->chains[i].main_h2p_op = NULL;
      tea->chains[i].tea_op_count = 0;
      tea->chains[i].tea_ops_fetched = 0;
    }
  }
  tea->num_active_chains = 0;

  /* Flush all stages (full flush) */
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
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_threads || !tea_threads[proc_id]) {
    return FALSE;
  }

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
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_threads || !tea_threads[proc_id]) {
    return;
  }

  Tea_Thread* tea = tea_threads[proc_id];

  if (tea->num_active_chains == 0) {
    return;
  }

  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    Tea_H2P_Chain* c = &tea->chains[i];

    switch (c->state) {
      case CHAIN_FETCHING:
        /* Fetch completion is detected by update_tea_fetch_stage() chain switching */
        break;

      case CHAIN_EXECUTING:
        /* Terminate chain when all its ops have completed (retired or flushed) */
        if (c->tea_op_count == 0 && c->tea_ops_fetched > 0) {
          terminate_tea_chain(proc_id, i);
          /* i may now point to a newly-set CHAIN_INACTIVE; loop continues safely */
        }
        break;

      case CHAIN_INACTIVE:
      default:
        break;
    }
  }
}

/**************************************************************************************/
/* Op Management */

void tea_op_completed(uns proc_id, Op* op) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, op && op->thread_id == 1);

  /* Mark op as OS_DONE so node_retire_tea_ops() can safely free it.
   * Per-chain tea_op_count is decremented in node_retire_tea_ops() at the
   * moment the op is physically removed from the Node Table. */
  op->state = OS_DONE;

  STAT_EVENT(proc_id, TEA_OPS_EXECUTED);
}

void tea_op_flushed(uns proc_id, Op* op) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, op && op->thread_id == 1);

  Tea_Thread* tea = tea_threads[proc_id];

  uns8 cid = op->h2p_chain_id;
  if (cid > 0 && cid <= MAX_TEA_CHAINS) {
    Tea_H2P_Chain* c = &tea->chains[cid - 1];
    if (c->tea_op_count > 0) {
      c->tea_op_count--;
    }
  }
}
