# TEA 구현 계획 및 현재 상태

**논문**: Timely, Efficient, and Accurate Branch Precomputation (MICRO 2024, UT Austin)
**논문 원본**: `/home/lee/scarab/docs/TEA_info/TEA_paper_origin.pdf`
**최종 갱신**: 2026-04-27
**베이스라인 코드**: `/home/lee/scarab/src/`

---

## 1. 현재 구현 아키텍처

Scarab TEA 구현은 논문의 "branch prediction override"가 아니라 "early misprediction recovery" 모델을 따른다. Main thread가 HBT에서 H2P branch를 감지하면 TEA thread가 이미 구축된 dependency chain을 별도 frontend에서 fetch/rename하고, shared backend에서 H2P branch를 먼저 execute하여 Main H2P의 recovery를 앞당긴다.

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

현재 코드는 Work F 1차 구현 이후 상태다. 단일 H2P active gate는 제거되었고, 최대 4개 chain slot을 갖는 multi-H2P 구조가 들어가 있다. 다만 아직 최신 시뮬레이션 결과로 안정성/성능 검증을 끝낸 상태는 아니다.

---

## 2. 구현 상태 요약

| 영역 | 현재 상태 | 핵심 위치 |
|------|-----------|-----------|
| HBT / TEA trigger | 구현됨 | `bp/hbt.c`, `bp/bp.c`, `tea/tea_thread.c` |
| BW Walk / Dep Chain Cache | 구현됨 | `tea/dependency_chain_cache.c`, `tea/fill_buffer.c` |
| TEA Fetch | 구현됨, multi-H2P sequential fetch | `tea/tea_fetch_stage.c` |
| TEA Rename / Shadow RAT | 구현됨, 모든 chain이 공유 | `tea/tea_rename.c` |
| Direct RS dispatch | 구현됨 | `node_stage.c`, `node_issue_queue.cc` |
| TEA Store Buffer | 구현됨, `h2p_chain_id` 태깅 | `tea/tea_store_buffer.c` |
| Early Flush Case 1/2 | 구현됨, per-chain 처리 | `exec_stage.c`, `cmp_model.c` |
| Work F multi-H2P | 1차 구현됨, 디버깅/검증 필요 | `tea_thread.c`, `tea_fetch_stage.c`, `node_stage.c` |
| Hybrid Chain / periodic reset | 아직 미구현 | `TEA_hybrid_chain_plan.md` |
| Iterative Walk | 아직 미구현 | 향후 Work B |
| Poison bit | 구현하지 않기로 결정 | oracle 기반 시뮬레이션 |

Work F 이전 문서에 있던 `TEA_TRIGGER_SKIP_ACTIVE` 중심 설명은 더 이상 현재 코드의 주 동작이 아니다. 현재 trigger 거부는 dependency chain miss 또는 모든 chain slot 사용(`TEA_TRIGGER_SKIP_FULL`)이 주 경로다.

---

## 3. Work F 현재 설계

### 핵심 데이터 구조

- `Op.h2p_chain_id`: `0`은 main op, `1..MAX_TEA_CHAINS`는 TEA chain slot+1.
- `Tea_H2P_Chain`: per-chain 상태, H2P PC/op_num, Main H2P 포인터, `saved_unique_num`, oracle/recovery info, `tea_op_count`, `tea_ops_fetched`.
- `Tea_Thread.chains[MAX_TEA_CHAINS]`: 최대 4개 chain slot.
- `Tea_Thread.num_active_chains`: active chain 수. `tea_is_active()`는 이 값으로 판단.
- `Tea_Thread.current_fetch_chain`: 현재 fetch 중인 chain slot. Fetch는 sequential, backend execution은 overlap.

### 현재 동작

1. `trigger_tea_thread()`가 빈 chain slot을 찾는다.
2. dependency chain cache hit이면 chain slot을 `CHAIN_FETCHING`으로 채운다.
3. 첫 active chain일 때만 Shadow RAT snapshot과 TEA op counter reset을 수행한다.
4. `update_tea_fetch_stage()`는 `current_fetch_chain`을 순차적으로 fetch하고, 완료된 chain을 `CHAIN_EXECUTING`으로 넘긴다.
5. `tea_create_op_from_cache()`는 생성한 TEA op에 `h2p_chain_id`를 부여하고 per-chain `tea_op_count`를 증가시킨다.
6. `exec_stage_bp_resolve()`는 TEA H2P op의 `h2p_chain_id`로 chain을 찾고 Case 1/2를 처리한다.
7. `terminate_tea_chain()`은 fetch, rename, node/RS/exec/dcache, store buffer를 해당 chain만 선택적으로 정리한다.
8. `recover_tea_on_flush(proc_id, recovery_op_num)`는 recovery point 이상의 H2P chain만 종료한다.

### 설계 결정

- Shadow RAT은 모든 chain이 공유한다. 첫 active chain에서 snapshot하고 이후 chain들은 같은 Shadow RAT을 이어서 사용한다.
- TEA preg pool도 공유한다. Per-chain preg 반환은 dangling mapping 위험 때문에 하지 않는다.
- SRT checkpoint는 oldest active H2P 기준으로 하나만 유지한다. `map_rename.c`에는 checkpoint 중복 생성을 막는 guard가 있다.
- `tea_op_completed()`는 `OS_DONE`만 설정한다. `tea_op_count` 감소는 `node_retire_tea_ops()`가 op을 실제 free하기 직전에 수행한다.
- `terminate_tea_thread()`는 전체 TEA 종료용으로 유지하고, 일반 per-chain 종료는 `terminate_tea_chain()`이 담당한다.

---

## 4. 현재 검증 기준

최신 Work F 코드에 대한 시뮬레이션 결과는 아직 문서화되어 있지 않다. 아래 수치는 Work F 이전 단일 H2P baseline으로만 사용한다.

```
2026-04-12, blender simpoint 25328, single-H2P baseline

TEA_TRIGGER_ATTEMPTS      1,194,296
TEA_TRIGGER_SKIP_ACTIVE   1,148,916   (96.2%)
TEA_TRIGGERS                 45,379
TEA_EARLY_FLUSHES               373
TEA_H2P_CORRECT              36,839
TEA_OPS_FETCHED           1,177,124
TEA_OPS_DISPATCHED        1,177,124
TEA_RENAME_STALL_DISPATCH     5,968
IPC: TEA_OFF=2.273 -> TEA_ON=2.235 (-1.65%)
```

Work F 이후 새로 확인해야 할 지표:

- `TEA_TRIGGER_SKIP_ACTIVE`는 legacy stat으로 남아 있지만 새 trigger path에서는 사실상 사용되지 않아야 한다.
- `TEA_TRIGGER_SKIP_FULL`이 chain slot 부족을 나타내야 한다.
- `TEA_TRIGGERS`, `TEA_EARLY_FLUSHES`, `TEA_CHAIN_TERMINATED`가 Work F 이전보다 증가하는지 확인한다.
- `TEA_CHAINS_CONCURRENT_MAX`는 stat 정의가 있지만 현재 코드에는 high-watermark emission이 아직 완성되지 않았다.
- `TEA_MAX_CHAINS` param은 정의되어 있지만 현재 loops는 compile-time `MAX_TEA_CHAINS`를 직접 사용한다.

---

## 5. 디버깅 우선순위

현재 소스 기준으로 문서에 기록해 둘 우선 점검 항목은 다음이다.

| 우선순위 | 항목 | 이유 |
|----------|------|------|
| P0 | `op_pool_setup_op()`에서 `h2p_chain_id` reset 여부 | TEA op이 main op으로 재사용될 때 stale chain id가 남을 수 있음 |
| P0 | `flush_tea_ops_by_chain_id()`의 `thread_id == 1` guard | 현재 여러 필터가 `h2p_chain_id`만 보므로 stale id와 결합하면 main op 오염 가능 |
| P1 | surviving TEA op의 flushed-main-producer dependency cleanup | `recover_tea_on_flush()`가 younger chain만 종료할 때 older surviving chain이 stale not-ready bit를 가질 수 있음 |
| P1 | `TEA_CHAINS_CONCURRENT_MAX` instrumentation | Work F 효과 확인에 필요 |
| P1 | `TEA_MAX_CHAINS` runtime param 반영 | 현재 compile-time `MAX_TEA_CHAINS=4`와 param 정의가 분리되어 있음 |
| P2 | fetch/rename selective recovery의 stage-data compaction | `sd.op_count--`만으로 sparse buffer가 생기는지 시뮬레이션으로 확인 필요 |

이 항목들은 소스 수정 전 시뮬레이션 결과와 ASSERT/통계 로그로 먼저 우선순위를 재확인한다.

---

## 6. 향후 작업

1. Work F 1차 구현을 대상으로 짧은 simpoint에서 빌드/런타임 안정성을 확인한다.
2. 통계에서 trigger skip 원인, concurrent chain 수, per-chain termination 수, early flush 증감, RS/preg/store-buffer stall을 본다.
3. P0 디버깅 항목을 먼저 수정한 뒤 같은 simpoint로 회귀 비교한다.
4. Work F 안정화 후 Hybrid Chain / periodic reset을 진행한다.
5. Hybrid Chain 이후 필요하면 Iterative Walk를 진행한다.

---

## 7. 문서 인덱스

| 주제 | 계획 문서 | 상태 문서 |
|------|-----------|-----------|
| Multi-H2P Work F | `TEA_implementation_plan/TEA_multi_h2p_plan.md` | `TEA_implementation_status/TEA_multi_h2p_status.md` |
| Op 관리 / selective flush | `TEA_implementation_plan/TEA_op_manage_plan.md` | `TEA_implementation_status/TEA_op_manage_status.md` |
| Early Flush | `TEA_implementation_plan/TEA_early_flush_plan.md` | `TEA_implementation_status/TEA_early_flush_status.md` |
| Register dependency / wakeup | `TEA_implementation_plan/TEA_reg_dependency_plan.md` | `TEA_implementation_status/TEA_reg_dependency_status.md` |
| Shadow FTQ 대체 fetch 모델 | `TEA_implementation_plan/TEA_shadow_ftq_plan.md` | `TEA_implementation_status/TEA_shadow_ftq_status.md` |
| Direct dispatch | `TEA_implementation_plan/TEA_dispatch_plan.md` | - |
| Hybrid Chain / periodic reset | `TEA_implementation_plan/TEA_hybrid_chain_plan.md` | - |

문서를 수정할 때는 실제 코드 상태와 시뮬레이션 결과를 분리해서 기록한다. 구현된 구조, 검증된 결과, 의심 중인 버그를 같은 상태로 섞지 않는다.
