# TEA Register Dependency: 구현 계획

> **설계 방침**: TEA 스레드에서 무조건 올바른 register dependency를 추적 가능하도록 설계.
> Main thread의 Wakeup/Select 메커니즘(`add_to_wake_up_lists()` + `wake_up_ops()`)을
> TEA thread에도 연결하되, Poison bit는 구현하지 않는다.
>
> **핵심 변경**: Shadow RAT에 producer Op 추적 필드를 추가하고,
> `tea_rename_op()`에서 `oracle_info.src_info[]` 설정 + `add_to_wake_up_lists()` 호출.

---

## 1. 구현 개요

```
현재 (broken):
  tea_create_op_from_cache() → alloc_op() [srcs_not_rdy=0, num_srcs=0]
  tea_rename_op()            → phys 매핑만 읽기 [의존성 없음]
  RS dispatch               → 즉시 ready [모든 TEA ops 동시 issue]

구현 후:
  tea_create_op_from_cache() → alloc_op() [srcs_not_rdy=0, num_srcs=0]  (동일)
  tea_rename_op()            → (A) Shadow RAT에서 producer Op 조회
                             → (B) oracle_info.src_info[] 설정 + srcs_not_rdy_vector 세팅
                             → (C) Shadow RAT에 dst의 producer Op 기록
                             → (D) add_to_wake_up_lists() 호출
  RS dispatch               → srcs_not_rdy_vector != 0이면 wakeup 대기
  Producer 실행 시           → wake_up_ops() → cmp_wake() → ready list 이동
```

---

## 2. Shadow RAT 확장 — Producer Op 추적

### 2.1 구조체 변경 (`tea_rename.h`)

```c
typedef struct Shadow_RAT_struct {
  uns8 proc_id;

  /* 기존: arch→phys 매핑 */
  int* gp_mappings;
  int* vec_mappings;
  uns gp_size;
  uns vec_size;

  /* 기존: TEA preg pool */
  Tea_Preg_Free_List* tea_gp_preg_pool;
  Tea_Preg_Free_List* tea_vec_preg_pool;
  int tea_gp_start_idx;
  int tea_vec_start_idx;

  /* 신규: Producer Op 추적 (의존성 wakeup용) */
  Op**     gp_producer_ops;       /* GP reg별 마지막 write op */
  Counter* gp_producer_unums;     /* 유효성 검증용 unique_num */
  Op**     vec_producer_ops;      /* VEC reg별 마지막 write op */
  Counter* vec_producer_unums;

  Flag is_valid;
} Shadow_RAT;
```

### 2.2 초기화 (`init_shadow_rat()`, `tea_rename.c`)

기존 `init_shadow_rat()` 끝에 추가:

```c
static void init_shadow_rat(uns proc_id, Shadow_RAT* srat) {
  // ... 기존 코드 유지 ...

  /* Producer Op 배열 할당 */
  srat->gp_producer_ops = (Op**)calloc(srat->gp_size, sizeof(Op*));
  srat->gp_producer_unums = (Counter*)calloc(srat->gp_size, sizeof(Counter));
  srat->vec_producer_ops = (Op**)calloc(srat->vec_size, sizeof(Op*));
  srat->vec_producer_unums = (Counter*)calloc(srat->vec_size, sizeof(Counter));
}
```

### 2.3 리셋 (`reset_tea_rename_stage()`, `tea_rename.c`)

기존 `reset_tea_rename_stage()` 끝에 추가:

```c
void reset_tea_rename_stage(uns proc_id) {
  // ... 기존 코드 유지 (매핑 초기화, Stage_Data free) ...

  /* Producer Op 배열 초기화 */
  memset(srat->gp_producer_ops, 0, srat->gp_size * sizeof(Op*));
  memset(srat->gp_producer_unums, 0, srat->gp_size * sizeof(Counter));
  memset(srat->vec_producer_ops, 0, srat->vec_size * sizeof(Op*));
  memset(srat->vec_producer_unums, 0, srat->vec_size * sizeof(Counter));
}
```

---

## 3. `shadow_rat_snapshot()` 확장 — Producer Op 복사

TEA trigger 시 Main thread의 `map_data->reg_map[]`에서 각 register의 마지막 writer op을 복사:

```c
#include "map.h"  // Map_Data, map_data 접근

void shadow_rat_snapshot(uns proc_id) {
  // === 기존: arch→phys 매핑 복사 (유지) ===
  extern struct reg_file** reg_file;

  // GP register mappings (기존 코드 유지)
  if (reg_file && reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]) {
    struct reg_table* arch_table =
      reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];
    if (arch_table && arch_table->entries) {
      for (uns i = 0; i < arch_table->size && i < srat->gp_size; i++) {
        srat->gp_mappings[i] = arch_table->entries[i].child_reg_id;
      }
    }
  }

  // VEC register mappings (기존 코드 유지)
  if (reg_file && reg_file[REG_FILE_REG_TYPE_VECTOR]) {
    struct reg_table* arch_table =
      reg_file[REG_FILE_REG_TYPE_VECTOR]->reg_table[REG_TABLE_TYPE_ARCHITECTURAL];
    if (arch_table && arch_table->entries) {
      for (uns i = 0; i < arch_table->size && i < srat->vec_size; i++) {
        srat->vec_mappings[i] = arch_table->entries[i].child_reg_id;
      }
    }
  }

  // === 신규: Producer Op 복사 ===
  // map_data는 전역 변수 (map.h:69). trigger_tea_thread()는 bp_predict_op() 내부
  // (bp.c:878)에서 호출되며, 이 시점은 cmp_model.c의 per-cycle 루프에서
  // set_map_data()가 호출된 이후이므로 map_data는 유효.

  for (uns i = 0; i < NUM_REG_IDS; i++) {
    int reg_type = get_reg_type_for_rename(i);
    if (reg_type < 0) continue;

    // Main thread의 on-path 매핑 읽기
    uns ind = i << 1 | map_data->map_flags[i];
    Map_Entry* entry = &map_data->reg_map[ind];

    if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
      srat->gp_producer_ops[i] = entry->op;
      srat->gp_producer_unums[i] = entry->unique_num;
    } else if (reg_type == REG_FILE_REG_TYPE_VECTOR) {
      int vec_idx = i - REG_ZMM0;
      if (vec_idx >= 0 && vec_idx < (int)srat->vec_size) {
        srat->vec_producer_ops[vec_idx] = entry->op;
        srat->vec_producer_unums[vec_idx] = entry->unique_num;
      }
    }
  }

  srat->is_valid = TRUE;
}
```

### 3.1 두 데이터 소스의 일관성

`shadow_rat_snapshot()`은 **두 가지 다른 소스**에서 서로 다른 정보를 복사한다:

| 데이터 | 소스 | 용도 |
|--------|------|------|
| phys 매핑 (기존) | `reg_file[type]->reg_table[ARCH]->entries[i].child_reg_id` | TEA op의 src/dst physical register 결정 |
| producer Op (신규) | `map_data->reg_map[i << 1 \| map_flags[i]].op` | TEA op의 dependency wakeup 등록 |

이 두 소스는 파이프라인 내 **map-rename 갭**에 있는 ops 때문에 이론적으로 불일치할 수 있다:
- `map_data->reg_map[]`은 map stage에서 갱신 (producer Op 기록)
- `reg_file->reg_table[ARCH]`은 rename stage에서 갱신 (phys reg 할당)
- map은 되었지만 아직 rename되지 않은 op이 있으면, `reg_map`은 새 producer를 가리키지만
  `reg_table`은 아직 이전 phys reg을 가리킴

**이것은 안전하다**: `tea_rename_op()`에서 producer의 `op_pool_valid && unique_num` 검증을 수행하므로,
stale producer는 무시되고 즉시 ready로 처리된다 (Section 5.3 참조).
phys 매핑과 producer가 잠시 불일치하더라도 기능적 정확성에 영향 없음.

### 3.2 `map_data` 접근 유효성 검증

`shadow_rat_snapshot()`은 `trigger_tea_thread()` → `tea_thread.c:141`에서 호출.
호출 경로:

```
cmp_model.c per-cycle 루프:
  set_map_data(&td->map_data)     ← map_data 세팅
  ...
  update_icache_stage()
    → icache_fill_line()
      → bp_predict_op()           ← bp.c
        → trigger_tea_thread()    ← tea_thread.c:99
          → shadow_rat_snapshot() ← 여기서 map_data 사용
```

`set_map_data()`는 `icache_stage` 업데이트 전에 호출되므로 `map_data`는 유효. ✅

### 3.3 다중 H2P에서의 Snapshot 타이밍

첫 번째 chain 활성화 시에만 snapshot 수행 (논문 IV-D와 일치).
이후 chain의 rename은 이전 chain이 갱신한 Shadow RAT의 producer를 참조.

```
Chain A trigger → shadow_rat_snapshot() (Main RAT + reg_map 복사)
Chain A rename  → Shadow RAT producer 갱신 (Chain A의 ops가 producer)
Chain B trigger → snapshot 안 함 (num_active_chains > 1)
Chain B rename  → Shadow RAT에서 Chain A의 ops를 producer로 참조 가능
```

---

## 4. `tea_rename_op()` 확장 — 의존성 설정 + Wakeup 등록

### 4.1 TEA 전용 Src Dependency 설정 함수 (신규)

```c
/* tea_add_src_dependency: add_src_from_map_entry()의 TEA 버전
 * 차이점: op_num < consumer op_num ASSERT 없음 (cross-thread dependency) */
static void tea_add_src_dependency(Op* op, Op* src_op,
                                    Counter src_unique_num, Dep_Type type) {
  uns src_num = op->oracle_info.num_srcs++;
  ASSERT(op->proc_id, src_num < MAX_DEPS);

  Src_Info* info = &op->oracle_info.src_info[src_num];
  info->type = type;
  info->op = src_op;
  info->op_num = src_op->op_num;
  info->unique_num = src_unique_num;

  /* not-rdy bit 세팅 */
  set_not_rdy_bit(op, src_num);
}
```

**`add_src_from_map_entry()`와의 차이**:
- `ASSERTM(map_entry->op_num < op->op_num)` 제거 — Main op_num << TEA op_num이므로
  항상 통과하지만, 의미적으로 cross-thread 비교이므로 불필요.
- `map_data->proc_id` 대신 `op->proc_id` 사용 — TEA rename 시점에서 map_data가
  올바른 proc의 것인지 보장되지만 명시적으로 op->proc_id 사용이 안전.

### 4.2 `tea_rename_op()` 전체 구현

```c
void tea_rename_op(uns proc_id, Op* op) {
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->thread_id == 1);

  Tea_Rename_Stage* rename = tea_rename_stages[proc_id];
  Shadow_RAT* srat = rename->shadow_rat;
  ASSERT(proc_id, srat->is_valid);

  Inst_Info* inst_info = op->inst_info;
  Table_Info* table_info = op->table_info;

  /* 의존성 카운터 초기화 */
  op->oracle_info.num_srcs = 0;

  /* === Source Registers — Shadow RAT 조회 + 의존성 설정 === */
  for (uns i = 0; i < table_info->num_src_regs; i++) {
    int arch_reg_id = inst_info->srcs[i].id;
    int reg_type = get_reg_type_for_rename(arch_reg_id);
    if (reg_type < 0) continue;

    /* (1) 물리 레지스터 매핑 읽기 (기존 로직) */
    int phys_reg_id = shadow_rat_read_mapping(srat, arch_reg_id, reg_type);
    op->src_reg_id[i][REG_TABLE_TYPE_ARCHITECTURAL] = arch_reg_id;
    op->src_reg_id[i][REG_TABLE_TYPE_PHYSICAL] = phys_reg_id;

    /* (2) Producer Op 조회 */
    Op* producer_op = NULL;
    Counter producer_unum = 0;

    if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
      producer_op = srat->gp_producer_ops[arch_reg_id];
      producer_unum = srat->gp_producer_unums[arch_reg_id];
    } else {
      int vec_idx = arch_reg_id - REG_ZMM0;
      producer_op = srat->vec_producer_ops[vec_idx];
      producer_unum = srat->vec_producer_unums[vec_idx];
    }

    /* (3) Producer가 유효하고 아직 파이프라인에 있으면 의존성 등록 */
    if (producer_op && producer_op->op_pool_valid &&
        producer_op->unique_num == producer_unum) {
      tea_add_src_dependency(op, producer_op, producer_unum, REG_DATA_DEP);
    }
    /* else: producer가 이미 retire됨 → 즉시 사용 가능 (비트 미설정) */
  }

  /* === Destination Registers — TEA preg 할당 + Shadow RAT 갱신 === */
  for (uns i = 0; i < table_info->num_dest_regs; i++) {
    int arch_reg_id = inst_info->dests[i].id;
    int reg_type = get_reg_type_for_rename(arch_reg_id);
    if (reg_type < 0) continue;

    /* prev 매핑 저장 (기존 로직) */
    int prev_phys_reg_id = shadow_rat_read_mapping(srat, arch_reg_id, reg_type);
    op->prev_dst_reg_id[i][REG_TABLE_TYPE_PHYSICAL] = prev_phys_reg_id;

    /* TEA preg 할당 (기존 로직) */
    Tea_Preg_Free_List* tea_pool = (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE)
      ? srat->tea_gp_preg_pool : srat->tea_vec_preg_pool;
    int new_phys_reg_id = tea_preg_pool_alloc(tea_pool);
    ASSERT(proc_id, new_phys_reg_id >= 0);
    STAT_EVENT(proc_id, TEA_PREGS_ALLOCATED);

    op->dst_reg_id[i][REG_TABLE_TYPE_ARCHITECTURAL] = arch_reg_id;
    op->dst_reg_id[i][REG_TABLE_TYPE_PHYSICAL] = new_phys_reg_id;

    /* Shadow RAT 갱신 — phys 매핑 */
    shadow_rat_write_mapping(srat, arch_reg_id, new_phys_reg_id, reg_type);

    /* Shadow RAT 갱신 — producer op (신규) */
    if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
      srat->gp_producer_ops[arch_reg_id] = op;
      srat->gp_producer_unums[arch_reg_id] = op->unique_num;
    } else {
      int vec_idx = arch_reg_id - REG_ZMM0;
      srat->vec_producer_ops[vec_idx] = op;
      srat->vec_producer_unums[vec_idx] = op->unique_num;
    }
  }

  /* === Wakeup List 등록 === */
  /* cmp_wake를 wake_action으로 사용 (Main thread와 동일) */
  extern void cmp_wake(Op*, Op*, uns8);
  add_to_wake_up_lists(op, &op->oracle_info, cmp_wake);
}
```

### 4.3 `add_to_wake_up_lists()` 호환성

`add_to_wake_up_lists()` (`map.c:578`)의 핵심 체크:

```c
ASSERT(op->proc_id == map_data->proc_id);  // ✅ 동일 코어
// ...
ASSERTM(op->proc_id == src_op->proc_id, ...);  // ✅ 동일 코어
```

- Main op → TEA op wakeup: `src_op->proc_id == dep_op->proc_id` ✅
- TEA op → TEA op wakeup: 역시 동일 코어 ✅
- `add_src_from_map_entry()`의 `op_num < consumer op_num` ASSERT는
  `add_to_wake_up_lists()`에 **없음** → cross-thread 안전 ✅

**`map_data` 유효성**: `tea_rename_op()`은 `update_tea_rename_stage()` → `update_node_stage()` 내부
(`node_stage.c:475`)에서 호출. `cmp_model.c`에서 `set_map_data()`는 이전에 호출됨 → 유효 ✅

### 4.4 `map_data->free_list_head` — Wake_Up_Entry 할당

`add_to_wake_up_lists()`는 `map_data->free_list_head`에서 Wake_Up_Entry를 할당.
TEA ops의 wakeup 등록이 추가되면 더 많은 Wake_Up_Entry가 필요할 수 있음.

현재 Main thread에서 entry가 부족하면 `expand_wake_up_entries()` (`map.c:603-606`)가
자동 확장하므로 메모리 부족 문제는 없음. 단, TEA terminate 시 반환이 필요 (Section 7 참조).

---

## 5. 의존성 시나리오별 동작

### 5.1 TEA op → Main thread op 의존 (cross-thread)

```
Main: MOV R1, [addr]  (op_num=100, 아직 실행 중, op_pool_valid=TRUE)
TEA:  ADD R2, R1, R3  (op_num=0x8000000000000005)
```

- `shadow_rat_snapshot()` 시점에 `reg_map[R1]`의 producer = Main MOV op
- `tea_rename_op(ADD)`: `gp_producer_ops[R1]` = Main MOV op
  → `tea_add_src_dependency()` → `srcs_not_rdy_vector |= 0x1`
- `add_to_wake_up_lists()`: Main MOV op의 `wake_up_head`에 TEA ADD 등록
- Main MOV 실행 완료 → `exec_stage_dep_wakeup(MOV)` → `wake_up_ops(MOV, REG_DATA_DEP, ...)`
  → `cmp_wake(Main_MOV, TEA_ADD, 0)` → `clear_not_rdy_bit` → TEA ADD ready

### 5.2 TEA op → TEA op 의존 (intra-chain)

```
TEA chain: LOAD R1, [x]   (op_num=0x8000000000000005)
           ADD R2, R1, R3  (op_num=0x8000000000000006)
```

- TEA LOAD rename 시: dst R1 → `gp_producer_ops[R1]` = TEA LOAD op
- TEA ADD rename 시: src R1 → producer = TEA LOAD op
  → `tea_add_src_dependency()` → wakeup 등록
- TEA LOAD 실행 완료 → `exec_stage_dep_wakeup(LOAD)` → `cmp_wake(TEA_LOAD, TEA_ADD, 0)`
  → TEA ADD ready

### 5.3 Producer가 이미 retire됨

```
Main: MOV R1, #5  (이미 retire → op_pool_valid == FALSE)
TEA:  ADD R2, R1, R3
```

- `tea_rename_op(ADD)`: `producer_op->op_pool_valid == FALSE`
  → 의존성 등록 안 함 → `srcs_not_rdy_vector` 비트 미설정 → 즉시 ready ✅

### 5.4 다중 H2P — Chain A → Chain B 의존 (inter-chain)

```
Chain A: ADD R5, R1, R2   → gp_producer_ops[R5] = Chain_A_ADD
Chain B: MUL R6, R5, R3   → src R5의 producer = Chain_A_ADD → 의존성 등록
```

Shadow RAT는 하나를 공유하며 순차 갱신 (논문과 일치).
Chain B의 op은 Chain A의 op을 producer로 참조 → 정상 wakeup ✅

---

## 6. `exec_stage_dep_wakeup()` — TEA 호환 확인

`exec_stage.c:431-453`:
```c
static inline void exec_stage_dep_wakeup(Op* op) {
  Counter exec_cycle = cycle_count + abs(op->inst_info->latency);

  if (op->table_info->mem_type == NOT_MEM) {
    op->wake_cycle = exec_cycle;
    wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
    return;
  }

  if (op->table_info->mem_type == MEM_ST) {
    if (op->exec_count == 0) {
      op->wake_cycle = exec_cycle;
      wake_up_ops(op, MEM_ADDR_DEP, model->wake_hook);
      wake_up_ops(op, MEM_DATA_DEP, model->wake_hook);
    }
    return;
  }
  // loads: handled by memory system
}
```

이 함수는 **모든 op에 대해** 호출됨 (TEA/Main 구분 없음, `exec_stage.c:260`).
TEA op이 실행되면 `wake_up_ops()`가 해당 op의 `wake_up_head` 리스트를 순회하여
dependent ops를 wakeup → 올바른 동작 ✅

TEA memory ops (loads)의 wakeup은 dcache stage에서 처리:
- `dcache_stage.c`의 cache hit/miss 시점에 `wake_up_ops()` 호출
- TEA load가 cache miss → MSHR 진입 → 데이터 도착 시 wakeup → 정확한 latency 모델링

---

## 7. Wake_Up_Entry 정리 (TEA 종료 시)

### 7.1 문제 (원래 우려)

TEA ops가 `add_to_wake_up_lists()`를 호출하면:
- TEA op (consumer) → producer op의 `wake_up_head`에 Wake_Up_Entry 등록
- TEA op (producer) → dependent ops가 자신의 `wake_up_head`에 등록

TEA terminate 시 이 엔트리들이 반환되지 않으면 `map_data->active_wake_up_entries`가
계속 증가하여 메모리 누수 발생.

### 7.2 해결: `free_op()` → `free_wake_up_list()`가 이미 처리 ✅

**코드 검증 결과**: `free_op()` (`op_pool.c:177`)이 이미 `free_wake_up_list(op)`을 호출:

```c
// op_pool.c:177
void free_op(Op* op) {
  // ... 기타 정리 ...
  op->op_pool_next = op_pool_free_head;
  op_pool_free_head = op;
  free_wake_up_list(op);   // ← 자동으로 wake_up list 반환!
}
```

따라서:
- `flush_tea_ops_from_node_stage()` → `free_op(op)` → `free_wake_up_list(op)` ✅ 자동
- `node_retire_tea_ops()` → `free_op(op)` → `free_wake_up_list(op)` ✅ 자동

**결론**: `flush_tea_ops_from_node_stage()`와 `node_retire_tea_ops()`에
explicit wake_up 반환 코드를 추가할 필요가 **없다**. `node_stage.c` 변경 불필요.

### 7.3 Producer가 Main op인 경우

Main op의 `wake_up_head`에 등록된 TEA op용 Wake_Up_Entry:
- TEA op이 `free_op()`으로 반환되면 `op_pool_valid = FALSE`
- Main op 실행 시 `wake_up_ops()`에서 `dep_op->op_pool_valid` 체크 → 자동 스킵 ✅
- Wake_Up_Entry 자체는 Main op의 리스트에 남아있다가 Main op retire 시
  `free_wake_up_list()`로 반환됨 ✅

### 7.4 ~~`node_retire_tea_ops()` 수정~~ — 변경 불필요

~~정상 retire 경로에서도 동일한 wake_up list 반환 필요~~
→ `free_op()`이 이미 `free_wake_up_list()`를 호출하므로 **추가 코드 불필요**.

---

## 8. `reg_file_consume()` / `reg_file_issue()` — 변경 불필요

### 8.1 `reg_file_consume()` (`map_rename.c:1589-1593`)

```c
void reg_file_consume(Op *op) {
  if (op->thread_id == 1) {
    return;  // TEA ops는 별도 preg pool → Main reg_table 건드리지 않음
  }
  // ...
}
```

이 skip은 **올바른 동작**. TEA ops의 preg은 TEA preg pool에서 할당되며,
Main thread의 physical register table의 reference counting과 무관.
Wakeup은 `op->srcs_not_rdy_vector`와 Wake_Up_Entry로만 동작하므로
`reg_file_consume()` skip이 wakeup에 영향 없음.

### 8.2 `reg_file_issue()` (`map_rename.c:962-965`)

```c
Flag reg_renaming_scheme_realistic_issue(Op *op) {
  return TRUE;  // 항상 TRUE
}
```

현재 Scarab의 realistic renaming scheme에서 issue는 항상 허용됨.
TEA ops에 대해서도 별도 처리 불필요.

---

## 8-A. TEA 생존 시 flushed Main op 의존성 stall (향후 대비)

### 8-A.1 문제 시나리오

향후 TEA selective survival (Main flush 시 TEA가 종료되지 않고 생존)이 구현되면,
다음 시나리오에서 의존성 stall이 발생할 수 있다:

```
Cycle 100: TEA trigger for H2P (op_num=100)
           shadow_rat_snapshot: R1의 producer = Main op Y (op_num=150, 아직 in-flight)
Cycle 105: TEA op A renamed, R1 의존성 등록, srcs_not_rdy_vector |= bit
           Main op Y의 wake_up_head에 TEA op A 등록
Cycle 110: Main misprediction at op_num=120 → cmp_recover()
           → Main op Y (op_num=150 > 120) flushed → free_op(Y) → free_wake_up_list(Y)
           → recover_tea_on_flush(proc_id)
           → tea->target_h2p_op_num=100 < 120 → TEA 생존!
결과: TEA op A의 srcs_not_rdy_vector bit가 영원히 clear되지 않음 → RS에서 무한 대기
```

**영향**: ASSERT 위반 없음. 영구 deadlock 없음 (H2P 실행 시 모든 TEA ops 일괄 flush).
실질적으로 TEA ops가 RS 슬롯을 점유한 채 issue되지 않아 자원 낭비.

### 8-A.2 현재 상태

현재 `recover_tea_on_flush()` (`cmp_model.c:389-399`)는 무조건 TEA를 종료:

```c
void recover_tea_on_flush(uns proc_id) {
  if (!TEA_ENABLE || !tea_is_active(proc_id)) {
    return;
  }
  terminate_tea_thread(proc_id);  // 무조건 종료 → 이 문제 발생 안 함
  reset_tea_fetch_stage(proc_id);
  reset_tea_rename_stage(proc_id);
  reset_tea_preg_pool(proc_id);
}
```

따라서 **현재 구현에서는 이 문제가 발생하지 않음**. 향후 selective survival 구현 시 대비.

### 8-A.3 해결 방안 (향후 구현 시)

`recover_tea_on_flush()`에서 TEA가 생존하는 경우, 생존 TEA ops를 순회하여
flushed producer의 not-rdy bit를 강제 clear.

**타이밍 순서 근거**: `cmp_recover()` (`cmp_model.c:404-449`)에서:
1. `recover_node_stage()` (line 437) → Main ops flush, `free_op()` → `op_pool_valid = FALSE`
2. `recover_tea_on_flush()` (line 445) → 이 시점에서 flushed Main ops는 이미 `op_pool_valid == FALSE`

따라서 `op_pool_valid + unique_num` 체크만으로 충분:

```c
void recover_tea_on_flush(uns proc_id, Counter recovery_op_num) {
  // ... 기존 chain 종료 로직 ...

  // 생존 chain의 ops에 대해 flushed producer 의존성 정리
  if (tea->num_active_chains > 0) {
    Node_Stage* node = &cmp_model.node_stage[proc_id];
    for (Op* op = node->node_head; op; op = op->next_node) {
      if (op->thread_id != 1) continue;

      for (uns i = 0; i < op->oracle_info.num_srcs; i++) {
        if (!(op->srcs_not_rdy_vector & (0x1 << i))) continue;

        Src_Info* src = &op->oracle_info.src_info[i];
        if (!src->op->op_pool_valid ||
            src->op->unique_num != src->unique_num) {
          clear_not_rdy_bit(op, i);
        }
      }

      // Force-ready된 op를 ready list에 추가
      if (op->srcs_not_rdy_vector == 0x0 &&
          op->state == OS_IN_RS && !op->in_rdy_list) {
        op->next_rdy = node->rdy_head;
        node->rdy_head = op;
        op->in_rdy_list = TRUE;
      }
    }
  }
}
```

**정확성 보장**: TEA ops는 oracle 값으로 실행되므로, producer가 flush되어도
functional correctness에 영향 없음. Force-ready로 timing이 약간 달라지지만,
recovery 발생 시점에서 원래 producer의 timing은 이미 무의미.

---

## 8-B. H2P Queue flush 비교 연산자 수정 (참고)

이전 계획 파일(TEA_multi_h2p_plan.md) §4.5의 `h2p_op_num > recovery_op_num` 조건에서,
`recovery_op_num` 자체도 flush 대상이므로 `>=` 연산자로 수정이 필요:

```c
// 수정 전: h2p_op_num > recovery_op_num
// 수정 후: h2p_op_num >= recovery_op_num
```

이 수정은 TEA_reg_dependency와 직접 관련은 없으나, `recover_tea_on_flush()` 확장 시
함께 적용해야 할 사항이므로 여기에 참고로 기록.

---

## 9. 다중 H2P에서의 추가 고려사항

### 9.1 Per-chain op의 Dependent Op not-rdy bit 강제 Clear

다중 H2P에서 개별 chain 종료 시 (`terminate_tea_chain()`):
- 해당 chain의 ops만 `flush_tea_ops_by_chain_id()`로 flush
- `free_op()` → `free_wake_up_list()`가 Wake_Up_Entry **메모리 반환**은 자동 처리 ✅
- 그러나 **다른 chain의 ops가 이 chain의 op에 의존**하는 경우:
  - producer가 `free_op()`되면 `op_pool_valid = FALSE`
  - dependent op의 `srcs_not_rdy_vector` not-rdy bit는 **영원히 clear되지 않음**
  - `free_wake_up_list()`는 producer의 wake_up list를 반환하지만, dependent op의
    not-rdy bit를 clear하는 것은 **다른 작업**이며 `free_wake_up_list()`가 수행하지 않음
  - **해결**: `flush_tea_ops_by_chain_id()`에서 `free_op()` 호출 **전에**
    flush된 op의 wake_up list를 순회하며 dependent ops의 not-rdy bit를 강제 clear

**참고**: §7.2에서 "explicit wake_up 반환 불필요"라고 한 것은 **메모리 반환** 측면.
§9.1의 코드는 **dependent op의 not-rdy bit clear** 측면이므로 역할이 다르며 여전히 필요.

```c
// flush_tea_ops_by_chain_id() 내부:
// op을 free하기 전에 — 이 op에 의존하는 다른 TEA ops를 wakeup
Wake_Up_Entry* wake = op->wake_up_head;
while (wake) {
  Op* dep_op = wake->op;
  if (dep_op->op_pool_valid && dep_op->unique_num == wake->unique_num) {
    // dependent op이 아직 유효 → not-rdy bit 강제 clear
    clear_not_rdy_bit(dep_op, wake->rdy_bit);
    // rdy_cycle은 현재 cycle로 설정 (즉시 ready)
    if (dep_op->state == OS_IN_RS &&
        dep_op->srcs_not_rdy_vector == 0x0 &&
        !dep_op->in_rdy_list) {
      dep_op->next_rdy = node->rdy_head;
      node->rdy_head = dep_op;
      dep_op->in_rdy_list = TRUE;
    }
  }
  Wake_Up_Entry* next = wake->next;
  wake = next;
}
// 그 후 wake_up list 반환 + free_op()
```

### 9.2 단일 H2P에서는 이 문제 없음

`terminate_tea_thread()`는 **모든** TEA ops를 일괄 flush.
dependent TEA op도 같이 flush되므로 not-rdy bit stall 불가.

### 9.3 Shadow RAT Producer 유효성

다중 H2P에서 Chain A가 종료되면 Shadow RAT의 `gp_producer_ops[]`에
Chain A의 op 포인터가 남을 수 있음 (dangling).

- Chain B rename 시 이 포인터를 참조하면 `op_pool_valid == FALSE` →
  의존성 등록 안 함 → 즉시 ready ✅
- 실제로는 producer가 retire되었으므로 값이 이미 register에 있음 → 올바른 동작

---

## 10. 수정 파일 요약

| 파일 | 변경 내용 | 규모 |
|------|----------|------|
| `src/tea/tea_rename.h` | Shadow_RAT에 `producer_ops`, `producer_unums` 필드 추가 | 4줄 추가 |
| `src/tea/tea_rename.c` | `init_shadow_rat()`: producer 배열 할당 | 4줄 추가 |
| | `reset_tea_rename_stage()`: producer 배열 초기화 | 4줄 추가 |
| | `shadow_rat_snapshot()`: `map_data->reg_map[]`에서 producer 복사 | ~20줄 추가 |
| | `tea_rename_op()`: 의존성 설정 + `add_to_wake_up_lists()` 호출 | ~40줄 수정 |
| | 새 함수 `tea_add_src_dependency()` | ~15줄 추가 |
| | `#include "map.h"` 추가 | 1줄 추가 |

**변경 불필요**:
- `src/node_stage.c` — `free_op()` → `free_wake_up_list()` 자동 호출로 explicit 반환 불필요
- `map.h` / `map.c` — `add_to_wake_up_lists()`, `wake_up_ops()` 인터페이스 변경 없음
- `cmp_model.c` — `cmp_wake()` 변경 없음
- `exec_stage.c` — `exec_stage_dep_wakeup()` 변경 없음 (모든 op에 대해 동작)
- `map_rename.c` — `reg_file_consume()` TEA skip 유지
- `op.h` — 기존 `srcs_not_rdy_vector`, `wake_up_head/tail` 활용

---

## 11. 구현 순서

```
단계 1: Shadow RAT producer 필드 추가
  ├─ tea_rename.h: Shadow_RAT에 4개 필드 추가
  ├─ tea_rename.c: init_shadow_rat()에 calloc 추가
  └─ tea_rename.c: reset_tea_rename_stage()에 memset 추가

단계 2: shadow_rat_snapshot() 확장
  ├─ tea_rename.c에 #include "map.h" 추가
  └─ map_data->reg_map[] 순회하며 producer 복사

단계 3: tea_rename_op() 수정
  ├─ tea_add_src_dependency() 구현
  ├─ src loop에 producer 조회 + 의존성 설정
  ├─ dst loop에 producer 기록
  └─ add_to_wake_up_lists() 호출

(단계 4: Wake_Up_Entry 정리 — 불필요)
  free_op() → free_wake_up_list() 자동 호출로 처리됨
  node_stage.c 변경 없음

단계 4: 빌드 + 테스트
```

---

## 12. 검증

```bash
cd ~/scarab-infra
./sci --build-scarab tea_dbg
./sci --sim tea_dbg
/analyze_tea_sim
```

**확인 지표**:
- TEA ops의 실행 순서가 dependency chain 순서와 일치
- `TEA_OPS_EXECUTED`의 cycle 분포가 cache latency를 반영
  (이전: 모든 ops가 동시 실행 → 이후: LOAD miss 이후에 dependent ops 실행)
- 시뮬레이션 정확도 향상: Early Flush 타이밍이 실제 precomputation latency를 반영

**디버그 방법**:
- `DEBUG_TEA` 플래그로 TEA op별 `srcs_not_rdy_vector`, `rdy_cycle` 출력
- `wake_up_count > 0`인 TEA ops 확인 (의존성이 올바르게 설정됨)
- 특정 dependency chain의 ops가 올바른 순서로 실행되는지 sim.log에서 확인

---

## 13. Poison Bit — 구현하지 않는 이유 (참고)

논문 Section IV-D:
> "If a main thread instruction that is part of the H2P branch dependence chains reads from
> a poisoned register, the TEA thread has an incorrect dependence chain and precomputation
> is preemptively terminated."

Poison bit는 Block Cache의 dependency chain이 실제 runtime과 다를 때
(예: control flow 변화로 chain이 stale) 이를 감지하는 메커니즘.

**시뮬레이터에서 불필요한 이유**:
1. `dependency_chain_caches`에 저장된 chain은 BW Walk 시점의 실제 실행 기반
2. Oracle 정보(`oracle_info`)를 사용하여 항상 올바른 값으로 실행
3. Chain이 stale이더라도 시뮬레이터에서는 functional correctness에 영향 없음
   (타이밍만 달라질 수 있으나 이는 `periodically_reset_caches()`로 관리)
4. 구현 복잡도 대비 효과가 미미 — Poison bit 감지로 조기 종료해도
   성능 차이는 무시할 수준 (논문 V-E: 0.7% 미만의 incorrect precomputation)
