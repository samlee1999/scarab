# TEA Op 관리: 현재 구현 상태

> **설계 목표**: 논문 Section III (TEA Thread) 및 Section IV-E (TEA thread backend)의 동작을 모델링.
> TEA 스레드와 Main 스레드가 Physical Registers, Reservation Stations, Execution Units를 공유하며,
> TEA Op을 별도 식별자로 구분하여 독립적 생명주기를 관리한다.

---

## 1. TEA Op 식별 메커니즘 — ✅ 구현 완료

논문에서는 멀티스레드 프로세서의 Thread ID로 TEA/Main을 구분하지만,
Scarab은 싱글스레드 시뮬레이터이므로 아래 별도 메커니즘을 사용한다.

| 메커니즘 | 구현 위치 | 상세 |
|---------|----------|------|
| `op->thread_id = 1` | `tea_fetch_stage.c:208` | TEA ops는 thread_id=1, Main은 0 |
| `tea_op_counter = 0x8000000000000000ULL` | `tea_thread.c:90` | Main op_num과 충돌 방지 (TEA Op은 별도 번호 공간 사용) |
| `tea->main_h2p_op` 포인터 | `tea_thread.c:131` | TEA H2P op ↔ Main H2P op 연결 (논문 III-B: synchronized timestamps) |

**Op 구조체** (`op.h:126`): `Op_struct`에 `thread_id` 필드(`op.h:135`)가 이미 존재.
TEA fetch 시 `tea_create_op_from_cache()` (`tea_fetch_stage.c:189`)에서 `thread_id = 1` 할당.

---

## 2. TEA Op 생성 (Fetch → Rename) — ✅ 구현 완료

논문 IV-D: "TEA thread is initiated on a hit in the Block Cache. The uops read out are rotated and
sent directly to the shadow Rename stage."

### 2.1 TEA Fetch Stage (`tea_fetch_stage.c`)

- **`update_tea_fetch_stage()`** (`tea_fetch_stage.c:113`): TEA_FETCHING 상태일 때 매 cycle 실행
  - Block Cache에서 dependency chain 조회 (`get_dependency_chain()`)
  - `TEA_FETCH_WIDTH` 개씩 fetch → `tea_create_op_from_cache()` 호출
  - `fetch_complete = TRUE` 시 TEA_EXECUTING으로 전이

- **`tea_create_op_from_cache()`** (`tea_fetch_stage.c:189`):
  ```c
  Op* tea_op = alloc_op(proc_id);      // op pool에서 할당
  tea_op->thread_id = 1;                // TEA 식별
  tea_op->op_num = tea->tea_op_counter++;  // 별도 op_num 공간
  tea->tea_op_count++;                  // 파이프라인 내 TEA op 수 추적
  ```
  - H2P branch op: `oracle_info`/`recovery_info`를 trigger 시점의 Main H2P에서 복사 (논문 IV-F: synchronized timestamps)
  - Non-H2P chain op: `oracle_info`/`recovery_info` 불필요

### 2.2 TEA Rename Stage (`tea_rename.c`)

- **Shadow RAT** (`tea_rename.c:176`, `shadow_rat_snapshot()`): TEA trigger 시 Main RAT → Shadow RAT 복사
  - 논문 IV-D: "The contents of the main RAT are copied into the shadow RAT before the first TEA
    thread instruction is renamed to synchronize the state of both threads."
- **`tea_rename_op()`** (`tea_rename.c:439`): Shadow RAT에서 src 레지스터 읽기 → TEA preg pool에서 dst 할당
- **TEA Preg Pool** (`tea_rename.c:296-361`):
  - 논문 IV-E: "192 Physical Registers are reserved for the TEA thread when it is active."
  - `TEA_PREG_RESERVATION` 개를 Main free list에서 분리하여 TEA 전용 pool 구성
  - `reset_tea_preg_pool()` (`tea_rename.c:334`): TEA 종료 시 전체 preg pool 리셋

---

## 3. TEA Op 디스패치 (Node Table) — ⚠️ 아키텍처 결함 발견, 독립 dispatch 전환 필요

논문 IV-D: "TEA thread instructions are sent to the Issue logic."
논문 IV-E: "192 Reservation Stations are reserved for TEA threads when TEA is active."

### 3.1 Node Table 진입 (`node_stage.c`)

- **`tea_dispatch_to_rs()`** (`node_stage.c:401`): TEA rename 출력을 Node Table에 삽입
  - Node Table linked list에 추가 (`node_head`/`node_tail`)
  - **node_count 증가 안 함**: TEA op은 ROB 카운트에서 제외 (논문: TEA ops do not enter the ROB)
  - `op->state = OS_IN_ROB` → 이후 `node_issue_queue_dispatch`에서 RS 진입

### 3.2 flush_window에서 TEA 건너뛰기 (`node_stage.c:240-251`)

```c
if (op->thread_id == 1) {
  last = &op->next_node;
  node->node_tail = op;
  continue;  // TEA ops는 FLUSH_OP() 비교 대상이 아님
}
```
TEA op_num이 Main op_num과 다른 공간이므로 `FLUSH_OP()` 매크로의
`recovery_op_num` 비교가 의미없음 → TEA ops는 `flush_tea_ops_from_node_stage()`로 별도 처리.

### 3.3 Dispatch 정책 (`node_issue_queue.cc:284-344`) — ⚠️ 아키텍처 결함 발견 (2026-03-17)

#### 현재 구현

`tea_dispatch_to_rs()` (`node_stage.c:401`)가 TEA ops를 Main ops와 **동일한 Node Table linked list**에 삽입하고,
`node_issue_queue_dispatch()`가 `next_op_into_rs`부터 **단일 in-order 순회**로 양쪽 스레드의 ops를 RS에 dispatch한다.

```
update_node_stage():
  (1) tea_dispatch_to_rs()  → TEA ops를 node table tail에 삽입 (state = OS_IN_ROB)
  (2) node_fill_rob()       → Main ops를 node table tail에 삽입
  (3) node_issue_queue_update():
      - node_issue_queue_clear()     → ready list에서 OS_SCHEDULED/OS_MISS → rs_op_count--
      - node_issue_queue_dispatch()  → next_op_into_rs부터 순회, OS_IN_ROB → OS_IN_RS
      - node_issue_queue_schedule()  → ready list → FU 배정 (TEA 우선 2-pass ✅)
```

#### 아키텍처 결함: 단일 dispatch 스트림에서의 인위적 순서 의존성

**논문 Section IV-D**: "After renaming, TEA thread instructions are sent to the Issue logic which picks
between the **two 8-wide Rename output stages**. The Issue logic is 8-wide but **prioritizes TEA thread
instructions** and uses the leftover Issue slots for the main thread."

**논문 Section IV-E**: "TEA thread instructions **do not enter the ROB**."

논문에서 TEA 스레드와 Main 스레드는:
- 각자 독립된 Rename 출력을 가짐
- Issue 로직에서 converge하되, **독립적으로** RS에 dispatch됨
- TEA ops 간 ordering과 Main ops 간 ordering은 각각 유지되나, **TEA↔Main 간에는 ordering이 없음**

현재 구현에서는 Node Table의 단일 linked list에 양쪽 ops가 interleaving되고,
`next_op_into_rs` 포인터가 하나의 순차 순회를 강제하여 인위적인 순서 의존성을 만든다.

#### 부작용: ASSERT 1, ASSERT 2 (2026-03-17 발견)

**ASSERT 1** (`node_issue_queue.cc:308`): `op->state == OS_IN_ROB`
- **simpoint**: leela 118750, **cycle**: 42133
- **원인**: TEA op이 dispatch 실패(RS partition full) → `first_undispatched = TEA_A` →
  뒤의 Main ops(Main_B, Main_C)는 dispatch 성공(OS_IN_RS) → `next_op_into_rs = TEA_A`(first_undispatched) →
  **다음 cycle에 이미 dispatch된 Main_B, Main_C를 재방문** → `ASSERT(state == OS_IN_ROB)` 실패
- **cycle-by-cycle 재현**:
  ```
  Cycle N:
    Node Table: ... → [TEA_A: OS_IN_ROB] → [Main_B: OS_IN_ROB] → [Main_C: OS_IN_ROB] → ...
                             ↑ next_op_into_rs
    Dispatch: TEA_A skip(continue) → Main_B dispatch(OS_IN_RS) → Main_C dispatch(OS_IN_RS)
    next_op_into_rs = first_undispatched = TEA_A  ← ★

  Cycle N+1:
    Node Table: ... → [TEA_A: OS_IN_ROB] → [Main_B: OS_IN_RS❗] → [Main_C: OS_IN_RS❗] → ...
                             ↑ next_op_into_rs (여전히 TEA_A)
    Dispatch: TEA_A skip → Main_B → ASSERT(state == OS_IN_ROB) 실패!
  ```

**ASSERT 2** (`node_issue_queue.cc:262`): `rs_op_count > 0`
- **simpoint**: leela 128383, **cycle**: 30322
- **원인**: ASSERT 1과 동일한 근본 원인(interleaved dispatch)의 다른 발현 형태.
  `next_op_into_rs`가 이미 dispatch된 Main op을 가리키는 상태에서 flush 또는 후속 dispatch가
  RS 카운터를 비정상적으로 조작 → underflow 발생

#### 이전 수정 시도 및 revert (2026-03-17)

ASSERT 1 해결을 위해 dispatch 루프 진입 시 `state != OS_IN_ROB → continue` 가드를 추가했으나,
이는 **band-aid 수정**으로 판명:
- 증상만 숨기고 근본 원인(단일 dispatch 스트림에 TEA/Main 혼재)을 해결하지 않음
- 논문의 독립 dispatch 모델과 불일치
- **revert 완료 (2026-03-17)**

#### 근본 해결 방향

`tea_dispatch_to_rs()`에서 TEA ops를 **직접 RS에 dispatch** (OS_IN_ROB → OS_IN_RS 전이,
rs_op_count/tea_op_count 증가, ready list 등록)하여 `node_issue_queue_dispatch()`를 우회.
TEA ops는 여전히 Node Table에 남아 scheduling/wakeup/flush에 참여하되,
`next_op_into_rs` 순회에서는 제외. → `TEA_dispatch_plan.md` 참조

### 3.4 RS 파티셔닝 (`node_stage.h:38-52`)

```c
typedef struct Reservation_Station_struct {
  uns32 rs_op_count;    // 전체 ops
  uns32 main_op_count;  // Main thread ops
  uns32 tea_op_count;   // TEA thread ops
  uns32 main_rs_limit;  // Main 최대 = size - TEA_RS_RESERVATION/NUM_RS
  uns32 tea_rs_limit;   // TEA 최대 = TEA_RS_RESERVATION/NUM_RS
} Reservation_Station;
```

---

## 4. TEA Op 실행 (Exec → Dcache) — ✅ 구현 완료

### 4.1 Exec Stage (`exec_stage.c:472-500`)

- **비메모리 TEA op**: exec 완료 시 `tea_op_completed()` 호출 → `tea_op_count--`
  ```c
  if (op && TEA_ENABLE && op->thread_id == 1) {
    if (op->table_info->mem_type != NOT_MEM) return;  // 메모리 ops는 dcache로
    tea_op_completed(exec->proc_id, op);
    exec->sd.ops[ii] = NULL;
    exec->sd.op_count--;
    return;
  }
  ```
- **H2P branch 해결** (`exec_stage.c:554-627`): TEA H2P branch 실행 완료 시 early flush 판단
  - 논문 IV-F: mispred 감지 → `bp_sched_recovery()` via Main H2P op

### 4.2 Dcache Stage (`dcache_stage.c:232-278`)

- **TEA Store**: `tea_store_buffer_write()` 호출 (논문 IV-E: "16-entry store data cache")
  - 버퍼 full → `terminate_tea_thread()` 호출 후 즉시 `continue` (freed op 접근 방지)
  - 성공 시: `done_cycle` 설정 + `tea_op_completed()` — `tea_op_completed()` 내부에서 `op->state = OS_DONE` 설정
- **TEA Load (buffer hit)**: Store buffer forwarding 성공 시 `done_cycle` 설정 + `wake_up_ops()` + `tea_op_completed()` (→ `OS_DONE`)
- **TEA Load (buffer miss)**: D-cache 정상 접근 (read-only) → `goto tea_load_dcache_access`
  - cache hit: `dcache_cacheline_hit()` 내부에서 `done_cycle`/`wake_cycle` 설정 + `wake_up_ops()` 호출 후 `tea_op_completed()` (→ `OS_DONE`)
  - cache miss fill: fill 콜백에서 `wake_up_ops()` + `tea_op_completed()` (→ `OS_DONE`)
  - store fwd hit on miss: `wake_up_ops()` + `tea_op_completed()` (→ `OS_DONE`)
  - 논문 IV-E: "TEA loads are similar to a prefetch that speculatively brings data into the D-cache."
  - **latency 보장**: `done_cycle`/`wake_cycle`은 실제 DCache/Memory latency 기반으로 설정되므로, `OS_DONE` 설정 시점이 곧 실제 완료 시점임. 의존 TEA ops는 `wake_up_ops()`로 올바른 시점에 깨어남.

---

## 5. TEA Op 정리 (Retirement / Termination) — ✅ 구현 완료

### 5.1 정상 경로: Op 완료 → Retire

```
비메모리 op:  exec_stage_clear_fu()
                → tea_op_completed()
                    → tea->tea_op_count--
                    → op->state = OS_DONE       ★ (2026-04-01 수정)
                → exec->sd.ops[ii] = NULL
              (같은 cycle, exec→node 실행 순서이므로 node retire에서 OS_DONE 체크 안전)

메모리 op:    dcache_stage (store buffer / dcache hit / miss fill)
                → done_cycle, wake_cycle 설정 (실제 DCache/Memory latency 반영)
                → wake_up_ops() (의존 TEA ops 올바른 시점에 깨움)
                → tea_op_completed()
                    → tea->tea_op_count--
                    → op->state = OS_DONE       ★ (2026-04-01 수정)
              (같은 cycle, dcache→node 실행 순서이므로 node retire에서 OS_DONE 체크 안전)
     ↓
node_retire_tea_ops():
  비메모리: (OP_DONE || OS_DONE) — 두 조건 모두 같은 cycle에 참
  메모리:   OS_DONE 전용 체크   ★ (2026-04-01 수정, OP_DONE 제거)
  → node table에서 제거 + free_op()
  (tea_op_count 감소 없음 — tea_op_completed()가 이미 처리함)
     ↓
update_tea_thread() → tea_op_count == 0 → terminate_tea_thread()
```

- **`tea_op_completed()`** (`tea_thread.c:251-270`): `tea_op_count--` + **`op->state = OS_DONE`** + `TEA_OPS_EXECUTED` stat
  - 비메모리: `exec_stage_clear_fu()`에서 호출 (`exec_stage.c:485`)
  - 메모리 (TEA store buffer path): `dcache_stage.c:269`에서 호출
  - 메모리 (normal dcache path): `dcache_stage.c` cache hit(`329`,`338`)/miss fill(`910`)/store fwd hit(`661`) 각 경로에서 호출
- **`node_retire_tea_ops()`** (`node_stage.c:659-715`): 매 cycle Node Table 순회
  - 비메모리 TEA op: `(OP_DONE(op) || op->state == OS_DONE)` — 기존과 동일, exec이 done_cycle 설정하므로 OP_DONE도 유효
  - **메모리 TEA op: `op->state == OS_DONE` 전용** — exec은 mem op의 done_cycle을 설정하지 않으므로
    OP_DONE은 MAX_CTR 기준이 되어 신뢰 불가. dcache가 tea_op_completed()를 호출하여 OS_DONE을 설정한 이후에만 retire.
  - **tea_op_count 감소 없음**: `tea_op_completed()`가 이미 처리했으므로 여기서 감소하면 이중 감소

### 5.2 종료 경로: terminate_tea_thread()

```
terminate_tea_thread() (tea_thread.c:154-185)
  │
  ├─ recover_tea_fetch_stage()    — fetch 중인 ops → free_op()
  │   (tea_fetch_stage.c:242-261)
  │
  ├─ recover_tea_rename_stage()   — rename 중인 ops → free_op()
  │   (tea_rename.c:506-509 → reset_tea_rename_stage():163-171)
  │
  ├─ flush_tea_ops_from_node_stage() — 5단계 일괄 정리
  │   (node_stage.c:1005-1094)
  │   ├─ 0a. exec_stage->sd.ops[]에서 thread_id==1 → NULL (free 안 함)
  │   ├─ 0b. dcache_stage->sd.ops[]에서 thread_id==1 → NULL (free 안 함)
  │   ├─ 1. ready list에서 thread_id==1 제거 (free 안 함)
  │   ├─ 2. scheduling buffer에서 thread_id==1 제거 (free 안 함)
  │   ├─ 3. next_op_into_rs: TEA op을 가리키면 다음 non-TEA op으로 전진 (free 안 함)
  │   └─ 4. node table 순회: thread_id==1 → RS 카운터 감소 + free_op()
  │       RS 카운터 감소 조건: state != OS_IN_ROB && != OS_SCHEDULED && != OS_MISS && != OS_DONE
  │       ⚠️ OS_SCHEDULED/OS_MISS는 same-cycle에서는 step 1에서, cross-cycle에서는
  │       이전 cycle의 clear()에서 이미 처리됨 → §8.5 참조
  │
  ├─ reset_tea_preg_pool()        — 물리 레지스터 전체 반환
  │   (tea_rename.c:334-361)
  │
  └─ reset_tea_store_buffer()     — 스토어 버퍼 초기화
      (tea_store_buffer.c:76-90)

최종 상태 리셋:
  tea->state = TEA_IDLE
  tea->tea_op_count = 0
  tea->tea_ops_fetched = 0
  tea->main_h2p_op = NULL  (dangling reference 방지)
```

**핵심 관찰**: 모든 정리 함수가 `thread_id == 1` 조건으로 **전체 TEA Op을 일괄 처리**.
현재는 단일 H2P chain만 지원하므로 이 방식이 정확하게 동작함.

**⚠️ 코드 중복 참고**: `recover_tea_on_flush()` (`cmp_model.c:389-399`)는 `terminate_tea_thread()` 호출 후
추가로 `reset_tea_fetch_stage()`, `reset_tea_rename_stage()`, `reset_tea_preg_pool()`을 호출.
`terminate_tea_thread()`가 이미 `recover_tea_fetch_stage()`, `recover_tea_rename_stage()`,
`reset_tea_preg_pool()`을 호출하므로, 후속 호출은 이중 실행.
`recover_*`와 `reset_*`이 서로 다른 함수라면 의도적이나, `reset_tea_preg_pool()`은 동일 함수가 2회 호출됨 (idempotent이므로 정확성 영향 없음).

---

## 6. TEA Op 통계 (`tea.stat.def`)

| Stat | 위치 | 설명 |
|------|------|------|
| `TEA_OPS_FETCHED` | `tea_fetch_stage.c:177` | fetch된 TEA op 수 |
| `TEA_OPS_DISPATCHED` | `node_stage.c:455` | Node Table에 진입한 TEA op 수 |
| `TEA_OPS_ISSUED` | issue 시점 | RS에서 스케줄된 TEA op 수 |
| `TEA_OPS_EXECUTED` | `tea_thread.c:262` | 실행 완료된 TEA op 수 |
| `TEA_OPS_RETIRED` | `node_stage.c:600` | Node Table에서 retire된 TEA op 수 |
| `TEA_OPS_FLUSHED` | `node_stage.c:1085` | terminate 시 flush된 TEA op 수 |
| `TEA_PREGS_ALLOCATED` | `tea_rename.c:489` | TEA preg pool에서 할당된 레지스터 수 |
| `TEA_STORES_BUFFERED` | `dcache_stage.c:248` | TEA store buffer에 기록된 store 수 |
| `TEA_STORE_FORWARDS` | `dcache_stage.c:260` | Store→Load forwarding 성공 수 |

---

## 7. 알려진 제한사항

1. **단일 H2P만 지원**: 현재 `Tea_Thread` 구조체에 chain 배열 없음. TEA가 active이면 새 H2P trigger는 skip됨 (`trigger_tea_thread()` → `TEA_TRIGGER_SKIP_ACTIVE`)
2. ~~**`node_retire_tea_ops()`에서 `tea_op_completed()` 미호출**~~ → ✅ **해결됨**: 메모리 TEA ops에 대해 `dcache_stage.c`에서 `tea_op_completed()` 호출 추가. 비메모리 ops는 기존 `exec_stage_clear_fu()`에서 호출. `node_retire_tea_ops()`는 retire만 담당하고 카운터는 건드리지 않음 (이중 감소 방지)
3. **TEA preg pool 공유**: 전체 TEA 스레드가 하나의 preg pool 공유. 개별 chain의 preg 반환 불가 (Shadow RAT dangling 매핑 위험)
4. **TEA RS Dispatch 정책**: `TEA_RS_RESERVATION`(192)을 `NUM_RS`(3)으로 균등 분배 (per-RS 64개, RS[2]는 50% cap=18). TEA ops는 대부분 ALU 연산 → RS[0]에 집중되지만, RS[1]/RS[2]의 TEA 슬롯은 거의 미사용. 성능 최적화 여지 있음 (정확성 문제는 아님).
5. **⚠️ TEA/Main 공유 dispatch 스트림**: TEA ops가 Main ops와 동일한 `next_op_into_rs` 순차 순회에 포함되어 인위적 순서 의존성 발생 → ASSERT 1 & 2의 근본 원인. 독립 dispatch로 전환 필요 (`TEA_dispatch_plan.md` 참조).

---

## 8. 버그 수정 이력

### 8.1 RS `tea_op_count` 카운터 누수 (2026-03-16, 2026-03-17 재수정)

**증상**: `flush_tea_ops_from_node_stage()` 호출 후에도 `rs[].tea_op_count > 0` 유지 → TEA RS 슬롯이 영구 점유 → Main ops의 RS 가용 공간 감소 → 장기 시뮬레이션에서 파이프라인 병목/데드락.

**1차 수정 (2026-03-16)**: Step 4의 RS 카운터 감소 조건을 3개 positive 상태에서 4개 negative 조건으로 확장:
```c
// v1 (buggy): 3개 상태만 확인
if (op->state == OS_IN_RS || op->state == OS_READY || op->state == OS_WAIT_FWD) {
  rs->rs_op_count--; rs->tea_op_count--;
}
// v2 (intermediate fix): OS_TENTATIVE/WAIT_DCACHE/WAIT_MEM도 포함
if (op->state != OS_IN_ROB && op->state != OS_SCHEDULED &&
    op->state != OS_MISS && op->state != OS_DONE) {
```
OS_SCHEDULED/OS_MISS를 제외한 이유: `node_issue_queue_clear()`가 처리한다고 가정.

**2차 수정 시도 (2026-03-17, revert됨)**: "flush는 항상 clear()보다 먼저 실행"이라는 전제하에
OS_SCHEDULED/OS_MISS도 flush에서 감소하도록 조건을 `!= OS_IN_ROB && != OS_DONE`으로 확장:
```c
// v3 (reverted — double decrement 버그): OS_IN_ROB과 OS_DONE만 제외
if (op->state != OS_IN_ROB && op->state != OS_DONE) { ... }
```
**이 전제는 같은 cycle 내에서만 성립함.**

cross-cycle 시나리오에서 clear()가 이전 cycle에 이미 처리한 OS_SCHEDULED/OS_MISS ops를
다음 cycle의 flush가 다시 감소 → **double decrement** → `flush_window()`에서 Main op의
`rs_op_count > 0` ASSERT 실패. 상세 분석은 §8.5 참조.

**현재 코드 (v2)**: OS_SCHEDULED/OS_MISS 제외가 올바름. 단, same-cycle flush에서의
OS_SCHEDULED/OS_MISS 처리는 별도 해결 필요 → `TEA_dispatch_plan.md` §10 참조.
```c
// v2 (current):
if (op->state != OS_IN_ROB && op->state != OS_SCHEDULED &&
    op->state != OS_MISS && op->state != OS_DONE) {
```

### 8.2 TEA ops가 Main dispatch를 차단 (2026-03-16)

**증상**: TEA ops가 RS partition full (tea_op_count >= tea_rs_limit)로 dispatch 실패할 때,
`node_issue_queue_dispatch()`가 `break`하여 뒤에 있는 Main ops도 dispatch 불가.
Node Table에서 TEA ops와 Main ops가 interleaving되므로, 특히 TEA RS 슬롯이 포화된 상태에서
Main 파이프라인이 정체됨.

**수정** (`node_issue_queue.cc:292-300`):
```c
if (rs_id == NODE_ISSUE_QUEUE_RS_SLOT_INVALID) {
  if (!first_undispatched)
    first_undispatched = op;
  if (op->thread_id == 1)
    continue;  // TEA ops: skip, don't block main
  break;       // Main ops: preserve in-order dispatch
}
```
**⚠️ 주의**: 이 수정은 Main ops blocking 문제를 해결하지만, `next_op_into_rs`가
skip된 TEA op으로 되돌아가는 부작용을 유발 → ASSERT 1 발생 원인. 이 문제는 독립
dispatch 구현(§3.3, `TEA_dispatch_plan.md`)으로 근본 해결 예정.

### 8.3 `next_op_into_rs`가 TEA op을 가리킬 때 NULL 설정 (2026-03-16)

**증상**: `flush_tea_ops_from_node_stage()` step 3에서 `next_op_into_rs`가 TEA op을 가리키면
NULL로 설정 → 다음 cycle dispatch가 Node Table 순회를 건너뜀 → Main ops dispatch 지연.

**수정** (`node_stage.c:1058-1067`):
```c
// BEFORE (buggy):
if (op && op->thread_id == 1) {
  node_local->next_op_into_rs = NULL;
}

// AFTER (fixed):
if (op && op->thread_id == 1) {
  Op* next = op->next_node;
  while (next && next->thread_id == 1)
    next = next->next_node;
  node_local->next_op_into_rs = next;  /* NULL if no main ops follow */
}
```

### 8.4 ASSERT 1 & 2: Interleaved dispatch 아키텍처 결함 (2026-03-17 발견, 미해결)

**ASSERT 1** (`node_issue_queue.cc:308`): `op->state == OS_IN_ROB` — leela simpoint 118750, C=42133
**ASSERT 2** (`node_issue_queue.cc:262`): `rs_op_count > 0` — leela simpoint 128383, C=30322

**근본 원인**: TEA ops와 Main ops가 동일한 Node Table linked list에서 단일 `next_op_into_rs`로
순차 dispatch됨. TEA op skip → `first_undispatched`가 TEA op 기억 → `next_op_into_rs` 복귀 →
이미 dispatch된 Main ops 재방문. 상세 분석은 §3.3 참조.

**band-aid 수정 시도 (revert됨)**: `state != OS_IN_ROB → continue` 가드를 dispatch 루프에 추가했으나,
논문의 독립 dispatch 모델과 불일치하여 revert.

**해결 계획**: `tea_dispatch_to_rs()`에서 TEA ops를 직접 RS에 dispatch하여 `node_issue_queue_dispatch()`를
완전히 우회. → `TEA_dispatch_plan.md` 참조

### 8.5 flush step 4 RS 카운터 double decrement (2026-03-17 발견, 미해결)

**ASSERT** (`node_stage.c:261`): `node->rs[op->rs_id].rs_op_count > 0` — 모든 벤치마크에서 발생

**발생 위치**: `flush_window()` — Main thread recovery flush 시 Main op의 RS 카운터 감소 경로

**근본 원인**: `flush_tea_ops_from_node_stage()` step 4에서 OS_SCHEDULED/OS_MISS TEA ops의
RS 카운터를 감소할 때, **같은 cycle** vs **다른 cycle** 구분 불가로 double decrement 발생.

**상세 시나리오** (cross-cycle double decrement):

```
Cycle N-1:
  cmp_cores() → update_node_stage():
    node_issue_queue_clear():
      TEA op (OS_SCHEDULED) in ready list → rs_op_count--, tea_op_count--  ✓
      TEA op removed from ready list, in_rdy_list = FALSE
    node_issue_queue_schedule(): ...
  cmp_cores() → update_exec_stage():
    TEA H2P 실행 → bp_sched_recovery(recovery_cycle = N)

Cycle N:
  cmp_istreams(): recovery_cycle == N → cmp_recover():
    recover_tea_on_flush() → flush_tea_ops_from_node_stage():
      Step 1: TEA op NOT in ready list (이미 clear()가 Cycle N-1에서 제거) → skip
      Step 4: TEA op state = OS_SCHEDULED
              v3 조건 (!= OS_IN_ROB && != OS_DONE) → TRUE
              → rs_op_count-- 다시! ← ★ DOUBLE DECREMENT
              → rs_op_count가 실제보다 1 낮아짐

    recover_node_stage() → flush_window():
      Main op (OS_IN_RS) 같은 RS → ASSERT(rs_op_count > 0) 실패!
```

**같은 cycle에서는 문제 없는 이유**:
flush가 clear()보다 먼저 실행되므로:
- Step 1: TEA op이 ready list에 있음 → 제거
- Step 4: 감소 (clear()가 아직 안 돌았으므로 최초 감소)
- 이후 clear(): TEA op은 ready list에 없음 → skip → 이중 감소 없음

**v2 조건에서의 same-cycle 문제** (RS 카운터 leak):
```
같은 cycle에서 flush가 먼저 실행되는 경우:
  Step 1: TEA op (OS_SCHEDULED)이 ready list에 있음 → 제거 (in_rdy_list = FALSE)
  Step 4: v2 조건 (!= OS_SCHEDULED) → skip → 카운터 미감소
  이후 clear(): TEA op은 ready list에 없음 → skip → 카운터 미감소
  → RS 카운터가 감소되지 않음 = leak
```

**현재 상태**: v2 조건으로 복귀. Cross-cycle double decrement는 해결되었으나,
same-cycle의 OS_SCHEDULED/OS_MISS leak은 미해결. 이 leak은 TEA RS 슬롯이
영구 점유되는 문제를 유발할 수 있으나, TEA 종료 시 `tea_op_count`가 전체 리셋되므로
실제 영향은 TEA가 active인 동안의 일시적 RS 가용 공간 감소에 한정.

**해결 방향**: flush step 1에서 ready list 제거 시 OS_SCHEDULED/OS_MISS TEA ops의 RS 카운터를
함께 감소. 이렇게 하면 step 4에서 이 상태들을 제외해도 안전.
→ `TEA_dispatch_plan.md` §10 참조

### 8.6 `node_retire_tea_ops()`의 `tea_op_count` 이중 감소 → op pool 고갈 (2026-03-27 발견, ✅ 수정 완료)

**증상**: blender 벤치마크에서 op pool 고갈 크래시.
```
op_pool_entries: 16512, limit: 16384
Active ops: 16384, Tracked in pipeline: 568
UNACCOUNTED ops: 15816  ← node table의 orphan TEA ops
```

**근본 원인**: `node_retire_tea_ops()` (`node_stage.c:676-679`)에서 `tea_op_count--`를 호출했지만,
이 시점에는 `tea_op_completed()`가 이미 `tea_op_count--`를 한 번 처리한 상태임 (이중 감소).

**이중 감소 연쇄 실패 시나리오** (N=4 ops 예시):
```
Initial: tea_op_count = 4

Cycle A: op1 exec + retire (같은 cycle):
  exec_stage: tea_op_completed() → tea_op_count = 3
  node_retire: tea_op_count-- → tea_op_count = 2  ← 이중!

Cycle B: op2 exec + retire:
  exec_stage: tea_op_count = 1
  node_retire: tea_op_count = 0 ← op3, op4 아직 node table에 있는데 count=0!

Cycle B의 update_map_stage():
  H2P branch arrive → trigger_tea_thread() → tea_ops_fetched = 0 (리셋!)

Cycle B의 update_tea_thread():
  tea_op_count==0 && tea_ops_fetched==0 → 조건 불만족 → terminate 안 함!
  → op3, op4가 node table에 orphan으로 남음
```

이 패턴이 반복되면 orphan ops가 node table에 누적. 각 새 TEA trigger가
`tea_ops_fetched`를 리셋하므로 `terminate_tea_thread()`가 호출되지 않아
이전 chain의 ops가 영원히 남음 → op pool 고갈.

**수정** (`node_stage.c:674-679`, 2026-03-27):
- `tea_op_count--` 블록 제거
- `tea_op_count` 관리는 전적으로 `tea_op_completed()` / `terminate_tea_thread()`가 담당

**수정 후 올바른 lifecycle**:
```
tea_fetch_stage: tea_op_count++   (alloc)
exec/dcache: tea_op_completed() → tea_op_count--  (completion)
node_retire_tea_ops(): free_op() 만 수행 (count 건드리지 않음)
update_tea_thread(): tea_op_count==0 → terminate_tea_thread() → count reset to 0
```

---

### 8.7 TEA Mem Op 조기 Retire 및 OS_DONE 게이트 도입 (2026-04-01)

**현상**: Phase 13 시뮬레이션에서 leela 벤치마크 TEA ON IPC -65.5% (TEA OFF 대비).
TEA가 92.3%의 cycle 동안 TEA_EXECUTING 상태를 유지하며 RS 192 슬롯을 점유 →
Main RS = 324 (baseline 352 대비 축소) → preg 고갈 → MAP_STAGE_STALL_ITSELF 87%.

**근본 원인 분석**:

```
exec_stage_process_op() (exec_stage.c:532-551):
  if (op->table_info->mem_type == NOT_MEM)
      op->done_cycle = op->exec_cycle;   ← 비메모리만 done_cycle 설정
  // 메모리 ops: done_cycle = MAX_CTR 유지 (exec은 주소 계산 latency만 담당)
```

메모리 TEA op의 `done_cycle`은 exec이 설정하지 않으므로 초기값 `MAX_CTR`.
`node_retire_tea_ops()`의 기존 조건 `OP_DONE(op) = (cycle_count >= done_cycle)`은
dcache가 `done_cycle`을 실제 latency로 업데이트한 이후에야 참이 됨.

그러나 dcache가 `done_cycle`을 설정해도 `op->state == OS_DONE`은 어디서도 설정되지 않았으므로,
`OS_DONE` 경로 또한 발동하지 않음 → **TEA 메모리 op이 node table에 무기한 잔류**.

Tea_op_count는 dcache가 `tea_op_completed()`를 올바르게 호출하여 감소되지만,
node table에 남아있는 op들은 retire되지 않고 누적됨.

이 상태에서 `terminate_tea_thread()`가 외부 이벤트(early flush 등)로 호출되면
`flush_tea_ops_from_node_stage()`가 node table의 미retire TEA ops를 일괄 해방하지만,
이미 freed된 op의 포인터가 `exec->sd`에 잔류하는 dangling pointer 위험 존재.

**수정 1: `tea_op_completed()`에 `op->state = OS_DONE` 추가** (`tea_thread.c:251-270`):

```c
void tea_op_completed(uns proc_id, Op* op) {
  ...
  if (tea->tea_op_count > 0) tea->tea_op_count--;

  /* op->state = OS_DONE: node_retire_tea_ops()가 이 op을 안전하게 free할 수 있는
   * 권한 부여. 비메모리 ops는 exec(dcache보다 먼저 실행) 이후, 메모리 ops는
   * dcache(node보다 먼저 실행) 이후 설정되므로 같은 cycle node_retire에서 체크 안전. */
  op->state = OS_DONE;

  STAT_EVENT(proc_id, TEA_OPS_EXECUTED);
}
```

**수정 2: `node_retire_tea_ops()`에 mem/non-mem 분리 게이트 도입** (`node_stage.c:665-682`):

```c
Flag tea_done;
if (op->thread_id == 1) {
  if (op->table_info->mem_type != NOT_MEM) {
    tea_done = (op->state == OS_DONE);          /* mem: dcache→tea_op_completed 필수 */
  } else {
    tea_done = (OP_DONE(op) || op->state == OS_DONE);  /* non-mem: exec latency 충분 */
  }
} else {
  tea_done = FALSE;
}
if (tea_done) { ... free_op(op); }
```

**수정의 효과**:
- 메모리 TEA op은 `dcache_stage`가 `tea_op_completed()`를 호출(→ `OS_DONE` 설정)한 cycle과
  동일한 cycle에 node table에서 retire (dcache < node 실행 순서 보장).
- `done_cycle`이 MAX_CTR이거나 잘못 설정된 경우에도 OP_DONE에 의존하지 않음.
- `tea_op_count` 감소와 node retire가 항상 동일 cycle에 발생 → 일관성 보장.
- 논문 IV-E와의 정합성: TEA Load는 DCache/Memory를 실제로 통과하여 올바른 latency를 받으며,
  그 결과가 완료된 시점에만 retire.

**검증 포인트** (시뮬레이션 후):
- `TEA_OPS_RETIRED ≈ TEA_OPS_EXECUTED` (flush로 종료된 건 `TEA_OPS_FLUSHED`에 반영)
- leela MAP_STAGE_STALL_ITSELF 감소 여부
- TEA_EXECUTING 상태 점유 비율 변화
