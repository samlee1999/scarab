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
typedef struct Tea_Pending_Case1_Flush_struct {
  Flag    valid;
  Op*     main_h2p_op;
  Counter main_h2p_unique_num;  /* validity stamp — matches main_h2p_op->unique_num */
  Counter main_h2p_op_num;      /* for selective clear on recovery flush */
} Tea_Pending_Case1_Flush;

/**************************************************************************************/
/* Per-Chain State */

typedef enum Tea_Chain_State_enum {
  CHAIN_INACTIVE,   /* Slot unused */
  CHAIN_FETCHING,   /* Fetching ops from Dep Chain Cache */
  CHAIN_EXECUTING,  /* Ops in OoO backend */
} Tea_Chain_State;

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
} Tea_H2P_Chain;

/**************************************************************************************/
/* TEA Thread Structure */

#define MAX_TEA_CHAINS 4  /* Compile-time upper bound; runtime limit = TEA_MAX_CHAINS */

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

} Tea_Thread;

/**************************************************************************************/
/* Global Variables */

extern Tea_Thread** tea_threads;   /* Per-core TEA thread state */

/**************************************************************************************/
/* Function Prototypes */

/* Initialization and reset */
void init_tea_thread(uns proc_id);
void reset_tea_thread(uns proc_id);

/* TEA thread control */
void trigger_tea_thread(uns proc_id, Addr h2p_pc, Counter h2p_op_num, Op* h2p_op);
void terminate_tea_chain(uns proc_id, int chain_slot);  /* Per-chain termination */
void terminate_tea_thread(uns proc_id);                  /* All chains termination */

/* State queries */
Flag tea_is_active(uns proc_id);
Tea_State tea_get_state(uns proc_id);

/* Per-cycle update */
void update_tea_thread(uns proc_id);

/* Op management */
void tea_op_completed(uns proc_id, Op* op);

/* Case 1 pending early flush management */
void tea_record_pending_case1_flush(uns proc_id, Op* main_h2p);
void tea_clear_pending_case1_flushes(uns proc_id);
void tea_selective_clear_pending_case1_flushes(uns proc_id, Counter recovery_op_num);

/**************************************************************************************/

#endif /* __TEA_THREAD_H__ */
