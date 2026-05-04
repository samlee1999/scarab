# Shadow FTQ / TEA Fetch 구현 계획

**최종 갱신**: 2026-05-04
**관련 상태 문서**: `../TEA_implementation_status/TEA_shadow_ftq_status.md`

> **현재 상태**: 논문의 Shadow FTQ FIFO와 BP fetch-address stream stitching은 구현하지 않았다. 현재 코드는 HBT가 H2P로 판정한 branch PC로 Dependency Chain Cache를 직접 조회하고, 해당 chain을 TEA fetch stage가 sequential하게 fetch한다.
>
> 이 문서는 현재 DCC 직접 조회 모델과 향후 Shadow FTQ/Hybrid Chain 후보 작업을 구분해서 기록한다.

---

## 0. 현재 코드 기준 요약

| 항목 | 현재 상태 |
|------|-----------|
| Shadow FTQ FIFO | 미구현 |
| BP fetch address stream 기반 stitch | 미구현 |
| Dependency Chain Cache 직접 조회 | 구현됨 |
| DCC size | 1024 direct-mapped entry |
| Block Cache OR 누적 | 구현됨, 현재 fetch path에는 직접 미사용 |
| snapshot 내 모든 H2P backward walk | 구현됨 |
| multi-H2P sequential fetch | 구현됨 |
| `TEA_FETCH_WIDTH` | 구현됨 |

현재 TEA fetch path:

```
trigger_tea_thread()
  -> setup_fetch_for_chain()
  -> get_dependency_chain(proc_id, target_h2p_pc)
  -> DCC entry chain[] sequential fetch
```

현재 남은 frontend 관련 분석:

- `TEA_TRIGGER_SKIP_NO_CHAIN`이 큰 benchmark에서 DCC coverage 부족 여부 확인.
- `TEA_TRIGGER_SKIP_FULL`이 frontend slot 병목인지 backend lifecycle 병목인지 분리.
- Block Cache OR 누적을 실제 TEA fetch source로 사용할 Hybrid Chain 작업의 필요성 판단.

---

> **Historical implementation record**
>
> 아래 섹션의 C-like pseudocode는 multi-H2P fetch 구현 전 설계 기록이다. 현재 코드 상태는 위 요약과 `TEA_shadow_ftq_status.md`를 우선한다.

---

## 1. 현재 구조의 다중 H2P 제약

### 1.1 단일 chain 전용 필드 (`tea_fetch_stage.h:45-60`)

```c
typedef struct Tea_Fetch_Stage_struct {
  // ... 단일 chain만 참조 가능 ...
  Dependency_Chain_Cache_Entry* active_chain;  // 하나의 chain만
  int current_chain_idx;                       // 하나의 position만
  int total_chain_length;
  Flag fetch_complete;
} Tea_Fetch_Stage;
```

### 1.2 trigger → fetch 1:1 관계

`trigger_tea_thread()` (`tea_thread.c:99`)에서 `state != TEA_IDLE`이면 거부.
새 H2P가 감지되어도 현재 chain의 fetch/execute가 끝날 때까지 대기 불가.

### 1.3 상태 전이의 단일 chain 가정

`update_tea_thread()` (`tea_thread.c:224-246`):
- `TEA_FETCHING` → `TEA_EXECUTING`: `fetch_complete` 하나만 확인
- `TEA_EXECUTING` → terminate: `tea_op_count == 0` (전체 TEA ops 합산)

---

## 2. 다중 H2P Fetch 설계: 순차 fetch + 중첩 실행

논문 Section III-B:
> "Subsequently, it can fetch and initiate the precomputation for the next instance of
> H2P branch A₂ faster than the main thread."

**모델**: TEA Fetch는 한 번에 하나의 chain만 fetch하지만, fetch 완료된 chain의 ops는
backend에서 계속 실행되는 동안 다음 chain의 fetch를 즉시 시작.

```
시간 →
Chain #0: [===FETCH===][=========EXECUTE=========]
Chain #1:              [===FETCH===][====EXECUTE====]
Chain #2:                           [==FETCH==][==EXEC==]
                                                        │
                          모든 chain ops 완료 → terminate_tea_thread()
```

---

## 3. 데이터 구조 변경

### 3.1 `Tea_H2P_Chain` 구조체 (`tea_thread.h` — 신규)

각 H2P chain의 독립적 상태를 추적:

```c
typedef enum Tea_Chain_State_enum {
  CHAIN_INACTIVE,   // 슬롯 비어있음
  CHAIN_FETCHING,   // fetch 대기 중 (아직 fetch 안 됨 or fetch 진행 중)
  CHAIN_EXECUTING,  // fetch 완료, backend에서 실행 중
} Tea_Chain_State;

typedef struct Tea_H2P_Chain_struct {
  Tea_Chain_State state;
  Addr            target_h2p_pc;
  Counter         target_h2p_op_num;

  /* Main H2P op 참조 (Early Flush용) */
  Op*             main_h2p_op;
  Counter         saved_unique_num;    // Op* 유효성 검증용
  Op_Info         h2p_oracle_info;     // trigger 시 복사
  Recovery_Info   h2p_recovery_info;   // trigger 시 복사

  /* Per-chain 추적 */
  uns             tea_op_count;        // 이 chain의 pipeline 내 ops 수
  Counter         tea_ops_fetched;     // 이 chain에서 fetch된 총 ops 수
} Tea_H2P_Chain;
```

### 3.2 `Tea_Thread` 구조체 변경 (`tea_thread.h`)

```c
#define MAX_TEA_CHAINS 4  // 파라미터 TEA_MAX_CHAINS로 제어

typedef struct Tea_Thread_struct {
  uns8 proc_id;

  /* 다중 H2P chain 배열 */
  Tea_H2P_Chain chains[MAX_TEA_CHAINS];
  uns num_active_chains;         // 현재 활성 chain 수 (FETCHING + EXECUTING)
  int current_fetch_chain;       // 현재 fetch 중인 chain의 인덱스 (-1이면 없음)

  /* 기존 단일 필드 → 제거 또는 미사용 */
  // Addr target_h2p_pc;         → chains[i].target_h2p_pc로 대체
  // Op* main_h2p_op;            → chains[i].main_h2p_op로 대체

  /* 공유 자원 (변경 없음) */
  Counter tea_op_counter;        // 전체 TEA ops 순차 카운터
  Counter tea_start_cycle;

  /* 통계 (변경 없음) */
  Counter stat_tea_triggers;
  Counter stat_tea_early_flushes;
  Counter stat_tea_ops_executed;
} Tea_Thread;
```

### 3.3 `Tea_Fetch_Stage` 구조체 변경 (`tea_fetch_stage.h`)

```c
typedef struct Tea_Fetch_Stage_struct {
  uns8 proc_id;
  Stage_Data sd;

  /* 현재 fetch 중인 chain 상태 */
  Dependency_Chain_Cache_Entry* active_chain;
  int current_chain_idx;
  int total_chain_length;
  Flag fetch_complete;
  Counter ops_fetched_this_cycle;

  /* 다중 H2P: 현재 어떤 chain을 fetch 중인지 */
  int current_chain_id;          // chains[] 인덱스 (-1이면 미할당)
} Tea_Fetch_Stage;
```

### 3.4 `Op` 구조체 추가 (`op.h`)

```c
uns8 h2p_chain_id;  // 0 = main op, 1~MAX_TEA_CHAINS = TEA chain ID
```

---

## 4. 핵심 로직 변경

### 4.1 `trigger_tea_thread()` — 빈 슬롯에 chain 추가

기존: `state != TEA_IDLE` → 거부
변경: 빈 chain 슬롯에 추가, 모든 슬롯 사용 중이면 거부

```c
void trigger_tea_thread(uns proc_id, Addr h2p_pc, Counter h2p_op_num, Op* h2p_op) {
  Tea_Thread* tea = tea_threads[proc_id];
  STAT_EVENT(proc_id, TEA_TRIGGER_ATTEMPTS);

  // 빈 chain 슬롯 찾기
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

  // Dependency chain 존재 확인
  Dependency_Chain_Cache_Entry* chain = get_dependency_chain(proc_id, h2p_pc);
  if (!chain || !chain->is_valid || chain->chain_length == 0) {
    STAT_EVENT(proc_id, TEA_TRIGGER_SKIP_NO_CHAIN);
    return;
  }

  // 슬롯에 H2P 정보 저장
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

  // 첫 번째 활성 chain이면 Shadow RAT snapshot + fetch 시작
  if (tea->num_active_chains == 1) {
    shadow_rat_snapshot(proc_id);
    tea->tea_op_counter = 0x8000000000000000ULL;
  }

  // 현재 fetch 중인 chain이 없으면 이 chain의 fetch 시작
  if (tea->current_fetch_chain < 0) {
    tea->current_fetch_chain = slot;
    reset_tea_fetch_stage(proc_id);
    tea_fetch_stages[proc_id]->current_chain_id = slot;
  }

  STAT_EVENT(proc_id, TEA_TRIGGERS);
}
```

**Shadow RAT snapshot 타이밍**:
- 첫 번째 chain 활성화 시 한 번만 snapshot (`num_active_chains == 1`)
- 이후 chain들은 이전 chain이 갱신한 Shadow RAT 위에서 rename (논문과 일치)
- 논문 IV-D: "The contents of the main RAT are copied into the shadow RAT before the
  first TEA thread instruction is renamed to synchronize the state of both threads."

### 4.2 `update_tea_fetch_stage()` — chain 전환 로직

현재 chain의 fetch 완료 시 다음 `CHAIN_FETCHING` 상태 chain으로 자동 전환:

```c
void update_tea_fetch_stage(uns proc_id) {
  Tea_Fetch_Stage* tea_fetch = tea_fetch_stages[proc_id];
  Tea_Thread* tea = tea_threads[proc_id];

  if (!tea || tea->num_active_chains == 0) return;

  int slot = tea->current_fetch_chain;
  if (slot < 0) {
    // 현재 fetch 중인 chain 없음 → 다음 CHAIN_FETCHING chain 검색
    slot = find_next_fetching_chain(proc_id);
    if (slot < 0) return;  // 모든 chain이 CHAIN_EXECUTING 또는 INACTIVE
    tea->current_fetch_chain = slot;
    setup_fetch_for_chain(tea_fetch, tea, slot);
  }

  Tea_H2P_Chain* chain = &tea->chains[slot];

  // --- 기존 fetch 로직 (active_chain 조회, op 생성) ---
  // get_dependency_chain() → tea_create_op_from_cache() 루프
  // 단, tea_create_op_from_cache() 내부에서:
  //   tea_op->h2p_chain_id = slot + 1;  // 1-based
  //   chain->tea_op_count++;             // per-chain 카운트
  // ...

  // Fetch 완료 확인
  if (tea_fetch->fetch_complete) {
    chain->state = CHAIN_EXECUTING;

    // 다음 CHAIN_FETCHING chain 검색
    int next = find_next_fetching_chain(proc_id);
    if (next >= 0) {
      tea->current_fetch_chain = next;
      setup_fetch_for_chain(tea_fetch, tea, next);
    } else {
      tea->current_fetch_chain = -1;  // 모든 chain fetch 완료
    }
  }
}
```

**헬퍼 함수**:

```c
/* 다음 CHAIN_FETCHING 상태의 chain 인덱스 반환. 없으면 -1.
 * current_fetch_chain 이후부터 순환 탐색 (공정성 보장). */
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

// 새 chain의 fetch를 위해 fetch stage 초기화
static void setup_fetch_for_chain(Tea_Fetch_Stage* tea_fetch,
                                   Tea_Thread* tea, int slot) {
  Tea_H2P_Chain* chain = &tea->chains[slot];

  tea_fetch->active_chain = get_dependency_chain(tea->proc_id,
                                                  chain->target_h2p_pc);
  if (!tea_fetch->active_chain || !tea_fetch->active_chain->is_valid) {
    // Chain 사라짐 → terminate_tea_chain()으로 정리 (방안 A 원칙)
    terminate_tea_chain(tea->proc_id, slot);
    STAT_EVENT(tea->proc_id, TEA_FETCH_CHAIN_MISS);
    return;
  }
  STAT_EVENT(tea->proc_id, TEA_FETCH_CHAIN_HIT);

  tea_fetch->current_chain_idx = 0;
  tea_fetch->total_chain_length = tea_fetch->active_chain->chain_length;
  tea_fetch->fetch_complete = FALSE;
  tea_fetch->current_chain_id = slot;
  tea_fetch->ops_fetched_this_cycle = 0;
}
```

### 4.3 `tea_create_op_from_cache()` 변경 — chain ID 할당

```c
Op* tea_create_op_from_cache(uns proc_id, Op* cached_op,
                              Flag is_h2p_branch) {
  Tea_Thread* tea = tea_threads[proc_id];
  Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
  Tea_H2P_Chain* chain = &tea->chains[tf->current_chain_id];  // fetch stage가 현재 chain 추적

  Op* tea_op = alloc_op(proc_id);
  if (!tea_op) return NULL;

  // 정적 정보 복사 (기존과 동일)
  tea_op->inst_info = cached_op->inst_info;
  tea_op->table_info = cached_op->table_info;

  // TEA 동적 정보
  tea_op->proc_id = proc_id;
  tea_op->thread_id = 1;
  tea_op->h2p_chain_id = tf->current_chain_id + 1;  // 1-based (0 = main op)
  tea_op->op_num = tea->tea_op_counter++;
  tea_op->fetch_cycle = cycle_count;
  tea_op->off_path = FALSE;
  tea_op->state = OS_FETCHED;
  tea_op->unique_num = unique_count++;
  tea_op->unique_num_per_proc = unique_count_per_core[proc_id]++;

  // H2P branch: per-chain oracle/recovery 사용
  if (is_h2p_branch) {
    tea_op->oracle_info = chain->h2p_oracle_info;
    tea_op->recovery_info = chain->h2p_recovery_info;
  }

  // Per-chain op count 증가
  chain->tea_op_count++;
  chain->tea_ops_fetched++;

  STAT_EVENT(proc_id, TEA_OPS_FETCHED);
  return tea_op;
}
```

### 4.4 `update_tea_thread()` — per-chain 완료 감지

```c
void update_tea_thread(uns proc_id) {
  Tea_Thread* tea = tea_threads[proc_id];
  if (tea->num_active_chains == 0) return;

  // (1) Fetch 중인 chain이 있으면 → 상태 전이는 update_tea_fetch_stage()에서 처리

  // (2) CHAIN_EXECUTING chain들의 완료 확인
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    Tea_H2P_Chain* chain = &tea->chains[i];
    if (chain->state == CHAIN_EXECUTING &&
        chain->tea_op_count == 0 && chain->tea_ops_fetched > 0) {
      // 이 chain의 모든 ops 완료
      terminate_tea_chain(proc_id, i);
    }
  }

  // (3) 공유 자원 정리는 terminate_tea_chain() 내부에서 자동 처리
  //     (num_active_chains == 0일 때 reset_tea_preg_pool 등 수행)
}
```

### 4.5 `terminate_tea_chain()` — 개별 chain 종료 (신규)

> **방안 A 채택** (`TEA_op_manage_plan.md` §3.3 방식):
> `terminate_tea_chain()`이 fetch/rename/node 모든 stage를 per-chain으로 정리.

```c
void terminate_tea_chain(uns proc_id, int chain_slot) {
  Tea_Thread* tea = tea_threads[proc_id];
  Tea_H2P_Chain* chain = &tea->chains[chain_slot];
  uns8 h2p_chain_id = chain_slot + 1;  // 1-based

  ASSERT(proc_id, chain->state != CHAIN_INACTIVE);

  /* 1. fetch 중인 chain이면 fetch stage에서 해당 chain ops 제거 */
  if (chain->state == CHAIN_FETCHING &&
      tea->current_fetch_chain == chain_slot) {
    recover_tea_fetch_stage_by_chain(proc_id, h2p_chain_id);
  }

  /* 2. rename stage에서 해당 chain ops 제거 */
  recover_tea_rename_stage_by_chain(proc_id, h2p_chain_id);

  /* 3. node stage (exec/dcache/RS/node table)에서 해당 chain ops flush */
  flush_tea_ops_by_chain_id(proc_id, h2p_chain_id);

  /* 4. store buffer에서 해당 chain 엔트리 무효화 */
  tea_store_buffer_clear_by_chain_id(proc_id, h2p_chain_id);

  /* 5. chain 상태 초기화 */
  chain->state = CHAIN_INACTIVE;
  chain->target_h2p_pc = 0;
  chain->target_h2p_op_num = 0;
  chain->main_h2p_op = NULL;
  chain->tea_op_count = 0;
  chain->tea_ops_fetched = 0;
  tea->num_active_chains--;
  STAT_EVENT(proc_id, TEA_CHAIN_TERMINATED);

  /* 6. 모든 chain 종료 시 전체 TEA 자원 정리 */
  if (tea->num_active_chains == 0) {
    reset_tea_preg_pool(proc_id);
    reset_tea_store_buffer(proc_id);
    recover_tea_rename_stage(proc_id);  // Shadow RAT 무효화
    tea->state = TEA_IDLE;
  }
}
```

### 4.6 `tea_op_completed()` — per-chain count 감소

```c
void tea_op_completed(uns proc_id, Op* op) {
  Tea_Thread* tea = tea_threads[proc_id];
  ASSERT(proc_id, op->thread_id == 1);

  int chain_slot = op->h2p_chain_id - 1;  // 1-based → 0-based
  ASSERT(proc_id, chain_slot >= 0 && chain_slot < MAX_TEA_CHAINS);

  Tea_H2P_Chain* chain = &tea->chains[chain_slot];
  if (chain->tea_op_count > 0) {
    chain->tea_op_count--;
  }

  STAT_EVENT(proc_id, TEA_OPS_EXECUTED);
}
```

**중요**: `terminate_tea_chain()`을 여기서 직접 호출하지 않는다 (re-entrancy 문제 방지).
Chain 완료 감지는 `update_tea_thread()`에서 수행 (deferred termination, 방법 A).

### 4.7 `tea_is_active()` 변경

```c
Flag tea_is_active(uns proc_id) {
  if (!tea_threads || !tea_threads[proc_id]) return FALSE;
  return tea_threads[proc_id]->num_active_chains > 0;
}
```

---

## 5. Early Flush와의 연동

다중 H2P에서의 Early Flush 처리는 `TEA_early_flush_plan.md`에 상세히 기술되어 있으며,
fetch stage와 관련된 핵심 포인트만 여기에 요약:

### 5.1 `exec_stage_bp_resolve()` — `h2p_chain_id` 직접 인덱싱

기존: `tea->target_h2p_pc` 하나와 비교
변경: `op->h2p_chain_id`로 chain 직접 식별 후 해당 chain의 H2P 정보 사용
(배열 순회 불필요 — `h2p_chain_id`를 `Op`에 추가한 이점 활용)

```c
// exec_stage.c에서:
int chain_id = op->h2p_chain_id - 1;  // 1-based → 0-based
if (chain_id < 0 || chain_id >= MAX_TEA_CHAINS) return;

Tea_H2P_Chain* c = &tea->chains[chain_id];
if (c->state == CHAIN_INACTIVE) return;  // 이미 종료된 chain의 잔여 op

if (op->inst_info->addr == c->target_h2p_pc) {
  // 이 chain의 H2P branch가 실행됨 → mispred 비교
  // ... (상세: TEA_multi_h2p_plan.md Section 5.1)
}
```

**참고**: `TEA_multi_h2p_plan.md` §5.1의 전체 구현과 동일한 방식.

### 5.2 Early Flush 시 fetch stage 영향

Case 1 (pre-rename: `recover_at_decode`):
- 현재 fetch 중인 chain이 flush 대상이면 → `recover_tea_fetch_stage()` + chain 종료
- 다른 chain은 영향 없음

Case 2 (post-rename: `bp_sched_recovery`):
- `recover_tea_on_flush(proc_id, recovery_op_num)` 호출
- `chain->target_h2p_op_num >= recovery_op_num`인 모든 chain 종료
- Fetch stage가 종료된 chain을 fetch 중이었으면 다음 chain으로 전환

---

## 6. `recover_tea_on_flush()` 연동

Main thread recovery 시 (`cmp_model.c` → `cmp_recover()`):

> **방안 A 채택** (`TEA_multi_h2p_plan.md` §7.2 방식):
> 루프에서 `terminate_tea_chain()` 호출. 공유 자원 정리는 `terminate_tea_chain()` 내부에서
> `num_active_chains==0` 시 자동 처리되므로 루프 후 중복 리셋 불필요.

```c
void recover_tea_on_flush(uns proc_id, Counter recovery_op_num) {
  if (!TEA_ENABLE || !tea_is_active(proc_id)) return;

  Tea_Thread* tea = tea_threads[proc_id];

  /* recovery_op_num보다 younger한 chain 종료
   * target_h2p_op_num (Main 네임스페이스)과 recovery_op_num (Main 네임스페이스)을 비교 */
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    Tea_H2P_Chain* chain = &tea->chains[i];
    if (chain->state == CHAIN_INACTIVE) continue;

    if (chain->target_h2p_op_num >= recovery_op_num) {
      terminate_tea_chain(proc_id, i);
      /* terminate_tea_chain() 내부에서:
       * - per-chain fetch/rename/node stage 정리
       * - store buffer 정리
       * - num_active_chains--
       * - num_active_chains==0이면 공유 자원 자동 정리 */
    }
  }
  /* 루프 후 중복 리셋 불필요 — terminate_tea_chain()에서 이미 처리 */
}
```

---

## 7. 파라미터 추가 (`core.param.def`)

```c
DEF_PARAM(tea_max_chains, TEA_MAX_CHAINS, uns, uns, 4, )
```

`MAX_TEA_CHAINS` 매크로를 이 파라미터 값으로 대체하거나, 컴파일 타임 상한으로 유지하고
런타임에 `TEA_MAX_CHAINS <= MAX_TEA_CHAINS`를 검증.

---

## 8. 통계 추가 (`tea.stat.def`)

```
TEA_TRIGGER_SKIP_FULL     - 모든 chain 슬롯 사용 중이어서 trigger 거부
TEA_CHAIN_TERMINATED      - 개별 chain 정상 종료 수
TEA_CHAINS_CONCURRENT_MAX - 동시 활성 chain 최대 수 (high watermark)
```

---

## 9. 수정 파일 요약

| 파일 | 변경 내용 | 규모 |
|------|----------|------|
| `src/tea/tea_thread.h` | `Tea_H2P_Chain` 구조체, `Tea_Thread` 다중 chain 필드 | ~40줄 추가 |
| `src/tea/tea_thread.c` | `trigger_tea_thread()` 슬롯 할당, `terminate_tea_chain()`, `tea_op_completed()` per-chain, `update_tea_thread()` per-chain 완료 감지 | ~100줄 수정 |
| `src/tea/tea_fetch_stage.h` | `current_chain_id` 필드 추가 | 2줄 |
| `src/tea/tea_fetch_stage.c` | `update_tea_fetch_stage()` chain 전환, `tea_create_op_from_cache()` chain ID, `setup_fetch_for_chain()` 헬퍼 | ~60줄 수정 |
| `src/op.h` | `h2p_chain_id` 필드 | 1줄 |
| `src/core.param.def` | `TEA_MAX_CHAINS` 파라미터 | 1줄 |
| `src/tea/tea.stat.def` | 3개 통계 추가 | 3줄 |

**변경 불필요** (다중 H2P fetch 관점):
- `dependency_chain_cache.h/c` — `get_dependency_chain()` 인터페이스 변경 없음. **단, 작업 HC (Hybrid Chain)에서 별도 변경 예정 — [`TEA_hybrid_chain_plan.md`](TEA_hybrid_chain_plan.md) 참조**
- `tea_rename.h/c` — Shadow RAT는 chain 무관하게 순차 갱신
- `node_issue_queue.cc` — thread_id 기반 스케줄링 (chain 무관)

---

## 10. 구현 순서

```
단계 1: 데이터 구조 변경
  ├─ Tea_H2P_Chain 구조체 정의 (tea_thread.h)
  ├─ Tea_Thread 다중 chain 필드 추가
  ├─ Tea_Fetch_Stage에 current_chain_id 추가
  └─ Op에 h2p_chain_id 추가

단계 2: trigger_tea_thread() 수정
  ├─ 빈 슬롯 탐색 → chain 정보 저장
  ├─ Shadow RAT snapshot은 첫 활성화 시에만
  └─ current_fetch_chain 설정

단계 3: update_tea_fetch_stage() 수정
  ├─ setup_fetch_for_chain() 헬퍼
  ├─ chain 전환 로직 (fetch_complete → 다음 chain)
  └─ tea_create_op_from_cache()에 chain_slot 전달

단계 4: tea_op_completed() / update_tea_thread() 수정
  ├─ per-chain tea_op_count 감소
  ├─ per-chain 완료 감지 → terminate_tea_chain()
  └─ 전체 종료 감지 → 공유 자원 정리

단계 5: recover_tea_on_flush() 수정
  ├─ chain 배열 순회하며 flush 대상 선별
  ├─ fetch stage 정리
  └─ 부분 종료 / 전체 종료 분기

단계 6: 통계 + 파라미터 추가
```

---

## 11. 검증

```bash
cd ~/scarab-infra
./sci --build-scarab tea_dbg
./sci --sim tea_dbg
/analyze_tea_sim
```

**확인 지표**:
- `TEA_TRIGGER_SKIP_FULL` < 이전 `TEA_TRIGGER_SKIP_ACTIVE` (슬롯이 있으므로 거부 감소)
- `TEA_TRIGGERS` 증가 (더 많은 H2P가 처리됨)
- `TEA_CHAIN_TERMINATED > 0` (개별 chain 종료 발생)
- `TEA_CHAINS_CONCURRENT_MAX > 1` (동시 활성 chain 확인)

---

## 12. 주의사항

### 12.1 Dependency Chain Cache 충돌

`dependency_chain_caches`는 `pc % DEPENDENCY_CHAIN_CACHE_SIZE`로 인덱싱.
같은 H2P PC의 다른 인스턴스가 동일 엔트리를 공유함 → 문제 없음 (같은 chain).
서로 다른 H2P PC가 같은 인덱스에 매핑되면 마지막 BW Walk 결과로 덮어쓰임.
다중 H2P에서 Chain A와 Chain B가 같은 인덱스를 사용하면 하나가 miss될 수 있음 →
cache size (1024)가 충분히 크므로 실제로는 드묾.

### 12.2 Shadow RAT 순차 갱신

Chain A rename 후 Chain B rename 시 Shadow RAT에는 Chain A의 변경이 반영됨.
이것이 논문의 의도된 동작.
Chain B의 src가 Chain A의 dst에 의존하면 올바른 producer가 설정됨.

### 12.3 tea_op_counter 공유

모든 chain이 하나의 `tea_op_counter`를 공유. 순차 fetch이므로 op_num이 겹치지 않음.
Chain A의 ops: `0x8000000000000000` ~ `0x800000000000000F`
Chain B의 ops: `0x8000000000000010` ~ `0x800000000000001A`
(예시 — 실제 값은 chain 크기에 따라 다름)

### 12.4 TEA preg pool 관리

모든 chain이 하나의 TEA preg pool을 공유 (`tea_rename.c:296-329`).
개별 chain 종료 시 preg 반환이 불가능 (Shadow RAT에 dangling 매핑 위험).
전체 TEA 종료 시에만 `reset_tea_preg_pool()` 호출.
→ 자세한 분석은 `TEA_op_manage_plan.md` Section 5 참조.
