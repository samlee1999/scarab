# TEA Op 관리: 현재 구현 상태

**최종 갱신**: 2026-04-12

---

## 1. TEA Op 식별 — ✅ 완료

| 메커니즘 | 위치 | 상세 |
|---------|------|------|
| `op->thread_id = 1` | `tea_fetch_stage.c:214` | TEA ops 식별 |
| `tea_op_counter = 0x8000000000000000ULL` | `tea_thread.c:90` | Main op_num 충돌 방지 |
| `tea->main_h2p_op` 포인터 | `tea_thread.c:131` | TEA ↔ Main H2P op 연결 |

---

## 2. TEA Op 생성 (Fetch → Rename) — ✅ 완료

### `tea_create_op_from_cache()` (`tea_fetch_stage.c:195`)

```c
Op* tea_op = alloc_op(proc_id);
tea_op->thread_id = 1;
tea_op->op_num = tea->tea_op_counter++;
// H2P branch: oracle_info/recovery_info를 Main H2P에서 복사
if (is_h2p_branch) {
  tea_op->oracle_info = tea->h2p_oracle_info;
  tea_op->recovery_info = tea->h2p_recovery_info;
}
tea->tea_op_count++;  // 파이프라인 내 TEA op 수 추적
```

H2P branch 판별: `current_chain_idx == total_chain_length - 1`

### 파이프라인 Backpressure — ✅ 완료 (2026-04-12)

```
Fetch Stage (`tea_fetch_stage.c:152`):
  if (tea_fetch->sd.op_count > 0) return;  // Rename이 소비 안 했으면 stall

Rename Stage (`tea_rename.c:443`):
  if (rename->sd.op_count > 0) {
    STAT_EVENT(proc_id, TEA_RENAME_STALL_DISPATCH);
    return;  // Node Stage가 소비 안 했으면 stall
  }
```

---

## 3. TEA Op Dispatch (Node Table) — ✅ Work I 완료

### `tea_dispatch_to_rs()` (`node_stage.c:418`)

TEA ops를 Node Table에 삽입하면서 **직접 RS에 dispatch** (Main thread의 `node_issue_queue_dispatch()` 우회).

```c
// Node Table linked list에 추가
op->in_node_list = TRUE;
node->node_tail = op;

// Direct RS dispatch
int64 rs_id = node_dispatch_find_emptiest_rs(op);
if (rs_id != NODE_ISSUE_QUEUE_RS_SLOT_INVALID) {
  op->state = OS_IN_RS;
  rs->rs_op_count++;
  rs->tea_op_count++;
  // srcs_not_rdy_vector == 0이면 ready list 등록
} else {
  op->state = OS_IN_ROB;  // RS full → tea_dispatch_retry()에서 재시도
  STAT_EVENT(TEA_RS_STALLS);
}
```

**핵심**: `next_op_into_rs`를 TEA op으로 설정하지 않음 → Main thread dispatch와 완전 분리.

### `tea_dispatch_retry()` (`node_stage.c:501`)

매 cycle `node_issue_queue_update()` 직후 호출. `OS_IN_ROB` 상태의 TEA ops를 Node Table에서 찾아 RS dispatch 재시도.

### `node_issue_queue_dispatch()` (`node_issue_queue.cc:287`)

```c
for (op = node->next_op_into_rs; op; op = op->next_node) {
  if (op->thread_id == 1) continue;  // TEA ops 완전 skip
  // Main ops only ...
  rs->main_op_count++;
}
```

### RS 파티셔닝 (`node_stage.h:38-52`)

```c
typedef struct Reservation_Station_struct {
  uns32 rs_op_count;
  uns32 main_op_count;
  uns32 tea_op_count;
  uns32 main_rs_limit;  // size - TEA_RS_RESERVATION/NUM_RS
  uns32 tea_rs_limit;   // TEA_RS_RESERVATION/NUM_RS
} Reservation_Station;
```

---

## 4. TEA Op 실행 (Exec → Dcache) — ✅ 완료

### Exec Stage (`exec_stage.c`)

- **0-latency 비메모리 TEA op** (`exec_stage.c:559`): `exec_stage_launch_op()` 내에서 동일 cycle에 `tea_op_completed()` 직접 호출 (exec_stage_clear_fu가 다음 cycle에 실행되기 전에 처리)
- **일반 비메모리 TEA op**: `exec_stage_clear_fu()` → `tea_op_completed()`
- **H2P branch**: `exec_stage_bp_resolve()` → Early Flush 판단 + `STAT_EVENT(TEA_OPS_EXECUTED)`

### Dcache Stage (`dcache_stage.c:232`)

- **TEA Store**: `tea_store_buffer_write()` 호출. 버퍼 full이면 `terminate_tea_thread()`.
- **TEA Load (buffer hit)**: Store forwarding 성공 → `done_cycle` + `wake_up_ops()` + `tea_op_completed()`
- **TEA Load (buffer miss)**: D-cache 정상 접근 (read-only, 논문 IV-E: prefetch 효과)

### `tea_op_completed()` (`tea_thread.c:251`)

```c
void tea_op_completed(uns proc_id, Op* op) {
  op->state = OS_DONE;          // node_retire_tea_ops()가 OS_DONE을 보고 retire
  STAT_EVENT(proc_id, TEA_OPS_EXECUTED);
  // ★ tea_op_count는 여기서 감소하지 않음 — node_retire_tea_ops()에서 감소
}
```

---

## 5. TEA Op 정리 (Retire / Terminate) — ✅ 완료

### 정상 경로: `node_retire_tea_ops()` (`node_stage.c:659`)

```
비메모리 op:  exec_stage_clear_fu() → tea_op_completed() → op->state = OS_DONE
              node_retire_tea_ops(): OP_DONE(op) || OS_DONE → free_op() + tea_op_count--

메모리 op:    dcache_stage → done_cycle 설정 → tea_op_completed() → OS_DONE
              node_retire_tea_ops(): op->state == OS_DONE → free_op() + tea_op_count--
```

**`tea_op_count` 감소 위치**: `node_retire_tea_ops()`가 유일한 권한적 감소 지점.
`tea_op_completed()`는 `OS_DONE`만 설정하고 카운터는 건드리지 않음.

```c
// node_stage.c:700-704
if (tea_threads && tea_threads[node->proc_id]) {
  Tea_Thread* tea_state = tea_threads[node->proc_id];
  if (tea_state->tea_op_count > 0)
    tea_state->tea_op_count--;
}
```

### 강제 종료 경로: `terminate_tea_thread()` (`tea_thread.c:154`)

```c
recover_tea_fetch_stage(proc_id);       // fetch->sd 내 ops free_op()
recover_tea_rename_stage(proc_id);      // rename->sd 내 ops free_op()
flush_tea_ops_from_node_stage(proc_id); // Node Table 내 모든 TEA ops 제거
reset_tea_preg_pool(proc_id);           // TEA preg pool 반환
reset_tea_store_buffer(proc_id);        // Store buffer 초기화
tea->tea_op_count = 0;                  // 강제 리셋
```

### `flush_tea_ops_from_node_stage()` (`node_stage.c:1127`)

5-step flush:
1. **Ready list**: TEA ops 제거. OS_SCHEDULED/OS_MISS이면 RS 카운터 동기화 (same-cycle leak 방지)
2. **Scheduling buffer** (`node->sd`): TEA ops 제거
3. **`next_op_into_rs`**: TEA op을 가리키면 다음 non-TEA op으로 전진
4. **Node Table**: TEA ops 순회 → RS 카운터 감소 (OS_IN_ROB/SCHEDULED/MISS/DONE 제외) → `free_op()`
5. **`node_tail` 복구**

---

## 6. 미구현 항목

| 항목 | 상태 | 비고 |
|------|------|------|
| `h2p_chain_id` in `Op` | ❌ 미구현 | 다중 H2P (Work F) 구현 시 추가 |
| `flush_tea_ops_by_chain_id()` | ❌ 미구현 | Work F 구현 시 추가 |
| `tea_store_buffer_clear_by_chain_id()` | ❌ 미구현 | Work F 구현 시 추가 |
