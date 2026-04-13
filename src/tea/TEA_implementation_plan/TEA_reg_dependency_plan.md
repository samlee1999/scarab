# TEA Register Dependency 구현

> **상태**: ✅ 단일 H2P 구현 완료 | 🔧 §4 다중 H2P는 Work F에서 구현

---

## 1. 해결한 문제

기존 `tea_rename_op()`은 Shadow RAT에서 arch→phys 매핑만 읽고 producer Op를 추적하지 않았음.
모든 TEA ops의 `srcs_not_rdy_vector == 0` → RS dispatch 즉시 ready → Early Flush 타이밍이
실제 dependency chain latency를 반영하지 못하고 수십 cycle 앞당겨짐.

---

## 2. 해결 방식 (구현 완료)

| 변경 | 내용 |
|------|------|
| `Shadow_RAT` 구조체 확장 | `gp_producer_ops/unums`, `vec_producer_ops/unums` 4개 필드 추가 |
| `init_shadow_rat()` | producer 배열 calloc |
| `reset_tea_rename_stage()` | producer 배열 memset |
| `shadow_rat_snapshot()` | `map_data->reg_map[]` 순회 → Main thread producer Op 포인터 + unique_num 복사 |
| `tea_add_src_dependency()` | 신규 함수. `add_src_from_map_entry()`의 TEA 버전 (`op_num < consumer op_num` ASSERT 제거) |
| `tea_rename_op()` | (A) Shadow RAT에서 producer 조회 → (B) `tea_add_src_dependency()` → (C) dst의 producer 기록 → (D) `add_to_wake_up_lists(cmp_wake)` 호출 |

**Wake_Up_Entry 정리**: 별도 코드 불필요. `free_op()` → `free_wake_up_list()` 자동 호출
(`op_pool.c`). Main op의 wake_up_head에 등록된 TEA Entry는 `op_pool_valid` 체크로 자동 스킵.

**`add_to_wake_up_lists()` 호환성**: 동일 코어(`op->proc_id == src_op->proc_id`) 조건 충족.
`map_data` 유효성: `cmp_set_all_stages()` → `set_map_data()` 이후 `update_tea_rename_stage()` 호출 (`cmp_model.c:257, 286`).

---

## 3. 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `src/tea/tea_rename.h` | `Shadow_RAT`에 `producer_ops`, `producer_unums` 필드 4개 추가 |
| `src/tea/tea_rename.c` | `init_shadow_rat()`: producer 배열 calloc |
| | `reset_tea_rename_stage()`: producer 배열 memset |
| | `shadow_rat_snapshot()`: `map_data->reg_map[]`에서 producer 복사 |
| | `tea_rename_op()`: 의존성 설정 + `add_to_wake_up_lists()` 호출 |
| | 신규 함수 `tea_add_src_dependency()` |

**변경 불필요**: `node_stage.c`, `map.c`, `cmp_model.c`, `exec_stage.c`, `map_rename.c`
(`reg_file_consume()` TEA skip 유지, `exec_stage_dep_wakeup()`은 thread 구분 없이 동작)

---

## 4. 다중 H2P — Dependency 관리 (Work F)

다중 H2P에서는 chain별 선택적 flush와 TEA 생존이 가능해지므로, 단일 H2P에서
발생하지 않던 두 가지 not-rdy bit stall 문제가 생긴다.

### 4.1 `flush_tea_ops_by_chain_id()` — dependent ops not-rdy bit 강제 clear

**문제**: Chain X ops를 `free_op()`으로 해제할 때, `free_op()` → `free_wake_up_list()`는
해제된 op의 wake_up_head **메모리를 반환**하지만, wake_up_head에 등록된 dependent ops
(다른 chain 또는 같은 chain 내 후속 op)의 `srcs_not_rdy_vector` bit는 **clear되지 않은 채로 남음**.
결과적으로 해당 ops는 RS에서 영원히 issue되지 않는다.

**해결**: `flush_tea_ops_by_chain_id()` step 5 (node table 순회)에서 `free_op()` 호출 **전에**
`op->wake_up_head`를 순회하여 dependent ops의 not-rdy bit를 강제 clear한다.
만약 clear 후 `srcs_not_rdy_vector == 0 && OS_IN_RS && !in_rdy_list`이면 ready list에 추가.

**구현 위치**: `src/node_stage.c: flush_tea_ops_by_chain_id()` step 5, `free_op(op)` 직전

```c
// free_op(op) 호출 전에 삽입:
Wake_Up_Entry* wake = op->wake_up_head;
while (wake) {
  Op* dep_op = wake->op;
  if (dep_op->op_pool_valid && dep_op->unique_num == wake->unique_num) {
    clear_not_rdy_bit(dep_op, wake->rdy_bit);
    if (dep_op->state == OS_IN_RS &&
        dep_op->srcs_not_rdy_vector == 0x0 &&
        !dep_op->in_rdy_list) {
      dep_op->next_rdy = node->rdy_head;
      node->rdy_head = dep_op;
      dep_op->in_rdy_list = TRUE;
    }
  }
  wake = wake->next;
}
// 그 후 free_op(op) — free_wake_up_list()가 wake_up_head 메모리 반환
```

**주의**: 이 코드의 목적은 메모리 반환이 아닌 **not-rdy bit 해제**. 두 작업은 독립적:
- `free_wake_up_list()` (in `free_op()`): producer의 wake_up_head 메모리 반환 ✅ 자동
- 위 코드: dependent op의 `srcs_not_rdy_vector` bit 강제 clear (별도로 필요)

`node_stage.c`는 이미 `map.h` include → `clear_not_rdy_bit()` 접근 가능, `op.h` → `op_info.h`
경로로 `srcs_not_rdy_vector`, `Wake_Up_Entry` 접근 가능. include 추가 불필요.

**단일 H2P에서 문제없는 이유**: `terminate_tea_thread()` → 모든 TEA ops 일괄 flush.
dependent op도 함께 flush되므로 stale not-rdy bit를 가진 유효한 op가 존재하지 않음.

---

### 4.2 `recover_tea_on_flush()` — 생존 chain의 flushed Main op 의존성 정리

**문제 시나리오**:

```
Cycle N:   TEA trigger, shadow_rat_snapshot: R1의 producer = Main op Y (op_num=150, in-flight)
Cycle N+5: TEA op A renamed, R1 의존성 등록, Main op Y의 wake_up_head에 등록
Cycle N+10: Main misprediction at recovery_op_num=120
            → cmp_recover():
              (1) recover_node_stage() (cmp_model.c:437)
                  → Main op Y (op_num=150 >= 120) flush → free_op(Y) → op_pool_valid = FALSE
              (2) recover_tea_on_flush(proc_id, 120) (cmp_model.c:445)
                  → Chain (target_h2p_op_num=100 < 120) → 생존!
결과: TEA op A의 srcs_not_rdy_vector bit가 영원히 clear되지 않음 → RS에서 영구 대기
```

**타이밍 보장**: `recover_node_stage()`(line 437)가 `recover_tea_on_flush()`(line 445)보다
먼저 실행되므로, `recover_tea_on_flush()` 시점에서 flushed Main ops는 이미 `op_pool_valid = FALSE`.
→ `op_pool_valid + unique_num` 체크만으로 stale producer를 식별 가능.

**해결**: `recover_tea_on_flush()` chain 종료 루프 이후, surviving chain이 있으면 node table을
순회하여 stale not-rdy bit를 강제 clear한다.

**구현 위치**: `src/cmp_model.c: recover_tea_on_flush()` — chain 종료 루프(`TEA_multi_h2p_plan.md §7.2`) 이후에 추가

```c
// recover_tea_on_flush() chain 종료 루프 이후:
if (tea->num_active_chains > 0) {
  // surviving chains exist — scan for stale not-rdy bits from flushed producers
  for (Op* op = node->node_head; op; op = op->next_node) {
    if (op->thread_id != 1) continue;

    for (uns i = 0; i < op->oracle_info.num_srcs; i++) {
      if (!(op->srcs_not_rdy_vector & (0x1u << i))) continue;
      Src_Info* src = &op->oracle_info.src_info[i];
      if (!src->op->op_pool_valid || src->op->unique_num != src->unique_num) {
        clear_not_rdy_bit(op, i);
      }
    }
    if (op->srcs_not_rdy_vector == 0x0 && op->state == OS_IN_RS && !op->in_rdy_list) {
      op->next_rdy = node->rdy_head;
      node->rdy_head = op;
      op->in_rdy_list = TRUE;
    }
  }
}
```

**`node` 전역 포인터 유효성**: `cmp_istreams()`(line 234)에서 `cmp_set_all_stages(proc_id)` 호출 후
`cmp_recover()`가 실행되므로 `node` 전역은 이미 올바른 proc의 node_stage를 가리킴.

**`cmp_model.c` include 경로**: `cmp_model.h` → `map.h` → `op.h` → `op_info.h` 로
`Src_Info`, `clear_not_rdy_bit()` 접근 가능. include 추가 불필요.

**정확성**: TEA ops는 oracle 값으로 실행되므로 producer flush에 따른 functional 오류 없음.
force-ready 처리로 실제 latency와 약간 차이가 생기나, recovery 시점에서 original producer의
timing은 이미 무의미.

**단일 H2P에서 문제없는 이유**: 현재 `recover_tea_on_flush()`가 무조건 `terminate_tea_thread()`
호출 → `num_active_chains = 0` → guard 조건 `if (tea->num_active_chains > 0)`에서 스킵.

---

### 4.3 수정 파일 요약

| 파일 | 함수 | 변경 내용 | 연결 계획 |
|------|------|----------|----------|
| `src/node_stage.c` | `flush_tea_ops_by_chain_id()` | step 5에 not-rdy bit clear 블록 추가 | `TEA_multi_h2p_plan.md §6.1` |
| `src/cmp_model.c` | `recover_tea_on_flush()` | chain 종료 루프 이후 §4.2 코드 블록 추가 | `TEA_multi_h2p_plan.md §7.2` |
| `src/cmp_model.h` | `recover_tea_on_flush()` | 시그니처 변경 (`recovery_op_num` 파라미터 추가) | `TEA_multi_h2p_plan.md §7.1` |
