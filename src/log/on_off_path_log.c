#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "on_off_path_log.h"
#include "../globals/utils.h"
#include "../globals/global_vars.h"
#include "../isa/isa.h"
#include "../debug/debug_print.h" // Op_Type_str() 매크로를 위해 포함
#include "debug/debug.param.h"
#include "debug/debug_macros.h"

extern char* OUTPUT_DIR;
static FILE* on_off_path_log_file = NULL;

#define REG(x) #x,
static const char* reg_names[NUM_REGS] = {
#include "../isa/x86_regs.def"
};
#undef REG

static char* disasm_cached_op(Op* op) {
    static char buf[512];
    int i = 0;

    const char* opcode = Op_Type_str(op->table_info->op_type);
    if (op->table_info->op_type == OP_CF) {
        opcode = cf_type_names[op->table_info->cf_type];
    }
    i += sprintf(&buf[i], "%-8s ", opcode);

    for (int j = 0; j < op->table_info->num_dest_regs; j++) {
        i += sprintf(&buf[i], "r%u(%s)%s", op->inst_info->dests[j].id, reg_names[op->inst_info->dests[j].id], (j == op->table_info->num_dest_regs - 1) ? "" : ",");
    }
    if (op->table_info->num_dest_regs > 0 && op->table_info->num_src_regs > 0) {
        i += sprintf(&buf[i], " <- ");
    }
    for (int j = 0; j < op->table_info->num_src_regs; j++) {
        i += sprintf(&buf[i], "r%u(%s)%s", op->inst_info->srcs[j].id, reg_names[op->inst_info->srcs[j].id], (j == op->table_info->num_src_regs - 1) ? "" : ",");
    }
    if (op->table_info->mem_type == MEM_LD && op->oracle_info.mem_size > 0) {
      i += sprintf(&buf[i], " %d@%08llx", op->oracle_info.mem_size, op->oracle_info.va);
    }
    if (op->table_info->mem_type == MEM_ST && op->oracle_info.mem_size > 0) {
      i += sprintf(&buf[i], " %d@%08llx", op->oracle_info.mem_size, op->oracle_info.va);
    }
    buf[i] = '\0';
    return buf;
}

static void close_on_off_path_log(void) {
    if (on_off_path_log_file) fclose(on_off_path_log_file);
}

void init_on_off_path_log(void) {
    if (DEBUG_ONPATH_CONF && on_off_path_log_file == NULL) {
        on_off_path_log_file = file_tag_fopen(OUTPUT_DIR, "on_off_path", "w");
        if (on_off_path_log_file) atexit(close_on_off_path_log);
    }
}

void finalize_on_off_path_log(void) {
    close_on_off_path_log();
}

void log_on_off_path_entry(uns proc_id, On_Off_Path_Cache_Entry* entry, Counter cycle_count) {
    if (!on_off_path_log_file || !entry || !entry->is_valid||
        !DEBUG_RANGE_COND(proc_id)) return;

    fprintf(on_off_path_log_file, "--- [LOG] On-Off Path for Core %u ---\n", proc_id);
    fprintf(on_off_path_log_file, "Triggering H2P Branch PC: 0x%llx, OpNum: %llu, Path Length: %u\n",
            entry->h2p_branch_pc, entry->h2p_branch_op_num, entry->path_length);
    fprintf(on_off_path_log_file, "------------------------------------------------------------------\n");

    for (int i = 0; i < entry->path_length; ++i) {
        Op* op = &entry->path[i];
        char* disasm_str = disasm_cached_op(op);
        fprintf(on_off_path_log_file, "[PC: 0x%08llx] OpNum:%-10llu (S/E/D/R: %-4llu/%-4llu/%-4llu/%-4llu) H2p:%s Disasm: %-45s\n",
            op->inst_info->addr, 
            op->op_num,
            op->sched_cycle,
            op->exec_cycle,
            op->done_cycle,
            op->retire_cycle,
            op->oracle_info.hbt_pred_is_hard ? "O" : "X", 
            disasm_str);
    }
    fprintf(on_off_path_log_file, "-------------------------- END LOG ---------------------------\n\n");
    fflush(on_off_path_log_file);
}
