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
 * File         : tea/tea_fetch_stage.h
 * Author       : TEA Implementation
 * Date         : 2025
 * Description  : TEA Fetch Stage - fetches dependency chain ops from Block Cache
 ***************************************************************************************/

#ifndef __TEA_FETCH_STAGE_H__
#define __TEA_FETCH_STAGE_H__

#include "globals/global_types.h"
#include "stage_data.h"
#include "dependency_chain_cache.h"

/**************************************************************************************/
/* Forward Declarations */

struct Op_struct;
typedef struct Op_struct Op;

/**************************************************************************************/
/* TEA Fetch Stage Structure */

typedef struct Tea_Fetch_Stage_struct {
  uns8 proc_id;

  /* Stage interface data - output to TEA Rename */
  Stage_Data sd;

  /* Block Cache traversal state */
  Dependency_Chain_Cache_Entry* active_chain;  /* Current dependency chain */
  int current_chain_idx;                       /* Index within chain */
  int total_chain_length;                      /* Total ops in chain */

  /* Fetch state */
  Flag fetch_complete;                         /* All ops fetched from chain */
  Counter ops_fetched_this_cycle;              /* Ops fetched in current cycle */

} Tea_Fetch_Stage;

/**************************************************************************************/
/* Global Variables */

extern Tea_Fetch_Stage** tea_fetch_stages;  /* Per-core TEA fetch stage */

/**************************************************************************************/
/* Function Prototypes */

/* Initialization and reset */
void init_tea_fetch_stage(uns proc_id);
void reset_tea_fetch_stage(uns proc_id);

/* Per-cycle update */
void update_tea_fetch_stage(uns proc_id);

/* Recovery */
void recover_tea_fetch_stage(uns proc_id);

/* Op creation from Block Cache */
Op* tea_create_op_from_cache(uns proc_id, Op* cached_op, Flag is_h2p_branch);

/**************************************************************************************/

#endif /* __TEA_FETCH_STAGE_H__ */
