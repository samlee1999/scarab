# ZERECO: H2P-Chain Load 가속을 통한 Branch 오예측 조기 해소

> Last updated: 2026-08-04
>
> Simulator: Scarab / Scarab-infra
>
> 범위: TEA helper thread 없이 main-thread H2P dependence chain만 가속한다.

## 1. 연구 목표

History-based branch predictor가 계속 복잡해져도 data-dependent H2P(Hard-to-Predict) branch는 안정적으로 예측하기 어렵다. ZERECO는 predictor accuracy를 더 높이는 대신, 오예측된 H2P branch가 실제 outcome을 확인하는 시점을 앞당겨 misprediction penalty를 줄인다.

핵심 가설은 다음과 같다.

1. H2P misprediction penalty에서 branch fetch부터 resolution까지가 중요한 구간이다.
2. 이 구간은 branch dependence chain의 operand가 늦게 준비되어 길어진다.
3. 특히 chain 안의 Target Load latency가 dependency wait와 branch resolution을 늦춘다.
4. 예측 가능한 Target Load는 RF-style load acceleration으로 처리하고, RF가 안정적으로 처리하지 못하는 H2P slice는 PUBS-style IQ priority로 보완할 수 있다.

> H2P-chain membership을 criticality filter로 사용해 predictable load에는 RF acceleration을, residual slice에는 finite P-IQ를 적용함으로써 별도 precomputation thread 없이 branch resolution을 앞당긴다.

## 2. Motivation과 Prior Work

### 2.1 Fetch-to-resolution을 줄여야 하는 이유

Branch predictor가 틀리면 branch가 execute되어 실제 direction과 target이 확인될 때까지 wrong path가 계속 유입된다. Fetch-to-resolution은 다음 단계로 구성된다.

- frontend 통과
- dependence chain의 source operand가 준비되기를 기다리는 dependency wait
- operand-ready 이후 FU 선택까지의 scheduler wait
- branch execution

현재 profiler에서 `dependency`는 branch가 Node Table에 들어간 뒤 마지막 source operand가 ready될 때까지의 시간이다. 이 값에는 older producer의 실행과 load service latency가 포함된다. `scheduler`는 operand-ready 이후 issue되기까지의 시간이다.

ZERECO 실험에서는 fetch-to-resolution의 대부분이 dependency wait였고, Target Load latency를 줄였을 때 dependency wait, resolution latency와 IPC가 함께 개선되었다. 따라서 Target Load는 긴 resolution과 단순히 상관된 명령어가 아니라 직접 줄여볼 가치가 있는 bottleneck이다.

### 2.2 Prior Work의 한계

#### TEA

TEA는 H2P branch dependence chain을 helper thread에서 precompute하여 early flush를 만든다. 효과적인 대신 별도 fetch/rename 상태, register 관리, chain 실행과 shared backend resource가 필요하다. ZERECO는 TEA-derived H2P-chain identification은 사용하지만 helper thread와 duplicated chain execution은 사용하지 않는다.

- Reference: `/home/lee/scarab/reference/[2024, MICRO] Timely_Efficient_and_Accurate_Branch_Precomputation.pdf`

#### PUBS

PUBS는 low-confidence branch의 backward slice를 priority IQ entry에 배치하여 issue wait를 줄인다. 그러나 broadly marked slice가 많으면 priority entry 자체의 contention과 normal-entry 감소가 발생할 수 있고, scheduling을 앞당겨도 issue 이후의 cache/memory latency는 제거하지 못한다. ZERECO는 H2P-chain으로 대상을 제한하고, RF가 처리할 수 있는 slice를 priority population에서 제외하는 방식을 평가한다.

- Reference: `/home/lee/scarab/reference/[2018, MICRO] PUBS.pdf`

#### Branch Runahead

Branch Runahead는 dependence chain을 별도 엔진에서 미리 실행해 H2P branch outcome을 predictor보다 먼저 공급한다. 결과가 branch fetch 전에 준비되어야 하므로 timeliness 요구가 강하고 별도 chain extraction/execution 구조가 필요하다. ZERECO는 outcome 전체의 선행 계산이 아니라 main-thread branch의 fetch 이후 resolution을 앞당긴다.

- Reference: `/home/lee/scarab/reference/[2021, MICRO] Branch Runahead_Pruett,Y.Patt.pdf`

## 3. 검증된 Key Insights

| 결과 디렉터리 | 새롭게 확인한 내용 |
|---|---|
| `/home/lee/simulations/zereco/260624_h2p_chain_load_access_pattern_all_simpoints` | Target Load access는 제한된 load PC에 집중되며, 일부 address stream은 PC-local history로 예측 가능하다. 동시에 irregular tail이 존재하므로 RF만으로 모든 slice를 처리할 수 없다. |
| `/home/lee/simulations/zereco/260625_perf_comparison` | Target Load latency를 제거한 full oracle에서 큰 성능 headroom이 나타났다. 이는 prefetcher-OFF motivation 결과이며 realizable predictor 결과로 해석하지 않는다. |
| `/home/lee/simulations/zereco/zereco_260729_misp_penalty_breakdown` | H2P misprediction/misfetch의 fetch-to-resolution과 dependency wait가 중요하며, predictor가 실제로 cover한 Target Load만 가속해도 resolution latency와 IPC가 개선된다. |

이 결과들은 다음 인과관계를 지원한다.

```text
H2P branch를 predictor만으로 해결하기 어려움
  -> 오예측 후 fetch-to-resolution을 줄일 필요
  -> branch dependency wait가 긴 구간을 형성
  -> Target Load latency 감소가 resolution과 IPC를 함께 개선
  -> predictable load는 RF, residual slice는 IQ priority로 분리
```

## 4. 현재 Scarab 구현

현재 구현은 main-thread-only single-pass 모델이다. `tea_enable=0`을 사용하며 별도 TEA thread scheduling은 고려하지 않는다.

### 4.1 H2P chain identification

- Retired Fill Buffer snapshot에서 H2P branch별 backward dataflow walk를 수행한다.
- Register producer와 동일-address store 관계를 추적해 branch backward slice를 만든다.
- Block Cache에 `dependency_mask`, `iq_priority_candidate_mask`, `iq_priority_mask`를 저장한다.
- Frontend lookup으로 main-thread on-path op에 `chain_bit`과 IQ priority bit를 붙인다.
- 이번 finite P-IQ 실험은 H2P branch의 full backward slice만 사용한다.

### 4.2 Predictor-gated RF model

- Target Load는 main-thread on-path H2P backward-slice load이다.
- Per-PC Stride predictor가 exact virtual address를 예측한다.
- 같은 dynamic load는 한 번만 `predict -> actual address 비교 -> update`를 수행한다.
- Exact-address prediction이 맞고 store-forwarding conflict가 없을 때만 1-cycle RF service를 적용한다.
- Abstain, wrong address, store conflict는 정상 demand path를 사용한다.

이 모델은 dcache-stage에서 correct prediction의 load latency를 줄이는 predictor-gated upper bound다. 실제 RF prefetch의 early launch, bandwidth, timeliness와 wrong-prediction recovery 비용은 아직 모델링하지 않는다.

### 4.3 Online RF filtering

`zereco_iq_priority_policy=2`는 retired dynamic slice의 RF 결과로 같은 H2P branch의 다음 occurrence를 제어한다.

- 모든 Target Load가 RF-covered인 occurrence가 2회 연속 관찰되면 다음 occurrence의 IQ priority를 억제한다.
- Target Load가 하나라도 abstain, wrong-address 또는 store-conflict이면 streak을 reset하고 slice 전체에 priority를 유지한다.
- Target Load가 없는 H2P slice도 priority를 유지한다.
- 여러 H2P slice가 같은 block slot을 공유하면, 그 op가 uncovered slice 하나에라도 필요할 때 priority를 유지한다.

현재 occurrence의 load 결과를 older producer에 소급하지 않으므로 two-pass oracle mask가 아니다.

### 4.4 Finite P-IQ

각 distributed RS의 기존 `main_rs_limit`을 Priority/Normal 전용 partition으로 나눈다.

```text
ready-op selection: Priority op > Normal op
same class: oldest-first
dispatch: matching partition이 full이면 in-order stall
spill: Priority <-> Normal partition 간 entry 공유 없음
```

현재 baseline의 distributed main-thread RS 용량은 `185/132/36`이다. P-IQ config만 총 RS 용량이 커지지 않도록 이 용량 안에서 비율을 적용한다.

| P-IQ 비율 | RS0 / RS1 / RS2 Priority entries |
|---:|---:|
| 10% | 19 / 13 / 4 |
| 15% | 28 / 20 / 5 |
| 20% | 37 / 26 / 7 |
| 25% | 46 / 33 / 9 |
| 50% | 93 / 66 / 18 |

기존 `iq_all_h2p`는 capacity를 분할하지 않는 select-only scheduling reference다. 50% P-IQ도 strict finite partition이므로 select-only와 동일한 upper bound가 아니다.

## 5. 현재 P-IQ 실험

- Descriptor: `/home/lee/scarab-infra/json/zereco_dbg.json`
- Output directory: `/home/lee/simulations/zereco_260804_piq_sweep`
- Workload/SimPoint 집합은 descriptor에 명시된 기존 공통 집합을 사용한다.
- `mispred`와 `misfetch`를 모두 H2P penalty profiler에 포함한다.

### 5.1 Configuration

Reference configuration은 세 개다.

| Config | 의미 |
|---|---|
| `baseline` | RF OFF, P-IQ OFF, oldest-first |
| `iq_all_h2p` | RF OFF, 모든 H2P full slice에 select-only priority |
| `rf_stride_only` | Stride RF ON, IQ scheduling/P-IQ OFF |

Finite P-IQ는 `R={10,15,20,25,50}`에 대해 세 정책을 비교한다.

| Config family | RF | P-IQ 대상 |
|---|---|---|
| `piq_all_h2p_R` | OFF | 모든 H2P full slice |
| `rf_stride_unfiltered_piq_R` | Stride | RF coverage와 관계없이 모든 H2P full slice |
| `rf_stride_filtered_piq_R` | Stride | online RF filtering 후 남은 H2P full slice |

총 configuration 수는 reference 3개와 finite P-IQ 15개를 합친 18개다. 이번 단계에서는 Top-delta와 Target-Load-prefix policy를 실행하지 않는다.

### 5.2 확인할 질문

1. `iq_all_h2p / baseline`: select contention을 우선 처리하는 것만으로 성능과 H2P latency가 개선되는가?
2. `piq_all_h2p_R / baseline`: Priority population이 finite P-IQ를 포화시키는가? Normal partition 축소가 성능 향상을 반납시키는가?
3. 같은 `R`에서 `rf_stride_filtered_piq_R / rf_stride_unfiltered_piq_R`: RF filtering이 Priority population과 P-IQ contention을 줄이는가?
4. `rf_stride_filtered_piq_R / rf_stride_only`: residual slice priority가 RF-only 대비 추가적인 IPC와 resolution 개선을 만드는가?

### 5.3 핵심 통계

| 통계 | 의미 |
|---|---|
| `Periodic_IPC`, `ZERECO_H2P_FETCH_TO_RESOLUTION_*`, `ZERECO_H2P_DEPENDENCY_*`, `ZERECO_H2P_SCHEDULER_*` | 성능과 H2P resolution 경로 |
| `ZERECO_IQ_PRIORITY_MARKED_PORTION`, `ZERECO_IQ_READY_PRIORITY_AVG` | Priority population |
| `ZERECO_IQ_PRIORITY_CONTENTION_CYCLES` | ready Priority op가 다른 Priority winner와 같은 FU를 경쟁한 cycle |
| `ZERECO_IQ_PRIORITY_NORMAL_COMPETITION_CYCLES` | ready Priority와 Normal op가 같은 FU 선택 후보였던 cycle |
| `ZERECO_IQ_NORMAL_DISPLACED_BY_PRIORITY_*` | Priority-first selection 때문에 oldest-first normal selection이 바뀐 효과 |
| `ZERECO_PIQ_*_OCCUPANCY_PCT`, `ZERECO_PIQ_*_FULL_*` | Priority/Normal partition 사용률과 포화도 |
| `ZERECO_PIQ_*_DISPATCH_STALL_CYCLES`, `ZERECO_PIQ_*_DISPATCH_WAIT_*` | matching partition 부족으로 발생한 dispatch 지연 |
| `ZERECO_PIQ_*_STALL_WITH_UNUSED_*`, `ZERECO_PIQ_UNUSED_*_SLOTS_*` | 반대 partition이 비어 있는데도 strict partition 때문에 발생한 capacity loss |
| `ZERECO_IQ_SHADOW_SELECTION_MISMATCHES` | scheduling OFF reference에서 실제 oldest-first와 shadow 결과의 일치성; 반드시 0 |
| `ZERECO_PIQ_PARTITION_INTEGRITY_MISMATCHES` | RS main/Priority/Normal occupancy accounting 일치성; 반드시 0 |

해석 기준은 다음과 같다.

- Priority full/stall과 P-vs-P contention이 높으면 all-H2P priority population이 너무 크다는 가설을 지지한다.
- Normal full/stall과 unused Priority slot이 증가하면서 IPC가 떨어지면 P-IQ 비율이 과도하게 크다는 뜻이다.
- Filtered가 같은 비율의 unfiltered보다 Priority population/stall을 줄이고 IPC를 높이면 RF filtering의 당위성이 생긴다.
- P-IQ와 select contention이 모두 낮다면 IQ priority가 작은 이유는 scheduler contention이 아니라 operand/cache/memory latency가 지배적이기 때문일 가능성이 크다.

## 6. 결과 집계 원칙

- 완료된 공통 SimPoint만 configuration 간 비교에 사용한다.
- Workload 내부 latency는 SimPoint weight를 정규화한 뒤 `weighted total cycles / weighted event count`로 계산한다.
- 전체 latency 대표값은 workload-equal mean을 사용한다.
- IPC speedup의 AVG는 workload별 IPC ratio의 geometric mean을 사용한다.
- Coverage는 `correct predictions / candidate Target Loads`, accuracy는 `correct predictions / predictions made`다.
- 결과 인용 시 config의 `PARAMS.out`, 완료 상태와 integrity counter를 먼저 확인한다.

## 7. 현재 한계와 다음 판단

- RF 결과는 predictor-gated latency upper bound다. P-IQ headroom이 확인된 뒤 실제 launch point, RF capacity/bandwidth와 recovery를 모델링한다.
- Finite P-IQ는 strict static partition이다. Sweep 결과에 따라 적절한 비율을 선택하고, 필요할 때만 dynamic borrowing 또는 overflow policy를 검토한다.
- 현재 H2P identification 구조는 연구용 Fill Buffer와 backward walk를 유지한다. 최종 논문에서는 identification storage와 walk cost를 별도로 정량화해야 한다.

## 8. 주요 코드와 자료

| 경로 | 역할 |
|---|---|
| `src/dcache_stage.c` | online address predictor와 predictor-gated RF service |
| `src/fill_buffer.c`, `src/dependency_chain_cache.c` | H2P backward walk, full-slice mask와 online RF filtering |
| `src/decoupled_frontend.cc` | Block Cache lookup과 main-thread op tagging |
| `src/exec_ports.c`, `src/node_stage.c` | distributed RS partition과 recovery-safe occupancy 관리 |
| `src/node_issue_queue.cc` | P-IQ dispatch, Priority-first scheduling과 interference 계측 |
| `src/zereco/h2p_mispred_latency.c` | H2P misprediction/misfetch penalty profiler |
| `src/core.param.def`, `src/zereco/zereco.stat.def` | ZERECO parameter와 통계 정의 |
| `/home/lee/scarab-infra/json/zereco_dbg.json` | 현재 18-configuration P-IQ sweep descriptor |
| `/home/lee/scarab/reference/` | TEA, PUBS, Branch Runahead reference papers |
