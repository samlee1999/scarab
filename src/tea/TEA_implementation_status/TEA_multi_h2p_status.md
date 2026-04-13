# TEA 다중 H2P+DC 현재 구현 상태

**최종 갱신**: 2026-04-12
**관련 계획**: [`TEA_multi_h2p_plan.md`](../TEA_implementation_plan/TEA_multi_h2p_plan.md)
**마스터 문서**: [`TEA_implementation_plan.md`](../TEA_implementation_plan.md) Section 3.4, 8

---

## 1. 요약

| 항목 | 상태 | 설명 |
|------|------|------|
| 다중 H2P chain 데이터 구조 | ❌ 미구현 | `Tea_H2P_Chain` 구조체 없음, 단일 필드만 존재 |
| `h2p_chain_id` in `Op` | ❌ 미구현 | `op.h`에 chain ID 필드 없음 |
| 다중 trigger (빈 슬롯 할당) | ❌ 미구현 | `state != TEA_IDLE` 게이트로 단일 H2P만 수용 |
| 순차 fetch + chain 전환 | ❌ 미구현 | `active_chain` 단일 포인터, chain 전환 로직 없음 |
| per-chain 종료 | ❌ 미구현 | `terminate_tea_thread()`가 전체 TEA 일괄 flush |
| per-chain Early Flush | ❌ 미구현 | `exec_stage_bp_resolve()`가 단일 `target_h2p_pc`만 비교 |
| selective recovery | ❌ 미구현 | `recover_tea_on_flush()`가 무조건 전체 종료 |
| 이미 다중 H2P 호환 | ✅ 13개 컴포넌트 | `thread_id` 기반 로직은 변경 불필요 |

**현재 상태**: 단일 H2P만 지원. TEA가 활성 상태(`state != TEA_IDLE`)이면 후속 H2P branch는
`TEA_TRIGGER_SKIP_ACTIVE`로 모두 거부됨.

---

## 2. 단일 H2P 제약 지점 상세

### 2.1 `Tea_Thread` 구조체 — 단일 H2P 필드

**파일**: `tea/tea_thread.h:48-76`

```c
typedef struct Tea_Thread_struct {
  uns8 proc_id;
  Tea_State state;                 // Line 50: 전체 TEA에 대한 단일 상태

  /* H2P 정보 — 모두 단일 값 */
  Addr target_h2p_pc;              // Line 53: 하나의 H2P PC
  Counter target_h2p_op_num;       // Line 54: 하나의 op_num
  Op* main_h2p_op;                 // Line 55: 하나의 Main H2P 포인터
  Op_Info h2p_oracle_info;         // Line 56
  Recovery_Info h2p_recovery_info; // Line 57

  /* Block Cache 탐색 — 단일 chain */
  Addr current_block_pc;           // Line 60
  int current_chain_idx;           // Line 61

  /* TEA 파이프라인 — 전체 합산 카운터 */
  uns tea_op_count;                // Line 64: per-chain 구분 불가
  Counter tea_ops_fetched;         // Line 65
  Counter tea_op_counter;          // Line 66: 0x8000000000000000 으로 초기화
  // ...
} Tea_Thread;
```

**문제**: `Tea_H2P_Chain` 배열 없이는 동시에 여러 H2P를 추적할 수 없음.

### 2.2 `trigger_tea_thread()` 게이트

**파일**: `tea/tea_thread.c:110-115`

```c
if (tea->state != TEA_IDLE) {
    STAT_EVENT(proc_id, TEA_TRIGGER_SKIP_ACTIVE);
    return;  // ← TEA가 활성이면 무조건 거부
}
```

**문제**: 현재 fetch/execute 중인 chain이 있으면 새 H2P를 수용할 수 없음.
다중 H2P에서는 `num_active_chains < MAX_TEA_CHAINS`로 빈 슬롯 여부를 확인해야 함.

### 2.3 `Tea_Fetch_Stage` — 단일 chain 포인터

**파일**: `tea/tea_fetch_stage.h:52-57`

```c
Dependency_Chain_Cache_Entry* active_chain;  // Line 52: 하나의 chain만 fetch
int current_chain_idx;                       // Line 53
int total_chain_length;                      // Line 54
Flag fetch_complete;                         // Line 57: 단일 완료 플래그
Counter ops_fetched_this_cycle;              // Line 58
```

**문제**: chain 전환 메커니즘이 없으므로, Chain A fetch 완료 후 Chain B로 넘어가는
"순차 fetch, 중첩 실행" 모델 구현 불가.

### 2.4 `tea_create_op_from_cache()` — chain ID 미부여

**파일**: `tea/tea_fetch_stage.c:195-243`

- `tea_op->thread_id = 1`만 설정, `h2p_chain_id`는 `Op` 구조체에 필드 자체가 없음
- H2P branch의 oracle/recovery 정보를 `tea->h2p_oracle_info` 단일 값에서 복사
- 다중 H2P에서는 각 chain의 `Tea_H2P_Chain`에서 해당 정보를 가져와야 함

### 2.5 `update_tea_thread()` 상태 기계 — 단일 상태

**파일**: `tea/tea_thread.c:210-246`

```c
switch (tea->state) {
  case TEA_FETCHING:
    if (tea_fetch_stages[proc_id]->fetch_complete) {
      tea->state = TEA_EXECUTING;  // ← 전체 TEA의 단일 전환
    }
    break;
  case TEA_EXECUTING:
    if (tea->tea_op_count == 0 && tea->tea_ops_fetched > 0) {
      terminate_tea_thread(proc_id);  // ← 전체 TEA 종료
    }
    break;
}
```

**문제**: "Chain A = EXECUTING, Chain B = FETCHING"와 같은 per-chain 상태 표현 불가.

### 2.6 `terminate_tea_thread()` — 전체 flush

**파일**: `tea/tea_thread.c:154-185`

```c
// tea_thread.c:154-185
recover_tea_fetch_stage(proc_id);        // Line 166: 전체 fetch stage flush
recover_tea_rename_stage(proc_id);       // Line 167: 전체 rename stage flush
flush_tea_ops_from_node_stage(proc_id);  // Line 168: 모든 thread_id==1 ops flush
reset_tea_preg_pool(proc_id);            // Line 171: 전체 preg pool 리셋
reset_tea_store_buffer(proc_id);         // Line 174: 전체 store buffer 리셋
```

**문제**: Chain A의 H2P가 실행 완료되어도 Chain B, C의 ops까지 모두 flush됨.
다중 H2P에서는 `terminate_tea_chain(chain_id)`으로 특정 chain만 정리해야 함.

### 2.7 `exec_stage_bp_resolve()` — 단일 H2P PC 비교

**파일**: `exec_stage.c:576`

```c
if (op->inst_info->addr == tea->target_h2p_pc) {  // 단일 PC만 비교
```

**문제**: 다중 H2P에서는 실행된 TEA H2P branch가 어느 chain에 속하는지 식별해야 함.
`h2p_chain_id`로 매칭하거나 `chains[]` 배열을 순회해야 함.

### 2.8 `recover_tea_on_flush()` — 무조건 전체 종료

**파일**: `cmp_model.c:389-399`

```c
void recover_tea_on_flush(uns proc_id) {
  if (!TEA_ENABLE || !tea_is_active(proc_id)) return;
  terminate_tea_thread(proc_id);  // ← 전체 종료
}
```

**문제**: Main thread recovery 시 recovery point보다 older한 chain은 유지해야 함.
`recover_tea_on_flush(proc_id, recovery_op_num)` 시그니처로 변경 필요.

### 2.9 `Tea_Store_Buffer` — per-chain 태깅 없음

**파일**: `tea/tea_store_buffer.h:45-63`

```c
typedef struct Tea_Store_Buffer_Entry_struct {
  Flag valid;
  Addr addr;
  uns size;
  uns8 data[64];
} Tea_Store_Buffer_Entry;  // h2p_chain_id 필드 없음
```

**문제**: chain 종료 시 해당 chain의 store만 선택적으로 무효화할 수 없음.

### 2.10 `Op` 구조체 — `h2p_chain_id` 필드 없음

**파일**: `op.h:135`

- `thread_id` 필드만 존재 (0=main, 1=TEA)
- TEA op이 어느 H2P chain에 속하는지 구분하는 `h2p_chain_id` 필드가 없음
- downstream (node_stage, exec_stage, dcache_stage)에서 per-chain 처리 불가

---

## 3. 이미 다중 H2P 호환되는 부분 (변경 불필요)

아래 컴포넌트들은 `thread_id` 기반 또는 주소 기반으로 동작하므로
다중 H2P 구현 시에도 수정이 필요하지 않음.

| # | 컴포넌트 | 파일:라인 | 호환 이유 |
|---|---------|----------|----------|
| 1 | RS 파티셔닝 | `node_issue_queue.cc:100-111` | `thread_id` 기반 카운터 |
| 2 | 2-pass 스케줄링 (TEA 우선) | `node_issue_queue.cc:403-446` | `thread_id==1` 전체 우선 |
| 3 | `tea_dispatch_to_rs()` | `node_stage.c:418` | `thread_id==1` 체크만 사용 |
| 4 | Node Table 링크드 리스트 | `node_stage.c` | 모든 TEA ops 공존 가능 |
| 5 | `node_retire_tea_ops()` | `node_stage.c:659` | `thread_id==1`이면 모두 처리 |
| 6 | `flush_window()` TEA skip | `node_stage.c:252-260` | `thread_id==1` → skip |
| 7 | `flush_tea_ops_from_node_stage()` | `node_stage.c:1127` | `thread_id==1` 일괄 제거 |
| 8 | Main RAT에서 TEA 제외 | `map_rename.c:1591-1615` | `thread_id==1` → return |
| 9 | `exec_stage_clear_fu()` TEA 처리 | `exec_stage.c:475-494` | `thread_id==1` dispatch |
| 10 | Dcache TEA load/store | `dcache_stage.c:228-267` | 주소 기반, H2P 무관 |
| 11 | TEA op_num 공간 분리 | `tea_thread.c:90` | `0x8000...` 시작, 공유 카운터 |
| 12 | Fill Buffer | `fill_buffer.c:42-69` | Main thread retire만 추적 |
| 13 | Dep Chain Cache 조회 | `dependency_chain_cache.c:266-285` | PC 인덱싱, 다중 H2P 자연 지원 |

**참고**: `flush_tea_ops_from_node_stage()`는 현재 모든 TEA ops를 일괄 flush하지만,
이는 `terminate_tea_thread()`에서만 호출되므로 전체 종료 시에는 올바른 동작임.
다중 H2P에서는 `flush_tea_ops_by_chain_id()`를 추가하되,
전체 종료가 필요한 경우에는 기존 함수도 유지.

**⚠️ `flush_tea_ops_by_chain_id()` 구현 시 적용 필수**: `flush_tea_ops_from_node_stage()` (`node_stage.c:1127`)의 5-step 로직 참조:
- **RS 카운터 감소 조건**: whitelist 기반 (`OS_IN_RS || OS_READY || OS_WAIT_FWD`) — OS_IN_ROB/SCHEDULED/MISS/DONE 제외
- **next_op_into_rs 처리**: NULL이 아닌 다음 non-target op으로 전진
- 상세: `TEA_op_manage_status.md` §3, §5 참조

---

## 4. 부분적 호환 컴포넌트 (BW Walk 관련)

| 컴포넌트 | 파일:라인 | 현재 상태 | 비고 |
|---------|----------|----------|------|
| `cycle_backward_walk_engine()` | `dependency_chain_cache.c:232-244` | 코어당 1개 엔진, 순차 처리 | 다중 H2P chain을 순차적으로 생성하므로 병목 가능 |
| `add_dependency_chain()` | `dependency_chain_cache.c:112-119` | 스냅샷 내 마지막 H2P만 처리 | 스냅샷 내 여러 H2P가 있어도 1개만 chain 생성 |

**영향**: BW Walk는 TEA thread의 "백그라운드" 파이프라인이므로
다중 H2P 동시 **처리**와는 직접적 관련 없음.
단, 여러 H2P의 dep chain이 cache에 적재되는 속도에 영향을 줌.

---

## 5. Shadow RAT 관련 설계 결정

**현재 구현**: Shadow RAT는 코어당 1개 (`tea_rename.c:176`, `tea_rename.h:99`)

**다중 H2P에서의 설계**: Shadow RAT는 **하나를 공유하며 순차적으로 갱신** (논문과 동일).

| 항목 | 설명 |
|------|------|
| snapshot 시점 | 첫 번째 chain 활성화 시 1회만 수행 |
| 갱신 방식 | Chain A rename → Shadow RAT 갱신 → Chain B rename 시 A의 변경 반영됨 |
| 논문 근거 | "순차 fetch, 중첩 실행" — Shadow RAT는 모든 chain이 공유 |
| preg pool | 하나의 TEA preg pool을 모든 chain이 공유 (per-chain 분할 안 함) |

이 설계는 `TEA_op_manage_plan.md` Section 5와
`TEA_shadow_ftq_plan.md` Section 12.3에서도 확인됨.

---

## 6. 전제조건 (다중 H2P 구현 전 필요)

| 전제 작업 | 상태 | 설명 |
|----------|------|------|
| 작업 A: BW Walk Trigger | ✅ 완료 | `fill_buffer.c:59-71` — H2P eviction 시 `engine->state = BW_WALKING` 설정 |
| 작업 G: TEA 의존성 Wakeup | ✅ 완료 | Shadow RAT producer 추적 + `add_to_wake_up_lists()` (`tea_rename.c:502-593`) |
| 작업 I: 독립 Dispatch | ✅ 완료 | `tea_dispatch_to_rs()` 직접 RS dispatch, `tea_dispatch_retry()`, `node_issue_queue_dispatch()`에서 `thread_id==1` skip |
| Op Pool Backpressure | ✅ 완료 | `rename->sd.op_count > 0` stall (`tea_rename.c:443`), `tea_fetch->sd.op_count > 0` stall (`tea_fetch_stage.c:152`) |
| 단일 H2P 시뮬레이션 검증 | ✅ 완료 | blender simpoint 25328 정상 완료 (2026-04-12) |

다중 H2P 구현(작업 F)은 단일 H2P의 정상 동작이 검증된 상태에서 진행.
현재 blender 기준 96.2% TEA_TRIGGER_SKIP_ACTIVE → Work F가 핵심 성능 개선 포인트.
(`TEA_implementation_plan.md` §3 우선순위 참조)
