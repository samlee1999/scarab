# ZERECO: H2P-Chain Load 가속을 통한 Branch 오예측 조기 해소

> Last updated: 2026-08-06
> Simulator: Scarab / Scarab-infra
> 범위: 별도 helper thread 없이 main-thread H2P dependence chain만 가속한다.

## 1. 연구 문제와 Thesis

복잡한 history-based branch predictor도 data-dependent H2P(Hard-to-Predict) branch를 안정적으로 예측하기는 어렵다. Predictor complexity를 계속 높이는 방법은 저장 공간, 접근 latency와 설계 복잡도를 키우지만 H2P branch의 정확도 개선은 제한적일 수 있다.

ZERECO는 예측 정확도를 더 높이는 대신 다음 질문에서 출발한다.

> H2P branch를 피하기 어렵다면, 오예측을 더 빨리 발견해 penalty를 줄일 수 있는가?

핵심 Thesis는 다음과 같다.

> H2P branch의 긴 fetch-to-resolution latency는 dependence chain의 operand 준비, 특히 chain 안의 Target Load latency에 크게 좌우된다. 예측 가능한 Target Load는 RF로 가속하고, RF로 처리하기 어려운 residual chain은 priority scheduling으로 가속해 branch resolution을 앞당긴다.

이 문서에서 사용하는 핵심 용어는 다음과 같다.

- **H2P branch**: 반복적으로 높은 misprediction을 보이며 history만으로 안정적으로 예측하기 어려운 branch
- **Backward slice**: branch outcome 계산에 필요한 older producer 명령어의 data-dependence chain
- **Target Load**: H2P branch의 on-path backward slice에 속해 branch operand 준비에 영향을 주는 load
- **RF acceleration**: 예측한 load address의 데이터를 Register File에 미리 공급해 load service latency를 줄이는 방식
- **P-IQ**: H2P slice op를 우선 scheduling하기 위한 논리적 Priority-IQ/RS 영역

## 2. Motivation

### 2.1 Accuracy가 아니라 penalty를 줄이는 이유

TAGE-SC-L처럼 크고 복잡한 history-based predictor도 load가 만든 runtime value에 direction이 의존하는 branch를 안정적으로 맞히기 어렵다. 더 많은 history와 predictor table에 비용을 투자해도 load value 자체를 알 수 없으므로 H2P branch의 정확도 개선에는 한계가 있다.

ZERECO는 predictor complexity를 더 높이는 데 사용할 storage, lookup과 energy budget을 branch misprediction penalty를 줄이는 데 투자할 수 있는지 묻는다. Prediction 정확도를 높이는 대신, direction을 결정하는 dependence chain을 가속해 실제 outcome과 misprediction을 더 일찍 확인하는 것이 핵심이다. Misprediction을 일찍 detect하면 recovery와 redirect도 더 빨리 시작할 수 있으므로, off-path instruction의 fetch를 그만큼 일찍 중단하고 correct path로 복귀할 수 있다.

### 2.2 Fetch-to-resolution이 중요한 이유

Branch가 잘못 예측되면 실제 direction과 target이 확인될 때까지 wrong-path instruction이 계속 유입된다. 전체 misprediction timeline은 크게 다음과 같이 볼 수 있다.

```text
branch fetch
  -> branch resolution / misprediction detection
  -> recovery and redirect
  -> first correct-path fetch
```

Fetch-to-resolution은 branch operand와 execution latency에 의해 결정되므로 chain 가속이 직접 줄일 수 있다. 다만 이 구간의 비중만으로 IPC bottleneck이 증명되지는 않으며, 실제 latency를 줄였을 때 resolution과 IPC가 함께 개선되어야 한다.

### 2.3 왜 resolution이 늦어지는가

Scarab profiler는 fetch-to-resolution을 다음 단계로 나눈다.

- `frontend`: branch fetch부터 Node Table 진입까지
- `dependency`: Node Table 진입부터 마지막 source operand가 ready될 때까지
- `scheduler`: operand-ready부터 FU 선택까지
- `execution`: branch issue부터 실행 완료까지

긴 H2P resolution에서는 `dependency`가 가장 크다. 이 시간에는 backward-slice producer의 실행, Target Load의 address generation과 cache/memory service가 포함되므로 scheduling과 load latency를 서로 다른 수단으로 줄여야 한다.

## 3. Key Observations와 Root Cause

### 3.1 Fetch-to-resolution에는 줄일 수 있는 headroom이 있다

H2P misprediction/misfetch의 전체 penalty에서 fetch-to-resolution이 큰 portion을 차지한다. 이는 branch outcome을 더 일찍 계산하는 접근이 의미 있는 구조적 headroom을 가짐을 보여준다.

### 3.2 Dependency wait이 긴 resolution의 중심이다

Fetch-to-resolution 내부에서는 branch의 src operand가 준비되기를 기다리는 시간이 지배적이다. Branch 자체의 ready-to-issue 시간만 보는 것으로는 이 병목을 설명할 수 없으며, branch까지 이어지는 older producer chain을 함께 봐야 한다.

### 3.3 왜 Target Load에 RF prefetch를 적용하는가

Exact address가 예측된 Target Load만 짧은 RF latency로 서비스해도 dependency wait, fetch-to-resolution과 IPC가 함께 개선된다. 이 sensitivity 결과는 Target Load latency가 현재 모델에서 성능에 영향을 주는 원인임을 보여준다.

일반적인 cache prefetch는 data를 cache 가까이 가져오더라도 demand load가 address generation, scheduling과 cache access를 거쳐 destination register를 채울 때까지 dependent op를 깨울 수 없다. RF prefetch는 Target Load의 address를 미리 예측해 value fetch를 앞당기고 그 값을 destination register에서 직접 사용할 수 있게 한다. 따라서 branch backward slice의 consumer를 더 일찍 실행시켜 dependency wait을 줄이려는 ZERECO의 목적에 더 직접적이다.

### 3.4 모든 Target Load를 RF prefetch할 수는 없다

Target Load access는 일부 load PC에 집중되고, PC-local history로 높은 정확도에서 예측할 수 있는 address stream이 존재한다. 동시에 irregular한 access도 남으므로 모든 Target Load를 가속하는 oracle은 현실적인 설계가 아니다.

잘못된 cache prefetch는 주로 bandwidth와 cache pollution을 낭비하지만, RF prefetch가 wrong address의 값이나 stale value를 register에 공급하면 dependent op와 branch가 잘못된 값으로 실행될 수 있다. 이를 검출하려면 address/value validation이 필요하고, 실패하면 replay 또는 pipeline recovery 비용이 발생한다. 따라서 높은 confidence로 exact address를 예측할 수 있고 memory-order/store conflict가 없는 Target Load만 RF 대상으로 선택해야 한다. 나머지 Target Load는 정상 demand path를 사용하고 그 residual slice는 IQ priority로 보완한다.

현재 시뮬레이터는 exact-address prediction과 conflict 부재를 oracle로 확인한 경우에만 짧은 RF latency를 적용하며, wrong-value injection과 그 recovery cost는 아직 모델링하지 않는다. 따라서 현재 RF 결과는 선택 가능한 Target Load의 잠재력을 보는 predictor-gated upper bound다.

### 3.5 IQ priority는 RF를 보완하는 secondary mechanism이다

Oldest-first는 이미 older producer를 우대하므로 P-IQ가 바꿀 수 있는 winner가 적다. Random-physical sensitivity에서는 age advantage가 제거될 때 priority가 dependency wait과 IPC를 개선할 수 있음을 확인했다.

Strict partition은 dispatch stall을 만들지만 non-stall fallback은 이를 줄인다. RF와 결합한 filtered P-IQ는 RF-only에 추가 이득을 만들 수 있으나, 효과는 RF보다 작고 priority population 감소가 항상 성능 증가로 이어지지는 않는다.

## 4. Observation에서 ZERECO 구조가 도출되는 과정

앞의 관찰은 다음 설계로 이어진다.

```text
1. H2P branch와 backward slice 식별
2. slice 안의 Target Load address 예측
3. RF-covered Target Load의 service latency 가속
4. RF로 처리하기 어려운 residual slice에 IQ priority 부여
5. finite P-IQ가 full이면 Normal entry로 fallback
6. main-thread branch의 resolution을 앞당김
```

RF를 먼저 적용하는 이유는 긴 dependency wait의 큰 부분인 load latency를 직접 줄일 수 있기 때문이다. IQ priority는 load address를 만들기 위한 producer와 RF-uncovered residual chain의 scheduling을 앞당기는 보완 경로다.

RF filtering은 이미 RF로 처리 가능한 slice까지 모두 priority로 표시해 P-IQ가 사실상 normal IQ처럼 되는 것을 막기 위한 장치다. 다만 filtering은 priority population을 제어하는 수단이지, 그 자체가 성능 향상을 보장하지는 않는다.

## 5. ZERECO Mechanism Overview

### 5.1 H2P chain identification

- Retired Fill Buffer에서 register producer와 동일-address store를 따라 H2P backward slice를 만든다.
- Block Cache mask와 frontend lookup으로 main-thread on-path op에 chain/priority 정보를 붙인다.
- TEA의 identification 아이디어만 사용하며 helper thread는 두지 않는다.

### 5.2 Predictor-gated RF acceleration

- Per-PC Stride predictor가 Target Load의 virtual address를 예측한다.
- Dynamic load마다 한 번만 `predict -> actual address 비교 -> update`를 수행한다.
- Exact prediction이고 store conflict가 없을 때만 RF service를 적용하며, 나머지는 정상 demand path를 사용한다.

실제 RF prefetch의 early launch, value validation/recovery와 storage/bandwidth 동작은 후속 모델 범위다.

### 5.3 Online RF filtering

- Retired RF 결과로 같은 H2P branch의 다음 occurrence를 제어한다.
- Target Load가 안정적으로 covered되면 priority를 억제하고, uncovered load가 있거나 Target Load가 없으면 유지한다.
- 현재 occurrence에 결과를 소급하지 않는 single-pass 모델이다.

### 5.4 Priority scheduling과 finite P-IQ

Ready op의 선택 순서는 다음과 같다.

```text
ZERECO Priority op > Normal op
same class: configured baseline scheduling policy
```

Scarab에서는 중앙 IQ 기능이 distributed RS, Node Table과 issue queue에 나뉜다. P-IQ는 각 RS의 기존 용량 안에서 Priority/Normal admission capacity를 나누는 논리적 partition이다.

Non-stall policy에서는 Priority partition이 full이면 op를 Normal entry에 배치하고 이후 Normal op로 scheduling한다. Normal op는 Priority partition을 사용하지 않는다.

Scheduler policy는 다음 두 가지를 지원한다.

- **Oldest-first**: 낮은 `op_num` 우선; 현실적인 기본 baseline
- **Random-physical**: 낮은 physical RS entry ID 우선; oldest-first가 감추는 priority headroom을 확인하는 sensitivity baseline

## 6. Prior Work와 ZERECO의 차이

| Prior Work | 가져오는 요소 | 그대로 사용하지 않는 부분과 ZERECO의 차이 |
|---|---|---|
| TEA | H2P branch와 backward-slice identification | Helper thread, duplicated chain execution과 별도 architectural state를 사용하지 않는다. |
| RFP | Load address prediction과 RF prefetch | 모든 load가 아니라 H2P resolution에 영향을 주는 Target Load만 선택한다. 현재 early-launch hardware timing은 후속 과제다. |
| PUBS | Branch backward-slice op의 IQ priority | Scheduling만으로 제거할 수 없는 cache/memory latency를 RF로 먼저 줄이고, residual slice에 non-stall P-IQ를 적용한다. |
| Branch Runahead | Dependence-chain 기반 branch 가속 | 별도 엔진에서 outcome 전체를 선행 계산하지 않고 main-thread의 fetch 이후 resolution을 줄인다. |

References:

- `/home/lee/scarab/reference/[2024, MICRO] Timely_Efficient_and_Accurate_Branch_Precomputation.pdf`
- `/home/lee/scarab/reference/[2022, ISCA] Reg File prefetching.pdf`
- `/home/lee/scarab/reference/[2018, MICRO] PUBS.pdf`
- `/home/lee/scarab/reference/[2021, MICRO] Branch Runahead_Pruett,Y.Patt.pdf`

## 7. 현재 구현

현재 구현은 `tea_enable=0`인 main-thread-only single-pass 모델이며 `mispred`와 `misfetch`를 모두 H2P profiler에 포함한다.

| 경로 | 역할 |
|---|---|
| `src/dcache_stage.c` | Online address predictor와 predictor-gated RF service |
| `src/fill_buffer.c`, `src/dependency_chain_cache.c`, `src/decoupled_frontend.cc` | H2P backward walk, slice mask, RF filtering과 op tagging |
| `src/exec_ports.c`, `src/node_stage.c` | Distributed RS, physical entry와 P-IQ occupancy 관리 |
| `src/node_issue_queue.cc` | Oldest/random-physical scheduling과 Priority-first selection |
| `src/zereco/h2p_mispred_latency.c` | H2P misprediction/misfetch latency profiler |
| `src/core.param.def`, `src/zereco/zereco.stat.def` | Parameter와 통계 정의 |
| `/home/lee/scarab-infra/json/zereco_dbg.json` | 현재 build/run descriptor |

필수 무결성 조건은 physical-entry, shadow-selection, P-IQ partition과 non-stall 관련 mismatch counter가 모두 0인 것이다.

## 8. 실험 근거와 검증 범위

세부 그래프와 수치는 아래 결과 디렉터리의 `analysis/`를 기준으로 확인한다.

| 결과 디렉터리 | 이 실험에서 확인한 내용 |
|---|---|
| `/home/lee/simulations/zereco/260624_h2p_chain_load_access_pattern_all_simpoints` | Target Load가 일부 PC에 집중되며 predictable stream과 irregular tail이 함께 존재한다. |
| `/home/lee/simulations/zereco/260625_perf_comparison` | Target Load latency를 제거할 때의 큰 성능 headroom을 확인한 full-oracle motivation 실험이다. |
| `/home/lee/simulations/zereco/zereco_260729_misp_penalty_breakdown` | Penalty portion, dependency-stage dominance와 predictor-gated RF의 resolution/IPC 효과를 확인했다. |
| `/home/lee/simulations/zereco_260802_iq_priority_online` | Online RF filtering이 priority population을 줄이지만 oldest-first에서 P-IQ의 추가 효과는 작음을 확인했다. |
| `/home/lee/simulations/zereco_260803_iq_priority_interference` | Priority scheduling이 normal-op selection에 미치는 영향과 shared-RS select-only 효과를 계측했다. |
| `/home/lee/simulations/zereco_260804_piq_sweep` | Strict finite partition의 stall 비용과 RF filtering의 population/stall 감소를 확인했다. |
| `/home/lee/simulations/zereco_260805_piq_nonstall_sweep` | Non-stall fallback이 partition blocking을 줄이며, oldest-first에서 남는 P-IQ headroom이 작음을 확인했다. |
| `/home/lee/simulations/zereco_260805_random_iq_comparison` | Oldest-first와 random-physical의 scheduling headroom 차이를 비교하고 논문용 분석 자료를 정리했다. |
| `/home/lee/simulations/zereco_260806_random_piq_policy_sweep (random_iq 적용)` | Random-physical에서 P-IQ 효과, stall/non-stall 차이와 finite P-IQ ratio sensitivity를 확인했다. |

집계 원칙:

- 완료된 공통 SimPoint만 configuration 간 비교한다.
- Workload 내부 latency는 SimPoint-weighted total cycles / weighted event count로 계산한다.
- 전체 latency는 workload-equal mean, IPC speedup은 workload별 IPC ratio의 geometric mean을 사용한다.
- Coverage는 correct predictions / candidate Target Loads, accuracy는 correct predictions / predictions made다.
- 결과 인용 전 `PARAMS.out`, 완료 상태와 integrity counter를 확인한다.

## 9. 현재 모델의 한계와 Open Items

- Predictor-gated RF는 latency upper bound다. 실제 prediction/launch timing, RF capacity/bandwidth와 recovery를 모델링해야 한다.
- H2P identification은 연구용 Fill Buffer와 backward walk를 사용한다. 최종 storage, lookup과 walk cost가 필요하다.
- Random-physical은 P-IQ scheduling headroom을 분리하는 sensitivity 도구이며, 현실적인 oldest-first baseline을 대체하는 최종 성능 기준은 아니다.
- P-IQ가 줄일 수 있는 것은 ready-op selection과 producer scheduling 지연이다. Operand-not-ready와 cache/memory wait까지 모두 제거할 수는 없다.
- RF filtering threshold와 P-IQ ratio는 priority population, fallback과 normal-op pressure를 함께 고려해 결정해야 한다.
