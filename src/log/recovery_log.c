/*
 * Copyright (c) 2024 University of California, Santa Cruz
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recovery_log.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "op.h"
#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "op_trace_log.h"
#include "bp/bp_conf.h"
#include "bp/hbt.h"
#include "map_rename.h"
#include "debug/debug_print.h"
#include "inst_info.h"
#include "node_stage.h"

// 전역 변수 정의
struct reg_file **reg_file;

// 이 파일 전용 로그(recovery_log)를 위한 포인터입니다.
static FILE* recovery_log_file = NULL;
extern char* OUTPUT_DIR;

// 이 파일 전용 로그 파일을 닫는 함수입니다.
void close_recovery_log_file(void) {
    if (recovery_log_file) {
        fclose(recovery_log_file);
        recovery_log_file = NULL;
    }
}

// 이 파일 전용 로그 파일을 초기화하는 함수입니다.
void init_recovery_log(void) {
    if ((DEBUG_BP || DEBUG_DECODE_STAGE || DEBUG_EXEC_STAGE) &&
        recovery_log_file == NULL) {
        recovery_log_file = file_tag_fopen(OUTPUT_DIR, "recovery_log", "w");
        if (recovery_log_file == NULL) {
            perror("Error opening recovery_log in output directory");
        } else {
            atexit(close_recovery_log_file);
        }
    }
}

/**
 * @brief RAT (Register Alias Table)의 현재 상태를 파일에 기록합니다.
 */
void log_rat_state(FILE* log_file, const char* title, struct reg_table_entry* arch_entries, uns arch_size, struct reg_table_entry* physical_entries, uns physical_size) {
    if (!log_file) return;

    fprintf(log_file, "  %s:\n", title);
    for (uns i = 0; i < arch_size; ++i) {
        struct reg_table_entry* arch_entry = &arch_entries[i];
        int logical_id = i;
        int physical_id = arch_entry->child_reg_id;

        if (physical_id != REG_TABLE_REG_ID_INVALID) {
            //ASSERT(map_data->proc_id, physical_id < physical_size);
            struct reg_table_entry* phys_entry = &physical_entries[physical_id];
            const char* path_status = phys_entry->off_path ? "Off-Path" : "On-Path";
            fprintf(log_file, "    - ArchReg: %-5s (r%2d) -> PhysReg: p%-3d (OpNum: %-7lld, Path: %s)\n",
                    disasm_reg(logical_id), logical_id, physical_id,
                    phys_entry->op_num, path_status);
        } else {
            fprintf(log_file, "    - ArchReg: %-5s (r%2d) -> Not Mapped\n",
                    disasm_reg(logical_id), logical_id);
        }
    }
    fflush(log_file);
}

/**
 * @brief 분기 예측 실패가 감지되었을 때의 시스템 상태를 기록합니다.
 */
void log_misprediction_detection_at_decode(Op* op, Node_Stage* node, Counter cycle_count, Bp_Recovery_Info* bp_recovery_info) {
    if ((!recovery_log_file && !retired_op_log_file) || !DEBUG_RANGE_COND(op->proc_id)) return;

    Addr pc_val = op->inst_info->addr;
    
    // RS 상태를 on/off path로 나누어 집계하기 위해 3D 배열로 변경
    int rs_state_counts[NUM_RS][OS_DONE+1][2];
    memset(rs_state_counts, 0, sizeof(rs_state_counts));

    // ROB Done/Pending 요약을 위한 변수들
    int done_before = 0, pending_before = 0, done_after = 0, pending_after = 0;
    int miss_off_path_count = 0, miss_on_path_count = 0;
    Flag op_found = FALSE;

    Op* current_op = node->node_head;
    while(current_op) {
        // RS 상태 집계 (On/Off Path 구분)
        Op_State current_state = current_op->state;
        int rs_id = current_op->rs_id;
        if (rs_id >= 0 && rs_id < NUM_RS && current_state >= 0 && current_state < OS_DONE+1) {
            int path_idx = current_op->off_path;
            rs_state_counts[rs_id][current_state][path_idx]++;
        }
        
        // ROB Done/Pending 및 Miss 카운팅
        if (current_op->state == OS_MISS) {
             if (current_op->off_path) miss_off_path_count++; else miss_on_path_count++;
        }
        if (current_op == op) { 
            op_found = TRUE; 
        } else {
            if (op_found) {
                if (cycle_count >= current_op->done_cycle && current_op->done_cycle > 0) done_after++; else pending_after++;
            } else {
                if (cycle_count >= current_op->done_cycle && current_op->done_cycle > 0) done_before++; else pending_before++;
            }
        }
        current_op = current_op->next_node;
    }

    if (recovery_log_file) {
        fprintf(recovery_log_file, "[Mispred Early Detection for PC 0x%llx] [Cycle %-10llu] op_num:%-10llu (off_path:%d) Mispred_Type:%-4s H2P:%s HBT_CTR:%-3u Target:0x%llx\n",
                pc_val, cycle_count, op->op_num, op->off_path, op->oracle_info.mispred ? "MISP" : (op->oracle_info.misfetch ? "MISF" : "----"),  op->oracle_info.hbt_pred_is_hard ? "YES" : "NO",  op->oracle_info.hbt_misp_counter,  op->oracle_info.npc);
        fprintf(recovery_log_file, "  Next Fetch Addr: 0x%llx, Recovery Ends at Cycle: %-10llu\n",
                bp_recovery_info->recovery_fetch_addr, bp_recovery_info->recovery_cycle);
        
        // ROB 상태를 Done/Pending 방식으로 출력
        fprintf(recovery_log_file, "  ROB state | Before op: Done=%-3d, Pending=%-3d | After op: Done=%-3d, Pending=%-3d | Miss (Off:%-3d, On:%-3d) | Total=%-3d\n",
                done_before, pending_before, done_after, pending_after, miss_off_path_count, miss_on_path_count, node->node_count);
        
        // RS 상태를 on/off path로 나누어 출력
        fprintf(recovery_log_file, "  RS State Breakdown (On-Path/Off-Path):\n");
        for (int i = 0; i < NUM_RS; i++) {
            fprintf(recovery_log_file, "    - %-10s:", node->rs[i].name);
            int total_in_rs = 0;
            for (int j = 0; j < OS_DONE; j++) {
                int on_path_count = rs_state_counts[i][j][0];
                int off_path_count = rs_state_counts[i][j][1];
                if (on_path_count > 0 || off_path_count > 0) {
                    fprintf(recovery_log_file, " %s(", Op_State_str(j));
                    if (on_path_count > 0) fprintf(recovery_log_file, "On:%d", on_path_count);
                    if (on_path_count > 0 && off_path_count > 0) fprintf(recovery_log_file, ",");
                    if (off_path_count > 0) fprintf(recovery_log_file, "Off:%d", off_path_count);
                    fprintf(recovery_log_file, ")");
                    total_in_rs += on_path_count + off_path_count;
                }
            }
            fprintf(recovery_log_file, " (Total: %d)\n", total_in_rs);
        }

        struct reg_file* rf_int = reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE];
        if (rf_int) {
            struct reg_table* rat_int = rf_int->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];
            struct reg_table* prf_int = rf_int->reg_table[REG_TABLE_TYPE_PHYSICAL];
            log_rat_state(recovery_log_file, "Current (Speculative) Integer RAT before recovery", rat_int->entries, rat_int->size, prf_int->entries, prf_int->size);
            if (rf_int->reg_checkpoint->is_valid) {
                log_rat_state(recovery_log_file, "Checkpoint Integer RAT for recovery", rf_int->reg_checkpoint->entries, rat_int->size, prf_int->entries, prf_int->size);
            }
        }

        fflush(recovery_log_file);
    }

    if (retired_op_log_file) {
       fprintf(retired_op_log_file, "[Mispred Early Detection for PC 0x%llx] [Cycle %-10llu] op_num:%-10llu (off_path:%d) Mispred_Type:%-4s H2P:%s HBT_CTR:%-3u Target:0x%llx\n",
                pc_val, cycle_count, op->op_num, op->off_path, op->oracle_info.mispred ? "MISP" : (op->oracle_info.misfetch ? "MISF" : "----"), op->oracle_info.hbt_pred_is_hard ? "YES" : "NO",  op->oracle_info.hbt_misp_counter, op->oracle_info.npc);
        fprintf(retired_op_log_file, "  Next Fetch Addr: 0x%llx, Recovery Ends at Cycle: %-10llu\n",
                bp_recovery_info->recovery_fetch_addr, bp_recovery_info->recovery_cycle);
        
        // ROB 상태를 Done/Pending 방식으로 출력
        fprintf(retired_op_log_file, "  ROB state | Before op: Done=%-3d, Pending=%-3d | After op: Done=%-3d, Pending=%-3d | Miss (Off:%-3d, On:%-3d) | Total=%-3d\n",
                done_before, pending_before, done_after, pending_after, miss_off_path_count, miss_on_path_count, node->node_count);
        
        // RS 상태를 on/off path로 나누어 출력
        fprintf(retired_op_log_file, "  RS State Breakdown (On-Path/Off-Path):\n");
        for (int i = 0; i < NUM_RS; i++) {
            fprintf(retired_op_log_file, "    - %-10s:", node->rs[i].name);
            int total_in_rs = 0;
            for (int j = 0; j < OS_DONE; j++) {
                int on_path_count = rs_state_counts[i][j][0];
                int off_path_count = rs_state_counts[i][j][1];
                if (on_path_count > 0 || off_path_count > 0) {
                    fprintf(retired_op_log_file, " %s(", Op_State_str(j));
                    if (on_path_count > 0) fprintf(retired_op_log_file, "On:%d", on_path_count);
                    if (on_path_count > 0 && off_path_count > 0) fprintf(retired_op_log_file, ",");
                    if (off_path_count > 0) fprintf(retired_op_log_file, "Off:%d", off_path_count);
                    fprintf(retired_op_log_file, ")");
                    total_in_rs += on_path_count + off_path_count;
                }
            }
            fprintf(retired_op_log_file, " (Total: %d)\n", total_in_rs);
        }
    }
}


void log_misprediction_detection_at_exec(Op* op, Node_Stage* node, Counter cycle_count, Bp_Recovery_Info* bp_recovery_info) {
    if ((!recovery_log_file && !retired_op_log_file) || !DEBUG_RANGE_COND(op->proc_id)) return;

    Addr pc_val = op->inst_info->addr;

    // RS 상태를 on/off path로 나누어 집계하기 위해 3D 배열로 변경
    int rs_state_counts[NUM_RS][OS_DONE+1][2];
    memset(rs_state_counts, 0, sizeof(rs_state_counts));

    // ROB Done/Pending 요약을 위한 변수들
    int done_before = 0, pending_before = 0, done_after = 0, pending_after = 0;
    int miss_off_path_count = 0, miss_on_path_count = 0;
    Flag op_found = FALSE;

    Op* current_op = node->node_head;
    while(current_op) {
        // RS 상태 집계 (On/Off Path 구분)
        Op_State current_state = current_op->state;
        int rs_id = current_op->rs_id;
        if (rs_id >= 0 && rs_id < NUM_RS && current_state >= 0 && current_state < OS_DONE+1) {
            int path_idx = current_op->off_path;
            rs_state_counts[rs_id][current_state][path_idx]++;
        }
        
        // ROB Done/Pending 및 Miss 카운팅
        if (current_op->state == OS_MISS) {
             if (current_op->off_path) miss_off_path_count++; else miss_on_path_count++;
        }
        if (current_op == op) { 
            op_found = TRUE; 
        } else {
            if (op_found) {
                if (cycle_count >= current_op->done_cycle && current_op->done_cycle > 0) done_after++; else pending_after++;
            } else {
                if (cycle_count >= current_op->done_cycle && current_op->done_cycle > 0) done_before++; else pending_before++;
            }
        }
        current_op = current_op->next_node;
    }

    if (recovery_log_file) {
        fprintf(recovery_log_file, "[Mispred Late Detection for PC 0x%llx] [Cycle %-10llu] op_num:%-10llu (off_path:%d) Mispred_Type:%-4s H2P:%s HBT_CTR:%-3u Target:0x%llx\n",
                pc_val, cycle_count, op->op_num, op->off_path, op->oracle_info.mispred ? "MISP" : (op->oracle_info.misfetch ? "MISF" : "----"),  op->oracle_info.hbt_pred_is_hard ? "YES" : "NO",  op->oracle_info.hbt_misp_counter,  op->oracle_info.npc);
        fprintf(recovery_log_file, "  Next Fetch Addr: 0x%llx, Recovery Ends at Cycle: %-10llu\n",
                bp_recovery_info->recovery_fetch_addr, bp_recovery_info->recovery_cycle);
        
        // ROB 상태를 Done/Pending 방식으로 출력
        fprintf(recovery_log_file, "  ROB state | Before op: Done=%-3d, Pending=%-3d | After op: Done=%-3d, Pending=%-3d | Miss (Off:%-3d, On:%-3d) | Total=%-3d\n",
                done_before, pending_before, done_after, pending_after, miss_off_path_count, miss_on_path_count, node->node_count);
        
        // RS 상태를 on/off path로 나누어 출력
        fprintf(recovery_log_file, "  RS State Breakdown (On-Path/Off-Path):\n");
        for (int i = 0; i < NUM_RS; i++) {
            fprintf(recovery_log_file, "    - %-10s:", node->rs[i].name);
            int total_in_rs = 0;
            for (int j = 0; j < OS_DONE; j++) {
                int on_path_count = rs_state_counts[i][j][0];
                int off_path_count = rs_state_counts[i][j][1];
                if (on_path_count > 0 || off_path_count > 0) {
                    fprintf(recovery_log_file, " %s(", Op_State_str(j));
                    if (on_path_count > 0) fprintf(recovery_log_file, "On:%d", on_path_count);
                    if (on_path_count > 0 && off_path_count > 0) fprintf(recovery_log_file, ",");
                    if (off_path_count > 0) fprintf(recovery_log_file, "Off:%d", off_path_count);
                    fprintf(recovery_log_file, ")");
                    total_in_rs += on_path_count + off_path_count;
                }
            }
            fprintf(recovery_log_file, " (Total: %d)\n", total_in_rs);
        }

        struct reg_file* rf_int = reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE];
        if (rf_int) {
            struct reg_table* rat_int = rf_int->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];
            struct reg_table* prf_int = rf_int->reg_table[REG_TABLE_TYPE_PHYSICAL];
            log_rat_state(recovery_log_file, "Current (Speculative) Integer RAT before recovery", rat_int->entries, rat_int->size, prf_int->entries, prf_int->size);
            if (rf_int->reg_checkpoint->is_valid) {
                log_rat_state(recovery_log_file, "Checkpoint Integer RAT for recovery", rf_int->reg_checkpoint->entries, rat_int->size, prf_int->entries, prf_int->size);
            }
        }

        fflush(recovery_log_file);
    }

    if (retired_op_log_file) {
       fprintf(retired_op_log_file, "[Mispred Late Detection for PC 0x%llx] [Cycle %-10llu] op_num:%-10llu (off_path:%d) Mispred_Type:%-4s H2P:%s HBT_CTR:%-3u Target:0x%llx\n",
                pc_val, cycle_count, op->op_num, op->off_path, op->oracle_info.mispred ? "MISP" : (op->oracle_info.misfetch ? "MISF" : "----"), op->oracle_info.hbt_pred_is_hard ? "YES" : "NO",  op->oracle_info.hbt_misp_counter, op->oracle_info.npc);
        fprintf(retired_op_log_file, "  Next Fetch Addr: 0x%llx, Recovery Ends at Cycle: %-10llu\n",
                bp_recovery_info->recovery_fetch_addr, bp_recovery_info->recovery_cycle);
        
        // ROB 상태를 Done/Pending 방식으로 출력
        fprintf(retired_op_log_file, "  ROB state | Before op: Done=%-3d, Pending=%-3d | After op: Done=%-3d, Pending=%-3d | Miss (Off:%-3d, On:%-3d) | Total=%-3d\n",
                done_before, pending_before, done_after, pending_after, miss_off_path_count, miss_on_path_count, node->node_count);
        
        // RS 상태를 on/off path로 나누어 출력
        fprintf(retired_op_log_file, "  RS State Breakdown (On-Path/Off-Path):\n");
        for (int i = 0; i < NUM_RS; i++) {
            fprintf(retired_op_log_file, "    - %-10s:", node->rs[i].name);
            int total_in_rs = 0;
            for (int j = 0; j < OS_DONE; j++) {
                int on_path_count = rs_state_counts[i][j][0];
                int off_path_count = rs_state_counts[i][j][1];
                if (on_path_count > 0 || off_path_count > 0) {
                    fprintf(retired_op_log_file, " %s(", Op_State_str(j));
                    if (on_path_count > 0) fprintf(retired_op_log_file, "On:%d", on_path_count);
                    if (on_path_count > 0 && off_path_count > 0) fprintf(retired_op_log_file, ",");
                    if (off_path_count > 0) fprintf(retired_op_log_file, "Off:%d", off_path_count);
                    fprintf(retired_op_log_file, ")");
                    total_in_rs += on_path_count + off_path_count;
                }
            }
            fprintf(retired_op_log_file, " (Total: %d)\n", total_in_rs);
        }
    }
}


/**
 * @brief 분기 예측 실패로부터 복구가 완료된 시점의 시스템 상태를 기록합니다.
 */
void log_recovery_end(Node_Stage* node, Counter cycle_count, Bp_Recovery_Info* bp_recovery_info) {
    if ((!recovery_log_file && !retired_op_log_file) || !DEBUG_RANGE_COND(node->proc_id)) return;

    Op* op = bp_recovery_info->recovery_op;
    Addr pc_val = op->inst_info->addr;
    
    // RS 상태를 on/off path로 나누어 집계하기 위해 3D 배열로 변경
    int rs_state_counts[NUM_RS][OS_DONE+1][2];
    memset(rs_state_counts, 0, sizeof(rs_state_counts));

    // ROB Done/Pending 요약을 위한 변수들
    int done_before = 0, pending_before = 0, done_after = 0, pending_after = 0;
    int miss_off_path_count = 0, miss_on_path_count = 0;
    Flag op_found = FALSE;

    Op* current_op = node->node_head;
    while(current_op) {
        // RS 상태 집계 (On/Off Path 구분)
        Op_State current_state = current_op->state;
        int rs_id = current_op->rs_id;
        if (rs_id >= 0 && rs_id < NUM_RS && current_state >= 0 && current_state < OS_DONE+1) {
            int path_idx = current_op->off_path;
            rs_state_counts[rs_id][current_state][path_idx]++;
        }
        
        // ROB Done/Pending 및 Miss 카운팅
        if (current_op->state == OS_MISS) {
             if (current_op->off_path) miss_off_path_count++; else miss_on_path_count++;
        }
        if (current_op == op) { 
            op_found = TRUE; 
        } else {
            if (op_found) {
                if (cycle_count >= current_op->done_cycle && current_op->done_cycle > 0) done_after++; else pending_after++;
            } else {
                if (cycle_count >= current_op->done_cycle && current_op->done_cycle > 0) done_before++; else pending_before++;
            }
        }
        current_op = current_op->next_node;
    }
    
    if (recovery_log_file) {
        fprintf(recovery_log_file, "[Recovery End for PC 0x%llx] [Cycle %-10llu] op_num:%-10llu (off_path:%d) Mispred_Type:%-4s H2P:%s HBT_CTR:%-3u Target:0x%llx\n",
                pc_val, cycle_count, op->op_num, op->off_path, op->oracle_info.mispred ? "MISP" : (op->oracle_info.misfetch ? "MISF" : "----"),  op->oracle_info.hbt_pred_is_hard ? "YES" : "NO",  op->oracle_info.hbt_misp_counter, op->oracle_info.npc);
        fprintf(recovery_log_file, "  Next Fetch Addr: 0x%llx, Recovery Ends at Cycle: %-10llu\n",
                bp_recovery_info->recovery_fetch_addr, bp_recovery_info->recovery_cycle);
        
        // ROB 상태를 Done/Pending 방식으로 출력
        fprintf(recovery_log_file, "  ROB state | Before op: Done=%-3d, Pending=%-3d | After op: Done=%-3d, Pending=%-3d | Miss (Off:%-3d, On:%-3d) | Total=%-3d\n",
                done_before, pending_before, done_after, pending_after, miss_off_path_count, miss_on_path_count, node->node_count);
        
        // RS 상태를 on/off path로 나누어 출력
        fprintf(recovery_log_file, "  RS State Breakdown (On-Path/Off-Path):\n");
        for (int i = 0; i < NUM_RS; i++) {
            fprintf(recovery_log_file, "    - %-10s:", node->rs[i].name);
            int total_in_rs = 0;
            for (int j = 0; j < OS_DONE; j++) {
                int on_path_count = rs_state_counts[i][j][0];
                int off_path_count = rs_state_counts[i][j][1];
                if (on_path_count > 0 || off_path_count > 0) {
                    fprintf(recovery_log_file, " %s(", Op_State_str(j));
                    if (on_path_count > 0) fprintf(recovery_log_file, "On:%d", on_path_count);
                    if (on_path_count > 0 && off_path_count > 0) fprintf(recovery_log_file, ",");
                    if (off_path_count > 0) fprintf(recovery_log_file, "Off:%d", off_path_count);
                    fprintf(recovery_log_file, ")");
                    total_in_rs += on_path_count + off_path_count;
                }
            }
            fprintf(recovery_log_file, " (Total: %d)\n", total_in_rs);
        }

        struct reg_file* rf_int = reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE];
        if (rf_int) {
            struct reg_table* rat_int = rf_int->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];
            struct reg_table* prf_int = rf_int->reg_table[REG_TABLE_TYPE_PHYSICAL];
            log_rat_state(recovery_log_file, "Current (Speculative) Integer RAT before recovery", rat_int->entries, rat_int->size, prf_int->entries, prf_int->size);
            if (rf_int->reg_checkpoint->is_valid) {
                log_rat_state(recovery_log_file, "Checkpoint Integer RAT for recovery", rf_int->reg_checkpoint->entries, rat_int->size, prf_int->entries, prf_int->size);
            }
        }

        fflush(recovery_log_file);
    }

    if (retired_op_log_file) {
       fprintf(retired_op_log_file, "[Recovery End Detection for PC 0x%llx] [Cycle %-10llu] op_num:%-10llu (off_path:%d) Mispred_Type:%-4s H2P:%s HBT_CTR:%-3u Target:0x%llx\n",
                pc_val, cycle_count, op->op_num, op->off_path, op->oracle_info.mispred ? "MISP" : (op->oracle_info.misfetch ? "MISF" : "----"), op->oracle_info.hbt_pred_is_hard ? "YES" : "NO",  op->oracle_info.hbt_misp_counter, op->oracle_info.npc);
        fprintf(retired_op_log_file, "  Next Fetch Addr: 0x%llx, Recovery Ends at Cycle: %-10llu\n",
                bp_recovery_info->recovery_fetch_addr, bp_recovery_info->recovery_cycle);
        
        // ROB 상태를 Done/Pending 방식으로 출력
        fprintf(retired_op_log_file, "  ROB state | Before op: Done=%-3d, Pending=%-3d | After op: Done=%-3d, Pending=%-3d | Miss (Off:%-3d, On:%-3d) | Total=%-3d\n",
                done_before, pending_before, done_after, pending_after, miss_off_path_count, miss_on_path_count, node->node_count);
        
        // RS 상태를 on/off path로 나누어 출력
        fprintf(retired_op_log_file, "  RS State Breakdown (On-Path/Off-Path):\n");
        for (int i = 0; i < NUM_RS; i++) {
            fprintf(retired_op_log_file, "    - %-10s:", node->rs[i].name);
            int total_in_rs = 0;
            for (int j = 0; j < OS_DONE; j++) {
                int on_path_count = rs_state_counts[i][j][0];
                int off_path_count = rs_state_counts[i][j][1];
                if (on_path_count > 0 || off_path_count > 0) {
                    fprintf(retired_op_log_file, " %s(", Op_State_str(j));
                    if (on_path_count > 0) fprintf(retired_op_log_file, "On:%d", on_path_count);
                    if (on_path_count > 0 && off_path_count > 0) fprintf(retired_op_log_file, ",");
                    if (off_path_count > 0) fprintf(retired_op_log_file, "Off:%d", off_path_count);
                    fprintf(retired_op_log_file, ")");
                    total_in_rs += on_path_count + off_path_count;
                }
            }
            fprintf(retired_op_log_file, " (Total: %d)\n", total_in_rs);
        }
    }
}
