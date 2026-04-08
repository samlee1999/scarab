# TEA 구현 계획 (TEA Implementation Plan)

**논문**: Timely, Efficient, and Accurate Branch Precomputation (MICRO 2024, UT Austin)
**논문 원본**: /home/lee/scarab/docs/TEA_info/TEA_paper_origin.pdf
**최종 갱신**: 2026-03-18
**베이스라인 아키텍처 코드 베이스**: `/home/lee/scarab/src/`
**베이스라인 아키텍처 코드 베이스 구조 분석**: `/home/lee/scarab/src/research_scarab_src.md`

---

## 1. Scarab TEA 아키텍처 요약

> 이 섹션은 **논문의 설계를 그대로 옮긴 것이 아니라**, Scarab 시뮬레이터 위에 TEA를 어떻게
> 구현하는지를 기술한다. 논문과의 주요 차이점은 Section 3에서 항목별로 정리한다.
>
> **주의**: 이 다이어그램은 **목표 아키텍처** (모든 작업 완료 후)를 기술한다.
> 현재 구현 상태와의 차이는 §2를 참조.

```
═══ 백그라운드: 의존성 체인 사전 구축 (메인 스레드 retire 기반) ═══

  Main Thread Retire
    → node_stage.c: node_retire()
    → fill_buffer.c: fill_buffer_add()
         │
         │  Fill Buffer가 가득 차고 evict되는 op이 H2P이면
         │  (hbt_pred_is_hard == TRUE)
         ▼
  Backward Dataflow Walk 시작 (BACKWARD_WALK_CYCLES 파라미터)
    dependency_chain_cache.c: cycle_backward_walk_engine()
    - Fill Buffer 스냅샷에서 youngest → oldest 순으로 walk
    - H2P branch의 src regs/mem로부터 역방향 의존성 추적
    - 의존 op에 chain_bit 마킹
         │
         ▼
  Dependency Chain Cache에 전체 chain 저장 (H2P PC로 인덱싱)
    dependency_chain_cache.c: add_dependency_chain()
    - 논문의 Block Cache + bitmask OR 조합의 최종 결과물을 사전 계산하여 저장
    - TEA Fetch는 이 캐시만 사용 (Block Cache는 미사용)

═══ Process: TEA 스레드 트리거 및 실행 ═══

  bp.c: bp_predict_op() — HBT가 H2P branch 감지
         │
         ├─ get_dependency_chain(proc_id, h2p_pc) 으로 chain 존재 확인
         │   chain 없으면 TEA_TRIGGER_SKIP_NO_CHAIN → 종료
         │
         ▼
  tea_thread.c: trigger_tea_thread()
    - TEA 상태 → FETCHING
    - Shadow RAT: Main map_data->reg_map[]에서 arch→phys 매핑 + producer op 복사
      (논문의 "Main RAT 스냅샷"이 아닌, producer 추적 포함한 확장 스냅샷)
    - TEA preg pool에서 dst reg 할당 준비
         │
         ▼
  tea_fetch_stage.c: update_tea_fetch_stage()
    - Dep Chain Cache에서 chain[0..N]을 TEA_FETCH_WIDTH개씩 순차 fetch
    - 논문의 Shadow FTQ + Block Cache 조합을 사용하지 않음 (방안 A)
    - 각 op에 thread_id=1, h2p_chain_id 부여
         │
         ▼
  tea_rename.c: tea_rename_op()
    - Shadow RAT에서 src의 producer op 조회 → add_to_wake_up_lists()
      (논문의 Poison bit 대신, 명시적 producer 추적 + wakeup으로 정확한 의존성 모델링)
    - Shadow RAT에서 TEA preg 할당 → dst 매핑 갱신
         │
         ▼
  node_stage.c: tea_dispatch_to_rs()
    - Shared OoO Backend의 RS에 dispatch (TEA 전용 파티션)
    - 2-pass 스케줄링: TEA ops 우선 issue (node_issue_queue.cc)
    - ROB 미사용 (TEA ops는 Node Table에만 존재)
         │
         ▼
  exec_stage.c: TEA H2P branch 실행 시 (exec_stage_bp_resolve)
    - h2p_chain_id로 해당 chain의 Main H2P op 매칭
    - Main H2P가 off_path이면 무시
    - Case 1 (Main H2P가 rename 전, SRT checkpoint 없음):
        main_h2p->decode_cycle 확인하여 분기:
        - Case 1a (Main H2P가 decode 통과): recover_at_exec = TRUE
          → Main H2P exec 시 cmp_recover()로 메인 스레드 recovery
        - Case 1b (Main H2P가 decode 미도달): recover_at_decode = TRUE
          → Main H2P decode 도달 시 recovery 진행 X
        해당 chain만 즉시 종료 (terminate_tea_chain)
    - Case 2 (Main H2P가 rename 후, SRT checkpoint 존재):
        bp_sched_recovery() → 다음 cycle에 cmp_recover() 실행:
          메인 스레드: SRT rollback + BP 복원 + younger ops flush
          TEA 스레드: recover_tea_on_flush(proc_id, recovery_op_num)
            → recovery point보다 younger한 chain만 종료, older chain은 유지
```


**Scarab TEA 주요 설정값** (논문 Table II 대응):
| 구조 | Scarab 설정 | 파라미터/위치 | 논문 |
|------|------------|---------------|------|
| Fill Buffer | 512 uops | `FILL_BUFFER_SIZE` | 512 uops |
| HBT | 1024-entry, 3-bit, 50K decrement | `bp/hbt.h` | 256-entry, 8-way, 3-bit |
| Dep Chain Cache | 1024 entries | `DEPENDENCY_CHAIN_CACHE_SIZE` | — (Block Cache 512+256) |
| Shadow FTQ | **미사용** (방안 A) | — | 128-entry FIFO |
| TEA Physical Regs | 192 reserved | `TEA_PREG_POOL_SIZE` | 192 PR |
| TEA RS Reservation | 파라미터 | `TEA_RS_RESERVATION` | 192 RS |
| TEA Fetch Width | 2-wide | `TEA_FETCH_WIDTH` | 8-wide |
| TEA Store Buffer | 16 entries | `TEA_STORE_BUFFER_SIZE` | 16 half-lines |
| BW Walk Latency | 파라미터 | `BACKWARD_WALK_CYCLES` | ~500 cycles |
| Poison Bit | **미구현** (oracle 사용) | — | 있음 |

---

## 2. 현재 구현 상태 진단

### ✅ 완전 구현된 부분

| 컴포넌트 | 파일 | 상태 |
|---------|------|------|
| HBT (1024-entry, 3-bit, 50K decrement) | `bp/hbt.h/c` | ✅ |
| HBT → TEA trigger 연결 | `bp/bp.c:876` (bp_predict_op_evaluate 이후) | ✅ |
| TEA 상태 기계 (IDLE/FETCHING/EXECUTING) | `tea/tea_thread.h/c` | ✅ (단일 H2P) |
| TEA Fetch Stage (dep cache 조회 → Op 생성) | `tea/tea_fetch_stage.h/c` | ✅ (단일 체인) |
| Shadow RAT + TEA Rename Stage + Dependency Wakeup | `tea/tea_rename.h/c` | ✅ producer 추적 + `add_to_wake_up_lists()` 연결 완료 |
| TEA Physical Register 전용 풀 | `tea/tea_rename.h/c` (Phase 4) | ✅ |
| RS 파티셔닝 (main/tea limits) | `node_stage.h:51-54`, `exec_ports.c:223` | ✅ |
| 2-pass 우선 스케줄링 (TEA first) | `node_issue_queue.cc:395-443` | ✅ |
| TEA Store Buffer (forwarding + full 종료) | `tea/tea_store_buffer.h/c` | ✅ |
| TEA Node Stage dispatch + flush | `node_stage.c:418-525, 1116-1244` | ✅ 독립 dispatch 구현 완료: `tea_dispatch_to_rs()` 직접 RS dispatch, `tea_dispatch_retry()` 신규, `flush_tea_ops_from_node_stage()` step 1 RS 카운터 동기화 완료 ([`TEA_dispatch_plan.md`](TEA_implementation_plan/TEA_dispatch_plan.md)) |
| Early Flush Case 1 (pre-rename: recover_at_decode) | `exec_stage.c:597-602` | ✅ |
| Early Flush Case 2 (post-rename: bp_sched_recovery) | `exec_stage.c:579-596` | ✅ |
| Off-path 가드 (main_h2p->off_path 체크) | `exec_stage.c:573-575` | ✅ |
| reg_file_checkpoint_is_valid() | `map_rename.c:558-567` | ✅ |
| Fill Buffer (circular buffer, retirement 추가) | `fill_buffer.c` | ✅ |
| BW Walk Trigger (H2P eviction → snapshot → walk) | `fill_buffer.c:59-70` | ✅ (oldest H2P chain 미생성 제약) |
| Backward Dataflow Walk 알고리즘 | `dependency_chain_cache.c:92-226` | ✅ |
| Block Cache bitmask OR (multi-path) | `dependency_chain_cache.c:194-214` | ✅ |
| periodically_reset_caches() 함수 | `dependency_chain_cache.c:246-258` | ✅ (미연결) |
| Op pool 디버그 인프라 | `op_pool.c:303-432` | ✅ |

### ❌ 누락 / 미구현 (우선순위 순)

| ID | 누락 항목 | 파일 | 영향 |
|----|---------|------|------|
| **A** | ~~BW Walk Trigger 없음~~ | `fill_buffer.c` | ✅ 구현 완료 (§4 참조, oldest H2P chain 미생성 제약 있음) |
| **G** | ~~TEA 의존성 Wakeup 미연결~~ | `tea_rename.c`, `tea_rename.h` | ✅ 구현 완료 (Shadow RAT producer 추적 + `add_to_wake_up_lists()`) |
| **F** | **다중 H2P+DC 동시 처리 불가** | `tea_thread.h/c`, `tea_fetch_stage.c`, `exec_stage.c` | **Precomputation 효과 저하** |
| **B** | Iterative Walk (chain_bit 활용) | `fill_buffer.c`, `dependency_chain_cache.c` | 긴 체인 추적 불가 |
| **C+HC** | `periodically_reset_caches()` 미호출 + Hybrid Chain 미구현 | `dependency_chain_cache.c/h`, `node_stage.c` | Block Cache stale, dep cache 단일 경로 덮어쓰기 |
| **D** | Poison bit 미구현 (구현 안 할 예정) | — | 사용자 결정: oracle 사용으로 대체 |
| **E** | Partial Frontend Flush 미구현 | — | recover_at_decode로 단순화 완료 |

---

## 3. 논문과의 설계 차이점 분석 (6가지 항목)

### 3.1 항목 1: TEA Op와 Main Op 분리

**현재 구현 상태**: 상세 내용은 [`TEA_op_manage_status.md`](TEA_implementation_status/TEA_op_manage_status.md) 참조
**다중 H2P 구현 계획**: [`TEA_op_manage_plan.md`](TEA_implementation_plan/TEA_op_manage_plan.md) 참조
**독립 Dispatch 계획**: [`TEA_dispatch_plan.md`](TEA_implementation_plan/TEA_dispatch_plan.md) 참조 — ASSERT 1&2 근본 해결

### 3.2 항목 2: Shadow FTQ

**현재 구현 상태**: "Dependency Chain Cache 직접 조회" 방식 채택 — 상세 내용은 [`TEA_shadow_ftq_status.md`](TEA_implementation_status/TEA_shadow_ftq_status.md) 참조
**다중 H2P Fetch 구현 계획**: [`TEA_shadow_ftq_plan.md`](TEA_implementation_plan/TEA_shadow_ftq_plan.md) 참조

### 3.3 항목 3+4: Early Flush → 별도 문서로 분리

- 현재 구현 상태: [`TEA_early_flush_status.md`](TEA_implementation_status/TEA_early_flush_status.md)
- 향후 구현 계획: [`TEA_early_flush_plan.md`](TEA_implementation_plan/TEA_early_flush_plan.md)

### 3.4 항목 5: 동시에 여러 H2P+DC 추적 및 처리 ⭐ 핵심 변경

**현재 구현 상태**: [`TEA_multi_h2p_status.md`](TEA_implementation_status/TEA_multi_h2p_status.md) 참조
**구현 계획**: [`TEA_multi_h2p_plan.md`](TEA_implementation_plan/TEA_multi_h2p_plan.md) 참조

**논문의 동작 모델**:

논문에서 TEA는 **"순차 fetch, 중첩 실행"** 모델:
> "Subsequently, it can fetch and initiate the precomputation for the next instance of H2P branch faster than the main thread."

- TEA Fetch가 Chain A를 다 fetch → Chain A의 ops가 backend에서 실행 중이어도 Chain B가 H2P 분기를 포함한 Chain이라는 게 확인되면, 즉시 Chain B fetch 시작
- 모든 chains의 ops가 동일한 OoO backend에서 동시 실행
- Shadow RAT는 하나를 공유하며 순차적으로 갱신

**현재 구현 상태**: 단일 H2P만 지원

**구현 전략**: 아래 Section 8 참조

### 3.5 항목 6: Shadow RAT, Dependency Wakeup, 및 Poison bit

**현재 구현 상태**: ✅ 구현 완료 — Shadow RAT producer 추적 + `add_to_wake_up_lists()` 연결. 상세 내용은 [`TEA_reg_dependency_status.md`](TEA_implementation_status/TEA_reg_dependency_status.md) 참조
**구현 계획**: [`TEA_reg_dependency_plan.md`](TEA_implementation_plan/TEA_reg_dependency_plan.md) 참조
**사용자 결정**: Poison bit 미구현. TEA에서 무조건 올바르게 Register dependency 추적.

---

## 4. 작업 A: Backward Walk 트리거 구현 [CRITICAL]

### 4.1 구현 완료 (2026-03-18)

**트리거 조건**: Fill Buffer가 가득 찬 상태에서 oldest op (head)이 evict될 때, 해당 op이 H2P이면 BW Walk 시작.

**핵심 설계 결정**:
- BW Walk 중 (`BW_WALKING`) Fill Buffer에 새 op 추가를 차단 (하드웨어 제약, 논문 일치)
- evict되는 oldest H2P는 스냅샷에서 **제외** (`head + 1`부터 복사) — 이 H2P보다 older한 op이 없어 chain 생성 불가하므로
- Walk 완료 시 `reset_fill_buffer()`로 버퍼 초기화

**알려진 제약**: evict되는 oldest H2P 자체의 dependency chain은 생성되지 않음. 스냅샷 내 younger H2P의 chain만 생성됨. 실용적 영향은 미미 (H2P는 빈번하게 등장).

### 4.2 현재 코드: `src/fill_buffer.c`

```c
void fill_buffer_add(uns proc_id, Op* op) {
    if (bw_engines[proc_id]->state == BW_WALKING) {
        return; // 엔진 작동 중이면 buffer에 추가 금지
    }
    Fill_Buffer* fb = retired_fill_buffers[proc_id];
    if (!fb) return;

    if (fb->count == fb->size) {
        Op* evicted_op = &fb->entries[fb->head];
        if (evicted_op->oracle_info.hbt_pred_is_hard) {
            record_on_off_path(proc_id, evicted_op);

            // BW Walk 트리거: 엔진이 유휴 상태일 때만 새 Walk를 시작
            // evict되는 oldest H2P는 스냅샷에서 제외 (older ops 부재로 chain 생성 불가)
            Backward_Walk_Engine* engine = bw_engines[proc_id];
            if (engine && engine->state == BW_IDLE) {
                int idx = (fb->head + 1) % fb->size;  // evicted H2P 제외
                engine->snapshot_op_count = 0;
                for (int i = 0; i < fb->count - 1; i++) {
                    engine->snapshot_buffer[engine->snapshot_op_count++] = fb->entries[idx];
                    idx = (idx + 1) % fb->size;
                }
                engine->walk_cycles_remaining = BACKWARD_WALK_CYCLES;
                engine->state = BW_WALKING;
            }
        }
        fb->head = (fb->head + 1) % fb->size;
        fb->count--;
    }
    // 새 op 추가
    ...
}
```

### 4.3 필요한 파라미터

`core.param.def`에 추가:
```c
DEF_PARAM(backward_walk_cycles, BACKWARD_WALK_CYCLES, uns, uns, 500, )
```

### 4.4 검증

`TEA_TRIGGERS > 0`, `TEA_TRIGGER_SKIP_NO_CHAIN ≈ 0` 확인.

---

## 5. 작업 B: Iterative Walk (Section III-C)

(기존 내용 유지 — chain_bit 마킹 로직)

---

## 6. 작업 C + HC: `periodically_reset_caches()` 호출 연결 + Hybrid Chain

### 6.1 Hybrid Chain (작업 HC)

**상세 계획**: [`TEA_hybrid_chain_plan.md`](TEA_implementation_plan/TEA_hybrid_chain_plan.md)

현재 dep cache는 매 BW Walk마다 chain을 덮어써서 단일 경로만 유지.
Hybrid Chain은 Block Cache의 bitmask OR 누적을 활용하여 다중 경로의 합집합 chain을 구축:
- dep cache 엔트리에 `block_pcs[]` 추가 → walk마다 합집합 갱신
- 파트 4 신규: Block Cache의 누적 bitmask로 dep cache chain 재구축
- `get_dependency_chain()` 인터페이스 변경 없음 → downstream (TEA Fetch, Rename) 영향 없음
- 시뮬레이터에서 정확성 문제 없음 (oracle_info 사용)

### 6.2 periodically_reset_caches() 호출 연결

`node_stage.c`의 retire 루프에서 500K 명령어마다 `periodically_reset_caches()` 호출.
Hybrid Chain 도입으로 이 함수가 실질적 의미를 가짐:
- Block Cache bitmask 리셋
- dep cache의 chain/block_pcs 초기화 (stale 데이터 제거)

`core.param.def`에 추가:
```c
DEF_PARAM(block_cache_reset_period, BLOCK_CACHE_RESET_PERIOD, uns, uns, 500000, )
```

---

## 7. 작업 D: Poison Bit — 구현 안 함

사용자 결정: TEA 스레드에서 무조건 올바르게 Register dependency를 추적. Poison bit로 인한 Wrong Precomputation 감지는 시뮬레이터에서 불필요 (oracle 값 사용 가능).

---

## 8. 작업 F: 다중 H2P+DC 동시 처리 구현

**상세 계획**: [`TEA_multi_h2p_plan.md`](TEA_implementation_plan/TEA_multi_h2p_plan.md)
**현재 상태**: [`TEA_multi_h2p_status.md`](TEA_implementation_status/TEA_multi_h2p_status.md)

**모델**: 순차 fetch + 중첩 실행 (논문과 동일). 최대 `MAX_TEA_CHAINS`(기본 4)개 H2P가 동시 활성.

**핵심 변경 요약**:
- `Tea_H2P_Chain` 구조체 + `chains[]` 배열로 per-chain 상태 관리
- `Op.h2p_chain_id` 필드로 chain 식별
- `trigger_tea_thread()`: 빈 슬롯 할당 (기존 `state != TEA_IDLE` 게이트 → 슬롯 검색)
- `terminate_tea_chain()`: 개별 chain 종료 (`flush_tea_ops_by_chain_id()`)
- `recover_tea_on_flush(proc_id, recovery_op_num)`: 시그니처 변경, younger chain만 선택적 종료

**전제조건**: 작업 A (BW Walk Trigger), 작업 G (TEA 의존성 Wakeup) 완료 후 진행

**설계 결정 완료**: [`TEA_multi_h2p_plan.md`](TEA_implementation_plan/TEA_multi_h2p_plan.md) Section 13에서
4건의 설계 불일치가 모두 해결됨:
- §13.1: `terminate_tea_chain()` — 방안 A (자기완결형, per-chain fetch/rename/node 정리 내장)
- §13.2: `recover_tea_on_flush()` — 방안 A (terminate_tea_chain() 위임, 중복 리셋 제거)
- §13.4: `tea_create_op_from_cache()` — 3 args (fetch stage 내부에서 chain 추적)
- §13.3: 필드 이름 — `current_fetch_chain`, `current_chain_id`로 통일 완료

---

## 9. 수정 파일 요약

### 작업 A (BW Walk Trigger)

| 파일 | 변경 |
|------|------|
| `src/fill_buffer.c` | BW Walk 트리거 코드 추가 |
| `src/core.param.def` | `BACKWARD_WALK_CYCLES` 파라미터 |

### 작업 G (TEA 의존성 Wakeup) — [`TEA_reg_dependency_plan.md`](TEA_implementation_plan/TEA_reg_dependency_plan.md)

| 파일 | 변경 |
|------|------|
| `src/tea/tea_rename.h` | Shadow RAT에 producer op 추적 필드 추가 (`gp_producer_ops`, `gp_producer_unums` 등) |
| `src/tea/tea_rename.c` | `shadow_rat_snapshot()` producer 복사, `tea_rename_op()` 의존성 설정 + `add_to_wake_up_lists()` 호출 |

### 작업 I (독립 Dispatch) — [`TEA_dispatch_plan.md`](TEA_implementation_plan/TEA_dispatch_plan.md)

| 파일 | 변경 |
|------|------|
| `src/node_stage.c` | `tea_dispatch_to_rs()` 직접 RS dispatch, `tea_dispatch_retry()` 신규, flush step 1 RS 카운터 동기화 |
| `src/node_issue_queue.cc` | `node_issue_queue_dispatch()` Main-only로 단순화 |
| `src/node_issue_queue.h` | `node_dispatch_find_emptiest_rs()` 프로토타입 외부 노출 |

### 작업 F (다중 H2P+DC)

13개 파일 수정, 9개 파일 변경 불필요 — 상세 목록은 [`TEA_multi_h2p_plan.md`](TEA_implementation_plan/TEA_multi_h2p_plan.md) Section 9 참조

---

## 10. 구현 우선순위

```
작업 A (BW Walk Trigger) ✅ 완료 (2026-03-18)
    │
    ▼
작업 G (TEA 의존성 Wakeup) ✅ 완료 (2026-03-18)
    │   Shadow RAT producer 추적 + add_to_wake_up_lists() 연결
    │
    ├──→ 작업 I (독립 Dispatch) ✅ 완료 (2026-04-03)
    │       tea_dispatch_to_rs(): 직접 RS dispatch
    │       tea_dispatch_retry(): RS full 재시도
    │       node_issue_queue_dispatch(): TEA 완전 분리
    │       flush_tea_ops_from_node_stage() step 1: RS 카운터 동기화
    │
    ▼
작업 C + HC (periodically_reset + Hybrid Chain) ← 함께 구현
    │   dep cache chain을 Block Cache OR 누적으로 재구축 (TEA_hybrid_chain_plan.md)
    │
    ▼
시뮬레이션 검증: TEA_TRIGGERS > 0, 의존성 체인 내 ops 순서대로 실행 확인
    │
    ▼
작업 F (다중 H2P+DC) ← 핵심 기능 (Phase F.1~F.6, 상세: TEA_multi_h2p_plan.md)
    │
    ▼
작업 B (Iterative Walk) ← 체인 품질 향상
```

---

## 11. 검증 체크리스트

### 작업 A 완료 후

```
TEA_TRIGGER_ATTEMPTS    > 0   (HBT가 H2P 감지)
TEA_TRIGGERS            > 0   ← 핵심
TEA_TRIGGER_SKIP_NO_CHAIN ≈ 0 (dep cache 채워짐)
TEA_OPS_FETCHED         > 0
TEA_OPS_EXECUTED        > 0
TEA_EARLY_FLUSHES       > 0   ← 전체 파이프라인 동작 확인
```

### 작업 F 완료 후

[`TEA_multi_h2p_plan.md`](TEA_implementation_plan/TEA_multi_h2p_plan.md) Section 11 참조

### 빌드 및 시뮬레이션

```bash
cd ~/scarab-infra
./sci --build-scarab tea_dbg
./sci --sim tea_dbg
/analyze_tea_sim
```

---

## 12. 주요 코드 위치 빠른 참조

| 기능 | 파일 | 위치 |
|------|------|------|
| BW Walk 트리거 | `fill_buffer.c` | `fill_buffer_add()` 내 H2P eviction |
| BW Walk 알고리즘 | `dependency_chain_cache.c` | `add_dependency_chain()` |
| BW Walk 엔진 | `dependency_chain_cache.c` | `cycle_backward_walk_engine()` |
| TEA 트리거 | `bp/bp.c:876` | `bp_predict_op()` 내 |
| TEA Fetch 체인 조회 | `tea/tea_fetch_stage.c:131` | `update_tea_fetch_stage()` |
| TEA Op 생성 | `tea/tea_fetch_stage.c:189` | `tea_create_op_from_cache()` |
| Shadow RAT snapshot | `tea/tea_rename.c:176` | `shadow_rat_snapshot()` |
| TEA Rename | `tea/tea_rename.c:439` | `tea_rename_op()` |
| TEA Dispatch | `node_stage.c:397` | `tea_dispatch_to_rs()` |
| TEA Retire | `node_stage.c:581` | `node_retire_tea_ops()` |
| Early Flush 감지 | `exec_stage.c:554` | `exec_stage_bp_resolve()` |
| TEA 전체 flush | `node_stage.c:1001` | `flush_tea_ops_from_node_stage()` |
| Recovery | `cmp_model.c:389` | `recover_tea_on_flush()` |
| RS 파티셔닝 | `exec_ports.c:223` | `init_exec_ports_rs_list()` |
| 2-pass 스케줄링 | `node_issue_queue.cc:395` | `node_issue_queue_schedule()` |
| periodically_reset | `dependency_chain_cache.c:246` | `periodically_reset_caches()` |

---

## 13. 알려진 버그 및 주의사항

### 13.1 현재 코드의 알려진 ASSERT (2026-03-17)

| ASSERT | 파일:라인 | simpoint | 상태 | 원인 |
|--------|----------|----------|------|------|
| ASSERT 1: `op->state == OS_IN_ROB` | `node_issue_queue.cc:308` | leela 118750, C=42133 | ✅ 해결 (2026-04-03) | 작업 I 독립 dispatch: `node_issue_queue_dispatch()`에서 TEA ops skip, `tea_dispatch_to_rs()`에서 직접 RS dispatch |
| ASSERT 2: `rs_op_count > 0` | `node_issue_queue.cc:262` | leela 128383, C=30322 | ✅ 해결 (2026-04-03) | ASSERT 1과 동일 근본 원인 → 작업 I로 해결 |
| flush_window `rs_op_count > 0` | `node_stage.c:261` | 모든 벤치마크 | ✅ 해결 (2026-04-03) | `flush_tea_ops_from_node_stage()` step 1에서 OS_SCHEDULED/OS_MISS RS 카운터 동기화 구현 완료 ([`TEA_dispatch_plan.md`](TEA_implementation_plan/TEA_dispatch_plan.md) §10) |
| FTQ deadlock (Case 1) | `decoupled_frontend.cc:280` | mcf | ⚠️ 부분 해결 | Case 1 `recover_at_decode` deadlock ([`TEA_early_flush_status.md`](TEA_implementation_status/TEA_early_flush_status.md) §7.1) |
| TEA mem op node 무기한 잔류 / OS_DONE 미설정 | `tea_thread.c`, `node_stage.c` | leela (IPC -65.5%) | ✅ 해결 (2026-04-01) | exec이 mem op의 done_cycle 미설정 → OP_DONE 발동 불가, OS_DONE도 미설정 → node table 무기한 잔류. 수정: `tea_op_completed()`에서 `op->state = OS_DONE` 설정 + `node_retire_tea_ops()`에서 mem op는 OS_DONE 전용 게이트 사용. 상세: [`TEA_op_manage_status.md §8.7`](TEA_implementation_status/TEA_op_manage_status.md) |

**해결 완료**: ASSERT 1&2 및 flush_window ASSERT → 작업 I (독립 dispatch + flush step 1 RS 카운터 동기화) 구현으로 해결 (`TEA_dispatch_plan.md`)

### 13.2 다중 H2P 관련 주의사항

Op* 유효성, tea_op_counter 공유, Shadow RAT 순차 갱신, Recovery 정책, Store Buffer 격리 →
[`TEA_multi_h2p_plan.md`](TEA_implementation_plan/TEA_multi_h2p_plan.md) Section 12 참조.
