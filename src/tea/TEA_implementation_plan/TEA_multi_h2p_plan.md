# TEA 다중 H2P+DC 동시 처리 구현 계획

**최종 갱신**: 2026-03-11
**현재 상태**: [`TEA_multi_h2p_status.md`](../TEA_implementation_status/TEA_multi_h2p_status.md)
**마스터 문서**: [`TEA_implementation_plan.md`](../TEA_implementation_plan.md) Section 3.4, 8
**관련 문서**:
- [`TEA_op_manage_plan.md`](TEA_op_manage_plan.md) — `h2p_chain_id`, per-chain op count, selective flush
- [`TEA_shadow_ftq_plan.md`](TEA_shadow_ftq_plan.md) — 다중 chain fetch, chain 전환 로직
- [`TEA_early_flush_plan.md`](TEA_early_flush_plan.md) — EF-1~EF-4: 다중 H2P Early Flush
- [`TEA_reg_dependency_plan.md`](TEA_reg_dependency_plan.md) — Shadow RAT producer 추적, wakeup

---

## 1. 개요

### 1.1 목표

논문의 **"순차 fetch, 중첩 실행"** 모델 구현:

```
H2P #0 trigger → fetch chain #0 → rename → [backend 실행 중...]
                                                  ↓
H2P #1 trigger → fetch chain #1 → rename → [backend 실행 중...]
                                                  ↓
H2P #2 trigger → fetch chain #2 → rename → [backend 실행 중...]
```

- 최대 `MAX_TEA_CHAINS`(기본값 4)개의 H2P+DC가 동시 활성
- 각 chain은 독립적인 상태(FETCHING/EXECUTING/DONE)를 가짐
- 모든 chain의 ops가 동일한 OoO backend(RS, FU, Node Table)에서 동시 실행
- 각 chain의 H2P branch 실행 완료 시 **개별적으로** Early Flush 또는 완료 처리
- Shadow RAT와 TEA preg pool은 모든 chain이 **공유** (논문과 동일)

### 1.2 전제조건

| 작업 | 상태 | 의존 이유 |
|------|------|----------|
| 작업 A (BW Walk Trigger) | ❌ | Dep Chain Cache가 비어야 chain 자체가 존재하지 않음 |
| 작업 G (TEA 의존성 Wakeup) | ❌ | 타이밍이 무의미한 상태에서 다중 chain의 상호작용 검증 불가 |
| 단일 H2P 검증 (TEA_TRIGGERS > 0) | ❌ | 기본 동작이 올바른지 먼저 확인 |

### 1.3 작업 분해

| Phase | 내용 | 의존 |
|-------|------|------|
| F.1 | 데이터 구조 변경 (`op.h`, `tea_thread.h`) | — |
| F.2 | trigger/terminate per-chain 로직 (`tea_thread.c`) | F.1 |
| F.3 | fetch stage chain 전환 (`tea_fetch_stage.c`) | F.1, F.2 |
| F.4 | exec_stage 다중 매칭 (`exec_stage.c`) | F.1 |
| F.5 | selective flush + store buffer (`node_stage.c`, `tea_store_buffer.c`) | F.1 |
| F.6 | `recover_tea_on_flush()` 시그니처 변경 (`cmp_model.c`) | F.1, F.4, F.5 |

---

## 2. Phase F.1: 데이터 구조 변경

### 2.1 `Op` 구조체 — `h2p_chain_id` 추가

**파일**: `src/op.h`

```c
// 추가 (thread_id 필드 근처, 약 line 135)
uns8 h2p_chain_id;  // 0 = main op, 1~MAX_TEA_CHAINS = TEA chain ID
```

- 0: Main thread op (기본값, `alloc_op()`에서 0으로 초기화)
- 1~N: TEA chain ID (`tea_create_op_from_cache()`에서 할당)
- downstream에서 `thread_id==1 && h2p_chain_id==X`로 특정 chain의 ops 식별

### 2.2 `Tea_H2P_Chain` 구조체 (신규)

**파일**: `src/tea/tea_thread.h`

```c
typedef enum Tea_Chain_State_enum {
  CHAIN_INACTIVE,    // 슬롯 비어있음
  CHAIN_FETCHING,    // fetch 진행 중
  CHAIN_EXECUTING,   // fetch 완료, backend에서 실행 중
} Tea_Chain_State;

typedef struct Tea_H2P_Chain_struct {
  Tea_Chain_State state;

  /* H2P branch 정보 */
  Addr target_h2p_pc;
  Counter target_h2p_op_num;      // Main thread op_num (older/younger 비교용)
  Op* main_h2p_op;                // Main thread H2P branch Op 포인터
  Counter saved_unique_num;       // main_h2p_op의 unique_num (유효성 검증)
  Op_Info h2p_oracle_info;        // trigger 시점의 oracle 사본
  Recovery_Info h2p_recovery_info;// trigger 시점의 recovery 사본

  /* per-chain 카운터 */
  uns tea_op_count;               // 현재 파이프라인 내 이 chain의 ops 수
  Counter tea_ops_fetched;        // 이 chain에서 fetch된 총 ops 수
} Tea_H2P_Chain;
```

### 2.3 `Tea_Thread` 구조체 변경

**파일**: `src/tea/tea_thread.h`

기존 단일 H2P 필드들을 `Tea_H2P_Chain` 배열로 교체:

```c
#define MAX_TEA_CHAINS 4  // 파라미터화: TEA_MAX_CHAINS

typedef struct Tea_Thread_struct {
  uns8 proc_id;

  /* === 기존 단일 필드 → 삭제/이동 === */
  Tea_State state;                 // 유지: num_active_chains==0일 때 TEA_IDLE 설정용
                                   // tea_is_active()는 num_active_chains>0으로 판별
  // Addr target_h2p_pc;           // 삭제 (per-chain으로 이동)
  // Counter target_h2p_op_num;    // 삭제
  // Op* main_h2p_op;              // 삭제
  // Op_Info h2p_oracle_info;      // 삭제
  // Recovery_Info h2p_recovery_info; // 삭제
  // Addr current_block_pc;        // 삭제
  // int current_chain_idx;        // 삭제
  // uns tea_op_count;             // 삭제 (per-chain으로 이동)
  // Counter tea_ops_fetched;      // 삭제 (per-chain으로 이동)

  /* === 다중 H2P chain 추적 === */
  Tea_H2P_Chain chains[MAX_TEA_CHAINS];
  uns num_active_chains;          // 현재 활성 chain 수 (INACTIVE가 아닌 것)
  uns current_fetch_chain;        // 현재 fetch 중인 chain의 인덱스

  /* === 공유 자원 (변경 없음) === */
  Counter tea_op_counter;         // 전체 TEA ops 순차 카운터 (0x8000... 시작)
  Counter tea_start_cycle;

  /* 통계 */
  Counter stat_tea_triggers;
  Counter stat_tea_early_flushes;
  Counter stat_tea_ops_executed;
} Tea_Thread;
```

### 2.4 `Tea_Fetch_Stage` 구조체 변경

**파일**: `src/tea/tea_fetch_stage.h`

```c
typedef struct Tea_Fetch_Stage_struct {
  uns8 proc_id;
  Stage_Data sd;

  /* 현재 fetch 중인 chain 상태 */
  Dependency_Chain_Cache_Entry* active_chain;  // 유지: 현재 fetch 대상
  int current_chain_idx;                       // 유지: 현재 chain 내 위치
  int total_chain_length;                      // 유지: 현재 chain 길이
  Flag fetch_complete;                         // 유지: 현재 chain의 fetch 완료 여부
  uns current_chain_id;                        // 추가: chains[] 인덱스 (0-based)

  Counter ops_fetched_this_cycle;
} Tea_Fetch_Stage;
```

**설계 결정**: fetch stage는 한 시점에 하나의 chain만 fetch하므로
per-chain 배열이 아닌 `current_chain_id`로 현재 대상을 가리킨다.
chain 전환은 현재 chain fetch 완료 시 `find_next_fetching_chain()`으로 수행.

### 2.5 `Tea_Store_Buffer_Entry` — chain ID 추가

**파일**: `src/tea/tea_store_buffer.h`

```c
typedef struct Tea_Store_Buffer_Entry_struct {
  Flag valid;
  Addr addr;
  uns size;
  uns8 data[64];
  uns8 h2p_chain_id;  // 추가: 어느 chain의 store인지
} Tea_Store_Buffer_Entry;
```

### 2.6 파라미터

**파일**: `src/core.param.def`

```c
DEF_PARAM(tea_max_chains, TEA_MAX_CHAINS, uns, uns, 4, )
```

---

## 3. Phase F.2: trigger/terminate per-chain 로직

### 3.1 `trigger_tea_thread()` — 빈 슬롯에 chain 추가

**파일**: `src/tea/tea_thread.c`

기존 `state != TEA_IDLE` 게이트(line 110-114) → 빈 슬롯 검색으로 교체:

```c
void trigger_tea_thread(uns proc_id, Addr h2p_pc, Counter h2p_op_num, Op* h2p_op) {
  Tea_Thread* tea = tea_threads[proc_id];
  STAT_EVENT(proc_id, TEA_TRIGGER_ATTEMPTS);

  // 1. 빈 chain 슬롯 찾기
  int slot = -1;
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    if (tea->chains[i].state == CHAIN_INACTIVE) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    STAT_EVENT(proc_id, TEA_TRIGGER_SKIP_FULL);
    return;
  }

  // 2. Dependency chain 존재 확인
  Dependency_Chain_Cache_Entry* chain = get_dependency_chain(proc_id, h2p_pc);
  if (!chain || !chain->is_valid || chain->chain_length == 0) {
    STAT_EVENT(proc_id, TEA_TRIGGER_SKIP_NO_CHAIN);
    return;
  }

  // 3. 슬롯에 H2P 정보 저장
  Tea_H2P_Chain* c = &tea->chains[slot];
  c->state = CHAIN_FETCHING;
  c->target_h2p_pc = h2p_pc;
  c->target_h2p_op_num = h2p_op_num;
  c->main_h2p_op = h2p_op;
  c->saved_unique_num = h2p_op->unique_num;
  c->h2p_oracle_info = h2p_op->oracle_info;
  c->h2p_recovery_info = h2p_op->recovery_info;
  c->tea_op_count = 0;
  c->tea_ops_fetched = 0;
  tea->num_active_chains++;

  // 4. 첫 번째 활성 chain이면 Shadow RAT snapshot + fetch 시작
  if (tea->num_active_chains == 1) {
    shadow_rat_snapshot(proc_id);
    reset_tea_fetch_stage(proc_id);
    tea->current_fetch_chain = slot;
    // fetch stage에 chain 정보 설정
    setup_fetch_for_chain(proc_id, slot, chain);
  }
  // 이미 다른 chain이 fetch 중이면, 현재 chain의 CHAIN_FETCHING 상태로 대기.
  // update_tea_fetch_stage()에서 현재 chain fetch 완료 시 자동으로 이 chain 발견.

  STAT_EVENT(proc_id, TEA_TRIGGERS);
}
```

**Shadow RAT snapshot 정책**: 첫 번째 chain 활성화 시 1회만 수행.
이후 chain들은 이전 chain의 rename 결과가 반영된 Shadow RAT를 그대로 사용.
이는 논문의 "순차 fetch, 순차 rename" 모델과 동일.

### 3.2 `terminate_tea_chain()` — 개별 chain 종료 (신규)

**파일**: `src/tea/tea_thread.c`

```c
void terminate_tea_chain(uns proc_id, int chain_id) {
  Tea_Thread* tea = tea_threads[proc_id];
  Tea_H2P_Chain* c = &tea->chains[chain_id];
  uns8 h2p_chain_id = chain_id + 1;  // 1-based

  ASSERT(proc_id, c->state != CHAIN_INACTIVE);

  // 1. fetch 중인 chain이면 fetch stage에서 해당 chain ops 제거
  if (c->state == CHAIN_FETCHING &&
      tea->current_fetch_chain == chain_id) {
    recover_tea_fetch_stage_by_chain(proc_id, h2p_chain_id);
  }

  // 2. rename stage에서 해당 chain ops 제거
  recover_tea_rename_stage_by_chain(proc_id, h2p_chain_id);

  // 3. node stage (exec/dcache/RS/node table)에서 해당 chain ops flush
  flush_tea_ops_by_chain_id(proc_id, h2p_chain_id);

  // 4. store buffer에서 해당 chain 엔트리 무효화
  tea_store_buffer_clear_by_chain_id(proc_id, h2p_chain_id);

  // 5. chain 상태 리셋
  c->state = CHAIN_INACTIVE;
  c->target_h2p_pc = 0;
  c->target_h2p_op_num = 0;
  c->main_h2p_op = NULL;
  c->tea_op_count = 0;
  c->tea_ops_fetched = 0;
  tea->num_active_chains--;
  STAT_EVENT(proc_id, TEA_CHAIN_TERMINATED);

  // 6. 모든 chain이 종료되면 전체 정리
  if (tea->num_active_chains == 0) {
    reset_tea_preg_pool(proc_id);
    reset_tea_store_buffer(proc_id);
    recover_tea_rename_stage(proc_id);  // Shadow RAT 무효화
    tea->state = TEA_IDLE;
  }
}
```

**preg 관리 정책**: per-chain 종료 시에는 해당 chain의 preg을 개별 반환하지 **않음**.
모든 chain이 종료된 후에만 `reset_tea_preg_pool()`로 일괄 반환.
이유: chain 간 Shadow RAT 공유로 인해 chain A가 할당한 preg을
chain B가 src로 참조할 수 있어, 개별 반환 시 dangling reference 위험.
(상세: `TEA_op_manage_plan.md` Section 5)

### 3.3 `terminate_tea_thread()` — 전체 종료 (기존 수정)

```c
void terminate_tea_thread(uns proc_id) {
  Tea_Thread* tea = tea_threads[proc_id];

  // 모든 활성 chain 종료
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    if (tea->chains[i].state != CHAIN_INACTIVE) {
      tea->chains[i].state = CHAIN_INACTIVE;
      tea->chains[i].main_h2p_op = NULL;
    }
  }
  tea->num_active_chains = 0;

  // 기존과 동일한 전체 flush
  recover_tea_fetch_stage(proc_id);
  recover_tea_rename_stage(proc_id);
  flush_tea_ops_from_node_stage(proc_id);
  reset_tea_preg_pool(proc_id);
  reset_tea_store_buffer(proc_id);
}
```

### 3.4 `update_tea_thread()` — per-chain 상태 관리

```c
void update_tea_thread(uns proc_id) {
  Tea_Thread* tea = tea_threads[proc_id];
  if (tea->num_active_chains == 0) return;

  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    Tea_H2P_Chain* c = &tea->chains[i];

    switch (c->state) {
      case CHAIN_FETCHING:
        // fetch 완료 확인은 fetch stage의 chain 전환 시 처리
        break;

      case CHAIN_EXECUTING:
        // 이 chain의 모든 ops가 완료되면 종료
        if (c->tea_op_count == 0 && c->tea_ops_fetched > 0) {
          terminate_tea_chain(proc_id, i);
        }
        break;

      case CHAIN_INACTIVE:
      default:
        break;
    }
  }
}
```

### 3.5 `tea_is_active()` — 다중 chain 반영

```c
Flag tea_is_active(uns proc_id) {
  return tea_threads[proc_id]->num_active_chains > 0;
}
```

> **주의: `tea_is_active()` 호출부 6곳 전수 재확인 필요**
>
> 의미는 "inactive → 파이프라인 업데이트 skip"으로 동일하게 유지되지만, 전환 시
> 각 호출 site에서 `num_active_chains > 0` 의미가 기존 `state != TEA_IDLE` 의미와
> 동치인지 반드시 확인할 것.
>
> | 파일 | 라인 | 호출 컨텍스트 |
> |------|------|---------------|
> | `src/cmp_model.c` | 285 | `update_tea_*()` 진입 gate |
> | `src/cmp_model.c` | 390 | `recover_tea_on_flush()` early return |
> | `src/node_stage.c` | 543 | dispatch 시 TEA ops 처리 gate |
> | `src/node_stage.c` | 557 | 동일 context |
> | `src/node_issue_queue.cc` | 397 | 2-pass scheduling TEA skip |
> | `src/exec_stage.c` | 572 | `exec_stage_bp_resolve()` 내 TEA gate |
>
> 특히 `recover_tea_on_flush()` 호출부(`cmp_model.c:390`)는 다중 H2P 변환 후 생존
> chain이 있으면 함수 내부에서 `num_active_chains > 0`이 TRUE인 상태로 진입해야
> 하므로 **early return 제거** 또는 `num_active_chains == 0`으로 변경 필요
> (`TEA_early_flush_plan.md §3.6` 참조).

### 3.6 `tea_op_completed()` — op 상태 전환만 (방안 A)

> **중요**: `tea_op_count` 감소는 이 함수가 아닌 `node_retire_tea_ops()`에서 수행.
> 이유: 0-latency op의 use-after-free 방지 (`TEA_op_manage_plan.md §4.1` 참조).
> 감소 구현은 `TEA_op_manage_plan.md §4.3` 참조.

```c
void tea_op_completed(uns proc_id, Op* op) {
  /* op->state = OS_DONE 설정 필수: 이것이 없으면 node_retire_tea_ops()가
   * TEA op을 retire하지 못해 ROB 누적 → deadlock.
   * 카운터 감소는 node_retire_tea_ops()에서 담당 (현재 단일 H2P와 동일). */
  op->state = OS_DONE;
  STAT_EVENT(proc_id, TEA_OPS_EXECUTED);
}
```

### 3.7 헬퍼 함수

```c
/* 다음 CHAIN_FETCHING 상태의 chain 인덱스 반환. 없으면 -1. */
static int find_next_fetching_chain(uns proc_id) {
  Tea_Thread* tea = tea_threads[proc_id];
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    int next = (tea->current_fetch_chain + 1 + i) % MAX_TEA_CHAINS;
    if (tea->chains[next].state == CHAIN_FETCHING) {
      return next;
    }
  }
  return -1;
}

/* fetch stage를 특정 chain의 dep chain으로 설정 */
static void setup_fetch_for_chain(uns proc_id, int chain_id,
                                   Dependency_Chain_Cache_Entry* dep_chain) {
  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  tf->active_chain = dep_chain;
  tf->current_chain_idx = 0;
  tf->total_chain_length = dep_chain->chain_length;
  tf->fetch_complete = FALSE;
  tf->current_chain_id = chain_id;
}
```

---

## 4. Phase F.3: fetch stage chain 전환

### 4.1 `update_tea_fetch_stage()` — chain 전환 로직 추가

**파일**: `src/tea/tea_fetch_stage.c`

기존 `update_tea_fetch_stage()`의 fetch 완료 감지(line 178-182) 이후에
chain 전환 로직 추가:

```c
void update_tea_fetch_stage(uns proc_id) {
  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  Tea_Thread* tea = tea_threads[proc_id];
  if (tea->num_active_chains == 0) return;

  tf->ops_fetched_this_cycle = 0;

  // 현재 chain의 fetch가 완료되었으면 chain 전환
  if (tf->fetch_complete) {
    // 현재 chain → CHAIN_EXECUTING
    tea->chains[tf->current_chain_id].state = CHAIN_EXECUTING;

    // 다음 CHAIN_FETCHING chain 탐색
    int next = find_next_fetching_chain(proc_id);
    if (next >= 0) {
      tea->current_fetch_chain = next;
      Dependency_Chain_Cache_Entry* dep_chain =
        get_dependency_chain(proc_id, tea->chains[next].target_h2p_pc);
      if (dep_chain && dep_chain->is_valid) {
        setup_fetch_for_chain(proc_id, next, dep_chain);
      } else {
        // dep chain이 사라짐 (reset됨) → 해당 chain 종료
        terminate_tea_chain(proc_id, next);
      }
    }
    // 다음 FETCHING chain이 없으면 fetch stage는 idle
    return;
  }

  // 기존 fetch 로직 (변경 없음)
  while (tf->ops_fetched_this_cycle < TEA_FETCH_WIDTH &&
         tf->current_chain_idx < tf->total_chain_length) {
    Op* cached_op = &tf->active_chain->chain[tf->current_chain_idx];
    Flag is_h2p_branch = (tf->current_chain_idx == tf->total_chain_length - 1);

    Op* tea_op = tea_create_op_from_cache(proc_id, cached_op, is_h2p_branch);
    if (!tea_op) break;

    // === 추가: chain ID 부여 ===
    tea_op->h2p_chain_id = tf->current_chain_id + 1;  // 1-based

    tf->sd.ops[tf->sd.op_count++] = tea_op;
    tf->current_chain_idx++;
    tf->ops_fetched_this_cycle++;
    tea->chains[tf->current_chain_id].tea_ops_fetched++;
    STAT_EVENT(proc_id, TEA_OPS_FETCHED);
  }

  if (tf->current_chain_idx >= tf->total_chain_length) {
    tf->fetch_complete = TRUE;
  }
}
```

### 4.2 `tea_create_op_from_cache()` — per-chain oracle/recovery

기존 H2P branch의 oracle/recovery를 `tea->h2p_oracle_info` 단일 값에서 복사하던 것을
해당 chain의 `Tea_H2P_Chain`에서 복사하도록 변경:

```c
Op* tea_create_op_from_cache(uns proc_id, Op* cached_op, Flag is_h2p_branch) {
  Tea_Thread* tea = tea_threads[proc_id];
  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  Tea_H2P_Chain* c = &tea->chains[tf->current_chain_id];  // 현재 fetch 중인 chain

  Op* tea_op = alloc_op(proc_id);
  if (!tea_op) return NULL;

  tea_op->inst_info = cached_op->inst_info;
  tea_op->table_info = cached_op->table_info;
  tea_op->proc_id = proc_id;
  tea_op->thread_id = 1;
  tea_op->fetch_cycle = cycle_count;
  tea_op->off_path = FALSE;
  tea_op->state = OS_FETCHED;
  tea_op->op_num = tea->tea_op_counter++;  // 공유 카운터 (chain 무관)

  if (is_h2p_branch) {
    tea_op->oracle_info = c->h2p_oracle_info;       // ← chain별 oracle
    tea_op->recovery_info = c->h2p_recovery_info;   // ← chain별 recovery
  }

  tea_op->unique_num = unique_count++;
  tea_op->unique_num_per_proc = unique_count_per_core[proc_id]++;

  c->tea_op_count++;  // ← per-chain 카운터
  return tea_op;
}
```

**참고**: `tea_op->h2p_chain_id`는 `update_tea_fetch_stage()`에서 설정
(Section 4.1의 `tea_op->h2p_chain_id = tf->current_chain_id + 1`).

---

## 5. Phase F.4: exec_stage 다중 매칭

### 5.1 `exec_stage_bp_resolve()` — `h2p_chain_id` 직접 인덱싱

**파일**: `src/exec_stage.c`

기존(line 561): `op->inst_info->addr == tea->target_h2p_pc` 단일 비교
→ `h2p_chain_id`로 chain 식별 후 해당 chain의 H2P 정보 사용:

```c
static inline void exec_stage_bp_resolve(Op* op) {
  if (TEA_ENABLE && op->thread_id == 1) {
    if (!tea_is_active(op->proc_id)) return;

    Tea_Thread* tea = tea_threads[op->proc_id];

    // TEA H2P branch인지 확인: chain의 마지막 op이 H2P
    int chain_id = op->h2p_chain_id - 1;  // 1-based → 0-based
    if (chain_id < 0 || chain_id >= MAX_TEA_CHAINS) {
      STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
      return;
    }

    Tea_H2P_Chain* c = &tea->chains[chain_id];
    if (c->state == CHAIN_INACTIVE) {
      // 이미 종료된 chain의 잔여 op
      STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
      return;
    }

    // H2P branch 매칭: op의 PC가 해당 chain의 target_h2p_pc와 일치
    if (op->inst_info->addr == c->target_h2p_pc) {
      if (op->oracle_info.mispred || op->oracle_info.misfetch) {
        // main_h2p_op 유효성 검증 (unique_num 비교)
        Op* main_h2p = c->main_h2p_op;
        if (!main_h2p || !main_h2p->op_pool_valid ||
            main_h2p->unique_num != c->saved_unique_num) {
          // Main H2P가 이미 retire/flush됨 → 무시
          terminate_tea_chain(op->proc_id, chain_id);
          return;
        }

        if (main_h2p->off_path) {
          return;  // off-path → 상위 recovery가 처리
        }

        if (!main_h2p->oracle_info.recovery_sch) {
          if (reg_file_checkpoint_is_valid()) {
            // Case 2: post-rename (SRT checkpoint 존재)
            bp_sched_recovery(bp_recovery_info, main_h2p, op->exec_cycle,
                              FALSE, FALSE, EXTRA_LATE_RECOVERY_CYCLES);
            if (main_h2p->oracle_info.recovery_sch) {
              main_h2p->recovery_scheduled = TRUE;
            }
            main_h2p->oracle_info.recover_at_exec = FALSE;  // 이중 recovery 방지
            // → 다음 cycle cmp_recover() → recover_tea_on_flush(proc_id, recovery_op_num)
            //   recovery_op_num보다 younger한 chain만 종료
          } else {
            // Case 1: pre-rename (SRT checkpoint 없음)
            // recover_at_exec=TRUE 유지 — Main H2P가 rename→exec 정상 통과 후 recovery 발동
            // (recover_at_decode=TRUE 사용 안 함: flush_mispredict 우회 시 ALLOC orphan 발생)
            if (main_h2p->decode_cycle) {
              STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_NO_CHKPT);  // Case 1a: past decode
            } else {
              STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_DECODE);    // Case 1b: not yet decoded
            }
            terminate_tea_chain(op->proc_id, chain_id);  // 해당 chain만 즉시 종료
          }
          STAT_EVENT(op->proc_id, TEA_EARLY_FLUSHES);
        }
      } else {
        STAT_EVENT(op->proc_id, TEA_H2P_CORRECT);
        // 정답 예측: 해당 chain 종료 (precomputation 불필요)
        terminate_tea_chain(op->proc_id, chain_id);
      }
    }

    STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
    return;
  }
  /* ... 기존 Main thread BP 로직 ... */
}
```

**핵심 변경**: 단일 `tea->target_h2p_pc` 비교 → `chains[chain_id].target_h2p_pc` 비교.
chain 식별은 `op->h2p_chain_id`로 수행하므로 배열 순회가 필요 없음.

---

## 6. Phase F.5: selective flush + store buffer

### 6.1 `flush_tea_ops_by_chain_id()` (신규)

**파일**: `src/node_stage.c`

기존 `flush_tea_ops_from_node_stage()`는 모든 `thread_id==1` ops를 flush.
새 함수는 특정 `h2p_chain_id`의 ops만 선택적으로 flush:

```c
void flush_tea_ops_by_chain_id(uns proc_id, uns8 chain_id) {
  // 1. exec_stage에서 해당 chain의 ops 제거
  for (uns ii = 0; ii < exec->sd.max_op_count; ii++) {
    Op* op = exec->sd.ops[ii];
    if (op && op->thread_id == 1 && op->h2p_chain_id == chain_id) {
      exec->sd.ops[ii] = NULL;
      exec->sd.op_count--;
    }
  }

  // 2. dcache_stage에서 해당 chain의 ops 제거
  // (동일 패턴)

  // 3. ready_list에서 해당 chain의 ops 제거
  //    OS_SCHEDULED/OS_MISS가 ready list에 있으면 clear()가 미처리 → 여기서 RS 카운터도 감소
  //    (TEA_dispatch_plan.md §10.3 참조)
  Op** last;
  for (Op* rop = node->rdy_head, last = &node->rdy_head; rop;) {
    if (rop->thread_id == 1 && rop->h2p_chain_id == chain_id) {
      *last = rop->next_rdy;
      rop->in_rdy_list = FALSE;
      if (rop->state == OS_SCHEDULED || rop->state == OS_MISS) {
        if (node->rs[rop->rs_id].rs_op_count > 0)
          node->rs[rop->rs_id].rs_op_count--;
        if (node->rs[rop->rs_id].tea_op_count > 0)
          node->rs[rop->rs_id].tea_op_count--;
      }
      rop = rop->next_rdy;
    } else {
      last = &rop->next_rdy;
      rop = rop->next_rdy;
    }
  }

  // 4. scheduling buffer에서 해당 chain의 ops 제거
  // (동일 패턴)

  // 5. node table에서 해당 chain의 ops 제거
  node->node_tail = NULL;
  Op** last = &node->node_head;
  Op* op = node->node_head;
  while (op) {
    Op* next = op->next_node;
    if (op->thread_id == 1 && op->h2p_chain_id == chain_id) {
      *last = next;
      op->in_node_list = FALSE;

      // RS 카운터 갱신 (Bug Fix 8.1 반영: 모든 post-dispatch 미issue 상태 포괄)
      if (op->state != OS_IN_ROB && op->state != OS_SCHEDULED &&
          op->state != OS_MISS && op->state != OS_DONE) {
        if (node->rs[op->rs_id].rs_op_count > 0)
          node->rs[op->rs_id].rs_op_count--;
        if (node->rs[op->rs_id].tea_op_count > 0)
          node->rs[op->rs_id].tea_op_count--;
      }

      // dependent TEA ops의 not-rdy bit 강제 clear
      // → 구현 상세: TEA_reg_dependency_plan.md §4.1

      free_op(op);
      STAT_EVENT(proc_id, TEA_OPS_FLUSHED);
    } else {
      last = &op->next_node;
      node->node_tail = op;  // 마지막 유지 op를 tail로 갱신
    }
    op = next;
  }

  // 6. RS 카운터 갱신
  // flush된 TEA op 수만큼 rs->tea_op_count 감소
}
```

**참고**: `flush_tea_ops_from_node_stage()`(기존)는 `terminate_tea_thread()`에서
모든 chain을 한꺼번에 종료할 때 사용. 두 함수 공존.

### 6.2 `tea_store_buffer_clear_by_chain_id()` (신규)

**파일**: `src/tea/tea_store_buffer.c`

```c
void tea_store_buffer_clear_by_chain_id(uns proc_id, uns8 chain_id) {
  Tea_Store_Buffer* buf = tea_store_buffers[proc_id];
  for (uns i = 0; i < buf->capacity; i++) {
    if (buf->entries[i].valid && buf->entries[i].h2p_chain_id == chain_id) {
      buf->entries[i].valid = FALSE;
      buf->count--;
    }
  }
}
```

### 6.3 Store Buffer load forwarding

기존 `tea_store_buffer_lookup()`의 동작 유지:
모든 chain의 valid store를 검색하여 forwarding.
다른 chain의 store도 forwarding 가능 — functional correctness에 영향 없음
(논문에서 명시하지 않았으나 시뮬레이션에서는 안전).

### 6.4 Store Buffer에 chain ID 기록

`tea_store_buffer_insert()` 호출 시 `op->h2p_chain_id`를 entry에 복사:

```c
void tea_store_buffer_insert(uns proc_id, Op* op, Addr addr, uns size, uns8* data) {
  // ... 기존 로직 ...
  entry->h2p_chain_id = op->h2p_chain_id;  // 추가
}
```

---

## 7. Phase F.6: `recover_tea_on_flush()` 시그니처 변경

### 7.1 시그니처 변경

**파일**: `src/cmp_model.c`, `src/cmp_model.h`

```c
// 기존
void recover_tea_on_flush(uns proc_id);

// 변경
void recover_tea_on_flush(uns proc_id, Counter recovery_op_num);
```

### 7.2 구현

```c
void recover_tea_on_flush(uns proc_id, Counter recovery_op_num) {
  if (!TEA_ENABLE || !tea_is_active(proc_id)) return;

  Tea_Thread* tea = tea_threads[proc_id];

  /* recovery_op_num보다 younger한 chain 종료
   *
   * older/younger 판별 전략:
   *   TEA op_num (0x8000... 네임스페이스)과 Main op_num은 별도 카운터이므로
   *   직접 비교할 수 없다. 대신 각 chain이 trigger 시점에 저장한
   *   target_h2p_op_num (= Main H2P branch의 Main op_num)을 사용한다.
   *   recovery_op_num도 Main op_num이므로 동일 네임스페이스에서 비교가 성립.
   *
   *   예: Chain A (target_h2p_op_num=500), Chain B (target_h2p_op_num=600),
   *       Chain C (target_h2p_op_num=800)
   *       recovery_op_num=600이면 → Chain B, C 종료 (600 >= 600, 800 >= 600), Chain A는 유지
   */
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    Tea_H2P_Chain* c = &tea->chains[i];
    if (c->state == CHAIN_INACTIVE) continue;

    if (c->target_h2p_op_num >= recovery_op_num) {
      // younger chain → 종료
      terminate_tea_chain(proc_id, i);
    }
    // older chain → 유지 (recovery point 이전이므로 flush 대상 아님)
  }

  // 생존 chain의 flushed Main op 의존성 정리
  // → 구현 상세: TEA_reg_dependency_plan.md §4.2
}
```

### 7.3 `cmp_recover()` 호출부 변경

**파일**: `src/cmp_model.c` (line 443-446)

```c
// 기존
if (TEA_ENABLE) {
  recover_tea_on_flush(bp_recovery_info->proc_id);
}

// 변경
if (TEA_ENABLE) {
  recover_tea_on_flush(bp_recovery_info->proc_id,
                       bp_recovery_info->recovery_op_num);
}
```

### 7.4 Case 1 (pre-rename) 호출부 변경

**파일**: `exec_stage.c` — Case 1을 두 sub-case로 구분하되 처리 방식은 동일:

- **Case 1a** (`main_h2p->decode_cycle > 0`): Main H2P가 decode를 통과했지만 아직 rename 전.
  `recover_at_exec=TRUE` 유지 → Main H2P가 exec 도달 시 정상 recovery 발동.
- **Case 1b** (`main_h2p->decode_cycle == 0`): Main H2P가 아직 decode 전.
  `recover_at_decode=TRUE` **미사용** — flush_mispredict/SRT rollback 우회로 ALLOC orphan 발생하기 때문.
  `recover_at_exec=TRUE` 유지 → Main H2P가 rename→exec 정상 통과 후 recovery 발동.

양 sub-case 모두 `recover_tea_on_flush()` 직접 호출하지 않음.
대신 `terminate_tea_chain(proc_id, chain_id)`로 해당 chain만 즉시 종료.
Main thread recovery는 Main H2P가 exec에 도달할 때 `cmp_recover()`가 처리하며,
이때 `recover_tea_on_flush(proc_id, recovery_op_num)`이 호출되어
추가적으로 younger chain이 있으면 종료됨.

---

## 8. 추가 통계

**파일**: `src/tea/tea.stat.def`

```
DEF_STAT(TEA_TRIGGER_SKIP_FULL,     COUNT, NO_RATIO, "TEA trigger skipped: all chain slots full")
DEF_STAT(TEA_CHAIN_TERMINATED,       COUNT, NO_RATIO, "Individual TEA chain terminated")
DEF_STAT(TEA_CHAINS_CONCURRENT_MAX,  COUNT, NO_RATIO, "Max concurrent active TEA chains")
```

`update_tea_thread()`에서 `num_active_chains`의 최대값 추적:

```c
if (tea->num_active_chains > tea->stat_max_concurrent) {
  tea->stat_max_concurrent = tea->num_active_chains;
}
```

---

## 9. 수정 파일 요약

| 파일 | 변경 내용 | Phase | 규모 |
|------|----------|-------|------|
| `src/op.h` | `h2p_chain_id` 필드 추가 | F.1 | 1줄 |
| `src/tea/tea_thread.h` | `Tea_H2P_Chain` 구조체, `Tea_Thread` 변경 | F.1 | ~40줄 |
| `src/tea/tea_thread.c` | trigger/terminate/update를 per-chain으로 | F.2 | ~150줄 수정 |
| `src/tea/tea_fetch_stage.h` | `current_chain_id` 추가 | F.1 | 1줄 |
| `src/tea/tea_fetch_stage.c` | chain 전환 로직, chain_id 할당, per-chain oracle | F.3 | ~40줄 수정 |
| `src/exec_stage.c` | `exec_stage_bp_resolve()` 다중 chain 매칭 + `recover_exec_stage()` TEA op 스킵 | F.4 / F.5 | ~35줄 수정 |
| `src/dcache_stage.c` | `recover_dcache_stage()` TEA op 스킵 | F.5 | ~5줄 수정 |
| `src/node_stage.c` | `flush_tea_ops_by_chain_id()` 추가 | F.5 | ~50줄 추가 |
| `src/tea/tea_store_buffer.h` | entry에 `h2p_chain_id` 추가 | F.1 | 1줄 |
| `src/tea/tea_store_buffer.c` | `clear_by_chain_id()` 추가, insert에 chain_id 기록 | F.5 | ~15줄 |
| `src/cmp_model.c` | `recover_tea_on_flush()` 시그니처 + per-chain recovery | F.6 | ~25줄 |
| `src/cmp_model.h` | 함수 선언 변경 | F.6 | 1줄 |
| `src/tea/tea.stat.def` | 다중 H2P 통계 추가 | — | ~5줄 |
| `src/core.param.def` | `TEA_MAX_CHAINS` 파라미터 | F.1 | 1줄 |

**변경 불필요한 파일** (이미 multi-H2P 호환):

| 파일 | 이유 |
|------|------|
| `node_issue_queue.cc` | `thread_id` 기반 RS 파티셔닝/스케줄링 |
| `exec_ports.c` | RS 파티셔닝 |
| `map_rename.c` | `thread_id` 기반 TEA 제외 |
| `bp/bp.c` | 트리거 위치/조건 동일 |
| `bp/hbt.c` | H2P 감지 독립 |
| `tea/tea_rename.h/c` | Shadow RAT 공유 설계 유지 |
| `fill_buffer.c` | Main thread retire만 추적 |
| `dependency_chain_cache.c/h` | PC 인덱싱, chain 조회 동일. **단, 작업 HC (Hybrid Chain)에서 `block_pcs[]` 필드 추가 및 chain 재구축 로직 변경 — [`TEA_hybrid_chain_plan.md`](TEA_hybrid_chain_plan.md) 참조** |

> **⚠️ `dcache_stage.c` 및 `exec_stage.c` 추가 수정 필요**
>
> Main thread recovery 시 `recover_exec_stage()` / `recover_dcache_stage()`가
> TEA op_num(`0x8000...`)을 main thread recovery_op_num보다 크다고 판단하여
> 생존 chain의 TEA ops를 제거 → `tea_op_count` 영구 > 0 → chain 종료 불가.
>
> **두 함수 모두 TEA ops를 건너뛰도록 수정 필요** (`TEA_op_manage_plan.md §2.3` 참조):
> ```c
> if (TEA_ENABLE && op->thread_id == 1) continue;
> ```
> 이 수정이 없으면 `flush_tea_ops_by_chain_id()` step 0a/0b가 올바르게 동작하지 않는다.

---

## 10. 구현 순서

```
Phase F.1: 데이터 구조 변경
  op.h (h2p_chain_id)
  tea_thread.h (Tea_H2P_Chain, Tea_Thread 변경)
  tea_fetch_stage.h (current_chain_id)
  tea_store_buffer.h (entry에 chain_id)
  core.param.def (TEA_MAX_CHAINS)
    │
    ├──→ Phase F.2: tea_thread.c (trigger/terminate/update per-chain)
    │       │
    │       └──→ Phase F.3: tea_fetch_stage.c (chain 전환, chain_id 부여)
    │
    ├──→ Phase F.4: exec_stage.c (다중 chain 매칭)
    │
    └──→ Phase F.5: node_stage.c (flush_by_chain_id)
                tea_store_buffer.c (clear_by_chain_id, insert chain_id)
                    │
                    └──→ Phase F.6: cmp_model.c/h (recover_tea_on_flush 시그니처)
```

**빌드 검증**: 각 Phase 완료 후 `./sci --build-scarab tea_dbg`로 컴파일 확인.
Phase F.1~F.2 완료 시점에서 `MAX_TEA_CHAINS=1`로 설정하면
기존 단일 H2P 동작과 동일해야 함 (회귀 테스트).

---

## 11. 검증 체크리스트

### Phase F.1~F.2 완료 후 (MAX_TEA_CHAINS=1)

```
TEA_TRIGGERS            ≈ 기존 값 (회귀 확인)
TEA_TRIGGER_SKIP_FULL   = 기존 TEA_TRIGGER_SKIP_ACTIVE (이름 변경)
TEA_OPS_FETCHED         ≈ 기존 값
TEA_EARLY_FLUSHES       ≈ 기존 값
```

### Phase F.3~F.6 완료 후 (MAX_TEA_CHAINS=4)

```
TEA_TRIGGER_SKIP_FULL   < 기존 TEA_TRIGGER_SKIP_ACTIVE (감소)
TEA_TRIGGERS            > 기존 값 (증가)
TEA_CHAINS_CONCURRENT_MAX > 1   ← 다중 chain 동시 실행 확인
TEA_CHAIN_TERMINATED    > 0     ← 개별 chain 종료 발생
```

### 시뮬레이션

```bash
cd ~/scarab-infra
./sci --build-scarab tea_dbg
./sci --sim tea_dbg
/analyze_tea_sim
```

---

## 12. 알려진 주의사항

### 12.1 Op* 유효성 (dangling pointer)

`Tea_H2P_Chain.main_h2p_op`은 Main thread에서 해당 Op이 retire되면 dangling.
`saved_unique_num`과 `op_pool_valid`로 사용 시점에 유효성 검증 필수.
(상세: `TEA_early_flush_plan.md` EF-4)

### 12.2 tea_op_counter 공유

`0x8000000000000000ULL`에서 시작하는 단일 카운터를 모든 chain이 공유.
순차 fetch이므로 chain 간 op_num이 겹치지 않음.
단, older/younger 비교 시 TEA op_num이 아닌 `target_h2p_op_num`(Main 네임스페이스) 사용.
(상세: `TEA_early_flush_plan.md` Section 3.6)

### 12.3 Shadow RAT 순차 갱신

Chain A rename 후 Chain B rename 시 Shadow RAT에는 Chain A의 변경이 반영된 상태.
이것이 논문의 의도된 동작. Chain B가 Chain A와 동일한 arch reg을 사용하면
Chain A가 할당한 TEA preg을 src로 읽게 됨.

### 12.4 preg pool 공유

모든 chain이 하나의 TEA preg pool을 공유. `TEA_PREG_RESERVATION=192`에서
4개 chain이 동시 활성이면 chain당 평균 48개 preg.
chain 길이가 긴 경우 preg 부족 가능 — `tea_preg_pool_alloc()` 실패 시 chain 종료.

### 12.5 전체 Recovery 시 TEA 정리

`cmp_recover()` → `recover_tea_on_flush(proc_id, recovery_op_num)`:
`target_h2p_op_num >= recovery_op_num`인 모든 chain 종료.
Recovery point가 모든 활성 chain보다 older하면 전체 TEA가 종료됨.

### 12.6 Store Buffer 격리

다중 chain의 stores가 동일 store buffer에 공존.
`h2p_chain_id`로 태깅하여 chain 종료 시 해당 entry만 무효화.
Load forwarding은 모든 chain의 stores를 검색.

---

## 13. Plan 파일 간 불일치 — 구현 시 해결 필요

> 다중 H2P 관련 로직이 4개 plan 파일(`TEA_multi_h2p_plan.md`, `TEA_early_flush_plan.md`,
> `TEA_shadow_ftq_plan.md`, `TEA_op_manage_plan.md`)에 분산 기술되어 있으며,
> 아래 4건의 설계 불일치가 존재한다. 구현(작업 F) 시작 시 canonical 버전을 확정해야 한다.

### 13.1 `terminate_tea_chain()` 구현 불일치 — ✅ **방안 A로 결정**

동일 함수가 4개 파일에 서로 다른 버전으로 기술되어 있었으며, **방안 A**로 통일 완료:

**`TEA_op_manage_plan.md` §3.3 방식** 채택:
- fetch를 `recover_tea_fetch_stage_by_chain()`, rename을 `recover_tea_rename_stage_by_chain()` (신규 헬퍼)로 per-chain 정리
- `flush_tea_ops_by_chain_id()` 내부 호출
- caller 단순화 (`terminate_tea_chain()` 한 번만 호출로 완결)
- `tea_op_count == 0`인 경우 `flush_tea_ops_by_chain_id()`가 실질적으로 no-op이므로 항상 호출해도 성능 문제 없음

**적용 완료한 파일**: 본 문서 §3.2, `TEA_early_flush_plan.md` §3.4, `TEA_shadow_ftq_plan.md` §4.5

### 13.2 `recover_tea_on_flush()` 구현 불일치 — ✅ **방안 A로 결정**

§13.1에서 `terminate_tea_chain()`이 canonical 종료 함수로 결정되었으므로, **본 문서 §7.2 방식** 채택:
- 루프에서 `terminate_tea_chain()` 호출
- `terminate_tea_chain()` 내부에서 `num_active_chains==0` 시 공유 자원 자동 정리
- 루프 후 중복 `reset_tea_preg_pool()` 등 재호출 **불필요** → 제거
- `chains[]` 배열 인덱스로 순회하므로 `terminate_tea_chain()` 호출로 `num_active_chains`가 변해도 안전

**적용 완료한 파일**: `TEA_early_flush_plan.md` §3.6 (중복 리셋 제거), `TEA_shadow_ftq_plan.md` §6 (인라인→terminate 호출)

### 13.3 필드 이름 불일치 — ✅ **본 문서 이름으로 통일 완료**

본 문서(`TEA_multi_h2p_plan.md`)의 이름을 기준으로 `TEA_shadow_ftq_plan.md`를 통일:

| 개념 | 통일된 이름 | 이전 `TEA_shadow_ftq_plan.md` 이름 |
|------|-----------|-----------------------------------|
| Tea_Thread의 현재 fetch chain 인덱스 | `current_fetch_chain` | `current_fetch_chain_idx` |
| Tea_Fetch_Stage의 chain 참조 | `current_chain_id` | `fetching_chain_slot` |

**적용 완료한 파일**: `TEA_shadow_ftq_plan.md` §3.2, §3.3, §3.5, §4.5 및 구현 일정표

### 13.4 `tea_create_op_from_cache()` 시그니처 불일치 — ✅ **방안 A (3 args)로 결정**

3 args 방식 (본 문서 §4.2) 채택. fetch stage가 현재 chain을 이미 추적(`tf->current_chain_id`)하므로
추가 인자 불필요. `tea_create_op_from_cache()`는 항상 `update_tea_fetch_stage()` 내부에서만 호출됨.

**적용 완료한 파일**: `TEA_shadow_ftq_plan.md` §4.3 (4-args → 3-args 변경)
