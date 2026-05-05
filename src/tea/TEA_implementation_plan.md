# TEA 구현 계획 및 현재 상태

**논문**: Timely, Efficient, and Accurate Branch Precomputation (MICRO 2024, UT Austin)
**논문 원본**: `/home/lee/scarab/docs/TEA_info/TEA_paper_origin.pdf`
**최종 갱신**: 2026-05-05
**베이스라인 코드**: `/home/lee/scarab/src/`

---

## 1. 현재 구현 아키텍처

TEA 논문과 Scarab 구현 모두 "early misprediction flush" 모델을 따른다. Branch predictor를 override하는 것이 아니라, TEA thread가 H2P branch를 Main thread보다 먼저 execute하여 기존 Scarab flush 메커니즘을 더 일찍 trigger하는 방식으로 misprediction penalty를 줄인다. "Branch prediction override"는 이전 연구들이 사용하는 방식으로, TEA 논문은 이를 명시적으로 사용하지 않는다. Main thread가 HBT에서 H2P branch를 감지하면 TEA thread가 이미 구축된 dependency chain을 별도 frontend에서 fetch/rename하고, shared backend에서 H2P branch를 먼저 execute하여 Main H2P의 recovery를 앞당긴다.

```
Main retire
  -> Fill Buffer
  -> BW Walk
  -> Dependency Chain Cache

Main fetch / BP
  -> HBT says H2P
  -> trigger_tea_thread()
  -> TEA fetch / rename / direct RS dispatch
  -> TEA H2P execute
  -> per-chain terminate or Main recovery scheduling
```

현재 코드는 Work F multi-H2P가 기본 TEA 실행 모델인 상태다. 단일 H2P active gate는 제거되었고, compile-time storage capacity `MAX_TEA_CHAINS=16` 안에서 runtime parameter `TEA_MAX_CHAINS`만큼의 chain slot을 사용할 수 있다. 최근 작업으로 per-chain Shadow RAT/PREG pool, selective flush, stale dependency cleanup, Case 1 timing stat까지 반영되어 있다.

---

## 2. 구현 상태 요약

| 영역 | 현재 상태 | 핵심 위치 |
|------|-----------|-----------|
| HBT / TEA trigger | 구현됨 | `bp/hbt.c`, `bp/bp.c`, `tea/tea_thread.c` |
| BW Walk / Dep Chain Cache | 구현됨, snapshot 내 모든 H2P 처리 | `tea/dependency_chain_cache.c`, `tea/fill_buffer.c` |
| TEA Fetch | 구현됨, multi-H2P sequential fetch | `tea/tea_fetch_stage.c` |
| TEA Rename / Shadow RAT | 구현됨, per-chain snapshot | `tea/tea_rename.c` |
| TEA PREG pool | 구현됨, `TEA_PREG_RESERVATION / TEA_MAX_CHAINS` per-chain sub-pool | `tea/tea_rename.c` |
| Direct RS dispatch | 구현됨 | `node_stage.c`, `node_issue_queue.cc` |
| TEA op retire/free | 구현됨, 추가 최적화 예정 | `node_stage.c` |
| TEA Store Buffer | 구현됨, `h2p_chain_id` 태깅 | `tea/tea_store_buffer.c` |
| Early Flush Case 1/2 | 구현됨, Case 1 pending flush 포함 | `exec_stage.c`, `cmp_model.c` |
| Work F multi-H2P | 구현됨, 병목 분석 단계 | `tea_thread.c`, `tea_fetch_stage.c`, `node_stage.c` |
| Hybrid Chain / Block Cache fetch | 아직 미구현 | `TEA_hybrid_chain_plan.md` |
| Iterative Walk | 아직 미구현 | 향후 Work B |
| Poison bit | 구현하지 않기로 결정 | oracle 기반 시뮬레이션 |

Work F 이전 문서에 있던 `TEA_TRIGGER_SKIP_ACTIVE` 중심 설명은 더 이상 현재 코드의 주 동작이 아니다. 현재 trigger 거부는 dependency chain miss 또는 모든 chain slot 사용(`TEA_TRIGGER_SKIP_FULL`)이 주 경로다.

---

## 3. Work F 현재 설계

### 핵심 데이터 구조

- `Op.h2p_chain_id`: `0`은 main op, `1..MAX_TEA_CHAINS`는 TEA chain slot+1.
- `Tea_H2P_Chain`: per-chain 상태, H2P PC/op_num, Main H2P 포인터, `saved_unique_num`, oracle/recovery info, `tea_op_count`, `tea_ops_fetched`.
- `Tea_Thread.chains[MAX_TEA_CHAINS]`: compile-time capacity 16. 실제 사용 slot 수는 `TEA_MAX_CHAINS`.
- `Tea_Thread.num_active_chains`: active chain 수. `tea_is_active()`는 이 값으로 판단.
- `Tea_Thread.current_fetch_chain`: 현재 fetch 중인 chain slot. Fetch는 sequential, backend execution은 overlap.

### 현재 동작

1. `trigger_tea_thread()`가 빈 chain slot을 찾는다.
2. dependency chain cache hit이면 chain slot을 `CHAIN_FETCHING`으로 채운다.
3. Chain slot마다 trigger 시점의 Main RAT을 독립 Shadow RAT으로 snapshot한다.
4. `update_tea_fetch_stage()`는 `current_fetch_chain`을 순차적으로 fetch하고, 완료된 chain을 `CHAIN_EXECUTING`으로 넘긴다.
5. `tea_create_op_from_cache()`는 생성한 TEA op에 `h2p_chain_id`를 부여하고 per-chain `tea_op_count`를 증가시킨다.
6. `exec_stage_bp_resolve()`는 TEA H2P op의 `h2p_chain_id`로 chain을 찾고 Case 1/2를 처리한다.
7. `terminate_tea_chain()`은 fetch, rename, node/RS/exec/dcache, store buffer를 해당 chain만 선택적으로 정리한다.
8. `recover_tea_on_flush(proc_id, recovery_op_num)`는 recovery point 이상의 H2P chain만 종료한다.

### 설계 결정

- Shadow RAT은 chain slot별로 독립 snapshot을 가진다. Cross-chain TEA-produced register dependency는 만들지 않는다.
- TEA preg pool은 physical table 끝부분에서 `TEA_PREG_RESERVATION`개를 예약하고 `TEA_MAX_CHAINS`로 균등 분할한다.
- Per-chain termination 시 해당 chain PREG pool을 reset하고, TEA op retire 시 previous mapping preg를 해당 chain pool로 반환한다.
- SRT checkpoint는 기존 Scarab recovery machinery를 사용한다. Case 1은 checkpoint가 없을 때 pending flush로 기록했다가 Main H2P rename 직후 recovery를 schedule한다.
- `tea_op_completed()`는 `OS_DONE`만 설정한다. `tea_op_count` 감소는 `node_retire_tea_ops()`가 op을 실제 free하기 직전에 수행한다.
- `terminate_tea_thread()`는 전체 TEA 종료용으로 유지하고, 일반 H2P 결과 처리는 `terminate_tea_chain_with_reason()`이 담당한다.

---

## 4. 현재 검증 기준

현재 검증은 cumulative stat이 아니라 periodic stat을 기준으로 수행한다. TEA 구조의 효과는 benchmark 평균 IPC뿐 아니라, H2P가 얼마나 빨리 execute되었는지, Case 1 pending delay가 얼마나 큰지, backend shared resource pressure가 early flush benefit을 상쇄하는지까지 같이 봐야 한다.

주요 지표:

- Trigger coverage: `TEA_TRIGGER_ATTEMPTS`, `TEA_TRIGGERS`, `TEA_TRIGGER_SKIP_NO_CHAIN`, `TEA_TRIGGER_SKIP_FULL`
- Chain pressure: `TEA_TRIGGER_ACTIVE_CHAINS_*`, `TEA_ACTIVE_CHAIN_SLOTS_TOTAL`, `TEA_CHAINS_CONCURRENT_MAX`
- Chain lifetime: `TEA_CHAIN_LIFETIME_*`, `TEA_CHAIN_FETCH_WAIT_TOTAL`, `TEA_CHAIN_EXEC_WAIT_TOTAL`
- Early flush timing: `TEA_H2P_TRIGGER_TO_EXEC_*`, `TEA_EARLY_FLUSH_*_TO_DETECT_*`, `TEA_H2P_EXEC_SAVED_CYCLES_AVG`
- Case 1 delay: `TEA_EARLY_FLUSH_CASE1_TO_SCHEDULE_*`, `TEA_EARLY_FLUSH_CASE1_TO_RECOVERY_*`
- Main H2P timing: `TEA_MAIN_H2P_FETCH_TO_EXEC_*`, `TEA_MAIN_H2P_MISPRED_FETCH_TO_EXEC_*`
- Backend pressure: `TEA_OP_NODE_CYCLES_*`, `TEA_RENAME_STALL_PREG`, `TEA_RS_STALLS`, `TEA_READY_LIST_DONE_CLEARED`

`TEA_TRIGGER_SKIP_ACTIVE`는 legacy single-H2P stat이다. 현재 multi-H2P trigger path에서는 증가하지 않는 것이 기대값이다.

---

## 5. 다음 작업 계획

### 5.1 Case 1 recovery penalty 조정 (완료)

**상태**: 완료.

Case 1 TEA early flush가 Main H2P rename 시점에 recovery를 schedule할 때 `EXTRA_LATE_RECOVERY_CYCLES=15` 대신 `EXTRA_EARLY_RECOVERY_CYCLES=5`를 적용하도록 수정했다.

이 변경은 Case 1이 TEA에 의해 이미 misprediction detect된 early recovery 상황이라는 점을 반영한다. 이후 실험에서는 periodic IPC와 완료 simpoint 기준으로 회귀 여부를 확인했다.

### 5.2 ASSERT FAILED simpoint 원인 분석 (완료)

**상태**: 완료.

Case 1 recovery penalty 조정 후 발생한 ASSERT는 Main H2P recovery flag/checkpoint lifetime 관리 문제로 정리되었고, recovery scheduling/cleanup 경로 수정으로 해결했다.

기존 failing 11개 대상 simpoint가 모두 `Core 0 Finished`로 완료되었고, ASSERT/recovery 관련 실패 패턴은 발견되지 않았다. 원인 파악용 임시 `sim.log` debug 출력도 제거 완료했다.

### 5.3 `TEA_TRIGGER_SKIP_FULL` 병목 분석

`TEA_TRIGGER_SKIP_FULL`은 다음 두 가능성을 분리해서 봐야 한다.

1. 제한된 chain slot 때문에 생기는 정상 구조적 backpressure.
2. Chain이 제때 retire/free되지 못하거나 pending/RS/PREG state가 오래 묶이는 구현 병목.

분석 기준:

- active/fetching/executing chain slot 분포
- `TEA_CHAIN_TERM_*` reason
- chain lifetime, fetch wait, exec wait
- `TEA_OP_NODE_CYCLES_*`
- `TEA_RENAME_STALL_PREG`, `TEA_RS_STALLS`
- Case 1 pending delay와 too-late 비율

### 5.4 TEA op 단위 retire/free 및 PREG 조기 회수 강화

현재 TEA op 자체는 `node_retire_tea_ops()`에서 op 단위로 free되지만, chain context와 일부 자원 lifecycle은 chain 단위다. 다음 최적화는 완료된 TEA op이 backend shared structure에 머무는 시간을 더 줄이고, per-chain PREG가 조기에 재사용되도록 만드는 것이다.

목표:

- 완료된 TEA op이 Node/RS/ready-list에 남는 window 최소화.
- previous mapping preg 반환 경로가 모든 retire/free path에서 정확히 한 번 수행되는지 재검증.
- Chain 단위 reset에만 의존하지 않고 긴 chain에서도 PREG pressure가 줄어드는지 확인.
- Main/TEA shared backend pressure가 IPC benefit을 상쇄하는지 완화.

검증 기준:

- `TEA_OPS_RETIRED`, `TEA_OPS_FLUSHED`
- `TEA_OP_NODE_CYCLES_AVG`
- `TEA_READY_LIST_DONE_CLEARED`, `TEA_RETIRE_READY_LIST_ESCAPE`
- `TEA_RENAME_STALL_PREG`
- `TEA_TRIGGER_SKIP_FULL`

---

## 6. 이후 후보 작업

1. `TEA_TRIGGER_SKIP_FULL` 분석 결과에 따라 TEA op/PREG lifecycle 최적화 범위를 확정한다.
2. Backend pressure가 핵심이면 TEA op retire/free 및 PREG reclaim을 먼저 진행한다.
3. Frontend coverage가 핵심이면 Hybrid Chain / Block Cache 기반 fetch를 재검토한다.
4. Hybrid Chain 이후 필요하면 Iterative Walk를 진행한다.

---

## 7. 문서 인덱스

| 주제 | 계획 문서 | 상태 문서 |
|------|-----------|-----------|
| Multi-H2P Work F | `TEA_implementation_plan/TEA_multi_h2p_plan.md` | `TEA_implementation_status/TEA_multi_h2p_status.md` |
| Op 관리 / selective flush | `TEA_implementation_plan/TEA_op_manage_plan.md` | `TEA_implementation_status/TEA_op_manage_status.md` |
| Early Flush | `TEA_implementation_plan/TEA_early_flush_plan.md` | `TEA_implementation_status/TEA_early_flush_status.md` |
| Register dependency / wakeup | `TEA_implementation_plan/TEA_reg_dependency_plan.md` | `TEA_implementation_status/TEA_reg_dependency_status.md` |
| Shadow FTQ 대체 fetch 모델 | `TEA_implementation_plan/TEA_shadow_ftq_plan.md` | `TEA_implementation_status/TEA_shadow_ftq_status.md` |
| Direct dispatch | `TEA_implementation_plan/TEA_dispatch_plan.md` | `TEA_implementation_status/TEA_op_manage_status.md` |
| Hybrid Chain / Block Cache | `TEA_implementation_plan/TEA_hybrid_chain_plan.md` | `TEA_implementation_status/TEA_shadow_ftq_status.md` |

문서를 수정할 때는 실제 코드 상태와 시뮬레이션 결과를 분리해서 기록한다. 구현된 구조, 검증된 결과, 의심 중인 버그를 같은 상태로 섞지 않는다.
