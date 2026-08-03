# ZERECO: H2P-Chain Load 가속을 통한 Branch 오예측 조기 해소

> 연구 README. Last updated: 2026-08-02.
> Simulator: Scarab / Scarab-infra. TEA(MICRO 2024)는 구현 완료된 비교 baseline으로 둔다.
> 최신 정량 기준은 prefetcher-ON run `zereco_260729_misp_penalty_breakdown`이다.

---

## 1. 연구 목표와 핵심 주장

### 문제 정의

Branch misprediction penalty를 줄이려면 mispredicted branch를 가능한 빨리 resolve해야 한다.
특히 Conventional TAGE 예측기 자체로 잡기 어려운 "H2P(Hard-to-Predict) branch"가 오래 unresolved 상태로 남으면 frontend와 backend의 낭비가 커진다. 왜냐면, Misprediction으로 인해 Wrong path (또는 Off path) 명령어들이 파이프라인으로 Fetch되기 때문이다.

이 연구의 핵심 질문은 다음이다.

> 왜 mispredicted H2P branch가 resolve되기까지 오래 걸리는가?

관찰한 원인은 branch outcome이 dependence chain 안의 load 값에 의존하고, 그 load의 service latency가 branch resolution을 지연시킨다는 점이다.
따라서, Misprediction을 빨리 detect하기 위해 H2P branch를 별도 thread로 precompute하지 않아도, **H2P dependence chain 안의 load latency만 줄이면** main thread의 branch execute 시점이 자연스럽게 앞당겨진다.

### 한 줄 주장

> H2P-chain membership을 criticality filter로 사용하고, 예측 가능한 chain load에는 RF prefetch를, 예측 불가능한 잔여 chain에는 backend priority를 적용하면 TEA보다 낮은 하드웨어 복잡도로 branch 오예측 해소를 앞당길 수 있다.

---

## 2. Motivation

### A. Baseline에서의 Motivation

#### 왜 predictor accuracy가 아니라 misprediction penalty인가?

현대 branch predictor는 긴 global history, 다수의 prediction component와 큰 storage를 사용하며 지속적으로 정확도를 높여 왔다. 그러나 남아 있는 오예측 중 상당수는 branch history보다 실행 중 계산되거나 memory에서 읽힌 값에 의해 outcome이 결정되는 H2P branch에서 발생한다. 이러한 branch는 predictor capacity나 history length를 늘리는 것만으로는 안정적으로 예측하기 어렵다.

Branch Runahead와 TEA에서 지적한 것과 같이, 매우 큰 history-based predictor도 data-dependent H2P branch에 대해서는 제한적인 개선만 제공한다. 남은 오예측을 더 복잡한 predictor로 흡수하려는 접근은 hardware cost 대비 이득이 점차 작아진다. 따라서 ZERECO는 branch predictor를 다시 설계하는 대신, **예측에 실패하더라도 그 사실을 더 빨리 알아내어 misprediction penalty를 줄이는 방향**을 택한다.

#### Fetch-to-resolution penalty

Branch가 mispredict되면 processor는 해당 branch가 execute되어 실제 direction과 target이 확인될 때까지 wrong-path instruction을 계속 fetch하고 실행한다. 이때 branch fetch부터 resolution까지의 구간은 다음 단계로 구성된다.

- Branch가 frontend를 통과해 issue queue에 도달하는 시간
- Branch dependence chain의 source operand가 준비되기를 기다리는 시간
- Ready instruction이 scheduler에서 선택되기를 기다리는 시간
- Branch가 실행되어 misprediction이 확인되는 시간

Fetch-to-resolution이 길어지는 가장 직접적인 이유는 branch가 dependence chain의 마지막 consumer이기 때문이다. Chain 안의 load가 cache hierarchy나 memory system에서 값을 늦게 받으면 그 load의 destination register가 ready 상태가 되지 않는다. 그러면 그 값을 사용하는 arithmetic/comparison instruction이 연쇄적으로 실행되지 못하고, 최종 branch도 operand-ready 상태가 되지 않아 issue와 resolution이 뒤로 밀린다. Load miss처럼 긴 access뿐 아니라 cache hit의 load-to-use latency와 queue/port contention도 serial dependency chain을 따라 branch resolution 지연으로 전파될 수 있다.

실험에서도 fetch-to-resolution의 대부분이 branch source operand를 기다리는 dependency 구간에서 발생했다. 더 중요한 것은 Target Load latency를 줄였을 때 dependency wait와 branch resolution이 함께 짧아지고 IPC가 증가했다는 점이다. 모든 Target Load를 가속한 full oracle뿐 아니라, online address predictor가 올바르게 예측한 Target Load만 가속한 경우에도 같은 방향의 변화가 나타났다. 이는 load latency와 긴 resolution 사이에 단순한 상관관계만 있는 것이 아니라, **H2P-chain Target Load가 fetch-to-resolution을 늘리는 실제 bottleneck 중 하나**임을 보여준다.

Resolution 이후에도 pipeline recovery와 correct-path fetch를 위한 고정적인 시간이 필요하지만, ZERECO가 직접 줄일 수 있는 구간은 resolution 이전이다. 따라서 핵심 질문은 **H2P branch의 dependence chain에서 무엇이 resolution을 늦추며, 그 latency를 main thread 안에서 어떻게 줄일 것인가**이다.

### B. Prior Work의 Limitation

#### TEA

TEA는 H2P branch의 dependence chain을 별도의 precomputation thread로 실행하고, main-thread branch보다 먼저 계산된 결과로 early flush를 발생시킨다. 높은 coverage와 accuracy를 얻을 수 있지만 이를 위해 Block Cache와 Fill Buffer뿐 아니라 전용 fetch/rename path, shadow Fetch Queue와 RAT, store-data 구조, 별도 physical register 관리가 필요하다. Backend에서도 precomputation thread를 위해 reservation station과 physical register를 main thread와 share하고, issue 우선권을 부여한다.
즉, TEA는 branch resolution을 앞당기기 위해 dependence chain 전체를 복제 실행한다. 이 방식은 효과적이지만 hardware 구조와 dynamic instruction 실행량이 증가하고, shared execution/cache resources에서 main thread와 경쟁할 수 있다. ZERECO는 TEA의 H2P-chain identification은 활용하되, 별도 thread와 전체 chain 재실행 없이 **main-thread chain의 실제 bottleneck만 가속**하는 것을 목표로 한다.

- Reference: `/home/lee/scarab/reference/[2024, MICRO] Timely_Efficient_and_Accurate_Branch_Precomputation.pdf`

#### PUBS

PUBS는 branch prediction confidence가 낮은 branch의 backward slice를 식별하고, 해당 instruction을 IQ의 priority entry에 배치해 issue wait를 줄인다. Helper thread 없이 main-thread scheduling만 변경한다는 점에서 구조가 가볍지만, 직접 줄이는 것은 slice instruction들이 issue queue에서 기다리는 시간이다. Load를 더 일찍 issue할 수는 있어도, issue 이후의 cache/memory service latency 자체를 제거하지는 못한다.
또한 PUBS의 slice identification은 branch confidence와 logical-register producer 관계에 기반한다. 따라서 많은 branch를 broadly unconfident로 분류하면 priority entry 부족으로 dispatch가 지연되거나 normal IQ capacity가 줄어들 수 있으며, store-to-load와 같은 memory producer 관계도 명시적으로 복원하지 않는다. ZERECO는 TEA-derived H2P-chain 정보를 사용해 target을 좁히고, latency를 유발하는 load에는 scheduling priority보다 직접적인 load 가속을 우선 적용한다.

- Reference: `/home/lee/scarab/reference/[2018, MICRO] PUBS.pdf`

#### Branch Runahead

Branch Runahead는 runtime에 H2P branch dependence chain을 추출하고, Dependence Chain Engine(DCE)에서 반복 실행해 branch predictor보다 먼저 outcome을 공급한다. History-based predictor가 맞히기 어려운 data-dependent branch를 실제 computation으로 처리하여 Prediction Accuracy를 높이는 목적이 있다.
그러나 fetch prediction을 override하려면 dependence-chain 결과가 branch fetch보다 먼저 준비되어야 하므로 timeliness 조건이 매우 강하다. Synchronization이 늦거나 chain에 long-latency operation이 포함되면 결과가 늦어 사용되지 못한다. 이를 위해 chain extraction, H2P tracking, live-in synchronization, prediction queue와 별도 DCE가 필요하며, 빠른 실행을 위해 chain length와 포함 가능한 operation도 제한한다. ZERECO는 branch outcome 전체를 미리 계산하는 대신, main-thread fetch 이후에도 가치가 있는 **early resolution**을 목표로 하고 chain의 critical load만 선택적으로 가속한다.

- Reference: `/home/lee/scarab/reference/[2021, MICRO] Branch Runahead_Pruett,Y.Patt.pdf`

---

## 3. Key Insights

### 1. Fetch-to-resolution이 핵심 penalty 구간이다
H2P branch가 fetch된 뒤 resolution되기까지의 시간이 recovery 이후의 redirect 시간보다 훨씬 길다. 따라서 predictor가 틀린 뒤의 고정 recovery latency만 최적화하는 것보다, branch execute 자체를 앞당기는 것이 더 큰 기회다.

### 2. Resolution을 늦추는 중심 원인은 dependency wait이다
Fetch-to-resolution을 분해하면 frontend 통과나 branch execution보다 source operand가 준비되기를 기다리는 구간이 지배적이다. 즉, H2P branch는 scheduler에서 단순히 선택되지 못해서라기보다 dependence chain의 결과가 늦게 도착해 오래 unresolved 상태로 남는다.

### 3. Target Load latency가 dependency wait의 주요 원인이다
H2P-chain 안의 모든 operation을 동일하게 다루는 대신 Target Load만 가속해도 dependency wait와 branch resolution이 함께 감소한다. 이는 Target Load latency가 단순한 상관관계가 아니라 resolution latency를 만드는 causal bottleneck임을 보여준다.

### 4. Target Load는 제한적으로 예측 가능하며, 예측 가능성에 따라 메커니즘을 나눌 수 있다
Target Load access는 일부 load PC에 집중되고, 상당수는 PC-local address history로 예측할 수 있다. 예측 가능한 load에는 RF prefetch를 적용하고, 주소 예측이 어려운 chain tail에는 PUBS-style backend priority를 적용하는 hybrid 구성이 적절하다.

### 5. 전체 chain precomputation은 필수가 아니다
TEA와 Branch Runahead는 branch outcome을 얻기 위해 dependence chain을 별도로 재실행하지만, 실험 결과는 main thread의 resolution-critical load만 가속해도 의미 있는 효과가 있음을 보여준다. 따라서 H2P-chain identification은 유지하면서 helper thread와 duplicated execution pipeline을 제거할 수 있는 설계 공간이 존재한다.

---

## 4. 아키텍처 설계

### 전체 구조

ZERECO는 H2P-chain load를 세 단계로 처리한다.

1. **Chain identification**
   - TEA의 Block Cache / Fill Buffer / backward dataflow walk를 활용한다.
   - 별도 precomputation thread는 실행하지 않는다.
   - Fetch 시 Block Cache mask로 main-thread instruction에 chain bit를 태깅하고, 그중 load op를 Target Load로 판정한다.

2. **Predictable load acceleration**
   - Exact virtual address를 높은 신뢰도로 예측할 수 있으면 **RFP-style RF prefetch**를 수행한다.
   - 주소가 틀린 경우 wrong value를 commit하지 않고 기존 scheduler replay 또는 demand load path로 복구하는 no-flush 방향을 전제로 한다.

3. **Unpredictable chain fallback**
   - RF가 처리하지 못하는 Target Load가 하나라도 있는 H2P branch slice 전체에는 **PUBS-style IQ priority**를 적용해 issue 대기 시간을 줄인다.
   - 이 축은 RF prefetch를 대체하는 주력 메커니즘이 아니라, predictor tail을 줄이는 보조 메커니즘이다.

### TEA 구조의 슬림화 방향

TEA의 Block Cache를 identification 전용으로 축소하면, 사실상 **Chain Mask Cache**가 된다. 이 구조는 precomputation thread를 위한 decoded uop store가 아니라 "어떤 slot이 H2P chain member인지"를 알려주는 mask cache이다.

| 필드 | 크기 예시 | 용도 |
|------|-----------|------|
| `tag` | 약 40-bit | basic-block start PC 식별 |
| `chain mask` | 32-bit | block 내 H2P-chain member slot 표시. Dispatch priority 및 Fill Buffer walk 시작점으로 사용 |
| `load mask` | 32-bit | chain slot 중 load 표시. Fetch 시 RF predictor 조회 대상으로 사용 |

위 표는 최종 Chain Mask Cache의 설계 방향이다. 현재 simulator는 64-bit `dependency_mask`, `iq_priority_candidate_mask`, `iq_priority_mask`를 사용하며, 별도 `load mask` 대신 tagged op의 `mem_type == MEM_LD`를 확인해 Target Load를 구분한다.

제거 또는 축소 대상:

- decoded uop bytes data store
- shadow Fetch/Rename/RAT/Fetch Queue
- TEA-thread backend partition
- store data cache
- timestamp flush 기구
- 별도 thread 종료 방지용 empty-block tag store

이 방향의 장점은 oracle target 정의가 Block-Cache-hit chain load였기 때문에, 최종 coverage와 실험 target 정의가 일치한다는 점이다.

### Predictor 후보

현재 실험과 설계 논의에서 중심이 되는 predictor는 per-PC delta 계열이다.

| Predictor | 특성 | 주 용도 |
|-----------|------|---------|
| `stride` / `pc_stride_vaddr` | 같은 delta가 confidence threshold 이상 반복될 때만 `last + stride`를 예측. Coverage는 낮지만 accuracy가 높음. | Exact-vaddr RF prefetch의 conservative path |
| `top-delta` / `pc_top_delta` | per-PC delta histogram에서 최빈 delta를 예측. Make-rate와 coverage가 높지만 accuracy는 낮음. | No-flush recovery를 전제로 한 aggressive RF path |
| temporal / Markov 후보 | stride/delta가 놓치는 반복 주소 또는 pointer-chasing tail을 추적. | `xz`, `omnetpp`, `pr` 등 low-predictability gap 보완 |

H2P-chain filtering 덕분에 target load PC 수가 작다. 이전 분석에서는 상위 수백 개 load PC가 target access의 큰 비중을 덮어, 작은 table 구조 주장을 뒷받침했다.

---

## 5. Scarab 구현 상태

현재 구현은 helper thread 없이 main thread의 H2P-chain Target Load와 backward slice만 가속하는 single-pass 모델이다.

| 기능 | 주요 코드 | 현재 동작 |
|------|----------|----------|
| Target Load 특성화 | `dcache_stage.c`, `h2p_chain_load_predictor_replay.py` | Access pattern 수집과 offline predictor replay |
| Predictor-gated RF | `dcache_stage.c` | Per-PC stride/top-delta가 exact vaddr를 맞히고 store conflict가 없을 때 1-cycle RF service |
| Penalty profiler | `zereco/h2p_mispred_latency.c` | H2P misprediction/misfetch의 fetch-to-resolution과 frontend/dependency/scheduler/execution 분해 |
| Chain mask와 RF filtering | `dependency_chain_cache.c`, `decoupled_frontend.cc` | Full branch slice 또는 Target-Load prefix mask를 저장하고, RF-covered streak를 다음 occurrence의 priority 결정에 반영 |
| IQ Priority와 interference | `node_issue_queue.cc` | Priority-first/oldest-first scheduling, shadow baseline, counterfactual normal-op displacement 계측 |

`dependency`는 branch가 Node Table에 들어간 뒤 마지막 source operand가 ready될 때까지이며, producer execution과 load latency를 포함한다. `scheduler`는 operand-ready 이후 실제 FU 선택까지다.

Predictor는 각 dynamic load에서 한 번만 predict-then-update하며, 잘못된 예측과 store-forwarding 가능 load는 정상 memory path를 사용한다. Online RF 결과는 같은 occurrence의 older producer에 소급하지 않고 다음 H2P slice occurrence부터 priority mask에 반영한다.

현재 RF 1-cycle service와 unlimited select-only Priority는 headroom 모델이다. 실제 RF bandwidth/timeliness와 PUBS식 reserved priority entry 비용은 아직 반영하지 않는다.

---

## 6. 실험 결과 요약

이 절에는 정량 결과를 다시 나열하지 않고, 각 실험의 결과 디렉터리와 해당 실험에서 새롭게 확인한 내용만 정리한다.
세부 수치와 그래프는 각 디렉터리의 분석 산출물을 기준으로 한다.

### 6.1 H2P-chain target load 특성화 (`260624`)

- 결과 디렉터리: `/home/lee/simulations/zereco/260624_h2p_chain_load_access_pattern_all_simpoints`

이 실험을 통해 H2P-chain Target Load access가 비교적 적은 수의 load PC에 집중되며, 상당수의 address stream이 per-PC history로 예측 가능함을 확인했다. 동시에 predictor가 처리하기 어려운 irregular tail의 크기와 특성은 workload마다 다르므로, 하나의 predictor만으로 모든 Target Load를 cover하기는 어렵다.

### 6.2 Baseline, TEA, perfect-load oracle 비교 (`260625`)

- 결과 디렉터리: `/home/lee/simulations/zereco/260625_perf_comparison`

이 실험을 통해 main-thread H2P-chain Target Load latency를 줄이는 것만으로도 큰 성능 headroom이 존재하며, 전체 dependence chain을 helper thread에서 재실행하지 않아도 TEA와 경쟁할 수 있는 설계 가능성이 있음을 확인했다. 이 결과는 prefetcher-OFF 환경의 full-oracle motivation이며, realizable predictor 효과는 다음 실험에서 별도로 검증한다.

### 6.3 Misprediction penalty breakdown과 predictor-gated RF 가속 (`260729`)

- 결과 디렉터리: `/home/lee/simulations/zereco/zereco_260729_misp_penalty_breakdown`

이 실험을 통해 fetch-to-resolution이 H2P misprediction penalty의 핵심 구간이며, 그 안에서는 dependence chain의 operand-ready wait가 가장 큰 원인임을 확인했다. 또한 online predictor가 실제로 cover하는 Target Load만 가속해도 dependency wait와 branch resolution이 줄고 IPC가 개선되므로, Target Load latency가 resolution을 늦추는 causal bottleneck임을 확인했다.

세 실험을 함께 보면 논리는 다음과 같이 연결된다. `260624`는 Target Load의 주소 예측 가능성을, `260625`는 load-latency 제거의 full-oracle headroom을, `260729`는 fetch-to-resolution의 중요성과 predictor가 cover하는 Target Load만 가속해도 resolution 및 IPC가 개선된다는 causal evidence를 제공한다.

---

## 7. 최신 실험 설정과 집계 원칙

### Target Load 정의

Oracle과 predictor 실험에서 Target Load는 Block Cache 또는 Dependency Chain Cache를 통해 H2P dependence chain member로 식별된 main-thread on-path load이다. 앞선 store에서 forwarding되어야 하는 load는 address/value prefetch의 대상이 아니며 in-flight store conflict를 만들 수 있으므로 RF bypass에서 제외한다.

### Config semantics

| Config | 의미 |
|--------|------|
| `baseline` | 정상 cache/memory latency. Full-slice eligibility는 shadow로 계산하지만 oldest-first scheduling 유지 |
| `iq_all_h2p` | RF OFF, 모든 H2P backward slice에 select-only IQ priority |
| `iq_target_load_prefix` | RF OFF, oldest H2P-slice op부터 마지막 Target Load까지의 dependent-op prefix만 priority |
| `rf_stride_only` | Stride RF, full-slice eligibility는 shadow로 계산하지만 oldest-first scheduling 유지 |
| `rf_top_delta_only` | Top-delta RF, full-slice eligibility는 shadow로 계산하지만 oldest-first scheduling 유지 |
| `hybrid_stride_filtered_iq` | Stride RF + online RF-uncovered H2P slice IQ priority |
| `hybrid_top_delta_filtered_iq` | Top-delta RF + online RF-uncovered H2P slice IQ priority |
| `hybrid_stride_filtered_iq_load_prefix` | Stride RF + RF-uncovered slice의 Target-Load prefix priority |
| `hybrid_top_delta_filtered_iq_load_prefix` | Top-delta RF + RF-uncovered slice의 Target-Load prefix priority |

Prediction을 만들지 못했거나 주소가 틀린 load는 정상 demand path를 사용한다. Predictor가 correct한 경우라도 store-forwarding conflict가 있으면 bypass하지 않는다. Full perfect-load oracle과 L1-line predictor config는 최신 실험에서 의도적으로 제외했다.

### Aggregation과 비교 집합

- Baseline penalty breakdown은 완료된 baseline run을 사용한다.
- Predictor 비교는 세 config에 공통으로 완료된 SimPoint 집합을 사용한다.
- Workload 내부 latency는 SimPoint weight를 정규화한 뒤 `weighted total cycles / weighted event count`로 계산한다.
- 전체 latency 대표값은 workload-equal mean이다. Event-weighted 값은 별도 CSV에 보존한다.
- IPC speedup의 AVG는 workload별 IPC ratio의 geometric mean이다.
- Coverage는 `correct predictions / candidate Target Loads`, accuracy는 `correct / predictions made`이다.

완료 run, 제외된 SimPoint, event-population sensitivity와 profiler integrity는 결과 디렉터리의 `analysis` 자료를 기준으로 한다.

---

## 8. 남은 설계 질문과 다음 단계

### Predictor / prefetch 구조
- Stride predictor의 coverage가 낮고 top-delta predictor도 coverage/accuracy를 충분히 회복하지 못한 workload(`omnetpp`, `leela`, `gcc`, `mcf`, `deepsjeng` 등)에 대해 temporal/Markov predictor가 coverage-accuracy trade-off를 개선하는지 평가한다.
- RFP의 Prefetch Table(confidence/stride)을 H2P-chain 특화로 축소할지, 별도 confidence 구조를 만들지 정해야 한다.
- Top-delta처럼 accuracy가 낮은 predictor를 현실 하드웨어에서 사용할 때 scheduler replay 비용을 정량화해야 한다.
- prefetch 처리할 지 IQ에서 priority 부여하는 방식으로 처리할지를 정하는 address predictability 기준을 정해야 한다.

### Timeliness
- 현재 online oracle은 "예측이 맞으면 latency를 줄인다"는 회수 가능 상한에 가깝다.
- 실제 RF prefetch는 fetch/rename 시점에서 demand load보다 충분히 빨라야 한다.
- DRAM miss급 또는 long-chain load에는 N-instance-ahead prefetch, trigger distance, in-flight predictor update 정책이 필요할 수 있다.

### 실험 모델 보강
- Predictor table storage, lookup latency, port contention과 RF write bandwidth를 모델링한다.
- Wrong prediction이 정상 demand path와 scheduler replay에 주는 비용을 측정한다.
- Event-population 변화가 큰 workload의 원인을 확인하고, 동일 branch-instance 또는 안정 population 기준 sensitivity를 보강한다.

### PUBS-style priority fallback
- PUBS의 6 priority-entry 최적점은 4-wide, 64-entry IQ, 71% unconfident branch 마킹 기준이다.
- 논문의 baseline과 실제 우리의 Baseline의 Structure size가 다르기에, 고려하여 적용해야 함.
- ZERECO는 H2P-chain으로 훨씬 선별적으로 마킹하지만 chain span은 길 수 있다.
- 첫 단계로 RS capacity를 바꾸지 않는 select-only upper bound와 online RF filtering을 구현했다.
- Shadow oldest-first와 active Priority의 동일 eligibility cohort를 비교하고, normal displacement cycle, 누적 delay, ready-to-issue wait 및 IPC를 먼저 확인한다.
- Full branch slice와 Target-Load prefix scope를 비교해 priority population과 normal-op 경합을 줄이면서 resolution 효과를 유지할 수 있는지 평가한다.
- Normal-op bottleneck이 큰 경우에만 다음 단계에서 전체 RS의 priority-entry 비율과 stall/non-stall dispatch policy를 sweep한다.

### Cost story
- 논문 방어에서 가장 중요한 질문은 "TEA에서 제일 비싼 구조를 그대로 둔 것 아닌가?"이다.
- 따라서 Chain Mask Cache 슬림화, decoded-uop store 제거, shadow frontend/backend 제거를 정량 cost table로 올려야 한다.
- TEA의 Block Cache 19KB, Fill Buffer 8KB, RS/PR reservation, dynamic instruction 증가 같은 기존 cost claim과 ZERECO cost를 직접 비교해야 한다.

### Related-work 방어
- CRISP 대비: software profiling/post-link prefix가 아니라 pure hardware이고, issue priority만이 아니라 RF prefetch를 결합한다는 점을 강조한다.
- RFP 대비: 전체 load가 아니라 H2P-chain critical load만 타깃으로 한다.
- DLVP/PAP 대비: flush-requiring value prediction이 아니라 RFP-style no-flush replay 경로를 목표로 한다.
- PUBS 대비: frontend branch-slice predictor가 아니라 TEA-derived H2P-chain identification을 쓴다.

---

## 9. 파일과 run 색인

### 주요 코드

| 경로 | 역할 |
|------|------|
| `src/dcache_stage.c` | Target Load profiling, online predictor, RF oracle |
| `src/dependency_chain_cache.c` | H2P backward slice, priority scope와 online RF filtering |
| `src/decoupled_frontend.cc` | Block Cache mask lookup과 op tagging |
| `src/node_issue_queue.cc` | IQ Priority scheduling과 normal-op interference 계측 |
| `src/zereco/h2p_mispred_latency.c` | H2P penalty 및 resolution-stage profiler |
| `src/core.param.def`, `src/zereco/zereco.stat.def` | 실험 parameter와 통계 정의 |

### Descriptor / runs

- 현재 descriptor: `~/scarab-infra/json/zereco_dbg.json`
- 현재 experiment: `zereco_260803_iq_priority_interference`
- 기존 IQ upper-bound 결과: `~/simulations/zereco_260802_iq_priority_online`
- Motivation/characterization 결과와 해석은 §6의 결과 디렉터리와 각 `analysis/`를 기준으로 한다.
- Reference papers: `/home/lee/scarab/reference/`

---

## 10. 현재 결론

1. H2P branch의 fetch-to-resolution은 전체 fetch-to-correct-fetch penalty의 지배 구간이다.
2. Fetch-to-resolution 안에서는 branch operand의 dependency wait가 가장 큰 비중을 차지한다.
3. Predictor가 exact vaddr를 맞힌 H2P-chain Target Load만 가속해도 dependency wait, resolution latency, 전체 penalty와 IPC가 함께 개선된다.
4. Stride는 높은 accuracy, top-delta는 높은 coverage라는 trade-off를 보이며, 두 방식 모두 평가 workload 전반에서 일관된 IPC 개선을 보였다.
5. 현재 수치는 실제 RF prefetcher의 cost와 timeliness를 모두 반영한 최종 성능이 아니라 predictor-gated upper bound이다.
6. ZERECO의 논문 포지션은 TEA의 expensive precomputation thread를 없애고, H2P-chain identification을 criticality filter로 사용해 RFP/PUBS의 비용 대비 효율을 높이는 방향이다.
