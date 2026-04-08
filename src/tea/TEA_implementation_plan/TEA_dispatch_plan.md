# TEA 독립 Dispatch 구현 계획

> **설계 목표**: 논문 Section IV-D — "After renaming, TEA thread instructions are sent to the Issue logic
> which picks between the two 8-wide Rename output stages. The Issue logic is 8-wide but prioritizes TEA
> thread instructions and uses the leftover Issue slots for the main thread."
>
> TEA ops와 Main ops의 dispatch를 독립적으로 수행하여, 단일 `next_op_into_rs` 순회에서 발생하는
> 인위적 순서 의존성과 관련 ASSERT를 근본적으로 해결한다.

**선행 조건**: `TEA_op_manage_status.md` §3.3 및 §8.4의 분석 완료.

---

## 1. 현재 구현의 문제점 요약

### 1.1 현재 흐름

```
update_node_stage():
  (1) tea_dispatch_to_rs()         → TEA ops를 Node Table tail에 삽입 (state = OS_IN_ROB)
  (2) node_fill_rob()              → Main ops를 Node Table tail에 삽입 (state = OS_IN_ROB)
  (3) node_issue_queue_update():
      - node_issue_queue_clear()   → ready list에서 OS_SCHEDULED/OS_MISS 처리
      - node_issue_queue_dispatch()→ next_op_into_rs부터 순회: OS_IN_ROB → OS_IN_RS
      - node_issue_queue_schedule()→ ready list → FU 배정 (TEA 우선 2-pass ✅)
```

`node_issue_queue_dispatch()`는 Node Table의 linked list를 `next_op_into_rs`부터 순차 순회하여
TEA/Main ops를 구분 없이 dispatch한다. TEA op이 RS partition full로 skip되면
`first_undispatched`가 해당 TEA op을 기억하고, 다음 cycle에 이미 dispatch된 Main ops를 재방문.

### 1.2 ASSERT 발생 메커니즘

```
Cycle N:
  Node Table: ... → [TEA_A: OS_IN_ROB] → [Main_B: OS_IN_ROB] → [Main_C: OS_IN_ROB]
                          ↑ next_op_into_rs
  Dispatch: TEA_A(skip) → Main_B(→OS_IN_RS) → Main_C(→OS_IN_RS)
  next_op_into_rs = first_undispatched = TEA_A  ← 문제

Cycle N+1:
  TEA_A(skip) → Main_B(OS_IN_RS) → ASSERT(state == OS_IN_ROB) 실패!
```

### 1.3 논문과의 불일치

| 항목 | 논문 | 현재 구현 |
|------|------|----------|
| Dispatch 스트림 | 2개 독립 (TEA Rename, Main Rename) | 1개 공유 (next_op_into_rs) |
| TEA↔Main 순서 의존성 | 없음 | TEA skip → Main 재방문 |
| ROB 진입 | TEA ops는 ROB 미진입 | TEA ops도 Node Table 진입 |
| Issue 로직 | 2개 Rename 출력에서 선택, TEA 우선 | 순차 순회 후 schedule에서만 TEA 우선 |

---

## 2. 설계: `tea_dispatch_to_rs()`에서 직접 RS dispatch

### 2.1 핵심 아이디어

`tea_dispatch_to_rs()`가 TEA ops를 Node Table에 삽입할 때 **동시에 RS에도 dispatch** (상태를
`OS_IN_RS`로 전이, `rs_op_count`/`tea_op_count` 증가, 소스 ready 시 ready list 등록).

이렇게 하면:
- TEA ops는 `node_issue_queue_dispatch()`의 `next_op_into_rs` 순회 대상이 아님
- `next_op_into_rs`는 Main ops만 추적
- TEA↔Main 간 순서 의존성 완전 제거
- 기존 scheduling(TEA 우선 2-pass)과 wakeup 로직은 변경 없이 동작

### 2.2 TEA ops가 Node Table에 남아야 하는 이유

논문에서 TEA ops가 ROB에 들어가지 않는다고 했지만, Scarab에서 Node Table은 ROB 이상의 역할:
- **Wakeup**: `wake_up_ops()`가 Node Table의 ops를 대상으로 소스 ready 신호 전파
- **Ready list**: RS에 있는 ops가 소스 ready 시 ready list에 등록 → scheduling 대상
- **Flush**: `flush_tea_ops_from_node_stage()`가 Node Table을 순회하여 TEA ops 제거
- **State tracking**: `OP_DONE()` 체크, `OS_DONE` 전이 등

따라서 TEA ops는 Node Table에 유지하되, **dispatch만 독립적으로 수행**.

### 2.3 `next_op_into_rs`에서 TEA ops 제외

TEA ops가 이미 RS에 dispatch된 상태(OS_IN_RS 등)로 Node Table에 존재하므로,
`node_issue_queue_dispatch()`가 이들을 만나도 자연스럽게 건너뛴다.
그러나 `next_op_into_rs`가 TEA op을 가리키면 불필요한 순회가 발생하므로,
`tea_dispatch_to_rs()`에서 `next_op_into_rs`를 **TEA op으로 설정하지 않도록** 해야 한다.

---

## 3. 구현 상세

### 3.1 `tea_dispatch_to_rs()` 수정 (`node_stage.c:401-457`)

**현재**:
```c
static void tea_dispatch_to_rs(Stage_Data* tea_sd) {
  for (uns i = 0; i < tea_sd->max_op_count; i++) {
    Op* op = tea_sd->ops[i];
    if (!op) continue;
    // ... Node Table에 삽입 ...
    op->state = OS_IN_ROB;  // ← node_issue_queue_dispatch()에서 OS_IN_RS로 전이
  }
}
```

**변경 후**:
```c
static void tea_dispatch_to_rs(Stage_Data* tea_sd) {
  if (!tea_sd || tea_sd->op_count == 0) return;

  for (uns i = 0; i < tea_sd->max_op_count; i++) {
    Op* op = tea_sd->ops[i];
    if (!op) continue;

    ASSERT(node->proc_id, op->thread_id == 1);
    ASSERT(node->proc_id, op->proc_id == node->proc_id);

    /* Check Node Table capacity */
    if (is_node_table_full()) break;

    /* Remove from TEA rename stage */
    tea_sd->ops[i] = NULL;
    tea_sd->op_count--;

    /* Set op fields */
    op->node_id = node->node_count;
    op->issue_cycle = cycle_count;

    /* Add to Node Table linked list */
    ASSERT(node->proc_id, !op->in_node_list);
    if (node->node_tail)
      node->node_tail->next_node = op;
    if (node->node_head == NULL)
      node->node_head = op;
    op->next_node = NULL;
    op->in_node_list = TRUE;
    node->node_tail = op;

    /* ★ 핵심 변경: next_op_into_rs를 TEA op으로 설정하지 않음 */
    /* next_op_into_rs는 Main ops 전용 포인터로 유지 */
    /* (기존: if (!node->next_op_into_rs) node->next_op_into_rs = op;) */

    /* ★ 핵심 변경: 직접 RS dispatch (node_issue_queue_dispatch 우회) */
    int64 rs_id = node_dispatch_find_emptiest_rs(op);
    if (rs_id != NODE_ISSUE_QUEUE_RS_SLOT_INVALID) {
      Reservation_Station* rs = &node->rs[rs_id];
      op->state = OS_IN_RS;
      op->rs_id = (Counter)rs_id;
      rs->rs_op_count++;
      rs->tea_op_count++;

      /* Ready list 등록 (소스 모두 ready인 경우) */
      if (op->srcs_not_rdy_vector == 0) {
        op->state = (cycle_count + 1 >= op->rdy_cycle ? OS_READY : OS_WAIT_FWD);
        op->next_rdy = node->rdy_head;
        node->rdy_head = op;
        op->in_rdy_list = TRUE;
      }
    } else {
      /* TEA RS partition full → Node Table에는 남아있되, OS_IN_ROB 유지.
       * 다음 cycle의 tea_dispatch_retry()에서 재시도 (§3.3 참조) */
      op->state = OS_IN_ROB;
      STAT_EVENT(node->proc_id, TEA_RS_STALLS);
    }

    STAT_EVENT(node->proc_id, TEA_OPS_DISPATCHED);
  }
}
```

### 3.2 `node_issue_queue_dispatch()` 수정 (`node_issue_queue.cc:284-344`)

TEA skip 로직과 `first_undispatched` 포인터 **전체 제거**. Main ops만 순회하는 원래 구조로 복원.

**변경 후**:
```c
void node_issue_queue_dispatch() {
  Op* op = NULL;
  uns32 num_fill_rs = 0;

  for (op = node->next_op_into_rs; op; op = op->next_node) {
    /* TEA ops는 tea_dispatch_to_rs()에서 이미 RS에 직접 dispatch됨.
     * Node Table에는 남아있지만 state가 OS_IN_ROB이 아닌 상태 → 건너뜀.
     * 단, RS dispatch 실패한 TEA ops (state == OS_IN_ROB)도 여기서 skip —
     * tea_dispatch_retry()에서 처리. */
    if (op->thread_id == 1) {
      continue;
    }

    int64 rs_id = dispatch_func_table[NODE_ISSUE_QUEUE_DISPATCH_SCHEME](op);
    if (rs_id == NODE_ISSUE_QUEUE_RS_SLOT_INVALID)
      break;
    ASSERT(node->proc_id, rs_id >= 0 && rs_id < NUM_RS);

    Reservation_Station* rs = &node->rs[rs_id];
    ASSERTM(node->proc_id, !rs->size || rs->rs_op_count < rs->size,
            "There must be at least one free space in selected RS!\n");

    ASSERT(node->proc_id, op->state == OS_IN_ROB);
    op->state = OS_IN_RS;
    op->rs_id = (Counter)rs_id;
    rs->rs_op_count++;
    rs->main_op_count++;

    num_fill_rs++;
    // ... ready list 등록, RS_FILL_WIDTH 체크 등 기존 로직 유지 ...
  }

  node->next_op_into_rs = op;
}
```

**핵심 변경점**:
- `first_undispatched` 완전 제거
- TEA ops는 `thread_id == 1 → continue`로 즉시 skip
- Main ops만 in-order dispatch, `break` 시맨틱 유지
- `next_op_into_rs = op`으로 단순화 (기존 Scarab 원본과 동일)
- per-thread 카운터: Main ops는 항상 `main_op_count++`, TEA 분기 불필요

### 3.3 `tea_dispatch_retry()` — RS 실패한 TEA ops 재시도 (신규)

`tea_dispatch_to_rs()`에서 RS partition full로 OS_IN_ROB 상태로 남은 TEA ops를 매 cycle 재시도.

**파일**: `src/node_stage.c`

```c
static void tea_dispatch_retry() {
  /* Node Table에서 OS_IN_ROB 상태의 TEA ops를 찾아 RS dispatch 재시도 */
  for (Op* op = node->node_head; op; op = op->next_node) {
    if (op->thread_id != 1 || op->state != OS_IN_ROB)
      continue;

    int64 rs_id = node_dispatch_find_emptiest_rs(op);
    if (rs_id == NODE_ISSUE_QUEUE_RS_SLOT_INVALID) {
      STAT_EVENT(node->proc_id, TEA_RS_STALLS);
      continue;  /* 여전히 full → 다음 TEA op 시도 */
    }

    Reservation_Station* rs = &node->rs[rs_id];
    op->state = OS_IN_RS;
    op->rs_id = (Counter)rs_id;
    rs->rs_op_count++;
    rs->tea_op_count++;

    if (op->srcs_not_rdy_vector == 0) {
      op->state = (cycle_count + 1 >= op->rdy_cycle ? OS_READY : OS_WAIT_FWD);
      op->next_rdy = node->rdy_head;
      node->rdy_head = op;
      op->in_rdy_list = TRUE;
    }
  }
}
```

**호출 위치**: `update_node_stage()` 내부, `node_issue_queue_update()` **직후** (clear()가 해제한 RS 슬롯을 활용):
```c
void update_node_stage(Stage_Data* src_sd) {
  // ... OP_DONE 상태 업데이트 ...

  if (TEA_ENABLE && tea_is_active(node->proc_id)) {
    tea_dispatch_to_rs(&tea_rename_stages[node->proc_id]->sd);
  }

  node_fill_rob(src_sd);
  node_precommit_update();
  node_issue_queue_update();

  /* TEA RS dispatch 재시도: 이전 cycle에서 RS full로 실패한 TEA ops */
  if (TEA_ENABLE && tea_is_active(node->proc_id)) {
    tea_dispatch_retry();
  }

  node_retire();
}
```

**설계 근거**:
- `node_issue_queue_clear()`가 먼저 실행되어 RS 공간을 확보한 후 재시도
- Main dispatch도 완료된 후이므로 RS 가용 상태가 정확

### 3.4 `flush_tea_ops_from_node_stage()` step 3 수정 (`node_stage.c:1058-1067`)

`next_op_into_rs`가 TEA op을 가리키는 상황은 독립 dispatch 이후에도 발생 가능.
(Node Table에서 TEA ops를 제거할 때 `next_op_into_rs`가 TEA op을 가리키면 전진 필요)

현재 로직은 이미 올바르지만, 코멘트를 업데이트:
```c
/* 3. next_op_into_rs: TEA op을 가리키면 다음 non-TEA op으로 전진.
 *    독립 dispatch 후에도 next_op_into_rs가 TEA op 사이에 끼일 수 있음
 *    (TEA ops가 Node Table에는 남아있으므로). */
```

### 3.5 `flush_tea_ops_from_node_stage()` step 4 RS 조건

현재 코드는 **v2 조건** (`state != OS_IN_ROB && != OS_SCHEDULED && != OS_MISS && != OS_DONE`)이며,
§10.3의 flush step 1 RS 카운터 동기화가 적용되면 OS_SCHEDULED/OS_MISS는 step 1에서 처리되므로
step 4에서 제외하는 v2 조건이 올바름.

독립 dispatch 이후에도 동일하게 적용:
- 독립 dispatch로 RS에 진입한 TEA ops (OS_IN_RS, OS_READY 등) → step 4에서 감소
- RS dispatch 실패하여 OS_IN_ROB인 TEA ops → 감소 불필요
- OS_SCHEDULED/OS_MISS → step 1에서 처리됨 → step 4에서 제외
- 변경 없음

---

## 4. `node_dispatch_find_emptiest_rs()` 가시성 (`node_issue_queue.cc`)

현재 `node_dispatch_find_emptiest_rs()`는 `node_issue_queue.cc` 내부 함수.
`tea_dispatch_to_rs()` (`node_stage.c`)에서 호출하려면 외부 가시성 필요.

**방안 A (권장)**: `node_issue_queue.h`에 프로토타입 추가
```c
// node_issue_queue.h
int64 node_dispatch_find_emptiest_rs(Op* op);
```

**방안 B**: `tea_dispatch_to_rs()`를 `node_issue_queue.cc`로 이동.
→ 구조적으로 부자연스러움 (TEA 코드가 node_issue_queue에 종속).

---

## 5. `node_issue_queue_dispatch()`에서 per-thread 카운터 분리

현재 코드는 dispatch 성공 시 `op->thread_id`를 확인하여 `tea_op_count++` 또는 `main_op_count++`를 분기.
독립 dispatch 후에는 `node_issue_queue_dispatch()`에서 Main ops만 처리하므로:

```c
// 변경 전:
if (op->thread_id == 1) {
  rs->tea_op_count++;
} else {
  rs->main_op_count++;
}

// 변경 후 (Main ops only):
rs->main_op_count++;
```

---

## 6. 다중 H2P와의 호환성

`TEA_op_manage_plan.md` §5bis에서 언급한 dispatch 정책은 독립 dispatch로 자연스럽게 해결:
- chain A, B 모두 `tea_dispatch_to_rs()`에서 독립적으로 RS dispatch
- RS partition은 전체 TEA(모든 chain 합산)에 대해 `tea_rs_limit` 적용
- `flush_tea_ops_by_chain_id()`의 step 3 (`next_op_into_rs` 전진)도 동일하게 동작

---

## 7. 수정 파일 요약

| 파일 | 변경 내용 |
|------|----------|
| `src/node_stage.c` | `tea_dispatch_to_rs()`: 직접 RS dispatch 추가. `tea_dispatch_retry()` 신규. |
| `src/node_issue_queue.cc` | `node_issue_queue_dispatch()`: `first_undispatched` 제거, TEA ops skip, Main-only. |
| `src/node_issue_queue.h` | `node_dispatch_find_emptiest_rs()` 프로토타입 외부 노출 |

---

## 8. 구현 순서

```
(1) node_issue_queue.h에 node_dispatch_find_emptiest_rs() 프로토타입 추가

(2) tea_dispatch_to_rs() 수정:
    - next_op_into_rs 설정 코드 제거 (TEA ops 제외)
    - 직접 RS dispatch 로직 추가 (find_emptiest_rs → state 전이 → ready list)
    - RS 실패 시 OS_IN_ROB 유지

(3) node_issue_queue_dispatch() 수정:
    - first_undispatched 변수 및 관련 로직 전체 제거
    - thread_id == 1 → continue 추가 (TEA ops skip)
    - per-thread 카운터: Main-only (main_op_count++ 만)
    - next_op_into_rs = op (원래 Scarab 단순 로직으로 복원)

(4) tea_dispatch_retry() 신규 추가:
    - update_node_stage()에서 node_issue_queue_update() 이후 호출
    - OS_IN_ROB 상태의 TEA ops를 Node Table에서 찾아 RS dispatch 재시도

(5) 빌드 및 검증:
    - tea_dbg descriptor로 빌드
    - leela simpoint 118750 (ASSERT 1), 128383 (ASSERT 2)에서 재발 여부 확인
    - TEA_OPS_DISPATCHED, TEA_RS_STALLS stat으로 dispatch 동작 검증
```

---

## 9. 검증 체크리스트

```
[ ] 빌드 성공 (tea_dbg)
[ ] ASSERT 1 (node_issue_queue.cc:308) 미발생 — leela simpoint 118750
[ ] ASSERT 2 (node_issue_queue.cc:262) 미발생 — leela simpoint 128383
[ ] flush_window rs_op_count ASSERT (node_stage.c:261) 미발생 — 모든 벤치마크
[ ] TEA_OPS_DISPATCHED > 0 (TEA dispatch 정상 동작)
[ ] TEA_OPS_ISSUED > 0 (scheduling 정상 동작)
[ ] TEA_EARLY_FLUSHES > 0 (early flush 정상 동작)
[ ] TEA_RS_STALLS ≥ 0 (RS 실패 시 재시도 동작 확인)
[ ] rs_op_count, tea_op_count, main_op_count 일관성 (flush 후 0)
[ ] 기존 non-TEA 시뮬레이션 결과 동일 (regression 없음)
```

---

## 10. flush step 1 RS 카운터 동기화 (ASSERT: `flush_window rs_op_count`)

### 10.1 문제

`flush_tea_ops_from_node_stage()` step 4의 RS 카운터 감소 조건에서 OS_SCHEDULED/OS_MISS를
포함하면 cross-cycle double decrement, 제외하면 same-cycle leak이 발생하는 딜레마.

**Cross-cycle double decrement** (v3 조건 `!= OS_IN_ROB && != OS_DONE`):
```
Cycle N-1: clear()가 OS_SCHEDULED TEA op → rs_op_count-- (정상)
Cycle N:   flush step 4가 같은 op → rs_op_count-- (중복!)
           → flush_window()에서 Main op의 rs_op_count > 0 ASSERT 실패
```

**Same-cycle leak** (v2 조건 `!= OS_SCHEDULED && != OS_MISS`):
```
같은 Cycle: flush step 1이 OS_SCHEDULED TEA op을 ready list에서 제거
            flush step 4가 OS_SCHEDULED → skip (v2 조건)
            이후 clear()가 ready list 순회 → TEA op 없음 → skip
            → RS 카운터가 어디서도 감소되지 않음 = leak
```

### 10.2 근본 원인

OS_SCHEDULED/OS_MISS ops의 RS 카운터 감소 주체가 flush와 clear 사이에서 모호함.
- **cross-cycle**: clear()가 이미 처리 → step 4에서 제외해야 함
- **same-cycle**: clear()가 아직 미실행 → 누군가가 감소해야 함

### 10.3 해결: flush step 1에서 RS 카운터 감소

핵심 관찰: **step 1에서 ready list에 존재하는 TEA op = clear()가 아직 처리 안 한 op**.
(cross-cycle에서는 clear()가 이미 ready list에서 제거했으므로 step 1에서 발견되지 않음)

따라서 step 1에서 ready list 제거 시 OS_SCHEDULED/OS_MISS TEA ops의 RS 카운터를
**함께 감소**하면, step 4에서 이 상태들을 안전하게 제외 가능.

**변경 코드** (`node_stage.c`, `flush_tea_ops_from_node_stage()` step 1):

```c
/* 1. Flush ready list — also decrement RS counter for OS_SCHEDULED/OS_MISS TEA ops.
 *    If found in ready list → clear() has NOT yet processed them (same-cycle case).
 *    In cross-cycle case, clear() already removed them → not found here → safe. */
for (op = node_local->rdy_head, last = &node_local->rdy_head; op;) {
  if (op->thread_id == 1) {
    *last = op->next_rdy;
    op->in_rdy_list = FALSE;

    /* RS counter decrement for OS_SCHEDULED/OS_MISS:
     * Normally clear() does this, but we just removed op from ready list
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
```

**step 4는 v2 조건 유지** (OS_SCHEDULED/OS_MISS 제외):
```c
/* 4. Node table flush — OS_SCHEDULED/OS_MISS는 step 1 또는 이전 cycle의 clear()에서 이미 처리 */
if (op->state != OS_IN_ROB && op->state != OS_SCHEDULED &&
    op->state != OS_MISS && op->state != OS_DONE) {
  rs->rs_op_count--; rs->tea_op_count--;
}
```

### 10.4 정확성 검증

| 시나리오 | step 1 | step 4 | clear() | 총 감소 |
|---------|--------|--------|---------|---------|
| **Same-cycle, OS_SCHEDULED** | ready list에서 발견 → rs_op_count-- | OS_SCHEDULED → skip | TEA op 없음 → skip | 1회 ✅ |
| **Cross-cycle, OS_SCHEDULED** | ready list에 없음 → skip | OS_SCHEDULED → skip | 이전 cycle에서 처리 완료 | 1회 ✅ |
| **Same-cycle, OS_IN_RS** | ready list에서 발견 → 제거 (MISS/SCHED 아님 → 미감소) | OS_IN_RS → 감소 | TEA op 없음 → skip | 1회 ✅ |
| **Cross-cycle, OS_IN_RS** | ready list에 없음 (OS_IN_RS는 clear() 대상 아님) → skip | OS_IN_RS → 감소 | OS_IN_RS는 clear() 대상 아님 | 1회 ✅ |
| **Any cycle, OS_IN_ROB** | ready list에 없음 → skip | OS_IN_ROB → skip | 대상 아님 | 0회 ✅ |
| **Any cycle, OS_DONE** | ready list에 없음 (이미 clear() 처리) → skip | OS_DONE → skip | 이전에 처리 완료 | 1회 ✅ |

### 10.5 `flush_tea_ops_by_chain_id()`에도 동일 적용

`TEA_op_manage_plan.md` §2.2의 `flush_tea_ops_by_chain_id()` step 1에도 동일한 RS 카운터 감소 추가 필요.

### 10.6 독립 dispatch와의 관계

독립 dispatch 구현 (§2-3) 이후에도 이 수정은 여전히 필요:
- TEA ops가 `tea_dispatch_to_rs()`에서 직접 RS에 dispatch되더라도, scheduling 후 OS_SCHEDULED 상태가 됨
- TEA 종료 시 flush step 1/4의 RS 카운터 동기화 로직은 동일하게 동작

### 10.7 구현 순서 (§8과 통합)

```
(0) flush step 1 RS 카운터 동기화 추가 ← 독립 dispatch와 무관하게 먼저 적용 가능
    - flush_tea_ops_from_node_stage() step 1에 OS_SCHEDULED/OS_MISS 감소 추가
    - step 4 조건은 v2 유지 (변경 없음)
    - 빌드 후 flush_window rs_op_count ASSERT 미발생 확인

(1)-(5) 독립 dispatch 구현 (§8 참조)
```
