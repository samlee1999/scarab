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
#include "memory/memory.param.h"
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

#define TEA_RECENT_LOAD_TABLE_SIZE 4096
#define TEA_H2P_PC_TRACKER_SIZE    4096

typedef struct Tea_Recent_Load_Access_struct {
  Flag    valid;
  Addr    line_addr;
  Addr    pc;
  Counter op_num;
  Counter access_cycle;
} Tea_Recent_Load_Access;

typedef struct Tea_H2P_PC_Tracker_Entry_struct {
  Flag valid;
  Addr pc;
} Tea_H2P_PC_Tracker_Entry;

typedef enum Tea_Main_Load_Instance_Class_enum {
  TEA_MAIN_LOAD_INSTANCE_UNKNOWN,
  TEA_MAIN_LOAD_INSTANCE_CURRENT,
  TEA_MAIN_LOAD_INSTANCE_OLDER,
  TEA_MAIN_LOAD_INSTANCE_NEWER,
} Tea_Main_Load_Instance_Class;

static Tea_Recent_Load_Access tea_recent_main_issue[MAX_NUM_PROCS][TEA_RECENT_LOAD_TABLE_SIZE];
static Tea_Recent_Load_Access tea_recent_tea_issue[MAX_NUM_PROCS][TEA_RECENT_LOAD_TABLE_SIZE];
static Tea_Recent_Load_Access tea_recent_main_cache[MAX_NUM_PROCS][TEA_RECENT_LOAD_TABLE_SIZE];
static Tea_Recent_Load_Access tea_recent_tea_cache[MAX_NUM_PROCS][TEA_RECENT_LOAD_TABLE_SIZE];
static Tea_H2P_PC_Tracker_Entry tea_h2p_pc_tracker[MAX_NUM_PROCS][TEA_H2P_PC_TRACKER_SIZE];

static inline Addr tea_line_addr_from_va(Addr va) {
  return va & ~((Addr)DCACHE_LINE_SIZE - 1);
}

static inline uns tea_recent_load_index(Addr line_addr) {
  Addr line = line_addr >> LOG2(DCACHE_LINE_SIZE);
  return (uns)((line ^ (line >> 12) ^ (line >> 24)) &
               (TEA_RECENT_LOAD_TABLE_SIZE - 1));
}

static inline uns tea_h2p_pc_tracker_index(Addr pc) {
  return (uns)(((pc >> 2) ^ (pc >> 11) ^ (pc >> 19)) &
               (TEA_H2P_PC_TRACKER_SIZE - 1));
}

static void clear_tea_tracking_tables(uns proc_id) {
  if (proc_id >= MAX_NUM_PROCS)
    return;

  memset(tea_recent_main_issue[proc_id], 0,
         sizeof(tea_recent_main_issue[proc_id]));
  memset(tea_recent_tea_issue[proc_id], 0,
         sizeof(tea_recent_tea_issue[proc_id]));
  memset(tea_recent_main_cache[proc_id], 0,
         sizeof(tea_recent_main_cache[proc_id]));
  memset(tea_recent_tea_cache[proc_id], 0,
         sizeof(tea_recent_tea_cache[proc_id]));
  memset(tea_h2p_pc_tracker[proc_id], 0,
         sizeof(tea_h2p_pc_tracker[proc_id]));
}

static void record_h2p_pc_reuse(uns proc_id, Addr h2p_pc) {
  if (proc_id >= MAX_NUM_PROCS)
    return;

  uns idx = tea_h2p_pc_tracker_index(h2p_pc);
  Tea_H2P_PC_Tracker_Entry* entry = &tea_h2p_pc_tracker[proc_id][idx];

  if (!entry->valid) {
    entry->valid = TRUE;
    entry->pc = h2p_pc;
    STAT_EVENT(proc_id, TEA_TRIGGER_H2P_PC_UNIQUE);
  } else if (entry->pc == h2p_pc) {
    STAT_EVENT(proc_id, TEA_TRIGGER_H2P_PC_REUSE);
  } else {
    entry->pc = h2p_pc;
    STAT_EVENT(proc_id, TEA_TRIGGER_H2P_PC_TRACKER_COLLISION);
    STAT_EVENT(proc_id, TEA_TRIGGER_H2P_PC_UNIQUE);
  }
}

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
  INC_STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_AVG, length);
  if      (length <= 8)  STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_1_8);
  else if (length <= 16) STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_9_16);
  else if (length <= 32) STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_17_32);
  else if (length <= 64) STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_33_64);
  else                   STAT_EVENT(proc_id, TEA_CHAIN_LENGTH_65_PLUS);
}

static Tea_Case1_Main_Stage classify_main_h2p_stage(Op* main_h2p) {
  if (!main_h2p)
    return TEA_CASE1_MAIN_STAGE_UNKNOWN;

  if (main_h2p->decode_cycle == 0)
    return TEA_CASE1_MAIN_STAGE_PRE_DECODE;

  if (main_h2p->map_cycle == MAX_CTR)
    return TEA_CASE1_MAIN_STAGE_DECODED_PRE_RENAME;

  if (main_h2p->issue_cycle == MAX_CTR)
    return TEA_CASE1_MAIN_STAGE_IN_RENAME;

  if (main_h2p->exec_cycle == MAX_CTR)
    return TEA_CASE1_MAIN_STAGE_IN_NODE_OR_RS;

  if (cycle_count < main_h2p->exec_cycle)
    return TEA_CASE1_MAIN_STAGE_SCHEDULED_OR_EXECUTING;

  return TEA_CASE1_MAIN_STAGE_DONE_OR_LATER;
}

static void record_trigger_main_stage(uns proc_id, Op* main_h2p) {
  Tea_Case1_Main_Stage stage = classify_main_h2p_stage(main_h2p);

  STAT_EVENT(proc_id, TEA_TRIGGER_MAIN_STAGE_SAMPLES);
  switch (stage) {
    case TEA_CASE1_MAIN_STAGE_PRE_DECODE:
      STAT_EVENT(proc_id, TEA_TRIGGER_MAIN_STAGE_PRE_DECODE);
      break;
    case TEA_CASE1_MAIN_STAGE_DECODED_PRE_RENAME:
      STAT_EVENT(proc_id, TEA_TRIGGER_MAIN_STAGE_DECODED_PRE_RENAME);
      break;
    case TEA_CASE1_MAIN_STAGE_IN_RENAME:
      STAT_EVENT(proc_id, TEA_TRIGGER_MAIN_STAGE_IN_RENAME);
      break;
    case TEA_CASE1_MAIN_STAGE_IN_NODE_OR_RS:
      STAT_EVENT(proc_id, TEA_TRIGGER_MAIN_STAGE_IN_NODE_OR_RS);
      break;
    case TEA_CASE1_MAIN_STAGE_SCHEDULED_OR_EXECUTING:
      STAT_EVENT(proc_id, TEA_TRIGGER_MAIN_STAGE_SCHEDULED_OR_EXECUTING);
      break;
    case TEA_CASE1_MAIN_STAGE_DONE_OR_LATER:
      STAT_EVENT(proc_id, TEA_TRIGGER_MAIN_STAGE_DONE_OR_LATER);
      break;
    case TEA_CASE1_MAIN_STAGE_UNKNOWN:
    default:
      STAT_EVENT(proc_id, TEA_TRIGGER_MAIN_STAGE_UNKNOWN);
      break;
  }
}

static void record_chain_load_count_bucket(uns proc_id, uns load_ops) {
  INC_STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_TOTAL, load_ops);
  INC_STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_AVG, load_ops);

  if      (load_ops == 0)  STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_0);
  else if (load_ops == 1)  STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_1);
  else if (load_ops == 2)  STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_2);
  else if (load_ops <= 4)  STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_3_4);
  else if (load_ops <= 8)  STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_5_8);
  else if (load_ops <= 16) STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_9_16);
  else                    STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_17_PLUS);
}

static void capture_chain_load_identities(Tea_H2P_Chain* c,
                                          Dependency_Chain_Cache_Entry* chain) {
  if (!c || !chain)
    return;

  c->load_identity_count = 0;
  for (uns ci = 0; ci < chain->chain_length &&
                   c->load_identity_count < MAX_CHAIN_LENGTH; ci++) {
    Op* cached_op = &chain->chain[ci];
    if (!cached_op->table_info ||
        cached_op->table_info->mem_type != MEM_LD ||
        chain->h2p_branch_op_num < cached_op->op_num)
      continue;

    Tea_Chain_Load_Identity* id =
      &c->load_identities[c->load_identity_count++];
    id->valid = TRUE;
    id->pc = cached_op->inst_info ? cached_op->inst_info->addr : 0;
    id->line_addr = tea_line_addr_from_va(cached_op->oracle_info.va);
    id->h2p_op_delta = chain->h2p_branch_op_num - cached_op->op_num;
  }
}

static void record_dcc_age_bucket(uns proc_id, Counter age,
                                  Flag cycle_age) {
  if (cycle_age) {
    if      (age < 100)    STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_CYCLES_0_99);
    else if (age < 1000)   STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_CYCLES_100_999);
    else if (age < 10000)  STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_CYCLES_1000_9999);
    else if (age < 100000) STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_CYCLES_10000_99999);
    else                   STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_CYCLES_100000_PLUS);
  } else {
    if      (age < 100)    STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_UNIQUE_0_99);
    else if (age < 1000)   STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_UNIQUE_100_999);
    else if (age < 10000)  STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_UNIQUE_1000_9999);
    else if (age < 100000) STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_UNIQUE_10000_99999);
    else                   STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_UNIQUE_100000_PLUS);
  }
}

static void record_dcc_trigger_entry_age(uns proc_id,
                                         Dependency_Chain_Cache_Entry* chain,
                                         Op* h2p_op) {
  if (!chain || !h2p_op)
    return;

  if (cycle_count >= chain->insert_cycle) {
    Counter cycle_age = cycle_count - chain->insert_cycle;
    STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_SAMPLES);
    INC_STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_CYCLES_TOTAL, cycle_age);
    INC_STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_CYCLES_AVG, cycle_age);
    record_dcc_age_bucket(proc_id, cycle_age, TRUE);
  }

  if (h2p_op->unique_num >= chain->h2p_branch_unique_num) {
    Counter unique_age = h2p_op->unique_num - chain->h2p_branch_unique_num;
    STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_UNIQUE_SAMPLES);
    INC_STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_UNIQUE_TOTAL, unique_age);
    INC_STAT_EVENT(proc_id, DCC_TRIGGER_ENTRY_AGE_UNIQUE_AVG, unique_age);
    record_dcc_age_bucket(proc_id, unique_age, FALSE);
  }
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

  if (!c->h2p_resolved) {
    STAT_EVENT(proc_id, TEA_CHAINS_TERMINATED_BEFORE_H2P_RESOLVE);
    return;
  }

  STAT_EVENT(proc_id, TEA_H2P_RESOLVED_CHAINS);
  if (c->tea_load_dcache_miss_count > 0) {
    STAT_EVENT(proc_id, TEA_CHAINS_WITH_LOAD_MISS);
    INC_STAT_EVENT(proc_id, TEA_CHAIN_LOAD_MISSES_TOTAL,
                   c->tea_load_dcache_miss_count);
    INC_STAT_EVENT(proc_id, TEA_CHAIN_LOAD_MISS_MAX_LATENCY_TOTAL,
                   c->tea_load_miss_max_latency);
    INC_STAT_EVENT(proc_id, TEA_CHAIN_LOAD_MISS_MAX_LATENCY_AVG,
                   c->tea_load_miss_max_latency);
  } else {
    STAT_EVENT(proc_id, TEA_CHAINS_WITHOUT_LOAD_MISS);
  }
}

static void update_recent_load_entry(Tea_Recent_Load_Access* entry, Op* op,
                                     Addr line_addr) {
  entry->valid = TRUE;
  entry->line_addr = line_addr;
  entry->pc = op->inst_info ? op->inst_info->addr : 0;
  entry->op_num = op->op_num;
  entry->access_cycle = cycle_count;
}

static void record_load_order_delta(uns proc_id, Counter delta,
                                    Stat_Enum total_stat,
                                    Stat_Enum avg_stat) {
  INC_STAT_EVENT(proc_id, total_stat, delta);
  INC_STAT_EVENT(proc_id, avg_stat, delta);
}

static Flag tea_chain_trigger_cycle_for_op(Op* op, Counter* trigger_cycle) {
  if (!TEA_ENABLE || !tea_threads || !op || op->thread_id != 1 ||
      op->proc_id >= MAX_NUM_PROCS)
    return FALSE;

  int slot = (int)op->h2p_chain_id - 1;
  if (!tea_chain_slot_is_valid(op->proc_id, slot))
    return FALSE;

  Tea_H2P_Chain* c = &tea_threads[op->proc_id]->chains[slot];
  if (c->state == CHAIN_INACTIVE || c->trigger_cycle == 0)
    return FALSE;

  *trigger_cycle = c->trigger_cycle;
  return TRUE;
}

static void record_cache_hit_after_trigger_delta_bucket(uns proc_id,
                                                       Counter delta) {
  if      (delta < 10)    STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_AFTER_TRIGGER_DELTA_0_9);
  else if (delta < 100)   STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_AFTER_TRIGGER_DELTA_10_99);
  else if (delta < 1000)  STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_AFTER_TRIGGER_DELTA_100_999);
  else if (delta < 10000) STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_AFTER_TRIGGER_DELTA_1000_9999);
  else                    STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_AFTER_TRIGGER_DELTA_10000_PLUS);
}

static void record_cache_hit_before_trigger_delta_bucket(uns proc_id,
                                                        Counter delta) {
  if      (delta < 100)    STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TRIGGER_DELTA_0_99);
  else if (delta < 1000)   STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TRIGGER_DELTA_100_999);
  else if (delta < 10000)  STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TRIGGER_DELTA_1000_9999);
  else if (delta < 100000) STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TRIGGER_DELTA_10000_99999);
  else                     STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TRIGGER_DELTA_100000_PLUS);
}

static Tea_Main_Load_Instance_Class classify_main_load_instance(
  Tea_H2P_Chain* c, Tea_Recent_Load_Access* main_entry) {
  if (!c || !main_entry || !main_entry->valid)
    return TEA_MAIN_LOAD_INSTANCE_UNKNOWN;

  Flag found_identity = FALSE;
  Counter best_expected_op_num = 0;
  Counter best_distance = MAX_CTR;

  for (uns i = 0; i < c->load_identity_count; i++) {
    Tea_Chain_Load_Identity* id = &c->load_identities[i];
    if (!id->valid || id->pc != main_entry->pc ||
        id->line_addr != main_entry->line_addr ||
        c->target_h2p_op_num < id->h2p_op_delta)
      continue;

    Counter expected_op_num = c->target_h2p_op_num - id->h2p_op_delta;
    if (main_entry->op_num == expected_op_num)
      return TEA_MAIN_LOAD_INSTANCE_CURRENT;

    Counter distance = (main_entry->op_num > expected_op_num) ?
                       (main_entry->op_num - expected_op_num) :
                       (expected_op_num - main_entry->op_num);
    if (!found_identity || distance < best_distance) {
      found_identity = TRUE;
      best_distance = distance;
      best_expected_op_num = expected_op_num;
    }
  }

  if (!found_identity)
    return TEA_MAIN_LOAD_INSTANCE_UNKNOWN;

  return (main_entry->op_num < best_expected_op_num) ?
         TEA_MAIN_LOAD_INSTANCE_OLDER :
         TEA_MAIN_LOAD_INSTANCE_NEWER;
}

static void record_main_load_instance_class(uns proc_id,
                                            Tea_Main_Load_Instance_Class cls,
                                            Flag main_access_after_trigger) {
  switch (cls) {
    case TEA_MAIN_LOAD_INSTANCE_CURRENT:
      STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_CURRENT_INSTANCE);
      STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_CURRENT_INSTANCE_RATE);
      if (main_access_after_trigger)
        STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_CURRENT_INSTANCE_AFTER_TRIGGER);
      else
        STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_CURRENT_INSTANCE_BEFORE_TRIGGER);
      break;
    case TEA_MAIN_LOAD_INSTANCE_OLDER:
      STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_OLDER_INSTANCE);
      STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_OLDER_INSTANCE_RATE);
      break;
    case TEA_MAIN_LOAD_INSTANCE_NEWER:
      STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_NEWER_INSTANCE);
      STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_NEWER_INSTANCE_RATE);
      break;
    case TEA_MAIN_LOAD_INSTANCE_UNKNOWN:
    default:
      STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_INSTANCE_UNKNOWN);
      STAT_EVENT(proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_INSTANCE_UNKNOWN_RATE);
      break;
  }
}

static void record_load_order_at_point(
  Op* op, Addr line_addr,
  Tea_Recent_Load_Access main_table[MAX_NUM_PROCS][TEA_RECENT_LOAD_TABLE_SIZE],
  Tea_Recent_Load_Access tea_table[MAX_NUM_PROCS][TEA_RECENT_LOAD_TABLE_SIZE],
  Stat_Enum samples_stat,
  Stat_Enum tea_samples_stat,
  Stat_Enum main_samples_stat,
  Stat_Enum main_before_stat,
  Stat_Enum main_before_rate_stat,
  Stat_Enum main_pc_before_stat,
  Stat_Enum main_pc_before_rate_stat,
  Stat_Enum tea_before_stat,
  Stat_Enum tea_before_rate_stat,
  Stat_Enum tea_pc_before_stat,
  Stat_Enum tea_pc_before_rate_stat,
  Stat_Enum same_cycle_stat,
  Stat_Enum main_before_total_stat,
  Stat_Enum main_before_avg_stat,
  Stat_Enum tea_before_total_stat,
  Stat_Enum tea_before_avg_stat) {
  if (!TEA_ENABLE || !op || !op->table_info ||
      op->table_info->mem_type != MEM_LD ||
      op->proc_id >= MAX_NUM_PROCS)
    return;

  uns idx = tea_recent_load_index(line_addr);
  Addr pc = op->inst_info ? op->inst_info->addr : 0;
  STAT_EVENT(op->proc_id, samples_stat);

  if (op->thread_id == 1) {
    Tea_Recent_Load_Access* main_entry = &main_table[op->proc_id][idx];
    STAT_EVENT(op->proc_id, tea_samples_stat);

    if (main_entry->valid && main_entry->line_addr == line_addr &&
        main_entry->access_cycle <= cycle_count) {
      if (main_entry->access_cycle == cycle_count) {
        STAT_EVENT(op->proc_id, same_cycle_stat);
      } else {
        Counter delta = cycle_count - main_entry->access_cycle;
        STAT_EVENT(op->proc_id, main_before_stat);
        STAT_EVENT(op->proc_id, main_before_rate_stat);
        record_load_order_delta(op->proc_id, delta,
                                main_before_total_stat,
                                main_before_avg_stat);

        if (main_entry->pc == pc) {
          STAT_EVENT(op->proc_id, main_pc_before_stat);
          STAT_EVENT(op->proc_id, main_pc_before_rate_stat);
        }
      }
    }

    update_recent_load_entry(&tea_table[op->proc_id][idx], op, line_addr);
  } else {
    Tea_Recent_Load_Access* tea_entry = &tea_table[op->proc_id][idx];
    STAT_EVENT(op->proc_id, main_samples_stat);
    if (tea_entry->valid && tea_entry->line_addr == line_addr &&
        tea_entry->access_cycle <= cycle_count) {
      if (tea_entry->access_cycle == cycle_count) {
        STAT_EVENT(op->proc_id, same_cycle_stat);
      } else {
        Counter delta = cycle_count - tea_entry->access_cycle;
        STAT_EVENT(op->proc_id, tea_before_stat);
        STAT_EVENT(op->proc_id, tea_before_rate_stat);
        record_load_order_delta(op->proc_id, delta,
                                tea_before_total_stat,
                                tea_before_avg_stat);

        if (tea_entry->pc == pc) {
          STAT_EVENT(op->proc_id, tea_pc_before_stat);
          STAT_EVENT(op->proc_id, tea_pc_before_rate_stat);
        }
      }
    }

    update_recent_load_entry(&main_table[op->proc_id][idx], op, line_addr);
  }
}

void tea_record_load_issue_order(Op* op) {
  if (!op || !op->table_info || op->table_info->mem_type != MEM_LD)
    return;

  record_load_order_at_point(
    op, tea_line_addr_from_va(op->oracle_info.va),
    tea_recent_main_issue, tea_recent_tea_issue,
    TEA_LOAD_ISSUE_ORDER_SAMPLES,
    TEA_LOAD_ISSUE_TEA_LOAD_SAMPLES,
    TEA_LOAD_ISSUE_MAIN_LOAD_SAMPLES,
    TEA_LOAD_ISSUE_MAIN_LINE_BEFORE_TEA,
    TEA_LOAD_ISSUE_MAIN_LINE_BEFORE_TEA_RATE,
    TEA_LOAD_ISSUE_MAIN_PC_LINE_BEFORE_TEA,
    TEA_LOAD_ISSUE_MAIN_PC_LINE_BEFORE_TEA_RATE,
    TEA_LOAD_ISSUE_TEA_LINE_BEFORE_MAIN,
    TEA_LOAD_ISSUE_TEA_LINE_BEFORE_MAIN_RATE,
    TEA_LOAD_ISSUE_TEA_PC_LINE_BEFORE_MAIN,
    TEA_LOAD_ISSUE_TEA_PC_LINE_BEFORE_MAIN_RATE,
    TEA_LOAD_ISSUE_SAME_CYCLE,
    TEA_LOAD_ISSUE_MAIN_BEFORE_TEA_CYCLES_TOTAL,
    TEA_LOAD_ISSUE_MAIN_BEFORE_TEA_CYCLES_AVG,
    TEA_LOAD_ISSUE_TEA_BEFORE_MAIN_CYCLES_TOTAL,
    TEA_LOAD_ISSUE_TEA_BEFORE_MAIN_CYCLES_AVG);
}

void tea_record_load_cache_access_order(Op* op, Addr line_addr) {
  if (!op || !op->table_info || op->table_info->mem_type != MEM_LD)
    return;

  record_load_order_at_point(
    op, line_addr,
    tea_recent_main_cache, tea_recent_tea_cache,
    TEA_LOAD_CACHE_ORDER_SAMPLES,
    TEA_LOAD_CACHE_TEA_LOAD_SAMPLES,
    TEA_LOAD_CACHE_MAIN_LOAD_SAMPLES,
    TEA_LOAD_CACHE_MAIN_LINE_BEFORE_TEA,
    TEA_LOAD_CACHE_MAIN_LINE_BEFORE_TEA_RATE,
    TEA_LOAD_CACHE_MAIN_PC_LINE_BEFORE_TEA,
    TEA_LOAD_CACHE_MAIN_PC_LINE_BEFORE_TEA_RATE,
    TEA_LOAD_CACHE_TEA_LINE_BEFORE_MAIN,
    TEA_LOAD_CACHE_TEA_LINE_BEFORE_MAIN_RATE,
    TEA_LOAD_CACHE_TEA_PC_LINE_BEFORE_MAIN,
    TEA_LOAD_CACHE_TEA_PC_LINE_BEFORE_MAIN_RATE,
    TEA_LOAD_CACHE_SAME_CYCLE,
    TEA_LOAD_CACHE_MAIN_BEFORE_TEA_CYCLES_TOTAL,
    TEA_LOAD_CACHE_MAIN_BEFORE_TEA_CYCLES_AVG,
    TEA_LOAD_CACHE_TEA_BEFORE_MAIN_CYCLES_TOTAL,
    TEA_LOAD_CACHE_TEA_BEFORE_MAIN_CYCLES_AVG);
}

void tea_record_load_cache_hit_warm_source(Op* op, Addr line_addr) {
  if (!TEA_ENABLE || !op || op->thread_id != 1 || !op->table_info ||
      op->table_info->mem_type != MEM_LD || op->proc_id >= MAX_NUM_PROCS)
    return;

  STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_SOURCE_SAMPLES);

  uns idx = tea_recent_load_index(line_addr);
  Tea_Recent_Load_Access* main_entry = &tea_recent_main_cache[op->proc_id][idx];
  Addr pc = op->inst_info ? op->inst_info->addr : 0;

  if (!main_entry->valid || main_entry->line_addr != line_addr) {
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_NO_MAIN_LINE_BEFORE_TEA);
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_NO_MAIN_LINE_BEFORE_TEA_RATE);
    return;
  }

  if (main_entry->access_cycle == cycle_count) {
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_SAME_CYCLE);
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_SAME_CYCLE_RATE);
    return;
  }

  if (main_entry->access_cycle > cycle_count) {
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_NO_MAIN_LINE_BEFORE_TEA);
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_NO_MAIN_LINE_BEFORE_TEA_RATE);
    return;
  }

  STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TEA);
  STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TEA_RATE);

  Flag same_pc_line = (main_entry->pc == pc);
  if (same_pc_line) {
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_BEFORE_TEA);
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_BEFORE_TEA_RATE);
  }

  Counter trigger_cycle = 0;
  if (!tea_chain_trigger_cycle_for_op(op, &trigger_cycle)) {
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TEA_UNKNOWN_TRIGGER);
    if (same_pc_line)
      record_main_load_instance_class(op->proc_id,
                                      TEA_MAIN_LOAD_INSTANCE_UNKNOWN,
                                      FALSE);
    return;
  }

  int slot = (int)op->h2p_chain_id - 1;
  Tea_H2P_Chain* c = &tea_threads[op->proc_id]->chains[slot];
  Flag main_access_after_trigger = main_entry->access_cycle >= trigger_cycle;
  if (same_pc_line) {
    Tea_Main_Load_Instance_Class cls =
      classify_main_load_instance(c, main_entry);
    record_main_load_instance_class(op->proc_id, cls,
                                    main_access_after_trigger);
  }

  if (main_access_after_trigger) {
    Counter delta = main_entry->access_cycle - trigger_cycle;
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TEA_AFTER_TRIGGER);
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TEA_AFTER_TRIGGER_RATE);
    record_cache_hit_after_trigger_delta_bucket(op->proc_id, delta);
    if (same_pc_line) {
      STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_BEFORE_TEA_AFTER_TRIGGER);
      STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_BEFORE_TEA_AFTER_TRIGGER_RATE);
    }
  } else {
    Counter delta = trigger_cycle - main_entry->access_cycle;
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TEA_BEFORE_TRIGGER);
    STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_LINE_BEFORE_TEA_BEFORE_TRIGGER_RATE);
    record_cache_hit_before_trigger_delta_bucket(op->proc_id, delta);
    if (same_pc_line) {
      STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_BEFORE_TEA_BEFORE_TRIGGER);
      STAT_EVENT(op->proc_id, TEA_LOAD_CACHE_HIT_MAIN_PC_LINE_BEFORE_TEA_BEFORE_TRIGGER_RATE);
    }
  }
}

void tea_chain_note_load_result(uns proc_id, Op* op,
                                Tea_Load_Result result,
                                Counter latency) {
  if (!TEA_ENABLE || !tea_threads || !tea_threads[proc_id] ||
      !op || op->thread_id != 1)
    return;

  int slot = (int)op->h2p_chain_id - 1;
  if (!tea_chain_slot_is_valid(proc_id, slot))
    return;

  Tea_H2P_Chain* c = &tea_threads[proc_id]->chains[slot];
  if (c->state == CHAIN_INACTIVE)
    return;

  c->tea_load_exec_count++;

  switch (result) {
    case TEA_LOAD_RESULT_STORE_FORWARD:
      c->tea_load_store_forward_count++;
      break;
    case TEA_LOAD_RESULT_BYPASS:
      c->tea_load_bypass_count++;
      break;
    case TEA_LOAD_RESULT_DCACHE_HIT:
      c->tea_load_dcache_hit_count++;
      break;
    case TEA_LOAD_RESULT_STORE_SCAN_FWD:
      c->tea_load_store_scan_fwd_count++;
      break;
    case TEA_LOAD_RESULT_DCACHE_MISS:
      c->tea_load_dcache_miss_count++;
      c->tea_load_miss_latency_total += latency;
      c->tea_load_miss_max_latency =
        MAX2(c->tea_load_miss_max_latency, latency);
      if (c->first_load_miss_access_cycle == MAX_CTR)
        c->first_load_miss_access_cycle = op->dcache_cycle;
      c->last_load_miss_access_cycle = op->dcache_cycle;
      c->last_load_miss_done_cycle = op->done_cycle;
      STAT_EVENT(proc_id, TEA_LOAD_MISS_LATENCY_SAMPLES);
      INC_STAT_EVENT(proc_id, TEA_LOAD_MISS_LATENCY_TOTAL, latency);
      INC_STAT_EVENT(proc_id, TEA_LOAD_MISS_LATENCY_AVG, latency);
      break;
    default:
      break;
  }
}

void tea_record_h2p_load_miss_impact(uns proc_id, Op* tea_h2p) {
  if (!TEA_ENABLE || !tea_threads || !tea_threads[proc_id] ||
      !tea_h2p || tea_h2p->thread_id != 1)
    return;

  int slot = (int)tea_h2p->h2p_chain_id - 1;
  if (!tea_chain_slot_is_valid(proc_id, slot))
    return;

  Tea_H2P_Chain* c = &tea_threads[proc_id]->chains[slot];
  if (c->state == CHAIN_INACTIVE)
    return;

  c->h2p_resolved = TRUE;
  c->h2p_exec_cycle = tea_h2p->exec_cycle;

  Flag h2p_mispred = tea_h2p->oracle_info.mispred ||
                     tea_h2p->oracle_info.misfetch;

  if (c->tea_load_dcache_miss_count > 0) {
    STAT_EVENT(proc_id, TEA_H2P_EXEC_WITH_LOAD_MISS_CHAIN);
    INC_STAT_EVENT(proc_id, TEA_H2P_EXEC_CHAIN_LOAD_MISSES_TOTAL,
                   c->tea_load_dcache_miss_count);
    INC_STAT_EVENT(proc_id, TEA_H2P_EXEC_LOAD_MISS_MAX_LATENCY_TOTAL,
                   c->tea_load_miss_max_latency);
    INC_STAT_EVENT(proc_id, TEA_H2P_EXEC_LOAD_MISS_MAX_LATENCY_AVG,
                   c->tea_load_miss_max_latency);

    if (h2p_mispred)
      STAT_EVENT(proc_id, TEA_H2P_MISPRED_WITH_LOAD_MISS_CHAIN);
    else
      STAT_EVENT(proc_id, TEA_H2P_CORRECT_WITH_LOAD_MISS_CHAIN);

    if (c->last_load_miss_done_cycle != MAX_CTR &&
        tea_h2p->exec_cycle != MAX_CTR &&
        c->last_load_miss_done_cycle <= tea_h2p->exec_cycle) {
      Counter delta = tea_h2p->exec_cycle - c->last_load_miss_done_cycle;
      STAT_EVENT(proc_id, TEA_H2P_EXEC_AFTER_LAST_LOAD_MISS_DONE);
      INC_STAT_EVENT(proc_id,
                     TEA_H2P_EXEC_AFTER_LAST_LOAD_MISS_DONE_CYCLES_TOTAL,
                     delta);
      INC_STAT_EVENT(proc_id,
                     TEA_H2P_EXEC_AFTER_LAST_LOAD_MISS_DONE_CYCLES_AVG,
                     delta);
    } else {
      STAT_EVENT(proc_id, TEA_H2P_EXEC_WITH_LOAD_MISS_OUTSTANDING);
    }
  } else {
    STAT_EVENT(proc_id, TEA_H2P_EXEC_WITHOUT_LOAD_MISS_CHAIN);
    if (h2p_mispred)
      STAT_EVENT(proc_id, TEA_H2P_MISPRED_WITHOUT_LOAD_MISS_CHAIN);
    else
      STAT_EVENT(proc_id, TEA_H2P_CORRECT_WITHOUT_LOAD_MISS_CHAIN);
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
    tea->chains[i].first_load_miss_access_cycle = MAX_CTR;
    tea->chains[i].last_load_miss_access_cycle = MAX_CTR;
    tea->chains[i].last_load_miss_done_cycle = MAX_CTR;
    tea->chains[i].h2p_exec_cycle = MAX_CTR;
  }
  memset(tea->pending_case1_flushes, 0, sizeof(tea->pending_case1_flushes));
  clear_tea_tracking_tables(proc_id);
}

/**************************************************************************************/
/* TEA Thread Control */

void trigger_tea_thread(uns proc_id, Addr h2p_pc, Counter h2p_op_num, Op* h2p_op) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_threads && tea_threads[proc_id]);
  ASSERT(proc_id, h2p_op);

  Tea_Thread* tea = tea_threads[proc_id];
  STAT_EVENT(proc_id, TEA_TRIGGER_ATTEMPTS);
  record_trigger_main_stage(proc_id, h2p_op);

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
  record_dcc_trigger_entry_age(proc_id, chain, h2p_op);

  /* [EXPERIMENT: TEA_PERFECT_LOAD] Count load ops in triggered dependency chain */
  uns load_ops = 0;
  for (uns ci = 0; ci < chain->chain_length; ci++) {
    if (chain->chain[ci].table_info &&
        chain->chain[ci].table_info->mem_type == MEM_LD)
      load_ops++;
  }
  record_chain_load_count_bucket(proc_id, load_ops);

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
  c->dcc_insert_cycle = chain->insert_cycle;
  c->dcc_insert_h2p_op_num = chain->h2p_branch_op_num;
  c->triggered_chain_load_ops = load_ops;
  c->load_identity_count = 0;
  capture_chain_load_identities(c, chain);
  c->tea_load_exec_count = 0;
  c->tea_load_dcache_hit_count = 0;
  c->tea_load_dcache_miss_count = 0;
  c->tea_load_store_forward_count = 0;
  c->tea_load_bypass_count = 0;
  c->tea_load_store_scan_fwd_count = 0;
  c->tea_load_miss_latency_total = 0;
  c->tea_load_miss_max_latency = 0;
  c->first_load_miss_access_cycle = MAX_CTR;
  c->last_load_miss_access_cycle = MAX_CTR;
  c->last_load_miss_done_cycle = MAX_CTR;
  c->h2p_exec_cycle = MAX_CTR;
  c->h2p_resolved = FALSE;
  tea->num_active_chains++;
  update_max_active_chains(proc_id, tea, tea->num_active_chains);
  record_h2p_pc_reuse(proc_id, h2p_pc);

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
  c->dcc_insert_cycle = 0;
  c->dcc_insert_h2p_op_num = 0;
  c->triggered_chain_load_ops = 0;
  c->load_identity_count = 0;
  c->tea_load_exec_count = 0;
  c->tea_load_dcache_hit_count = 0;
  c->tea_load_dcache_miss_count = 0;
  c->tea_load_store_forward_count = 0;
  c->tea_load_bypass_count = 0;
  c->tea_load_store_scan_fwd_count = 0;
  c->tea_load_miss_latency_total = 0;
  c->tea_load_miss_max_latency = 0;
  c->first_load_miss_access_cycle = MAX_CTR;
  c->last_load_miss_access_cycle = MAX_CTR;
  c->last_load_miss_done_cycle = MAX_CTR;
  c->h2p_exec_cycle = MAX_CTR;
  c->h2p_resolved = FALSE;
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
    tea->chains[i].dcc_insert_cycle = 0;
    tea->chains[i].dcc_insert_h2p_op_num = 0;
    tea->chains[i].triggered_chain_load_ops = 0;
    tea->chains[i].load_identity_count = 0;
    tea->chains[i].tea_load_exec_count = 0;
    tea->chains[i].tea_load_dcache_hit_count = 0;
    tea->chains[i].tea_load_dcache_miss_count = 0;
    tea->chains[i].tea_load_store_forward_count = 0;
    tea->chains[i].tea_load_bypass_count = 0;
    tea->chains[i].tea_load_store_scan_fwd_count = 0;
    tea->chains[i].tea_load_miss_latency_total = 0;
    tea->chains[i].tea_load_miss_max_latency = 0;
    tea->chains[i].first_load_miss_access_cycle = MAX_CTR;
    tea->chains[i].last_load_miss_access_cycle = MAX_CTR;
    tea->chains[i].last_load_miss_done_cycle = MAX_CTR;
    tea->chains[i].h2p_exec_cycle = MAX_CTR;
    tea->chains[i].h2p_resolved = FALSE;
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
