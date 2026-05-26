#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "op_trace_log.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "op.h"
#include "debug/debug_print.h"
#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "bp/bp_conf.h"
#include "bp/hbt.h"

static FILE* op_asm_log_file = NULL;
FILE* retired_op_log_file = NULL;
extern char* OUTPUT_DIR;

void close_op_asm_log_file(void) {
    if (op_asm_log_file) {
        fclose(op_asm_log_file);
        op_asm_log_file = NULL;
    }
}

void close_retired_op_log_file(void) {
    if (retired_op_log_file) {
        fclose(retired_op_log_file);
        retired_op_log_file = NULL;
    }
}

void init_op_trace_log(void) {
    if (DEBUG_NODE_STAGE && op_asm_log_file == NULL) {
        op_asm_log_file = file_tag_fopen(OUTPUT_DIR, "fill_rob_op_per_cycle", "w");
        if (op_asm_log_file == NULL) {
            perror("Error opening fill_rob_op_per_cycle in output directory");
        } else {
            atexit(close_op_asm_log_file);
        }
    }
    if (DEBUG_RETIRED_UOPS && retired_op_log_file == NULL) {
        retired_op_log_file = file_tag_fopen(OUTPUT_DIR, "retired_op_per_cycle", "w");
        if (retired_op_log_file == NULL) {
            perror("Error opening retired_op_per_cycle in output directory");
        } else {
            atexit(close_retired_op_log_file);
        }
    }
}

void log_fill_rob_op(Op* op, Counter cycle_count) {
    if (op_asm_log_file && DEBUG_RANGE_COND(op->proc_id)) {
        const char* disasm_str = disasm_op(op, TRUE);
        if (op->table_info->cf_type) {

            fprintf(op_asm_log_file, "Cycle:%-10llu OpNum:%-10llu PC:0x%-10llx OffPath:%d Disasm: %-80s H2P:%s HBT_CTR:%-3u Mispred_Type:%-4s Target:0x%-10llx Dir:%d PredNPC:0x%llx Pred:%d\n",
                    cycle_count,
                    op->op_num,
                    op->inst_info->addr,
                    op->off_path,
                    disasm_str,
                    op->oracle_info.hbt_pred_is_hard ? "YES" : "NO", 
                    op->oracle_info.hbt_misp_counter,
                    op->oracle_info.mispred ? "MISP" : (op->oracle_info.misfetch ? "MISF" : "----"),
                    op->oracle_info.npc,
                    op->oracle_info.dir,
                    op->oracle_info.pred_npc,
                    op->oracle_info.pred);
        } else {
            fprintf(op_asm_log_file, "Cycle:%-10llu OpNum:%-10llu PC:0x%-10llx OffPath:%d Disasm: %-60s\n",
                    cycle_count,
                    op->op_num,
                    op->inst_info->addr,
                    op->off_path,
                    disasm_str);
        }
        fflush(op_asm_log_file);
    }
}

void log_retired_ops(Counter cycle_count, uns ret_count) {
    if (retired_op_log_file && DEBUG_RANGE_COND(0)) {
        if (ret_count > 0) {
            fprintf(retired_op_log_file, "Cycle:%-10llu Retired Ops: %u\n", cycle_count, ret_count);
            fflush(retired_op_log_file);
        }
    }
}
