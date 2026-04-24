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
 * File         : tea/tea_fetch_stage.c
 * Author       : TEA Implementation
 * Date         : 2025
 * Description  : TEA Fetch Stage - multi-chain fetch with round-robin chain switching
 ***************************************************************************************/

#include "tea/tea_fetch_stage.h"
#include "tea/tea_thread.h"
#include "tea/tea_rename.h"
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "core.param.h"
#include "op_pool.h"
#include "op.h"
#include "statistics.h"

#include <stdlib.h>
#include <string.h>

/**************************************************************************************/
/* Global Variables */

Tea_Fetch_Stage** tea_fetch_stages = NULL;

/**************************************************************************************/
/* Local Prototypes */

static void tea_fetch_stage_init_stage_data(Tea_Fetch_Stage* tea_fetch);
static int  find_next_fetching_chain(uns proc_id);

/**************************************************************************************/
/* Initialization and Reset */

void init_tea_fetch_stage(uns proc_id) {
  if (!tea_fetch_stages) {
    tea_fetch_stages = (Tea_Fetch_Stage**)calloc(NUM_CORES, sizeof(Tea_Fetch_Stage*));
    ASSERT(0, tea_fetch_stages);
  }

  ASSERT(proc_id, proc_id < NUM_CORES);

  tea_fetch_stages[proc_id] = (Tea_Fetch_Stage*)calloc(1, sizeof(Tea_Fetch_Stage));
  ASSERT(proc_id, tea_fetch_stages[proc_id]);

  Tea_Fetch_Stage* tea_fetch = tea_fetch_stages[proc_id];
  tea_fetch->proc_id = proc_id;

  tea_fetch_stage_init_stage_data(tea_fetch);

  reset_tea_fetch_stage(proc_id);
}

static void tea_fetch_stage_init_stage_data(Tea_Fetch_Stage* tea_fetch) {
  Stage_Data* sd = &tea_fetch->sd;

  sd->name = "TEA_FETCH";
  sd->max_op_count = TEA_FETCH_WIDTH;
  sd->op_count = 0;
  sd->ops = (Op**)calloc(TEA_FETCH_WIDTH, sizeof(Op*));
  ASSERT(tea_fetch->proc_id, sd->ops);
}

void reset_tea_fetch_stage(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_fetch_stages && tea_fetch_stages[proc_id]);

  Tea_Fetch_Stage* tea_fetch = tea_fetch_stages[proc_id];

  for (int i = 0; i < TEA_FETCH_WIDTH; i++) {
    if (tea_fetch->sd.ops[i]) {
      free_op(tea_fetch->sd.ops[i]);
      tea_fetch->sd.ops[i] = NULL;
    }
  }
  tea_fetch->sd.op_count = 0;

  tea_fetch->active_chain = NULL;
  tea_fetch->current_chain_idx = 0;
  tea_fetch->total_chain_length = 0;
  tea_fetch->fetch_complete = FALSE;
  tea_fetch->ops_fetched_this_cycle = 0;
  tea_fetch->current_chain_id = 0;
}

/**************************************************************************************/
/* Helpers */

/* Returns index of next CHAIN_FETCHING chain after current_fetch_chain, or -1 */
static int find_next_fetching_chain(uns proc_id) {
  Tea_Thread* tea = tea_threads[proc_id];
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    int next = (int)((tea->current_fetch_chain + 1 + i) % MAX_TEA_CHAINS);
    if (tea->chains[next].state == CHAIN_FETCHING) {
      return next;
    }
  }
  return -1;
}

/* Set up fetch stage to fetch from the given chain */
void setup_fetch_for_chain(uns proc_id, int chain_id,
                            Dependency_Chain_Cache_Entry* dep_chain) {
  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  tf->active_chain = dep_chain;
  tf->current_chain_idx = 0;
  tf->total_chain_length = dep_chain->chain_length;
  tf->fetch_complete = FALSE;
  tf->current_chain_id = (uns)chain_id;
}

/**************************************************************************************/
/* Per-Cycle Update */

void update_tea_fetch_stage(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_fetch_stages || !tea_fetch_stages[proc_id]) {
    return;
  }

  STAT_EVENT(proc_id, TEA_FETCH_CALLED);

  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  Tea_Thread* tea = tea_threads[proc_id];

  if (!tea || tea->num_active_chains == 0) {
    STAT_EVENT(proc_id, TEA_FETCH_SKIP_STATE);
    return;
  }

  /* Handle chain switching when current chain fetch is complete */
  if (tf->fetch_complete) {
    /* Transition current chain to EXECUTING */
    int cur = (int)tf->current_chain_id;
    if (tea->chains[cur].state == CHAIN_FETCHING) {
      tea->chains[cur].state = CHAIN_EXECUTING;
    }

    /* Find the next CHAIN_FETCHING chain */
    int next = find_next_fetching_chain(proc_id);
    if (next >= 0) {
      tea->current_fetch_chain = (uns)next;
      Dependency_Chain_Cache_Entry* dep_chain =
        get_dependency_chain(proc_id, tea->chains[next].target_h2p_pc);
      if (dep_chain && dep_chain->is_valid) {
        /* Each chain must start from a fresh main-thread arch state.
         * Without this, chains inherit stale Shadow RAT entries from
         * prior chains whose pregs are already freed, causing ops to
         * wait for producers that will never write (stuck in RS). */
        shadow_rat_snapshot(proc_id);
        setup_fetch_for_chain(proc_id, next, dep_chain);
        STAT_EVENT(proc_id, TEA_FETCH_CHAIN_HIT);
      } else {
        /* Dep chain gone (reset) — terminate that chain */
        STAT_EVENT(proc_id, TEA_FETCH_CHAIN_MISS);
        terminate_tea_chain(proc_id, next);
      }
    }
    /* No next fetching chain: fetch stage is idle until a new trigger */
    return;
  }

  /* Initialise active_chain on first call after trigger */
  if (!tf->active_chain) {
    int cur = (int)tf->current_chain_id;
    /* Guard: current chain may have been terminated by recover_tea_on_flush()
     * while fetch stage was idle (active_chain cleared by recover_tea_fetch_stage_by_chain).
     * Find the next CHAIN_FETCHING chain instead of using the stale index. */
    if (cur >= MAX_TEA_CHAINS || tea->chains[cur].state != CHAIN_FETCHING) {
      int next = find_next_fetching_chain(proc_id);
      if (next < 0) return;  /* No more chains waiting to fetch */
      tea->current_fetch_chain = (uns)next;
      tf->current_chain_id    = (uns)next;
      cur = next;
    }
    tf->active_chain = get_dependency_chain(proc_id, tea->chains[cur].target_h2p_pc);
    if (!tf->active_chain || !tf->active_chain->is_valid) {
      STAT_EVENT(proc_id, TEA_FETCH_CHAIN_MISS);
      terminate_tea_thread(proc_id);
      return;
    }
    /* Same reason as above: take fresh snapshot so this chain starts
     * from current main-thread arch state, not stale prior-chain state. */
    shadow_rat_snapshot(proc_id);
    STAT_EVENT(proc_id, TEA_FETCH_CHAIN_HIT);
    tf->total_chain_length = tf->active_chain->chain_length;
    tf->current_chain_idx = 0;
  }

  /* Backpressure: stall if rename hasn't consumed last cycle's output */
  if (tf->sd.op_count > 0) {
    return;
  }

  tf->sd.op_count = 0;
  tf->ops_fetched_this_cycle = 0;

  STAT_EVENT(proc_id, TEA_FETCH_LOOP_ENTERED);

  /* Fetch up to TEA_FETCH_WIDTH ops per cycle */
  while (tf->ops_fetched_this_cycle < TEA_FETCH_WIDTH &&
         tf->current_chain_idx < tf->total_chain_length) {

    Op* cached_op = &tf->active_chain->chain[tf->current_chain_idx];
    Flag is_h2p_branch = (tf->current_chain_idx == tf->total_chain_length - 1);

    Op* tea_op = tea_create_op_from_cache(proc_id, cached_op, is_h2p_branch);
    if (!tea_op) {
      break;
    }

    /* Tag with chain ID (1-based) */
    tea_op->h2p_chain_id = (uns8)(tf->current_chain_id + 1);

    tf->sd.ops[tf->sd.op_count++] = tea_op;
    tf->current_chain_idx++;
    tf->ops_fetched_this_cycle++;
    tea->chains[tf->current_chain_id].tea_ops_fetched++;
    STAT_EVENT(proc_id, TEA_OPS_FETCHED);
  }

  if (tf->current_chain_idx >= tf->total_chain_length) {
    tf->fetch_complete = TRUE;
  }
}

/**************************************************************************************/
/* Op Creation from Block Cache */

Op* tea_create_op_from_cache(uns proc_id, Op* cached_op, Flag is_h2p_branch) {
  ASSERT(proc_id, cached_op);

  Tea_Thread* tea = tea_threads[proc_id];
  ASSERT(proc_id, tea);

  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  Tea_H2P_Chain* c = &tea->chains[tf->current_chain_id];

  Op* tea_op = alloc_op(proc_id);
  if (!tea_op) {
    return NULL;
  }

  tea_op->inst_info = cached_op->inst_info;
  tea_op->table_info = cached_op->table_info;

  tea_op->proc_id = proc_id;
  tea_op->thread_id = 1;
  tea_op->fetch_cycle = cycle_count;
  tea_op->off_path = FALSE;
  tea_op->state = OS_FETCHED;
  tea_op->op_num = tea->tea_op_counter++;

  if (is_h2p_branch) {
    /* Use chain-specific oracle/recovery info */
    tea_op->oracle_info = c->h2p_oracle_info;
    tea_op->recovery_info = c->h2p_recovery_info;
  }

  tea_op->unique_num = unique_count++;
  tea_op->unique_num_per_proc = unique_count_per_core[proc_id]++;

  /* Increment per-chain op count (h2p_chain_id set by caller after this returns) */
  c->tea_op_count++;

  return tea_op;
}

/**************************************************************************************/
/* Recovery */

void recover_tea_fetch_stage(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_fetch_stages || !tea_fetch_stages[proc_id]) {
    return;
  }

  Tea_Fetch_Stage* tea_fetch = tea_fetch_stages[proc_id];

  for (int i = 0; i < tea_fetch->sd.op_count; i++) {
    if (tea_fetch->sd.ops[i]) {
      free_op(tea_fetch->sd.ops[i]);
      tea_fetch->sd.ops[i] = NULL;
    }
  }

  reset_tea_fetch_stage(proc_id);
}

/* recover_tea_fetch_stage_by_chain: Free only the ops from a specific chain
 * in the fetch stage's pending buffer. */
void recover_tea_fetch_stage_by_chain(uns proc_id, uns8 h2p_chain_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_fetch_stages || !tea_fetch_stages[proc_id]) {
    return;
  }

  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];

  for (int i = 0; i < TEA_FETCH_WIDTH; i++) {
    Op* op = tf->sd.ops[i];
    if (op && op->h2p_chain_id == h2p_chain_id) {
      free_op(op);
      tf->sd.ops[i] = NULL;
      if (tf->sd.op_count > 0) tf->sd.op_count--;
    }
  }

  /* Reset fetch traversal state so the next chain starts fresh */
  tf->active_chain = NULL;
  tf->current_chain_idx = 0;
  tf->total_chain_length = 0;
  tf->fetch_complete = FALSE;
}
