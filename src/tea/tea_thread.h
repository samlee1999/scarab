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
 * File         : tea/tea_thread.h
 * Author       : TEA Implementation
 * Date         : 2025
 * Description  : TEA (Timely, Efficient, and Accurate) Thread state management
 *                for branch precomputation.
 ***************************************************************************************/

#ifndef __TEA_THREAD_H__
#define __TEA_THREAD_H__

#include "globals/global_types.h"
#include "op.h"
#include "dependency_chain_cache.h"

/**************************************************************************************/
/* TEA Thread State (top-level) */

typedef enum Tea_State_enum {
  TEA_IDLE,       /* No active chains */
  TEA_FETCHING,   /* At least one chain is FETCHING (kept for compatibility) */
  TEA_EXECUTING,  /* At least one chain is EXECUTING (kept for compatibility) */
} Tea_State;

/**************************************************************************************/
/* Case 1 Pending Early Flush */

/* When TEA detects a mispredicted H2P branch (Case 1) before the main thread's
 * SRT checkpoint exists, we cannot call bp_sched_recovery immediately.  Instead
 * we record the intent here and trigger recovery the moment the main H2P reaches
 * rename and its SRT checkpoint is created. */
typedef enum Tea_Case1_Main_Stage_enum {
  TEA_CASE1_MAIN_STAGE_UNKNOWN,
  TEA_CASE1_MAIN_STAGE_PRE_DECODE,
  TEA_CASE1_MAIN_STAGE_DECODED_PRE_RENAME,
  TEA_CASE1_MAIN_STAGE_IN_RENAME,
  TEA_CASE1_MAIN_STAGE_IN_NODE_OR_RS,
  TEA_CASE1_MAIN_STAGE_SCHEDULED_OR_EXECUTING,
  TEA_CASE1_MAIN_STAGE_DONE_OR_LATER,
} Tea_Case1_Main_Stage;

typedef struct Tea_Pending_Case1_Flush_struct {
  Flag    valid;
  Op*     main_h2p_op;
  Counter main_h2p_unique_num;  /* validity stamp — matches main_h2p_op->unique_num */
  Counter main_h2p_op_num;      /* for selective clear on recovery flush */
  Counter detect_cycle;         /* TEA H2P exec cycle that created this pending flush */
  Tea_Case1_Main_Stage main_stage_at_detect;
} Tea_Pending_Case1_Flush;

/**************************************************************************************/
/* Per-Chain State */

typedef enum Tea_Chain_State_enum {
  CHAIN_INACTIVE,   /* Slot unused */
  CHAIN_FETCHING,   /* Fetching ops from Dep Chain Cache */
  CHAIN_EXECUTING,  /* Ops in OoO backend */
} Tea_Chain_State;

typedef enum Tea_Chain_Termination_Reason_enum {
  TEA_CHAIN_TERM_REASON_UNKNOWN,
  TEA_CHAIN_TERM_REASON_NATURAL,
  TEA_CHAIN_TERM_REASON_H2P_CORRECT,
  TEA_CHAIN_TERM_REASON_EARLY_FLUSH_CASE1,
  TEA_CHAIN_TERM_REASON_MAIN_RECOVERY,
  TEA_CHAIN_TERM_REASON_INVALID_MAIN_H2P,
  TEA_CHAIN_TERM_REASON_DEP_CHAIN_LOST,
  TEA_CHAIN_TERM_REASON_FULL_THREAD,
} Tea_Chain_Termination_Reason;

typedef enum Tea_Load_Result_enum {
  TEA_LOAD_RESULT_STORE_FORWARD,
  TEA_LOAD_RESULT_BYPASS,
  TEA_LOAD_RESULT_DCACHE_HIT,
  TEA_LOAD_RESULT_DCACHE_MISS,
  TEA_LOAD_RESULT_STORE_SCAN_FWD,
} Tea_Load_Result;

typedef struct Tea_Chain_Load_Identity_struct {
  Flag    valid;
  Addr    pc;
  Addr    line_addr;
  Counter h2p_op_delta;
} Tea_Chain_Load_Identity;

typedef struct Tea_H2P_Chain_struct {
  Tea_Chain_State state;

  /* H2P branch information */
  Addr    target_h2p_pc;
  Counter target_h2p_op_num;      /* Main thread op_num — for older/younger compare */
  Op*     main_h2p_op;            /* Pointer to Main H2P op (for early flush) */
  Counter saved_unique_num;       /* main_h2p_op->unique_num at trigger time */
  Op_Info         h2p_oracle_info;    /* Saved at trigger */
  Recovery_Info   h2p_recovery_info;  /* Saved at trigger */

  /* Per-chain counters */
  uns     tea_op_count;           /* Ops currently in pipeline (node stage) */
  Counter tea_ops_fetched;        /* Total ops fetched from dep chain */
  Counter trigger_cycle;          /* Cycle this chain slot became active */
  Counter fetch_done_cycle;       /* Cycle dependency-chain fetch completed */
  Counter dcc_insert_cycle;       /* Cycle when the replayed DCC entry was created */
  Counter dcc_insert_h2p_op_num;  /* Main H2P op_num in the saved DCC entry */
  uns     triggered_chain_load_ops;
  uns     load_identity_count;
  Tea_Chain_Load_Identity load_identities[MAX_CHAIN_LENGTH];

  /* Per-chain TEA load summary */
  Counter tea_load_exec_count;
  Counter tea_load_dcache_hit_count;
  Counter tea_load_dcache_miss_count;
  Counter tea_load_store_forward_count;
  Counter tea_load_bypass_count;
  Counter tea_load_store_scan_fwd_count;
  Counter tea_load_miss_latency_total;
  Counter tea_load_miss_max_latency;
  Counter first_load_miss_access_cycle;
  Counter last_load_miss_access_cycle;
  Counter last_load_miss_done_cycle;
  Counter h2p_exec_cycle;
  Flag    h2p_resolved;
} Tea_H2P_Chain;

/**************************************************************************************/
/* TEA Thread Structure */

#define MAX_TEA_CHAINS 16  /* Compile-time capacity; runtime limit = TEA_MAX_CHAINS */
#define TEA_OP_NUM_BASE 0x8000000000000000ULL

typedef struct Tea_Thread_struct {
  uns8 proc_id;
  Tea_State state;                /* TEA_IDLE when num_active_chains == 0 */

  /* Multi-H2P chain array */
  Tea_H2P_Chain chains[MAX_TEA_CHAINS];
  uns num_active_chains;          /* Non-INACTIVE chain count */
  int current_fetch_chain;        /* chains[] index being fetched (-1 = none) */

  /* Shared resources (chain-independent) */
  Counter tea_op_counter;         /* Global TEA op_num counter (starts at 0x8000...0) */
  Counter tea_start_cycle;

  /* Case 1 pending early flushes (max one per chain slot) */
  Tea_Pending_Case1_Flush pending_case1_flushes[MAX_TEA_CHAINS];

  /* Statistics */
  Counter stat_tea_triggers;
  Counter stat_tea_early_flushes;
  Counter stat_tea_ops_executed;
  uns     stat_max_active_chains;

} Tea_Thread;

/**************************************************************************************/
/* Global Variables */

extern Tea_Thread** tea_threads;   /* Per-core TEA thread state */

/**************************************************************************************/
/* Configuration Helpers */

uns tea_max_chains(uns proc_id);
Flag tea_chain_slot_is_valid(uns proc_id, int chain_slot);

/**************************************************************************************/
/* Function Prototypes */

/* Initialization and reset */
void init_tea_thread(uns proc_id);
void reset_tea_thread(uns proc_id);

/* TEA thread control */
void trigger_tea_thread(uns proc_id, Addr h2p_pc, Counter h2p_op_num, Op* h2p_op);
void terminate_tea_chain(uns proc_id, int chain_slot);  /* Per-chain termination */
void terminate_tea_chain_with_reason(uns proc_id, int chain_slot,
                                     Tea_Chain_Termination_Reason reason);
void terminate_tea_thread(uns proc_id);                  /* All chains termination */

/* State queries */
Flag tea_is_active(uns proc_id);
Tea_State tea_get_state(uns proc_id);

/* Per-cycle update */
void update_tea_thread(uns proc_id);

/* Op management */
void tea_op_completed(uns proc_id, Op* op);
void tea_record_load_issue_order(Op* op);
void tea_record_load_cache_access_order(Op* op, Addr line_addr);
void tea_record_load_cache_hit_warm_source(Op* op, Addr line_addr);
void tea_chain_note_load_result(uns proc_id, Op* op,
                                Tea_Load_Result result,
                                Counter latency);
void tea_record_h2p_load_miss_impact(uns proc_id, Op* tea_h2p);

/* Case 1 pending early flush management */
Flag tea_record_pending_case1_flush(uns proc_id, Op* main_h2p,
                                    Counter detect_cycle,
                                    Tea_Case1_Main_Stage main_stage_at_detect);
void tea_clear_pending_case1_flushes(uns proc_id);
void tea_selective_clear_pending_case1_flushes(uns proc_id, Counter recovery_op_num);

/**************************************************************************************/

#endif /* __TEA_THREAD_H__ */
