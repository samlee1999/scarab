# ZERECO: H2P-Chain Load 가속을 통한 Branch 오예측 조기 해소

> 연구 README. Last updated: 2026-07-30.
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
   - Fetch 시 Block Cache mask로 main-thread instruction에 chain bit와 load bit를 태깅한다.

2. **Predictable load acceleration**
   - Exact virtual address를 높은 신뢰도로 예측할 수 있으면 **RFP-style RF prefetch**를 수행한다.
   - 주소가 틀린 경우 wrong value를 commit하지 않고 기존 scheduler replay 또는 demand load path로 복구하는 no-flush 방향을 전제로 한다.

3. **Unpredictable chain fallback**
   - 주소 예측이 어려운 chain op/load는 **PUBS-style IQ priority**로 issue 대기 시간을 줄인다.
   - 이 축은 RF prefetch를 대체하는 주력 메커니즘이 아니라, predictor tail을 줄이는 보조 메커니즘이다.

### TEA 구조의 슬림화 방향

TEA의 Block Cache를 identification 전용으로 축소하면, 사실상 **Chain Mask Cache**가 된다. 이 구조는 precomputation thread를 위한 decoded uop store가 아니라 "어떤 slot이 H2P chain member인지"를 알려주는 mask cache이다.

| 필드 | 크기 예시 | 용도 |
|------|-----------|------|
| `tag` | 약 40-bit | basic-block start PC 식별 |
| `chain mask` | 32-bit | block 내 H2P-chain member slot 표시. Dispatch priority 및 Fill Buffer walk 시작점으로 사용 |
| `load mask` | 32-bit | chain slot 중 load 표시. Fetch 시 RF predictor 조회 대상으로 사용 |

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

### 구현된 기능

- **Perfect-load oracle**
  - Parameter: `h2p_chain_perfect_load`, `h2p_chain_perfect_load_latency`
  - Main hook: `src/dcache_stage.c:dcache_stage_try_main_chain_load_oracle`
  - 모든 on-path H2P-chain target load의 latency를 고정 또는 predictor-gated 방식으로 낮춘다.

- **Access-pattern profiling**
  - Parameter: `h2p_chain_load_pattern_profile`
  - Per-PC reuse, stride, delta, latency, hit-level 특성을 수집한다.

- **Raw stream dump와 offline replay**
  - Parameter: `h2p_chain_load_raw_stream_dump`
  - Tool: `src/tools/h2p_chain_load_predictor_replay.py`
  - Last-value, stride, top-delta, Markov 계열을 vaddr/cache-line 단위로 replay한다.

- **Online predictor oracle**
  - Offline replay와 같은 알고리즘을 timing simulation 내부에서 수행하고 oracle을 gating한다.
  - Parameter:
    - `h2p_chain_oracle_predictor`: `0=none/all`, `1=stride`, `2=top-delta`
    - `h2p_chain_oracle_granularity`: `0=vaddr/RF`, `1=line/L1`
    - `h2p_chain_oracle_hit_latency`: vaddr/RF path latency, 기본 1
    - `h2p_chain_oracle_stride_confidence`: 기본 2
    - `h2p_chain_oracle_min_count`: 기본 2
  - Predictor stats: `H2P_CHAIN_LOAD_ORACLE_CANDIDATES`, `H2P_CHAIN_LOAD_ORACLE_PRED_MADE`, `H2P_CHAIN_LOAD_ORACLE_PRED_CORRECT`, `H2P_CHAIN_LOAD_ORACLE_PRED_WRONG`, `H2P_CHAIN_LOAD_ORACLE_BYPASSED`

- **H2P misprediction-latency profiler**
  - Parameter: `zereco_h2p_mispred_latency_profile` (기본 OFF, experiment descriptor에서 ON)
  - Main implementation: `src/zereco/h2p_mispred_latency.c`
  - Branch fetch-to-resolution, post-resolution, first correct-path fetch와 frontend/dependency/scheduler/execution stage를 수집한다.
  - Execute-resolved H2P misprediction과 misfetch를 모두 포함한다.

### 구현상 중요한 정합성 장치

- `op->h2p_oracle_pred_checked` guard로 predictor는 각 dynamic load를 한 번만 통과시킨다. Prediction miss 이후 demand cache access가 재시도되더라도 같은 load가 실제 주소를 학습한 뒤 다시 예측되는 self-training은 허용하지 않는다.
- Predictor는 predict-then-update 순서를 사용해 현재 instance의 실제 주소가 현재 prediction에 누설되지 않도록 한다.
- Store-forwarding 가능 load는 RF bypass에서 제외하고 정상 memory path를 사용한다.
- Line granularity 비교 코드는 남아 있지만 최신 `260729` 실험에서는 exact-vaddr/RF config만 사용했다.

### 외부 mask 방식은 별도 open item

외부 mask 방식은 ZERECO의 실제 hardware mechanism이 아니라, **offline predictor 분석 결과를 timing simulation에서 그대로 재현하기 위한 실험용 주입 방식**이다. Offline replay는 raw trace를 읽으면서 각 dynamic Target Load instance에 대해 predictor가 성공했는지를 미리 판정할 수 있다. 이 결과를 bit mask로 저장하고, timing simulator가 해당 bit만 읽어 선택된 load의 latency를 줄이는 방식이다.

예상 형식:

```text
<load_pc_hex> <n_instances> <bitstring>
```

각 필드의 의미는 다음과 같다.

- `load_pc_hex`: 동일한 static load instruction을 식별하는 PC
- `n_instances`: offline trace에서 관측한 해당 load PC의 dynamic 실행 횟수
- `bitstring`: program order에 따른 각 dynamic instance의 선택 여부. `1`이면 predictor가 성공한 instance로 간주해 RF-style latency를 적용하고, `0`이면 정상 cache/memory path를 사용

예를 들어 다음 mask가 있다고 가정한다.

```text
0x400abc 5 10110
```

PC `0x400abc`의 load가 다섯 번 실행될 때 첫 번째, 세 번째, 네 번째 dynamic instance만 offline predictor가 성공했다는 의미다. Timing simulator는 이 load PC를 만날 때마다 per-PC instance counter로 몇 번째 실행인지를 확인한다. 대응 bit가 `1`이면 oracle bypass를 적용하고, `0`이면 baseline과 동일하게 실행한다.

이 방식이 올바르게 동작하려면 다음 조건이 필요하다.

1. Offline replay와 timing simulation이 동일한 SimPoint, warmup 범위와 Target Load filter를 사용해야 한다.
2. 같은 load PC의 dynamic instance가 두 환경에서 정확히 같은 program order로 관측되어야 한다.
3. Wrong-path load, store-forwarding 제외 load와 동일 load의 cache-access retry가 mask counter를 잘못 소비하면 안 된다.
4. Mask의 `n_instances`와 simulation에서 실제로 소비한 instance 수가 일치하는지 검증해야 한다.

이 조건 중 하나라도 어긋나면 이후 mask bit가 모두 다른 dynamic instance에 적용되는 alignment error가 발생한다. 따라서 실제 구현에는 mask 파일 loader, load-PC별 instance counter, bit 범위 검사와 simulation 종료 시 instance-count validation이 필요하다.

현재 online predictor oracle은 simulator 내부에서 각 dynamic load에 대해 직접 predict-then-update를 수행하고, prediction이 맞은 경우에만 latency를 줄인다. 따라서 현재 predictor-gated IPC 실험에는 외부 mask가 필요하지 않으며, offline trace와 dynamic-instance 순서를 맞추는 문제도 없다.

외부 mask 방식은 향후 별도의 offline predictor가 선택한 고정 subset을 timing simulation에서 정확히 재생하거나, offline 결과와 timing 결과를 instance 단위로 교차검증할 때만 필요한 선택 사항이다. 정리하면 online predictor는 **simulation 중 predictor를 직접 실행하는 방식**이고, 외부 mask는 **simulation 전에 만든 정답지를 읽어 지정된 dynamic instance만 가속하는 방식**이다.

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
| `baseline` | golden_cove, stream prefetcher ON, 정상 cache/memory latency |
| `pred_stride_vaddr` | Online stride predictor가 exact vaddr를 맞힌 Target Load만 1-cycle RF service |
| `pred_top_delta_vaddr` | Online top-delta predictor가 exact vaddr를 맞힌 Target Load만 1-cycle RF service |

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
- Prefetcher-ON 환경에서 priority entry 수, stall/non-stall dispatch, mode switch를 처음부터 sweep해야 한다.

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

### 코드

- `src/dcache_stage.c`
  - `dcache_stage_try_main_chain_load_oracle`
  - perfect-load oracle
  - online predictor oracle
  - access-pattern profiling hooks
- `src/core.param.def`
  - `h2p_chain_perfect_load`
  - `h2p_chain_perfect_load_latency`
  - `h2p_chain_load_pattern_profile`
  - `h2p_chain_load_raw_stream_dump`
  - `h2p_chain_oracle_predictor`
  - `h2p_chain_oracle_granularity`
  - `h2p_chain_oracle_hit_latency`
  - `h2p_chain_oracle_stride_confidence`
  - `h2p_chain_oracle_min_count`
  - `zereco_h2p_mispred_latency_profile`
- `src/zereco/h2p_mispred_latency.c`
  - H2P misprediction/misfetch timeline 및 resolution stage profiler
- `src/zereco/zereco.stat.def`
  - `ZERECO_H2P_FETCH_TO_RESOLUTION_*`
  - `ZERECO_H2P_FETCH_TO_CORRECT_FETCH_*`
  - `ZERECO_H2P_{FRONTEND,DEPENDENCY,SCHEDULER,EXECUTION}_*`
- `src/tea/tea.stat.def`
  - `H2P_CHAIN_LOAD_ORACLE_CANDIDATES`
  - `H2P_CHAIN_LOAD_ORACLE_PRED_MADE`
  - `H2P_CHAIN_LOAD_ORACLE_PRED_CORRECT`
  - `H2P_CHAIN_LOAD_ORACLE_PRED_WRONG`
  - `H2P_CHAIN_LOAD_ORACLE_BYPASSED`
- `src/tools/h2p_chain_load_predictor_replay.py`
  - raw stream offline replay

### Descriptor / runs

- Descriptor:
  - `~/scarab-infra/json/zereco_dbg.json`
  - configs: `baseline`, `pred_stride_vaddr`, `pred_top_delta_vaddr`
- Runs:
  - `~/simulations/zereco/260624_h2p_chain_load_access_pattern_all_simpoints`: access pattern + offline replay
  - `~/simulations/zereco/260625_perf_comparison`: prefetcher-OFF full oracle motivation
  - `~/simulations/zereco/zereco_260729_misp_penalty_breakdown`: prefetcher-ON penalty breakdown + predictor-gated RF 결과
- Reference papers:
  - `/home/lee/scarab/reference/`

---

## 10. 현재 결론

1. H2P branch의 fetch-to-resolution은 전체 fetch-to-correct-fetch penalty의 지배 구간이다.
2. Fetch-to-resolution 안에서는 branch operand의 dependency wait가 가장 큰 비중을 차지한다.
3. Predictor가 exact vaddr를 맞힌 H2P-chain Target Load만 가속해도 dependency wait, resolution latency, 전체 penalty와 IPC가 함께 개선된다.
4. Stride는 높은 accuracy, top-delta는 높은 coverage라는 trade-off를 보이며, 두 방식 모두 평가 workload 전반에서 일관된 IPC 개선을 보였다.
5. 현재 수치는 실제 RF prefetcher의 cost와 timeliness를 모두 반영한 최종 성능이 아니라 predictor-gated upper bound이다.
6. ZERECO의 논문 포지션은 TEA의 expensive precomputation thread를 없애고, H2P-chain identification을 criticality filter로 사용해 RFP/PUBS의 비용 대비 효율을 높이는 방향이다.
