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
/* TEA Thread State */

typedef enum Tea_State_enum {
  TEA_IDLE,       /* No TEA thread active */
  TEA_FETCHING,   /* Fetching ops from Block Cache */
  TEA_EXECUTING,  /* TEA ops in pipeline */
} Tea_State;

/**************************************************************************************/
/* TEA Thread Structure */

typedef struct Tea_Thread_struct {
  uns8 proc_id;                    /* Processor ID */
  Tea_State state;                 /* Current TEA state */

  /* Target H2P branch info */
  Addr target_h2p_pc;              /* PC of the H2P branch being precomputed */
  Counter target_h2p_op_num;       /* Main thread's op_num for the H2P branch */
  Op* main_h2p_op;                 /* Pointer to Main thread's H2P op (for recovery identity) */
  Op_Info h2p_oracle_info;         /* Oracle info from main H2P op (saved at trigger) */
  Recovery_Info h2p_recovery_info; /* Recovery info from main H2P op (for BP checkpoint) */

  /* Block Cache traversal */
  Addr current_block_pc;           /* Current block being fetched from Block Cache */
  int current_chain_idx;           /* Index within current dependency chain */

  /* TEA pipeline tracking */
  uns tea_op_count;                /* Number of TEA ops currently in pipeline */
  Counter tea_ops_fetched;         /* Total TEA ops fetched in current activation */
  Counter tea_op_counter;          /* Counter for assigning TEA op_nums */

  /* Timing */
  Counter tea_start_cycle;         /* Cycle when TEA was triggered */

  /* Statistics */
  Counter stat_tea_triggers;       /* Number of times TEA was triggered */
  Counter stat_tea_early_flushes;  /* Number of early misprediction flushes */
  Counter stat_tea_ops_executed;   /* Total TEA ops executed */

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
