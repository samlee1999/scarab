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
 * Description  : TEA (Timely, Efficient, and Accurate) Thread state management
 ***************************************************************************************/

#include "tea/tea_thread.h"
#include "tea/tea_fetch_stage.h"
#include "tea/tea_rename.h"
#include "globals/assert.h"
#include "globals/global_vars.h"
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
  /* Allocate per-core array on first call */
  if (!tea_threads) {
    tea_threads = (Tea_Thread**)calloc(NUM_CORES, sizeof(Tea_Thread*));
    ASSERT(0, tea_threads);
  }

  ASSERT(proc_id, proc_id < NUM_CORES);

  /* Allocate TEA thread state for this core */
  tea_threads[proc_id] = (Tea_Thread*)calloc(1, sizeof(Tea_Thread));
  ASSERT(proc_id, tea_threads[proc_id]);

  Tea_Thread* tea = tea_threads[proc_id];
  tea->proc_id = proc_id;

  /* Phase 4.1: Initialize TEA store buffer */
  init_tea_store_buffer(proc_id);

  reset_tea_thread(proc_id);
}

void reset_tea_thread(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);

  Tea_Thread* tea = tea_threads[proc_id];

  tea->state = TEA_IDLE;
  tea->target_h2p_pc = 0;
  tea->target_h2p_op_num = 0;
  tea->main_h2p_op = NULL;
  tea->current_block_pc = 0;
  tea->current_chain_idx = 0;
  tea->tea_op_count = 0;
  tea->tea_ops_fetched = 0;
  tea->tea_op_counter = 0x8000000000000000ULL;  /* Large value to avoid Main op_num collision */
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

  /* Track all trigger attempts */
  STAT_EVENT(proc_id, TEA_TRIGGER_ATTEMPTS);

  /* Don't trigger if already active */
  if (tea->state != TEA_IDLE) {
    STAT_EVENT(proc_id, TEA_TRIGGER_SKIP_ACTIVE);
    _DEBUG(0, DEBUG_TEA, "TEA trigger skipped: already active (state=%d) for PC=0x%llx\n",
           tea->state, (unsigned long long)h2p_pc);
    return;
  }

  /* Check if we have a dependency chain for this H2P branch */
  Dependency_Chain_Cache_Entry* chain = get_dependency_chain(proc_id, h2p_pc);
  if (!chain || !chain->is_valid || chain->chain_length == 0) {
    STAT_EVENT(proc_id, TEA_TRIGGER_SKIP_NO_CHAIN);
    _DEBUG(0, DEBUG_TEA, "TEA trigger skipped: no chain for PC=0x%llx (chain=%p, valid=%d, len=%d)\n",
           (unsigned long long)h2p_pc, (void*)chain,
           chain ? chain->is_valid : 0, chain ? chain->chain_length : 0);
    return;  /* No chain available */
  }

  /* Activate TEA thread */
  tea->state = TEA_FETCHING;
  tea->target_h2p_pc = h2p_pc;
  tea->target_h2p_op_num = h2p_op_num;
  tea->main_h2p_op = h2p_op;               /* Save pointer to Main H2P op for recovery identity */
  tea->h2p_oracle_info = h2p_op->oracle_info;      /* Save oracle from main H2P op */
  tea->h2p_recovery_info = h2p_op->recovery_info;  /* Save recovery info for BP checkpoint */
  tea->current_block_pc = h2p_pc;
  tea->current_chain_idx = 0;
  tea->tea_op_count = 0;
  tea->tea_ops_fetched = 0;
  tea->tea_start_cycle = cycle_count;

  /* Initialize Shadow RAT with current Main RAT state (논문 IV-D 요구사항) */
  shadow_rat_snapshot(proc_id);

  /* Reset TEA fetch stage for new trigger */
  reset_tea_fetch_stage(proc_id);

  tea->stat_tea_triggers++;
  STAT_EVENT(proc_id, TEA_TRIGGERS);

  _DEBUG(0, DEBUG_TEA, "TEA TRIGGERED: PC=0x%llx, op_num=%llu, chain_len=%d, cycle=%llu\n",
         (unsigned long long)h2p_pc, (unsigned long long)h2p_op_num,
         chain->chain_length, (unsigned long long)cycle_count);
}

void terminate_tea_thread(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);

  Tea_Thread* tea = tea_threads[proc_id];

  /* Record statistics before terminating */
  tea->stat_tea_ops_executed += tea->tea_ops_fetched;

  /* CRITICAL: Flush all orphan TEA ops from all stages */
  /* Order matters: recover stages first (they hold ops not yet dispatched),
   * then flush node stage (ops already dispatched) */
  recover_tea_fetch_stage(proc_id);
  recover_tea_rename_stage(proc_id);
  flush_tea_ops_from_node_stage(proc_id);

  /* Phase 4: Reset TEA preg pool to reclaim physical registers */
  reset_tea_preg_pool(proc_id);

  /* Phase 4.1: Clear TEA store buffer on termination */
  reset_tea_store_buffer(proc_id);

  /* Reset state */
  tea->state = TEA_IDLE;
  tea->target_h2p_pc = 0;
  tea->target_h2p_op_num = 0;
  tea->main_h2p_op = NULL;    /* Clear pointer to avoid dangling reference */
  tea->current_block_pc = 0;
  tea->current_chain_idx = 0;
  tea->tea_op_count = 0;
  tea->tea_ops_fetched = 0;
}

/**************************************************************************************/
/* State Queries */

Flag tea_is_active(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_threads || !tea_threads[proc_id]) {
    return FALSE;
  }

  return tea_threads[proc_id]->state != TEA_IDLE;
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

  if (tea->state == TEA_IDLE) {
    return;
  }

  /* State machine for TEA thread */
  switch (tea->state) {
    case TEA_FETCHING:
      /* Transition to EXECUTING only when fetch is complete */
      /* BUG FIX: Previous logic transitioned on first op (tea_op_count > 0),
       * which caused fetch stage to stop prematurely for multi-op chains */
      if (tea_fetch_stages && tea_fetch_stages[proc_id] &&
          tea_fetch_stages[proc_id]->fetch_complete) {
        tea->state = TEA_EXECUTING;
      }
      break;

    case TEA_EXECUTING:
      /* Check if all TEA ops have completed */
      if (tea->tea_op_count == 0 && tea->tea_ops_fetched > 0) {
        terminate_tea_thread(proc_id);
      }
      break;

    case TEA_IDLE:
    default:
      break;
  }
}

/**************************************************************************************/
/* Op Management */

void tea_op_completed(uns proc_id, Op* op) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, op && op->thread_id == 1);

  /* Mark op as OS_DONE so node_retire_tea_ops() can safely free it.
   * tea_op_count is NOT decremented here.  It is decremented in
   * node_retire_tea_ops() at the moment the op is physically removed from
   * the Node Table.  This is the only point where ALL op types — including
   * 0-latency non-mem ops (done_cycle == issue_cycle, so OP_DONE fires before
   * exec_stage_clear_fu can run) — are guaranteed to decrement exactly once. */
  op->state = OS_DONE;

  STAT_EVENT(proc_id, TEA_OPS_EXECUTED);
}

void tea_op_flushed(uns proc_id, Op* op) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, op && op->thread_id == 1);

  Tea_Thread* tea = tea_threads[proc_id];

  if (tea->tea_op_count > 0) {
    tea->tea_op_count--;
  }
}
