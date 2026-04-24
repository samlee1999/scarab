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
 * File         : tea/tea_rename.c
 * Author       : TEA Implementation
 * Date         : 2025
 * Description  : TEA Rename Stage - Shadow RAT for register renaming
 ***************************************************************************************/

#include "tea/tea_rename.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"

#include "core.param.h"
#include "inst_info.h"
#include "isa/isa.h"
#include "isa/isa_macros.h"
#include "map.h"
#include "map_rename.h"
#include "op.h"
#include "op_pool.h"
#include "statistics.h"
#include "tea/tea_thread.h"
#include "xed-interface.h"

/**************************************************************************************/
/* Global Variables */

Tea_Rename_Stage** tea_rename_stages = NULL;

/**************************************************************************************/
/* Local Prototypes */

static void tea_rename_stage_init_stage_data(Tea_Rename_Stage* rename);
static void init_shadow_rat(uns proc_id, Shadow_RAT* srat);
static int get_reg_type_for_rename(int reg_id);
static int shadow_rat_read_mapping(Shadow_RAT* srat, int arch_reg_id, int reg_type);
static void shadow_rat_write_mapping(Shadow_RAT* srat, int arch_reg_id, int phys_reg_id, int reg_type);

/**************************************************************************************/
/* init_tea_rename_stage */

void init_tea_rename_stage(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_rename_stages) {
    tea_rename_stages = (Tea_Rename_Stage**)calloc(NUM_CORES, sizeof(Tea_Rename_Stage*));
  }

  Tea_Rename_Stage* rename = (Tea_Rename_Stage*)calloc(1, sizeof(Tea_Rename_Stage));
  tea_rename_stages[proc_id] = rename;

  rename->proc_id = proc_id;

  /* Initialize Stage_Data */
  tea_rename_stage_init_stage_data(rename);

  /* Allocate and initialize Shadow RAT */
  rename->shadow_rat = (Shadow_RAT*)calloc(1, sizeof(Shadow_RAT));
  init_shadow_rat(proc_id, rename->shadow_rat);
}

/**************************************************************************************/
/* tea_rename_stage_init_stage_data */

static void tea_rename_stage_init_stage_data(Tea_Rename_Stage* rename) {
  Stage_Data* sd = &rename->sd;

  sd->name = "TEA_RENAME";
  sd->max_op_count = TEA_FETCH_WIDTH;  /* Same as TEA fetch width */
  sd->op_count = 0;
  sd->ops = (Op**)calloc(TEA_FETCH_WIDTH, sizeof(Op*));
  ASSERT(rename->proc_id, sd->ops);
}

/**************************************************************************************/
/* init_shadow_rat */

static void init_shadow_rat(uns proc_id, Shadow_RAT* srat) {
  ASSERT(proc_id, srat);

  srat->proc_id = proc_id;

  /* Allocate mapping arrays
   * Size based on ISA architectural registers
   * x86-64: ~16 GP regs, ~32 VEC regs (XMM/YMM/ZMM)
   */
  srat->gp_size = NUM_REG_IDS;   /* Conservative upper bound */
  srat->vec_size = NUM_REG_IDS;  /* Conservative upper bound */

  srat->gp_mappings = (int*)calloc(srat->gp_size, sizeof(int));
  srat->vec_mappings = (int*)calloc(srat->vec_size, sizeof(int));

  /* Initialize all mappings to invalid */
  for (uns i = 0; i < srat->gp_size; i++) {
    srat->gp_mappings[i] = REG_TABLE_REG_ID_INVALID;
  }
  for (uns i = 0; i < srat->vec_size; i++) {
    srat->vec_mappings[i] = REG_TABLE_REG_ID_INVALID;
  }

  /* Phase 4: Initialize TEA preg pools to NULL
   * Actual initialization happens in init_tea_preg_pools() after reg_file_init
   */
  srat->tea_gp_preg_pool = NULL;
  srat->tea_vec_preg_pool = NULL;

  /* Phase 4: Calculate partition boundaries
   * Main: indices 0 to (SIZE - TEA_PREG_RESERVATION - 1)
   * TEA:  indices (SIZE - TEA_PREG_RESERVATION) to (SIZE - 1)
   */
  srat->tea_gp_start_idx = REG_TABLE_INTEGER_PHYSICAL_SIZE - TEA_PREG_RESERVATION;
  srat->tea_vec_start_idx = REG_TABLE_VECTOR_PHYSICAL_SIZE - TEA_PREG_RESERVATION;

  /* Producer Op arrays for dependency wakeup */
  srat->gp_producer_ops = (Op**)calloc(srat->gp_size, sizeof(Op*));
  srat->gp_producer_unums = (Counter*)calloc(srat->gp_size, sizeof(Counter));
  srat->vec_producer_ops = (Op**)calloc(srat->vec_size, sizeof(Op*));
  srat->vec_producer_unums = (Counter*)calloc(srat->vec_size, sizeof(Counter));

  srat->is_valid = FALSE;
}

/**************************************************************************************/
/* reset_tea_rename_stage */

void reset_tea_rename_stage(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];
  Shadow_RAT* srat = rename->shadow_rat;

  /* Invalidate Shadow RAT */
  srat->is_valid = FALSE;

  /* Clear all mappings */
  for (uns i = 0; i < srat->gp_size; i++) {
    srat->gp_mappings[i] = REG_TABLE_REG_ID_INVALID;
  }
  for (uns i = 0; i < srat->vec_size; i++) {
    srat->vec_mappings[i] = REG_TABLE_REG_ID_INVALID;
  }

  /* Clear producer Op arrays */
  memset(srat->gp_producer_ops, 0, srat->gp_size * sizeof(Op*));
  memset(srat->gp_producer_unums, 0, srat->gp_size * sizeof(Counter));
  memset(srat->vec_producer_ops, 0, srat->vec_size * sizeof(Op*));
  memset(srat->vec_producer_unums, 0, srat->vec_size * sizeof(Counter));

  /* Free and clear Stage_Data - CRITICAL: must free_op to avoid op pool exhaustion */
  for (uns i = 0; i < rename->sd.max_op_count; i++) {
    if (rename->sd.ops[i]) {
      free_op(rename->sd.ops[i]);
      rename->sd.ops[i] = NULL;
    }
  }
  rename->sd.op_count = 0;
}

/**************************************************************************************/
/* shadow_rat_snapshot */

void shadow_rat_snapshot(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];
  Shadow_RAT* srat = rename->shadow_rat;

  /* Copy architectural register mappings from Main RAT
   * Main RAT is in reg_file[reg_type]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL]
   */
  extern struct reg_file** reg_file;

  /* Copy GP register mappings */
  if (reg_file && reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]) {
    struct reg_table* arch_table =
      reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];

    if (arch_table && arch_table->entries) {
      for (uns i = 0; i < arch_table->size && i < srat->gp_size; i++) {
        /* Copy the child_reg_id which points to current physical register */
        srat->gp_mappings[i] = arch_table->entries[i].child_reg_id;
      }
    }
  }

  /* Copy VEC register mappings */
  if (reg_file && reg_file[REG_FILE_REG_TYPE_VECTOR]) {
    struct reg_table* arch_table =
      reg_file[REG_FILE_REG_TYPE_VECTOR]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];

    if (arch_table && arch_table->entries) {
      for (uns i = 0; i < arch_table->size && i < srat->vec_size; i++) {
        /* Copy the child_reg_id which points to current physical register */
        srat->vec_mappings[i] = arch_table->entries[i].child_reg_id;
      }
    }
  }

  /* Clear producer op pointers: main-thread arch inputs are immediately ready.
   * We do NOT copy speculative SRT producer ops (from map_data->reg_map).
   * Those in-flight ops can be squashed by a main-thread recovery that does NOT
   * terminate the TEA chain (older chains survive recover_tea_on_flush).  Any
   * TEA op that registered a dependency on a squashed main-thread op would then
   * wait in the RS forever, keeping tea_op_count > 0 and permanently occupying
   * the chain slot.  Since arch pregs hold committed values, no wakeup wait is
   * needed — set producer_op = NULL so all main-thread inputs are ready on
   * dispatch.  Intra-chain producers are written by tea_rename_op() when each
   * TEA destination register is renamed. */
  if (srat->gp_producer_ops)
    memset(srat->gp_producer_ops, 0, srat->gp_size * sizeof(Op*));
  if (srat->gp_producer_unums)
    memset(srat->gp_producer_unums, 0, srat->gp_size * sizeof(Counter));
  if (srat->vec_producer_ops)
    memset(srat->vec_producer_ops, 0, srat->vec_size * sizeof(Op*));
  if (srat->vec_producer_unums)
    memset(srat->vec_producer_unums, 0, srat->vec_size * sizeof(Counter));

  srat->is_valid = TRUE;
}

/**************************************************************************************/
/* Phase 4: TEA Preg Pool Helper Functions */

/* Allocate from TEA preg pool - returns preg index or -1 if empty */
static int tea_preg_pool_alloc(Tea_Preg_Free_List* pool) {
  if (!pool || pool->count == 0) {
    return -1;  /* Pool exhausted */
  }

  int preg_idx = (int)pool->indices[pool->head];
  pool->head = (pool->head + 1) % pool->size;
  pool->count--;

  return preg_idx;
}

/* Return preg to TEA pool (unused in Phase 4, kept for future use) */
static void tea_preg_pool_free(Tea_Preg_Free_List* pool, uns preg_idx) __attribute__((unused));
static void tea_preg_pool_free(Tea_Preg_Free_List* pool, uns preg_idx) {
  if (!pool) return;

  ASSERT(0, pool->count < pool->size);

  pool->indices[pool->tail] = preg_idx;
  pool->tail = (pool->tail + 1) % pool->size;
  pool->count++;
}

/* Create and initialize a TEA preg pool */
static Tea_Preg_Free_List* create_tea_preg_pool(int start_idx, int count) {
  Tea_Preg_Free_List* pool = (Tea_Preg_Free_List*)calloc(1, sizeof(Tea_Preg_Free_List));
  pool->size = count;
  pool->count = count;
  pool->head = 0;
  pool->tail = 0;
  pool->indices = (uns*)calloc(count, sizeof(uns));

  /* Populate with indices from start_idx to start_idx + count - 1 */
  for (int i = 0; i < count; i++) {
    pool->indices[i] = start_idx + i;
  }
  pool->tail = 0;  /* Circular buffer: all slots filled, tail wraps to 0 */

  return pool;
}

/* Remove TEA entries from main free list by rebuilding without them */
static void remove_tea_entries_from_main_free_list(struct reg_table* phys_table,
                                                    int tea_start_idx) {
  struct reg_free_list* main_fl = phys_table->free_list;

  /* Rebuild main free list without TEA entries */
  struct reg_table_entry* new_head = NULL;
  struct reg_table_entry** tail_ptr = &new_head;
  uns new_count = 0;

  struct reg_table_entry* entry = main_fl->reg_free_list_head;
  while (entry) {
    struct reg_table_entry* next = entry->next_free;

    if (entry->self_reg_id < tea_start_idx) {
      /* Keep in main free list */
      *tail_ptr = entry;
      tail_ptr = &entry->next_free;
      entry->next_free = NULL;
      new_count++;
    }
    /* else: TEA entry - skip (already tracked in TEA pool) */

    entry = next;
  }

  main_fl->reg_free_list_head = new_head;
  main_fl->reg_free_num = new_count;
}

/**************************************************************************************/
/* init_tea_preg_pools: Called once after reg_file_init() to partition preg pools */

void init_tea_preg_pools(uns proc_id) {
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);

  extern struct reg_file** reg_file;
  ASSERT(proc_id, reg_file);  /* Must be called after reg_file_init() */

  Shadow_RAT* srat = tea_rename_stages[proc_id]->shadow_rat;

  /* Create TEA GP preg pool and partition main free list */
  if (reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]) {
    struct reg_table* gp_phys =
      reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]->reg_table[REG_TABLE_TYPE_PHYSICAL];

    /* Create TEA pool with reserved indices */
    srat->tea_gp_preg_pool = create_tea_preg_pool(srat->tea_gp_start_idx,
                                                   TEA_PREG_RESERVATION);

    /* Remove TEA entries from main free list */
    remove_tea_entries_from_main_free_list(gp_phys, srat->tea_gp_start_idx);
  }

  /* Create TEA VEC preg pool and partition main free list */
  if (reg_file[REG_FILE_REG_TYPE_VECTOR]) {
    struct reg_table* vec_phys =
      reg_file[REG_FILE_REG_TYPE_VECTOR]->reg_table[REG_TABLE_TYPE_PHYSICAL];

    /* Create TEA pool with reserved indices */
    srat->tea_vec_preg_pool = create_tea_preg_pool(srat->tea_vec_start_idx,
                                                    TEA_PREG_RESERVATION);

    /* Remove TEA entries from main free list */
    remove_tea_entries_from_main_free_list(vec_phys, srat->tea_vec_start_idx);
  }
}

/**************************************************************************************/
/* reset_tea_preg_pool: Return all TEA pregs to pool on TEA termination */

void reset_tea_preg_pool(uns proc_id) {
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);

  Shadow_RAT* srat = tea_rename_stages[proc_id]->shadow_rat;

  /* Reset GP preg pool to full */
  if (srat->tea_gp_preg_pool) {
    Tea_Preg_Free_List* pool = srat->tea_gp_preg_pool;
    pool->head = 0;
    pool->tail = 0;
    pool->count = pool->size;
    /* Re-populate indices (in case they got scrambled) */
    for (uns i = 0; i < pool->size; i++) {
      pool->indices[i] = srat->tea_gp_start_idx + i;
    }
  }

  /* Reset VEC preg pool to full */
  if (srat->tea_vec_preg_pool) {
    Tea_Preg_Free_List* pool = srat->tea_vec_preg_pool;
    pool->head = 0;
    pool->tail = 0;
    pool->count = pool->size;
    /* Re-populate indices */
    for (uns i = 0; i < pool->size; i++) {
      pool->indices[i] = srat->tea_vec_start_idx + i;
    }
  }
}

/**************************************************************************************/
/* Phase 4.1: tea_preg_pool_available - Check if enough pregs available for stalling */

Flag tea_preg_pool_available(uns proc_id, uns ops_count) {
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];
  Shadow_RAT* srat = rename->shadow_rat;

  if (!srat || !srat->is_valid) {
    return FALSE;
  }

  /* Conservative estimate: each op may have up to 2 dest regs (GP) + 1 (VEC) */
  uns required_gp = ops_count * 2;
  uns required_vec = ops_count * 1;

  /* Check GP pool availability */
  Flag gp_ok = (!srat->tea_gp_preg_pool ||
                srat->tea_gp_preg_pool->count >= required_gp);

  /* Check VEC pool availability */
  Flag vec_ok = (!srat->tea_vec_preg_pool ||
                 srat->tea_vec_preg_pool->count >= required_vec);

  return gp_ok && vec_ok;
}

/**************************************************************************************/
/* update_tea_rename_stage */

void update_tea_rename_stage(uns proc_id, Stage_Data* tea_fetch_sd) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);
  ASSERT(proc_id, tea_fetch_sd);

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];
  Shadow_RAT* srat = rename->shadow_rat;

  /* Backpressure: if Node Stage didn't consume all ops from last cycle, stall.
   * Same pattern as main thread: map_stage.c stall = (last_sd->op_count > 0).
   * Prevents overwriting ops still in rename->sd without calling free_op(). */
  if (rename->sd.op_count > 0) {
    STAT_EVENT(proc_id, TEA_RENAME_STALL_DISPATCH);
    return;
  }

  /* Safe to clear — all previous ops were consumed by Node Stage */
  rename->sd.op_count = 0;

  /* Check if Shadow RAT is valid */
  if (!srat->is_valid) {
    return;
  }

  /* Phase 4.1: Check if TEA preg pool has enough resources before processing */
  if (tea_fetch_sd->op_count > 0 &&
      !tea_preg_pool_available(proc_id, tea_fetch_sd->op_count)) {
    /* Preg pool exhausted: terminate TEA thread entirely to break deadlock.
     * Stalling indefinitely would leave tea_op_count > 0 (stuck fetch ops)
     * so chains never terminate and the pool is never reset.
     * terminate_tea_thread() flushes all stages, resets preg pool, and allows
     * new TEA triggers in subsequent cycles. */
    STAT_EVENT(proc_id, TEA_RENAME_STALL_PREG);
    terminate_tea_thread(proc_id);
    return;
  }

  /* Rename all ops from TEA Fetch stage */
  for (uns i = 0; i < tea_fetch_sd->op_count; i++) {
    Op* op = tea_fetch_sd->ops[i];
    if (op) {
      tea_rename_op(proc_id, op);

      /* Add to output Stage_Data */
      ASSERT(proc_id, rename->sd.op_count < rename->sd.max_op_count);
      rename->sd.ops[rename->sd.op_count++] = op;

      /* CRITICAL: Clear source to transfer ownership and prevent double-free */
      tea_fetch_sd->ops[i] = NULL;
    }
  }
  tea_fetch_sd->op_count = 0;
}

/**************************************************************************************/
/* tea_add_src_dependency: TEA version of add_src_from_map_entry()
 * Difference: no op_num < consumer op_num ASSERT (cross-thread dependency) */

static void tea_add_src_dependency(Op* op, Op* src_op,
                                    Counter src_unique_num, Dep_Type type) {
  uns src_num = op->oracle_info.num_srcs++;
  ASSERT(op->proc_id, src_num < MAX_DEPS);

  Src_Info* info = &op->oracle_info.src_info[src_num];
  info->type = type;
  info->op = src_op;
  info->op_num = src_op->op_num;
  info->unique_num = src_unique_num;

  set_not_rdy_bit(op, src_num);
}

/**************************************************************************************/
/* tea_rename_op */

void tea_rename_op(uns proc_id, Op* op) {
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->thread_id == 1);  /* Verify this is a TEA op */

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];
  Shadow_RAT* srat = rename->shadow_rat;

  ASSERT(proc_id, srat->is_valid);

  Inst_Info* inst_info = op->inst_info;
  Table_Info* table_info = op->table_info;

  /* Initialize dependency counter */
  op->oracle_info.num_srcs = 0;

  /* === Source Registers — Shadow RAT read + dependency setup === */
  for (uns i = 0; i < table_info->num_src_regs; i++) {
    int arch_reg_id = inst_info->srcs[i].id;
    int reg_type = get_reg_type_for_rename(arch_reg_id);
    if (reg_type < 0) continue;

    /* (1) Physical register mapping (existing logic) */
    int phys_reg_id = shadow_rat_read_mapping(srat, arch_reg_id, reg_type);
    op->src_reg_id[i][REG_TABLE_TYPE_ARCHITECTURAL] = arch_reg_id;
    op->src_reg_id[i][REG_TABLE_TYPE_PHYSICAL] = phys_reg_id;

    /* (2) Producer Op lookup */
    Op* producer_op = NULL;
    Counter producer_unum = 0;

    if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
      producer_op = srat->gp_producer_ops[arch_reg_id];
      producer_unum = srat->gp_producer_unums[arch_reg_id];
    } else {
      int vec_idx = arch_reg_id - REG_ZMM0;
      producer_op = srat->vec_producer_ops[vec_idx];
      producer_unum = srat->vec_producer_unums[vec_idx];
    }

    /* (3) Register dependency if producer is still in-flight */
    if (producer_op && producer_op->op_pool_valid &&
        producer_op->unique_num == producer_unum) {
      tea_add_src_dependency(op, producer_op, producer_unum, REG_DATA_DEP);
    }
    /* else: producer already retired → immediately ready (no bit set) */
  }

  /* === Destination Registers — TEA preg alloc + Shadow RAT update === */
  for (uns i = 0; i < table_info->num_dest_regs; i++) {
    int arch_reg_id = inst_info->dests[i].id;
    int reg_type = get_reg_type_for_rename(arch_reg_id);
    if (reg_type < 0) continue;

    /* Save previous physical register mapping */
    int prev_phys_reg_id = shadow_rat_read_mapping(srat, arch_reg_id, reg_type);
    op->prev_dst_reg_id[i][REG_TABLE_TYPE_PHYSICAL] = prev_phys_reg_id;

    /* Allocate from TEA preg pool */
    Tea_Preg_Free_List* tea_pool =
      (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE)
        ? srat->tea_gp_preg_pool
        : srat->tea_vec_preg_pool;

    int new_phys_reg_id = tea_preg_pool_alloc(tea_pool);
    ASSERT(proc_id, new_phys_reg_id >= 0);
    STAT_EVENT(proc_id, TEA_PREGS_ALLOCATED);

    op->dst_reg_id[i][REG_TABLE_TYPE_ARCHITECTURAL] = arch_reg_id;
    op->dst_reg_id[i][REG_TABLE_TYPE_PHYSICAL] = new_phys_reg_id;

    /* Update Shadow RAT — phys mapping */
    shadow_rat_write_mapping(srat, arch_reg_id, new_phys_reg_id, reg_type);

    /* Update Shadow RAT — producer op (for intra-chain dependencies) */
    if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
      srat->gp_producer_ops[arch_reg_id] = op;
      srat->gp_producer_unums[arch_reg_id] = op->unique_num;
    } else {
      int vec_idx = arch_reg_id - REG_ZMM0;
      srat->vec_producer_ops[vec_idx] = op;
      srat->vec_producer_unums[vec_idx] = op->unique_num;
    }
  }

  /* === Register to wakeup lists ===
   * Uses cmp_wake as wake action (same as Main thread).
   * add_to_wake_up_lists() uses map_data->free_list_head for Wake_Up_Entry alloc.
   * map_data is valid: set_map_data() called before update_node_stage() in cmp_model.c.
   */
  extern void cmp_wake(Op*, Op*, uns8);
  add_to_wake_up_lists(op, &op->oracle_info, cmp_wake);
}

/**************************************************************************************/
/* recover_tea_rename_stage */

void recover_tea_rename_stage(uns proc_id) {
  reset_tea_rename_stage(proc_id);
}

/* recover_tea_rename_stage_by_chain: Free only ops from a specific chain
 * that are sitting in the rename stage's output buffer. */
void recover_tea_rename_stage_by_chain(uns proc_id, uns8 h2p_chain_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_rename_stages || !tea_rename_stages[proc_id]) {
    return;
  }

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];

  for (int i = 0; i < rename->sd.op_count; i++) {
    Op* op = rename->sd.ops[i];
    if (op && op->h2p_chain_id == h2p_chain_id) {
      free_op(op);
      rename->sd.ops[i] = NULL;
    }
  }

  /* Compact the ops array to remove NULLs */
  int write = 0;
  for (int i = 0; i < rename->sd.op_count; i++) {
    if (rename->sd.ops[i]) {
      rename->sd.ops[write++] = rename->sd.ops[i];
    }
  }
  rename->sd.op_count = write;
}

/**************************************************************************************/
/* Helper Functions */

/* get_reg_type_for_rename: Determine register type for renaming
 * Returns: REG_FILE_REG_TYPE_GENERAL_PURPOSE, REG_FILE_REG_TYPE_VECTOR, or -1
 * Based on map_rename.c:reg_file_get_reg_type()
 */
static int get_reg_type_for_rename(int reg_id) {
  /* GP registers: RAX-R15, TMP0-TMP4, ZPS-LAST_FLAG */
  if ((reg_id >= REG_RAX && reg_id < REG_CS) ||
      (reg_id >= REG_TMP0 && reg_id <= REG_TMP4) ||
      (reg_id >= REG_ZPS && reg_id < REG_ZMM0)) {
    return REG_FILE_REG_TYPE_GENERAL_PURPOSE;
  }

  /* Vector registers: ZMM0-ZMM31 */
  if (reg_id >= REG_ZMM0 && reg_id < REG_K0) {
    return REG_FILE_REG_TYPE_VECTOR;
  }

  /* Other registers (segment, control, etc.) - not renamed */
  return -1;
}

/* shadow_rat_read_mapping: Read arch->phys mapping from Shadow RAT */
static int shadow_rat_read_mapping(Shadow_RAT* srat, int arch_reg_id, int reg_type) {
  ASSERT(srat->proc_id, srat);

  if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
    if (arch_reg_id < (int)srat->gp_size) {
      return srat->gp_mappings[arch_reg_id];
    }
  } else if (reg_type == REG_FILE_REG_TYPE_VECTOR) {
    int vec_idx = arch_reg_id - REG_ZMM0;
    if (vec_idx >= 0 && vec_idx < (int)srat->vec_size) {
      return srat->vec_mappings[vec_idx];
    }
  }

  return REG_TABLE_REG_ID_INVALID;
}

/* shadow_rat_write_mapping: Update arch->phys mapping in Shadow RAT */
static void shadow_rat_write_mapping(Shadow_RAT* srat, int arch_reg_id, int phys_reg_id, int reg_type) {
  ASSERT(srat->proc_id, srat);

  if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
    if (arch_reg_id < (int)srat->gp_size) {
      srat->gp_mappings[arch_reg_id] = phys_reg_id;
    }
  } else if (reg_type == REG_FILE_REG_TYPE_VECTOR) {
    int vec_idx = arch_reg_id - REG_ZMM0;
    if (vec_idx >= 0 && vec_idx < (int)srat->vec_size) {
      srat->vec_mappings[vec_idx] = phys_reg_id;
    }
  }
}

/**************************************************************************************/
/* free_shadow_rat - Currently unused, will be used for cleanup in future phases */

#if 0  /* Commented out until needed */
static void free_shadow_rat(Shadow_RAT* srat) {
  if (srat) {
    if (srat->gp_mappings) {
      free(srat->gp_mappings);
    }
    if (srat->vec_mappings) {
      free(srat->vec_mappings);
    }
    /* Free lists will be handled in Phase 4 */
  }
}
#endif
