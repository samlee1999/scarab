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
 * File         : tea/tea_rename.h
 * Author       : TEA Implementation
 * Date         : 2025
 * Description  : TEA Rename Stage - Shadow RAT for register renaming
 ***************************************************************************************/

#ifndef __TEA_RENAME_H__
#define __TEA_RENAME_H__

#include "globals/global_types.h"
#include "stage_data.h"
#include "isa/isa_macros.h"
#include "tea/tea_thread.h"  /* MAX_TEA_CHAINS */

/**************************************************************************************/
/* Forward Declarations */

struct Op_struct;
typedef struct Op_struct Op;

/**************************************************************************************/
/* Phase 4: TEA Physical Register Free List Structure */

/* Simple array-based free list for TEA preg pool
 * Uses circular buffer for efficient alloc/free operations
 */
typedef struct Tea_Preg_Free_List_struct {
  uns*  indices;     /* Array of physical register indices */
  uns   head;        /* Stack top index (next to allocate) */
  uns   tail;        /* Stack bottom index (next free slot) */
  uns   size;        /* Total capacity */
  uns   count;       /* Current free count */
} Tea_Preg_Free_List;

/**************************************************************************************/
/* Shadow RAT Structure */

/* Shadow RAT: Per-chain snapshot of Main RAT's architectural register mappings.
 * One instance per chain slot (MAX_TEA_CHAINS total).
 * Each chain gets its own independent mapping state and PREG pool,
 * eliminating cross-chain PREG dependencies and enabling safe per-chain reset.
 */
typedef struct Shadow_RAT_struct {
  uns8 proc_id;

  /* Architectural to Physical register mappings
   * Copied from Main RAT at TEA trigger time for this chain
   * Updated as this chain's TEA ops are renamed
   */
  int* gp_mappings;      /* General-purpose register mappings */
  int* vec_mappings;     /* Vector register mappings */

  /* Sizes of mapping arrays */
  uns gp_size;           /* Number of GP arch registers */
  uns vec_size;          /* Number of VEC arch registers */

  /* Per-chain TEA dedicated preg pools */
  Tea_Preg_Free_List* tea_gp_preg_pool;
  Tea_Preg_Free_List* tea_vec_preg_pool;

  /* Partition boundaries for this chain slot's PREG sub-range */
  int tea_gp_start_idx;
  int tea_vec_start_idx;

  /* Producer Op tracking (for intra-chain dependency wakeup only) */
  Op**     gp_producer_ops;
  Counter* gp_producer_unums;
  Op**     vec_producer_ops;
  Counter* vec_producer_unums;

  /* Validity flag: TRUE after snapshot, FALSE after reset */
  Flag is_valid;

} Shadow_RAT;

/**************************************************************************************/
/* TEA Rename Stage Structure */

typedef struct Tea_Rename_Stage_struct {
  uns8 proc_id;

  /* Stage interface data - output to Node Stage (RS insertion) */
  Stage_Data sd;

  /* Per-chain Shadow RATs: chain_srats[slot] owns its own mapping state and PREG pool.
   * Indexed by chain slot (0-based). Initialized at startup, snapshotted per trigger,
   * reset individually on per-chain termination. */
  Shadow_RAT* chain_srats[MAX_TEA_CHAINS];

} Tea_Rename_Stage;

/**************************************************************************************/
/* Global Variables */

extern Tea_Rename_Stage** tea_rename_stages;  /* Per-core TEA rename stage */

/**************************************************************************************/
/* Function Prototypes */

/* Initialization and reset */
void init_tea_rename_stage(uns proc_id);
void reset_tea_rename_stage(uns proc_id);

/* Phase 4: TEA preg pool initialization (call after reg_file_init) */
void init_tea_preg_pools(uns proc_id);

/* Shadow RAT operations — now per-chain */
void shadow_rat_snapshot(uns proc_id, int chain_slot);
void reset_tea_preg_pool(uns proc_id, int chain_slot);

/* Phase 4.1: Resource availability check for stalling (exact dest reg counts) */
Flag tea_preg_pool_available(uns proc_id, int chain_slot,
                              uns required_gp, uns required_vec);

/* Per-cycle update */
void update_tea_rename_stage(uns proc_id, Stage_Data* tea_fetch_sd);

/* Single op rename */
void tea_rename_op(uns proc_id, Op* op);

/* Recovery */
void recover_tea_rename_stage(uns proc_id);
void recover_tea_rename_stage_by_chain(uns proc_id, uns8 chain_id);

/**************************************************************************************/

#endif /* __TEA_RENAME_H__ */
