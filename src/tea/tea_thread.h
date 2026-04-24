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
 *                for branch precomputation. Supports multiple concurrent H2P chains.
 ***************************************************************************************/

#ifndef __TEA_THREAD_H__
#define __TEA_THREAD_H__

#include "globals/global_types.h"
#include "op.h"

/**************************************************************************************/
/* Maximum number of concurrent TEA chains (parameterized via TEA_MAX_CHAINS) */

#define MAX_TEA_CHAINS 4

/**************************************************************************************/
/* TEA Chain State */

typedef enum Tea_Chain_State_enum {
  CHAIN_INACTIVE,    /* Slot empty */
  CHAIN_FETCHING,    /* Fetch in progress */
  CHAIN_EXECUTING,   /* Fetch complete, ops running in backend */
} Tea_Chain_State;

/**************************************************************************************/
/* Per-chain H2P context */

typedef struct Tea_H2P_Chain_struct {
  Tea_Chain_State state;

  /* H2P branch info */
  Addr target_h2p_pc;
  Counter target_h2p_op_num;       /* Main thread op_num (older/younger comparison) */
  Op* main_h2p_op;                 /* Main thread H2P branch Op pointer */
  Counter saved_unique_num;        /* main_h2p_op's unique_num (validity check) */
  Op_Info h2p_oracle_info;         /* Oracle info snapshot at trigger time */
  Recovery_Info h2p_recovery_info; /* Recovery info snapshot at trigger time */

  /* Per-chain counters */
  uns tea_op_count;                /* Ops currently in pipeline for this chain */
  Counter tea_ops_fetched;         /* Total ops fetched for this chain */
} Tea_H2P_Chain;

/**************************************************************************************/
/* TEA Thread State (legacy — used only for tea_is_active() compatibility) */

typedef enum Tea_State_enum {
  TEA_IDLE,       /* No TEA chain active */
  TEA_FETCHING,   /* At least one chain fetching */
  TEA_EXECUTING,  /* TEA ops in pipeline */
} Tea_State;

/**************************************************************************************/
/* TEA Thread Structure */

typedef struct Tea_Thread_struct {
  uns8 proc_id;

  Tea_State state;                 /* Legacy state (TEA_IDLE when num_active_chains==0) */

  /* Multi-H2P chain tracking */
  Tea_H2P_Chain chains[MAX_TEA_CHAINS];
  uns num_active_chains;           /* Number of non-INACTIVE chains */
  uns current_fetch_chain;         /* Index of chain currently being fetched */

  /* Shared resources */
  Counter tea_op_counter;          /* Global TEA op_num counter (0x8000... namespace) */
  Counter tea_start_cycle;

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
void terminate_tea_chain(uns proc_id, int chain_id);
void terminate_tea_thread(uns proc_id);

/* State queries */
Flag tea_is_active(uns proc_id);
Tea_State tea_get_state(uns proc_id);

/* Per-cycle update */
void update_tea_thread(uns proc_id);

/* Op management */
void tea_op_completed(uns proc_id, Op* op);
void tea_op_flushed(uns proc_id, Op* op);

/**************************************************************************************/

#endif /* __TEA_THREAD_H__ */
