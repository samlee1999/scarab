// fill_buffer_log.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fill_buffer_log.h"
#include "../globals/utils.h"
#include "../globals/global_vars.h"
#include "../isa/isa.h"
#include "../table_info.h"
#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "../debug/debug_print.h"

extern char* OUTPUT_DIR;

static FILE* fill_buffer_log_file = NULL;

#define REG(x) #x,
static const char* reg_names[NUM_REGS] = {
#include "../isa/x86_regs.def"
};
#undef REG

#define REG_ARCH 0

static void close_fill_buffer_log(void) {
    if (fill_buffer_log_file) fclose(fill_buffer_log_file);
}

void init_fill_buffer_log(void) {
    if ((DEBUG_HBT || DEBUG_TEA) && fill_buffer_log_file == NULL) {
        fill_buffer_log_file = file_tag_fopen(OUTPUT_DIR, "fill_buffer", "w");
        if (fill_buffer_log_file) atexit(close_fill_buffer_log);
    }
}

void finalize_fill_buffer_log(void) {
    close_fill_buffer_log();
}

static char* disasm_retired_op(Op* op) {
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

void log_fill_buffer_entry(uns proc_id, Fill_Buffer* fb, Counter cycle_count) {
    if (!fill_buffer_log_file || !fb || fb->count == 0 ||
        !DEBUG_RANGE_COND(proc_id)) {
        return;
    }

    fprintf(fill_buffer_log_file, "--- [LOG] Fill Buffer for Core %u @ Cycle:%-6llu ---\n", proc_id, cycle_count);
    fprintf(fill_buffer_log_file, "Buffer Size: %u | Count: %u | Head: %u | Tail: %u\n",
            fb->size, fb->count, fb->head, fb->tail);
    fprintf(fill_buffer_log_file, "---------------------------------------------------------\n");

    for (int i = 0; i < fb->count; ++i) {
        int idx = (fb->head + i) % fb->size;
        Op* op = &fb->entries[idx];
        char* disasm_str = disasm_retired_op(op);

        fprintf(fill_buffer_log_file,
            "[%3d] PC: 0x%08llx | OpNum: %-10llu | H2P: %s | Disasm: %-45s\n",
            i,
            op->inst_info->addr,
            op->op_num,
            op->oracle_info.hbt_pred_is_hard ? "O" : "X",
            disasm_str);
    }

    fprintf(fill_buffer_log_file, "-------------------------- END LOG ---------------------------\n\n");
    fflush(fill_buffer_log_file);
}
