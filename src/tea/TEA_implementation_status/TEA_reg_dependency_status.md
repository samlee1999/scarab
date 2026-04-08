# TEA Register Dependency: 현재 구현 상태

> **설계 목표**: TEA 스레드의 register dependency를 올바르게 추적하여 정확한 타이밍 시뮬레이션을 달성.
> 논문의 Shadow RAT + Poison bit 메커니즘 중 Poison bit는 구현하지 않으며,
> 시뮬레이션 편의를 위해 TEA 스레드에서는 무조건 올바른 register dependency를 추적 가능하도록 설계한다.
>
> **Poison bit 미구현 이유**: 논문에서 Poison bit는 TEA의 dependency chain이 잘못되었을 때
> 이를 감지하여 precomputation을 조기 종료하는 용도. 시뮬레이터에서는 oracle 값을 사용하므로
> dependency chain이 항상 올바르고, Poison bit 감지 메커니즘이 불필요.

---

## 1. 현재 상태 요약 — ✅ 구현 완료 (2026-03-18)

| 컴포넌트 | 상태 | 위치 |
|---------|------|------|
| Shadow RAT (arch→phys 매핑) | ✅ 구현됨 | `tea_rename.c:176-215` |
| TEA preg pool (전용 물리 레지스터) | ✅ 구현됨 | `tea_rename.c:296-329` |
| Poison bit | ❌ 미구현 (의도적) | — |
| **Inter-op dependency wakeup** | **✅ 구현됨** | `tea_rename.c:494-585` |
| **Producer Op 추적 (Shadow RAT)** | **✅ 구현됨** | `tea_rename.h:84-87`, `tea_rename.c:215-235` |

**구현 내용**:
- Shadow RAT에 `gp_producer_ops/unums`, `vec_producer_ops/unums` 필드 추가
- `shadow_rat_snapshot()`: `map_data->reg_map[]`에서 producer Op 포인터 복사
- `tea_add_src_dependency()`: TEA 전용 src dependency 설정 (cross-thread ASSERT 제거)
- `tea_rename_op()`: src register의 producer 조회 → dependency 등록 + dst의 producer 갱신
- `add_to_wake_up_lists()` 호출로 Main thread의 wakeup 인프라에 연결

---

## 2. Main Thread의 Dependency 추적 경로

Main thread에서 op의 register dependency가 설정되는 전체 경로:

```
map_stage.c:map_stage_process_op(op)
  │
  ├─ map.c:thread_map_op(op)
  │   │
  │   ├─ read_reg_map(op)                          (map.c:261-277)
  │   │   └─ for each src reg:
  │   │       ind = id << 1 | map_data->map_flags[id]
  │   │       Map_Entry* entry = &map_data->reg_map[ind]
  │   │       add_src_from_map_entry(op, entry, REG_DATA_DEP)
  │   │
  │   ├─ read_store_map(op)                        (map.c:284-296)
  │   │   └─ add_src_from_map_entry(op, last_store, MEM_ADDR_DEP)
  │   │
  │   └─ update_map(op)                            (map.c:301-328)
  │       └─ for each dst reg:
  │           reg_map[ind].op = op
  │           reg_map[ind].unique_num = op->unique_num
  │
  ├─ map.c:add_to_wake_up_lists(op, &op->oracle_info, cmp_wake)
  │   │                                             (map.c:578-654)
  │   └─ for each src in oracle_info.src_info[]:
  │       if (src_op->op_pool_valid && unique_num 일치):
  │         Wake_Up_Entry 할당 (map_data->free_list_head)
  │         src_op->wake_up_head 리스트에 등록
  │         if (src_op->wake_up_signaled[type]):
  │           즉시 clear_not_rdy_bit + wake_action(cmp_wake)
  │       else:
  │         src_op 이미 retired → clear_not_rdy_bit
  │
  └─ map_rename.c:reg_file_rename(op)              (map_rename.c:352-383)
      └─ 물리 레지스터 할당 + arch→phys 매핑 갱신
```

### 2.1 `add_src_from_map_entry()` (`map.c:710-730`)

```c
void add_src_from_map_entry(Op* op, Map_Entry* map_entry, Dep_Type type) {
  uns src_num = op->oracle_info.num_srcs++;
  Src_Info* info = &op->oracle_info.src_info[src_num];

  ASSERTM(..., map_entry->op_num < op->op_num, ...);  // producer < consumer

  info->type = type;
  info->op = map_entry->op;
  info->op_num = map_entry->op_num;
  info->unique_num = map_entry->unique_num;

  set_not_rdy_bit(op, src_num);  // srcs_not_rdy_vector |= (1 << src_num)
}
```

**핵심**: `Map_Entry`에서 producer `Op*`와 `unique_num`을 읽어 `Src_Info`에 저장하고,
`srcs_not_rdy_vector`에 not-ready 비트를 세팅.

### 2.2 `add_to_wake_up_lists()` (`map.c:578-654`)

```c
void add_to_wake_up_lists(Op* op, Op_Info* op_info,
                           void (*wake_action)(Op*, Op*, uns8)) {
  for (ii = 0; ii < op_info->num_srcs; ii++) {
    Src_Info* src_info = &op_info->src_info[ii];
    Op* src_op = src_info->op;

    if (src_op->op_pool_valid && src_op->unique_num == src_info->unique_num) {
      // Wake_Up_Entry 할당 (map_data->free_list_head에서)
      Wake_Up_Entry* wake = map_data->free_list_head;
      map_data->free_list_head = wake->next;
      map_data->active_wake_up_entries++;

      wake->op = op;                   // dependent op
      wake->unique_num = op->unique_num;
      wake->dep_type = src_info->type;
      wake->rdy_bit = ii;

      // producer의 wake_up list에 추가
      src_op->wake_up_tail->next = wake;  (또는 head에 설정)

      // producer가 이미 실행됨 → 즉시 wakeup
      if (src_op->wake_up_signaled[src_info->type]) {
        clear_not_rdy_bit(op, ii);
        wake_action(src_op, op, ii);  // → cmp_wake()
      }
    } else {
      // producer 이미 retired → 즉시 ready
      clear_not_rdy_bit(op, ii);
    }
  }
}
```

### 2.3 `wake_up_ops()` (Producer 실행 시, `map.c:533-573`)

```c
void wake_up_ops(Op* op, Dep_Type type, void (*wake_action)(...)) {
  for (Wake_Up_Entry* temp = op->wake_up_head; temp; temp = temp->next) {
    Op* dep_op = temp->op;
    if (dep_op->unique_num == temp->unique_num && dep_op->op_pool_valid) {
      if (test_not_rdy_bit(dep_op, temp->rdy_bit)) {
        clear_not_rdy_bit(dep_op, temp->rdy_bit);
        wake_action(op, dep_op, temp->rdy_bit);  // → cmp_wake()
      }
    }
  }
  op->wake_up_signaled[type] = TRUE;
}
```

호출 위치: `exec_stage.c:260` → `exec_stage_dep_wakeup(op)` → `wake_up_ops(op, REG_DATA_DEP, model->wake_hook)`

### 2.4 `cmp_wake()` (`cmp_model.c:357-384`)

```c
void cmp_wake(Op* src_op, Op* dep_op, uns8 rdy_bit) {
  set_node_stage(&cmp_model.node_stage[src_op->proc_id]);
  ASSERT(src_op->proc_id == dep_op->proc_id);  // 동일 코어

  if (dep_op->state != OS_IN_RS) {
    // RS 진입 전이면 rdy_cycle만 갱신
    dep_op->rdy_cycle = MAX2(dep_op->rdy_cycle, src_op->wake_cycle);
    return;
  }

  simple_wake(src_op, dep_op, rdy_bit);  // rdy_cycle 갱신 + clear_not_rdy_bit

  if (dep_op->srcs_not_rdy_vector == 0x0 && cycle_count >= dep_op->issue_cycle
      && !dep_op->in_rdy_list) {
    dep_op->next_rdy = node->rdy_head;
    node->rdy_head = dep_op;
    dep_op->in_rdy_list = TRUE;  // ready list에 추가
  }
}
```

### 2.5 RS Dispatch 시 Ready 판별 (`node_issue_queue.cc:315-323`)

```c
// node_issue_queue_dispatch()에서:
if (op->srcs_not_rdy_vector == 0) {
  op->state = (cycle_count + 1 >= op->rdy_cycle ? OS_READY : OS_WAIT_FWD);
  op->next_rdy = node->rdy_head;
  node->rdy_head = op;
  op->in_rdy_list = TRUE;
}
```

`srcs_not_rdy_vector == 0`이면 즉시 ready list에 추가됨.

---

## 3. TEA Thread의 Dependency 경로 — ✅ 구현 완료

### 3.1 TEA Op 생성 (`tea_fetch_stage.c`)

```c
Op* tea_create_op_from_cache(uns proc_id, Op* cached_op, Flag is_h2p_branch) {
  Op* tea_op = alloc_op(proc_id);
  // alloc_op()에서 초기화:
  //   srcs_not_rdy_vector = 0x0     (op_pool.c:201)
  //   oracle_info.num_srcs = 0       (op_pool.c:244)
  //   wake_up_head = NULL
  tea_op->inst_info = cached_op->inst_info;
  tea_op->table_info = cached_op->table_info;
  // oracle_info.src_info[]는 tea_rename_op()에서 설정됨
}
```

### 3.2 TEA Rename (`tea_rename.c:494-585`)

```c
void tea_rename_op(uns proc_id, Op* op) {
  op->oracle_info.num_srcs = 0;  // 의존성 카운터 초기화

  // Source registers: Shadow RAT에서 phys mapping + producer 조회
  for (uns i = 0; i < table_info->num_src_regs; i++) {
    int phys_reg_id = shadow_rat_read_mapping(srat, arch_reg_id, reg_type);
    op->src_reg_id[i][REG_TABLE_TYPE_PHYSICAL] = phys_reg_id;

    // ✅ Producer Op 조회 (Shadow RAT에서)
    Op* producer_op = srat->gp_producer_ops[arch_reg_id];  // (또는 vec)
    Counter producer_unum = srat->gp_producer_unums[arch_reg_id];

    // ✅ 유효한 producer가 있으면 dependency 등록
    if (producer_op && producer_op->op_pool_valid &&
        producer_op->unique_num == producer_unum) {
      tea_add_src_dependency(op, producer_op, producer_unum, REG_DATA_DEP);
      // → set_not_rdy_bit(op, src_num) → srcs_not_rdy_vector 세팅
    }
  }

  // Destination registers: TEA preg 할당 + Shadow RAT 갱신
  for (uns i = 0; i < table_info->num_dest_regs; i++) {
    int new_phys_reg_id = tea_preg_pool_alloc(tea_pool);
    shadow_rat_write_mapping(srat, arch_reg_id, new_phys_reg_id, reg_type);
    // ✅ Producer Op 기록 (intra-chain dependency용)
    srat->gp_producer_ops[arch_reg_id] = op;
    srat->gp_producer_unums[arch_reg_id] = op->unique_num;
  }

  // ✅ Wakeup list 등록
  extern void cmp_wake(Op*, Op*, uns8);
  add_to_wake_up_lists(op, &op->oracle_info, cmp_wake);
}
```

### 3.3 TEA Dispatch → Issue

`node_issue_queue_dispatch()`에서 RS에 진입 시:
- `srcs_not_rdy_vector == 0` → 즉시 ready list에 추가
- `srcs_not_rdy_vector != 0` → RS에서 wakeup 대기 (producer 실행 완료까지)

### 3.4 TEA Exec — Wakeup 동작

```
exec_stage_dep_wakeup(tea_op)  (exec_stage.c:431)
  → wake_up_ops(tea_op, REG_DATA_DEP, model->wake_hook)
    → tea_op->wake_up_head에 등록된 dependent ops를 wakeup
    → cmp_wake(tea_op, dep_op, rdy_bit) → dep_op ready list 이동
```

TEA op이 다른 TEA op의 producer인 경우 (intra-chain dependency), 정상적으로 wakeup 동작.

### 3.5 `reg_file_consume()` — TEA Skip (올바른 동작)

`map_rename.c`:
```c
void reg_file_consume(Op *op) {
  if (op->thread_id == 1) {
    return;  // TEA ops는 별도 preg pool → Main reg_table 건드리지 않음
  }
}
```

TEA ops의 preg은 TEA preg pool에서 할당. Wakeup은 `srcs_not_rdy_vector`와
Wake_Up_Entry로만 동작하므로 `reg_file_consume()` skip이 wakeup에 영향 없음.

---

## 4. Dependency 동작 시나리오

### 4.1 Cross-thread dependency (TEA op → Main op)

```
Main: MOV R1, [addr]  (op_num=100, 아직 실행 중)
TEA:  ADD R2, R1, R3  (op_num=0x8000000000000005)
```

- `shadow_rat_snapshot()` 시점에 `reg_map[R1]`의 producer = Main MOV op
- `tea_rename_op(ADD)`: `gp_producer_ops[R1]` = Main MOV → `tea_add_src_dependency()` → `srcs_not_rdy_vector |= 0x1`
- Main MOV 실행 완료 → `wake_up_ops()` → `cmp_wake(Main_MOV, TEA_ADD, 0)` → TEA ADD ready

### 4.2 Intra-chain dependency (TEA op → TEA op)

```
TEA: LOAD R1, [x]   → gp_producer_ops[R1] = TEA LOAD
TEA: ADD R2, R1, R3  → producer = TEA LOAD → wakeup 등록
```

TEA LOAD 실행 완료 → `cmp_wake(TEA_LOAD, TEA_ADD, 0)` → TEA ADD ready

### 4.3 Producer 이미 retired

`producer_op->op_pool_valid == FALSE` → 의존성 미등록 → 즉시 ready ✅

### 4.4 Wake_Up_Entry 메모리 관리

- `free_op()` → `free_wake_up_list(op)` 자동 호출로 Wake_Up_Entry 반환 ✅
- `flush_tea_ops_from_node_stage()` → `free_op()` ✅
- `node_retire_tea_ops()` → `free_op()` ✅

---

## 5. Shadow RAT — Producer 추적 ✅ 구현 완료

`Shadow_RAT` 구조체 (`tea_rename.h:62-93`):

```c
typedef struct Shadow_RAT_struct {
  uns8 proc_id;
  int* gp_mappings;      // arch→phys 매핑
  int* vec_mappings;
  uns gp_size;
  uns vec_size;
  Tea_Preg_Free_List* tea_gp_preg_pool;
  Tea_Preg_Free_List* tea_vec_preg_pool;
  int tea_gp_start_idx;
  int tea_vec_start_idx;

  // ✅ Producer Op 추적 필드
  Op**     gp_producer_ops;       // GP reg별 마지막 write op
  Counter* gp_producer_unums;     // 유효성 검증용 unique_num
  Op**     vec_producer_ops;      // VEC reg별 마지막 write op
  Counter* vec_producer_unums;

  Flag is_valid;
} Shadow_RAT;
```

`shadow_rat_snapshot()` (`tea_rename.c:189-252`):
- `reg_file[type]->reg_table[ARCH]`에서 arch→phys 매핑 복사 (기존)
- `map_data->reg_map[]`에서 producer Op 포인터 + unique_num 복사 (신규)

---

## 6. `free_wake_up_list()` — `free_op()` 경로에서 이미 호출됨

### 6.1 `free_wake_up_list()` (`map.c:659-675`)

```c
void free_wake_up_list(Op* op) {
  if (op->wake_up_tail) {
    op->wake_up_tail->next = map_data->free_list_head;
    map_data->free_list_head = op->wake_up_head;
    map_data->active_wake_up_entries -= op->wake_up_count;
    op->wake_up_head = NULL;
    op->wake_up_tail = NULL;
  }
}
```

### 6.2 `free_op()` → `free_wake_up_list()` 자동 호출 (`op_pool.c:177`)

```c
void free_op(Op* op) {
  // ... 기타 정리 ...
  op->op_pool_next = op_pool_free_head;
  op_pool_free_head = op;
  free_wake_up_list(op);   // ← 여기서 자동으로 wake_up list 반환
}
```

**핵심 발견**: `free_op()`은 이미 `free_wake_up_list(op)`을 호출하여 해당 op의
`wake_up_head` 리스트에 있는 모든 Wake_Up_Entry를 `map_data->free_list_head`에 반환한다.

따라서 TEA ops가 flush되거나 retire될 때:
- `flush_tea_ops_from_node_stage()` → `free_op(op)` → `free_wake_up_list(op)` ✅
- `node_retire_tea_ops()` → `free_op(op)` → `free_wake_up_list(op)` ✅

**구현 계획(TEA_reg_dependency_plan.md) §7.2, §7.4의 explicit wake_up 반환 코드는
완전히 중복이며, 추가할 필요가 없다.**

단, §9.1의 다중 H2P `flush_tea_ops_by_chain_id()` 코드는 다른 역할을 수행하므로 필요:
- `free_wake_up_list()`는 producer의 wake_up list를 반환 (메모리 관리)
- §9.1은 dependent op의 `srcs_not_rdy_vector` not-rdy bit를 강제 clear (deadlock 방지)
- 이 두 가지는 서로 독립적인 작업

---

## 7. 계획 검증 — 발견된 문제점 3건

### 7.1 ✅ 확인 완료: §7.2, §7.4는 이미 올바름

**파일**: `op_pool.c:177`, `map.c:659-675`

구현 계획(TEA_reg_dependency_plan.md) §7.2와 §7.4는 `free_op()` → `free_wake_up_list()`에 의해
자동 정리됨을 이미 확인하고, **explicit wake_up 반환 코드가 불필요**하다고 올바르게 결론을 내림.
추가 코드 변경 불필요.

### 7.2 🟡 문제 2: TEA 생존 시 flushed Main op에 대한 의존성 stall

**시나리오** (다중 H2P 또는 향후 TEA selective survival 구현 시 발생):
```
Cycle 100: TEA trigger for H2P (op_num=100)
           shadow_rat_snapshot: R1의 producer = Main op Y (op_num=150, in-flight)
Cycle 105: TEA op A renamed, R1 의존성 등록
           Main op Y의 wake_up_head에 TEA op A 등록
Cycle 110: Main misprediction at op_num=120 → cmp_recover()
           → Main op Y (op_num=150 > 120) flushed → free_op(Y) → free_wake_up_list(Y)
           → recover_tea_on_flush(proc_id)
           → tea->target_h2p_op_num=100 < 120 → TEA 생존!
결과: TEA op A의 srcs_not_rdy_vector bit가 영원히 clear되지 않음 → RS에서 무한 대기
```

**타이밍 순서 근거**: `cmp_recover()` (`cmp_model.c:404-449`)에서:
1. `recover_node_stage()` (line 437) → Main ops flush, `free_op()` → `op_pool_valid = FALSE`
2. `recover_tea_on_flush()` (line 445) → 이 시점에서 flushed Main ops는 이미 `op_pool_valid == FALSE`

**영향**:
- ASSERT 위반: 없음 (op이 RS에 앉아있을 뿐)
- 영구 deadlock: 없음 (H2P가 결국 실행되면 `terminate_tea_thread()` → 모든 TEA ops flush)
- 실질적 영향: TEA ops가 RS 슬롯을 점유한 채 issue되지 않아 자원 낭비

**해결 방안**: `recover_tea_on_flush()`에서 TEA가 생존하는 경우, 생존 TEA ops를 순회하여
flushed producer의 not-rdy bit를 강제 clear. `op_pool_valid + unique_num` 체크만으로 충분.

**현재 영향**: 현재 `recover_tea_on_flush()`는 무조건 `terminate_tea_thread()` 호출하므로
이 문제가 발생하지 않음. 향후 selective survival 구현 시 반드시 해결 필요.

### 7.3 🟡 문제 3: H2P Queue flush 비교 연산자

이전 계획(TEA_multi_h2p_plan.md) §4.5의 `h2p_op_num > recovery_op_num` 조건에
`>=` 연산자가 적용되어야 함 (recovery_op_num 자체도 flush 대상):

```c
// 수정 전: h2p_op_num > recovery_op_num
// 수정 후: h2p_op_num >= recovery_op_num
```

---

## 8. ASSERT 위반 가능성 검증 — 안전 확인

| ASSERT 위치 | 조건 | TEA에서 안전한 이유 |
|-------------|------|---------------------|
| `map.c:741` (`clear_not_rdy_bit`) | `bit < num_srcs` | `tea_add_src_dependency()`가 `num_srcs++` 후 호출 |
| `map.c:751` (`set_not_rdy_bit`) | `bit < num_srcs` | 동일 |
| `map.c:583-585` (`add_to_wake_up_lists`) | `op->proc_id == map_data->proc_id` | TEA/Main 동일 코어, `set_map_data()` 이후 호출 |
| `map.c:599` (`add_to_wake_up_lists`) | `op->proc_id == src_op->proc_id` | TEA/Main 동일 코어 |
| `map.c:660-661` (`free_wake_up_list`) | `op->proc_id == map_data->proc_id` | `free_op()` 호출 시 `set_map_data()` 이후 |
| `tea_rename.c:488` | `new_phys_reg_id >= 0` | preg pool 가용성 체크 후 rename 진행 |
| `node_issue_queue.cc:359` | `srcs_not_rdy_vector == 0x0` | issue 시점이므로 wakeup 완료 후 |

---

## 9. 관련 파일 참조

| 파일 | 역할 | 핵심 함수 |
|------|------|----------|
| `map.h` | Map_Entry, Map_Data, Wake_Up_Entry 정의 | — |
| `map.c` | Dependency 설정 + Wakeup 메커니즘 | `add_src_from_map_entry()`, `add_to_wake_up_lists()`, `wake_up_ops()`, `free_wake_up_list()` |
| `op.h` | `srcs_not_rdy_vector`, `wake_up_head/tail`, `Wake_Up_Entry` | — |
| `op_info.h` | `Src_Info`, `Op_Info.src_info[]`, `MAX_DEPS=128` | — |
| `op_pool.c` | `alloc_op()` 초기화: `srcs_not_rdy_vector=0`, `num_srcs=0` | — |
| `cmp_model.c` | `cmp_wake()` — wake action callback | `cmp_wake()` |
| `exec_stage.c` | TEA op 완료 + wakeup 호출 | `exec_stage_dep_wakeup()`, `exec_stage_clear_fu()` |
| `node_issue_queue.cc` | RS dispatch 시 ready 판별 | `node_issue_queue_dispatch()` |
| `map_rename.c` | `reg_file_consume()` — TEA skip | `reg_file_consume()` |
| `tea/tea_rename.h` | Shadow_RAT (producer 없음) | — |
| `tea/tea_rename.c` | `shadow_rat_snapshot()`, `tea_rename_op()` | — |
