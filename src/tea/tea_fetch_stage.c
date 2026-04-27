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
 * Description  : TEA Fetch Stage — multi-H2P chain fetch with sequential switching
 ***************************************************************************************/

#include "tea/tea_fetch_stage.h"
#include "tea/tea_thread.h"
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
static void setup_fetch_for_chain(uns proc_id, int slot);

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

  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];

  for (int i = 0; i < TEA_FETCH_WIDTH; i++) {
    if (tf->sd.ops[i]) {
      free_op(tf->sd.ops[i]);
      tf->sd.ops[i] = NULL;
    }
  }
  tf->sd.op_count = 0;

  tf->active_chain = NULL;
  tf->current_chain_idx = 0;
  tf->total_chain_length = 0;
  tf->fetch_complete = FALSE;
  tf->ops_fetched_this_cycle = 0;
  tf->current_chain_id = -1;
}

/* setup_fetch_for_chain: Point the fetch stage at chains[slot].
 * If the dep chain disappeared, terminate that chain immediately. */
static void setup_fetch_for_chain(uns proc_id, int slot) {
  Tea_Thread* tea = tea_threads[proc_id];
  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  Tea_H2P_Chain* c = &tea->chains[slot];

  Dependency_Chain_Cache_Entry* dep = get_dependency_chain(proc_id, c->target_h2p_pc);
  if (!dep || !dep->is_valid || dep->chain_length == 0) {
    STAT_EVENT(proc_id, TEA_FETCH_CHAIN_MISS);
    terminate_tea_chain(proc_id, slot);
    return;
  }
  STAT_EVENT(proc_id, TEA_FETCH_CHAIN_HIT);

  tf->active_chain = dep;
  tf->current_chain_idx = 0;
  tf->total_chain_length = dep->chain_length;
  tf->fetch_complete = FALSE;
  tf->current_chain_id = slot;
  tf->ops_fetched_this_cycle = 0;
}

/**************************************************************************************/
/* Per-Cycle Update */

void update_tea_fetch_stage(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_fetch_stages || !tea_fetch_stages[proc_id])
    return;

  STAT_EVENT(proc_id, TEA_FETCH_CALLED);

  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  Tea_Thread* tea = tea_threads[proc_id];

  if (!tea || tea->num_active_chains == 0) {
    STAT_EVENT(proc_id, TEA_FETCH_SKIP_STATE);
    return;
  }

  /* No chain currently being fetched */
  if (tea->current_fetch_chain < 0) {
    STAT_EVENT(proc_id, TEA_FETCH_SKIP_STATE);
    return;
  }

  int slot = tea->current_fetch_chain;
  Tea_H2P_Chain* chain = &tea->chains[slot];

  /* Sanity: current_fetch_chain must be CHAIN_FETCHING */
  if (chain->state != CHAIN_FETCHING) {
    STAT_EVENT(proc_id, TEA_FETCH_SKIP_STATE);
    return;
  }

  /* First entry: fetch stage might not have dep chain yet */
  if (!tf->active_chain || tf->current_chain_id != slot) {
    setup_fetch_for_chain(proc_id, slot);
    if (tea->current_fetch_chain < 0)
      return;  /* chain was terminated in setup */
  }

  /* Fetch complete for current chain → transition + switch to next */
  if (tf->fetch_complete) {
    chain->state = CHAIN_EXECUTING;

    /* Find next CHAIN_FETCHING chain (skip current) */
    int next = -1;
    for (int i = 1; i <= MAX_TEA_CHAINS; i++) {
      int idx = (slot + i) % MAX_TEA_CHAINS;
      if (tea->chains[idx].state == CHAIN_FETCHING) {
        next = idx;
        break;
      }
    }

    tea->current_fetch_chain = next;
    if (next >= 0) {
      setup_fetch_for_chain(proc_id, next);
    }
    /* Whether or not there's a next chain, stop for this cycle */
    return;
  }

  /* Backpressure: rename stage hasn't consumed last batch */
  if (tf->sd.op_count > 0)
    return;

  tf->sd.op_count = 0;
  tf->ops_fetched_this_cycle = 0;

  STAT_EVENT(proc_id, TEA_FETCH_LOOP_ENTERED);

  /* Fetch up to TEA_FETCH_WIDTH ops this cycle */
  while (tf->ops_fetched_this_cycle < TEA_FETCH_WIDTH &&
         tf->current_chain_idx < tf->total_chain_length) {

    Op* cached_op = &tf->active_chain->chain[tf->current_chain_idx];
    Flag is_h2p = (tf->current_chain_idx == tf->total_chain_length - 1);

    Op* tea_op = tea_create_op_from_cache(proc_id, cached_op, is_h2p);
    if (!tea_op)
      break;  /* Op pool exhausted — retry next cycle */

    tf->sd.ops[tf->sd.op_count++] = tea_op;
    tf->current_chain_idx++;
    tf->ops_fetched_this_cycle++;
  }

  if (tf->current_chain_idx >= tf->total_chain_length)
    tf->fetch_complete = TRUE;
}

/**************************************************************************************/
/* Op Creation from Block Cache */

Op* tea_create_op_from_cache(uns proc_id, Op* cached_op, Flag is_h2p_branch) {
  ASSERT(proc_id, cached_op);

  Tea_Thread* tea = tea_threads[proc_id];
  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  ASSERT(proc_id, tea && tf);
  ASSERT(proc_id, tf->current_chain_id >= 0 && tf->current_chain_id < MAX_TEA_CHAINS);

  Tea_H2P_Chain* c = &tea->chains[tf->current_chain_id];

  Op* tea_op = alloc_op(proc_id);
  if (!tea_op)
    return NULL;

  tea_op->inst_info = cached_op->inst_info;
  tea_op->table_info = cached_op->table_info;

  tea_op->proc_id = proc_id;
  tea_op->thread_id = 1;
  tea_op->h2p_chain_id = (uns8)(tf->current_chain_id + 1);  /* 1-based */
  tea_op->fetch_cycle = cycle_count;
  tea_op->off_path = FALSE;
  tea_op->state = OS_FETCHED;
  tea_op->op_num = tea->tea_op_counter++;

  if (is_h2p_branch) {
    tea_op->oracle_info = c->h2p_oracle_info;
    tea_op->recovery_info = c->h2p_recovery_info;
  }

  tea_op->unique_num = unique_count++;
  tea_op->unique_num_per_proc = unique_count_per_core[proc_id]++;

  c->tea_op_count++;
  c->tea_ops_fetched++;
  STAT_EVENT(proc_id, TEA_OPS_FETCHED);

  return tea_op;
}

/**************************************************************************************/
/* Recovery */

void recover_tea_fetch_stage(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_fetch_stages || !tea_fetch_stages[proc_id])
    return;

  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];

  for (int i = 0; i < tf->sd.op_count; i++) {
    if (tf->sd.ops[i]) {
      free_op(tf->sd.ops[i]);
      tf->sd.ops[i] = NULL;
    }
  }

  reset_tea_fetch_stage(proc_id);
}

/* recover_tea_fetch_stage_by_chain: Free only ops from the specified chain
 * in the fetch stage's staging buffer.  Called by terminate_tea_chain(). */
void recover_tea_fetch_stage_by_chain(uns proc_id, uns8 chain_id) {
  if (!tea_fetch_stages || !tea_fetch_stages[proc_id])
    return;

  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];

  for (int i = 0; i < TEA_FETCH_WIDTH; i++) {
    Op* op = tf->sd.ops[i];
    if (op && op->h2p_chain_id == chain_id) {
      free_op(op);
      tf->sd.ops[i] = NULL;
      if (tf->sd.op_count > 0)
        tf->sd.op_count--;
    }
  }

  /* If this was the active chain, reset traversal state */
  if (tf->current_chain_id == (int)(chain_id - 1)) {
    tf->active_chain = NULL;
    tf->current_chain_idx = 0;
    tf->total_chain_length = 0;
    tf->fetch_complete = FALSE;
    tf->current_chain_id = -1;
  }
}
