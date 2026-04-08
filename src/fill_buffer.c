#include "fill_buffer.h"
#include <stdlib.h>
#include <string.h>
#include "globals/assert.h"
#include "core.param.h"
#include "dependency_chain_cache.h"
#include "on_off_path_cache.h"
#include "node_stage.h"
#include "log/fill_buffer_log.h"

Fill_Buffer** retired_fill_buffers;

void init_fill_buffer(uns proc_id, const char* name) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (!retired_fill_buffers) {
        retired_fill_buffers = (Fill_Buffer**)calloc(NUM_CORES, sizeof(Fill_Buffer*));
        ASSERT(0, retired_fill_buffers);
    }

    retired_fill_buffers[proc_id] = (Fill_Buffer*)malloc(sizeof(Fill_Buffer));
    ASSERT(proc_id, retired_fill_buffers[proc_id]);
    Fill_Buffer* fb = retired_fill_buffers[proc_id];
    
    fb->name = strdup(name);
    fb->size = FILL_BUFFER_SIZE;
    fb->entries = (Op*)calloc(fb->size, sizeof(Op));
    ASSERT(proc_id, fb->entries);
    reset_fill_buffer(proc_id);
}

void reset_fill_buffer(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    Fill_Buffer* fb = retired_fill_buffers[proc_id];
    if (fb) {
        fb->head = 0;
        fb->tail = 0;
        fb->count = 0;
        memset(fb->entries, 0, sizeof(Op) * fb->size);
    }
}

void fill_buffer_add(uns proc_id, Op* op) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (bw_engines[proc_id]->state == BW_WALKING) {
    return; // 엔진 작동 중이면 buffer에 추가 금지
    }
    Fill_Buffer* fb = retired_fill_buffers[proc_id];
    if (!fb) return;

    // Buffer is full, overwrite the oldest entry
    if (fb->count == fb->size) {
        // 1. 덮어씌워질, 즉 가장 오래된 op(head 위치)를 가져옵니다.
        Op* evicted_op = &fb->entries[fb->head];

        // 2. 만약 이 op가 H2P 브랜치라면, 경로 기록 함수를 호출합니다.
        if (evicted_op->oracle_info.hbt_pred_is_hard) {
            record_on_off_path(proc_id, evicted_op);

            // BW Walk 트리거: 엔진이 유휴 상태일 때만 새 Walk를 시작
            // evict되는 oldest H2P는 스냅샷에서 제외 (older ops 부재로 chain 생성 불가)
            Backward_Walk_Engine* engine = bw_engines[proc_id];
            if (engine && engine->state == BW_IDLE) {
                int idx = (fb->head + 1) % fb->size;  // evicted H2P 제외
                engine->snapshot_op_count = 0;
                for (int i = 0; i < fb->count - 1; i++) {
                    engine->snapshot_buffer[engine->snapshot_op_count++] = fb->entries[idx];
                    idx = (idx + 1) % fb->size;
                }
                engine->walk_cycles_remaining = BACKWARD_WALK_CYCLES;
                engine->state = BW_WALKING;
            }
        }
        // head 이동
        fb->head = (fb->head + 1) % fb->size;
        fb->count--;
    }

    // 새 op 추가
    op->chain_bit = FALSE;
    fb->entries[fb->tail] = *op;
    fb->tail = (fb->tail + 1) % fb->size;
    fb->count++;
}