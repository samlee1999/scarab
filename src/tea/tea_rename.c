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
static void reset_shadow_rat(Shadow_RAT* srat);
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

  /* Allocate one Shadow RAT per chain slot */
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    rename->chain_srats[i] = (Shadow_RAT*)calloc(1, sizeof(Shadow_RAT));
    init_shadow_rat(proc_id, rename->chain_srats[i]);
  }
}

/**************************************************************************************/
/* tea_rename_stage_init_stage_data */

static void tea_rename_stage_init_stage_data(Tea_Rename_Stage* rename) {
  Stage_Data* sd = &rename->sd;

  sd->name = "TEA_RENAME";
  sd->max_op_count = TEA_FETCH_WIDTH;
  sd->op_count = 0;
  sd->ops = (Op**)calloc(TEA_FETCH_WIDTH, sizeof(Op*));
  ASSERT(rename->proc_id, sd->ops);
}

/**************************************************************************************/
/* init_shadow_rat: Allocate all arrays; PREG pool set later by init_tea_preg_pools */

static void init_shadow_rat(uns proc_id, Shadow_RAT* srat) {
  ASSERT(proc_id, srat);

  srat->proc_id = proc_id;

  srat->gp_size = NUM_REG_IDS;
  srat->vec_size = NUM_REG_IDS;

  srat->gp_mappings = (int*)calloc(srat->gp_size, sizeof(int));
  srat->vec_mappings = (int*)calloc(srat->vec_size, sizeof(int));

  for (uns i = 0; i < srat->gp_size; i++)
    srat->gp_mappings[i] = REG_TABLE_REG_ID_INVALID;
  for (uns i = 0; i < srat->vec_size; i++)
    srat->vec_mappings[i] = REG_TABLE_REG_ID_INVALID;

  srat->tea_gp_preg_pool = NULL;
  srat->tea_vec_preg_pool = NULL;
  srat->tea_gp_start_idx = 0;
  srat->tea_vec_start_idx = 0;

  srat->gp_producer_ops   = (Op**)calloc(srat->gp_size, sizeof(Op*));
  srat->gp_producer_unums = (Counter*)calloc(srat->gp_size, sizeof(Counter));
  srat->vec_producer_ops   = (Op**)calloc(srat->vec_size, sizeof(Op*));
  srat->vec_producer_unums = (Counter*)calloc(srat->vec_size, sizeof(Counter));

  srat->is_valid = FALSE;
}

/**************************************************************************************/
/* reset_shadow_rat: Clear mappings/producers and mark invalid (does NOT touch pool) */

static void reset_shadow_rat(Shadow_RAT* srat) {
  if (!srat) return;

  srat->is_valid = FALSE;

  for (uns i = 0; i < srat->gp_size; i++)
    srat->gp_mappings[i] = REG_TABLE_REG_ID_INVALID;
  for (uns i = 0; i < srat->vec_size; i++)
    srat->vec_mappings[i] = REG_TABLE_REG_ID_INVALID;

  memset(srat->gp_producer_ops,   0, srat->gp_size  * sizeof(Op*));
  memset(srat->gp_producer_unums, 0, srat->gp_size  * sizeof(Counter));
  memset(srat->vec_producer_ops,  0, srat->vec_size * sizeof(Op*));
  memset(srat->vec_producer_unums,0, srat->vec_size * sizeof(Counter));
}

/**************************************************************************************/
/* reset_tea_rename_stage: Full reset — all chain SRATs + Stage_Data */

void reset_tea_rename_stage(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];

  /* Invalidate all per-chain Shadow RATs */
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    reset_shadow_rat(rename->chain_srats[i]);
  }

  /* Free and clear Stage_Data */
  for (uns i = 0; i < rename->sd.max_op_count; i++) {
    if (rename->sd.ops[i]) {
      free_op(rename->sd.ops[i]);
      rename->sd.ops[i] = NULL;
    }
  }
  rename->sd.op_count = 0;
}

/**************************************************************************************/
/* shadow_rat_snapshot: Per-chain snapshot from Main RAT at trigger time */

void shadow_rat_snapshot(uns proc_id, int chain_slot) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);
  ASSERT(proc_id, tea_chain_slot_is_valid(proc_id, chain_slot));

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];
  Shadow_RAT* srat = rename->chain_srats[chain_slot];

  ASSERT(proc_id, srat);

  /* Copy GP register mappings from Main RAT */
  extern struct reg_file** reg_file;

  if (reg_file && reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]) {
    struct reg_table* arch_table =
      reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];

    if (arch_table && arch_table->entries) {
      for (uns i = 0; i < arch_table->size && i < srat->gp_size; i++) {
        srat->gp_mappings[i] = arch_table->entries[i].child_reg_id;
      }
    }
  }

  /* Copy VEC register mappings from Main RAT.
   * arch_table is indexed by absolute arch_reg_id (size=NUM_REG_IDS).
   * vec_mappings uses compact index (0=ZMM0) to match shadow_rat_read/write_mapping. */
  if (reg_file && reg_file[REG_FILE_REG_TYPE_VECTOR]) {
    struct reg_table* arch_table =
      reg_file[REG_FILE_REG_TYPE_VECTOR]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];

    if (arch_table && arch_table->entries) {
      for (int i = (int)REG_ZMM0; i < (int)REG_K0 && i < (int)arch_table->size; i++) {
        int compact_idx = i - (int)REG_ZMM0;
        if (compact_idx < (int)srat->vec_size)
          srat->vec_mappings[compact_idx] = arch_table->entries[i].child_reg_id;
      }
    }
  }

  /* Copy producer Op pointers from Main thread's reg_map (for cross-thread wakeup) */
  for (uns i = 0; i < NUM_REG_IDS; i++) {
    int reg_type = get_reg_type_for_rename(i);
    if (reg_type < 0) continue;

    uns ind = i << 1 | map_data->map_flags[i];
    Map_Entry* entry = &map_data->reg_map[ind];

    if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
      srat->gp_producer_ops[i]   = entry->op;
      srat->gp_producer_unums[i] = entry->unique_num;
    } else if (reg_type == REG_FILE_REG_TYPE_VECTOR) {
      int vec_idx = i - REG_ZMM0;
      if (vec_idx >= 0 && vec_idx < (int)srat->vec_size) {
        srat->vec_producer_ops[vec_idx]   = entry->op;
        srat->vec_producer_unums[vec_idx] = entry->unique_num;
      }
    }
  }

  srat->is_valid = TRUE;
}

/**************************************************************************************/
/* Phase 4: TEA Preg Pool Helper Functions */

static int tea_preg_pool_alloc(Tea_Preg_Free_List* pool) {
  if (!pool || pool->count == 0)
    return -1;

  int preg_idx = (int)pool->indices[pool->head];
  pool->head = (pool->head + 1) % pool->size;
  pool->count--;
  return preg_idx;
}

static Tea_Preg_Free_List* create_tea_preg_pool(int start_idx, int count) {
  Tea_Preg_Free_List* pool = (Tea_Preg_Free_List*)calloc(1, sizeof(Tea_Preg_Free_List));
  pool->size  = count;
  pool->count = count;
  pool->head  = 0;
  pool->tail  = 0;
  pool->indices = (uns*)calloc(count, sizeof(uns));

  for (int i = 0; i < count; i++)
    pool->indices[i] = start_idx + i;

  return pool;
}

static void remove_tea_entries_from_main_free_list(struct reg_table* phys_table,
                                                    int tea_start_idx) {
  struct reg_free_list* main_fl = phys_table->free_list;

  struct reg_table_entry* new_head = NULL;
  struct reg_table_entry** tail_ptr = &new_head;
  uns new_count = 0;

  struct reg_table_entry* entry = main_fl->reg_free_list_head;
  while (entry) {
    struct reg_table_entry* next = entry->next_free;

    if (entry->self_reg_id < tea_start_idx) {
      *tail_ptr = entry;
      tail_ptr = &entry->next_free;
      entry->next_free = NULL;
      new_count++;
    }

    entry = next;
  }

  main_fl->reg_free_list_head = new_head;
  main_fl->reg_free_num = new_count;
}

/**************************************************************************************/
/* init_tea_preg_pools: Partition TEA PREG space into per-chain sub-pools */

void init_tea_preg_pools(uns proc_id) {
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);

  extern struct reg_file** reg_file;
  ASSERT(proc_id, reg_file);

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];

  /* TEA_PREG_RESERVATION / TEA_MAX_CHAINS PREGs per active chain slot.
   * Must divide evenly; remainder would be removed from main list but wasted. */
  uns max_chains = tea_max_chains(proc_id);
  ASSERT(proc_id, TEA_PREG_RESERVATION > 0);
  ASSERT(proc_id, TEA_PREG_RESERVATION <= REG_TABLE_INTEGER_PHYSICAL_SIZE);
  ASSERT(proc_id, TEA_PREG_RESERVATION <= REG_TABLE_VECTOR_PHYSICAL_SIZE);
  ASSERT(proc_id, TEA_PREG_RESERVATION % max_chains == 0);
  uns per_chain = TEA_PREG_RESERVATION / max_chains;
  ASSERT(proc_id, per_chain > 0);

  int gp_base  = REG_TABLE_INTEGER_PHYSICAL_SIZE - TEA_PREG_RESERVATION;
  int vec_base = REG_TABLE_VECTOR_PHYSICAL_SIZE  - TEA_PREG_RESERVATION;

  /* GP pools */
  if (reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]) {
    struct reg_table* gp_phys =
      reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]->reg_table[REG_TABLE_TYPE_PHYSICAL];

    /* Remove all TEA GP indices from main free list (one contiguous block) */
    remove_tea_entries_from_main_free_list(gp_phys, gp_base);

    /* Create one sub-pool per runtime-active chain slot */
    for (uns i = 0; i < max_chains; i++) {
      Shadow_RAT* srat = rename->chain_srats[i];
      srat->tea_gp_start_idx = gp_base + i * (int)per_chain;
      srat->tea_gp_preg_pool = create_tea_preg_pool(srat->tea_gp_start_idx, per_chain);
    }
  }

  /* VEC pools */
  if (reg_file[REG_FILE_REG_TYPE_VECTOR]) {
    struct reg_table* vec_phys =
      reg_file[REG_FILE_REG_TYPE_VECTOR]->reg_table[REG_TABLE_TYPE_PHYSICAL];

    remove_tea_entries_from_main_free_list(vec_phys, vec_base);

    for (uns i = 0; i < max_chains; i++) {
      Shadow_RAT* srat = rename->chain_srats[i];
      srat->tea_vec_start_idx = vec_base + i * (int)per_chain;
      srat->tea_vec_preg_pool = create_tea_preg_pool(srat->tea_vec_start_idx, per_chain);
    }
  }
}

/**************************************************************************************/
/* tea_preg_pool_return_prev: Return a retired op's prev-mapping PREGs to the chain pool.
 * Standard RAT behavior: the old physical register (overwritten by this op's rename)
 * becomes free when the op that overwrote it retires.
 * Only TEA PREGs within this chain's sub-range are returned; the initial snapshot
 * mapping (main-thread PREGs) are skipped. */

void tea_preg_pool_return_prev(uns proc_id, int chain_slot, Op* op) {
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);
  ASSERT(proc_id, tea_chain_slot_is_valid(proc_id, chain_slot));
  ASSERT(proc_id, op);

  Shadow_RAT* srat = tea_rename_stages[proc_id]->chain_srats[chain_slot];
  if (!srat) return;

  Inst_Info*  inst_info  = op->inst_info;
  Table_Info* table_info = op->table_info;

  for (uns i = 0; i < table_info->num_dest_regs; i++) {
    int arch_reg_id = inst_info->dests[i].id;
    int reg_type    = get_reg_type_for_rename(arch_reg_id);
    if (reg_type < 0) continue;

    int prev_preg = op->prev_dst_reg_id[i][REG_TABLE_TYPE_PHYSICAL];
    if (prev_preg == REG_TABLE_REG_ID_INVALID) continue;

    if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE && srat->tea_gp_preg_pool) {
      Tea_Preg_Free_List* pool = srat->tea_gp_preg_pool;
      if (prev_preg >= srat->tea_gp_start_idx &&
          prev_preg < srat->tea_gp_start_idx + (int)pool->size) {
        ASSERT(proc_id, pool->count < pool->size);
        pool->indices[pool->tail] = (uns)prev_preg;
        pool->tail  = (pool->tail + 1) % pool->size;
        pool->count++;
      }
    } else if (reg_type == REG_FILE_REG_TYPE_VECTOR && srat->tea_vec_preg_pool) {
      Tea_Preg_Free_List* pool = srat->tea_vec_preg_pool;
      if (prev_preg >= srat->tea_vec_start_idx &&
          prev_preg < srat->tea_vec_start_idx + (int)pool->size) {
        ASSERT(proc_id, pool->count < pool->size);
        pool->indices[pool->tail] = (uns)prev_preg;
        pool->tail  = (pool->tail + 1) % pool->size;
        pool->count++;
      }
    }
  }
}

/**************************************************************************************/
/* reset_tea_preg_pool: Reset one chain slot's PREG pool to full */

void reset_tea_preg_pool(uns proc_id, int chain_slot) {
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);
  ASSERT(proc_id, tea_chain_slot_is_valid(proc_id, chain_slot));

  Shadow_RAT* srat = tea_rename_stages[proc_id]->chain_srats[chain_slot];

  if (srat->tea_gp_preg_pool) {
    Tea_Preg_Free_List* pool = srat->tea_gp_preg_pool;
    pool->head  = 0;
    pool->tail  = 0;
    pool->count = pool->size;
    for (uns i = 0; i < pool->size; i++)
      pool->indices[i] = srat->tea_gp_start_idx + i;
  }

  if (srat->tea_vec_preg_pool) {
    Tea_Preg_Free_List* pool = srat->tea_vec_preg_pool;
    pool->head  = 0;
    pool->tail  = 0;
    pool->count = pool->size;
    for (uns i = 0; i < pool->size; i++)
      pool->indices[i] = srat->tea_vec_start_idx + i;
  }
}

/**************************************************************************************/
/* tea_preg_pool_available: Check if chain_slot has enough PREGs for exact dest counts */

Flag tea_preg_pool_available(uns proc_id, int chain_slot,
                              uns required_gp, uns required_vec) {
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);
  ASSERT(proc_id, tea_chain_slot_is_valid(proc_id, chain_slot));

  Shadow_RAT* srat = tea_rename_stages[proc_id]->chain_srats[chain_slot];

  if (!srat || !srat->is_valid)
    return FALSE;

  Flag gp_ok  = (!srat->tea_gp_preg_pool  || srat->tea_gp_preg_pool->count  >= required_gp);
  Flag vec_ok = (!srat->tea_vec_preg_pool || srat->tea_vec_preg_pool->count >= required_vec);

  return gp_ok && vec_ok;
}

/**************************************************************************************/
/* update_tea_rename_stage */

void update_tea_rename_stage(uns proc_id, Stage_Data* tea_fetch_sd) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_rename_stages && tea_rename_stages[proc_id]);
  ASSERT(proc_id, tea_fetch_sd);

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];

  /* Backpressure: stall while Node Stage hasn't consumed previous output */
  if (rename->sd.op_count > 0) {
    STAT_EVENT(proc_id, TEA_RENAME_STALL_DISPATCH);
    return;
  }

  rename->sd.op_count = 0;

  /* PREG stall check: count exact GP/VEC dest regs in this batch */
  if (tea_fetch_sd->op_count > 0) {
    int chain_slot = -1;
    uns batch_ops = 0, required_gp = 0, required_vec = 0;
    for (uns i = 0; i < (uns)tea_fetch_sd->max_op_count; i++) {
      Op* op = tea_fetch_sd->ops[i];
      if (!op) continue;
      batch_ops++;
      if (chain_slot < 0) chain_slot = (int)op->h2p_chain_id - 1;
      for (uns j = 0; j < op->table_info->num_dest_regs; j++) {
        int rtype = get_reg_type_for_rename(op->inst_info->dests[j].id);
        if (rtype == REG_FILE_REG_TYPE_GENERAL_PURPOSE) required_gp++;
        else if (rtype == REG_FILE_REG_TYPE_VECTOR)     required_vec++;
      }
    }

    if (chain_slot >= 0) {
      STAT_EVENT(proc_id, TEA_RENAME_BATCHES);
      INC_STAT_EVENT(proc_id, TEA_RENAME_BATCH_OPS_TOTAL, batch_ops);
      INC_STAT_EVENT(proc_id, TEA_RENAME_BATCH_GP_DESTS_TOTAL, required_gp);
      INC_STAT_EVENT(proc_id, TEA_RENAME_BATCH_VEC_DESTS_TOTAL, required_vec);

      Shadow_RAT* srat = rename->chain_srats[chain_slot];
      if (!srat || !srat->is_valid) {
        /* Chain was terminated while ops were in-flight in tf->sd.
         * This should only happen after terminate_tea_chain() has run
         * (chain INACTIVE), meaning these ops are truly orphaned. */
        Tea_Thread* tea = tea_threads[proc_id];
        ASSERT(proc_id, tea->chains[chain_slot].state == CHAIN_INACTIVE);
        for (uns i = 0; i < (uns)tea_fetch_sd->max_op_count; i++) {
          if (tea_fetch_sd->ops[i]) {
            free_op(tea_fetch_sd->ops[i]);
            tea_fetch_sd->ops[i] = NULL;
          }
        }
        tea_fetch_sd->op_count = 0;
        STAT_EVENT(proc_id, TEA_RENAME_ORPHANED_BATCHES);
        return;
      }

      if (!tea_preg_pool_available(proc_id, chain_slot, required_gp, required_vec)) {
        STAT_EVENT(proc_id, TEA_RENAME_STALL_PREG);
        INC_STAT_EVENT(proc_id, TEA_RENAME_STALL_PREG_BATCH_OPS_TOTAL, batch_ops);
        INC_STAT_EVENT(proc_id, TEA_RENAME_STALL_PREG_GP_REQ_TOTAL, required_gp);
        INC_STAT_EVENT(proc_id, TEA_RENAME_STALL_PREG_VEC_REQ_TOTAL, required_vec);
        return;
      }
    }
  }

  /* Rename all ops from TEA Fetch stage */
  for (uns i = 0; i < tea_fetch_sd->op_count; i++) {
    Op* op = tea_fetch_sd->ops[i];
    if (op) {
      tea_rename_op(proc_id, op);

      ASSERT(proc_id, rename->sd.op_count < rename->sd.max_op_count);
      rename->sd.ops[rename->sd.op_count++] = op;

      tea_fetch_sd->ops[i] = NULL;
    }
  }
  tea_fetch_sd->op_count = 0;
}

/**************************************************************************************/
/* tea_add_src_dependency */

static void tea_add_src_dependency(Op* op, Op* src_op,
                                    Counter src_unique_num, Dep_Type type) {
  uns src_num = op->oracle_info.num_srcs++;
  ASSERT(op->proc_id, src_num < MAX_DEPS);

  Src_Info* info = &op->oracle_info.src_info[src_num];
  info->type       = type;
  info->op         = src_op;
  info->op_num     = src_op->op_num;
  info->unique_num = src_unique_num;

  set_not_rdy_bit(op, src_num);
}

/**************************************************************************************/
/* tea_rename_op: Rename one TEA op using its chain's Shadow RAT */

void tea_rename_op(uns proc_id, Op* op) {
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->thread_id == 1);

  int slot = (int)op->h2p_chain_id - 1;
  ASSERT(proc_id, tea_chain_slot_is_valid(proc_id, slot));

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];
  Shadow_RAT* srat = rename->chain_srats[slot];

  ASSERT(proc_id, srat->is_valid);

  Inst_Info*  inst_info  = op->inst_info;
  Table_Info* table_info = op->table_info;

  op->oracle_info.num_srcs = 0;

  /* Source Registers: read from this chain's Shadow RAT */
  for (uns i = 0; i < table_info->num_src_regs; i++) {
    int arch_reg_id = inst_info->srcs[i].id;
    int reg_type    = get_reg_type_for_rename(arch_reg_id);
    if (reg_type < 0) continue;

    int phys_reg_id = shadow_rat_read_mapping(srat, arch_reg_id, reg_type);
    op->src_reg_id[i][REG_TABLE_TYPE_ARCHITECTURAL] = arch_reg_id;
    op->src_reg_id[i][REG_TABLE_TYPE_PHYSICAL]      = phys_reg_id;

    Op*     producer_op   = NULL;
    Counter producer_unum = 0;

    if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
      producer_op   = srat->gp_producer_ops[arch_reg_id];
      producer_unum = srat->gp_producer_unums[arch_reg_id];
    } else {
      int vec_idx   = arch_reg_id - REG_ZMM0;
      producer_op   = srat->vec_producer_ops[vec_idx];
      producer_unum = srat->vec_producer_unums[vec_idx];
    }

    if (producer_op && producer_op->op_pool_valid &&
        producer_op->unique_num == producer_unum) {
      tea_add_src_dependency(op, producer_op, producer_unum, REG_DATA_DEP);
    }
  }

  /* Destination Registers: allocate from this chain's PREG pool */
  for (uns i = 0; i < table_info->num_dest_regs; i++) {
    int arch_reg_id = inst_info->dests[i].id;
    int reg_type    = get_reg_type_for_rename(arch_reg_id);
    if (reg_type < 0) continue;

    int prev_phys_reg_id = shadow_rat_read_mapping(srat, arch_reg_id, reg_type);
    op->prev_dst_reg_id[i][REG_TABLE_TYPE_PHYSICAL] = prev_phys_reg_id;

    Tea_Preg_Free_List* tea_pool =
      (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE)
        ? srat->tea_gp_preg_pool
        : srat->tea_vec_preg_pool;

    int new_phys_reg_id = tea_preg_pool_alloc(tea_pool);
    ASSERT(proc_id, new_phys_reg_id >= 0);
    STAT_EVENT(proc_id, TEA_PREGS_ALLOCATED);

    op->dst_reg_id[i][REG_TABLE_TYPE_ARCHITECTURAL] = arch_reg_id;
    op->dst_reg_id[i][REG_TABLE_TYPE_PHYSICAL]      = new_phys_reg_id;

    shadow_rat_write_mapping(srat, arch_reg_id, new_phys_reg_id, reg_type);

    if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
      srat->gp_producer_ops[arch_reg_id]   = op;
      srat->gp_producer_unums[arch_reg_id] = op->unique_num;
    } else {
      int vec_idx = arch_reg_id - REG_ZMM0;
      srat->vec_producer_ops[vec_idx]   = op;
      srat->vec_producer_unums[vec_idx] = op->unique_num;
    }
  }

  /* Register wakeup lists */
  extern void cmp_wake(Op*, Op*, uns8);
  add_to_wake_up_lists(op, &op->oracle_info, cmp_wake);
}

/**************************************************************************************/
/* recover_tea_rename_stage: Full flush — invalidate all chain SRATs + free SD ops */

void recover_tea_rename_stage(uns proc_id) {
  reset_tea_rename_stage(proc_id);
}

/**************************************************************************************/
/* recover_tea_rename_stage_by_chain: Per-chain flush */

void recover_tea_rename_stage_by_chain(uns proc_id, uns8 chain_id) {
  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];
  if (!rename) return;

  /* Free ops in rename SD belonging to this chain */
  for (uns i = 0; i < rename->sd.max_op_count; i++) {
    Op* op = rename->sd.ops[i];
    if (op && op->h2p_chain_id == chain_id) {
      free_op(op);
      rename->sd.ops[i] = NULL;
      if (rename->sd.op_count > 0)
        rename->sd.op_count--;
    }
  }

  /* Invalidate this chain's Shadow RAT (PREG pool reset done separately) */
  int slot = (int)chain_id - 1;
  if (tea_chain_slot_is_valid(proc_id, slot))
    reset_shadow_rat(rename->chain_srats[slot]);
}

/**************************************************************************************/
/* Helper Functions */

static int get_reg_type_for_rename(int reg_id) {
  if ((reg_id >= REG_RAX && reg_id < REG_CS) ||
      (reg_id >= REG_TMP0 && reg_id <= REG_TMP4) ||
      (reg_id >= REG_ZPS && reg_id < REG_ZMM0)) {
    return REG_FILE_REG_TYPE_GENERAL_PURPOSE;
  }

  if (reg_id >= REG_ZMM0 && reg_id < REG_K0) {
    return REG_FILE_REG_TYPE_VECTOR;
  }

  return -1;
}

static int shadow_rat_read_mapping(Shadow_RAT* srat, int arch_reg_id, int reg_type) {
  ASSERT(srat->proc_id, srat);

  if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
    if (arch_reg_id < (int)srat->gp_size)
      return srat->gp_mappings[arch_reg_id];
  } else if (reg_type == REG_FILE_REG_TYPE_VECTOR) {
    int vec_idx = arch_reg_id - REG_ZMM0;
    if (vec_idx >= 0 && vec_idx < (int)srat->vec_size)
      return srat->vec_mappings[vec_idx];
  }

  return REG_TABLE_REG_ID_INVALID;
}

static void shadow_rat_write_mapping(Shadow_RAT* srat, int arch_reg_id,
                                      int phys_reg_id, int reg_type) {
  ASSERT(srat->proc_id, srat);

  if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
    if (arch_reg_id < (int)srat->gp_size)
      srat->gp_mappings[arch_reg_id] = phys_reg_id;
  } else if (reg_type == REG_FILE_REG_TYPE_VECTOR) {
    int vec_idx = arch_reg_id - REG_ZMM0;
    if (vec_idx >= 0 && vec_idx < (int)srat->vec_size)
      srat->vec_mappings[vec_idx] = phys_reg_id;
  }
}
