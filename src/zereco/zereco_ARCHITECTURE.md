# ZERECO Architecture: H2P Branch Resolution Acceleration

> 문서 목적: ZERECO를 **시뮬레이터 구현이 아니라 실제 OoO processor의 microarchitecture 관점**에서 설명한다.
>
> 범위: 별도 helper thread 없이 main thread의 H2P branch dependence chain만 가속한다.
>
> 상태: 일부 메커니즘은 실험으로 검증했지만, RF prefetch의 조기 launch·검증·recovery는 아직 제안 아키텍처이며 현재 Scarab 모델과 구분한다.
>
> Last updated: 2026-08-07

## 1. Architectural Thesis

TAGE-SC-L처럼 크고 복잡한 history-based predictor도 runtime load value에 direction이 의존하는 H2P(Hard-to-Predict) branch를 안정적으로 예측하기는 어렵다. ZERECO는 predictor의 storage와 energy를 계속 늘리는 대신 다음 경로를 택한다.

> 예측하기 어려운 H2P branch는 그대로 두되, branch outcome을 만드는 dependence chain을 가속해 misprediction을 더 일찍 resolve한다.

Misprediction을 일찍 발견하면 off-path fetch를 더 빨리 중단하고 redirect와 recovery를 앞당길 수 있다. 이를 위해 ZERECO는 두 가지 서로 다른 병목을 다룬다.

- **RF prefetch**: 예측 가능한 Target Load의 data를 physical register에 일찍 공급해 load-to-use latency를 줄인다.
- **Priority IQ(P-IQ)**: RF로 처리하기 어려운 residual H2P slice의 producer를 우선 issue해 scheduling 지연을 줄인다.

RF prefetch가 primary mechanism이고 P-IQ는 residual chain을 위한 secondary mechanism이다.

## 2. Experimental Insights That Drive the Architecture

### 2.1 Fetch-to-resolution에는 구조적으로 줄일 수 있는 구간이 있다

H2P misprediction/misfetch penalty에서 branch fetch부터 resolution까지의 구간이 무시할 수 없는 portion을 차지한다.

![Baseline H2P misprediction penalty breakdown](../../../simulations/zereco/zereco_260729_misp_penalty_breakdown/analysis/01_baseline_penalty_breakdown.svg)

**Architecture implication:** recovery latency만 최적화하는 것으로는 부족하다. Branch가 실제 operand를 받아 direction을 계산하는 시점을 앞당겨야 한다. 단, 위 그래프는 timeline의 구성 비율을 보여주는 관찰이며 그 구간이 IPC bottleneck이라는 인과 증거는 아니다.

### 2.2 긴 resolution의 중심은 operand dependency다

Fetch-to-resolution을 분해하면 branch가 frontend를 통과한 뒤 source operand가 준비되기를 기다리는 dependency 구간이 큰 비중을 차지한다. 이 구간에는 branch 자체의 대기뿐 아니라 backward-slice producer의 실행, load address generation, cache/memory service가 포함된다.

![Fetch-to-resolution stage breakdown](../../../simulations/zereco/zereco_260729_misp_penalty_breakdown/analysis/02_resolution_stage_breakdown_baseline_vs_predictors.svg)

**Architecture implication:** branch만 우선 issue하는 것으로는 충분하지 않다. Branch까지 값을 전달하는 older producer와 Target Load를 함께 가속해야 한다.

### 2.3 Target Load latency는 단순한 상관관계가 아니라 causal bottleneck이다

Address predictor가 정확히 cover한 Target Load의 latency를 줄였을 때 dependency wait과 fetch-to-resolution이 감소하고 IPC도 함께 증가했다.

![Predictor-gated Target Load effect](<../../../simulations/zereco/zereco_260729_misp_penalty_breakdown/analysis/(중요)03_predictor_gated_effect.svg>)

**Architecture implication:** Target Load latency는 긴 timeline과 함께 나타나는 현상만이 아니라, 실제로 줄였을 때 성능이 반응하는 병목이다. 따라서 ZERECO의 첫 번째 가속 지점은 H2P slice 안의 Target Load다.

### 2.4 높은 정확도와 불완전한 coverage가 함께 존재한다

PC-indexed online predictor는 prediction을 생성한 경우 높은 정확도를 보이지만 모든 Target Load를 cover하지는 않는다.

![Online Target Load predictor coverage and accuracy](<../../../simulations/zereco/zereco_260729_misp_penalty_breakdown/analysis/(중요) online_predictor_coverage_accuracy.svg>)

**Architecture implication:** 모든 Target Load에 공격적으로 RF prefetch를 적용하면 안 된다. High-confidence load만 RF로 처리하고, abstain·wrong-address·memory-order conflict가 예상되는 residual slice에는 정상 demand execution과 P-IQ를 사용해야 한다.

## 3. High-Level Architecture

ZERECO는 retired instruction stream에서 H2P dependence metadata를 만들고, 다음 dynamic occurrence가 frontend를 통과할 때 그 metadata를 main-thread instruction에 다시 부착한다.

```text
                          Retire / training path
        +-------------------------------------------------------+
        |                                                       |
        v                                                       |
 H2P Branch Table -> Retired Fill Buffer -> Backward Walk Engine
                                              |
                                              v
                              Block Metadata Cache
                         [chain / target-load / priority masks]
                                              |
                                              | frontend lookup
                                              v
Fetch -> Decode -> Rename -> Distributed RS / P-IQ -> Execute -> Retire
                         |                         |
                         | Target Load            | residual slice
                         v                         v
                Address Prefetch Table      Priority-first issue
                         |
                         v
            RFP Queue -> LSQ/store checks -> L1/cache hierarchy
                         |
                         v
                  Physical Register File
                         |
                         v
              dependent wakeup -> H2P branch resolution
```

핵심 하드웨어 구조는 다음과 같다.

| Structure | Architectural role | 주요 state |
|---|---|---|
| H2P Branch Table | 반복적으로 예측에 실패하는 branch 식별 | branch PC, confidence/mispred history |
| Retired Fill Buffer | 안전하게 dependence chain을 학습할 retired op 보관 | PC, src/dst register, load/store address metadata |
| Backward Walk Engine | H2P branch의 register·memory producer 추적 | source register vector, memory live-ins, walk state |
| Block Metadata Cache | frontend에서 chain op와 Target Load를 빠르게 식별 | block tag, chain mask, Target-Load mask, P-IQ mask |
| Address Prefetch Table | Target Load의 future virtual address 예측 | load-PC tag, last/base address, stride, confidence, utility, in-flight count |
| RFP Queue/State | rename된 load와 prefetch request 연결 | predicted address, destination PRF ID, load sequence, request state |
| RF Coverage Metadata | future slice의 P-IQ 적용 여부 결정 | branch/slice ID, covered history 또는 confidence |
| Distributed P-IQ entries | RF-uncovered slice의 producer를 우선 issue | entry class, ready bits, FU compatibility |

표의 크기와 associativity는 아직 최종 hardware budget으로 확정하지 않는다. 현재 연구의 우선 과제는 각 구조가 제공하는 성능 headroom과 필요한 correctness 동작을 분리해 확인하는 것이다.

## 4. H2P Chain Identification

### 4.1 H2P branch detection

Retire 시점의 branch PC별 prediction history를 이용해 반복적으로 misprediction을 일으키는 branch를 H2P로 분류한다. 이 metadata는 direction을 대신 예측하는 것이 아니라, 다음 occurrence에서 acceleration을 적용할 대상을 선택하는 데만 사용한다.

### 4.2 Retire-time backward walk

Retired Fill Buffer가 충분히 채워지면 youngest-to-oldest 방향으로 walk한다.

1. H2P branch의 source register와 필요한 memory address를 Source List에 넣는다.
2. Source List의 register 또는 memory location을 정의하는 older op를 slice에 포함한다.
3. 해당 op의 destination을 제거하고 source를 추가한다.
4. 여러 H2P branch가 겹치는 경우 동일 op를 공유 slice member로 표시한다.
5. 완성된 slice를 basic-block 단위 bit mask로 나눠 Block Metadata Cache에 저장한다.

이 방식은 TEA의 Fill Buffer, backward dataflow walk와 Block Cache를 identification hardware로 차용한다. 그러나 ZERECO는 chain을 별도 thread에서 재실행하지 않는다. Main-thread op 자체에 metadata를 붙여 RF prefetch와 scheduling policy만 바꾼다.

### 4.3 Metadata masks

Block Metadata Cache entry는 최소한 다음 mask를 제공한다.

- `chain mask`: H2P branch backward slice 전체
- `Target-Load mask`: slice 안에서 load인 op
- `priority-candidate mask`: P-IQ가 가속할 수 있는 범위
- `effective-priority mask`: RF coverage feedback을 반영한 최종 범위

여러 slice가 같은 block/op를 공유할 수 있으므로 어느 하나의 uncovered slice가 그 op를 필요로 하면 priority를 유지해야 한다. 한 slice가 RF-covered라는 이유만으로 다른 uncovered slice에 필요한 shared producer를 지우면 안 된다.

## 5. Target-Load Register-File Prefetching

### 5.1 왜 cache prefetch가 아니라 RF prefetch인가

Cache prefetch는 miss latency를 줄일 수 있지만 demand load는 여전히 다음 단계를 거쳐야 한다.

```text
producer scheduling -> address generation -> load scheduling
-> cache access -> destination register write -> dependent wakeup
```

ZERECO의 목표는 cache locality 자체가 아니라 H2P branch operand의 ready 시점을 앞당기는 것이다. RF prefetch는 predicted address의 data를 load의 destination physical register로 먼저 전달함으로써, 정확한 경우 demand load의 cache service와 load-to-use latency를 직접 숨길 수 있다.

### 5.2 Prediction and launch timing

제안하는 하드웨어 동작은 RFP-style pipeline을 따른다.

1. Frontend의 Target-Load mask가 해당 load를 RFP candidate로 표시한다.
2. Load PC로 Address Prefetch Table을 조회한다. 조회는 rename보다 앞서 시작할 수 있다.
3. Rename에서 load의 destination physical register ID가 정해지면 prefetch packet을 완성한다.
4. 같은 static load의 여러 dynamic instance가 동시에 존재할 수 있으므로 in-flight count를 이용해 `base + stride × distance` 형태로 다음 instance address를 만든다.
5. Packet을 RFP queue와 LSQ로 보내 older store, memory disambiguation과 ordering constraint를 확인한다.
6. 통과한 request는 demand load보다 낮은 우선순위로 L1/cache port를 사용한다.
7. Data가 도착하면 destination PRF에 쓰고 dependent wakeup 시점을 앞당긴다.
8. Predictor와 coverage metadata는 retired instance만으로 학습하고, squash된 load는 in-flight state에서 제거한다.

Prediction은 반드시 현재 dynamic load의 실제 address를 학습하기 전에 이루어져야 한다. 동일 load가 cache access를 재시도하더라도 address predictor를 다시 통과시켜 더 학습된 prediction을 얻는 동작은 허용하지 않는다.

### 5.3 Correctness of RF-prefetched data

RF prefetch는 wrong data를 architecturally commit하지 않도록 실제 load와 연결되어야 한다.

| 상황 | Required hardware action |
|---|---|
| Predicted address = actual address, ordering valid | Prefetched value를 load result로 인정하고 demand cache lookup을 생략하거나 병합 |
| Address mismatch | Prefetched value를 무효화하고 normal demand access 수행 |
| Unknown/overlapping older store | LSQ·memory disambiguation 결과까지 wait하거나 store data forwarding |
| Memory-dependence prediction failure | 해당 load와 speculative dependents를 replay; 필요한 경우 기존 memory-order recovery 사용 |
| Coherence invalidation before validation | Prefetch를 load proxy로 추적해 invalidate/replay |
| DTLB miss or request congestion | Prefetch를 drop하고 demand load가 정상 실행 |
| Branch squash | RFP queue entry, in-flight count와 speculative wakeup state를 함께 정리 |

Address prediction이 틀렸을 때 핵심 비용은 extra L1/LSQ bandwidth와 speculative dependent의 cancel/reissue다. 정확한 address에 대해 LSQ/store/coherence 검사를 정상 load와 동일하게 수행하면 별도의 second L1 validation access 없이 data correctness를 보장할 수 있다.

## 6. RF Filtering and Priority IQ

### 6.1 Slice-level online filtering

현재 dynamic occurrence의 RF 성공 여부는 이미 frontend를 지난 older producer에게 소급할 수 없다. 실제 hardware에서 filtering은 retired outcome을 이용해 **다음 occurrence**를 제어해야 한다.

- Slice의 Target Load가 안정적으로 RF-covered이면 다음 occurrence의 P-IQ marking을 억제한다.
- 하나라도 RF-uncovered이면 branch까지의 backward slice를 priority 대상으로 유지한다.
- Target Load가 없는 slice는 RF로 처리할 수 없으므로 priority를 유지한다.
- Covered와 uncovered Target Load가 섞이면 covered load에는 RFP를 적용하고, 전체 slice에는 P-IQ를 유지한다.

Filtering 단위는 load 하나가 아니라 H2P branch slice다. Uncovered Target Load가 하나라도 branch resolution을 지연시킬 수 있기 때문이다.

Hardware에서 `RF-covered`는 address prediction이 맞았다는 뜻만으로 정의하면 부족하다. Address와 memory ordering이 모두 valid하고, value가 normal demand result보다 일찍 도착해 실제 dependent wakeup을 앞당긴 경우까지 포함해야 한다. 그렇지 않으면 정확하지만 늦은 prefetch를 근거로 P-IQ를 잘못 억제할 수 있다.

### 6.2 P-IQ organization

ZERECO는 distributed IQ/RS의 각 bank에 소수의 Priority entry를 예약한다. Priority entry는 select logic에서 고정적으로 Normal entry보다 앞선 위치 우선순위를 갖는다. 이렇게 하면 매 cycle 전체 ready request를 priority bit로 정렬하는 큰 CAM/MUX network를 추가하지 않고도 PUBS-style priority를 구현할 수 있다.

Dispatch 정책은 다음과 같다.

```text
priority candidate + free compatible P-IQ entry
    -> P-IQ entry, Priority-first scheduling

priority candidate + P-IQ full + free compatible Normal entry
    -> Normal entry fallback, 이후 Normal scheduling

no compatible entry
    -> in-order dispatch stall
```

Non-stall fallback op는 slice member라는 history는 유지할 수 있지만, Normal entry에 들어간 뒤에는 scheduling priority를 잃는다. 이 정책은 P-IQ 부족이 frontend/dispatch 전체를 막는 비용을 제한한다.

### 6.3 P-IQ의 효과와 한계

Random-queue scheduling에서는 P-IQ가 H2P producer의 issue order를 바꿔 dependency wait과 IPC를 개선할 수 있다. RF prefetch가 가장 큰 효과를 제공하고 filtered P-IQ는 추가 개선을 제공한다.

![P-IQ and RF-prefetch effect under random-queue scheduling](<../../../simulations/zereco_260805_random_iq_comparison/analysis/(중요) 01_random_iq_priority_effect.svg>)

**Architecture implication:** P-IQ는 독립적인 주 메커니즘이 아니라 RF-uncovered chain의 scheduling을 보완하는 구조로 두는 것이 맞다. 또한 age matrix나 oldest-first가 이미 old critical producer를 우대하는 processor에서는 P-IQ의 추가 headroom이 더 작을 수 있다.

Strict-stall은 Priority entry가 부족할 때 전체 dispatch를 막는다. 현재 workload에서는 non-stall fallback이 이 capacity loss를 줄이고 더 안정적인 성능을 보였다.

![Stall versus non-stall P-IQ policy](<../../../simulations/zereco_260805_random_iq_comparison/analysis/(중요) 03_nonstall_vs_stall.svg>)

**Architecture implication:** ZERECO의 기본 정책은 non-stall이다. 이는 PUBS의 기본 strict-stall 정책을 그대로 복제한 것이 아니라, 높은 H2P population을 가진 target workload에 맞춘 선택이다.

P-IQ를 크게 만들수록 priority admission은 쉬워지지만 Normal capacity와 dispatch pressure가 악화될 수 있다. 현재 random-queue sweep에서는 15%가 IPC gain과 dispatch pressure의 balance point다.

![P-IQ ratio selection](../../../simulations/zereco_260805_random_iq_comparison/analysis/04_piq_ratio_selection.svg)

**Architecture implication:** 15%는 현재 workload와 backend width에 대한 설계점이며 보편적인 상수가 아니다. 최종 설계에서는 RS bank별 FU compatibility, H2P population과 memory intensity에 따라 ratio 또는 mode switching을 검토해야 한다.

## 7. End-to-End Operation

한 H2P branch occurrence에 대한 동작은 다음과 같다.

1. **Metadata lookup:** frontend가 block PC로 Block Metadata Cache를 조회해 chain, Target-Load와 priority bit를 op에 붙인다.
2. **RFP eligibility:** Target Load가 decode/rename에 도달하면 Address Prefetch Table의 confidence와 utility로 request 생성 여부를 결정한다.
3. **Early launch:** rename에서 destination PRF가 배정되는 즉시 predicted address request를 LSQ/L1로 보낸다.
4. **Priority dispatch:** residual slice op는 compatible P-IQ entry로, full이면 Normal entry로 fallback한다.
5. **Issue:** local select logic은 ready Priority entry를 Normal entry보다 먼저 선택한다.
6. **RF completion:** RFP data가 도착하면 load state와 PRF를 갱신하고 적절한 시점에 dependent를 wakeup한다.
7. **Validation:** demand AGU가 만든 actual address와 memory-order 결과로 RFP data 사용 가능 여부를 확정한다.
8. **Resolution:** branch source가 준비되면 branch를 issue하고 actual direction/target을 계산한다.
9. **Recovery:** misprediction/misfetch이면 off-path fetch를 중단하고 correct path로 redirect한다.
10. **Training:** retire된 load의 address와 RF outcome으로 predictor 및 slice coverage를 갱신한다.

## 8. Hardware Correctness and Recovery Requirements

Architecture는 다음 invariant를 보장해야 한다.

- **Retire-only training:** wrong-path load가 address predictor나 RF coverage state를 오염시키지 않는다.
- **One prediction per dynamic load:** cache/LSQ retry가 predictor 재시도나 duplicate training을 만들지 않는다.
- **PRF lifetime:** RFP packet이 살아 있는 동안 destination PRF mapping이 유효해야 하며 squash 시 reference를 해제한다.
- **Address validation:** actual AGU address와 predicted virtual/physical address의 일치를 load 완료 전에 확인한다.
- **Memory ordering:** older store, forwarding, fence와 coherence event를 demand load와 동일한 순서 규칙으로 처리한다.
- **Speculative wakeup recovery:** wrong address, miss-latency speculation 또는 ordering failure 시 이미 wakeup된 dependents를 cancel/reissue한다.
- **Block-mask consistency:** shared op는 자신을 필요로 하는 모든 live slice가 covered된 경우에만 priority에서 제외한다.
- **Distributed admission:** op가 실행 가능한 FU와 연결된 P-IQ/Normal entry에만 dispatch된다.

RF prefetch가 실제 값을 조기에 노출하는 만큼 address correctness, value freshness와 memory ordering을 하나의 `prediction accuracy`로 뭉뚱그리면 안 된다. 세 조건을 독립적으로 검증하고 각각의 실패 비용을 계측해야 한다.

## 9. Timing, Area and Energy Considerations

### 9.1 Frontend metadata lookup

Block Metadata Cache는 block PC로 조회하며 instruction bit mask를 반환한다. I-cache/uop-cache access와 병렬로 수행하고 miss이면 normal execution을 선택하므로 frontend의 correctness-critical latency를 늘리지 않아야 한다.

### 9.2 Address predictor

Prefetch Table lookup은 load PC가 보이는 시점에 일찍 시작하되 destination PRF ID는 rename 뒤 packet에 결합한다. Confidence가 낮거나 DTLB/LSQ resource가 부족하면 request를 drop할 수 있어야 한다.

### 9.3 RFP bandwidth

RFP request는 demand load보다 낮은 cache-port priority를 가져야 한다. Correct request는 demand access를 대체하거나 병합해 추가 L1 access를 만들지 않는 것이 목표이며, wrong request만 extra bandwidth를 사용한다. Queue occupancy, port contention, drop rate와 useless writeback을 hardware cost에 포함해야 한다.

### 9.4 P-IQ select logic

P-IQ entry를 물리적으로 고정된 high-priority position으로 두면 full-queue priority sorting을 피할 수 있다. Distributed backend에서는 각 RS bank에 Priority/Normal free list와 occupancy counter가 필요하다. 예약 entry가 비어 있는 동안 Normal op가 사용할 수 없다는 capacity cost는 non-stall policy만으로 없어지지 않으므로 ratio 선택이 중요하다.

## 10. Proposed Hardware versus Current Scarab Model

이 구분은 논문의 architecture claim과 현재 실험 결과의 검증 범위를 혼동하지 않기 위해 중요하다.

| Component | Proposed hardware behavior | Current Scarab evaluation model |
|---|---|---|
| H2P chain learning | retire-time Fill Buffer와 backward walk, hardware-sized Block Metadata Cache | 동일한 개념을 모델링하지만 storage/timing cost는 최종 확정 전 |
| Target identification | frontend block-mask lookup | main-thread on-path chain/Target-Load bit 부착 |
| Address prediction | PC-indexed table lookup, retire-time training, in-flight-aware future address, rename-time packet launch | load가 dcache stage에 도달한 뒤 `predict -> current actual address 비교 -> update` 수행 |
| RF service | LSQ/store checks와 cache request를 거쳐 PRF writeback | exact address이고 store conflict가 없으면 짧은 oracle latency 적용 |
| Wrong prediction | bandwidth 소모, demand fallback, dependent cancel/reissue | 가속하지 않고 정상 path 사용; wrong-request/recovery 비용 미모델링 |
| RF timeliness | queue, ports, cache level과 PRF writeback timing에 의해 결정 | configurable short hit latency의 upper bound |
| Online filtering | correct·order-valid·timely RF outcome으로 future slice metadata 갱신 | retired exact-address/conflict 결과를 이용하는 single-pass filtering |
| P-IQ | distributed RS의 물리적 Priority entries, fixed select precedence와 non-stall fallback | finite 논리 partition, priority-bit comparator와 fallback을 모델링 |
| Scheduler baseline | 실제 core의 queue/select organization에 의존 | oldest-first와 deterministic random-physical을 각각 비교 |

따라서 현재 결과가 직접 지지하는 것은 다음이다.

- H2P fetch-to-resolution 안에 줄일 수 있는 dependency 구간이 존재한다.
- Predictor가 정확히 cover한 Target Load의 latency를 줄이면 resolution과 IPC가 개선된다.
- Online RF filtering과 finite non-stall P-IQ의 scheduling trade-off가 존재한다.

아직 직접 검증하지 않은 것은 다음이다.

- Rename-time RFP가 실제 queue/port contention 아래에서도 충분히 timely한가.
- Wrong-address, memory-order failure와 dependent replay 비용을 포함해도 순효과가 유지되는가.
- Block Metadata Cache, Prefetch Table, RFP queue와 P-IQ의 최종 area/energy budget이 타당한가.

## 11. Research Contribution Boundary

ZERECO의 핵심 contribution은 새로운 branch direction predictor가 아니다. 또한 TEA-style helper execution이나 full branch runahead engine도 아니다.

> Retire-time H2P slice identification을 이용해 main-thread Target Load를 선택하고, high-confidence load는 RF prefetch로, residual slice는 filtered P-IQ로 가속하는 branch-resolution architecture다.

논문에서는 RF predictor의 성공 case만으로 전체 architecture를 완성된 것으로 표현하면 안 된다. 현재 causal insight와 scheduling 결과를 architectural motivation으로 사용하고, early-launch RFP의 correctness와 cost는 별도 구현·평가 항목으로 제시해야 한다.

## 12. References

- TEA: `/home/lee/scarab/reference/[2024, MICRO] Timely_Efficient_and_Accurate_Branch_Precomputation.pdf`
- RFP: `/home/lee/scarab/reference/[2022, ISCA] Reg File prefetching.pdf`
- PUBS: `/home/lee/scarab/reference/[2018, MICRO] PUBS.pdf`
- Branch Runahead: `/home/lee/scarab/reference/[2021, MICRO] Branch Runahead_Pruett,Y.Patt.pdf`

실험의 configuration, 통계 정의, SimPoint 집계와 재현 절차는 [`zereco_README.md`](zereco_README.md)에 남기고, 이 문서는 architecture와 hardware contract만 유지한다.
