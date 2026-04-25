# TEA Early Flush 향후 구현 계획

**최종 갱신**: 2026-03-09
**관련 상태 문서**: `TEA_early_flush_status.md`
**현재 상태**: 단일 H2P 기준으로 완전 구현 ✅

---

## 1. 개요

현재 Early Flush 로직은 단일 H2P만 지원한다. 다음 작업들이 향후 필요하다:

| ID | 작업 | 관련 작업 | 우선순위 |
|----|------|----------|---------|
| **EF-1** | `recover_tea_on_flush()` 시그니처 변경 | H2P 큐 (작업 H) | 높음 |
| **EF-2** | 다중 H2P Early Flush 처리 | 작업 F | 높음 |
| **EF-3** | SRT Checkpoint 다중화 | 작업 F | 높음 |
| **EF-4** | `unique_num` 기반 Op* 유효성 검증 | 작업 H | 낮음 |

---

## 2. EF-1: `recover_tea_on_flush()` 시그니처 변경

### 2.1 문제

현재 `recover_tea_on_flush(uns proc_id)`는 무조건 TEA를 종료한다.
H2P 큐(작업 H)나 다중 H2P(작업 F) 구현 시에는 recovery 범위에 해당하는 TEA chain만
선택적으로 종료해야 한다. 또한 큐에 들어있는 H2P 중 flush 대상인 것도 제거해야 한다.

### 2.2 현재 코드

```c
// cmp_model.c:389-399
void recover_tea_on_flush(uns proc_id) {
  if (!TEA_ENABLE || !tea_is_active(proc_id)) return;
  terminate_tea_thread(proc_id);
  reset_tea_fetch_stage(proc_id);
  reset_tea_rename_stage(proc_id);
  reset_tea_preg_pool(proc_id);
}

// cmp_model.h:91
void recover_tea_on_flush(uns proc_id);
```

**호출 지점**:
- `cmp_model.c:445` — `cmp_recover()` 내부 (Main thread recovery)
- `exec_stage.c:602` — Case 1 (Pre-rename early flush, TEA 즉시 종료)

**`reset_tea_rename_stage()` vs `recover_tea_rename_stage()` 관계**:
현재 코드에서 `recover_tea_rename_stage()` (`tea_rename.c:506-509`)는 단순히
`reset_tea_rename_stage()`를 호출하는 wrapper이며 기능적으로 동일하다:
```c
void recover_tea_rename_stage(uns proc_id) {
  reset_tea_rename_stage(proc_id);  // 동일 함수 호출
}
```
현재 단일 H2P 코드(`cmp_model.c:394`)는 `reset_tea_rename_stage()`를 직접 호출하고,
다중 H2P 계획 코드(본 문서 §3.4, §3.6 등)는 `recover_tea_rename_stage()`를 사용한다.
두 함수가 동일하므로 동작 차이는 없으나, 의미적 구분을 위해 다중 H2P 구현 시
`recover_` 버전으로 통일하는 것을 권장한다 (recovery 컨텍스트에서의 호출임을 명시).

### 2.3 변경 계획

시그니처를 `(uns proc_id, Counter recovery_op_num)`으로 변경:

```c
// cmp_model.h
void recover_tea_on_flush(uns proc_id, Counter recovery_op_num);
```

```c
// cmp_model.c
void recover_tea_on_flush(uns proc_id, Counter recovery_op_num) {
  if (!TEA_ENABLE) return;

  Tea_Thread* tea = tea_threads[proc_id];
  if (!tea || tea->state == TEA_IDLE) return;

  /* 1. H2P 큐에서 flushed 엔트리 제거 (작업 H 구현 시) */
  // tea_h2p_queue_flush(proc_id, recovery_op_num);

  /* 2. 현재 활성 TEA의 target_h2p_op_num >= recovery_op_num이면 종료
   *    (활성 H2P 자체가 flush 대상이거나 recovery point와 동일) */
  if (tea->target_h2p_op_num >= recovery_op_num) {
    /* terminate_tea_thread()가 내부에서 다음 작업을 모두 수행하므로
     * 외부 reset_* 호출은 중복이다 (tea_thread.c:154-185 참조):
     *   - recover_tea_fetch_stage()  → reset_tea_fetch_stage()
     *   - recover_tea_rename_stage() → reset_tea_rename_stage()
     *   - flush_tea_ops_from_node_stage()
     *   - reset_tea_preg_pool()
     *   - reset_tea_store_buffer()
     * 기존 단일 H2P 코드의 중복 reset_* 3줄은 제거된다. */
    terminate_tea_thread(proc_id);
  }
  /* else: 활성 H2P가 flush 대상이 아니면 TEA 유지 (최적화) */
}
```

### 2.4 호출 지점 변경

```c
// cmp_model.c:445 — cmp_recover() 내부
recover_tea_on_flush(bp_recovery_info->proc_id,
                     bp_recovery_info->recovery_op_num);

// exec_stage.c:631 — Case 1 (Pre-rename, 양쪽 sub-case 공통)
// Case 1a/1b 모두 recover_at_exec=TRUE를 유지한 채 TEA만 즉시 종료.
// target_h2p_op_num >= target_h2p_op_num → TRUE → TEA 정상 종료.
recover_tea_on_flush(op->proc_id, tea->target_h2p_op_num);
```

**Case 1 현재 구현**: 현재 `exec_stage.c`는 Case 1을 두 sub-case로 구분한다:

- **Case 1a** (`main_h2p->decode_cycle > 0`): Main H2P가 decode를 통과했지만 아직
  rename 전. `recover_at_exec = TRUE`를 유지하여 Main H2P가 exec에 도달할 때 정상
  recovery 발동. TEA만 즉시 종료 (`TEA_EARLY_FLUSH_CASE1_NO_CHKPT`).
- **Case 1b** (`main_h2p->decode_cycle == 0`): Main H2P가 아직 decode 전.
  `recover_at_decode = TRUE`를 **사용하지 않음** — 이 경로는 `flush_mispredict()`와
  SRT rollback을 건너뛰어 off-path preg allocation이 ALLOC orphan으로 누수되고
  SRT 불일치가 발생하기 때문. `recover_at_exec = TRUE`를 유지하여 Main H2P가
  rename → exec를 정상 통과할 때 proper recovery 발동. TEA만 즉시 종료
  (`TEA_EARLY_FLUSH_CASE1_DECODE`).

두 sub-case 모두 `recover_tea_on_flush()` 단일 호출로 처리되며, EF-1 적용 후에도
`recovery_op_num = target_h2p_op_num`을 전달하면 `>=` 조건에서 TRUE가 되어 TEA가 종료됨.

**참고**: `>=` 연산자 이유 — `>` 사용 시 `target_h2p_op_num == recovery_op_num`인 케이스
(Case 1 + Case 2 모두 해당)에서 조건이 FALSE가 되어 TEA가 종료되지 않는 버그 발생.
`>=`로 수정하여 두 케이스 모두에서 올바르게 동작한다.

> **⚠️ 다중 H2P 전환 시 Case 1 경로 교체 필수**
>
> 단일 H2P EF-1에서는 `exec_stage.c:631` Case 1이 `recover_tea_on_flush()`를 경유해도
> chain이 1개뿐이라 결과적으로 `terminate_tea_thread()`로 수렴하지만, 다중 H2P에서는
> `recover_tea_on_flush()`가 **모든 younger chain**을 종료하므로 mispredicted H2P 이외의
> 정상 chain까지 불필요하게 종료된다. EF-2 전환 시 이 호출부를 다음과 같이 교체해야 함:
>
> ```c
> // 단일 H2P (EF-1):
> recover_tea_on_flush(op->proc_id, tea->target_h2p_op_num);
>
> // 다중 H2P (EF-2, 본 문서 §3.3): mispredicted chain만 직접 종료
> terminate_tea_chain(op->proc_id, chain_idx);
> ```
>
> EF-1 구현 시 이 경로를 별도 래퍼나 주석으로 표시해두면 EF-2 전환 시 누락을 방지할 수 있다.

### 2.5 수정 파일

| 파일 | 변경 |
|------|------|
| `src/cmp_model.h` | 함수 선언 시그니처 변경 |
| `src/cmp_model.c` | 함수 구현 변경 + `cmp_recover()` 내 호출 인자 변경 |
| `src/exec_stage.c` | Case 1 호출 변경 |

---

## 3. EF-2: 다중 H2P Early Flush 처리 (작업 F)

### 3.1 문제

다중 H2P 지원 시 `exec_stage_bp_resolve()`에서 TEA op이 어느 chain의 H2P인지 식별해야 한다.
현재는 `tea->target_h2p_pc` 하나와만 비교하지만, 여러 chain이 동시에 실행 중이면
배열 순회가 필요하다.

### 3.2 현재 코드의 제약

```c
// exec_stage.c:561 — 단일 PC 비교
if (op->inst_info->addr == tea->target_h2p_pc) {
```

```c
// exec_stage.c:567-569 — 단일 main_h2p_op
Op* main_h2p = tea->main_h2p_op;
ASSERT(op->proc_id, main_h2p != NULL);
ASSERT(op->proc_id, main_h2p->op_pool_valid);
```

### 3.3 변경 계획

`Op` 구조체에 `h2p_chain_id` 추가 후 (`op.h`), `tea_create_op_from_cache()`에서 설정.

```c
// exec_stage.c — 다중 H2P 버전
static inline void exec_stage_bp_resolve(Op* op) {
  if (TEA_ENABLE && op->thread_id == 1) {
    Tea_Thread* tea = tea_threads[op->proc_id];
    if (!tea || tea->num_active_chains == 0) {
      STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
      return;
    }

    /* chain_id는 1-based (0 = main op) */
    int chain_idx = op->h2p_chain_id - 1;
    if (chain_idx < 0 || chain_idx >= MAX_TEA_CHAINS) {
      STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
      return;
    }

    Tea_H2P_Chain* c = &tea->chains[chain_idx];
    if (c->state == CHAIN_INACTIVE) {
      STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
      return;
    }

    /* H2P branch 매칭 */
    if (op->inst_info->addr == c->target_h2p_pc) {
      Op* main_h2p = c->main_h2p_op;

      /* Op* 유효성 검증 (EF-4) */
      if (!main_h2p->op_pool_valid ||
          main_h2p->unique_num != c->saved_unique_num) {
        terminate_tea_chain(op->proc_id, chain_idx);
        STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
        return;
      }

      if (op->oracle_info.mispred || op->oracle_info.misfetch) {
        if (main_h2p->off_path) {
          STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
          return;
        }

        if (!main_h2p->oracle_info.recovery_sch) {
          if (reg_file_checkpoint_is_valid()) {
            /* Case 2: Post-rename */
            bp_sched_recovery(bp_recovery_info, main_h2p,
                              op->exec_cycle, FALSE, FALSE,
                              EXTRA_LATE_RECOVERY_CYCLES);
            if (main_h2p->oracle_info.recovery_sch)
              main_h2p->recovery_scheduled = TRUE;
          } else {
            /* Case 1: Pre-rename */
            main_h2p->oracle_info.recover_at_decode = TRUE;
            /* 이 chain만 즉시 종료 (다른 chains는 cmp_recover()에서 처리) */
            terminate_tea_chain(op->proc_id, chain_idx);
          }
          main_h2p->oracle_info.recover_at_exec = FALSE;
          STAT_EVENT(op->proc_id, TEA_EARLY_FLUSHES);
        }
      } else {
        /* 정확한 예측 → 이 chain 정상 완료 */
        STAT_EVENT(op->proc_id, TEA_H2P_CORRECT);
        terminate_tea_chain(op->proc_id, chain_idx);
      }
    }
    STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
    return;
  }
  // ... Main thread branch resolution ...
}
```

### 3.4 `terminate_tea_chain()` — per-chain 종료 (신규)

> **방안 A 채택** (`TEA_op_manage_plan.md` §3.3 방식):
> `terminate_tea_chain()`이 fetch/rename/node 모든 stage를 per-chain으로 정리하고,
> `flush_tea_ops_by_chain_id()`도 내부에서 호출. Caller는 한 번만 호출하면 됨.

```c
// tea_thread.c
void terminate_tea_chain(uns proc_id, int chain_id) {
  Tea_Thread* tea = tea_threads[proc_id];
  Tea_H2P_Chain* c = &tea->chains[chain_id];
  uns8 h2p_chain_id = chain_id + 1;  // 1-based

  ASSERT(proc_id, c->state != CHAIN_INACTIVE);

  /* 1. fetch 중인 chain이면 fetch stage에서 해당 chain ops 제거 */
  if (c->state == CHAIN_FETCHING &&
      tea->current_fetch_chain == chain_id) {
    recover_tea_fetch_stage_by_chain(proc_id, h2p_chain_id);
  }

  /* 2. rename stage에서 해당 chain ops 제거 */
  recover_tea_rename_stage_by_chain(proc_id, h2p_chain_id);

  /* 3. node stage (exec/dcache/RS/node table)에서 해당 chain ops flush */
  flush_tea_ops_by_chain_id(proc_id, h2p_chain_id);

  /* 4. store buffer에서 해당 chain 엔트리 무효화 */
  tea_store_buffer_clear_by_chain_id(proc_id, h2p_chain_id);

  /* 5. chain 상태 초기화 */
  c->state = CHAIN_INACTIVE;
  c->target_h2p_pc = 0;
  c->target_h2p_op_num = 0;
  c->main_h2p_op = NULL;
  c->tea_op_count = 0;
  c->tea_ops_fetched = 0;
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

### 3.5 `flush_tea_ops_by_chain_id()` — 선택적 flush (신규)

`flush_tea_ops_from_node_stage()`와 동일한 구조이나 `op->h2p_chain_id` 로 필터링:

```c
// node_stage.c
void flush_tea_ops_by_chain_id(uns proc_id, uns8 chain_id) {
  extern Cmp_Model cmp_model;
  Node_Stage* node_local = &cmp_model.node_stage[proc_id];

  /* 0a. exec_stage->sd.ops[] 에서 해당 chain ops 제거 */
  Exec_Stage* exec_local = &cmp_model.exec_stage[proc_id];
  for (uns ii = 0; ii < exec_local->sd.max_op_count; ii++) {
    Op* op = exec_local->sd.ops[ii];
    if (op && op->thread_id == 1 && op->h2p_chain_id == chain_id) {
      exec_local->sd.ops[ii] = NULL;
      exec_local->sd.op_count--;
    }
  }

  /* 0b. dcache_stage->sd.ops[] 에서 해당 chain ops 제거 */
  Dcache_Stage* dc_local = &cmp_model.dcache_stage[proc_id];
  for (uns ii = 0; ii < dc_local->sd.max_op_count; ii++) {
    Op* op = dc_local->sd.ops[ii];
    if (op && op->thread_id == 1 && op->h2p_chain_id == chain_id) {
      dc_local->sd.ops[ii] = NULL;
      dc_local->sd.op_count--;
    }
  }

  /* 1. Ready list에서 해당 chain ops 제거
   *    OS_SCHEDULED/OS_MISS ops가 ready list에 있으면 = clear()가 아직 처리 안 한 것.
   *    ready list에서 제거하면 clear()가 이후 찾지 못하므로 여기서 RS 카운터도 감소.
   *    (TEA_dispatch_plan.md §10.3 참조) */
  Op* op;
  Op** last;
  for (op = node_local->rdy_head, last = &node_local->rdy_head; op;) {
    if (op->thread_id == 1 && op->h2p_chain_id == chain_id) {
      *last = op->next_rdy;
      op->in_rdy_list = FALSE;

      /* RS counter sync for OS_SCHEDULED/OS_MISS */
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

  /* 2. Scheduling buffer 에서 제거 */
  for (uns ii = 0; ii < node_local->sd.max_op_count; ii++) {
    Op* op = node_local->sd.ops[ii];
    if (op && op->thread_id == 1 && op->h2p_chain_id == chain_id) {
      node_local->sd.ops[ii] = NULL;
      node_local->sd.op_count--;
    }
  }

  /* 3. next_op_into_rs: 해당 chain op을 가리키면 다음 non-target op으로 전진
   * (Bug Fix 8.3 반영: NULL 대신 전진)
   * NOTE: 독립 dispatch(TEA_dispatch_plan.md) 구현 후 TEA ops는 next_op_into_rs에
   * 영향을 주지 않으므로 이 step은 사실상 no-op이 됨. 안전을 위해 유지. */
  if (node_local->next_op_into_rs &&
      node_local->next_op_into_rs->thread_id == 1 &&
      node_local->next_op_into_rs->h2p_chain_id == chain_id) {
    Op* next = node_local->next_op_into_rs->next_node;
    while (next && next->thread_id == 1 && next->h2p_chain_id == chain_id)
      next = next->next_node;
    node_local->next_op_into_rs = next;
  }

  /* 4. Node table에서 해당 chain ops 제거 및 free */
  node_local->node_tail = NULL;
  for (op = node_local->node_head, last = &node_local->node_head; op;) {
    if (op->thread_id == 1 && op->h2p_chain_id == chain_id) {
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

### 3.6 `recover_tea_on_flush()` 다중 H2P 버전

```c
// cmp_model.c
void recover_tea_on_flush(uns proc_id, Counter recovery_op_num) {
  if (!TEA_ENABLE) return;
  Tea_Thread* tea = tea_threads[proc_id];
  if (!tea) return;

  /* 1. H2P 큐에서 flushed 엔트리 제거 (작업 H 구현 시) */
  // tea_h2p_queue_flush(proc_id, recovery_op_num);

  /* 2. recovery_op_num보다 young한 chain 무효화
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
    if (tea->chains[i].state != CHAIN_INACTIVE &&
        tea->chains[i].target_h2p_op_num >= recovery_op_num) {
      terminate_tea_chain(proc_id, i);
    }
  }

  /* 3. 공유 자원 정리는 terminate_tea_chain() 내부에서 자동 처리
   *    (num_active_chains == 0일 때 reset_tea_preg_pool, reset_tea_store_buffer,
   *     recover_tea_rename_stage, tea->state = TEA_IDLE 수행)
   *    → 루프 후 중복 리셋 불필요 */
}
```

### 3.7 수정 파일

| 파일 | 변경 |
|------|------|
| `src/op.h` | `uns8 h2p_chain_id` 필드 추가 |
| `src/exec_stage.c` | `exec_stage_bp_resolve()` 다중 chain 매칭으로 변경 |
| `src/tea/tea_thread.h` | `Tea_H2P_Chain` 구조체, `terminate_tea_chain()` 프로토타입 |
| `src/tea/tea_thread.c` | `terminate_tea_chain()` 구현 |
| `src/node_stage.c` | `flush_tea_ops_by_chain_id()` 추가 |
| `src/node_stage.h` | `flush_tea_ops_by_chain_id()` 프로토타입 |
| `src/cmp_model.c` | `recover_tea_on_flush()` 다중 chain 버전 |
| `src/cmp_model.h` | 시그니처 변경 |
| `src/tea/tea_fetch_stage.c` | `tea_create_op_from_cache()`에서 `h2p_chain_id` 설정 |

---

## 4. EF-3: SRT Checkpoint 다중화 (작업 F)

### 4.1 문제

Scarab은 SRT checkpoint을 **하나만** 유지한다:

```c
// map_rename.c:536
ASSERT(map_data->proc_id, !checkpoint->is_valid);
```

다중 H2P가 각각 rename을 통과하면 두 번째 H2P의 rename 시 `ASSERT` 실패.

### 4.2 발생 조건

```
Cycle 100: H2P_A rename → reg_file_snapshot_srt() → checkpoint.is_valid = TRUE
Cycle 105: H2P_B rename → reg_file_snapshot_srt() → ASSERT(!is_valid) 실패!
```

### 4.3 해결 방안

**방안 A: Oldest-only checkpoint (권장)**

가장 오래된 H2P의 checkpoint만 유지. 두 번째 이후 H2P는 snapshot을 건너뛴다.
Recovery 시 oldest H2P의 checkpoint으로 rollback하면 그보다 young한 모든 ops가 flush되므로 정확함.

```c
// map_rename.c:958-959 수정
if (!op->off_path && op->table_info->cf_type && op->oracle_info.recover_at_exec) {
  /* 다중 H2P: checkpoint이 이미 존재하면 snapshot 건너뛰기
   * oldest H2P의 checkpoint만 유지하면 충분 —
   * recovery 시 oldest 이후 모든 ops가 flush됨 */
  if (!reg_file_checkpoint_is_valid()) {
    reg_file_snapshot_srt();
  }
}
```

**장점**: 변경 최소 (ASSERT 제거 + 조건 추가), 기존 recovery 로직과 완전 호환.

**단점**: TEA가 younger H2P를 먼저 resolve하면, 해당 H2P의 정확한 SRT 상태가 아닌
oldest H2P 시점의 상태로 rollback. 하지만 youngest→flush_point까지 pregs를 해제하므로
functional correctness에는 영향 없음.

**방안 B: Checkpoint 배열**

`reg_checkpoint`을 배열로 확장. 각 `recover_at_exec` branch마다 별도 checkpoint 할당.
복잡도가 높고 `bp_sched_recovery()` / `reg_file_recover()` 전반 수정 필요. 비권장.

### 4.4 수정 파일 (방안 A)

| 파일 | 변경 |
|------|------|
| `src/map_rename.c` | `reg_file_snapshot_srt()` 호출 조건에 `!checkpoint_is_valid()` 추가 |

---

## 5. EF-4: `unique_num` 기반 Op* 유효성 검증

### 5.1 문제

`tea->main_h2p_op` (또는 `Tea_H2P_Chain.main_h2p_op`)은 Main thread의 Op 포인터.
이론적으로 Main H2P가 retire된 후 같은 메모리가 재사용되면 dangling pointer 문제 발생.

현재는 mispredicted branch가 execute 전에 retire할 수 없으므로 발생하지 않지만,
H2P 큐(작업 H)에서 dequeue 시점에는 이미 retire된 Op일 수 있다.

### 5.2 현재 코드

```c
// exec_stage.c:568-569
Op* main_h2p = tea->main_h2p_op;
ASSERT(op->proc_id, main_h2p != NULL);
ASSERT(op->proc_id, main_h2p->op_pool_valid);
```

### 5.3 변경 계획

**참고**: 다중 H2P 전환 시 이 필드는 `Tea_Thread`에서 `Tea_H2P_Chain`(per-chain)으로 이동.
[`TEA_multi_h2p_plan.md`](TEA_multi_h2p_plan.md) §2.2 참조.

```c
// tea_thread.h — Tea_Thread에 추가 (단일 H2P 단계; 다중 H2P에서 Tea_H2P_Chain으로 이동)
Counter saved_unique_num;  /* trigger 시 main_h2p_op->unique_num 저장 */

// tea_thread.c — trigger_tea_thread()에서 저장
tea->saved_unique_num = h2p_op->unique_num;

// exec_stage.c — 검증 추가
Op* main_h2p = tea->main_h2p_op;
if (!main_h2p || !main_h2p->op_pool_valid ||
    main_h2p->unique_num != tea->saved_unique_num) {
  /* Main H2P가 이미 free됨 → TEA 종료 */
  terminate_tea_thread(op->proc_id);
  STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
  return;
}
```

### 5.4 수정 파일

| 파일 | 변경 |
|------|------|
| `src/tea/tea_thread.h` | `saved_unique_num` 필드 추가 |
| `src/tea/tea_thread.c` | `trigger_tea_thread()`에서 저장 |
| `src/exec_stage.c` | ASSERT 대신 `unique_num` 검증 + graceful 종료 |

---

## 6. 파이프라인 순서 관련 주의사항

### 6.1 `cmp_cycle()` 실행 순서 (`cmp_model.c:209-294`)

```
cmp_istreams()             ← recovery_cycle 도달 시 cmp_recover() 실행
update_memory()
cmp_cores():
  update_dcache_stage()    ← DCache 처리
  update_exec_stage()      ← TEA H2P 실행 + bp_resolve → bp_sched_recovery()
  update_node_stage()      ← Node: dispatch, retirement
  update_map_stage()       ← Rename: SRT checkpoint 생성
  ...frontend stages...
  update_tea_*()           ← TEA fetch/rename/update
```

### 6.2 Recovery timing 보장

- Cycle X: `update_exec_stage()` → `bp_sched_recovery(recovery_cycle = X+1)`
- Cycle X+1: `cmp_istreams()` → `cmp_recover()` → **같은 cycle의 `cmp_cores()` 전에 recovery 완료**

### 6.3 SRT checkpoint timing

- `update_exec_stage()`가 `update_map_stage()`보다 **먼저** 실행
- TEA H2P 실행 시점에 `checkpoint_is_valid() == TRUE`이면 Main H2P는 **이전 cycle**에 rename됨
- 같은 cycle에 rename되는 경우 checkpoint은 아직 없음 → Case 1로 처리

---

## 7. 구현 순서

```
EF-4 (unique_num 검증) ← 독립적, 다른 작업과 무관하게 적용 가능
    │
    ▼
EF-1 (recover_tea_on_flush 시그니처) ← 작업 H (H2P 큐) 전제조건
    │
    ▼
EF-3 (SRT checkpoint) + EF-2 (다중 H2P Early Flush) ← 작업 F와 함께 구현
```

---

## 8. 검증 체크리스트

### EF-1 완료 후

```
빌드 성공 + 기존 TEA 시뮬레이션 결과 동일 (regression 없음)
TEA_EARLY_FLUSHES 값 변화 없음 (단일 H2P 기준)
```

### EF-2 + EF-3 완료 후 (작업 F와 함께)

```
TEA_TRIGGERS 증가 (다중 H2P 활성화)
TEA_EARLY_FLUSHES 증가 또는 유지
TEA_CHAIN_TERMINATED > 0
SRT checkpoint ASSERT 미발생
```
