# ZERECO: Hard-to-Predict Branch Resolution Acceleration

## 1. Introduction

현대의 branch predictor는 긴 global/local history, 여러 prediction component, 큰 storage budget을 활용한다. 그럼에도 최근에 load된 runtime value에 outcome이 의존하는 branch는 여전히 예측하기 어렵다. 이러한 Hard-to-Predict(H2P) branch는 history와 table을 추가해도 branch outcome을 결정하는 실제 data가 predictor에 보이지 않기 때문에, predictor complexity 증가에 비해 accuracy 개선이 제한적이다.

ZERECO는 이 문제를 branch prediction accuracy가 아니라 **misprediction detection latency**의 관점에서 접근한다. H2P branch의 prediction failure 자체를 완전히 없애는 대신, branch가 pipeline에 들어온 뒤 actual outcome을 계산하는 데까지 소요되는 시간을 줄여 misprediction을 더 일찍 발견한다.

> **ZERECO는 committed execution으로부터 H2P branch의 backward slice를 학습하고, 그 안의 predictable Target Load는 Register-File Prefetch로 가속하며, RF로 처리하지 못한 residual slice에는 issue priority를 부여해 branch를 조기에 resolve한다.**

Branch가 일찍 resolve되면 wrong-path instruction supply를 더 빨리 중단하고 correct path로의 redirect를 앞당길 수 있다. ZERECO는 새로운 branch-direction predictor나 duplicated helper thread 없이, H2P branch outcome에 기여하는 main-thread instruction만 선택적으로 가속한다.

ZERECO의 핵심 contribution은 다음과 같다.

1. H2P branch의 긴 resolution에서 branch execution 자체보다 operand dependency resolution이 주요한 reducible component임을 밝힌다.
2. H2P backward slice의 load latency가 이 delay의 causal source이며, lightweight PC-local history로 예측 가능한 subset이 존재함을 보인다.
3. 선택된 Target Load가 가져올 data를 해당 load의 destination physical register, 즉 dependent instruction이 source operand로 참조하는 올바른 PRF entry에 미리 기록하는 targeted RF prefetching을 제안한다.
4. RF prefetch가 처리하지 못한 residual slice만 finite non-stalling Priority IQ(P-IQ)로 가속하는 hybrid microarchitecture를 제안한다.

## 2. Motivation and Key Insights

### 2.1 왜 prediction accuracy가 아니라 penalty를 줄이는가?

Branch misprediction으로 인한 성능 손실은 misprediction의 발생 빈도와 event당 penalty에 의해 함께 결정된다. 대부분의 prior work는 branch prediction accuracy를 높여 첫 번째 항을 줄인다. 이 접근은 program history와 correlation이 있는 branch에는 효과적이지만, 아직 load되지 않은 data에 direction이나 target이 의존하는 branch에는 근본적인 한계가 있다.

H2P branch는 다음과 같은 보완적인 질문을 요구한다.

> Prediction failure를 안정적으로 피할 수 없다면, 그 failure를 더 일찍 발견할 수 있는가?

그 답은 branch의 실제 operand가 언제 준비되는지에 달려 있다. Branch는 dynamic backward slice의 모든 producer가 완료되어야 resolve될 수 있다. 이 slice에는 address-generation instruction, load, 그리고 load value를 branch condition으로 전달하는 arithmetic instruction이 포함된다. 따라서 ZERECO는 branch resolution을 dataflow critical-path 문제로 다룬다.

### 2.2 Fetch-to-resolution은 줄일 수 있는 구간이다

Mispredicted branch의 timeline은 크게 다음과 같이 나눌 수 있다.

```text
branch fetch
    -> branch resolution and misprediction detection
    -> recovery and redirect
    -> first correct-path fetch
```

첫 구간은 PUBS가 정의한 misspeculation interval에 해당한다. Branch가 frontend를 통과하고, dependence chain과 issue opportunity를 기다린 뒤, 실행을 마쳐 actual outcome을 드러내는 시간이다. 두 번째 구간은 processor state를 복구하고 fetch를 redirect하여 첫번째 on-path instruction이 fetch 될 때까지의 시간이다. ZERECO는 branch operand를 일찍 준비함으로써 줄일 수 있는 첫 구간을 타깃으로 한다.

Figure_1

### 2.3 Dependency wait이 reducible interval을 지배한다

Fetch-to-resolution은 frontend traversal, dependency wait, ready-to-issue scheduling wait, branch execution으로 나눌 수 있다. Target H2P branch에서는 source operand가 준비되기를 기다리는 dependency 구간이 가장 크다. 이 구간에는 older producer의 실행, Target Load의 address generation과 cache/memory service, 그리고 loaded value가 branch까지 전달되는 시간이 포함된다.

이 관찰은 필요한 mechanism의 범위를 결정한다. Branch가 ready된 뒤 branch 자체에만 priority를 부여하는 것은 너무 늦다. Branch를 ready 상태로 만드는 older producer 전체를 가속해야 한다.

Figure_2

### 2.4 Target Load latency는 causal bottleneck이다

**Target Load**는 H2P branch의 dynamic on-path backward slice에 속한 load다. Target Load의 service latency는 branch로 이어지는 모든 younger consumer를 지연시킬 수 있다. 짧고 반복적인 dependence chain을 직렬화한다면 L1 hit latency도 critical할 수 있으며, lower-level cache 또는 memory access는 그 영향을 더 키운다.

중요한 근거는 long-latency load와 long branch resolution이 함께 나타난다는 correlation만이 아니다. 올바르게 선택된 Target Load의 latency를 줄였을 때 dependency wait과 fetch-to-resolution이 감소하고 IPC도 함께 증가한다. 이 controlled intervention은 Target Load latency가 실제 branch misprediction penalty의 bottleneck 지점이라는 causal evidence다.

Figure_3

### 2.5 Target Load에는 predictable stream과 irregular tail이 공존한다

Target-Load access는 static load PC 전체에 균등하게 분산되지 않는다. 일부 load PC가 dynamic access의 큰 비중을 차지하며, 이들 가운데 다수는 stable stride 또는 recurring delta를 보인다. 반면 irregular access도 남기 때문에 predictor는 모든 load에 무리하게 request를 생성하지 않고 abstain할 수 있어야 한다.

높은 conditional accuracy와 불완전한 coverage의 공존은 selective hybrid design으로 이어진다.

- High-confidence Target Load에는 RF prefetch를 적용한다.
- Abstain, wrong address, late arrival, memory-order constraint가 발생한 load는 normal demand path를 사용한다.
- 이러한 residual load를 포함한 branch slice는 P-IQ 대상으로 유지한다.

Figure_4

### 2.6 RF prefetch는 primary mechanism이고 P-IQ는 complementary mechanism이다

Priority scheduling은 load가 issue된 뒤 발생하는 cache/memory service latency를 없앨 수 없다. 반대로 RF prefetch는 address stream이 불규칙하거나 non-load producer가 병목인 slice를 가속하지 못한다. ZERECO는 서로 다른 residual bottleneck에 서로 다른 mechanism을 적용한다.

```text
predictable Target Load
    -> early RF prefetch

RF-uncovered H2P slice
    -> priority admission and issue

both paths
    -> earlier branch-operand readiness
    -> earlier branch resolution
```

## 3. Background and Design Gap

### 3.1 H2P branch와 backward slice

H2P branch는 한 번 low-confidence prediction이 발생한 branch를 의미하지 않는다. Data dependence 또는 복잡한 control behavior로 인해 prediction failure가 반복되는 static branch를 뜻한다.

H2P branch의 dynamic backward slice는 branch와, branch operand를 직접 또는 간접적으로 생성하는 모든 older instruction으로 구성된다. 이 slice는 basic-block 경계를 넘을 수 있으며 register dependence와 memory dependence를 모두 포함할 수 있다. Relevant path는 runtime control flow와 address에 따라 달라지므로, ZERECO는 compiler가 만든 고정 slice에만 의존하지 않고 committed dynamic execution으로부터 slice를 학습한다.

### 3.2 일반적인 cache prefetch로는 충분하지 않은 이유

Cache prefetcher는 data를 core 가까이 이동시키지만 demand load는 여전히 operand readiness, address generation, issue, cache access, destination register write, dependent wakeup을 거쳐야 한다. ZERECO의 목표는 cache locality 자체가 아니라 **branch operand의 register-ready 시점**을 앞당기는 것이다. 따라서 Target Load가 가져올 data를 해당 load의 destination physical register, 즉 dependent instruction이 source operand로 참조할 PRF entry에 미리 기록하는 Register-File Prefetch가 더 직접적인 mechanism이다.

### 3.3 일반적인 issue priority로는 충분하지 않은 이유

Conventional issue selection은 흔히 age 또는 fixed queue position을 우선순위로 사용한다. 이러한 정책은 단순하고 효과적이지만, 특정 ready instruction이 unresolved H2P branch의 dependence chain에 속한다는 사실은 고려하지 않는다. 연속된 producer 각각이 issue conflict로 한 cycle씩 늦어지면 그 delay는 chain을 따라 누적된다. Residual slice 전체를 표시해야 여러 dependent operation에 걸쳐 urgency를 유지할 수 있다.

다만 age-based scheduler는 이미 older producer를 우대할 수 있다. 따라서 새로운 priority class의 incremental benefit은 baseline scheduling policy와 priority-marked population에 따라 달라진다. ZERECO에서 P-IQ는 RF prefetch를 대체하는 구조가 아니라 targeted secondary mechanism이다.

### 3.4 Prior work와의 관계

| Prior work | 차용하는 기반 | ZERECO가 해결하는 남은 gap |
|---|---|---|
| **TEA** | Runtime H2P identification, committed-instruction buffer, backward dataflow walk, block-granular chain metadata | TEA는 별도의 precomputation thread를 실행한다. ZERECO는 identification metadata만 활용하고 original main-thread instruction을 가속한다. |
| **RFP** | PC-indexed address prediction, in-flight-aware future address generation, post-rename RF-prefetch launch, memory-order integration | RFP는 general load를 대상으로 한다. ZERECO는 H2P branch resolution을 지연시키는 Target Load만 선택하고 residual-slice scheduling과 결합한다. |
| **PUBS** | Unconfident branch slice instruction에 대한 highest issue priority | Scheduling은 load service latency를 숨기지 못하며 strict Priority/Normal partition은 dispatch를 막을 수 있다. ZERECO는 predictable load latency를 먼저 줄이고 residual slice에 non-stalling priority admission을 적용한다. |
| **Branch Runahead** | History predictor가 처리하기 어려운 branch의 dynamic dependence-chain construction | Branch Runahead는 별도 engine에서 chain을 실행해 branch outcome을 precompute한다. ZERECO는 outcome engine 없이 main-thread의 post-fetch resolution을 줄인다. |

ZERECO가 채우는 design gap은 또 다른 branch precomputation mechanism이 아니다. Branch-resolution critical load의 data delivery와 남은 dependence chain의 selective scheduling을 결합하는 main-thread architecture다.

## 4. ZERECO Design Overview

### 4.1 Design goals

ZERECO는 다음 다섯 가지 원칙을 따른다.

1. **Persistent H2P branch만 타깃으로 한다.** Easy branch가 acceleration resource를 점유하지 않도록 한다.
2. **Committed execution으로 학습한다.** Wrong-path instruction이 persistent chain/address metadata를 오염시키지 않아야 한다.
3. **Computation을 복제하지 않고 dataflow를 가속한다.** Original main-thread slice가 유일한 architectural execution이다.
4. **불확실할 때는 abstain한다.** Low-confidence 또는 constrained RFP request는 normal demand execution으로 돌아간다.
5. **Backend interference를 제한한다.** Finite P-IQ와 non-stalling fallback으로 priority traffic의 resource 독점을 막는다.

### 4.2 High-level dataflow

```text
                         committed-training path
          +---------------------------------------------------+
          |                                                   |
          v                                                   |
  H2P Branch Table -> Retired Window -> Backward-Walk Engine
                                              |
                                              v
                                  Block Metadata Cache
                              [chain and priority masks]
                                              |
                                      next-occurrence lookup
                                              v
Fetch -> Decode -> Rename -> Distributed IQ / RS -> Execute -> Commit
                         |                    |
                         | Target Load        | RF-uncovered slice
                         v                    v
                Address Predictor       Priority issue class
                         |
                         v
          RFP Queue -> LSQ / L1 -> Physical Register File
                                      |
                                      v
                             dependent wakeup
                                      |
                                      v
                            H2P branch resolution
```

ZERECO는 세 단계로 동작한다.

1. Persistent H2P branch를 식별하고 committed backward slice를 학습한다.
2. High-confidence Target Load가 가져올 value를 해당 load의 destination physical register, 즉 dependent instruction의 source physical-register tag가 가리키는 PRF entry에 prefetch한다.
3. RF prefetch가 완전히 cover하지 못한 slice에 priority scheduling을 적용한다.

## 5. H2P Dependence-Chain Identification

### 5.1 H2P Branch Table

H2P Branch Table은 branch PC별로 반복되는 prediction failure를 추적한다. Saturating history 또는 confidence state를 이용해 persistent H2P behavior와 일시적인 misprediction을 구분한다. 이 table은 branch direction이나 target을 예측하지 않으며, chain learning과 acceleration을 적용할 branch만 선택한다.

Classifier는 architecturally resolved branch로 갱신한다. Periodic decay는 obsolete behavior가 table을 영구 점유하는 것을 막고 program phase 변화에 적응하게 한다.

### 5.2 Retired Instruction Window

Retired Instruction Window는 H2P slice를 복원하는 데 필요한 최근 committed dataflow를 저장한다. 각 entry는 instruction identity, register source/destination, instruction type, load/store의 memory-dependence metadata를 포함한다. Committed instruction만 보관하므로 backward walk를 위해 speculative checkpoint를 둘 필요가 없다.

### 5.3 Backward dataflow walk

H2P branch가 retire하면 Backward-Walk Engine은 branch source register에서 시작해 committed instruction을 youngest-to-oldest 방향으로 탐색한다.

1. Live source를 정의하는 instruction을 slice에 포함한다.
2. 해당 destination을 live set에서 제거하고 그 instruction의 source를 추가한다.
3. Memory producer가 필요하면 retired memory-dependence information으로 load와 store를 연결한다.
4. 모든 live-in을 찾거나, retained window가 끝나거나, bounded work limit에 도달하면 walk를 종료한다.
5. 완성된 branch slice에 포함된 모든 load를 Target-Load candidate로 분류한다.

각 H2P branch의 slice는 독립적으로 만든다. 이렇게 해야 여러 H2P branch가 producer를 공유하더라도 ownership을 보존하고 이후 slice-level RF filtering을 정확하게 적용할 수 있다.

### 5.4 Block Metadata Cache

학습한 slice를 instruction-block fragment로 나누어 Block Metadata Cache에 저장한다. Frontend instruction delivery와 병렬로 block metadata를 조회해 다음 dynamic occurrence의 main-thread instruction에 acceleration 정보를 부착한다. 각 fragment는 최소한 다음 정보를 제공한다.

- **chain mask**: H2P backward-slice instruction 식별
- **priority-candidate mask**: residual scheduling acceleration이 가능한 instruction 식별
- **effective-priority mask**: RF-coverage feedback이 반영된 최종 priority 대상

Target Load는 chain mask와 decoded load type을 결합해 식별할 수 있으며, lookup을 단순화하기 위해 별도의 load mask를 저장할 수도 있다. 여러 slice가 같은 block과 instruction을 공유하면 candidate mask를 union한다. Shared producer는 자신을 필요로 하는 모든 live slice가 RF-covered일 때만 priority를 잃는다.

Program phase 변화가 stale 또는 permanently over-marked chain을 남기지 않도록 metadata replacement, aging, versioning이 필요하다.

## 6. Target-Load Register-File Prefetching

### 6.1 Address prediction

Target-Load Address Predictor는 load PC로 indexing하며 recent base address, recurring stride/delta, confidence, utility, outstanding dynamic-instance count를 저장한다. In-flight count는 여러 dynamic instance가 동시에 OoO window에 존재할 때 이들을 구분하며, 다음과 같은 address prediction을 가능하게 한다.

```text
predicted address = retired base + recurring delta × dynamic distance
```

Prediction은 반드시 현재 dynamic load의 actual address가 predictor state를 갱신하기 전에 이루어진다. Cache 또는 memory request가 retry되더라도 한 dynamic load는 한 번만 prediction을 받는다. Low-confidence stream에는 abstain하며 persistent predictor state는 committed instance만 학습한다.

### 6.2 Early launch와 PRF binding

Load PC가 보이면 rename 이전부터 predictor lookup을 시작할 수 있다. Rename이 load의 destination physical register를 할당하면 predicted virtual address, destination PRF identifier, dynamic load identity, memory-order state를 포함한 RFP packet을 완성한다. 이 destination PRF entry는 이후 dependent instruction의 source physical-register tag가 참조하는 동일한 entry다. Packet은 작은 FIFO RFP Queue에 들어가며 translation, LSQ, L1 resource를 demand load보다 낮은 우선순위로 사용한다.

Rename 이후 launch는 두 가지 중요한 속성을 제공한다.

- Load destination과 dependent source가 공유하는 올바른 PRF entry에 prefetched value를 기록할 수 있다.
- Existing load-store ordering과 memory-disambiguation logic을 재사용할 수 있다.

Demand load보다 먼저 도착하기 어렵거나, 필요한 resource를 얻지 못하거나, 선택한 cache level에서 miss한 RFP request는 correctness 영향 없이 drop할 수 있다.

### 6.3 Memory ordering과 data correctness

Correct address prediction만으로 RF-prefetched value의 correctness가 보장되지는 않는다. 해당 value가 architectural load가 관찰해야 하는 최신 값이어야 한다. 따라서 RFP request는 older store, forwarding, fence, translation, coherence에 대해 demand load와 동일한 ordering constraint를 따른다.

Address가 맞으면 RFP는 coherent memory hierarchy에서 올바른 value를 얻고 demand access를 대체하거나 merge할 수 있다. Value validation만을 위한 두 번째 cache access는 필요하지 않다. Original load는 여전히 architectural address를 계산하고 predicted address와의 일치를 확인한다.

Address가 틀리면 prefetched state를 폐기하고 original load가 normal demand path로 완료된다. Dependent instruction을 speculative wakeup했다면 selective cancel과 replay가 필요하다. Squash 시에는 RFP packet, in-flight predictor contribution, PRF reference를 함께 제거한다.

### 6.4 Timeliness와 RF coverage

ZERECO에서 load가 **RF-covered**라는 것은 단순히 address가 맞았다는 뜻이 아니다. 다음 조건을 모두 만족해야 한다.

1. Address prediction이 생성되었다.
2. Predicted address와 architectural address가 일치한다.
3. Memory-order와 coherence check가 value를 검증했다.
4. Value가 dependent wakeup을 실제로 앞당길 만큼 일찍 PRF에 도착했다.

이 정의는 accurate하지만 late한 prefetch 때문에 유용한 P-IQ acceleration을 억제하는 것을 막는다. Coverage와 utility는 commit에서 기록하며 future occurrence에만 영향을 준다.

## 7. RF-Filtered Priority Scheduling

### 7.1 Slice-level filtering

RF filtering은 individual load가 아니라 H2P branch slice 단위로 수행한다. Slice 안의 Target Load 하나만 uncovered여도 branch resolution은 여전히 지연될 수 있기 때문이다.

- 모든 Target Load가 반복적으로 RF-covered이면 다음 occurrence에서 해당 slice의 P-IQ marking을 억제한다.
- Target Load가 하나라도 uncovered이면 full priority-candidate slice를 유지한다.
- Target Load가 없는 slice는 RF prefetch로 처리할 수 없으므로 priority를 유지한다.
- Covered와 uncovered Target Load가 섞이면 covered load에는 RFP를 적용하면서 전체 slice에는 P-IQ priority를 유지한다.

Filtering은 retroactive하지 않다. 현재 dynamic Target Load의 결과로 이미 backend에 들어간 older producer의 priority를 바꿀 수 없으며, 결과는 다음 occurrence를 학습한다.

### 7.2 Distributed finite P-IQ

각 distributed IQ 또는 reservation-station bank는 bounded subset의 entry를 Priority class에 예약하고 나머지를 Normal class에 둔다. Admission은 functional-unit compatibility를 보존한다. Local selector는 ready Priority entry를 ready Normal entry보다 먼저 선택하며, 같은 class 안에서는 processor의 baseline arbitration policy를 사용한다.

실제 구현에서는 entire queue를 priority bit로 매 cycle 정렬하기보다 Priority entry를 encoder의 fixed high-priority position에 배치할 수 있다. 이 방식은 추가 logic을 bank-local request gating과 occupancy control로 제한하지만, 최종 selector timing과 energy는 별도로 평가해야 한다.

### 7.3 Non-stalling admission

Strict partitioning은 scheduling optimization을 dispatch bottleneck으로 바꿀 수 있다. ZERECO는 one-way non-stalling fallback을 사용한다.

```text
priority candidate + compatible P-IQ space
    -> P-IQ에 들어가 scheduling priority 유지

priority candidate + P-IQ full + compatible Normal space
    -> Normal partition으로 fallback하고 priority 상실

no compatible space
    -> ordinary in-order dispatch stall
```

Normal instruction은 reserved P-IQ entry를 빌려 쓰지 않는다. 이를 통해 urgent slice를 위한 bounded capacity를 보존하면서도, Normal capacity가 남아 있을 때 full P-IQ 때문에 processor 전체가 막히는 것을 방지한다.

### 7.4 Scheduling headroom과 interference

P-IQ가 성능을 높이려면 priority가 branch dependence path의 실제 issue winner를 바꿔야 한다. Aggressive age-based scheduler는 이미 많은 older producer를 우대하므로 incremental headroom이 작을 수 있다. 반대로 age advantage를 제거한 scheduling sensitivity에서는 slice priority가 dependency wait과 IPC를 개선한다. 이는 priority scheduling의 causal potential을 보여주지만, 모든 production scheduler에서 같은 gain이 발생한다는 의미는 아니다.

Figure_5

H2P population이 높으면 finite Priority partition을 채울 만큼 많은 candidate가 발생할 수 있다. Non-stalling admission은 fallback instruction이 더 이상 prioritized되지 않는다는 사실을 유지하면서 partition-induced blocking을 줄인다.

Figure_6

P-IQ capacity는 protected priority capacity, fallback frequency, Normal capacity 사이의 trade-off를 만든다. 적절한 capacity는 workload와 backend organization에 따라 달라지며 보편적인 상수가 아니다.

Figure_7

## 8. Hardware Implementation

### 8.1 Major structures

| Structure | 역할 | Representative state |
|---|---|---|
| H2P Branch Table | Persistent H2P branch 선택 | branch tag, failure history, confidence/decay state |
| Retired Instruction Window | Slice discovery용 committed dataflow 보관 | instruction identity, register source/destination, memory metadata |
| Backward-Walk Engine | Register 및 memory producer 복원 | live-source set, walk cursor, slice identifier |
| Block Metadata Cache | 다음 main-thread occurrence tagging | block tag, chain mask, candidate/effective priority mask, slice ownership |
| Target-Load Address Predictor | Future dynamic address 예측 | load tag, base address, stride/delta, confidence, utility, in-flight count |
| RFP Queue | Early load request를 memory system으로 전달 | predicted address, destination PRF ID, dynamic identity, request state |
| RF-Coverage State | Future slice filtering 제어 | per-slice coverage confidence, timeliness history |
| Distributed P-IQ | Residual slice의 issue opportunity 보호 | Priority/Normal occupancy, ready state, FU compatibility |

각 structure의 정확한 capacity와 associativity는 implementation choice다. Metadata/predictor capacity가 커지면 RF coverage가 높아져 P-IQ demand가 감소할 수 있고, 작은 predictor는 더 많은 traffic을 backend priority mechanism으로 보낸다. 따라서 이 구조들은 독립적으로 sizing해서는 안 된다.

### 8.2 End-to-end operation

하나의 dynamic H2P branch occurrence에 대해 ZERECO는 다음 순서로 동작한다.

1. **Frontend metadata lookup:** Fetch block이 Block Metadata Cache를 조회하고 decoded instruction에 chain/effective-priority bit를 부착한다.
2. **Address lookup:** Tagged load가 Target-Load Address Predictor를 한 번 조회한다.
3. **Rename binding:** Destination PRF allocation이 RFP packet을 완성하고 dynamic load identity와 연결한다.
4. **RFP launch:** Request가 translation, LSQ/store check, low-priority cache-port arbitration을 통과한다.
5. **Priority admission:** RF-uncovered slice instruction이 compatible P-IQ entry에 들어가며, full이면 Normal entry로 fallback한다.
6. **Issue and execution:** Bank-local selector가 ready P-IQ entry를 먼저 선택하고 나머지는 normal policy를 따른다.
7. **RF completion:** Timely RFP가 destination PRF를 채우고 dependent wakeup을 앞당긴다.
8. **Validation:** Architectural load address와 memory-order result로 prefetched value를 accept하거나 reject한다.
9. **Branch resolution:** 모든 operand가 준비되면 branch가 actual direction과 target을 계산한다.
10. **Recovery or continuation:** Prediction failure이면 existing recovery path로 fetch를 redirect하고, correct prediction이면 정상 진행한다.
11. **Commit-time training:** Committed branch, slice, address, coverage, timeliness outcome으로 future metadata를 갱신한다.

### 8.3 Correctness invariants

Hardware는 다음 invariant를 만족해야 한다.

- **Committed training:** Squashed instruction은 persistent H2P, slice, predictor, coverage state를 갱신하지 않는다.
- **One prediction per load instance:** Retry가 더 학습된 두 번째 prediction이나 duplicate training을 만들지 않는다.
- **PRF lifetime safety:** RFP packet이 완료 또는 squash될 때까지 destination mapping reference가 유효해야 한다.
- **Address and ordering validation:** Matching address와 legal memory order가 확인된 value만 accept한다.
- **Selective replay:** Address, coherence, ordering failure가 발생하면 speculative consumer를 cancel하고 demand load를 다시 실행한다.
- **Shared-slice safety:** 어느 active RF-uncovered slice도 필요로 하지 않을 때만 shared producer의 priority를 제거한다.
- **Compatible admission:** P-IQ와 fallback placement는 instruction의 FU connectivity를 보존한다.
- **Metadata aging:** Obsolete branch, slice, coverage state가 permanent priority marking을 남기지 않아야 한다.

### 8.4 Timing, bandwidth, area, energy

Block Metadata Cache는 instruction-block delivery와 병렬로 접근하는 것이 바람직하다. Miss가 발생하면 normal execution을 선택하므로 correctness-critical frontend path를 늘리지 않는다. Address-predictor lookup은 load PC가 보일 때 시작할 수 있지만, RFP launch는 destination register binding과 memory ordering에 필요한 rename information을 기다린다.

RFP request는 translation, LSQ, cache-port, PRF-write bandwidth를 사용한다. Demand load보다 낮은 priority를 부여하고 timeliness가 부족하면 drop해야 한다. Correct RFP request는 demand access를 대체하거나 merge해야 한다. 그렇지 않으면 높은 prediction accuracy에도 L1 traffic이 불필요하게 증가한다.

P-IQ는 reserved storage, class별 occupancy state, admission control, priority selection을 추가한다. Priority entry가 지나치게 많으면 Normal capacity가 fragmentation되고, 너무 적으면 fallback이 증가한다. 따라서 table size뿐 아니라 selector delay, bank imbalance, cache-port contention, PRF bandwidth, late request의 energy를 함께 평가해야 한다.

## 9. Discussion and Scope

### 9.1 ZERECO가 제안하는 것

ZERECO는 committed H2P-slice learning, targeted Target-Load RF prefetching, RF-filtered priority scheduling을 결합한 branch-resolution architecture다. 목적은 실제 branch operand를 더 일찍 ready 상태로 만들고 direction/target error를 조기에 드러내는 것이다.

### 9.2 ZERECO가 제안하지 않는 것

- 더 크거나 새로운 branch-direction predictor가 아니다.
- Helper-thread architecture가 아니며 다른 context에서 branch slice를 duplicate execution하지 않는다.
- General-purpose cache prefetcher가 아니며 branch-resolution criticality로 request를 선택한다.
- Pure value prediction이 아니며 predicted load address에 대해 coherent memory hierarchy에서 data를 가져온다.
- P-IQ가 모든 age-based scheduler보다 우수하다는 주장이 아니다. P-IQ headroom은 underlying selection policy에 의존한다.

### 9.3 Remaining implementation questions

앞의 Figure들이 제공하는 latency intervention과 predictor selectivity evidence는 architectural opportunity를 입증하지만, fully timed RFP datapath의 최종 성능과 비용을 대신하지는 않는다. 제안 하드웨어는 최종적으로 다음 질문에 답해야 한다.

1. Post-rename RFP launch가 realistic translation/cache-port contention에서도 충분한 lead time을 제공하는가?
2. Wrong address, store conflict, coherence, replay 비용을 포함해도 선택된 confidence threshold에서 순효과가 유지되는가?
3. Block/slice metadata를 bounded storage로 구현하면서 phase adaptation과 shared-instruction over-marking을 제어할 수 있는가?
4. 어떤 P-IQ capacity가 Normal-capacity, selector, energy 비용을 과도하게 늘리지 않으면서 fallback을 줄이는가?

이 질문들은 architectural thesis의 변경이 아니라 hardware design과 evaluation의 범위다. ZERECO의 thesis가 성립하려면 유용한 H2P Target Load subset이 predictable하고 timely해야 하며, residual slice에 bounded priority resource를 정당화할 scheduling headroom이 존재해야 한다.

## 10. Conclusion

H2P branch는 accuracy-only branch prediction의 한계를 드러낸다. Branch outcome을 결정하는 정보가 아직 core에 도착하지 않은 value에 존재하기 때문이다. ZERECO는 prediction table을 더 키우는 대신 그 value의 생산을 가속한다. Committed execution에서 relevant main-thread slice를 학습하고, predictable Target Load가 가져올 data를 dependent instruction이 source operand로 참조하는 올바른 PRF entry에 미리 기록하며, 남은 slice에는 bounded issue priority를 부여한다. 이를 통해 helper thread나 새로운 direction predictor 없이 H2P branch resolution을 앞당긴다.

## References

- **PUBS**, “Performance Improvement by Prioritizing the Issue of the Instructions in Unconfident Branch Slices,” MICRO 2018.
- **Branch Runahead**, “Branch Runahead: An Alternative to Branch Prediction for Impossible to Predict Branches,” MICRO 2021.
- **RFP**, “Register File Prefetching,” ISCA 2022.
- **TEA**, “Timely, Efficient, and Accurate Branch Precomputation,” MICRO 2024.
