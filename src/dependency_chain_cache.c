#include "dependency_chain_cache.h"
#include "core.param.h"
#include "fill_buffer.h"
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "log/dependency_chain_log.h"
#include "statistics.h"
#include "zereco/rfp.h"
#include <string.h>
#include <stdbool.h>

// 전역 변수 정의
Dependency_Chain_Cache_Entry** dependency_chain_caches;
Dependency_Chain_Cache_Entry** block_caches;
Block_Cache_Tag_Entry** empty_block_tag_store;
Backward_Walk_Engine** bw_engines;

// =================================================================
// 초기화 및 리셋 함수
// =================================================================

void init_dependency_chain_cache(uns proc_id) {
    ASSERTM(proc_id, ZERECO_IQ_PRIORITY_POLICY <= 2,
            "zereco_iq_priority_policy must be 0, 1, or 2\n");
    ASSERTM(proc_id, ZERECO_IQ_PRIORITY_SCOPE <= 1,
            "zereco_iq_priority_scope must be 0 or 1\n");
    if (ZERECO_IQ_PRIORITY_POLICY) {
        ASSERTM(proc_id, !TEA_ENABLE,
                "ZERECO IQ priority is a main-thread-only experiment; disable TEA\n");
    }
    if (ZERECO_PIQ_ENABLE) {
        ASSERTM(proc_id, ZERECO_IQ_PRIORITY_POLICY == 1 ||
                         ZERECO_IQ_PRIORITY_POLICY == 2,
                "ZERECO P-IQ requires all-H2P or online RF-filtered priority\n");
        ASSERTM(proc_id, ZERECO_IQ_PRIORITY_SCHEDULE_ENABLE,
                "ZERECO P-IQ requires priority-first scheduling\n");
        ASSERTM(proc_id, ZERECO_IQ_PRIORITY_SCOPE == 0,
                "The current P-IQ sweep is defined for the full H2P branch slice\n");
        ASSERTM(proc_id, ZERECO_PIQ_ENTRY_PERCENT == 10 ||
                         ZERECO_PIQ_ENTRY_PERCENT == 15 ||
                         ZERECO_PIQ_ENTRY_PERCENT == 20 ||
                         ZERECO_PIQ_ENTRY_PERCENT == 25 ||
                         ZERECO_PIQ_ENTRY_PERCENT == 50,
                "P-IQ sweep percentage must be 10, 15, 20, 25, or 50\n");
    }
    if (ZERECO_IQ_PRIORITY_POLICY == 2) {
        /* RF filtering reads op->zereco_rf_covered, which today only the
           h2p_chain_perfect_load oracle sets.  The timed RFP model will set it
           once a covered load actually skips its cache access (Phase 3); until
           then combining rfp_enable with this policy would filter on a flag
           that is never TRUE, silently degrading it to policy 1. */
        ASSERTM(proc_id, !RFP_ENABLE,
                "RF-filtered IQ priority cannot use the timed RFP model yet: it "
                "needs the covered fast path that sets zereco_rf_covered (Phase 3)\n");
        ASSERTM(proc_id, H2P_CHAIN_PERFECT_LOAD,
                "online RF-filtered IQ priority requires h2p_chain_perfect_load=1\n");
        ASSERTM(proc_id, H2P_CHAIN_ORACLE_PREDICTOR == 1 ||
                         H2P_CHAIN_ORACLE_PREDICTOR == 2,
                "online RF-filtered IQ priority requires stride or top-delta predictor\n");
        ASSERTM(proc_id, H2P_CHAIN_ORACLE_GRANULARITY == 0,
                "online RF-filtered IQ priority is defined for exact-vaddr RF prediction\n");
        ASSERTM(proc_id, ZERECO_IQ_RF_COVERED_STREAK_THRESHOLD > 0,
                "RF-covered streak threshold must be non-zero\n");
    }
    if (!dependency_chain_caches) {
        dependency_chain_caches = (Dependency_Chain_Cache_Entry**)calloc(NUM_CORES, sizeof(Dependency_Chain_Cache_Entry*));
        block_caches = (Dependency_Chain_Cache_Entry**)calloc(NUM_CORES, sizeof(Dependency_Chain_Cache_Entry*));
        empty_block_tag_store = (Block_Cache_Tag_Entry**)calloc(NUM_CORES, sizeof(Block_Cache_Tag_Entry*));
        bw_engines = (Backward_Walk_Engine**)calloc(NUM_CORES, sizeof(Backward_Walk_Engine*));
        ASSERT(0, dependency_chain_caches && block_caches && empty_block_tag_store && bw_engines);
    }
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");

    dependency_chain_caches[proc_id] = (Dependency_Chain_Cache_Entry*)calloc(DEPENDENCY_CHAIN_CACHE_SIZE, sizeof(Dependency_Chain_Cache_Entry));
    block_caches[proc_id] = (Dependency_Chain_Cache_Entry*)calloc(BLOCK_CACHE_SIZE, sizeof(Dependency_Chain_Cache_Entry));
    empty_block_tag_store[proc_id] = (Block_Cache_Tag_Entry*)calloc(EMPTY_BLOCK_TAG_STORE_SIZE, sizeof(Block_Cache_Tag_Entry));
    bw_engines[proc_id] = (Backward_Walk_Engine*)calloc(1, sizeof(Backward_Walk_Engine));
    bw_engines[proc_id]->snapshot_buffer = (Op*)calloc(FILL_BUFFER_SIZE, sizeof(Op));
    ASSERT(proc_id, dependency_chain_caches[proc_id] && block_caches[proc_id] && empty_block_tag_store[proc_id] && bw_engines[proc_id]);
}

void reset_dependency_chain_cache(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (dependency_chain_caches[proc_id]) {
        memset(dependency_chain_caches[proc_id], 0, sizeof(Dependency_Chain_Cache_Entry) * DEPENDENCY_CHAIN_CACHE_SIZE);
    }
    if (block_caches[proc_id]) {
        memset(block_caches[proc_id], 0, sizeof(Dependency_Chain_Cache_Entry) * BLOCK_CACHE_SIZE);
    }
    if (empty_block_tag_store[proc_id]) {
        memset(empty_block_tag_store[proc_id], 0, sizeof(Block_Cache_Tag_Entry) * EMPTY_BLOCK_TAG_STORE_SIZE);
    }
    if (bw_engines[proc_id]) {
        bw_engines[proc_id]->state = BW_IDLE;
        bw_engines[proc_id]->walk_cycles_remaining = 0;
    }
}

// =================================================================
// SourceList 헬퍼 함수 (Bit Vector 최적화 적용)
// =================================================================

static void add_reg_to_live_in_list(SourceList* list, Reg_Info* reg) {
    if (reg->id >= NUM_REG_IDS)
        return;
    list->reg_vector[reg->id / 64] |= 1ULL << (reg->id % 64);
}

static bool remove_reg_from_live_in_list(SourceList* list, Reg_Info* reg) {
    if (reg->id >= NUM_REG_IDS)
        return false;
    uint64_t bit = 1ULL << (reg->id % 64);
    if (!(list->reg_vector[reg->id / 64] & bit))
        return false;
    list->reg_vector[reg->id / 64] &= ~bit;
    return true;
}

static void add_addr_to_live_in_list(SourceList* list, Addr addr) {
    if (list->addr_count >= MAX_MEM_LIVE_INS) return;
    for (uns i = 0; i < list->addr_count; ++i) if (list->addrs[i] == addr) return;
    list->addrs[list->addr_count++] = addr;
}

static bool remove_addr_from_live_in_list(SourceList* list, Addr addr) {
    for (uns i = 0; i < list->addr_count; i++) {
        if (list->addrs[i] == addr) {
            list->addrs[i] = list->addrs[--list->addr_count];
            return true;
        }
    }
    return false;
}

// =================================================================
// 핵심 로직: 스냅샷을 기반으로 체인 추출 및 캐시 저장
// =================================================================

static void build_block_position_maps(Op* ordered_ops, int ordered_op_count,
                                      Addr* block_start_pc_map,
                                      uns* block_op_idx_map,
                                      uns* block_total_ops_map) {
    Addr current_block_start_pc = 0;
    uns current_block_op_idx = 0;
    if (ordered_op_count > 0 && ordered_ops[0].inst_info) {
        current_block_start_pc = ordered_ops[0].inst_info->addr;
    }
    for (int i = 0; i < ordered_op_count; ++i) {
        if (ordered_ops[i].h2p_chain_block_start_pc) {
            block_start_pc_map[i] = ordered_ops[i].h2p_chain_block_start_pc;
            block_op_idx_map[i] = ordered_ops[i].h2p_chain_block_op_idx;
        } else {
            block_start_pc_map[i] = current_block_start_pc;
            block_op_idx_map[i] = current_block_op_idx;
        }
        if (ordered_ops[i].table_info && ordered_ops[i].table_info->cf_type != NOT_CF) {
            if (i + 1 < ordered_op_count && ordered_ops[i+1].inst_info) {
                current_block_start_pc = ordered_ops[i + 1].inst_info->addr;
                current_block_op_idx = 0;
            }
        } else {
            current_block_op_idx++;
        }
    }

    int block_begin = 0;
    for (int i = 0; i < ordered_op_count; ++i) {
        bool block_end = (ordered_ops[i].table_info &&
                          ordered_ops[i].table_info->cf_type != NOT_CF) ||
                         i == ordered_op_count - 1;
        if (!block_end)
            continue;
        uns total_ops = 0;
        for (int j = block_begin; j <= i; ++j)
            total_ops = MAX2(total_ops, block_op_idx_map[j] + 1);
        for (int j = block_begin; j <= i; ++j)
            block_total_ops_map[j] = total_ops;
        block_begin = i + 1;
    }
}

static int collect_h2p_indices(uns proc_id, Op* ordered_ops,
                               int ordered_op_count, int* h2p_indices) {
    int h2p_count = 0;
    for (int i = 0; i < ordered_op_count; ++i) {
        if (ordered_ops[i].oracle_info.hbt_pred_is_hard) {
            h2p_indices[h2p_count++] = i;
        }
    }

    INC_STAT_EVENT(proc_id, DCC_SNAPSHOT_H2P_BRANCHES_TOTAL, h2p_count);
    if (h2p_count == 0) {
        STAT_EVENT(proc_id, DCC_SNAPSHOT_H2P_BRANCHES_0);
    } else if (h2p_count == 1) {
        STAT_EVENT(proc_id, DCC_SNAPSHOT_H2P_BRANCHES_1);
    } else if (h2p_count == 2) {
        STAT_EVENT(proc_id, DCC_SNAPSHOT_H2P_BRANCHES_2);
    } else if (h2p_count == 3) {
        STAT_EVENT(proc_id, DCC_SNAPSHOT_H2P_BRANCHES_3);
    } else {
        STAT_EVENT(proc_id, DCC_SNAPSHOT_H2P_BRANCHES_4_PLUS);
    }

    return h2p_count;
}

static int build_dependency_mask_for_target(Op* ordered_ops, int trigger_op_idx,
                                            bool* is_data_dependent) {
    SourceList live_in_list = {0};
    memset(is_data_dependent, false, sizeof(bool) * FILL_BUFFER_SIZE);
    
    Op* trigger_op = &ordered_ops[trigger_op_idx];
    is_data_dependent[trigger_op_idx] = true;

    if (trigger_op->inst_info && trigger_op->table_info) {
        for (int i = 0; i < trigger_op->table_info->num_src_regs; ++i) add_reg_to_live_in_list(&live_in_list, &trigger_op->inst_info->srcs[i]);
    }
    if (trigger_op->table_info && trigger_op->table_info->mem_type == MEM_LD) add_addr_to_live_in_list(&live_in_list, trigger_op->oracle_info.va);

    int first_dep_op_idx = trigger_op_idx;
    for (int i = trigger_op_idx - 1; i >= 0; --i) {
        Op* current_op = &ordered_ops[i];
        bool depends = false;
        if (!current_op->table_info || !current_op->inst_info) continue;
        
        for (int d = 0; d < current_op->table_info->num_dest_regs; ++d) {
            if (remove_reg_from_live_in_list(&live_in_list, &current_op->inst_info->dests[d])) depends = true;
        }
        if (current_op->table_info->mem_type == MEM_ST && remove_addr_from_live_in_list(&live_in_list, current_op->oracle_info.va)) depends = true;

        if (depends) {
            is_data_dependent[i] = true;
            first_dep_op_idx = i;
            for (int s = 0; s < current_op->table_info->num_src_regs; ++s) add_reg_to_live_in_list(&live_in_list, &current_op->inst_info->srcs[s]);
            if (current_op->table_info->mem_type == MEM_LD) add_addr_to_live_in_list(&live_in_list, current_op->oracle_info.va);
        }
    }

    return first_dep_op_idx;
}

static int build_iq_priority_candidate_mask(Op* ordered_ops,
                                            int first_dep_op_idx,
                                            int trigger_op_idx,
                                            bool* is_data_dependent,
                                            bool* is_priority_candidate) {
    memset(is_priority_candidate, false, sizeof(bool) * FILL_BUFFER_SIZE);

    if (ZERECO_IQ_PRIORITY_SCOPE == 0) {
        for (int i = first_dep_op_idx; i <= trigger_op_idx; ++i)
            is_priority_candidate[i] = is_data_dependent[i];
        return trigger_op_idx;
    }

    /* Load-prefix scope: keep only dependent ops in program order from the
       oldest slice member through the last Target Load.  This accelerates the
       address/data path needed to issue all Target Loads, while leaving the
       load-to-branch tail at normal priority. */
    int last_target_load_idx = -1;
    for (int i = first_dep_op_idx; i <= trigger_op_idx; ++i) {
        if (is_data_dependent[i] && ordered_ops[i].table_info &&
            ordered_ops[i].table_info->mem_type == MEM_LD)
            last_target_load_idx = i;
    }
    if (last_target_load_idx < 0)
        return -1;

    for (int i = first_dep_op_idx; i <= last_target_load_idx; ++i)
        is_priority_candidate[i] = is_data_dependent[i];
    return last_target_load_idx;
}

static void commit_dependency_chain_entry(
  uns proc_id, Op* ordered_ops, int first_dep_op_idx, int trigger_op_idx,
  bool* is_data_dependent, bool* is_priority_candidate,
  Addr* block_start_pc_map,
  uns* block_op_idx_map, uns* block_total_ops_map) {
    Op* trigger_op = &ordered_ops[trigger_op_idx];
    if (!trigger_op->inst_info)
        return;

    Dependency_Chain_Cache_Entry* dep_cache = dependency_chain_caches[proc_id];
    int dep_entry_index = trigger_op->inst_info->addr % DEPENDENCY_CHAIN_CACHE_SIZE;
    Dependency_Chain_Cache_Entry* dep_entry = &dep_cache[dep_entry_index];
    bool same_branch = dep_entry->is_valid &&
                       dep_entry->h2p_branch_pc == trigger_op->inst_info->addr;
    uns previous_covered_streak = same_branch ? dep_entry->rf_covered_streak : 0;

    if (same_branch)
        STAT_EVENT(proc_id, DCC_CHAIN_OVERWRITE_SAME_PC);
    else
        memset(dep_entry, 0, sizeof(*dep_entry));

    uns target_load_count = 0;
    bool all_target_loads_rf_covered = true;
    for (int i = first_dep_op_idx; i <= trigger_op_idx; ++i) {
        if (!is_data_dependent[i] || !ordered_ops[i].table_info ||
            ordered_ops[i].table_info->mem_type != MEM_LD)
            continue;
        target_load_count++;
        if (!ordered_ops[i].zereco_rf_covered)
            all_target_loads_rf_covered = false;
        /* This load sits in an H2P backward slice, so it is a Target Load.
           Under RFP_SCOPE 0 that makes it -- and only it -- eligible to own a
           Prefetch Table entry.  Membership is what scopes RFP to the H2P
           chain; the per-instance address tracking happens at retire. */
        if (ordered_ops[i].inst_info)
            rfp_note_target_load(proc_id, ordered_ops[i].inst_info->addr,
                                 ordered_ops[i].oracle_info.va);
    }

    dep_entry->target_load_count = target_load_count;
    dep_entry->rf_covered_streak = previous_covered_streak;
    dep_entry->iq_priority_enabled = TRUE;
    if (ZERECO_IQ_PRIORITY_POLICY == 2) {
        STAT_EVENT(proc_id, ZERECO_IQ_H2P_SLICES);
        INC_STAT_EVENT(proc_id, ZERECO_IQ_TARGET_LOADS, target_load_count);
        for (int i = first_dep_op_idx; i <= trigger_op_idx; ++i) {
            if (!is_data_dependent[i] || !ordered_ops[i].table_info ||
                ordered_ops[i].table_info->mem_type != MEM_LD)
                continue;
            if (ordered_ops[i].zereco_rf_covered) {
                STAT_EVENT(proc_id, ZERECO_IQ_TARGET_LOADS_RF_COVERED);
                STAT_EVENT(proc_id, ZERECO_IQ_TARGET_LOADS_RF_COVERED_PCT);
            } else {
                STAT_EVENT(proc_id, ZERECO_IQ_TARGET_LOADS_RF_UNCOVERED);
            }
        }

        if (target_load_count == 0) {
            STAT_EVENT(proc_id, ZERECO_IQ_SLICES_NO_TARGET_LOAD);
            dep_entry->rf_covered_streak = 0;
        } else if (all_target_loads_rf_covered) {
            STAT_EVENT(proc_id, ZERECO_IQ_SLICES_ALL_RF_COVERED);
            dep_entry->rf_covered_streak = MIN2(
              previous_covered_streak + 1,
              ZERECO_IQ_RF_COVERED_STREAK_THRESHOLD);
            if (dep_entry->rf_covered_streak >=
                ZERECO_IQ_RF_COVERED_STREAK_THRESHOLD)
                dep_entry->iq_priority_enabled = FALSE;
        } else {
            STAT_EVENT(proc_id, ZERECO_IQ_SLICES_RF_UNCOVERED);
            dep_entry->rf_covered_streak = 0;
        }

        if (dep_entry->iq_priority_enabled)
            STAT_EVENT(proc_id, ZERECO_IQ_SLICES_PRIORITY_ENABLED);
        else {
            STAT_EVENT(proc_id, ZERECO_IQ_SLICES_PRIORITY_SUPPRESSED);
            STAT_EVENT(proc_id, ZERECO_IQ_SLICES_PRIORITY_SUPPRESSED_PCT);
        }
    }
    
    dep_entry->is_valid = TRUE;
    dep_entry->h2p_branch_pc = trigger_op->inst_info->addr;
    dep_entry->h2p_branch_op_num = trigger_op->op_num;
    dep_entry->h2p_branch_unique_num = trigger_op->unique_num;
    dep_entry->insert_cycle = cycle_count;
    dep_entry->chain_length = 0;
    dep_entry->block_fragment_count = 0;
    for (int i = first_dep_op_idx; i <= trigger_op_idx; ++i) {
        if (is_data_dependent[i]) {
            if (dep_entry->chain_length < MAX_CHAIN_LENGTH) {
                dep_entry->chain[dep_entry->chain_length++] = ordered_ops[i];
            }

            if (block_op_idx_map[i] >= 64)
                continue;
            uns fragment_idx = 0;
            for (; fragment_idx < dep_entry->block_fragment_count;
                 ++fragment_idx) {
                if (dep_entry->block_fragments[fragment_idx].block_start_pc ==
                    block_start_pc_map[i])
                    break;
            }
            if (fragment_idx == dep_entry->block_fragment_count) {
                if (fragment_idx >= MAX_CHAIN_BLOCK_FRAGMENTS)
                    continue;
                dep_entry->block_fragment_count++;
                memset(&dep_entry->block_fragments[fragment_idx], 0,
                       sizeof(dep_entry->block_fragments[fragment_idx]));
                dep_entry->block_fragments[fragment_idx].block_start_pc =
                  block_start_pc_map[i];
            }
            Dependency_Chain_Block_Fragment* fragment =
              &dep_entry->block_fragments[fragment_idx];
            fragment->mask |= 1ULL << block_op_idx_map[i];
            if (is_priority_candidate[i])
                fragment->priority_candidate_mask |=
                  1ULL << block_op_idx_map[i];
            fragment->total_ops_in_block = MAX2(
              fragment->total_ops_in_block, block_total_ops_map[i]);
        }
    }
    log_dependency_chain_entry(proc_id, dep_entry, cycle_count);
    STAT_EVENT(proc_id, DCC_CHAINS_INSERTED);
}

static void commit_block_cache_masks(uns proc_id, Op* ordered_ops,
                                     Addr* block_start_pc_map,
                                     uns* block_op_idx_map,
                                     uns* block_total_ops_map,
                                     bool* is_data_dependent_union,
                                     bool* is_priority_candidate_union,
                                     int first_union_dep_idx,
                                     int last_h2p_idx) {
    if (first_union_dep_idx < 0 || last_h2p_idx < first_union_dep_idx)
        return;

    Dependency_Chain_Cache_Entry* block_cache = block_caches[proc_id];
    Block_Cache_Tag_Entry* tag_store = empty_block_tag_store[proc_id];
    int current_block_start_idx = first_union_dep_idx;

    for (int i = first_union_dep_idx; i <= last_h2p_idx; ++i) {
        bool is_block_terminator = (ordered_ops[i].table_info && ordered_ops[i].table_info->cf_type != NOT_CF);
        bool is_last_op_in_slice = (i == last_h2p_idx);

        if (is_block_terminator || is_last_op_in_slice) {
            uint64_t new_dependency_mask = 0;
            uint64_t new_priority_candidate_mask = 0;
            int instructions_in_block = 0;
            int dependent_ops_count = 0;

            for (int j = current_block_start_idx; j <= i; ++j) {
                instructions_in_block = MAX2(
                  instructions_in_block, (int)block_total_ops_map[j]);
                if (is_data_dependent_union[j] && block_op_idx_map[j] < 64) {
                    new_dependency_mask |= (1ULL << block_op_idx_map[j]);
                    dependent_ops_count++;
                }
                if (is_priority_candidate_union[j] &&
                    block_op_idx_map[j] < 64)
                    new_priority_candidate_mask |=
                      1ULL << block_op_idx_map[j];
            }

            Addr real_block_start_pc = block_start_pc_map[current_block_start_idx];

            if (dependent_ops_count > 0) {
                int block_entry_index = real_block_start_pc % BLOCK_CACHE_SIZE;
                Dependency_Chain_Cache_Entry* block_entry = &block_cache[block_entry_index];

                bool same_block = block_entry->is_valid &&
                                  block_entry->h2p_branch_pc == real_block_start_pc;
                uint64_t old_mask = same_block ? block_entry->dependency_mask : 0;
                uint64_t old_priority_candidate_mask =
                  same_block ? block_entry->iq_priority_candidate_mask : 0;
                uns old_total_ops =
                  same_block ? block_entry->total_ops_in_block : 0;
                if (!same_block)
                    memset(block_entry, 0, sizeof(*block_entry));
                
                block_entry->is_valid = TRUE;
                block_entry->h2p_branch_pc = real_block_start_pc;
                if (old_mask == 0) {
                    block_entry->h2p_branch_op_num = ordered_ops[current_block_start_idx].op_num;
                    block_entry->h2p_branch_unique_num = ordered_ops[current_block_start_idx].unique_num;
                    block_entry->insert_cycle = cycle_count;
                }

                uint64_t merged_mask = old_mask | new_dependency_mask;
                if (merged_mask != old_mask)
                    STAT_EVENT(proc_id, DCC_BLOCK_MASK_OR_UPDATES);

                block_entry->dependency_mask = merged_mask;
                block_entry->iq_priority_candidate_mask =
                  old_priority_candidate_mask | new_priority_candidate_mask;
                /* A later snapshot can begin/end at a narrower portion of a
                   block.  Never shrink the valid slot range for older ORed
                   dependency bits. */
                block_entry->total_ops_in_block =
                  MAX2(old_total_ops, (uns)instructions_in_block);
                
                block_entry->chain_length = 0;
                for (int j = current_block_start_idx; j <= i; ++j) {
                    if (is_data_dependent_union[j] &&
                        block_entry->chain_length < MAX_CHAIN_LENGTH)
                        block_entry->chain[block_entry->chain_length++] =
                          ordered_ops[j];
                }
                log_dependency_chain_block(proc_id, block_entry, cycle_count);
            } else {
                int tag_entry_index = real_block_start_pc % EMPTY_BLOCK_TAG_STORE_SIZE;
                Block_Cache_Tag_Entry* tag_entry = &tag_store[tag_entry_index];
                tag_entry->is_valid = TRUE;
                tag_entry->block_start_pc = real_block_start_pc;
            }
            
            current_block_start_idx = i + 1;
        }
    }
}

static void rebuild_iq_priority_masks(uns proc_id) {
    if (ZERECO_IQ_PRIORITY_POLICY != 2)
        return;

    Dependency_Chain_Cache_Entry* block_cache = block_caches[proc_id];
    Dependency_Chain_Cache_Entry* dep_cache = dependency_chain_caches[proc_id];
    uint64_t enabled_masks[BLOCK_CACHE_SIZE];
    uint64_t suppressed_masks[BLOCK_CACHE_SIZE];
    memset(enabled_masks, 0, sizeof(enabled_masks));
    memset(suppressed_masks, 0, sizeof(suppressed_masks));

    STAT_EVENT(proc_id, ZERECO_IQ_FILTER_REBUILDS);
    for (int i = 0; i < DEPENDENCY_CHAIN_CACHE_SIZE; ++i) {
        Dependency_Chain_Cache_Entry* dep_entry = &dep_cache[i];
        if (!dep_entry->is_valid)
            continue;
        for (uns f = 0; f < dep_entry->block_fragment_count; ++f) {
            Dependency_Chain_Block_Fragment* fragment =
              &dep_entry->block_fragments[f];
            int block_index = fragment->block_start_pc % BLOCK_CACHE_SIZE;
            Dependency_Chain_Cache_Entry* block_entry =
              &block_cache[block_index];
            if (!block_entry->is_valid ||
                block_entry->h2p_branch_pc != fragment->block_start_pc)
                continue;
            uint64_t fragment_mask =
              fragment->priority_candidate_mask &
              block_entry->iq_priority_candidate_mask;
            if (dep_entry->iq_priority_enabled)
                enabled_masks[block_index] |= fragment_mask;
            else
                suppressed_masks[block_index] |= fragment_mask;
        }
    }

    for (int i = 0; i < BLOCK_CACHE_SIZE; ++i) {
        Dependency_Chain_Cache_Entry* block_entry = &block_cache[i];
        if (!block_entry->is_valid)
            continue;
        uint64_t old_mask = block_entry->iq_priority_mask;
        /* Unknown/evicted slice ownership stays priority by default.  Clear a
           bit only when at least one current slice suppresses it and no current
           RF-uncovered slice still needs it. */
        uint64_t exclusively_suppressed =
          suppressed_masks[i] & ~enabled_masks[i];
        block_entry->iq_priority_mask =
          block_entry->iq_priority_candidate_mask & ~exclusively_suppressed;
        if (block_entry->iq_priority_mask != old_mask)
            STAT_EVENT(proc_id, ZERECO_IQ_FILTER_BLOCK_MASK_UPDATES);
    }
}

void add_dependency_chain(uns proc_id, Op* ordered_ops, int ordered_op_count) {
    if (ordered_op_count < 1) return;

    // --- 파트 0: 이제 함수는 이미 정렬된 '스냅샷'을 받음 ---

    Addr block_start_pc_map[FILL_BUFFER_SIZE];
    uns block_op_idx_map[FILL_BUFFER_SIZE];
    uns block_total_ops_map[FILL_BUFFER_SIZE];
    build_block_position_maps(ordered_ops, ordered_op_count,
                              block_start_pc_map, block_op_idx_map,
                              block_total_ops_map);

    int h2p_indices[FILL_BUFFER_SIZE];
    int h2p_count = collect_h2p_indices(proc_id, ordered_ops, ordered_op_count,
                                        h2p_indices);
    if (h2p_count == 0) return;

    bool is_data_dependent_union[FILL_BUFFER_SIZE];
    memset(is_data_dependent_union, false, sizeof(is_data_dependent_union));
    bool is_priority_candidate_union[FILL_BUFFER_SIZE];
    memset(is_priority_candidate_union, false,
           sizeof(is_priority_candidate_union));

    int first_union_dep_idx = ordered_op_count;
    int last_h2p_idx = -1;

    // --- 파트 1/2: snapshot 안의 모든 H2P에 대해 독립 backward walk 수행 ---
    for (int h = 0; h < h2p_count; h++) {
        int trigger_op_idx = h2p_indices[h];
        bool is_data_dependent[FILL_BUFFER_SIZE];
        int first_dep_op_idx =
            build_dependency_mask_for_target(ordered_ops, trigger_op_idx,
                                             is_data_dependent);
        bool is_priority_candidate[FILL_BUFFER_SIZE];
        build_iq_priority_candidate_mask(ordered_ops, first_dep_op_idx,
                                         trigger_op_idx, is_data_dependent,
                                         is_priority_candidate);

        if (first_dep_op_idx < first_union_dep_idx)
            first_union_dep_idx = first_dep_op_idx;
        if (trigger_op_idx > last_h2p_idx)
            last_h2p_idx = trigger_op_idx;

        for (int i = first_dep_op_idx; i <= trigger_op_idx; i++) {
            if (is_data_dependent[i])
                is_data_dependent_union[i] = true;
            if (is_priority_candidate[i])
                is_priority_candidate_union[i] = true;
        }

        commit_dependency_chain_entry(proc_id, ordered_ops, first_dep_op_idx,
                                      trigger_op_idx, is_data_dependent,
                                      is_priority_candidate,
                                      block_start_pc_map, block_op_idx_map,
                                      block_total_ops_map);
    }

    // --- 파트 3: 모든 H2P walk 결과의 union mask를 Block Cache에 OR 누적 ---
    if (first_union_dep_idx < ordered_op_count) {
        commit_block_cache_masks(proc_id, ordered_ops, block_start_pc_map,
                                 block_op_idx_map, block_total_ops_map,
                                 is_data_dependent_union,
                                 is_priority_candidate_union,
                                 first_union_dep_idx,
                                 last_h2p_idx);
    }

    rebuild_iq_priority_masks(proc_id);

    log_full_cache_state(proc_id, cycle_count);
}

// =================================================================
// 주기적인 함수
// =================================================================
void cycle_backward_walk_engine(uns proc_id) {
    Backward_Walk_Engine* engine = bw_engines[proc_id];

    if (engine->state == BW_WALKING) {
        engine->walk_cycles_remaining--;

        if (engine->walk_cycles_remaining == 0) {
            add_dependency_chain(proc_id, engine->snapshot_buffer, engine->snapshot_op_count);
            engine->state = BW_IDLE;
            reset_fill_buffer(proc_id);
        }
    }
}

void periodically_reset_caches(uns proc_id) {
    if (block_caches && block_caches[proc_id]) {
        for (int i = 0; i < BLOCK_CACHE_SIZE; ++i) {
            if (block_caches[proc_id][i].is_valid) {
                block_caches[proc_id][i].dependency_mask = 0;
                block_caches[proc_id][i].iq_priority_candidate_mask = 0;
                block_caches[proc_id][i].iq_priority_mask = 0;
                block_caches[proc_id][i].chain_length = 0;
            }
        }
    }
    if (empty_block_tag_store && empty_block_tag_store[proc_id]) {
        memset(empty_block_tag_store[proc_id], 0, sizeof(Block_Cache_Tag_Entry) * EMPTY_BLOCK_TAG_STORE_SIZE);
    }
}


// =================================================================
// 캐시 조회 함수 (두 캐시에 대한 각각의 함수)
// =================================================================

// H2P 전체 체인 조회
Dependency_Chain_Cache_Entry* get_dependency_chain(uns proc_id, Addr pc) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    int entry_index = pc % DEPENDENCY_CHAIN_CACHE_SIZE;
    Dependency_Chain_Cache_Entry* entry = &dependency_chain_caches[proc_id][entry_index];
    if (entry->is_valid && entry->h2p_branch_pc == pc) {
        return entry;
    }
    return NULL;
}

// 기본 블록 체인 조회
Dependency_Chain_Cache_Entry* get_dependency_chain_block(uns proc_id, Addr pc) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    int entry_index = pc % BLOCK_CACHE_SIZE;
    Dependency_Chain_Cache_Entry* entry = &block_caches[proc_id][entry_index];
    if (entry->is_valid && entry->h2p_branch_pc == pc) {
        return entry;
    }
    return NULL;
}
