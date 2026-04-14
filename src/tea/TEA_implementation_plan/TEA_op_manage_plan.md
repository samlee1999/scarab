# TEA Op 관리: 다중 H2P 구현 계획

> **설계 목표**: 논문 Section IV-G "TEA thread termination" — "On a miss, the remaining TEA thread
> instructions continue to precompute directions for any remaining branches."
> 즉, 하나의 H2P chain이 완료/실패하더라도 다른 active chain은 계속 실행해야 한다.
> 이를 위해 TEA Op에 chain 소속 정보를 부여하고, per-chain 선택적 정리를 구현한다.

**선행 조건**: `TEA_op_manage_status.md`의 현재 구현이 모두 정상 동작하는 상태에서 진행.

---

## 1. `Op` 구조체에 `h2p_chain_id` 필드 추가

### 1.1 목적

현재 TEA Op은 `thread_id == 1`로만 식별되며, 어느 H2P chain에 속하는지 구분할 수 없다.
다중 H2P 시 특정 chain만 선택적으로 flush하려면 chain 소속 정보가 필요하다.

### 1.2 변경 내용

**파일**: `src/op.h` — `Op_struct` 내부 (`op.h:135` `thread_id` 근처)

```c
// op.h — Op 구조체에 추가 (thread_id 바로 아래)
uns proc_id;                  // processor id for cmp model
uns thread_id;                // id number for the thread to which this op belongs
uns8 h2p_chain_id;            // 0 = main op, 1~MAX_TEA_CHAINS = TEA chain ID
```

**규약**:
- `h2p_chain_id == 0`: Main thread op (기본값, `alloc_op()`에서 0 초기화)
- `h2p_chain_id == N` (1-based): TEA chain slot `N-1`에 속하는 TEA op

### 1.3 할당 시점

**파일**: `src/tea/tea_fetch_stage.c` — `tea_create_op_from_cache()` (`tea_fetch_stage.c:189`)

```c
tea_op->thread_id = 1;
tea_op->h2p_chain_id = current_chain_slot + 1;  // 1-based ID
```

`current_chain_slot`은 현재 fetch 중인 chain의 `Tea_Thread.chains[]` 인덱스.
(단일 H2P에서는 항상 0이므로 `h2p_chain_id = 1`)

---

## 2. `flush_tea_ops_by_chain_id()` — 선택적 Flush

### 2.1 목적

`flush_tea_ops_from_node_stage()` (`node_stage.c:1005-1094`)는 `thread_id == 1` 조건으로
**모든** TEA op을 일괄 제거한다. 다중 H2P에서는 특정 chain의 ops만 제거하고
나머지 chain의 ops는 유지해야 한다.

### 2.2 설계

기존 `flush_tea_ops_from_node_stage()`와 동일한 5단계 구조이나,
조건을 `thread_id == 1`에서 `op->h2p_chain_id == target_chain_id`로 변경.

**파일**: `src/node_stage.c`

```c
void flush_tea_ops_by_chain_id(uns proc_id, uns8 chain_id) {
  extern Cmp_Model cmp_model;
  Node_Stage* node_local = &cmp_model.node_stage[proc_id];
  Op* op;
  Op** last;

  /* 0a. exec_stage에서 해당 chain의 ops 제거 */
  Exec_Stage* exec_local = &cmp_model.exec_stage[proc_id];
  for (uns ii = 0; ii < exec_local->sd.max_op_count; ii++) {
    op = exec_local->sd.ops[ii];
    if (op && op->h2p_chain_id == chain_id) {
      exec_local->sd.ops[ii] = NULL;
      exec_local->sd.op_count--;
    }
  }

  /* 0b. dcache_stage에서 해당 chain의 ops 제거 */
  Dcache_Stage* dc_local = &cmp_model.dcache_stage[proc_id];
  for (uns ii = 0; ii < dc_local->sd.max_op_count; ii++) {
    op = dc_local->sd.ops[ii];
    if (op && op->h2p_chain_id == chain_id) {
      dc_local->sd.ops[ii] = NULL;
      dc_local->sd.op_count--;
    }
  }

  /* 1. ready list에서 해당 chain의 ops 제거
   *    OS_SCHEDULED/OS_MISS ops가 ready list에 있으면 = clear()가 아직 처리 안 한 것.
   *    ready list에서 제거하면 clear()가 이후 찾지 못하므로 여기서 RS 카운터도 감소.
   *    (TEA_dispatch_plan.md §10.3 참조) */
  for (op = node_local->rdy_head, last = &node_local->rdy_head; op;) {
    if (op->h2p_chain_id == chain_id) {
      *last = op->next_rdy;
      op->in_rdy_list = FALSE;

      /* RS counter sync for OS_SCHEDULED/OS_MISS:
       * Normally clear() does this, but we removed op from ready list
       * so clear() will never see it. Must decrement here. */
      if (op->state == OS_SCHEDULED || op->state == OS_MISS) {
        if (node_local->rs[op->rs_id].rs_op_count > 0)
          node_local->rs[op->rs_id].rs_op_count--;
        if (node_local->rs[op->rs_id].tea_op_count > 0)
          node_local->rs[op->rs_id].tea_op_count--;
      }

      op = op->next_rdy;
    } else {
      last = &op->next_rdy;
      op = op->next_rdy;
    }
  }

  /* 2. scheduling buffer에서 해당 chain의 ops 제거 */
  for (uns ii = 0; ii < node_local->sd.max_op_count; ii++) {
    op = node_local->sd.ops[ii];
    if (op && op->h2p_chain_id == chain_id) {
      node_local->sd.ops[ii] = NULL;
      node_local->sd.op_count--;
    }
  }

  /* 3. next_op_into_rs: 해당 chain op을 가리키면 다음 non-target op으로 전진
   * (Bug Fix 8.3 반영: NULL 대신 전진하여 Main/다른 chain ops dispatch 유지)
   * NOTE: 독립 dispatch(TEA_dispatch_plan.md) 구현 후 TEA ops는 next_op_into_rs에
   * 영향을 주지 않으므로 이 step은 사실상 no-op이 됨. 안전을 위해 유지. */
  if (node_local->next_op_into_rs &&
      node_local->next_op_into_rs->h2p_chain_id == chain_id) {
    Op* next = node_local->next_op_into_rs->next_node;
    while (next && next->h2p_chain_id == chain_id)
      next = next->next_node;
    node_local->next_op_into_rs = next;
  }

  /* 4. node table에서 해당 chain의 ops 제거 + free */
  node_local->node_tail = NULL;
  for (op = node_local->node_head, last = &node_local->node_head; op;) {
    if (op->h2p_chain_id == chain_id) {
      *last = op->next_node;
      op->in_node_list = FALSE;

      /* RS 카운터 감소: pre-scheduling 상태에서만 (whitelist).
       * 현재 flush_tea_ops_from_node_stage() (node_stage.c:1224-1226)와 동일.
       * OS_SCHEDULED/OS_MISS는 scheduling 시 clear()가 이미 감소했고,
       * OS_WAIT_DCACHE/OS_WAIT_MEM/OS_DONE 등 post-scheduling 상태도
       * clear()가 이미 처리했으므로 decrement 불필요 (double-decrement → underflow 방지). */
      if (op->state == OS_IN_RS || op->state == OS_READY ||
          op->state == OS_WAIT_FWD || op->state == OS_SLEEP ||
          op->state == OS_LOW_PRIORITY || op->state == OS_TENTATIVE) {
        if (node_local->rs[op->rs_id].rs_op_count > 0)
          node_local->rs[op->rs_id].rs_op_count--;
        if (node_local->rs[op->rs_id].tea_op_count > 0)
          node_local->rs[op->rs_id].tea_op_count--;
      }

      // dependent TEA ops의 not-rdy bit 강제 clear (TEA_reg_dependency_plan.md §9.1)
      Wake_Up_Entry* wake = op->wake_up_head;
      while (wake) {
        Op* dep_op = wake->op;
        if (dep_op->op_pool_valid && dep_op->unique_num == wake->unique_num) {
          clear_not_rdy_bit(dep_op, wake->rdy_bit);
          if (dep_op->state == OS_IN_RS &&
              dep_op->srcs_not_rdy_vector == 0x0 &&
              !dep_op->in_rdy_list) {
            dep_op->next_rdy = node_local->rdy_head;
            node_local->rdy_head = dep_op;
            dep_op->in_rdy_list = TRUE;
          }
        }
        wake = wake->next;
      }

      Op* next = op->next_node;
      free_op(op);
      STAT_EVENT(proc_id, TEA_OPS_FLUSHED);
      op = next;
    } else {
      last = &op->next_node;
      node_local->node_tail = op;
      op = op->next_node;
    }
  }
}
```

### 2.3 `flush_tea_ops_from_node_stage()`와의 관계

- 단일 H2P: 기존 `flush_tea_ops_from_node_stage()` 그대로 사용 (성능상 유리)
- 다중 H2P: 개별 chain 종료 시 `flush_tea_ops_by_chain_id()` 사용
- `flush_tea_ops_from_node_stage()`는 모든 chain이 종료될 때 fallback으로 유지 가능

---

## 3. `terminate_tea_chain()` — 개별 Chain 종료

### 3.1 목적

현재 `terminate_tea_thread()` (`tea_thread.c:154-185`)는 전체 TEA 상태를 일괄 리셋한다.
다중 H2P에서는 개별 chain만 종료하고, 남은 chain은 계속 실행해야 한다.

### 3.2 Tea_Thread 구조체 확장

**파일**: `src/tea/tea_thread.h`

```c
#define MAX_TEA_CHAINS 4  /* 논문: 동시 active H2P chain 수 제한 */

typedef enum Chain_State_enum {
  CHAIN_INACTIVE,   /* 미사용 */
  CHAIN_FETCHING,   /* Block Cache에서 fetch 중 */
  CHAIN_EXECUTING,  /* 파이프라인에서 실행 중 */
} Chain_State;

typedef struct Tea_H2P_Chain_struct {
  Chain_State state;
  Addr target_h2p_pc;
  Counter target_h2p_op_num;
  Op* main_h2p_op;
  Op_Info h2p_oracle_info;
  Recovery_Info h2p_recovery_info;
  uns tea_op_count;         /* 이 chain의 파이프라인 내 TEA op 수 */
  Counter tea_ops_fetched;  /* 이 chain에서 fetch된 총 TEA op 수 */
} Tea_H2P_Chain;

typedef struct Tea_Thread_struct {
  /* ... 기존 필드 ... */
  Tea_H2P_Chain chains[MAX_TEA_CHAINS];
  uns num_active_chains;
  int current_fetch_chain;  /* 현재 fetch 중인 chain slot (-1 = none) */
} Tea_Thread;
```

### 3.3 구현

**파일**: `src/tea/tea_thread.c`

```c
void terminate_tea_chain(uns proc_id, uns chain_slot) {
  Tea_Thread* tea = tea_threads[proc_id];
  Tea_H2P_Chain* c = &tea->chains[chain_slot];
  uns8 chain_id = chain_slot + 1;  // 1-based h2p_chain_id

  /* 1. fetch 중인 chain이면 fetch stage에서 해당 chain ops 제거 */
  if (c->state == CHAIN_FETCHING &&
      tea->current_fetch_chain == (int)chain_slot) {
    recover_tea_fetch_stage_by_chain(proc_id, chain_id);
  }

  /* 2. rename stage에서 해당 chain ops 제거 */
  recover_tea_rename_stage_by_chain(proc_id, chain_id);

  /* 3. node stage (exec/dcache/RS/node table)에서 해당 chain ops 선택적 flush */
  flush_tea_ops_by_chain_id(proc_id, chain_id);

  /* 4. store buffer에서 해당 chain 엔트리 무효화 */
  tea_store_buffer_clear_by_chain_id(proc_id, chain_id);

  /* 5. chain 상태 초기화 */
  c->state = CHAIN_INACTIVE;
  c->target_h2p_pc = 0;
  c->target_h2p_op_num = 0;
  c->main_h2p_op = NULL;
  c->tea_op_count = 0;
  c->tea_ops_fetched = 0;
  tea->num_active_chains--;

  /* 6. 모든 chain 종료 시 전체 TEA 자원 정리 */
  if (tea->num_active_chains == 0) {
    reset_tea_preg_pool(proc_id);       // 모든 TEA preg 반환
    reset_tea_store_buffer(proc_id);    // 전체 store buffer 리셋
    recover_tea_rename_stage(proc_id);  // Shadow RAT 무효화
    tea->state = TEA_IDLE;
  }

  STAT_EVENT(proc_id, TEA_CHAIN_TERMINATED);
}
```

### 3.4 필요한 신규 헬퍼 함수

| 함수 | 파일 | 역할 |
|------|------|------|
| `recover_tea_fetch_stage_by_chain(proc_id, chain_id)` | `tea_fetch_stage.c` | fetch stage의 `sd.ops[]` 중 `h2p_chain_id == chain_id`인 것만 `free_op()` |
| `recover_tea_rename_stage_by_chain(proc_id, chain_id)` | `tea_rename.c` | rename stage의 `sd.ops[]` 중 `h2p_chain_id == chain_id`인 것만 `free_op()` |
| `tea_store_buffer_clear_by_chain_id(proc_id, chain_id)` | `tea_store_buffer.c` | store buffer entries 중 chain_id 매치하는 것 무효화 (store buffer entry에 chain_id 필드 추가 필요) |

---

## 4. Per-Chain `tea_op_count` 추적 (방안 A)

### 4.1 현재 동작 및 문제점

**현재 단일 H2P 동작**:
- `tea_op_completed()` (`tea_thread.c:251-265`): `op->state = OS_DONE` 설정 + `STAT_EVENT`만 수행. **카운터 감소 없음**.
- `node_retire_tea_ops()` (`node_stage.c:692-705`): op을 node table에서 물리적으로 제거하는 시점에 `tea->tea_op_count--` 수행 — **유일한 감소 지점**.

**이 설계의 이유 (0-latency op 안전성)**:
`op_latency == 0`인 op의 경우, `exec_stage`가 같은 cycle에 `tea_op_completed()`를 호출하면서 `exec_stage_clear_fu()`는 다음 cycle로 예약한다. 그 사이 `node_retire_tea_ops()`가 해당 op을 `free_op()`하면 `clear_fu()`가 접근하는 op 포인터는 use-after-free가 된다. 따라서 **파이프라인에서 완전히 떠난 시점(=retire)**이 유일하게 안전한 감소 지점이다.

**다중 H2P에서의 문제**:
- `TEA_multi_h2p_plan.md:120`이 전역 `tea->tea_op_count` 필드를 삭제하고 per-chain `chains[i].tea_op_count`로 이동시킴.
- 따라서 `node_retire_tea_ops()`의 기존 전역 감소 코드는 **컴파일 불가** — per-chain 감소로 변경해야 함.
- 감소 지점은 **그대로 `node_retire_tea_ops()` 유지** (0-latency op 안전성 불변). `tea_op_completed()`는 변경하지 않음.

### 4.2 `tea_op_completed()` — 변경 없음

**파일**: `src/tea/tea_thread.c` (변경하지 않음)

현재 동작 그대로 유지. `op->state = OS_DONE` 설정과 `STAT_EVENT`만 수행하고 카운터는 건드리지 않는다.

```c
void tea_op_completed(uns proc_id, Op* op) {
  /* tea_op_count 감소는 이 함수에서 하지 않음. node_retire_tea_ops()가 담당.
   * 0-latency op의 use-after-free 방지 설계 (§4.1 참조). */
  op->state = OS_DONE;
  STAT_EVENT(proc_id, TEA_OPS_EXECUTED);
}
```

> **주의**: `tea_op_completed()`에 per-chain `c->tea_op_count--`를 추가하면 `node_retire_tea_ops()`의 감소와 겹쳐 **이중 감소** 발생. 반드시 둘 중 하나에서만 감소시킬 것.

### 4.3 `node_retire_tea_ops()` — Per-Chain 감소로 변경

**파일**: `src/node_stage.c` (`node_retire_tea_ops()` 내부, 기존 전역 감소 블록 교체)

**기존 코드** (`node_stage.c:692-705`):
```c
if (tea_threads && tea_threads[node->proc_id]) {
  Tea_Thread* tea_state = tea_threads[node->proc_id];
  if (tea_state->tea_op_count > 0)
    tea_state->tea_op_count--;
}
```

**수정 후 (방안 A)**:
```c
/* Per-chain tea_op_count 감소 — free_op() 직전.
 * 여기가 감소 지점인 이유: 0-latency op가 같은 cycle에 exec → clear_fu를
 * 거치는 동안 op 포인터 유효성을 보장하기 위해, 물리적으로 node table에서
 * 제거되는 이 시점까지 카운터를 유지해야 함 (§4.1 참조). */
if (tea_threads && tea_threads[node->proc_id]) {
  Tea_Thread* tea_state = tea_threads[node->proc_id];
  int slot = op->h2p_chain_id - 1;  // 1-based → 0-based
  if (slot >= 0 && slot < MAX_TEA_CHAINS &&
      tea_state->chains[slot].tea_op_count > 0) {
    tea_state->chains[slot].tea_op_count--;
  }
}
```

**종료 판단**: `update_tea_thread()`의 per-chain 루프가 `c->tea_op_count == 0 && c->tea_ops_fetched > 0 && c->state == CHAIN_EXECUTING` 조건으로 chain 종료를 감지 (`TEA_multi_h2p_plan.md §3.5`).

```c
// update_tea_thread() — per-chain 종료 판단
void update_tea_thread(uns proc_id) {
  Tea_Thread* tea = tea_threads[proc_id];
  if (tea->num_active_chains == 0) return;

  for (uns i = 0; i < MAX_TEA_CHAINS; i++) {
    Tea_H2P_Chain* c = &tea->chains[i];
    if (c->state == CHAIN_EXECUTING &&
        c->tea_op_count == 0 && c->tea_ops_fetched > 0) {
      terminate_tea_chain(proc_id, i);
    }
  }
}
```

---

## 5. Per-Chain Preg 관리 전략

### 5.1 결론: Per-chain preg 반환은 **구현하지 않음**

### 5.2 이유

논문 IV-E: "Freeing Physical Registers — The TEA thread only maintains a speculative RAT.
Its instructions free up backend resources as soon as possible to avoid the in-order retirement
bottleneck."

그러나 per-chain preg 반환은 아래 이유로 안전하지 않다:

1. TEA preg pool은 전체 TEA 스레드가 **공유**하는 자원
2. Chain A가 R5에 preg #200을 할당 → Shadow RAT에 R5→#200 매핑 생성
3. Chain A 종료 시 preg #200을 반환하면, Chain B가 R5를 읽을 때 **dangling 매핑** 참조
4. Shadow RAT의 일관성을 보장하려면 모든 chain 종료 시 일괄 `reset_tea_preg_pool()` 호출이 안전

### 5.3 Preg Pool 고갈 대응

- preg pool이 고갈되면 `tea_preg_pool_available()` (`tea_rename.c:367`)이 FALSE 반환
- 새 chain의 fetch/rename이 stall됨
- `TEA_PREGS_ALLOCATED` / `TEA_RENAME_STALL_PREG` stat으로 모니터링
- 극단적 경우: 가장 오래된(또는 가장 적게 진행된) chain을 강제 terminate하여 preg 해제

---

## 5bis. Dispatch 정책 참고사항 (2026-03-17 업데이트)

> **⚠️ 선행 작업**: `TEA_dispatch_plan.md`의 독립 dispatch 구현이 먼저 완료되어야 함.
> 독립 dispatch에서 TEA ops는 `tea_dispatch_to_rs()`에서 직접 RS에 dispatch되므로,
> 다중 H2P에서도 `node_issue_queue_dispatch()`와의 interleaving 문제가 발생하지 않음.

다중 H2P에서의 추가 고려 사항:

1. **RS 파티셔닝 비균등 분배**: 현재 `TEA_RS_RESERVATION/NUM_RS` 균등 분배이나,
   TEA ops는 주로 ALU 연산 → RS[0] (184 entries, FU 0,1,4,5,8) 집중.
   RS[1] (132, FU 2,3,9), RS[2] (36, FU 6,7)의 TEA 슬롯은 미사용될 가능성 높음.
   다중 H2P에서 TEA op 수가 증가하면 RS[0] 병목 가능 → 향후 비균등 분배 검토 필요.

2. **다중 chain의 RS 공유**: 독립 dispatch 후 모든 TEA chain이 동일한 `tea_rs_limit`을 공유.
   chain 간 RS 공정 분배가 필요하면 per-chain RS 카운터 추가 검토.

---

## 6. 수정 파일 요약

| 파일 | 변경 내용 |
|------|----------|
| `src/op.h` | `uns8 h2p_chain_id` 필드 추가 (Op_struct) |
| `src/tea/tea_thread.h` | `Tea_H2P_Chain` 구조체 추가, `Tea_Thread`에 `chains[]`/`num_active_chains` 필드 |
| `src/tea/tea_thread.c` | `terminate_tea_chain()` 추가, `tea_op_completed()` per-chain 카운터, `update_tea_thread()` per-chain 루프 |
| `src/tea/tea_fetch_stage.c` | op 생성 시 `h2p_chain_id` 할당, `recover_tea_fetch_stage_by_chain()` 추가 |
| `src/tea/tea_rename.c` | `recover_tea_rename_stage_by_chain()` 추가 |
| `src/tea/tea_store_buffer.h/c` | entry에 `chain_id` 필드, `tea_store_buffer_clear_by_chain_id()` 추가 |
| `src/node_stage.c` | `flush_tea_ops_by_chain_id()` 추가, `node_retire_tea_ops()`의 `tea_op_count--`를 per-chain `chains[slot].tea_op_count--`로 변경 |
| `src/node_stage.h` | `flush_tea_ops_by_chain_id()` 프로토타입 추가 |
| `src/tea/tea.stat.def` | `TEA_CHAIN_TERMINATED` stat 추가 |

---

## 7. 구현 순서

1. **`op.h`에 `h2p_chain_id` 추가** — 가장 기본, 모든 후속 작업의 전제
2. **`Tea_Thread` 구조체 확장** — `chains[]`, `num_active_chains`, `Chain_State`
3. **`tea_fetch_stage.c` 수정** — `h2p_chain_id` 할당 + `recover_by_chain`
4. **`node_stage.c`에 `flush_tea_ops_by_chain_id()` 추가**
5. **`node_retire_tea_ops()`의 `tea_op_count--`를 per-chain `chains[slot].tea_op_count--`로 변경** — `tea_op_completed()`는 변경하지 않음 (0-latency op 안전성)
6. **`terminate_tea_chain()` 구현**
7. **`update_tea_thread()` per-chain 루프 추가**
8. **Store buffer chain_id 지원**
9. **`trigger_tea_thread()` 수정** — 새 chain을 빈 slot에 할당

**단계 5는 단일 H2P 상태에서도 먼저 적용 가능** — `op->h2p_chain_id`가 항상 1이므로 `chains[0].tea_op_count`만 감소시키면 기존 전역 카운터와 동일한 동작. `tea_op_completed()`는 절대 카운터를 감소시키지 말 것 (이중 감소 방지).
