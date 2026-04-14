# TEA 구현 계획 (TEA Implementation Plan)

**논문**: Timely, Efficient, and Accurate Branch Precomputation (MICRO 2024, UT Austin)
**논문 원본**: `/home/lee/scarab/docs/TEA_info/TEA_paper_origin.pdf`
**최종 갱신**: 2026-04-12
**베이스라인 코드**: `/home/lee/scarab/src/`

---

## 1. 현재 구현 아키텍처

### 백그라운드: 의존성 체인 사전 구축 ✅

```
Main Thread Retire → fill_buffer_add()
  [Fill Buffer 가득 차고 oldest op이 H2P이면 BW Walk 시작]
  → cycle_backward_walk_engine() : youngest→oldest 역방향 의존성 추적
  → add_dependency_chain() : Dep Chain Cache에 chain 저장 (H2P PC 인덱싱)
```

### TEA 스레드 (단일 H2P 기준, 현재 구현 ✅)

```
bp.c: bp_predict_op() — HBT가 H2P 감지, get_dependency_chain()으로 존재 확인
  → trigger_tea_thread() : state=TEA_FETCHING, Shadow RAT snapshot
  → update_tea_fetch_stage() : Dep Chain Cache에서 TEA_FETCH_WIDTH씩 fetch
        [backpressure: tea_fetch->sd.op_count > 0 이면 stall]
  → update_tea_rename_stage() : Shadow RAT rename, TEA preg pool에서 dst 할당
        [backpressure: rename->sd.op_count > 0 이면 stall (TEA_RENAME_STALL_DISPATCH)]
  → tea_dispatch_to_rs() : 직접 RS dispatch (tea_dispatch_retry()로 RS full 재시도)
  → exec_stage_bp_resolve() : TEA H2P branch 실행 시
        mispred + SRT checkpoint 있음 → Case 2: bp_sched_recovery() → SRT rollback
        mispred + SRT checkpoint 없음 → Case 1: recover_at_exec/decode 설정
        correct prediction → TEA_H2P_CORRECT, terminate_tea_thread()
```

**단일 H2P 한계**: `tea->state != TEA_IDLE`이면 신규 H2P trigger를 TEA_TRIGGER_SKIP_ACTIVE로 거부.
현재 96.2%의 trigger가 거부됨 → Work F (다중 H2P)가 핵심 성능 개선 포인트.

### 주요 설정값 (논문 Table II 대응)

| 구조 | Scarab 설정 | 파라미터 |
|------|------------|---------|
| Fill Buffer | 512 uops | `FILL_BUFFER_SIZE` |
| HBT | 1024-entry, 3-bit, 50K decrement | `bp/hbt.h` |
| Dep Chain Cache | 1024 entries | `DEPENDENCY_CHAIN_CACHE_SIZE` |
| TEA Physical Regs | 192 reserved | `TEA_PREG_POOL_SIZE` |
| TEA RS Reservation | 파라미터 | `TEA_RS_RESERVATION` |
| TEA Fetch Width | 2-wide | `TEA_FETCH_WIDTH` |
| TEA Store Buffer | 16 entries | `TEA_STORE_BUFFER_SIZE` |

---

## 2. 구현 상태

### ✅ 완료

| 컴포넌트 | 파일 | 비고 |
|---------|------|------|
| HBT (1024-entry, 3-bit, 50K decrement) | `bp/hbt.h/c` | |
| HBT → TEA trigger 연결 | `bp/bp.c:876` | |
| BW Walk Trigger (H2P eviction) | `fill_buffer.c:59-70` | oldest H2P chain 미생성 제약 |
| Backward Dataflow Walk + Dep Chain Cache | `dependency_chain_cache.c:92-258` | Block Cache bitmask OR 포함 |
| TEA 상태 기계 (IDLE/FETCHING/EXECUTING) | `tea_thread.h/c` | 단일 H2P |
| TEA Fetch Stage | `tea_fetch_stage.h/c` | 단일 체인 |
| Shadow RAT + Rename + Preg Pool + Wakeup | `tea_rename.h/c` | producer 추적, `add_to_wake_up_lists()` |
| RS 파티셔닝 + 2-pass 스케줄링 (TEA 우선) | `node_stage.h`, `exec_ports.c`, `node_issue_queue.cc` | |
| TEA Node Stage 독립 Dispatch (Work I) | `node_stage.c:401-560` | `tea_dispatch_to_rs()` 직접 RS dispatch, `tea_dispatch_retry()` |
| TEA Store Buffer (forwarding + full 종료) | `tea_store_buffer.h/c` | |
| Early Flush Case 1 (pre-rename) | `exec_stage.c:614-631` | `decode_cycle` 기반 1a/1b 분기 |
| Early Flush Case 2 (post-rename) | `exec_stage.c:594-613` | `bp_sched_recovery()` + SRT rollback |
| Op Pool Backpressure (pipeline stall) | `tea_rename.c:443`, `tea_fetch_stage.c:152` | `op_count > 0` stall 체크 |

### ❌ 미구현 (우선순위 순)

| ID | 항목 | 파일 | 영향 |
|----|------|------|------|
| **F** | **다중 H2P+DC 동시 처리** | `tea_thread.h/c`, `tea_fetch_stage.c`, `exec_stage.c`, `node_stage.c`, `cmp_model.c` | ★ 핵심 — 96.2% trigger skip 해소 |
| **C+HC** | `periodically_reset_caches()` 연결 + Hybrid Chain | `dependency_chain_cache.c/h`, `node_stage.c` | dep cache stale, 단일 경로 덮어쓰기 |
| **B** | Iterative Walk (chain_bit 활용) | `fill_buffer.c`, `dependency_chain_cache.c` | 긴 체인 추적 불가 |
| **D** | Poison bit | — | 미구현 예정 (oracle로 대체) |

---

## 3. 구현 우선순위

```
✅ 작업 A (BW Walk Trigger, 2026-03-18)
✅ 작업 G (TEA 의존성 Wakeup, 2026-03-18)
✅ 작업 I (독립 Dispatch)
✅ Op Pool Backpressure (2026-04-12)
✅ 단일 H2P 시뮬레이션 검증 (blender 2026-04-12)
        ⚠️  96.2% TEA_TRIGGER_SKIP_ACTIVE → Work F 필요
    │
    ▼
★ 작업 F (다중 H2P+DC) ← 현재 다음 단계 (§5 참조)
    │   96.2% skip 해소 → TEA_TRIGGERS 대폭 증가 → early flush 효과 실현
    │
    ▼
작업 C+HC (periodically_reset + Hybrid Chain, §6 참조)
    │
    ▼
작업 B (Iterative Walk, §7 참조)
```

---

## 4. 검증 결과 (2026-04-12, blender simpoint 25328)

```
TEA_TRIGGER_ATTEMPTS      1,194,296
TEA_TRIGGER_SKIP_ACTIVE   1,148,916   (96.2% — 단일 H2P 한계)
TEA_TRIGGERS                 45,379
TEA_EARLY_FLUSHES               373   (0.8% of triggers)
TEA_H2P_CORRECT              36,839   (81% — 원래 BP가 맞은 경우)
TEA_OPS_FETCHED           1,177,124
TEA_OPS_DISPATCHED        1,177,124   (= FETCHED, op pool 누수 없음)
TEA_RENAME_STALL_DISPATCH     5,968   (backpressure 동작 확인)
IPC: TEA_OFF=2.273 → TEA_ON=2.235 (-1.65%)  ← Work F로 개선 필요
```

---

## 5. 코드 위치 빠른 참조

| 기능 | 파일:위치 | 함수 |
|------|----------|------|
| BW Walk 트리거 | `fill_buffer.c:59` | `fill_buffer_add()` 내 H2P eviction |
| BW Walk 알고리즘 | `dependency_chain_cache.c:92` | `add_dependency_chain()` |
| TEA 트리거 | `bp/bp.c:876` | `bp_predict_op()` |
| TEA Fetch | `tea/tea_fetch_stage.c:113` | `update_tea_fetch_stage()` |
| TEA Op 생성 | `tea/tea_fetch_stage.c:189` | `tea_create_op_from_cache()` |
| Shadow RAT snapshot | `tea/tea_rename.c:176` | `shadow_rat_snapshot()` |
| TEA Rename | `tea/tea_rename.c:439` | `tea_rename_op()` |
| TEA Dispatch | `node_stage.c:401` | `tea_dispatch_to_rs()` |
| TEA RS 재시도 | `node_stage.c:498` | `tea_dispatch_retry()` |
| TEA Retire | `node_stage.c:581` | `node_retire_tea_ops()` |
| Early Flush 감지 | `exec_stage.c:569` | `exec_stage_bp_resolve()` |
| TEA 전체 flush | `node_stage.c:1001` | `flush_tea_ops_from_node_stage()` |
| Recovery | `cmp_model.c:389` | `recover_tea_on_flush()` |
| RS 파티셔닝 | `exec_ports.c:223` | `init_exec_ports_rs_list()` |
| 2-pass 스케줄링 | `node_issue_queue.cc:395` | `node_issue_queue_schedule()` |

---

## 6. 작업 F: 다중 H2P+DC 동시 처리 ★ 현재 단계

**상세 계획 (구역별)**:
- 전체 아키텍처 & Phase 구분: [`TEA_multi_h2p_plan.md`](TEA_implementation_plan/TEA_multi_h2p_plan.md)
- Fetch stage chain 전환 (F.3): [`TEA_shadow_ftq_plan.md`](TEA_implementation_plan/TEA_shadow_ftq_plan.md)
- Per-chain Op 관리 / `terminate_tea_chain()` / `flush_tea_ops_by_chain_id()`: [`TEA_op_manage_plan.md`](TEA_implementation_plan/TEA_op_manage_plan.md)
- Early Flush 다중화 (EF-1 시그니처, EF-2 per-chain flush, EF-3 SRT checkpoint): [`TEA_early_flush_plan.md`](TEA_implementation_plan/TEA_early_flush_plan.md)
- 다중 H2P Reg Dependency 관리 (not-rdy bit 정리): [`TEA_reg_dependency_plan.md`](TEA_implementation_plan/TEA_reg_dependency_plan.md) §4

**현재 상태**: [`TEA_multi_h2p_status.md`](TEA_implementation_status/TEA_multi_h2p_status.md), [`TEA_op_manage_status.md`](TEA_implementation_status/TEA_op_manage_status.md), [`TEA_early_flush_status.md`](TEA_implementation_status/TEA_early_flush_status.md), [`TEA_reg_dependency_status.md`](TEA_implementation_status/TEA_reg_dependency_status.md)

### 문제

`trigger_tea_thread()`의 `state != TEA_IDLE` 게이트로 동시에 하나의 H2P만 수용. 96.2%의 H2P가 거부.

### 목표

최대 `MAX_TEA_CHAINS(=4)`개 H2P chain을 동시 활성 (순차 fetch, 중첩 실행 — 논문 모델과 동일).

### 구현 단계

| Phase | 내용 | 주요 파일 |
|-------|------|----------|
| F.1 | 데이터 구조: `Op.h2p_chain_id` (uns8) + `Tea_H2P_Chain` 구조체 + `Tea_Thread.chains[MAX_TEA_CHAINS]` + `num_active_chains` | `op.h`, `tea_thread.h` |
| F.2 | trigger/terminate per-chain: `trigger_tea_thread()` 빈 슬롯 할당, `terminate_tea_chain(proc_id, chain_slot)` per-chain 정리 | `tea_thread.c` |
| F.3 | Fetch stage chain 전환: `current_fetch_chain` 추적, fetch 완료 시 다음 INACTIVE chain 탐색, `recover_tea_fetch_stage_by_chain()` | `tea_fetch_stage.c` |
| F.4 | `exec_stage_bp_resolve()`: `h2p_chain_id`로 chain 매칭, per-chain Early Flush 처리 | `exec_stage.c` |
| F.5 | Selective flush: `flush_tea_ops_by_chain_id()`, `recover_tea_rename_stage_by_chain()`, `tea_store_buffer_clear_by_chain_id()` | `node_stage.c`, `tea_rename.c`, `tea_store_buffer.c` |
| F.6 | `recover_tea_on_flush(proc_id, recovery_op_num)`: `target_h2p_op_num >= recovery_op_num`인 chain만 종료 | `cmp_model.c` |

### 확정된 설계 결정

- **`Tea_H2P_Chain` 구조체**: `state(INACTIVE/FETCHING/EXECUTING)`, `target_h2p_pc`, `target_h2p_op_num`, `main_h2p_op`, `saved_unique_num`, `tea_op_count`, `tea_ops_fetched`
- **`terminate_tea_chain()`**: fetch→rename→node→store_buffer 순차 정리 (자기완결형). 마지막 chain 종료 시 `reset_tea_preg_pool()`, Shadow RAT reset, `state=TEA_IDLE`
- **Shadow RAT**: 모든 chain 공유 (첫 chain trigger 시 1회 snapshot, 이후 누적 갱신)
- **Preg pool**: 전체 공유, per-chain 반환 안 함 (chain 종료 시 dangling 매핑 위험)
- **SRT checkpoint**: oldest H2P만 유지 (방안 A) — `map_rename.c`의 `!checkpoint_is_valid()` guard 추가
- **per-chain `tea_op_count`**: `tea_op_completed()`에서 감소, `update_tea_thread()` per-chain 루프에서 0 감지 시 `terminate_tea_chain()` 호출

상세 설계: [`TEA_multi_h2p_plan.md`](TEA_implementation_plan/TEA_multi_h2p_plan.md)

### 작업 F 완료 후 검증 체크리스트

```
TEA_TRIGGER_SKIP_ACTIVE 대폭 감소 (< 50% 목표)
TEA_TRIGGER_SKIP_FULL > 0 (모든 chain 슬롯 사용 중 → trigger 거부, 신규 stat)
TEA_TRIGGERS 대폭 증가
TEA_EARLY_FLUSHES 증가
TEA_CHAIN_TERMINATED > 0 (개별 chain 정상 종료, 신규 stat)
TEA_CHAINS_CONCURRENT_MAX > 1 (동시 활성 chain 최대 수, 신규 stat)
SRT checkpoint ASSERT 미발생
IPC TEA_ON > TEA_OFF (성능 개선 확인)
```

---

## 7. 작업 C+HC: periodically_reset + Hybrid Chain

**상세 계획**: [`TEA_hybrid_chain_plan.md`](TEA_implementation_plan/TEA_hybrid_chain_plan.md)

현재 Dep Chain Cache는 매 BW Walk마다 chain을 덮어써 단일 경로만 유지. Hybrid Chain은 Block Cache의 bitmask OR 누적으로 다중 경로 합집합 chain을 구축.

**핵심 변경**:
- Dep Chain Cache 엔트리에 `block_pcs[]` 추가 → walk마다 합집합 갱신
- `periodically_reset_caches()` 호출 연결: `node_stage.c` retire 루프에서 `BLOCK_CACHE_RESET_PERIOD` (500K ops)마다 호출
- `get_dependency_chain()` 인터페이스 변경 없음 → downstream (TEA Fetch) 영향 없음

---

## 8. 작업 B: Iterative Walk

`fill_buffer.c`, `dependency_chain_cache.c`에서 chain_bit 마킹 활용. 긴 의존성 체인을 여러 BW Walk 사이클에 걸쳐 점진적 확장. 작업 C+HC 완료 후 구현.

---

## 9. 논문과의 주요 설계 차이점

| 항목 | 논문 | Scarab 구현 | 비고 |
|------|------|------------|------|
| Shadow FTQ | 128-entry FIFO | 미사용 | Dep Chain Cache 직접 조회 (방안 A) |
| Block Cache | TEA Fetch에 직접 사용 | Dep Chain Cache로 사전 통합 후 사용 | C+HC에서 Hybrid Chain으로 개선 예정 |
| Poison bit | 있음 | 미구현 | oracle 값 사용으로 대체 (구현 예정 없음) |
| SRT checkpoint | per-H2P | oldest H2P만 유지 | Work F 구현 시 방안 A 적용 |
| Multi-H2P 동시 실행 | 4 chains | 단일 H2P | Work F에서 구현 예정 |

---

## 10. 알려진 버그 이력

| 버그 | 파일 | 상태 | 해결 |
|------|------|------|------|
| FTQ deadlock (Case 1) | `decoupled_frontend.cc:280` | ✅ 해결 (2026-03-26) | `decode_cycle` 기반 Case 1a/1b 분기, `recover_at_exec` 이중 설정 제거 |
| Op pool 고갈 ASSERT | `op_pool.c` | ✅ 해결 (2026-04-12) | Rename/Fetch stage backpressure stall 추가 — undispatched ops 덮어쓰기 방지 |

---

## 11. 구현 계획·상태 문서 인덱스

각 feature의 상세 계획과 현재 상태는 서브디렉토리의 개별 문서에 있다.
아래 표는 master plan과 개별 문서 간의 매핑이다.

| 주제 | 계획 (`TEA_implementation_plan/`) | 상태 (`TEA_implementation_status/`) | 연결된 Master 섹션 |
|------|----------------------------------|-------------------------------------|-------------------|
| 독립 Dispatch (Work I) — ✅ | [`TEA_dispatch_plan.md`](TEA_implementation_plan/TEA_dispatch_plan.md) | — | §2 완료, §5 |
| Reg Dependency / Wakeup (Work G + Work F §4) | [`TEA_reg_dependency_plan.md`](TEA_implementation_plan/TEA_reg_dependency_plan.md) | [`TEA_reg_dependency_status.md`](TEA_implementation_status/TEA_reg_dependency_status.md) | §2 완료, §6 |
| Early Flush (Case 1/2 완료, EF-1~4 미구현) | [`TEA_early_flush_plan.md`](TEA_implementation_plan/TEA_early_flush_plan.md) | [`TEA_early_flush_status.md`](TEA_implementation_status/TEA_early_flush_status.md) | §2 완료, §6 |
| Op Pool / Backpressure / 다중 H2P Op 관리 | [`TEA_op_manage_plan.md`](TEA_implementation_plan/TEA_op_manage_plan.md) | [`TEA_op_manage_status.md`](TEA_implementation_status/TEA_op_manage_status.md) | §2 완료, §6 |
| 다중 H2P+DC 전체 아키텍처 (Work F) | [`TEA_multi_h2p_plan.md`](TEA_implementation_plan/TEA_multi_h2p_plan.md) | [`TEA_multi_h2p_status.md`](TEA_implementation_status/TEA_multi_h2p_status.md) | §6 |
| Shadow FTQ / Fetch stage chain 전환 (Work F F.3) | [`TEA_shadow_ftq_plan.md`](TEA_implementation_plan/TEA_shadow_ftq_plan.md) | [`TEA_shadow_ftq_status.md`](TEA_implementation_status/TEA_shadow_ftq_status.md) | §6 |
| Hybrid Chain / `periodically_reset_caches()` (Work C+HC) | [`TEA_hybrid_chain_plan.md`](TEA_implementation_plan/TEA_hybrid_chain_plan.md) | — | §7 |

TEA 코드를 수정하기 전에 (1) 관련 주제의 상태 문서로 현재 구현 상황을 확인하고,
(2) 계획 문서로 수정 범위와 설계 결정을 파악한 뒤,
(3) 작업 완료 후 상태 문서와 master plan §2/§3을 갱신한다.
