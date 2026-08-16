/*
 * Copyright 2020 HPS/SAFARI Research Groups
 * Copyright 2025 Litz Lab
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
 * File         : dcache_stage.c
 * Author       : HPS Research Group, Litz Lab
 * Date         : 3/8/1999, 4/15/2025
 * Description  :
 ***************************************************************************************/

#include "dcache_stage.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "core.param.h"
#include "libs/hash_lib.h"
#include "memory/memory.param.h"
#include "memory/memory.h"
#include "prefetcher//stream.param.h"
#include "prefetcher/pref.param.h"

#include "bp/bp.h"
#include "prefetcher/l2l1pref.h"
#include "prefetcher/pref_common.h"
#include "prefetcher/stream_pref.h"

#include "cmp_model.h"
#include "map.h"
#include "model.h"
#include "statistics.h"

/* Phase 4.1: TEA Store Buffer */
#include "tea/tea_store_buffer.h"
#include "tea/tea_thread.h"
#include "zereco/rfp.h"

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DCACHE_STAGE, ##args)
#define STAGE_MAX_OP_COUNT NUM_FUS

/**************************************************************************************/
/* Global Variables */

Dcache_Stage* dc = NULL;

#define H2P_CHAIN_LOAD_PROFILE_PC_TABLE_SIZE 32768
#define H2P_CHAIN_LOAD_PROFILE_SLOT_TABLE_SIZE 65536
#define H2P_CHAIN_LOAD_PROFILE_STRIDE_SLOTS 8
#define H2P_CHAIN_LOAD_PATTERN_TOPK 8
#define H2P_CHAIN_LOAD_PATTERN_SINGLE_STRIDE_PCT 90.0
#define H2P_CHAIN_LOAD_PATTERN_MULTI_STRIDE_PCT 90.0
#define H2P_CHAIN_LOAD_PATTERN_CONSTANT_PCT 95.0
#define H2P_CHAIN_LOAD_REPEATABILITY_HIST_BUCKETS 8192

typedef enum H2P_Chain_Load_Profile_Result_enum {
  H2P_CHAIN_LOAD_PROFILE_RESULT_DCACHE_HIT = 0,
  H2P_CHAIN_LOAD_PROFILE_RESULT_STORE_REQ_BUFFER_HIT,
  H2P_CHAIN_LOAD_PROFILE_RESULT_PREFETCH_MSHR_HIT,
  H2P_CHAIN_LOAD_PROFILE_RESULT_DEMAND_MSHR_HIT,
  H2P_CHAIN_LOAD_PROFILE_RESULT_MLC_HIT,
  H2P_CHAIN_LOAD_PROFILE_RESULT_SCARAB_L1_HIT,
  H2P_CHAIN_LOAD_PROFILE_RESULT_MEM_ACCESS,
} H2P_Chain_Load_Profile_Result;

typedef struct H2P_Chain_Load_Profile_Summary_Entry_struct {
  Flag valid;
  Addr block_start_pc;
  uns block_op_idx;
  Addr load_pc;

  Counter accesses;
  Counter dcache_hit;
  Counter store_req_buffer_hit;
  Counter dcache_miss;
  Counter prefetch_mshr_hit;
  Counter demand_mshr_hit;
  Counter mlc_hit;
  Counter scarab_l1_hit;
  Counter mem_access;
  Counter latency_samples;
  Counter latency_total;

  Flag has_last_addr;
  Addr last_va;
  Addr last_line_addr;
  Counter reuse;
  Counter same_va;
  Counter same_va_line;
  Counter stride_zero;
  Counter stride_pos_1;
  Counter stride_neg_1;
  Counter stride_small_abs_le_4;
  Counter stride_small_abs_le_16;
  Counter stride_other;

  Flag stride_valid[H2P_CHAIN_LOAD_PROFILE_STRIDE_SLOTS];
  SCounter stride_value[H2P_CHAIN_LOAD_PROFILE_STRIDE_SLOTS];
  Counter stride_count[H2P_CHAIN_LOAD_PROFILE_STRIDE_SLOTS];

  Counter unique_vaddr;
  Counter unique_line;
  Flag byte_delta_valid[H2P_CHAIN_LOAD_PATTERN_TOPK];
  SCounter byte_delta_value[H2P_CHAIN_LOAD_PATTERN_TOPK];
  Counter byte_delta_count[H2P_CHAIN_LOAD_PATTERN_TOPK];
  Flag line_delta_valid[H2P_CHAIN_LOAD_PATTERN_TOPK];
  SCounter line_delta_value[H2P_CHAIN_LOAD_PATTERN_TOPK];
  Counter line_delta_count[H2P_CHAIN_LOAD_PATTERN_TOPK];
} H2P_Chain_Load_Profile_Summary_Entry;

typedef struct H2P_Chain_Load_Pattern_Value_Key_struct {
  Addr load_pc;
  Addr value;
  Counter count;
} H2P_Chain_Load_Pattern_Value_Key;

typedef struct H2P_Chain_Load_Pattern_Delta_Key_struct {
  Addr load_pc;
  SCounter delta;
  Counter count;
} H2P_Chain_Load_Pattern_Delta_Key;

typedef struct H2P_Chain_Load_Pattern_Top_Entry_struct {
  Flag valid;
  SCounter delta;
  Counter count;
} H2P_Chain_Load_Pattern_Top_Entry;

typedef struct H2P_Chain_Load_Addr_Repeatability_Entry_struct {
  Counter repeatability;
  Counter target_load_accesses;
  Counter unique_pc_vaddr_count;
} H2P_Chain_Load_Addr_Repeatability_Entry;

static H2P_Chain_Load_Profile_Summary_Entry*
  h2p_chain_load_pc_profile_table[MAX_NUM_PROCS];
static H2P_Chain_Load_Profile_Summary_Entry*
  h2p_chain_load_slot_profile_table[MAX_NUM_PROCS];
static Counter h2p_chain_load_pc_profile_overflow[MAX_NUM_PROCS];
static Counter h2p_chain_load_slot_profile_overflow[MAX_NUM_PROCS];
static Flag h2p_chain_load_profile_dump_registered = FALSE;
static FILE* h2p_chain_load_raw_stream_file = NULL;
static Counter h2p_chain_load_raw_stream_seq = 0;

static Hash_Table h2p_chain_load_unique_vaddr_table[MAX_NUM_PROCS];
static Hash_Table h2p_chain_load_unique_line_table[MAX_NUM_PROCS];
static Hash_Table h2p_chain_load_byte_delta_table[MAX_NUM_PROCS];
static Hash_Table h2p_chain_load_line_delta_table[MAX_NUM_PROCS];
static Flag h2p_chain_load_unique_vaddr_table_valid[MAX_NUM_PROCS];
static Flag h2p_chain_load_unique_line_table_valid[MAX_NUM_PROCS];
static Flag h2p_chain_load_byte_delta_table_valid[MAX_NUM_PROCS];
static Flag h2p_chain_load_line_delta_table_valid[MAX_NUM_PROCS];

/* Online per-PC address predictor that gates the H2P-chain load oracle
   (H2P_CHAIN_ORACLE_PREDICTOR: 1=stride, 2=top-delta). A chain load is
   idealized only when the predictor would have produced its address, so the
   IPC gain measures the fraction of the perfect-load upper bound that a real
   address predictor can recover. Mirrors src/tools/h2p_chain_load_predictor_
   replay.py so sim coverage can be cross-checked against the offline replay. */
#define H2P_ORACLE_TOP_DELTA_SLOTS 16
typedef struct H2P_Oracle_Pred_Entry_struct {
  Flag    has_last;
  int64   last;
  Flag    has_stride;
  int64   stride;
  uns     conf;
  uns     n_deltas;
  int64   delta_val[H2P_ORACLE_TOP_DELTA_SLOTS];
  Counter delta_cnt[H2P_ORACLE_TOP_DELTA_SLOTS];
} H2P_Oracle_Pred_Entry;

static Hash_Table h2p_oracle_pred_table[MAX_NUM_PROCS];
static Flag       h2p_oracle_pred_table_valid[MAX_NUM_PROCS];

/**************************************************************************************/
/* Prototypes for Inline Methods */

static inline Flag dcache_stage_addr_unready(Op* op);
static inline Flag dcache_stage_check_mem_type(Op* op);
static inline Flag dcache_stage_try_main_chain_load_oracle(Op* op);
static inline Flag dcache_stage_main_onpath_load(Op* op);
static inline Flag dcache_stage_main_chain_load_target_op(Op* op);
static inline Flag dcache_stage_main_chain_load_profile_op(Op* op);
static inline void dcache_stage_record_main_onpath_load_access(Op* op);
static inline void dcache_stage_record_main_chain_load_raw_stream_access(
  Op* op, Addr line_addr, Flag first_access);
static inline void dcache_stage_record_main_chain_load_profile_access(Op* op, Addr line_addr, Flag first_access);
static inline void dcache_stage_record_main_chain_load_profile_dcache_hit(Op* op);
static inline void dcache_stage_record_main_chain_load_profile_store_fwd(Op* op);
static inline void dcache_stage_record_main_chain_load_profile_fill(Op* op, Mem_Req* req);
static inline void h2p_chain_load_profile_record_access(Op* op, Addr line_addr);
static inline void h2p_chain_load_profile_record_result(
  Op* op, H2P_Chain_Load_Profile_Result result);
static inline void h2p_chain_load_pattern_record_pc_access(
  H2P_Chain_Load_Profile_Summary_Entry* entry, Op* op, Addr line_addr);
static void dump_h2p_chain_load_profile_tables(void);
static inline void dcache_stage_remove_src_op(Stage_Data* src_sd, int ii);
static inline int dcache_stage_count_valid_ops(void);
static inline void dcache_stage_assert_occupancy(const char* context);
static inline void dcache_stage_preserve_unprocessed_ops_after_full_tea_flush(Counter last_processed_op_num);

static inline void dcache_cacheline_hit(Op* op, Addr line_addr, Dcache_Data* line);
static inline void dcache_cacheline_miss(Op* op, Addr line_addr);

static inline void dcache_fill_wp_collect_stats(Dcache_Data* line, Mem_Req* req);
static inline void dcache_hit_wp_collect_stats(Dcache_Data* line, Op* op);
static inline Flag dcache_miss_new_mem_req(Op* op, Addr line_addr, Mem_Req_Type mem_req_type);
static inline void dcache_miss_extra_access(Op* op, Cache* cache, Addr line_addr, uns8 proc_id, uns8 cache_cycle);

static inline Dcache_Data* dcache_fill_get_cacheline(Mem_Req* req);
static inline void dcache_fill_process_cacheline(Mem_Req* req, Dcache_Data* data);

/**************************************************************************************/
/* External Interfaces for CMP Model */

void set_dcache_stage(Dcache_Stage* new_dc) {
  dc = new_dc;
}

void init_dcache_stage(uns8 proc_id, const char* name) {
  DEBUG(proc_id, "Initializing %s stage\n", name);

  ASSERT(0, dc);
  memset(dc, 0, sizeof(Dcache_Stage));

  dc->proc_id = proc_id;
  dc->sd.name = (char*)strdup(name);
  dc->sd.max_op_count = STAGE_MAX_OP_COUNT;
  dc->sd.ops = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);

  /* initialize the cache structure */
  init_cache(&dc->dcache, "DCACHE", DCACHE_SIZE, DCACHE_ASSOC, DCACHE_LINE_SIZE, sizeof(Dcache_Data), DCACHE_REPL);
  reset_dcache_stage();

  dc->ports = (Ports*)malloc(sizeof(Ports) * DCACHE_BANKS);
  for (uns ii = 0; ii < DCACHE_BANKS; ii++) {
    char name[MAX_STR_LENGTH + 1];
    snprintf(name, MAX_STR_LENGTH, "DCACHE BANK %d PORTS", ii);
    init_ports(&dc->ports[ii], name, DCACHE_READ_PORTS, DCACHE_WRITE_PORTS, FALSE);
  }

  dc->dcache.repl_pref_thresh = DCACHE_REPL_PREF_THRESH;

  if (DC_PREF_CACHE_ENABLE)
    init_cache(&dc->pref_dcache, "DC_PREF_CACHE", DC_PREF_CACHE_SIZE, DC_PREF_CACHE_ASSOC, DCACHE_LINE_SIZE,
               sizeof(Dcache_Data), DCACHE_REPL);

  memset(dc->rand_wb_state, 0, NUM_ELEMENTS(dc->rand_wb_state));
}

void reset_dcache_stage(void) {
  uns ii;
  for (ii = 0; ii < STAGE_MAX_OP_COUNT; ii++)
    dc->sd.ops[ii] = NULL;
  dc->sd.op_count = 0;
  dc->idle_cycle = 0;
}

void reset_h2p_chain_load_profile_tables(void) {
  for (uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    if (h2p_chain_load_pc_profile_table[proc_id]) {
      memset(h2p_chain_load_pc_profile_table[proc_id], 0,
             sizeof(H2P_Chain_Load_Profile_Summary_Entry) *
               H2P_CHAIN_LOAD_PROFILE_PC_TABLE_SIZE);
    }
    if (h2p_chain_load_slot_profile_table[proc_id]) {
      memset(h2p_chain_load_slot_profile_table[proc_id], 0,
             sizeof(H2P_Chain_Load_Profile_Summary_Entry) *
               H2P_CHAIN_LOAD_PROFILE_SLOT_TABLE_SIZE);
    }
    h2p_chain_load_pc_profile_overflow[proc_id] = 0;
    h2p_chain_load_slot_profile_overflow[proc_id] = 0;
    if (h2p_chain_load_unique_vaddr_table_valid[proc_id])
      hash_table_clear(&h2p_chain_load_unique_vaddr_table[proc_id]);
    if (h2p_chain_load_unique_line_table_valid[proc_id])
      hash_table_clear(&h2p_chain_load_unique_line_table[proc_id]);
    if (h2p_chain_load_byte_delta_table_valid[proc_id])
      hash_table_clear(&h2p_chain_load_byte_delta_table[proc_id]);
    if (h2p_chain_load_line_delta_table_valid[proc_id])
      hash_table_clear(&h2p_chain_load_line_delta_table[proc_id]);
  }
}

void recover_dcache_stage() {
  uns ii;
  for (ii = 0; ii < NUM_FUS; ii++) {
    Op* op = dc->sd.ops[ii];
    if (!op) continue;
    /* TEA ops are managed by flush_tea_ops_by_chain_id() via terminate_tea_chain(). */
    if (TEA_ENABLE && op->thread_id == 1) continue;
    if (op->op_num > bp_recovery_info->recovery_op_num) {
      dc->sd.ops[ii] = NULL;
      dc->sd.op_count--;
    }
  }
  dc->idle_cycle = cycle_count + 1;
}

void debug_dcache_stage() {
  DPRINTF("# %-10s  op_count:%d  busy: %d\n", dc->sd.name, dc->sd.op_count, dc->idle_cycle > cycle_count);
  print_op_array(GLOBAL_DEBUG_STREAM, dc->sd.ops, STAGE_MAX_OP_COUNT, STAGE_MAX_OP_COUNT);
}

void update_dcache_stage(Stage_Data* src_sd) {
  /* phase 1 - move ops into the dcache stage */
  ASSERT(dc->proc_id, src_sd->max_op_count == dc->sd.max_op_count);
  for (uns ii = 0; ii < src_sd->max_op_count; ii++) {
    Op* op = src_sd->ops[ii];
    Op* dc_op = dc->sd.ops[ii];

    // op just got told to replay this cycle (clobber it)
    if (op && cycle_count < op->rdy_cycle) {
      ASSERTM(dc->proc_id, op->replay, "o:%s  rdy:%s", unsstr64(op->op_num), unsstr64(op->rdy_cycle));
      dcache_stage_remove_src_op(src_sd, ii);
      op = NULL;
    }

    /* check if the op in the dcache_stage is stall */
    if (dc_op) {
      if (dc_op->state == OS_WAIT_DCACHE || (STALL_ON_WAIT_MEM && dc_op->state == OS_WAIT_MEM)) {
        ASSERT(dc->proc_id, cycle_count >= dc->sd.ops[ii]->exec_cycle);
        continue;
      }

      // unless the op stalled getting a dcache port, it's gone
      dc->sd.ops[ii] = NULL;
      dc->sd.op_count--;
      ASSERT(dc->proc_id, dc->sd.op_count >= 0);
    }

    /* check if the op from the src_stage is ready */
    if (!op)
      continue;

    // not ready due to address generation latency
    if (dcache_stage_addr_unready(op)) {
      continue;
    }

    // squash non-memory and prefetch (when software prefetching is not enabled) ops
    if (!dcache_stage_check_mem_type(op)) {
      dcache_stage_remove_src_op(src_sd, ii);
      continue;
    }

    /* if the op is valid, move it into the dcache stage */
    dc->sd.ops[ii] = op;
    dc->sd.op_count++;
    ASSERT(dc->proc_id, dc->sd.op_count <= dc->sd.max_op_count);
    dcache_stage_remove_src_op(src_sd, ii);
    ASSERTM(dc->proc_id, cycle_count >= op->exec_cycle, "o:%s  %s\n", unsstr64(op->op_num), Op_State_str(op->state));
  }
  dcache_stage_assert_occupancy("phase1");

  /* Prefetch-first arbitration (RFP_PORT_PRIORITY 2) probes here, ahead of the
     demand loop; every other policy waits until the end of the cycle. */
  rfp_queue_drain(dc->proc_id, dc, TRUE);

  /* phase 2 - check the dcache port availability and do dcache access */
  int start_op_count = dc->sd.op_count;
  Counter last_oldest_op_num = 0;
  for (uns ii = 0; ii < start_op_count; ii++) {
    /* update in program order (make things easier) */
    uns oldest_index = 0;
    Counter oldest_op_num = MAX_CTR;
    // TODO: adjust this O(n2) algorithm by getting the program order outside first
    for (uns jj = 0; jj < dc->sd.max_op_count; jj++) {
      if (dc->sd.ops[jj] && dc->sd.ops[jj]->op_num > last_oldest_op_num && dc->sd.ops[jj]->op_num < oldest_op_num) {
        oldest_op_num = dc->sd.ops[jj]->op_num;
        oldest_index = jj;
      }
    }
    last_oldest_op_num = oldest_op_num;

    ASSERT(dc->proc_id, oldest_op_num < MAX_CTR);
    Op* op = dc->sd.ops[oldest_index];

    // if the op is replaying, squish it
    if (op->replay && op->exec_cycle == MAX_CTR) {
      dc->sd.ops[oldest_index] = NULL;
      dc->sd.op_count--;
      ASSERT(dc->proc_id, dc->sd.op_count >= 0);
      continue;
    }

    /* Phase 4.1: Intercept TEA memory operations - use store buffer instead of D-cache */
    if (TEA_ENABLE && op->thread_id == 1) {
      if (op->table_info->mem_type == MEM_ST) {
        /* TEA Store: Write to buffer, not D-cache */
        if (!tea_store_buffer_write(op->proc_id, op->oracle_info.va,
                                    op->oracle_info.new_mem_value,
                                    op->oracle_info.mem_size,
                                    op->h2p_chain_id)) {
          Counter flushed_op_num = op->op_num;
          /* Buffer full: terminate TEA thread immediately.
           * terminate_tea_thread() owns dcache-stage TEA op removal and may
           * free this op through the node table.  The local start_op_count
           * snapshot is no longer valid after that pipeline squash, so preserve
           * unprocessed surviving ops for the next cycle and stop this update. */
          STAT_EVENT(op->proc_id, TEA_STORE_BUFFER_FULL);
          terminate_tea_thread(op->proc_id);
          dcache_stage_preserve_unprocessed_ops_after_full_tea_flush(flushed_op_num);
          dcache_stage_assert_occupancy("TEA store-buffer-full flush");
          return;
        }
        STAT_EVENT(op->proc_id, TEA_STORES_BUFFERED);
        op->done_cycle = cycle_count + DCACHE_CYCLES;
        op->state = OS_SCHEDULED;
      } else if (op->table_info->mem_type == MEM_LD) {
        /* TEA Load: Check buffer first, then fall through to D-cache */
        Quad forwarded_data;
        if (tea_store_buffer_read(op->proc_id, op->oracle_info.va,
                                  op->oracle_info.mem_size, &forwarded_data)) {
          /* Forwarding hit from TEA store buffer */
          op->done_cycle = cycle_count + DCACHE_CYCLES;
          op->wake_cycle = cycle_count + DCACHE_CYCLES;
          op->state = OS_SCHEDULED;
          STAT_EVENT(op->proc_id, TEA_STORE_FORWARDS);
          /* Wake up dependent TEA ops waiting on this load's data */
          wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
          /* [EXPERIMENT: TEA_PERFECT_LOAD] store-forward hit stats */
          STAT_EVENT(op->proc_id, TEA_LOADS_EXECUTED);
          STAT_EVENT(op->proc_id, TEA_LOADS_STORE_FORWARD);
          STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_SAMPLES);
          INC_STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_TOTAL, DCACHE_CYCLES);
          INC_STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_AVG,   DCACHE_CYCLES);
          tea_chain_note_load_result(op->proc_id, op,
                                     TEA_LOAD_RESULT_STORE_FORWARD,
                                     DCACHE_CYCLES);
          tea_log_chain_load_addr(op, MAX_CTR, "STORE_FORWARD",
                                  DCACHE_CYCLES);
        } else {
          /* [EXPERIMENT: TEA_PERFECT_LOAD] original: goto tea_load_dcache_access; */
          if (TEA_PERFECT_LOAD) {
            /* Ideal upper bound: bypass real dcache/memory, apply static L1 latency */
            Counter latency = TEA_PERFECT_LOAD_LATENCY ? TEA_PERFECT_LOAD_LATENCY : DCACHE_CYCLES;
            op->done_cycle = cycle_count + latency;
            op->wake_cycle = cycle_count + latency;
            op->state = OS_SCHEDULED;
            op->oracle_info.dcmiss = FALSE;
            wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
            STAT_EVENT(op->proc_id, TEA_LOADS_EXECUTED);
            STAT_EVENT(op->proc_id, TEA_LOADS_BYPASSED);
            STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_SAMPLES);
            INC_STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_TOTAL, latency);
            INC_STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_AVG,   latency);
            tea_chain_note_load_result(op->proc_id, op,
                                       TEA_LOAD_RESULT_BYPASS,
                                       latency);
            tea_log_chain_load_addr(op, MAX_CTR, "PERFECT_LOAD_BYPASS",
                                    latency);
            /* falls through to tea_op_completed + stage removal below */
          } else {
            goto tea_load_dcache_access;
          }
        }
      }
      /* Mark TEA memory op as completed (decrement tea_op_count) */
      tea_op_completed(dc->proc_id, op);
      /* Remove TEA op from dcache stage after processing */
      dc->sd.ops[oldest_index] = NULL;
      dc->sd.op_count--;
      ASSERT(dc->proc_id, dc->sd.op_count >= 0);
      continue;
    }
tea_load_dcache_access:
    ;  /* Empty statement required after label in C */

    if (dcache_stage_try_main_chain_load_oracle(op))
      continue;

    /* Validate ahead of the port check: a covered load consumed its L1 access
       when the prefetch probed, so it must not take a port now. */
    if (rfp_try_validate(op))
      continue;

    /* check on the availability of a read port for the given bank */
    // the bank bits are the lowest order cache index bits
    uns bank = op->oracle_info.va >> dc->dcache.shift_bits & N_BIT_MASK(LOG2(DCACHE_BANKS));
    DEBUG(dc->proc_id, "check_read and write port availiabilty mem_type:%s bank:%d \n",
          (op->table_info->mem_type == MEM_ST) ? "ST" : "LD", bank);
    if (!PERFECT_DCACHE && ((op->table_info->mem_type == MEM_ST && !get_write_port(&dc->ports[bank])) ||
                            (op->table_info->mem_type != MEM_ST && !get_read_port(&dc->ports[bank])))) {
      if (op->table_info->mem_type != MEM_ST)
        rfp_note_demand_port_denied(dc->proc_id);
      op->state = OS_WAIT_DCACHE;
      continue;
    }

    // memory ops are marked as scheduled so that they can be removed from the node->rdy_list
    op->state = OS_SCHEDULED;

    // ideal l2 l1 prefetcher bring l1 data immediately
    if (IDEAL_L2_L1_PREFETCHER)
      ideal_l2l1_prefetcher(op);

    /* now access the dcache with it */
    Addr line_addr;
    Flag first_dcache_access = (op->dcache_cycle == MAX_CTR);
    Dcache_Data* line = (Dcache_Data*)cache_access(&dc->dcache, op->oracle_info.va, &line_addr, TRUE);
    tea_record_load_cache_access_order(op, line_addr);
    op->dcache_cycle = cycle_count;
    if (first_dcache_access)
      dcache_stage_record_main_onpath_load_access(op);
    dcache_stage_record_main_chain_load_raw_stream_access(
      op, line_addr, first_dcache_access);
    dcache_stage_record_main_chain_load_profile_access(op, line_addr,
                                                       first_dcache_access);
    dc->idle_cycle = MAX2(dc->idle_cycle, cycle_count + DCACHE_CYCLES);

    if (op->table_info->mem_type == MEM_ST)
      STAT_EVENT(op->proc_id, POWER_DCACHE_WRITE_ACCESS);
    else
      STAT_EVENT(op->proc_id, POWER_DCACHE_READ_ACCESS);

    // if the data hits dc_pref_cache then insert to the dcache immediately
    if (DC_PREF_CACHE_ENABLE && !line) {
      line = dc_pref_cache_access(op);
    }

    op->oracle_info.dcmiss = FALSE;
    if (PERFECT_DCACHE) {
      if (!op->off_path) {
        STAT_EVENT(op->proc_id, DCACHE_HIT);
        STAT_EVENT(op->proc_id, DCACHE_HIT_ONPATH);
      } else {
        STAT_EVENT(op->proc_id, DCACHE_HIT_OFFPATH);
      }

      op->done_cycle = cycle_count + DCACHE_CYCLES + op->inst_info->extra_ld_latency;
      if (op->table_info->mem_type != MEM_ST) {
        op->wake_cycle = op->done_cycle;
        wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
      }
      dcache_stage_record_main_chain_load_profile_dcache_hit(op);
      /* TEA load via normal dcache (PERFECT_DCACHE): mark completed */
      if (TEA_ENABLE && op->thread_id == 1) {
        /* [EXPERIMENT: TEA_PERFECT_LOAD] PERFECT_DCACHE hit stats */
        {
          Counter _lat = op->done_cycle - cycle_count;
          STAT_EVENT(dc->proc_id, TEA_LOADS_EXECUTED);
          STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_ACCESS);
          STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_HIT);
          STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_SAMPLES);
          INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_TOTAL, _lat);
          INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_AVG,   _lat);
          tea_record_load_cache_hit_warm_source(op, line_addr);
          tea_chain_note_load_result(dc->proc_id, op,
                                     TEA_LOAD_RESULT_DCACHE_HIT,
                                     _lat);
          tea_log_chain_load_addr(op, line_addr, "PERFECT_DCACHE_HIT",
                                  _lat);
        }
        tea_op_completed(dc->proc_id, op);
      }
      continue;
    }

    if (line) {
      dcache_cacheline_hit(op, line_addr, line);
      dcache_stage_record_main_chain_load_profile_dcache_hit(op);
      /* TEA load via normal dcache (cache hit): mark completed */
      if (TEA_ENABLE && op->thread_id == 1) {
        /* [EXPERIMENT: TEA_PERFECT_LOAD] first-level data cache/L1D hit stats */
        {
          Counter _lat = op->done_cycle - cycle_count;
          STAT_EVENT(dc->proc_id, TEA_LOADS_EXECUTED);
          STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_ACCESS);
          STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_HIT);
          STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_SAMPLES);
          INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_TOTAL, _lat);
          INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_AVG,   _lat);
          tea_record_load_cache_hit_warm_source(op, line_addr);
          tea_chain_note_load_result(dc->proc_id, op,
                                     TEA_LOAD_RESULT_DCACHE_HIT,
                                     _lat);
          tea_log_chain_load_addr(op, line_addr, "L1D_HIT", _lat);
        }
        tea_op_completed(dc->proc_id, op);
      }
      continue;
    }
    dcache_cacheline_miss(op, line_addr);
  }

  /* prefetcher update */
  if (STREAM_PREFETCH_ON)
    update_pref_queue();
  if (L2WAY_PREF && !L1PREF_IMMEDIATE)
    update_l2way_pref_req_queue();
  if (L2MARKV_PREF_ON && !L1MARKV_PREF_IMMEDIATE)
    update_l2markv_pref_req_queue();

  /* Last claim on this cycle's dcache resources: under the default priority RFP
     probes only ever run on read ports nothing else wanted, which is how the
     paper gives prefetches the lowest L1 arbitration priority (§3.2). */
  rfp_queue_drain(dc->proc_id, dc, FALSE);
  rfp_account_dcache_ports(dc->proc_id, dc);

  dcache_stage_assert_occupancy("phase2");
}

/**************************************************************************************/
/* External API for architectural cache */

Flag dcache_fill_line(Mem_Req* req) {
  set_dcache_stage(&cmp_model.dcache_stage[req->proc_id]);
  Counter old_cycle_count = cycle_count;  // FIXME HACK!
  cycle_count = freq_cycle_count(FREQ_DOMAIN_CORES[req->proc_id]);

  ASSERT(dc->proc_id, dc->proc_id == req->proc_id);
  ASSERT(dc->proc_id, req->op_count == req->op_ptrs.count);
  ASSERT(dc->proc_id, req->op_count == req->op_uniques.count);

  /* if it can't get a write port, fail */
  uns bank = req->addr >> dc->dcache.shift_bits & N_BIT_MASK(LOG2(DCACHE_BANKS));
  if (!get_write_port(&dc->ports[bank])) {
    cycle_count = old_cycle_count;
    STAT_EVENT(dc->proc_id, DCACHE_FILL_PORT_UNAVAILABLE_ONPATH + req->off_path);
    return FAILURE;
  }

  /* get new line in the cache */
  Dcache_Data* data = dcache_fill_get_cacheline(req);
  if (data == NULL) {  // if the line we need to replace is dirty
    /*
     * This is a hack to get around a deadlock issue.
     * It doesn't completely eliminate the deadlock, but makes it less likely...
     *
     * The deadlock occurs when all the mem_req buffers are used,
     * and all pending mem_reqs need to fill the dcache,
     * but the highest priority dcache fill ends up evicting a dirty line from the dcache,
     * which then needs to be written back to L1/MLC.
     *
     * This dcache fill will aquire a write port via get_write_port(), but then fail here,
     * because there are no more mem_req buffers available for dc wb req,
     * and new_mem_dc_wb_req() will return FALSE.
     *
     * If we don't release the write port, then all other mem_reqs,
     * which still need to fill the dcache, will fail, and we end up in a deadlock.
     * So instead, we release the write port below.
     *
     * HOWEVER, a deadlock is still possible if all pending mem_reqs fill the dcache
     * and all end up evicting a dirty line
     */

    ASSERT(dc->proc_id, 0 < dc->ports[bank].write_ports_in_use);
    dc->ports[bank].write_ports_in_use--;
    ASSERT(dc->proc_id, dc->ports[bank].write_ports_in_use < dc->ports->num_write_ports);

    /* TODO: fix this by using a new_cycle_count to avoid replacing cycle_count */
    cycle_count = old_cycle_count;
    return FAILURE;
  }

  /* update cacheline fields and wake up dependent ops */
  dcache_fill_process_cacheline(req, data);

  cycle_count = old_cycle_count;
  return SUCCESS;
}

Flag do_oracle_dcache_access(Op* op, Addr* line_addr) {
  Dcache_Data* hit;
  hit = (Dcache_Data*)cache_access(&dc->dcache, op->oracle_info.va, line_addr, FALSE);

  if (hit)
    return TRUE;
  else
    return FALSE;
}

/**************************************************************************************/
/* Inline Methods */

static inline int dcache_stage_count_valid_ops(void) {
  int count = 0;
  for (uns ii = 0; ii < dc->sd.max_op_count; ii++) {
    if (dc->sd.ops[ii])
      count++;
  }
  return count;
}

static inline void dcache_stage_assert_occupancy(const char* context) {
  int valid_ops = dcache_stage_count_valid_ops();
  ASSERTM(dc->proc_id, dc->sd.op_count == valid_ops,
          "dcache stage occupancy mismatch after %s: op_count=%d valid_ops=%d C=%llu\n",
          context ? context : "unknown", dc->sd.op_count, valid_ops,
          cycle_count);
}

static inline void dcache_stage_preserve_unprocessed_ops_after_full_tea_flush(Counter last_processed_op_num) {
  for (uns ii = 0; ii < dc->sd.max_op_count; ii++) {
    Op* pending = dc->sd.ops[ii];
    if (!pending || pending->op_num <= last_processed_op_num)
      continue;

    /* The local dcache worklist was invalidated by a TEA pipeline squash.
     * Surviving main-thread memory ops after the flushed TEA op have not
     * accessed the cache in this cycle, so keep them resident and retry them
     * next cycle instead of letting phase 1 discard them as completed ops. */
    ASSERTM(dc->proc_id, pending->thread_id != 1,
            "TEA op survived full-thread flush in dcache stage: op_num=%s chain=%u state=%d C=%llu\n",
            unsstr64(pending->op_num), pending->h2p_chain_id,
            pending->state, cycle_count);
    if (pending->state != OS_WAIT_MEM)
      pending->state = OS_WAIT_DCACHE;
  }
}

static inline void dcache_stage_remove_src_op(Stage_Data* src_sd, int ii) {
  src_sd->ops[ii] = NULL;
  src_sd->op_count--;
  ASSERT(dc->proc_id, src_sd->op_count >= 0);
}

static inline Flag dcache_stage_addr_unready(Op* op) {
  /*
   * this is a little screwy. if the addr gen time is more than one cycle, then the op
   * won't get cleared out of the exec stage, thus making it block the functional unit
   * (not for the henry mem system, which handles agen itself)
   */
  if (cycle_count >= op->exec_cycle)
    return FALSE;

  /*
   * the DCACHE_CYCLES == 0 check is to make a address + 0 cycle cache.
   * This stage will grab the op out of exec a cycle before normal,
   * so the wake up happens in the same cycle as execute
   */
  if (DCACHE_CYCLES == 0 && cycle_count + 1 == op->exec_cycle)
    return FALSE;

  return TRUE;
}

static inline Flag dcache_stage_check_mem_type(Op* op) {
  /* just squish non-memory ops */
  if (op->table_info->mem_type == NOT_MEM) {
    return FALSE;
  }

  /* skip prefetch ops if software prefetching is disabled */
  if (op->table_info->mem_type == MEM_PF && !ENABLE_SWPRF) {
    op->done_cycle = cycle_count + DCACHE_CYCLES;
    op->state = OS_SCHEDULED;
    return FALSE;
  }

  return TRUE;
}

static inline Flag dcache_stage_main_onpath_load(Op* op) {
  return op && op->thread_id == 0 && op->table_info &&
         op->table_info->mem_type == MEM_LD && !op->off_path;
}

static inline Flag dcache_stage_main_chain_load_target_op(Op* op) {
  return dcache_stage_main_onpath_load(op) && op->chain_bit && op->inst_info;
}

static inline Flag dcache_stage_main_chain_load_profile_op(Op* op) {
  return H2P_CHAIN_LOAD_PROFILE && dcache_stage_main_chain_load_target_op(op);
}

static inline void dcache_stage_record_main_onpath_load_access(Op* op) {
  if (dcache_stage_main_onpath_load(op))
    STAT_EVENT(op->proc_id, H2P_MAIN_ONPATH_LOADS_DCACHE_ACCESS);
}

static inline double h2p_chain_profile_pct(Counter num, Counter den) {
  return den ? (100.0 * (double)num / (double)den) : 0.0;
}

static inline uns h2p_chain_load_pattern_buckets(void) {
  return H2P_CHAIN_LOAD_PATTERN_HASH_BUCKETS ?
           H2P_CHAIN_LOAD_PATTERN_HASH_BUCKETS : 1;
}

static inline SCounter h2p_chain_load_pattern_delta(Addr curr, Addr prev) {
  return curr >= prev ? (SCounter)(curr - prev) : -(SCounter)(prev - curr);
}

static inline int64 h2p_chain_load_pattern_hash_key(Addr load_pc,
                                                    uns64 value) {
  uns64 x = load_pc ^ (load_pc >> 17) ^ (value << 7) ^ (value >> 13);
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return (int64)x;
}

static Flag h2p_chain_load_pattern_value_key_eq(void const* lhs,
                                                void const* rhs) {
  H2P_Chain_Load_Pattern_Value_Key const* lhs_key =
    (H2P_Chain_Load_Pattern_Value_Key const*)lhs;
  H2P_Chain_Load_Pattern_Value_Key const* rhs_key =
    (H2P_Chain_Load_Pattern_Value_Key const*)rhs;
  return lhs_key->load_pc == rhs_key->load_pc &&
         lhs_key->value == rhs_key->value;
}

static Flag h2p_chain_load_pattern_delta_key_eq(void const* lhs,
                                                void const* rhs) {
  H2P_Chain_Load_Pattern_Delta_Key const* lhs_key =
    (H2P_Chain_Load_Pattern_Delta_Key const*)lhs;
  H2P_Chain_Load_Pattern_Delta_Key const* rhs_key =
    (H2P_Chain_Load_Pattern_Delta_Key const*)rhs;
  return lhs_key->load_pc == rhs_key->load_pc &&
         lhs_key->delta == rhs_key->delta;
}

static inline void h2p_chain_load_pattern_init_table(
  Hash_Table* table, Flag* valid, const char* name, uns data_size,
  Flag (*eq_func)(void const*, void const*)) {
  if (!*valid) {
    init_complex_hash_table(table, name, h2p_chain_load_pattern_buckets(),
                            data_size, eq_func);
    *valid = TRUE;
  }
}

static inline Flag h2p_chain_load_pattern_note_unique_value(
  Hash_Table* table, Flag* valid, const char* name, Addr load_pc,
  Addr value) {
  h2p_chain_load_pattern_init_table(table, valid, name,
                                    sizeof(H2P_Chain_Load_Pattern_Value_Key),
                                    h2p_chain_load_pattern_value_key_eq);

  H2P_Chain_Load_Pattern_Value_Key key;
  key.load_pc = load_pc;
  key.value = value;
  key.count = 0;

  Flag new_entry = FALSE;
  H2P_Chain_Load_Pattern_Value_Key* entry =
    (H2P_Chain_Load_Pattern_Value_Key*)complex_hash_table_access_create(
      table, h2p_chain_load_pattern_hash_key(load_pc, value), &key,
      &new_entry);
  if (new_entry)
    *entry = key;
  entry->count++;
  return new_entry;
}

static inline Counter h2p_chain_load_pattern_note_delta(
  Hash_Table* table, Flag* valid, const char* name, Addr load_pc,
  SCounter delta) {
  h2p_chain_load_pattern_init_table(table, valid, name,
                                    sizeof(H2P_Chain_Load_Pattern_Delta_Key),
                                    h2p_chain_load_pattern_delta_key_eq);

  H2P_Chain_Load_Pattern_Delta_Key key;
  key.load_pc = load_pc;
  key.delta = delta;
  key.count = 0;

  Flag new_entry = FALSE;
  H2P_Chain_Load_Pattern_Delta_Key* entry =
    (H2P_Chain_Load_Pattern_Delta_Key*)complex_hash_table_access_create(
      table, h2p_chain_load_pattern_hash_key(load_pc, (uns64)delta), &key,
      &new_entry);
  if (new_entry)
    *entry = key;
  entry->count++;
  return entry->count;
}

static inline void h2p_chain_load_pattern_update_top(
  Flag valid[H2P_CHAIN_LOAD_PATTERN_TOPK],
  SCounter value[H2P_CHAIN_LOAD_PATTERN_TOPK],
  Counter count[H2P_CHAIN_LOAD_PATTERN_TOPK], SCounter delta,
  Counter new_count) {
  int empty_slot = -1;
  int min_slot = 0;

  for (int ii = 0; ii < H2P_CHAIN_LOAD_PATTERN_TOPK; ii++) {
    if (valid[ii] && value[ii] == delta) {
      count[ii] = new_count;
      return;
    }
    if (!valid[ii] && empty_slot < 0)
      empty_slot = ii;
    if (!valid[min_slot] || (valid[ii] && count[ii] < count[min_slot]))
      min_slot = ii;
  }

  if (empty_slot >= 0) {
    valid[empty_slot] = TRUE;
    value[empty_slot] = delta;
    count[empty_slot] = new_count;
  } else if (new_count > count[min_slot]) {
    value[min_slot] = delta;
    count[min_slot] = new_count;
  }
}

static inline uns h2p_chain_load_profile_hash(Addr block_start_pc,
                                              uns block_op_idx,
                                              Addr load_pc,
                                              uns table_mask) {
  uns64 x = load_pc ^ (load_pc >> 17) ^ (block_start_pc << 7) ^
            (block_start_pc >> 13) ^ ((uns64)block_op_idx << 32) ^
            (uns64)block_op_idx;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (uns)(x & table_mask);
}

static inline void h2p_chain_load_profile_register_dump(void) {
  if (!h2p_chain_load_profile_dump_registered) {
    atexit(dump_h2p_chain_load_profile_tables);
    h2p_chain_load_profile_dump_registered = TRUE;
  }
}

static FILE* h2p_chain_load_raw_stream_open(void) {
  if (h2p_chain_load_raw_stream_file)
    return h2p_chain_load_raw_stream_file;

  h2p_chain_load_profile_register_dump();
  h2p_chain_load_raw_stream_file =
    file_tag_fopen(OUTPUT_DIR, "h2p_chain_load_raw_stream", "w");
  if (!h2p_chain_load_raw_stream_file)
    return NULL;

  fprintf(h2p_chain_load_raw_stream_file,
          "seq,cycle,op_num,unique_num,proc_id,thread_id,off_path,"
          "is_target_h2p_chain_load,load_pc,vaddr,line_addr,line_index,"
          "mem_size,block_start_pc,block_op_idx,pred_global_hist\n");
  return h2p_chain_load_raw_stream_file;
}

static void h2p_chain_load_raw_stream_close(void) {
  if (!h2p_chain_load_raw_stream_file)
    return;

  fclose(h2p_chain_load_raw_stream_file);
  h2p_chain_load_raw_stream_file = NULL;
}

static H2P_Chain_Load_Profile_Summary_Entry*
h2p_chain_load_profile_get_pc_entry(uns proc_id, Addr load_pc, Flag create) {
  if (proc_id >= MAX_NUM_PROCS)
    return NULL;

  if (!h2p_chain_load_pc_profile_table[proc_id]) {
    if (!create)
      return NULL;
    h2p_chain_load_profile_register_dump();
    h2p_chain_load_pc_profile_table[proc_id] =
      (H2P_Chain_Load_Profile_Summary_Entry*)calloc(
        H2P_CHAIN_LOAD_PROFILE_PC_TABLE_SIZE,
        sizeof(H2P_Chain_Load_Profile_Summary_Entry));
    ASSERT(proc_id, h2p_chain_load_pc_profile_table[proc_id]);
  }

  H2P_Chain_Load_Profile_Summary_Entry* table =
    h2p_chain_load_pc_profile_table[proc_id];
  uns index = h2p_chain_load_profile_hash(0, 0, load_pc,
                                          H2P_CHAIN_LOAD_PROFILE_PC_TABLE_SIZE - 1);
  for (uns probe = 0; probe < H2P_CHAIN_LOAD_PROFILE_PC_TABLE_SIZE; probe++) {
    H2P_Chain_Load_Profile_Summary_Entry* entry =
      &table[(index + probe) & (H2P_CHAIN_LOAD_PROFILE_PC_TABLE_SIZE - 1)];
    if (!entry->valid) {
      if (!create)
        return NULL;
      entry->valid = TRUE;
      entry->load_pc = load_pc;
      return entry;
    }
    if (entry->load_pc == load_pc)
      return entry;
  }

  if (create)
    h2p_chain_load_pc_profile_overflow[proc_id]++;
  return NULL;
}

static H2P_Chain_Load_Profile_Summary_Entry*
h2p_chain_load_profile_get_slot_entry(uns proc_id, Addr block_start_pc,
                                      uns block_op_idx, Addr load_pc,
                                      Flag create) {
  if (proc_id >= MAX_NUM_PROCS)
    return NULL;

  if (!h2p_chain_load_slot_profile_table[proc_id]) {
    if (!create)
      return NULL;
    h2p_chain_load_profile_register_dump();
    h2p_chain_load_slot_profile_table[proc_id] =
      (H2P_Chain_Load_Profile_Summary_Entry*)calloc(
        H2P_CHAIN_LOAD_PROFILE_SLOT_TABLE_SIZE,
        sizeof(H2P_Chain_Load_Profile_Summary_Entry));
    ASSERT(proc_id, h2p_chain_load_slot_profile_table[proc_id]);
  }

  H2P_Chain_Load_Profile_Summary_Entry* table =
    h2p_chain_load_slot_profile_table[proc_id];
  uns index = h2p_chain_load_profile_hash(
    block_start_pc, block_op_idx, load_pc,
    H2P_CHAIN_LOAD_PROFILE_SLOT_TABLE_SIZE - 1);
  for (uns probe = 0; probe < H2P_CHAIN_LOAD_PROFILE_SLOT_TABLE_SIZE; probe++) {
    H2P_Chain_Load_Profile_Summary_Entry* entry =
      &table[(index + probe) & (H2P_CHAIN_LOAD_PROFILE_SLOT_TABLE_SIZE - 1)];
    if (!entry->valid) {
      if (!create)
        return NULL;
      entry->valid = TRUE;
      entry->block_start_pc = block_start_pc;
      entry->block_op_idx = block_op_idx;
      entry->load_pc = load_pc;
      return entry;
    }
    if (entry->block_start_pc == block_start_pc &&
        entry->block_op_idx == block_op_idx && entry->load_pc == load_pc)
      return entry;
  }

  if (create)
    h2p_chain_load_slot_profile_overflow[proc_id]++;
  return NULL;
}

static inline void h2p_chain_load_profile_record_stride_hist(
  H2P_Chain_Load_Profile_Summary_Entry* entry, SCounter stride) {
  int empty_slot = -1;
  int min_slot = 0;
  for (int ii = 0; ii < H2P_CHAIN_LOAD_PROFILE_STRIDE_SLOTS; ii++) {
    if (entry->stride_valid[ii] && entry->stride_value[ii] == stride) {
      entry->stride_count[ii]++;
      return;
    }
    if (!entry->stride_valid[ii] && empty_slot < 0)
      empty_slot = ii;
    if (entry->stride_count[ii] < entry->stride_count[min_slot])
      min_slot = ii;
  }

  int slot = empty_slot >= 0 ? empty_slot : min_slot;
  entry->stride_valid[slot] = TRUE;
  entry->stride_value[slot] = stride;
  entry->stride_count[slot] = 1;
}

static inline void h2p_chain_load_profile_record_reuse(
  H2P_Chain_Load_Profile_Summary_Entry* entry, Op* op, Addr line_addr) {
  if (entry->has_last_addr) {
    Addr prev_line = entry->last_line_addr >> LOG2(DCACHE_LINE_SIZE);
    Addr curr_line = line_addr >> LOG2(DCACHE_LINE_SIZE);
    Counter stride_abs = curr_line >= prev_line ? curr_line - prev_line :
                                                   prev_line - curr_line;
    SCounter stride = curr_line >= prev_line ? (SCounter)stride_abs :
                                               -(SCounter)stride_abs;

    entry->reuse++;
    if (entry->last_va == op->oracle_info.va)
      entry->same_va++;
    if (entry->last_line_addr == line_addr)
      entry->same_va_line++;

    if (stride_abs == 0)
      entry->stride_zero++;
    else if (stride == 1)
      entry->stride_pos_1++;
    else if (stride == -1)
      entry->stride_neg_1++;
    else if (stride_abs <= 4)
      entry->stride_small_abs_le_4++;
    else if (stride_abs <= 16)
      entry->stride_small_abs_le_16++;
    else
      entry->stride_other++;

    h2p_chain_load_profile_record_stride_hist(entry, stride);
  }

  entry->has_last_addr = TRUE;
  entry->last_va = op->oracle_info.va;
  entry->last_line_addr = line_addr;
}

static inline void h2p_chain_load_pattern_record_pc_access(
  H2P_Chain_Load_Profile_Summary_Entry* entry, Op* op, Addr line_addr) {
  if (!H2P_CHAIN_LOAD_PATTERN_PROFILE || !entry || !op || !op->inst_info)
    return;

  uns proc_id = op->proc_id;
  if (proc_id >= MAX_NUM_PROCS)
    return;

  Addr load_pc = op->inst_info->addr;
  Addr vaddr = op->oracle_info.va;
  Addr line_num = line_addr >> LOG2(DCACHE_LINE_SIZE);

  if (h2p_chain_load_pattern_note_unique_value(
        &h2p_chain_load_unique_vaddr_table[proc_id],
        &h2p_chain_load_unique_vaddr_table_valid[proc_id],
        "H2P chain load unique vaddr", load_pc, vaddr)) {
    entry->unique_vaddr++;
  }

  if (h2p_chain_load_pattern_note_unique_value(
        &h2p_chain_load_unique_line_table[proc_id],
        &h2p_chain_load_unique_line_table_valid[proc_id],
        "H2P chain load unique line", load_pc, line_num)) {
    entry->unique_line++;
  }

  if (!entry->has_last_addr)
    return;

  SCounter byte_delta =
    h2p_chain_load_pattern_delta(vaddr, entry->last_va);
  SCounter line_delta =
    h2p_chain_load_pattern_delta(line_num,
                                 entry->last_line_addr >>
                                   LOG2(DCACHE_LINE_SIZE));
  Counter byte_delta_count = h2p_chain_load_pattern_note_delta(
    &h2p_chain_load_byte_delta_table[proc_id],
    &h2p_chain_load_byte_delta_table_valid[proc_id],
    "H2P chain load byte delta", load_pc, byte_delta);
  Counter line_delta_count = h2p_chain_load_pattern_note_delta(
    &h2p_chain_load_line_delta_table[proc_id],
    &h2p_chain_load_line_delta_table_valid[proc_id],
    "H2P chain load line delta", load_pc, line_delta);

  h2p_chain_load_pattern_update_top(entry->byte_delta_valid,
                                    entry->byte_delta_value,
                                    entry->byte_delta_count, byte_delta,
                                    byte_delta_count);
  h2p_chain_load_pattern_update_top(entry->line_delta_valid,
                                    entry->line_delta_value,
                                    entry->line_delta_count, line_delta,
                                    line_delta_count);
}

static inline void h2p_chain_load_profile_record_access(Op* op,
                                                        Addr line_addr) {
  if (!dcache_stage_main_chain_load_profile_op(op))
    return;

  Addr load_pc = op->inst_info->addr;
  H2P_Chain_Load_Profile_Summary_Entry* pc_entry =
    h2p_chain_load_profile_get_pc_entry(op->proc_id, load_pc, TRUE);
  H2P_Chain_Load_Profile_Summary_Entry* slot_entry =
    h2p_chain_load_profile_get_slot_entry(
      op->proc_id, op->h2p_chain_block_start_pc,
      op->h2p_chain_block_op_idx, load_pc, TRUE);

  if (pc_entry) {
    pc_entry->accesses++;
    h2p_chain_load_pattern_record_pc_access(pc_entry, op, line_addr);
    h2p_chain_load_profile_record_reuse(pc_entry, op, line_addr);
  }
  if (slot_entry) {
    slot_entry->accesses++;
    h2p_chain_load_profile_record_reuse(slot_entry, op, line_addr);
  }

  op->h2p_chain_profile_access_recorded = TRUE;
}

static inline void h2p_chain_load_profile_record_result_for_entry(
  H2P_Chain_Load_Profile_Summary_Entry* entry, Op* op,
  H2P_Chain_Load_Profile_Result result) {
  if (!entry)
    return;

  switch (result) {
    case H2P_CHAIN_LOAD_PROFILE_RESULT_DCACHE_HIT:
      entry->dcache_hit++;
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_STORE_REQ_BUFFER_HIT:
      entry->store_req_buffer_hit++;
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_PREFETCH_MSHR_HIT:
      entry->dcache_miss++;
      entry->prefetch_mshr_hit++;
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_DEMAND_MSHR_HIT:
      entry->dcache_miss++;
      entry->demand_mshr_hit++;
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_MLC_HIT:
      entry->dcache_miss++;
      entry->mlc_hit++;
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_SCARAB_L1_HIT:
      entry->dcache_miss++;
      entry->scarab_l1_hit++;
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_MEM_ACCESS:
      entry->dcache_miss++;
      entry->mem_access++;
      break;
  }

  if (op->h2p_chain_profile_first_dcache_cycle != MAX_CTR &&
      op->done_cycle >= op->h2p_chain_profile_first_dcache_cycle) {
    Counter latency =
      op->done_cycle - op->h2p_chain_profile_first_dcache_cycle;
    entry->latency_samples++;
    entry->latency_total += latency;
  }
}

static inline void h2p_chain_load_profile_record_result(
  Op* op, H2P_Chain_Load_Profile_Result result) {
  if (!dcache_stage_main_chain_load_profile_op(op) ||
      !op->h2p_chain_profile_access_recorded)
    return;

  Addr load_pc = op->inst_info->addr;
  h2p_chain_load_profile_record_result_for_entry(
    h2p_chain_load_profile_get_pc_entry(op->proc_id, load_pc, FALSE), op,
    result);
  h2p_chain_load_profile_record_result_for_entry(
    h2p_chain_load_profile_get_slot_entry(
      op->proc_id, op->h2p_chain_block_start_pc,
      op->h2p_chain_block_op_idx, load_pc, FALSE),
    op, result);
}

static inline void dcache_stage_record_main_chain_load_profile_access(
  Op* op, Addr line_addr, Flag first_access) {
  if (!first_access || !dcache_stage_main_chain_load_profile_op(op))
    return;

  op->h2p_chain_profile_first_dcache_cycle = cycle_count;
  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_DCACHE_ACCESS);
  h2p_chain_load_profile_record_access(op, line_addr);
}

static inline void dcache_stage_record_main_chain_load_raw_stream_access(
  Op* op, Addr line_addr, Flag first_access) {
  if (!H2P_CHAIN_LOAD_RAW_STREAM_DUMP || !first_access ||
      !dcache_stage_main_chain_load_target_op(op))
    return;

  FILE* file = h2p_chain_load_raw_stream_open();
  if (!file)
    return;

  fprintf(file,
          "%llu,%llu,%llu,%llu,%u,%u,%u,%u,"
          "0x%llx,0x%llx,0x%llx,%llu,%u,0x%llx,%u,0x%x\n",
          (unsigned long long)h2p_chain_load_raw_stream_seq++,
          (unsigned long long)cycle_count,
          (unsigned long long)op->op_num,
          (unsigned long long)op->unique_num,
          (unsigned)op->proc_id,
          (unsigned)op->thread_id,
          (unsigned)op->off_path,
          1u,
          (unsigned long long)op->inst_info->addr,
          (unsigned long long)op->oracle_info.va,
          (unsigned long long)line_addr,
          (unsigned long long)(line_addr >> LOG2(DCACHE_LINE_SIZE)),
          (unsigned)op->oracle_info.mem_size,
          (unsigned long long)op->h2p_chain_block_start_pc,
          (unsigned)op->h2p_chain_block_op_idx,
          (unsigned)op->oracle_info.pred_global_hist);
}

static inline void dcache_stage_record_main_chain_load_profile_latency(
  Op* op, H2P_Chain_Load_Profile_Result result) {
  if (!dcache_stage_main_chain_load_profile_op(op) ||
      op->h2p_chain_profile_first_dcache_cycle == MAX_CTR ||
      op->done_cycle < op->h2p_chain_profile_first_dcache_cycle)
    return;

  Counter latency =
    op->done_cycle - op->h2p_chain_profile_first_dcache_cycle;
  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_LATENCY_SAMPLES);
  INC_STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_LATENCY_TOTAL,
                 latency);
  INC_STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_LATENCY_AVG,
                 latency);

#define RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(category)                   \
  do {                                                                     \
    STAT_EVENT(op->proc_id,                                                \
               H2P_CHAIN_LOAD_PROFILE_##category##_LATENCY_SAMPLES);       \
    INC_STAT_EVENT(op->proc_id,                                            \
                   H2P_CHAIN_LOAD_PROFILE_##category##_LATENCY_TOTAL,      \
                   latency);                                               \
    INC_STAT_EVENT(op->proc_id,                                            \
                   H2P_CHAIN_LOAD_PROFILE_##category##_LATENCY_AVG,        \
                   latency);                                               \
    INC_STAT_EVENT(op->proc_id,                                            \
                   H2P_CHAIN_LOAD_PROFILE_##category##_SERVICE_CYCLE_PCT,  \
                   latency);                                               \
  } while (0)

  switch (result) {
    case H2P_CHAIN_LOAD_PROFILE_RESULT_DCACHE_HIT:
      RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(DCACHE_HIT);
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_STORE_REQ_BUFFER_HIT:
      RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(STORE_FWD);
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_PREFETCH_MSHR_HIT:
      RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(MSHR_HIT);
      RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(PREFETCH_MSHR_HIT);
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_DEMAND_MSHR_HIT:
      RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(MSHR_HIT);
      RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(DEMAND_MSHR_HIT);
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_MLC_HIT:
      RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(MLC_HIT);
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_SCARAB_L1_HIT:
      RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(SCARAB_L1_HIT);
      break;
    case H2P_CHAIN_LOAD_PROFILE_RESULT_MEM_ACCESS:
      RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY(MEM_ACCESS);
      break;
  }

#undef RECORD_H2P_CHAIN_LOAD_CATEGORY_LATENCY
}

static inline void dcache_stage_record_main_chain_load_profile_dcache_hit(
  Op* op) {
  if (!dcache_stage_main_chain_load_profile_op(op))
    return;

  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_DCACHE_HIT);
  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_DCACHE_HIT_PCT);
  h2p_chain_load_profile_record_result(
    op, H2P_CHAIN_LOAD_PROFILE_RESULT_DCACHE_HIT);
  dcache_stage_record_main_chain_load_profile_latency(
    op, H2P_CHAIN_LOAD_PROFILE_RESULT_DCACHE_HIT);
}

static inline void dcache_stage_record_main_chain_load_profile_store_fwd(
  Op* op) {
  if (!dcache_stage_main_chain_load_profile_op(op))
    return;

  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_STORE_FWD);
  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_STORE_FWD_PCT);
  h2p_chain_load_profile_record_result(
    op, H2P_CHAIN_LOAD_PROFILE_RESULT_STORE_REQ_BUFFER_HIT);
  dcache_stage_record_main_chain_load_profile_latency(
    op, H2P_CHAIN_LOAD_PROFILE_RESULT_STORE_REQ_BUFFER_HIT);
}

static inline void dcache_stage_record_main_chain_load_profile_fill(Op* op,
                                                                    Mem_Req* req) {
  if (!dcache_stage_main_chain_load_profile_op(op))
    return;

  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_DCACHE_MISS);
  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_DCACHE_MISS_PCT);
  H2P_Chain_Load_Profile_Result result;
  if (op->mem_reqbuf_match) {
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_MSHR_HIT);
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_MSHR_HIT_PCT);
    if (op->mem_reqbuf_match_prefetch) {
      STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_PREFETCH_MSHR_HIT);
      STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_PREFETCH_MSHR_HIT_PCT);
      result = H2P_CHAIN_LOAD_PROFILE_RESULT_PREFETCH_MSHR_HIT;
    } else {
      STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_DEMAND_MSHR_HIT);
      STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_DEMAND_MSHR_HIT_PCT);
      result = H2P_CHAIN_LOAD_PROFILE_RESULT_DEMAND_MSHR_HIT;
    }
  } else if (req->mlc_hit) {
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_MLC_HIT);
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_MLC_HIT_PCT);
    result = H2P_CHAIN_LOAD_PROFILE_RESULT_MLC_HIT;
  } else if (req->l1_hit) {
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_SCARAB_L1_HIT);
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_SCARAB_L1_HIT_PCT);
    result = H2P_CHAIN_LOAD_PROFILE_RESULT_SCARAB_L1_HIT;
  } else {
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_MEM_ACCESS);
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_PROFILE_MEM_ACCESS_PCT);
    result = H2P_CHAIN_LOAD_PROFILE_RESULT_MEM_ACCESS;
  }
  h2p_chain_load_profile_record_result(op, result);
  dcache_stage_record_main_chain_load_profile_latency(op, result);
}

static inline H2P_Oracle_Pred_Entry* h2p_oracle_pred_get_entry(uns proc_id,
                                                              Addr load_pc) {
  if (!h2p_oracle_pred_table_valid[proc_id]) {
    init_hash_table(&h2p_oracle_pred_table[proc_id], "H2P chain oracle predictor",
                    16384, sizeof(H2P_Oracle_Pred_Entry));
    h2p_oracle_pred_table_valid[proc_id] = TRUE;
  }
  Flag new_entry = FALSE;
  H2P_Oracle_Pred_Entry* entry = (H2P_Oracle_Pred_Entry*)hash_table_access_create(
    &h2p_oracle_pred_table[proc_id], (int64)load_pc, &new_entry);
  if (new_entry)
    memset(entry, 0, sizeof(*entry));
  return entry;
}

/* Value the predictor learns/matches on: exact vaddr (RF-style) or cache line
   (L1-style), selected by H2P_CHAIN_ORACLE_GRANULARITY. */
static inline int64 h2p_oracle_pred_value(Op* op) {
  if (H2P_CHAIN_ORACLE_GRANULARITY == 1)
    return (int64)(op->oracle_info.va >> LOG2(DCACHE_LINE_SIZE));
  return (int64)op->oracle_info.va;
}

static inline Flag h2p_oracle_pred_predict(H2P_Oracle_Pred_Entry* entry,
                                           int64* pred) {
  if (!entry->has_last)
    return FALSE;
  if (H2P_CHAIN_ORACLE_PREDICTOR == 1) { /* stride */
    if (!entry->has_stride || entry->conf < H2P_CHAIN_ORACLE_STRIDE_CONFIDENCE)
      return FALSE;
    *pred = entry->last + entry->stride;
    return TRUE;
  }
  /* top-delta (2): predict last + most-frequent observed delta */
  if (entry->n_deltas == 0)
    return FALSE;
  uns best = 0;
  for (uns ii = 1; ii < entry->n_deltas; ii++)
    if (entry->delta_cnt[ii] > entry->delta_cnt[best])
      best = ii;
  if (entry->delta_cnt[best] < H2P_CHAIN_ORACLE_MIN_COUNT)
    return FALSE;
  *pred = entry->last + entry->delta_val[best];
  return TRUE;
}

static inline void h2p_oracle_pred_update(H2P_Oracle_Pred_Entry* entry,
                                          int64 value) {
  if (entry->has_last) {
    int64 delta = value - entry->last;
    if (H2P_CHAIN_ORACLE_PREDICTOR == 1) { /* stride */
      if (entry->has_stride && entry->stride == delta) {
        entry->conf++;
      } else {
        entry->stride = delta;
        entry->has_stride = TRUE;
        entry->conf = 1;
      }
    } else { /* top-delta: bounded histogram of deltas */
      uns ii;
      for (ii = 0; ii < entry->n_deltas; ii++)
        if (entry->delta_val[ii] == delta) {
          entry->delta_cnt[ii]++;
          break;
        }
      if (ii == entry->n_deltas) {
        if (entry->n_deltas < H2P_ORACLE_TOP_DELTA_SLOTS) {
          entry->delta_val[entry->n_deltas] = delta;
          entry->delta_cnt[entry->n_deltas] = 1;
          entry->n_deltas++;
        } else {
          uns least = 0;
          for (uns jj = 1; jj < entry->n_deltas; jj++)
            if (entry->delta_cnt[jj] < entry->delta_cnt[least])
              least = jj;
          entry->delta_val[least] = delta;
          entry->delta_cnt[least] = 1;
        }
      }
    }
  }
  entry->last = value;
  entry->has_last = TRUE;
}

static inline Flag dcache_stage_try_main_chain_load_oracle(Op* op) {
  if (!H2P_CHAIN_PERFECT_LOAD || op->thread_id != 0 ||
      op->table_info->mem_type != MEM_LD || !op->chain_bit || op->off_path)
    return FALSE;

  Flag predictor_on = (H2P_CHAIN_ORACLE_PREDICTOR != 0);
  /* A predictor-missed load can remain in the dcache stage when a bank port is
     unavailable.  Keep predictor evaluation separate from dcache access state:
     the same dynamic load may retry its demand access, but it must not learn its
     actual address and then re-predict itself on a later cycle. */
  if (predictor_on && op->h2p_oracle_pred_checked)
    return FALSE;

  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_CANDIDATES);

  Counter latency = H2P_CHAIN_PERFECT_LOAD_LATENCY;

  if (predictor_on) {
    op->h2p_oracle_pred_checked = TRUE;
    op->zereco_rf_covered = FALSE;
    H2P_Oracle_Pred_Entry* entry =
      h2p_oracle_pred_get_entry(op->proc_id, op->inst_info->addr);
    int64 pred = 0;
    Flag have_pred = h2p_oracle_pred_predict(entry, &pred);
    int64 actual = h2p_oracle_pred_value(op);
    /* The dcache-stage arrival stream is not necessarily program ordered.
       What matters here is that this dynamic load predicts before its own
       address updates the online predictor. */
    h2p_oracle_pred_update(entry, actual);

    if (have_pred)
      STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_PRED_MADE);
    if (!have_pred || pred != actual) {
      if (have_pred)
        STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_PRED_WRONG);
      return FALSE; /* not covered by the predictor → normal cache path */
    }
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_PRED_CORRECT);
    if (H2P_CHAIN_ORACLE_GRANULARITY == 1)
      /* L1-prefetch oracle: only the line was predicted, so the load still pays
         a normal L1-hit load-use latency (the value is in L1, not the RF). */
      latency = DCACHE_CYCLES + op->inst_info->extra_ld_latency;
    else
      /* RF-prefetch oracle: exact address predicted, value delivered to the
         physical register — near register-read latency. */
      latency = H2P_CHAIN_ORACLE_HIT_LATENCY;
  }

  if (scan_stores(op->oracle_info.va, op->oracle_info.mem_size)) {
    STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_STORE_FWD_EXCLUDED);
    return FALSE;
  }

  /* This outcome is consumed only after the op retires.  It therefore trains
     the filtered IQ policy for a future occurrence of the H2P slice and never
     changes the priority of older producers in the current occurrence. */
  if (predictor_on)
    op->zereco_rf_covered = TRUE;

  op->state = OS_SCHEDULED;
  op->dcache_cycle = cycle_count;
  op->done_cycle = cycle_count + latency;
  op->wake_cycle = op->done_cycle;
  op->oracle_info.dcmiss = FALSE;
  op->engine_info.dcmiss = FALSE;
  wake_up_ops(op, REG_DATA_DEP, model->wake_hook);

  dcache_stage_record_main_onpath_load_access(op);
  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_BYPASSED);
  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_BYPASSED_PCT);
  STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_LATENCY_SAMPLES);
  INC_STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_LATENCY_TOTAL, latency);
  INC_STAT_EVENT(op->proc_id, H2P_CHAIN_LOAD_ORACLE_LATENCY_AVG, latency);
  return TRUE;
}

static void h2p_chain_load_profile_dominant_stride(
  H2P_Chain_Load_Profile_Summary_Entry* entry, SCounter* stride,
  Counter* count) {
  *stride = 0;
  *count = 0;
  for (int ii = 0; ii < H2P_CHAIN_LOAD_PROFILE_STRIDE_SLOTS; ii++) {
    if (entry->stride_valid[ii] && entry->stride_count[ii] > *count) {
      *stride = entry->stride_value[ii];
      *count = entry->stride_count[ii];
    }
  }
}

static int h2p_chain_load_profile_entry_access_cmp(const void* lhs,
                                                   const void* rhs) {
  const H2P_Chain_Load_Profile_Summary_Entry* lhs_entry =
    *(const H2P_Chain_Load_Profile_Summary_Entry* const*)lhs;
  const H2P_Chain_Load_Profile_Summary_Entry* rhs_entry =
    *(const H2P_Chain_Load_Profile_Summary_Entry* const*)rhs;

  if (lhs_entry->accesses < rhs_entry->accesses)
    return 1;
  if (lhs_entry->accesses > rhs_entry->accesses)
    return -1;
  if (lhs_entry->load_pc < rhs_entry->load_pc)
    return -1;
  if (lhs_entry->load_pc > rhs_entry->load_pc)
    return 1;
  if (lhs_entry->block_start_pc < rhs_entry->block_start_pc)
    return -1;
  if (lhs_entry->block_start_pc > rhs_entry->block_start_pc)
    return 1;
  if (lhs_entry->block_op_idx < rhs_entry->block_op_idx)
    return -1;
  if (lhs_entry->block_op_idx > rhs_entry->block_op_idx)
    return 1;
  return 0;
}

static int h2p_chain_load_pattern_top_cmp(const void* lhs,
                                          const void* rhs) {
  const H2P_Chain_Load_Pattern_Top_Entry* lhs_entry =
    (const H2P_Chain_Load_Pattern_Top_Entry*)lhs;
  const H2P_Chain_Load_Pattern_Top_Entry* rhs_entry =
    (const H2P_Chain_Load_Pattern_Top_Entry*)rhs;

  if (!lhs_entry->valid && !rhs_entry->valid)
    return 0;
  if (!lhs_entry->valid)
    return 1;
  if (!rhs_entry->valid)
    return -1;
  if (lhs_entry->count < rhs_entry->count)
    return 1;
  if (lhs_entry->count > rhs_entry->count)
    return -1;
  if (lhs_entry->delta < rhs_entry->delta)
    return -1;
  if (lhs_entry->delta > rhs_entry->delta)
    return 1;
  return 0;
}

static void h2p_chain_load_pattern_collect_top(
  Flag valid[H2P_CHAIN_LOAD_PATTERN_TOPK],
  SCounter value[H2P_CHAIN_LOAD_PATTERN_TOPK],
  Counter count[H2P_CHAIN_LOAD_PATTERN_TOPK],
  H2P_Chain_Load_Pattern_Top_Entry top[H2P_CHAIN_LOAD_PATTERN_TOPK]) {
  for (int ii = 0; ii < H2P_CHAIN_LOAD_PATTERN_TOPK; ii++) {
    top[ii].valid = valid[ii];
    top[ii].delta = value[ii];
    top[ii].count = count[ii];
  }
  qsort(top, H2P_CHAIN_LOAD_PATTERN_TOPK,
        sizeof(H2P_Chain_Load_Pattern_Top_Entry),
        h2p_chain_load_pattern_top_cmp);
}

static Counter h2p_chain_load_pattern_top_coverage(
  H2P_Chain_Load_Pattern_Top_Entry top[H2P_CHAIN_LOAD_PATTERN_TOPK],
  int top_k) {
  Counter count = 0;
  int limit = MIN2(top_k, H2P_CHAIN_LOAD_PATTERN_TOPK);
  for (int ii = 0; ii < limit; ii++) {
    if (top[ii].valid)
      count += top[ii].count;
  }
  return count;
}

static const char* h2p_chain_load_pattern_classify(
  Counter accesses, Counter unique_values, Counter reuse, Counter zero_delta,
  H2P_Chain_Load_Pattern_Top_Entry top[H2P_CHAIN_LOAD_PATTERN_TOPK]) {
  if (!accesses)
    return "empty";
  if (unique_values == 1 ||
      h2p_chain_profile_pct(zero_delta, reuse) >=
        H2P_CHAIN_LOAD_PATTERN_CONSTANT_PCT)
    return "constant";
  if (top[0].valid && top[0].delta != 0 &&
      h2p_chain_profile_pct(top[0].count, reuse) >=
        H2P_CHAIN_LOAD_PATTERN_SINGLE_STRIDE_PCT)
    return "single_stride";
  if (h2p_chain_profile_pct(
        h2p_chain_load_pattern_top_coverage(top, 4), reuse) >=
      H2P_CHAIN_LOAD_PATTERN_MULTI_STRIDE_PCT)
    return "multi_stride";
  return "irregular";
}

static void h2p_chain_load_profile_dump_entry(
  FILE* file, uns proc_id, H2P_Chain_Load_Profile_Summary_Entry* entry,
  Counter total_accesses, Counter cumulative_accesses, uns rank,
  Flag include_block_key) {
  Counter lower_level_accesses =
    entry->mlc_hit + entry->scarab_l1_hit + entry->mem_access;
  Counter mshr_hit = entry->prefetch_mshr_hit + entry->demand_mshr_hit;
  Counter stride_abs_le_4 = entry->stride_zero + entry->stride_pos_1 +
                            entry->stride_neg_1 +
                            entry->stride_small_abs_le_4;
  Counter stride_abs_le_16 = stride_abs_le_4 +
                             entry->stride_small_abs_le_16;
  double avg_latency = entry->latency_samples ?
    (double)entry->latency_total / (double)entry->latency_samples : 0.0;
  double benefit_score = (double)lower_level_accesses * avg_latency;
  SCounter dominant_stride = 0;
  Counter dominant_stride_count = 0;
  h2p_chain_load_profile_dominant_stride(entry, &dominant_stride,
                                         &dominant_stride_count);

  if (include_block_key) {
    fprintf(file, "%u,0x%llx,%u,0x%llx,",
            (unsigned)proc_id,
            (unsigned long long)entry->block_start_pc,
            (unsigned)entry->block_op_idx,
            (unsigned long long)entry->load_pc);
  } else {
    fprintf(file, "%u,0x%llx,",
            (unsigned)proc_id,
            (unsigned long long)entry->load_pc);
  }

  fprintf(file,
          "%u,%.6f,"
          "%llu,%.6f,"
          "%llu,%.6f,%llu,%.6f,%llu,%.6f,"
          "%llu,%.6f,%llu,%.6f,%llu,%.6f,"
          "%llu,%.6f,%llu,%.6f,%llu,%.6f,%llu,%.6f,"
          "%llu,%llu,%.6f,%llu,%.6f,%.6f,"
          "%llu,%.6f,%llu,%.6f,%llu,%.6f,"
          "%llu,%.6f,%llu,%.6f,%llu,%.6f,"
          "%llu,%.6f,%llu,%.6f,"
          "%lld,%llu,%.6f\n",
          (unsigned)rank,
          h2p_chain_profile_pct(cumulative_accesses, total_accesses),
          (unsigned long long)entry->accesses,
          h2p_chain_profile_pct(entry->accesses, total_accesses),
          (unsigned long long)entry->dcache_hit,
          h2p_chain_profile_pct(entry->dcache_hit, entry->accesses),
          (unsigned long long)entry->store_req_buffer_hit,
          h2p_chain_profile_pct(entry->store_req_buffer_hit, entry->accesses),
          (unsigned long long)entry->dcache_miss,
          h2p_chain_profile_pct(entry->dcache_miss, entry->accesses),
          (unsigned long long)mshr_hit,
          h2p_chain_profile_pct(mshr_hit, entry->accesses),
          (unsigned long long)entry->prefetch_mshr_hit,
          h2p_chain_profile_pct(entry->prefetch_mshr_hit, entry->accesses),
          (unsigned long long)entry->demand_mshr_hit,
          h2p_chain_profile_pct(entry->demand_mshr_hit, entry->accesses),
          (unsigned long long)entry->mlc_hit,
          h2p_chain_profile_pct(entry->mlc_hit, entry->accesses),
          (unsigned long long)entry->scarab_l1_hit,
          h2p_chain_profile_pct(entry->scarab_l1_hit, entry->accesses),
          (unsigned long long)entry->mem_access,
          h2p_chain_profile_pct(entry->mem_access, entry->accesses),
          (unsigned long long)lower_level_accesses,
          h2p_chain_profile_pct(lower_level_accesses, entry->accesses),
          (unsigned long long)entry->latency_samples,
          (unsigned long long)entry->latency_total,
          avg_latency,
          (unsigned long long)entry->reuse,
          h2p_chain_profile_pct(entry->reuse, entry->accesses),
          benefit_score,
          (unsigned long long)entry->same_va,
          h2p_chain_profile_pct(entry->same_va, entry->reuse),
          (unsigned long long)entry->same_va_line,
          h2p_chain_profile_pct(entry->same_va_line, entry->reuse),
          (unsigned long long)entry->stride_zero,
          h2p_chain_profile_pct(entry->stride_zero, entry->reuse),
          (unsigned long long)entry->stride_pos_1,
          h2p_chain_profile_pct(entry->stride_pos_1, entry->reuse),
          (unsigned long long)entry->stride_neg_1,
          h2p_chain_profile_pct(entry->stride_neg_1, entry->reuse),
          (unsigned long long)stride_abs_le_4,
          h2p_chain_profile_pct(stride_abs_le_4, entry->reuse),
          (unsigned long long)stride_abs_le_16,
          h2p_chain_profile_pct(stride_abs_le_16, entry->reuse),
          (unsigned long long)entry->stride_other,
          h2p_chain_profile_pct(entry->stride_other, entry->reuse),
          (long long)dominant_stride,
          (unsigned long long)dominant_stride_count,
          h2p_chain_profile_pct(dominant_stride_count, entry->reuse));
}

static void h2p_chain_load_profile_dump_table(
  const char* name, H2P_Chain_Load_Profile_Summary_Entry** tables,
  uns table_size, Flag include_block_key) {
  FILE* file = file_tag_fopen(OUTPUT_DIR, name, "w");
  if (!file)
    return;

  if (include_block_key)
    fprintf(file, "proc_id,block_start_pc,block_op_idx,load_pc,");
  else
    fprintf(file, "proc_id,load_pc,");

  fprintf(file,
          "rank,cumulative_coverage_pct,"
          "accesses,coverage_pct,"
          "dcache_hit,dcache_hit_pct,"
          "store_req_buffer_hit_after_l1_miss,"
          "store_req_buffer_hit_after_l1_miss_pct,"
          "dcache_miss,dcache_miss_pct,"
          "mshr_hit,mshr_hit_pct,"
          "prefetch_mshr_hit,prefetch_mshr_hit_pct,"
          "demand_mshr_hit,demand_mshr_hit_pct,"
          "mlc_hit,mlc_hit_pct,"
          "scarab_l1_hit,scarab_l1_hit_pct,"
          "mem_access,mem_access_pct,"
          "lower_level_accesses,lower_level_accesses_pct,"
          "latency_samples,latency_total,avg_latency,"
          "reuse,reuse_pct_of_access,benefit_score,"
          "same_va,same_va_pct_of_reuse,"
          "same_line,same_line_pct_of_reuse,"
          "stride_zero,stride_zero_pct_of_reuse,"
          "stride_pos1,stride_pos1_pct_of_reuse,"
          "stride_neg1,stride_neg1_pct_of_reuse,"
          "stride_abs_le4_incl_0_1,stride_abs_le4_incl_0_1_pct_of_reuse,"
          "stride_abs_le16_incl_le4,"
          "stride_abs_le16_incl_le4_pct_of_reuse,"
          "stride_other,stride_other_pct_of_reuse,"
          "dominant_stride,dominant_stride_count,"
          "dominant_stride_pct_of_reuse\n");

  for (uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    H2P_Chain_Load_Profile_Summary_Entry* table = tables[proc_id];
    if (!table)
      continue;

    Counter total_accesses = 0;
    uns valid_count = 0;
    for (uns ii = 0; ii < table_size; ii++) {
      if (table[ii].valid) {
        total_accesses += table[ii].accesses;
        valid_count++;
      }
    }

    if (!valid_count)
      continue;

    H2P_Chain_Load_Profile_Summary_Entry** sorted_entries =
      (H2P_Chain_Load_Profile_Summary_Entry**)calloc(
        valid_count, sizeof(H2P_Chain_Load_Profile_Summary_Entry*));
    ASSERT(proc_id, sorted_entries);

    uns valid_idx = 0;
    for (uns ii = 0; ii < table_size; ii++) {
      if (table[ii].valid)
        sorted_entries[valid_idx++] = &table[ii];
    }

    qsort(sorted_entries, valid_count,
          sizeof(H2P_Chain_Load_Profile_Summary_Entry*),
          h2p_chain_load_profile_entry_access_cmp);

    Counter cumulative_accesses = 0;
    for (uns ii = 0; ii < valid_count; ii++) {
      cumulative_accesses += sorted_entries[ii]->accesses;
      h2p_chain_load_profile_dump_entry(file, proc_id, sorted_entries[ii],
                                        total_accesses,
                                        cumulative_accesses, ii + 1,
                                        include_block_key);
    }

    free(sorted_entries);
  }

  fclose(file);
}

static void h2p_chain_load_pattern_dump_pc_entry(
  FILE* file, uns proc_id, H2P_Chain_Load_Profile_Summary_Entry* entry,
  Counter total_accesses, Counter cumulative_accesses, uns rank) {
  H2P_Chain_Load_Pattern_Top_Entry byte_top[H2P_CHAIN_LOAD_PATTERN_TOPK];
  H2P_Chain_Load_Pattern_Top_Entry line_top[H2P_CHAIN_LOAD_PATTERN_TOPK];
  h2p_chain_load_pattern_collect_top(entry->byte_delta_valid,
                                     entry->byte_delta_value,
                                     entry->byte_delta_count, byte_top);
  h2p_chain_load_pattern_collect_top(entry->line_delta_valid,
                                     entry->line_delta_value,
                                     entry->line_delta_count, line_top);

  Counter byte_top2 = h2p_chain_load_pattern_top_coverage(byte_top, 2);
  Counter byte_top4 = h2p_chain_load_pattern_top_coverage(byte_top, 4);
  Counter byte_top8 = h2p_chain_load_pattern_top_coverage(byte_top, 8);
  Counter line_top2 = h2p_chain_load_pattern_top_coverage(line_top, 2);
  Counter line_top4 = h2p_chain_load_pattern_top_coverage(line_top, 4);
  Counter line_top8 = h2p_chain_load_pattern_top_coverage(line_top, 8);

  fprintf(file,
          "%u,0x%llx,%u,%.6f,"
          "%llu,%llu,%.6f,%llu,%.6f,"
          "%llu,%llu,%.6f,%llu,%.6f,"
          "%lld,%llu,%.6f,%.6f,%.6f,%.6f,"
          "%lld,%llu,%.6f,%.6f,%.6f,%.6f,"
          "%s,%s\n",
          (unsigned)proc_id,
          (unsigned long long)entry->load_pc,
          (unsigned)rank,
          h2p_chain_profile_pct(cumulative_accesses, total_accesses),
          (unsigned long long)entry->accesses,
          (unsigned long long)entry->unique_vaddr,
          h2p_chain_profile_pct(entry->unique_vaddr, entry->accesses),
          (unsigned long long)entry->unique_line,
          h2p_chain_profile_pct(entry->unique_line, entry->accesses),
          (unsigned long long)entry->reuse,
          (unsigned long long)entry->same_va,
          h2p_chain_profile_pct(entry->same_va, entry->reuse),
          (unsigned long long)entry->same_va_line,
          h2p_chain_profile_pct(entry->same_va_line, entry->reuse),
          byte_top[0].valid ? (long long)byte_top[0].delta : 0,
          (unsigned long long)(byte_top[0].valid ? byte_top[0].count : 0),
          h2p_chain_profile_pct(byte_top[0].valid ? byte_top[0].count : 0,
                                entry->reuse),
          h2p_chain_profile_pct(byte_top2, entry->reuse),
          h2p_chain_profile_pct(byte_top4, entry->reuse),
          h2p_chain_profile_pct(byte_top8, entry->reuse),
          line_top[0].valid ? (long long)line_top[0].delta : 0,
          (unsigned long long)(line_top[0].valid ? line_top[0].count : 0),
          h2p_chain_profile_pct(line_top[0].valid ? line_top[0].count : 0,
                                entry->reuse),
          h2p_chain_profile_pct(line_top2, entry->reuse),
          h2p_chain_profile_pct(line_top4, entry->reuse),
          h2p_chain_profile_pct(line_top8, entry->reuse),
          h2p_chain_load_pattern_classify(entry->accesses,
                                          entry->unique_vaddr,
                                          entry->reuse, entry->same_va,
                                          byte_top),
          h2p_chain_load_pattern_classify(entry->accesses,
                                          entry->unique_line,
                                          entry->reuse,
                                          entry->same_va_line, line_top));
}

static void h2p_chain_load_pattern_dump_pc_profile(void) {
  FILE* file = file_tag_fopen(OUTPUT_DIR,
                              "h2p_chain_load_pc_pattern_profile", "w");
  if (!file)
    return;

  fprintf(file,
          "proc_id,load_pc,rank,cumulative_coverage_pct,"
          "accesses,unique_vaddr,unique_vaddr_pct_of_access,"
          "unique_line,unique_line_pct_of_access,"
          "reuse,same_va,zero_byte_delta_pct_of_reuse,"
          "same_line,zero_line_delta_pct_of_reuse,"
          "dominant_byte_delta,dominant_byte_delta_count,"
          "dominant_byte_delta_pct_of_reuse,"
          "top2_byte_delta_coverage_pct_of_reuse,"
          "top4_byte_delta_coverage_pct_of_reuse,"
          "top8_byte_delta_coverage_pct_of_reuse,"
          "dominant_line_delta,dominant_line_delta_count,"
          "dominant_line_delta_pct_of_reuse,"
          "top2_line_delta_coverage_pct_of_reuse,"
          "top4_line_delta_coverage_pct_of_reuse,"
          "top8_line_delta_coverage_pct_of_reuse,"
          "byte_pattern_class,line_pattern_class\n");

  for (uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    H2P_Chain_Load_Profile_Summary_Entry* table =
      h2p_chain_load_pc_profile_table[proc_id];
    if (!table)
      continue;

    Counter total_accesses = 0;
    uns valid_count = 0;
    for (uns ii = 0; ii < H2P_CHAIN_LOAD_PROFILE_PC_TABLE_SIZE; ii++) {
      if (table[ii].valid) {
        total_accesses += table[ii].accesses;
        valid_count++;
      }
    }
    if (!valid_count)
      continue;

    H2P_Chain_Load_Profile_Summary_Entry** sorted_entries =
      (H2P_Chain_Load_Profile_Summary_Entry**)calloc(
        valid_count, sizeof(H2P_Chain_Load_Profile_Summary_Entry*));
    ASSERT(proc_id, sorted_entries);

    uns valid_idx = 0;
    for (uns ii = 0; ii < H2P_CHAIN_LOAD_PROFILE_PC_TABLE_SIZE; ii++) {
      if (table[ii].valid)
        sorted_entries[valid_idx++] = &table[ii];
    }

    qsort(sorted_entries, valid_count,
          sizeof(H2P_Chain_Load_Profile_Summary_Entry*),
          h2p_chain_load_profile_entry_access_cmp);

    Counter cumulative_accesses = 0;
    for (uns ii = 0; ii < valid_count; ii++) {
      cumulative_accesses += sorted_entries[ii]->accesses;
      h2p_chain_load_pattern_dump_pc_entry(file, proc_id, sorted_entries[ii],
                                           total_accesses,
                                           cumulative_accesses, ii + 1);
    }
    free(sorted_entries);
  }

  fclose(file);
}

typedef struct H2P_Chain_Load_Delta_Dump_Arg_struct {
  FILE* file;
  uns proc_id;
  const char* delta_type;
} H2P_Chain_Load_Delta_Dump_Arg;

typedef struct H2P_Chain_Load_Addr_Repeatability_Dump_Arg_struct {
  Hash_Table* histogram;
  Counter total_target_load_accesses;
  Counter total_unique_pc_vaddr;
} H2P_Chain_Load_Addr_Repeatability_Dump_Arg;

static void h2p_chain_load_pattern_dump_delta_entry(void* data, void* arg) {
  H2P_Chain_Load_Pattern_Delta_Key* entry =
    (H2P_Chain_Load_Pattern_Delta_Key*)data;
  H2P_Chain_Load_Delta_Dump_Arg* dump_arg =
    (H2P_Chain_Load_Delta_Dump_Arg*)arg;

  fprintf(dump_arg->file, "%u,%s,0x%llx,%lld,%llu\n",
          (unsigned)dump_arg->proc_id, dump_arg->delta_type,
          (unsigned long long)entry->load_pc, (long long)entry->delta,
          (unsigned long long)entry->count);
}

static void h2p_chain_load_pattern_dump_delta_histogram(void) {
  FILE* file = file_tag_fopen(OUTPUT_DIR,
                              "h2p_chain_load_pc_delta_histogram", "w");
  if (!file)
    return;

  fprintf(file, "proc_id,delta_type,load_pc,delta,count\n");

  for (uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    H2P_Chain_Load_Delta_Dump_Arg arg;
    arg.file = file;
    arg.proc_id = proc_id;

    if (h2p_chain_load_byte_delta_table_valid[proc_id]) {
      arg.delta_type = "byte";
      hash_table_scan(&h2p_chain_load_byte_delta_table[proc_id],
                      h2p_chain_load_pattern_dump_delta_entry, &arg);
    }
    if (h2p_chain_load_line_delta_table_valid[proc_id]) {
      arg.delta_type = "line";
      hash_table_scan(&h2p_chain_load_line_delta_table[proc_id],
                      h2p_chain_load_pattern_dump_delta_entry, &arg);
    }
  }

  fclose(file);
}

static void h2p_chain_load_pattern_collect_addr_repeatability(void* data,
                                                             void* arg) {
  H2P_Chain_Load_Pattern_Value_Key* value_entry =
    (H2P_Chain_Load_Pattern_Value_Key*)data;
  H2P_Chain_Load_Addr_Repeatability_Dump_Arg* dump_arg =
    (H2P_Chain_Load_Addr_Repeatability_Dump_Arg*)arg;

  if (!value_entry->count)
    return;

  Flag new_entry = FALSE;
  H2P_Chain_Load_Addr_Repeatability_Entry* repeat_entry =
    (H2P_Chain_Load_Addr_Repeatability_Entry*)hash_table_access_create(
      dump_arg->histogram, (int64)value_entry->count, &new_entry);
  if (new_entry) {
    repeat_entry->repeatability = value_entry->count;
    repeat_entry->target_load_accesses = 0;
    repeat_entry->unique_pc_vaddr_count = 0;
  }

  repeat_entry->target_load_accesses += value_entry->count;
  repeat_entry->unique_pc_vaddr_count++;
  dump_arg->total_target_load_accesses += value_entry->count;
  dump_arg->total_unique_pc_vaddr++;
}

static int h2p_chain_load_addr_repeatability_entry_cmp(const void* lhs,
                                                       const void* rhs) {
  H2P_Chain_Load_Addr_Repeatability_Entry const* lhs_entry =
    *(H2P_Chain_Load_Addr_Repeatability_Entry const* const*)lhs;
  H2P_Chain_Load_Addr_Repeatability_Entry const* rhs_entry =
    *(H2P_Chain_Load_Addr_Repeatability_Entry const* const*)rhs;

  if (lhs_entry->repeatability < rhs_entry->repeatability)
    return -1;
  if (lhs_entry->repeatability > rhs_entry->repeatability)
    return 1;
  return 0;
}

static void h2p_chain_load_pattern_dump_addr_repeatability_histogram(void) {
  FILE* file = file_tag_fopen(OUTPUT_DIR,
                              "h2p_chain_load_addr_repeatability_histogram",
                              "w");
  if (!file)
    return;

  fprintf(file,
          "proc_id,repeatability,target_load_accesses,"
          "unique_pc_vaddr_count,fraction_of_target_load_accesses_pct\n");

  for (uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    if (!h2p_chain_load_unique_vaddr_table_valid[proc_id])
      continue;

    Hash_Table histogram;
    init_hash_table(&histogram, "H2P chain load address repeatability",
                    H2P_CHAIN_LOAD_REPEATABILITY_HIST_BUCKETS,
                    sizeof(H2P_Chain_Load_Addr_Repeatability_Entry));

    H2P_Chain_Load_Addr_Repeatability_Dump_Arg arg;
    arg.histogram = &histogram;
    arg.total_target_load_accesses = 0;
    arg.total_unique_pc_vaddr = 0;

    hash_table_scan(&h2p_chain_load_unique_vaddr_table[proc_id],
                    h2p_chain_load_pattern_collect_addr_repeatability, &arg);

    if (histogram.count) {
      H2P_Chain_Load_Addr_Repeatability_Entry** entries =
        (H2P_Chain_Load_Addr_Repeatability_Entry**)hash_table_flatten(
          &histogram, NULL);
      ASSERT(proc_id, entries);
      qsort(entries, histogram.count,
            sizeof(H2P_Chain_Load_Addr_Repeatability_Entry*),
            h2p_chain_load_addr_repeatability_entry_cmp);

      for (int ii = 0; ii < histogram.count; ii++) {
        H2P_Chain_Load_Addr_Repeatability_Entry* entry = entries[ii];
        fprintf(file, "%u,%llu,%llu,%llu,%.6f\n", (unsigned)proc_id,
                (unsigned long long)entry->repeatability,
                (unsigned long long)entry->target_load_accesses,
                (unsigned long long)entry->unique_pc_vaddr_count,
                h2p_chain_profile_pct(entry->target_load_accesses,
                                      arg.total_target_load_accesses));
      }

      free(entries);
    }

    hash_table_clear(&histogram);
    free(histogram.entries);
    free(histogram.name);
  }

  fclose(file);
}

static void h2p_chain_load_pattern_dump_meta(void) {
  FILE* file = file_tag_fopen(OUTPUT_DIR,
                              "h2p_chain_load_pattern_profile_meta", "w");
  if (!file)
    return;

  fprintf(file,
          "proc_id,hash_buckets,unique_vaddr_entries,unique_line_entries,"
          "byte_delta_entries,line_delta_entries\n");
  for (uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    if (h2p_chain_load_unique_vaddr_table_valid[proc_id] ||
        h2p_chain_load_unique_line_table_valid[proc_id] ||
        h2p_chain_load_byte_delta_table_valid[proc_id] ||
        h2p_chain_load_line_delta_table_valid[proc_id]) {
      fprintf(file, "%u,%u,%d,%d,%d,%d\n", (unsigned)proc_id,
              (unsigned)h2p_chain_load_pattern_buckets(),
              h2p_chain_load_unique_vaddr_table_valid[proc_id] ?
                h2p_chain_load_unique_vaddr_table[proc_id].count : 0,
              h2p_chain_load_unique_line_table_valid[proc_id] ?
                h2p_chain_load_unique_line_table[proc_id].count : 0,
              h2p_chain_load_byte_delta_table_valid[proc_id] ?
                h2p_chain_load_byte_delta_table[proc_id].count : 0,
              h2p_chain_load_line_delta_table_valid[proc_id] ?
                h2p_chain_load_line_delta_table[proc_id].count : 0);
    }
  }

  fclose(file);
}

static void dump_h2p_chain_load_profile_tables(void) {
  h2p_chain_load_raw_stream_close();

  if (!H2P_CHAIN_LOAD_PROFILE)
    return;

  h2p_chain_load_profile_dump_table("h2p_chain_load_pc_profile",
                                    h2p_chain_load_pc_profile_table,
                                    H2P_CHAIN_LOAD_PROFILE_PC_TABLE_SIZE,
                                    FALSE);
  h2p_chain_load_profile_dump_table("h2p_chain_load_block_slot_profile",
                                    h2p_chain_load_slot_profile_table,
                                    H2P_CHAIN_LOAD_PROFILE_SLOT_TABLE_SIZE,
                                    TRUE);
  if (H2P_CHAIN_LOAD_PATTERN_PROFILE) {
    h2p_chain_load_pattern_dump_pc_profile();
    h2p_chain_load_pattern_dump_delta_histogram();
    h2p_chain_load_pattern_dump_addr_repeatability_histogram();
    h2p_chain_load_pattern_dump_meta();
  }

  FILE* file = file_tag_fopen(OUTPUT_DIR, "h2p_chain_load_profile_meta", "w");
  if (!file)
    return;
  fprintf(file, "proc_id,pc_table_overflow,slot_table_overflow\n");
  for (uns proc_id = 0; proc_id < MAX_NUM_PROCS; proc_id++) {
    if (h2p_chain_load_pc_profile_table[proc_id] ||
        h2p_chain_load_slot_profile_table[proc_id] ||
        h2p_chain_load_pc_profile_overflow[proc_id] ||
        h2p_chain_load_slot_profile_overflow[proc_id]) {
      fprintf(file, "%u,%llu,%llu\n", (unsigned)proc_id,
              (unsigned long long)h2p_chain_load_pc_profile_overflow[proc_id],
              (unsigned long long)h2p_chain_load_slot_profile_overflow[proc_id]);
    }
  }
  fclose(file);
}

static inline void dcache_hit_wp_collect_stats(Dcache_Data* line, Op* op) {
  if (!WP_COLLECT_STATS)
    return;

  if (!line) {
    ASSERT(dc->proc_id, PERFECT_DCACHE);
    return;
  }

  if (op->off_path) {
    if (line->fetched_by_offpath) {
      STAT_EVENT(dc->proc_id, DCACHE_HIT_OFFPATH_SAT_BY_OFFPATH);
    } else {
      STAT_EVENT(dc->proc_id, DCACHE_HIT_OFFPATH_SAT_BY_ONPATH);
    }
    return;
  }

  if (!line->fetched_by_offpath) {
    STAT_EVENT(dc->proc_id, DCACHE_HIT_ONPATH_SAT_BY_ONPATH);
    STAT_EVENT(dc->proc_id, DCACHE_USE_ONPATH);
    return;
  }
  line->fetched_by_offpath = FALSE;

  STAT_EVENT(dc->proc_id, DCACHE_HIT_ONPATH_SAT_BY_OFFPATH);
  STAT_EVENT(dc->proc_id, DCACHE_USE_OFFPATH);
  STAT_EVENT(dc->proc_id, DIST_DCACHE_FILL_OFFPATH_USED);
  STAT_EVENT(dc->proc_id, DIST_REQBUF_OFFPATH_USED);
  STAT_EVENT(dc->proc_id, DIST2_REQBUF_OFFPATH_USED_FULL);

  L1_Data* l1_line = do_l1_access(op);
  if (l1_line) {
    if (l1_line->fetched_by_offpath) {
      STAT_EVENT(dc->proc_id, L1_USE_OFFPATH);
      STAT_EVENT(dc->proc_id, DIST_L1_FILL_OFFPATH_USED);
      STAT_EVENT(dc->proc_id, L1_USE_OFFPATH_DATA);
      l1_line->fetched_by_offpath = FALSE;
      l1_line->l0_modified_fetched_by_offpath = TRUE;
    }
  }

  DEBUG(0, "Dcache hit: On path hits off path. va:%s op:%s op:0x%s wp_op:0x%s opu:%s wpu:%s dist:%s%s\n",
        hexstr64s(op->oracle_info.va), disasm_op(op, TRUE), hexstr64s(op->inst_info->addr),
        hexstr64s(line->offpath_op_addr), unsstr64(op->unique_num), unsstr64(line->offpath_op_unique),
        op->unique_num > line->offpath_op_unique ? " " : "-",
        op->unique_num > line->offpath_op_unique ? unsstr64(op->unique_num - line->offpath_op_unique)
                                                 : unsstr64(line->offpath_op_unique - op->unique_num));
}

static inline void dcache_fill_wp_collect_stats(Dcache_Data* line, Mem_Req* req) {
  if (!WP_COLLECT_STATS)
    return;

  if ((req->type == MRT_WB) || (req->type == MRT_WB_NODIRTY) ||
      (req->type == MRT_DPRF)) /* for now we don't consider prefetches */
    return;

  if (req->off_path) {
    switch (req->type) {
      case MRT_DFETCH:
      case MRT_DSTORE:
        STAT_EVENT(dc->proc_id, DCACHE_FILL_OFFPATH);
        STAT_EVENT(dc->proc_id, DIST_DCACHE_FILL);
        break;
      default:
        break;
    }
  } else {
    switch (req->type) {
      case MRT_DFETCH:
      case MRT_DSTORE:
        STAT_EVENT(dc->proc_id, DCACHE_FILL_ONPATH);
        STAT_EVENT(dc->proc_id, DIST_DCACHE_FILL);
        if (req->onpath_match_offpath)
          STAT_EVENT(dc->proc_id, DIST_DCACHE_FILL_ONPATH_PARTIAL);
        else
          STAT_EVENT(dc->proc_id, DIST_DCACHE_FILL_ONPATH);
        break;
      default:
        break;
    }
  }
}

static inline void dcache_miss_extra_access(Op* op, Cache* cache, Addr line_addr, uns8 proc_id, uns8 cache_cycle) {
  Addr one_more_addr;
  Addr extra_line_addr;
  Dcache_Data* extra_line;

  one_more_addr = ((line_addr >> LOG2(cache->line_size)) & 1)
                      ? ((line_addr >> LOG2(cache->line_size)) - 1) << LOG2(cache->line_size)
                      : ((line_addr >> LOG2(cache->line_size)) + 1) << LOG2(cache->line_size);

  extra_line = (Dcache_Data*)cache_access(cache, one_more_addr, &extra_line_addr, FALSE);
  ASSERT(proc_id, one_more_addr == extra_line_addr);

  if (extra_line) {
    STAT_EVENT_ALL(ONE_MORE_DISCARDED_L0CACHE);
    return;
  }

  Flag ret = new_mem_req(MRT_DFETCH, proc_id, extra_line_addr, cache->line_size,
                         cache_cycle - 1 + op->inst_info->extra_ld_latency, NULL, NULL, op->unique_num, 0);
  if (ret)
    STAT_EVENT_ALL(ONE_MORE_SUCESS);
  else
    STAT_EVENT_ALL(ONE_MORE_DISCARDED_MEM_REQ_FULL);
}

static inline Flag dcache_miss_new_mem_req(Op* op, Addr line_addr, Mem_Req_Type mem_req_type) {
  return new_mem_req((mem_req_type), dc->proc_id, line_addr, DCACHE_LINE_SIZE,
                     DCACHE_CYCLES - 1 + op->inst_info->extra_ld_latency, op, dcache_fill_line, op->unique_num, 0);
}

static inline void dcache_cacheline_hit(Op* op, Addr line_addr, Dcache_Data* line) {
  /* prefetching handle */
  if (PREF_FRAMEWORK_ON && (PREF_UPDATE_ON_WRONGPATH || !op->off_path)) {
    // if framework is on use new prefetcher. otherwise old one
    if (line->HW_prefetch) {
      pref_dl0_pref_hit(line_addr, op->inst_info->addr, 0);  // CHANGEME
      line->HW_prefetch = FALSE;
    } else {
      pref_dl0_hit(line_addr, op->inst_info->addr);
    }
  } else if ((STREAM_TRAIN_ON_WRONGPATH || !op->off_path) && line->HW_prefetch) {
    // old prefetcher code
    STAT_EVENT(op->proc_id, DCACHE_PREF_HIT);
    STAT_EVENT(op->proc_id, STREAM_DCACHE_PREF_HIT);
    line->HW_prefetch = FALSE;  // not anymore prefetched data
    if (L2L1PREF_ON)
      l2l1pref_dcache(line_addr, op);
    if (STREAM_PREFETCH_ON && STREAM_PREF_INTO_DCACHE) {
      stream_dl0_hit_train(line_addr);
    }
  }
  if (L2L1PREF_ON && L2L1_DC_HIT_TRAIN) {
    l2l1pref_dcache(line_addr, op);
  }

  /* update stats */
  dcache_hit_wp_collect_stats(line, op);
  if (!op->off_path) {
    STAT_EVENT(op->proc_id, DCACHE_HIT);
    STAT_EVENT(op->proc_id, DCACHE_HIT_ONPATH);
  } else {
    STAT_EVENT(op->proc_id, DCACHE_HIT_OFFPATH);
  }

  /* update cacheline state */
  op->done_cycle = cycle_count + DCACHE_CYCLES + op->inst_info->extra_ld_latency;
  line->read_count[op->off_path] = line->read_count[op->off_path] + (op->table_info->mem_type == MEM_LD);
  line->write_count[op->off_path] = line->write_count[op->off_path] + (op->table_info->mem_type == MEM_ST);
  line->misc_state = (line->misc_state & 2) | op->off_path;
  if (!op->off_path) {
    line->dirty |= op->table_info->mem_type == MEM_ST;
  }

  /* wake up source inst if the op is completed */
  if (op->table_info->mem_type != MEM_ST) {
    op->wake_cycle = op->done_cycle;
    wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
  }
}

static inline void dcache_cacheline_miss(Op* op, Addr line_addr) {
  if (op->table_info->mem_type == MEM_ST)
    STAT_EVENT(op->proc_id, POWER_DCACHE_WRITE_MISS);
  else
    STAT_EVENT(op->proc_id, POWER_DCACHE_READ_MISS);

  if (CACHE_STAT_ENABLE)
    dc_miss_stat(op);

  Flag wrongpath_dcmiss = FALSE;

  switch (op->table_info->mem_type) {
    case MEM_LD:
      // scan the store forwarding buffer
      if (scan_stores(op->oracle_info.va, op->oracle_info.mem_size)) {
        if (!op->off_path) {
          STAT_EVENT(op->proc_id, DCACHE_ST_BUFFER_HIT);
          STAT_EVENT(op->proc_id, DCACHE_ST_BUFFER_HIT_ONPATH);
        } else {
          STAT_EVENT(op->proc_id, DCACHE_ST_BUFFER_HIT_OFFPATH);
        }

        op->done_cycle = cycle_count + DCACHE_CYCLES + op->inst_info->extra_ld_latency;
        op->wake_cycle = cycle_count + DCACHE_CYCLES + op->inst_info->extra_ld_latency;
        wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
        dcache_stage_record_main_chain_load_profile_store_fwd(op);
        /* TEA load via normal dcache (store fwd hit on miss): mark completed */
        if (TEA_ENABLE && op->thread_id == 1) {
          /* [EXPERIMENT: TEA_PERFECT_LOAD] main store-scan forward-on-miss stats.
           * This is NOT an L1 hit — it's a dcache_cacheline_miss() path where
           * scan_stores() found a matching store in the memory request buffer. */
          Counter _lat = op->done_cycle - cycle_count;
          STAT_EVENT(dc->proc_id, TEA_LOADS_EXECUTED);
          STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_ACCESS);
          STAT_EVENT(dc->proc_id, TEA_LOADS_STORE_SCAN_FWD);
          STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_SAMPLES);
          INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_TOTAL, _lat);
          INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_AVG,   _lat);
          tea_chain_note_load_result(dc->proc_id, op,
                                     TEA_LOAD_RESULT_STORE_SCAN_FWD,
                                     _lat);
          tea_log_chain_load_addr(op, line_addr, "STORE_SCAN_FWD", _lat);
          tea_op_completed(dc->proc_id, op);
        }
        break;
      }

      if (!(model->mem == MODEL_MEM) || !dcache_miss_new_mem_req(op, line_addr, MRT_DFETCH)) {
        op->state = OS_WAIT_MEM;  // go into this state if no miss buffer is available
        cmp_model.node_stage[dc->proc_id].mem_blocked = TRUE;
        mem->uncores[dc->proc_id].mem_block_start = freq_cycle_count(FREQ_DOMAIN_L1);
        STAT_EVENT(op->proc_id, DCACHE_MISS_WAITMEM);
        break;
      }

      if (PREF_UPDATE_ON_WRONGPATH || !op->off_path) {
        pref_dl0_miss(line_addr, op->inst_info->addr);
      }

      if (ONE_MORE_CACHE_LINE_ENABLE) {
        dcache_miss_extra_access(op, &dc->dcache, line_addr, dc->proc_id, DCACHE_CYCLES);
      }

      if (!op->off_path) {
        STAT_EVENT(op->proc_id, DCACHE_MISS);
        STAT_EVENT(op->proc_id, DCACHE_MISS_ONPATH);
        STAT_EVENT(op->proc_id, DCACHE_MISS_LD_ONPATH);
        op->oracle_info.dcmiss = TRUE;
        STAT_EVENT(op->proc_id, DCACHE_MISS_LD);
      } else {
        STAT_EVENT(op->proc_id, DCACHE_MISS_OFFPATH);
        STAT_EVENT(op->proc_id, DCACHE_MISS_LD_OFFPATH);
        wrongpath_dcmiss = FALSE;
      }
      op->state = OS_MISS;
      op->engine_info.dcmiss = TRUE;
      break;

    case MEM_PF:
    case MEM_WH:
      if (!(model->mem == MODEL_MEM) || !dcache_miss_new_mem_req(op, line_addr, MRT_DPRF)) {
        op->state = OS_WAIT_MEM;  // go into this state if no miss buffer is available
        cmp_model.node_stage[dc->proc_id].mem_blocked = TRUE;
        mem->uncores[dc->proc_id].mem_block_start = freq_cycle_count(FREQ_DOMAIN_L1);
        STAT_EVENT(op->proc_id, DCACHE_MISS_WAITMEM);
        break;
      }

      if (ONE_MORE_CACHE_LINE_ENABLE) {
        dcache_miss_extra_access(op, &dc->dcache, line_addr, dc->proc_id, DCACHE_CYCLES);
      }

      if (!op->off_path) {
        STAT_EVENT(op->proc_id, DCACHE_MISS);
        STAT_EVENT(op->proc_id, DCACHE_MISS_ONPATH);
        STAT_EVENT(op->proc_id, DCACHE_MISS_LD_ONPATH);
        op->oracle_info.dcmiss = TRUE;
        STAT_EVENT(op->proc_id, DCACHE_MISS_LD);
      } else {
        STAT_EVENT(op->proc_id, DCACHE_MISS_OFFPATH);
        STAT_EVENT(op->proc_id, DCACHE_MISS_LD_OFFPATH);
        wrongpath_dcmiss = FALSE;
      }
      op->state = OS_MISS;
      if (PREFS_DO_NOT_BLOCK_WINDOW || op->table_info->mem_type == MEM_PF) {
        op->done_cycle = cycle_count + DCACHE_CYCLES + op->inst_info->extra_ld_latency;
        op->state = OS_SCHEDULED;
      }
      break;

    case MEM_ST:
      if (!(model->mem == MODEL_MEM) || !dcache_miss_new_mem_req(op, line_addr, MRT_DSTORE)) {
        op->state = OS_WAIT_MEM;
        cmp_model.node_stage[dc->proc_id].mem_blocked = TRUE;
        mem->uncores[dc->proc_id].mem_block_start = freq_cycle_count(FREQ_DOMAIN_L1);
        STAT_EVENT(op->proc_id, DCACHE_MISS_WAITMEM);
        break;
      }

      if (ONE_MORE_CACHE_LINE_ENABLE) {
        dcache_miss_extra_access(op, &dc->dcache, line_addr, dc->proc_id, DCACHE_CYCLES);
      }

      if (!op->off_path) {
        STAT_EVENT(op->proc_id, DCACHE_MISS);
        STAT_EVENT(op->proc_id, DCACHE_MISS_ONPATH);
        STAT_EVENT(op->proc_id, DCACHE_MISS_ST_ONPATH);
        op->oracle_info.dcmiss = TRUE;
        STAT_EVENT(op->proc_id, DCACHE_MISS_ST);
      } else {
        STAT_EVENT(op->proc_id, DCACHE_MISS_OFFPATH);
        STAT_EVENT(op->proc_id, DCACHE_MISS_ST_OFFPATH);
        wrongpath_dcmiss = FALSE;
      }
      op->state = OS_MISS;
      if (STORES_DO_NOT_BLOCK_WINDOW) {
        op->done_cycle = cycle_count + DCACHE_CYCLES + op->inst_info->extra_ld_latency;
        op->state = OS_SCHEDULED;
      }
      break;

    default:
      ASSERT(dc->proc_id, FALSE);
      break;
  }

  if (STREAM_PREFETCH_ON && (op->oracle_info.dcmiss || (STREAM_TRAIN_ON_WRONGPATH && wrongpath_dcmiss))) {
    _DEBUG(dc->proc_id, DEBUG_STREAM_MEM, "dl0 miss : line_addr :%d op_count %lld  type :%d\n", (int)line_addr,
           op->op_num, (int)op->table_info->mem_type);
    stream_dl0_miss(line_addr);
  }
}

static inline Dcache_Data* dcache_fill_get_cacheline(Mem_Req* req) {
  Dcache_Data* data;
  Addr line_addr, repl_line_addr;

  /* Prefetch */
  bool is_off_path = USE_CONFIRMED_OFF ? req->off_path_confirmed : req->off_path;
  bool is_prefetch = (req->type == MRT_DPRF);
  if (DC_PREF_CACHE_ENABLE && (is_off_path || is_prefetch)) {
    DEBUG(dc->proc_id,
          "Filling pref_dcache off_path:%d addr:0x%s  :%7d index:%7d "
          "op_count:%d oldest:%lld\n",
          req->off_path, hexstr64s(req->addr), (int)req->addr, (int)(req->addr >> LOG2(DCACHE_LINE_SIZE)),
          req->op_count, (req->op_count ? req->oldest_op_unique_num : -1));

    data = (Dcache_Data*)cache_insert(&dc->pref_dcache, dc->proc_id, req->addr, &line_addr, &repl_line_addr);
    ASSERT(dc->proc_id, req->emitted_cycle);
    ASSERT(dc->proc_id, cycle_count >= req->emitted_cycle);
    /*
     * mark the data as HW_prefetch if prefetch mark it as fetched_by_offpath if off_path
     * this is done downstairs
     */
    ASSERT(dc->proc_id, data != NULL);
    return data;
  }

  /*
   * Do not insert the line yet, just check which line we need to replace.
   * If that line is dirty, it's possible that we won't be able to insert the writeback into the memory system.
   */
  Flag repl_line_valid;
  data = (Dcache_Data*)get_next_repl_line(&dc->dcache, dc->proc_id, req->addr, &repl_line_addr, &repl_line_valid);
  if (repl_line_valid && data->dirty) {
    /* need to do a write-back */
    uns repl_proc_id = get_proc_id_from_cmp_addr(repl_line_addr);
    DEBUG(dc->proc_id, "Scheduling writeback of addr:0x%s\n", hexstr64s(repl_line_addr));
    ASSERT(dc->proc_id, data->read_count[0] || data->read_count[1] || data->write_count[0] || data->write_count[1]);
    ASSERT(dc->proc_id, repl_line_addr || data->fetched_by_offpath || data->HW_prefetched);

    if (!new_mem_dc_wb_req(MRT_WB, repl_proc_id, repl_line_addr, DCACHE_LINE_SIZE, 1, NULL, NULL, unique_count, TRUE)) {
      return NULL;
    }
    STAT_EVENT(dc->proc_id, DCACHE_WB_REQ_DIRTY);
    STAT_EVENT(dc->proc_id, DCACHE_WB_REQ);
  }

  DEBUG(dc->proc_id,
        "Filling dcache  off_path:%d addr:0x%s  :%7d index:%7d op_count:%d "
        "oldest:%lld\n",
        req->off_path, hexstr64s(req->addr), (int)req->addr, (int)(req->addr >> LOG2(DCACHE_LINE_SIZE)), req->op_count,
        (req->op_count ? req->oldest_op_unique_num : -1));

  data = (Dcache_Data*)cache_insert(&dc->dcache, dc->proc_id, req->addr, &line_addr, &repl_line_addr);
  ASSERT(dc->proc_id, req->emitted_cycle);
  ASSERT(dc->proc_id, cycle_count >= req->emitted_cycle);
  ASSERT(dc->proc_id, ((int)req->mlc_hit + (int)req->l1_hit) < 2);

  STAT_EVENT(dc->proc_id, DCACHE_FILL);
  INC_STAT_EVENT(dc->proc_id, DATA_LD_CYCLES_ONPATH + req->off_path, cycle_count - req->emitted_cycle);
  if (req->mlc_hit) {
    STAT_EVENT(dc->proc_id, DATA_LD_MLC_ACCESSES_ONPATH + req->off_path);
    INC_STAT_EVENT(dc->proc_id, DATA_LD_MLC_CYCLES_ONPATH + req->off_path, cycle_count - req->emitted_cycle);
  } else if (req->l1_hit) {
    STAT_EVENT(dc->proc_id, DATA_LD_L1_ACCESSES_ONPATH + req->off_path);
    INC_STAT_EVENT(dc->proc_id, DATA_LD_L1_CYCLES_ONPATH + req->off_path, cycle_count - req->emitted_cycle);
  } else {
    STAT_EVENT(dc->proc_id, DATA_LD_MEM_ACCESSES_ONPATH + req->off_path);
    INC_STAT_EVENT(dc->proc_id, DATA_LD_MEM_CYCLES_ONPATH + req->off_path, cycle_count - req->emitted_cycle);
  }

  return data;
}

static inline void dcache_fill_process_cacheline(Mem_Req* req, Dcache_Data* data) {
  /* collect wp stat */
  dcache_fill_wp_collect_stats(data, req);

  /* set up dcache line fields */
  data->dirty = req->dirty_l0 ? TRUE : FALSE;
  data->prefetch = TRUE;
  data->read_count[0] = 0;
  data->read_count[1] = 0;
  data->write_count[0] = 0;
  data->write_count[1] = 0;
  data->misc_state = req->off_path | req->off_path << 1;
  data->fetched_by_offpath = USE_CONFIRMED_OFF ? req->off_path_confirmed : req->off_path;
  data->offpath_op_addr = req->oldest_op_addr;
  data->offpath_op_unique = req->oldest_op_unique_num;
  data->fetch_cycle = cycle_count;
  data->onpath_use_cycle = (req->type == MRT_DPRF || req->off_path) ? 0 : cycle_count;

  if (req->type == MRT_DPRF) {  // cmp FIXME
    data->HW_prefetch = TRUE;
    data->HW_prefetched = TRUE;
  } else {
    data->HW_prefetch = FALSE;
    data->HW_prefetched = FALSE;
  }

  /* process req op */
  Op** op_p = (Op**)list_start_head_traversal(&req->op_ptrs);
  Counter* op_unique = (Counter*)list_start_head_traversal(&req->op_uniques);
  for (; op_p;
       op_p = (Op**)list_next_element(&req->op_ptrs), op_unique = (Counter*)list_next_element(&req->op_uniques)) {
    Op* op = *op_p;
    ASSERT(dc->proc_id, op);
    ASSERT(dc->proc_id, op_unique);
    ASSERT(dc->proc_id, dc->proc_id == op->proc_id);
    ASSERT(dc->proc_id, op->proc_id == req->proc_id);

    if (op->unique_num != *op_unique || !op->op_pool_valid) {
      continue;
    }

    /* update cacheline metadata */
    if (!op->off_path && op->table_info->mem_type == MEM_ST)
      ASSERT(dc->proc_id, data->dirty);

    data->prefetch &= op->table_info->mem_type == MEM_PF || op->table_info->mem_type == MEM_WH;
    data->read_count[op->off_path] += (op->table_info->mem_type == MEM_LD);
    data->write_count[op->off_path] += (op->table_info->mem_type == MEM_ST);

    DEBUG(dc->proc_id, "%s: %s line addr:0x%s: %7d\n", unsstr64(op->op_num), disasm_op(op, FALSE), hexstr64s(req->addr),
          (int)(req->addr >> LOG2(DCACHE_LINE_SIZE)));

    /* wake up dependent ops */
    DEBUG(dc->proc_id, "Awakening op_num:%lld %d %d\n", op->op_num, op->engine_info.l1_miss_satisfied, op->in_rdy_list);
    ASSERT(dc->proc_id, !op->in_rdy_list);

    op->done_cycle = cycle_count + 1;
    op->state = OS_SCHEDULED;

    if (op->table_info->mem_type != MEM_ST) {
      op->wake_cycle = op->done_cycle;
      wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
    }

    dcache_stage_record_main_chain_load_profile_fill(op, req);

    /* TEA load via normal dcache (cache miss fill): mark completed */
    if (TEA_ENABLE && op->thread_id == 1) {
      /* [EXPERIMENT: TEA_PERFECT_LOAD] DCACHE miss fill stats split by lower-level source */
      STAT_EVENT(dc->proc_id, TEA_LOADS_EXECUTED);
      STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_ACCESS);
      STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_MISS);
      if (req->mlc_hit) {
        STAT_EVENT(dc->proc_id, TEA_LOADS_MLC_HIT);
      } else if (req->l1_hit) {
        STAT_EVENT(dc->proc_id, TEA_LOADS_SCARAB_L1_HIT);
      } else {
        STAT_EVENT(dc->proc_id, TEA_LOADS_MEM_ACCESS);
      }
      if (op->dcache_cycle != MAX_CTR && op->done_cycle >= op->dcache_cycle) {
        Counter _lat = op->done_cycle - op->dcache_cycle;
        STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_SAMPLES);
        INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_TOTAL, _lat);
        INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_AVG,   _lat);
        tea_chain_note_load_result(dc->proc_id, op,
                                   TEA_LOAD_RESULT_DCACHE_MISS,
                                   _lat);
        tea_log_chain_load_addr(op, req->addr,
                                req->mlc_hit ? "MLC_HIT" :
                                (req->l1_hit ? "SCARAB_L1_HIT" :
                                               "MEM_ACCESS"),
                                _lat);
      }
      tea_op_completed(dc->proc_id, op);
    }
  }

  /*
   * This write_count is missing all the stores that retired before this fill happened.
   * Still, we know at least one on-path write must have occurred if the line is dirty.
   */
  if (data->dirty && data->write_count[0] == 0)
    data->write_count[0] = 1;

  ASSERT(dc->proc_id, data->read_count[0] || data->read_count[1] || data->write_count[0] || data->write_count[1] ||
                          req->off_path || data->prefetch || data->HW_prefetch);
}
