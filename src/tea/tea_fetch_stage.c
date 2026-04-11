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
 * Description  : TEA Fetch Stage - fetches dependency chain ops from Block Cache
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

/**************************************************************************************/
/* Initialization and Reset */

void init_tea_fetch_stage(uns proc_id) {
  /* Allocate per-core array on first call */
  if (!tea_fetch_stages) {
    tea_fetch_stages = (Tea_Fetch_Stage**)calloc(NUM_CORES, sizeof(Tea_Fetch_Stage*));
    ASSERT(0, tea_fetch_stages);
  }

  ASSERT(proc_id, proc_id < NUM_CORES);

  /* Allocate TEA fetch stage for this core */
  tea_fetch_stages[proc_id] = (Tea_Fetch_Stage*)calloc(1, sizeof(Tea_Fetch_Stage));
  ASSERT(proc_id, tea_fetch_stages[proc_id]);

  Tea_Fetch_Stage* tea_fetch = tea_fetch_stages[proc_id];
  tea_fetch->proc_id = proc_id;

  /* Initialize Stage_Data */
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

  /* Free and clear stage data - CRITICAL: must free_op to avoid op pool exhaustion */
  for (int i = 0; i < TEA_FETCH_WIDTH; i++) {
    if (tea_fetch->sd.ops[i]) {
      free_op(tea_fetch->sd.ops[i]);
      tea_fetch->sd.ops[i] = NULL;
    }
  }
  tea_fetch->sd.op_count = 0;

  /* Reset state */
  tea_fetch->active_chain = NULL;
  tea_fetch->current_chain_idx = 0;
  tea_fetch->total_chain_length = 0;
  tea_fetch->fetch_complete = FALSE;
  tea_fetch->ops_fetched_this_cycle = 0;
}

/**************************************************************************************/
/* Per-Cycle Update */

void update_tea_fetch_stage(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_fetch_stages || !tea_fetch_stages[proc_id]) {
    return;
  }

  STAT_EVENT(proc_id, TEA_FETCH_CALLED);

  Tea_Fetch_Stage* tea_fetch = tea_fetch_stages[proc_id];
  Tea_Thread* tea = tea_threads[proc_id];

  /* Only fetch when TEA is in FETCHING state */
  if (!tea || tea->state != TEA_FETCHING) {
    STAT_EVENT(proc_id, TEA_FETCH_SKIP_STATE);
    return;
  }

  /* Get the dependency chain from Block Cache if not already set */
  if (!tea_fetch->active_chain) {
    tea_fetch->active_chain = get_dependency_chain(proc_id, tea->target_h2p_pc);
    if (!tea_fetch->active_chain || !tea_fetch->active_chain->is_valid) {
      /* No valid chain, terminate TEA */
      STAT_EVENT(proc_id, TEA_FETCH_CHAIN_MISS);
      terminate_tea_thread(proc_id);
      return;
    }
    STAT_EVENT(proc_id, TEA_FETCH_CHAIN_HIT);
    tea_fetch->total_chain_length = tea_fetch->active_chain->chain_length;
    tea_fetch->current_chain_idx = 0;
  }

  /* Check if fetch is already complete */
  if (tea_fetch->fetch_complete) {
    return;
  }

  /* Backpressure: if Rename Stage didn't consume last cycle's fetch output, stall.
   * Prevents advancing chain index or calling alloc_op() while ops are stuck. */
  if (tea_fetch->sd.op_count > 0) {
    return;
  }

  /* Safe to clear — all previous ops were consumed by Rename Stage */
  tea_fetch->sd.op_count = 0;
  tea_fetch->ops_fetched_this_cycle = 0;

  STAT_EVENT(proc_id, TEA_FETCH_LOOP_ENTERED);

  /* Fetch up to TEA_FETCH_WIDTH ops per cycle */
  while (tea_fetch->ops_fetched_this_cycle < TEA_FETCH_WIDTH &&
         tea_fetch->current_chain_idx < tea_fetch->total_chain_length) {

    Op* cached_op = &tea_fetch->active_chain->chain[tea_fetch->current_chain_idx];

    /* Check if this is the H2P branch (last op in chain) */
    Flag is_h2p_branch = (tea_fetch->current_chain_idx == tea_fetch->total_chain_length - 1);

    /* Create new TEA op from cached static info */
    Op* tea_op = tea_create_op_from_cache(proc_id, cached_op, is_h2p_branch);
    if (!tea_op) {
      /* Op pool exhausted, try again next cycle */
      break;
    }

    /* Add to stage data */
    tea_fetch->sd.ops[tea_fetch->sd.op_count++] = tea_op;
    tea_fetch->current_chain_idx++;
    tea_fetch->ops_fetched_this_cycle++;
    tea->tea_ops_fetched++;
    STAT_EVENT(proc_id, TEA_OPS_FETCHED);
  }

  /* Check if fetch is complete */
  if (tea_fetch->current_chain_idx >= tea_fetch->total_chain_length) {
    tea_fetch->fetch_complete = TRUE;
  }
}

/**************************************************************************************/
/* Op Creation from Block Cache */

Op* tea_create_op_from_cache(uns proc_id, Op* cached_op, Flag is_h2p_branch) {
  ASSERT(proc_id, cached_op);

  Tea_Thread* tea = tea_threads[proc_id];
  ASSERT(proc_id, tea);

  /* Allocate new op from op pool */
  Op* tea_op = alloc_op(proc_id);
  if (!tea_op) {
    return NULL;
  }

  /* Copy static information from cached op */
  /* NOTE: Do NOT copy oracle_info from cache - it's stale (from past execution) */
  tea_op->inst_info = cached_op->inst_info;
  tea_op->table_info = cached_op->table_info;

  /* Set TEA-specific dynamic information */
  tea_op->proc_id = proc_id;
  tea_op->thread_id = 1;  /* TEA thread identifier */
  tea_op->fetch_cycle = cycle_count;
  tea_op->off_path = FALSE;  /* TEA ops are always on-path for TEA thread */
  tea_op->state = OS_FETCHED;

  /* Critical: op_num, oracle_info, and recovery_info assignment */
  if (is_h2p_branch) {
    /* H2P branch now uses the same TEA op counter as other TEA ops.
     * Identity link to Main H2P is maintained explicitly via tea->main_h2p_op pointer.
     * oracle_info and recovery_info from main H2P still needed for mispred detection. */
    tea_op->op_num = tea->tea_op_counter++;
    /* Use oracle_info saved at TEA trigger time (from main H2P op) */
    tea_op->oracle_info = tea->h2p_oracle_info;
    /* Use recovery_info from main H2P op (critical for BP checkpoint restore) */
    tea_op->recovery_info = tea->h2p_recovery_info;
  } else {
    /* Other dependency chain ops use TEA-specific counter */
    /* No oracle_info/recovery_info needed - only H2P branch needs mispred detection */
    tea_op->op_num = tea->tea_op_counter++;
  }

  /* Assign unique numbers */
  tea_op->unique_num = unique_count++;
  tea_op->unique_num_per_proc = unique_count_per_core[proc_id]++;

  /* Increment TEA op count for tracking */
  tea->tea_op_count++;

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

  /* Free any ops in stage data */
  for (int i = 0; i < tea_fetch->sd.op_count; i++) {
    if (tea_fetch->sd.ops[i]) {
      free_op(tea_fetch->sd.ops[i]);
      tea_fetch->sd.ops[i] = NULL;
    }
  }

  /* Reset the stage */
  reset_tea_fetch_stage(proc_id);
}
