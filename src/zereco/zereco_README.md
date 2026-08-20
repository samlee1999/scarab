# ZERECO: H2P-Chain Load 가속을 통한 Branch 오예측 조기 해소

> Last updated: 2026-08-19
> Simulator: Scarab / Scarab-infra · 별도 helper thread 없이 main-thread H2P dependence chain만 가속한다.
>
> **문서 역할 분담**
> - 이 문서 — 연구 문제, thesis, motivation, 관찰에서 설계로 가는 논리, 실험 이력의 인덱스
> - [zereco_ARCHITECTURE.md](zereco_ARCHITECTURE.md) — **하드웨어 마이크로아키텍처 명세**
>   (구조, cycle-by-cycle 동작, storage budget, invariant, 논문 Mechanism 절의 원본)
> - [zereco_RFP_IMPLEMENTATION_PLAN.md](zereco_RFP_IMPLEMENTATION_PLAN.md) — 구현·검증 대장,
>   실험 기록, 미결정 사항과 sweep 목록
> - [zereco_MOTIVATING_EXAMPLE.md](zereco_MOTIVATING_EXAMPLE.md) — 논문 Figure 1용 예시
> - [zereco_REFERENCE_NOTES.md](zereco_REFERENCE_NOTES.md) — 인용 논문 정리

## 1. 연구 문제와 Thesis

복잡한 history-based branch predictor도 data-dependent H2P(Hard-to-Predict) branch를 안정적으로
예측하기 어렵다. Predictor complexity를 계속 높이는 방법은 저장 공간, 접근 latency, 설계
복잡도를 키우지만 H2P branch의 정확도 개선은 제한적이다.

ZERECO는 정확도를 더 높이는 대신 다음 질문에서 출발한다.

> H2P branch를 피하기 어렵다면, 오예측을 더 빨리 발견해 penalty를 줄일 수 있는가?

핵심 Thesis:

> H2P branch의 긴 fetch-to-resolution latency는 dependence chain의 operand 준비, 특히 chain
> 안의 Target Load latency에 좌우된다. Branch-resolution criticality를 예측기 하나로 선별해
> 예측 가능한 Target Load를 두 delivery path(Register File 직접 전달 / lower-level cache fill)로
> 가속하고, RF로 처리하기 어려운 residual chain은 bounded priority scheduling으로 가속해
> branch resolution을 앞당긴다.

핵심 용어:

- **H2P branch**: 반복적으로 높은 misprediction을 보이며 history만으로 안정적으로 예측하기 어려운 branch
- **Backward slice**: branch outcome 계산에 필요한 older producer 명령어의 data-dependence chain
- **Target Load**: H2P branch의 on-path backward slice에 속해 branch operand 준비에 영향을 주는 load
- **RF prefetch (RFP)**: Target Load가 읽을 주소를 미리 예측해, 그 데이터를 load의 destination
  physical register — dependent instruction이 source operand로 참조할 바로 그 PRF entry — 에
  미리 채워 넣는 것
- **P-IQ**: H2P slice op에 issue 우선순위를 주기 위한 Priority-IQ/RS의 유한 partition

## 2. Motivation

### 2.1 Accuracy가 아니라 penalty를 줄이는 이유

TAGE-SC-L처럼 크고 복잡한 predictor도 load가 만든 runtime value에 direction이 의존하는 branch를
안정적으로 맞히기 어렵다. History와 table에 더 투자해도 load value 자체를 모르므로 한계가 있다.

ZERECO는 그 budget을 penalty 축소에 쓸 수 있는지 묻는다. Misprediction을 일찍 detect하면
recovery와 redirect도 그만큼 일찍 시작되므로, off-path instruction의 fetch를 일찍 중단하고
correct path로 더 빨리 복귀한다.

### 2.2 Fetch-to-resolution이 중요한 이유

```text
branch fetch ──▶ branch resolution / misprediction detection ──▶ recovery and redirect ──▶ first correct-path fetch
```

실측(oldest-first baseline, H2P recovery event 15.3M회 평균): fetch→resolution **42.02 cycle**,
resolution→recovery 16.00, recovery→correct fetch 1.01. 전체 59.03 cycle 중 **71.2%가 dataflow로
공격 가능한 구간**이다. 뒤 두 구간은 recovery 하드웨어의 고정 비용이라 chain 가속으로 줄일 수 없다.

다만 비중만으로 bottleneck이 증명되지는 않는다. 실제로 latency를 줄였을 때 resolution과 IPC가
함께 개선되어야 하며, §3.3이 그 controlled intervention 결과다.

### 2.3 왜 resolution이 늦어지는가

Fetch-to-resolution을 네 단계로 분해한다.

| 단계 | 정의 | 실측 평균 |
|---|---|---:|
| `frontend` | branch fetch → ROB allocation | 10.99 cy |
| `dependency` | ROB allocation → 마지막 source operand ready | **29.94 cy (71%)** |
| `scheduler` | operand-ready → FU select | **0.10 cy** |
| `execution` | select → outcome 확정 | 1.00 cy |

두 숫자가 설계를 결정한다.

1. `dependency`가 지배적이다 → backward slice 전체를 가속해야 한다.
2. `scheduler`가 0.10 cycle이다 → **branch 자체에 priority를 주는 것은 무의미하다.** P-IQ가
   branch가 아니라 그 backward slice 전체를 marking하는 이유가 여기 있다.

## 3. Key Observations와 Root Cause

### 3.1 Fetch-to-resolution에는 줄일 수 있는 headroom이 있다

§2.2의 71.2%. Branch outcome을 더 일찍 계산하는 접근이 구조적 headroom을 가진다.

### 3.2 Dependency wait이 긴 resolution의 중심이다

Branch 자체의 ready-to-issue 시간(0.10 cycle)만으로는 이 병목을 설명할 수 없다. Branch까지
이어지는 older producer chain — 그 안의 address generation, load service, value 전달 — 을 함께
봐야 한다.

### 3.3 왜 Target Load를 가속하는가

선택된 Target Load의 latency만 줄였을 때 `dependency` 29.94 → 25.50 cy, fetch→resolution
42.02 → 37.52 cy, IPC +5.90%가 함께 움직인다. 상관이 아니라 **controlled intervention**이므로
Target Load latency가 causal bottleneck이라는 근거가 된다.

일반적인 cache prefetch는 data를 cache 가까이 가져와도 demand load가 address generation,
scheduling, cache access를 거쳐 destination register를 채울 때까지 dependent op를 깨울 수 없다.
RF prefetch는 그 전달 자체를 앞당긴다.

### 3.4 그러나 실제 이득의 지배 성분은 두 번째 경로였다

구현 후 측정에서 나온 가장 중요한 수정 사항이다. RF prefetch를 시도했는데 L1 miss일 때,
packet을 버리는 대신 하위 계층으로 진행시켜 line을 L1로 끌어올리면 이득이 훨씬 크다.

| 정책 | IPC | L1D read miss |
|---|---:|---:|
| L1 miss 시 drop | +1.75% | — |
| **L1 miss 시 하위 계층 진행** | **+5.90%** | 126.0M → 95.8M (−24%) |

이유는 이득 구조가 다르기 때문이다. RF path의 load당 이득은 `L1 hit latency − RF hit latency
= 4 cycle`이 구조적 상한이고(실측 평균 2.70), fill path는 `MLC/LLC/DRAM latency − L1 latency`라
상한이 없다. 유도와 근거는 ARCHITECTURE §6.9.

⇒ 서사를 **"cache prefetch로는 불충분"**에서 **"criticality 예측기 하나, delivery path 둘"**로
재구성한다.

### 3.5 모든 Target Load를 가속할 수는 없다

Target Load access는 일부 load PC에 집중되고 PC-local history로 높은 정확도(92.7%)에서 예측
가능한 stream이 존재한다. 동시에 irregular access도 남는다 — stride reset : confidence 포화 =
**37 : 1**로 거절이 압도적이다. 그래서 predictor는 abstain할 수 있어야 하고, 전체 load 기준
최종 coverage는 19.9%에 그친다.

잘못된 cache prefetch는 bandwidth와 cache pollution을 낭비할 뿐이지만, RF prefetch가 잘못된
값을 register에 공급하면 dependent op와 branch가 잘못 실행된다. ZERECO는 이를
**validate-then-use**로 막는다: load 자신의 AGU에서 주소를 비교하고 memory order를 확인한 뒤에만
값을 채택한다. 그래서 오예측 비용은 **낭비된 probe 1회**가 전부이며 flush나 replay가 없다
(실측 "demand delayed by prefetch = 0"). Store dependence가 있는 load는 아예 abstain한다.

### 3.6 IQ priority는 RF를 보완하는 secondary mechanism이다

Oldest-first는 이미 older producer를 우대하므로 P-IQ가 바꿀 수 있는 winner가 적다. Age advantage가
없는 random-physical 조직 — PUBS가 현대 대표 IQ 조직으로 채택·정당화한 baseline — 에서는
priority가 select 대기를 2.42 → 0.63 cycle로 줄이며 효과가 크게 나타난다.

무제한 marking은 정당한 설계가 아니다. 추적한 chain op에게만 무조건 우선권을 줄 수는 없으므로,
제안 설계는 **유한 partition(기본 20% 예약) + non-stall fallback**이고 무제한 marking은
상한(upper bound)으로만 보고한다.

## 4. Observation에서 ZERECO 구조가 도출되는 과정

```text
1. H2P branch와 backward slice 식별            (retire-time, committed execution만)
2. slice 안의 Target Load를 판정                → 예측기 추적 대상 선별 (criticality)
3. 그 PC들의 주소 스트림을 학습                  → 예측 가능성 판정 (predictability)
4. 예측 가능한 Target Load를 rename에서 발사     → L1 hit이면 RF 전달, miss면 L1로 fill
5. RF로 처리하기 어려운 residual slice에 priority 부여
6. finite P-IQ가 full이면 Normal entry로 fallback (priority 상실)
7. main-thread branch의 resolution을 앞당김
```

2와 3이 분리되어 있는 것이 설계의 핵심이다. **Backward walk는 "어느 PC를 추적할지"만 정하고,
주소 학습은 retire에서 한다.** Walk 중에는 retire 기록이 gating되어 sampled stream이 되므로,
walk에서 주소를 학습하면 `base + stride × inflight` 점화식이 깨진다. 반면 PC 멤버십은 집합
판정이라 sampling에 강인하다.

이 선별의 값어치는 예측기 압력으로 측정된다: Target Load만 추적하면 PT allocation 155K,
전체 load를 추적하면 18.9M(완전 스래싱)으로 **122배** 차이가 나면서 성능은 동등하다.

## 5. ZERECO Mechanism Overview

> 구조·cycle 동작·storage는 [zereco_ARCHITECTURE.md](zereco_ARCHITECTURE.md)가 정본이다.
> 여기서는 세 엔진의 역할만 요약한다.

### 5.1 Identification (retire side, critical path 밖)

- **H2P Branch Table** (1024 entry, 3-bit saturating, decay 50K retire)로 persistent H2P branch 선별
- **Retired Instruction Window** (512 uop)에 committed dataflow를 보관
- 가장 오래된 H2P branch가 window 밖으로 밀려나는 순간 **Backward-Walk Engine**이 500 cycle
  동안 backward slice를 복원 (register live-in bit vector + committed VA 기반 store→load 연결)
- 결과를 **Block Metadata Cache** (1024 entry)에 block-fragment mask로 OR 누적
- Frontend는 block-start PC로 1회 조회해 uop마다 chain/priority bit를 부착 (fetch와 병렬, miss면 normal)
- TEA의 identification 아이디어만 재사용하며 helper thread는 두지 않는다

### 5.2 Delivery — Target-Load RF Prefetch

- **Prefetch Table** 1024 entry / 8-way, load PC indexed. **allocation 권한은 backward walk에만**
  있으므로 PT 멤버십 ≡ Target Load 판정
- **retire**: `inflight--`, delta 계산, 같은 stride 반복 시 1/16 확률로 confidence++, 깨지면 reset
- **rename**: `inflight++` 후 관문 3단 — ① PT hit(criticality) ② store dependence 없음
  ③ confidence 포화(predictability) — 를 통과하면
  `predicted VA = base_va + stride × inflight`로 packet을 만들어 발사. destination PRF는 이미
  할당되어 있으므로 write target이 확정된다
- **RFP Queue** 64-entry FIFO, cycle당 2개 drain, **demand load가 쓰고 남긴 L1 read port만** 사용
- **probe**: L1 hit → 데이터를 destination PRF로 / L1 miss → MSHR 확보 후 하위 계층 진행, fill 시 L1에 삽입
- **validate-then-use**: load의 AGU에서 주소 비교 + store queue 스캔 + timeliness 판정을 통과하면
  cache 재접근 없이 완료하고 dependent를 깨운다. 실패하면 정상 demand path로 진행 (무해)
- **squash**: `inflight--`, packet tombstone. persistent state는 committed instruction만 갱신

### 5.3 Scheduling — RF-filtered P-IQ

Ready op의 선택 순서:

```text
ZERECO Priority op > Normal op
같은 class 안에서는 baseline 정책 (oldest-first 또는 random-physical)
```

- 각 RS의 기존 용량을 Priority/Normal로 나눈 **유한 partition** (기본 20% 예약)
- **non-stall fallback**: Priority 구획이 full이면 Normal entry에 배치하고 **priority를 영구 상실**.
  Normal op는 Priority 구획을 빌리지 않는다 (one-way)
- 하드웨어 구현은 PUBS의 **free-list 분할** — Priority op에 낮은 physical entry ID를 주면
  기존 fixed-priority encoder가 select logic 수정 없이 class 순서를 구현한다
- **RF filtering**: slice의 Target Load가 연속 2회 전부 RF-covered이면 다음 occurrence의
  priority를 억제한다. shared producer는 어느 RF-uncovered slice도 필요로 하지 않을 때만 해제된다

Scheduler policy 두 가지를 지원한다.

- **Oldest-first**: 낮은 `op_num` 우선 — 현실적 기본 baseline
- **Random-physical**: 낮은 physical RS entry ID 우선 — PUBS가 현대 대표 조직으로 채택한 baseline이며,
  oldest-first가 감추는 priority headroom을 드러낸다

## 6. Prior Work와 ZERECO의 차이

| Prior Work | 가져오는 요소 | ZERECO의 차이 |
|---|---|---|
| **TEA** (MICRO'24) | H2P branch·backward-slice identification, Retired Instruction Window, Block Cache | Helper thread, duplicated chain execution, 별도 architectural state를 쓰지 않는다. identification metadata만 재사용한다. |
| **RFP** (ISCA'22) | Load address prediction, in-flight-aware 주소 생성, post-rename launch, validate-then-use | 모든 load가 아니라 H2P resolution에 영향을 주는 Target Load만 선택한다. RFP 논문 §6이 직접 criticality 기반 결합을 future work로 열어 두었다. |
| **PUBS** (MICRO'18) | Branch slice op의 IQ priority, free-list 분할식 partition | Scheduling만으로 제거할 수 없는 cache/memory latency를 먼저 RF/fill로 줄이고, residual slice에만 non-stall bounded priority를 적용한다. |
| **Branch Runahead** (MICRO'21) | Dependence-chain 기반 branch 가속 | 별도 엔진에서 outcome을 선행 계산하지 않고 main-thread의 fetch 이후 resolution만 줄인다. |

References:

- `/home/lee/scarab/reference/[2024, MICRO] Timely_Efficient_and_Accurate_Branch_Precomputation.pdf`
- `/home/lee/scarab/reference/[2022, ISCA] Reg File prefetching.pdf`
- `/home/lee/scarab/reference/[2018, MICRO] PUBS.pdf`
- `/home/lee/scarab/reference/[2021, MICRO] Branch Runahead_Pruett,Y.Patt.pdf`

## 7. 현재 구현 상태

`tea_enable=0`인 main-thread-only 모델이며 `mispred`와 `misfetch`를 모두 H2P profiler에 포함한다.
RFP는 **완전한 timed 모델**이고 기존 oracle 경로(`h2p_chain_perfect_load`)와 상호 배제된다.

| 경로 | 역할 |
|---|---|
| `src/zereco/rfp.c`, `rfp.h` | Prefetch Table, launch, RFP Queue, L1 probe, fill path, validate-then-use |
| `src/fill_buffer.c`, `src/dependency_chain_cache.c` | Retired Instruction Window, backward walk, Target Load 판정, slice/block mask, RF filtering |
| `src/decoupled_frontend.cc` | Block Metadata Cache 조회와 op tagging |
| `src/bp/hbt.c` | H2P Branch Table |
| `src/map_stage.c` | rename 시점 RFP launch 훅 |
| `src/dcache_stage.c` | queue drain, port 회계, validate 훅, store-write 기록 |
| `src/node_stage.c`, `src/exec_ports.c` | P-IQ partition 크기와 occupancy |
| `src/node_issue_queue.cc` | admission, non-stall fallback, priority-first selection |
| `src/zereco/h2p_mispred_latency.c` | H2P misprediction/misfetch latency profiler |
| `src/core.param.def`, `src/zereco/zereco.stat.def` | 파라미터(RFP 19종 + priority/P-IQ 7종 + profiler 1종)와 통계 정의 |
| `/home/lee/scarab-infra/json/zereco_dbg.json` | build/run descriptor |

빌드: `cd /home/lee/scarab-infra && ./sci --build-scarab zereco_dbg` (확장자 없이).
실행은 `./sci --sim zereco_dbg`. 워크로드 목록은 고정이고 `configurations`만 수정한다.

필수 무결성 조건: shadow-selection / physical-entry / P-IQ partition / non-stall /
demand-delayed-by-prefetch / stale-deref mismatch counter가 모두 0.

## 8. 실험 근거

### 8.1 현재 사이클의 실험 (timed RFP + P-IQ)

| 결과 디렉터리 | 확인한 내용 |
|---|---|
| `260815_RFP_baseline_oldest_first` | Phase 1 profile-only — 예측기 특성, timing 중립성 |
| `260816_RFP_phase2_oldest_first` | Phase 2 — 대역폭·timeliness 깔때기 실측 |
| `260817_RFP_phase3_oldest_first` | Phase 3 — covered fast path, L1-miss 정책·축별 오라클 |
| `260818_RFP_piq_integration` | RFP + P-IQ 첫 결합, 두 scheduler 축의 사다리 |
| `260819_RFP_piq_axes` | randq 축 분해와 partition 축 완성 — 제안 설계 확정 |

성능 사다리 (geomean IPC, 각 축의 baseline 대비):

```text
[OLDEST-FIRST]                  IPC      f→res      dep    br-select
  baseline                       —      42.02cy   29.94cy   0.10cy
  + RFP                       +5.90%    37.52     25.50     0.10
  + P-IQ 상한(무제한 mark)    +6.43%    37.28     25.20     0.07
  + P-IQ 20% partition        +6.15%    37.38     25.30     0.09

[RANDOM QUEUE (PUBS 대표 조직)]
  baseline_randq                 —      45.76     31.07     2.42
  + RFP                       +6.75%    40.05     26.17     1.66
  + P-IQ 상한                 +9.40%    37.96     25.12     0.53
  + P-IQ 20% partition        +8.95%    38.24     25.35     0.63
```

### 8.2 이전 사이클 (oracle 기반 motivation·sensitivity)

| 결과 디렉터리 | 확인한 내용 |
|---|---|
| `zereco/260624_h2p_chain_load_access_pattern_all_simpoints` | Target Load가 일부 PC에 집중되며 predictable stream과 irregular tail이 공존 |
| `zereco/260625_perf_comparison` | Target Load latency를 제거했을 때의 성능 headroom (full-oracle motivation) |
| `zereco/zereco_260729_misp_penalty_breakdown` | penalty portion, dependency 단계 지배, predictor-gated RF의 효과 |
| `zereco_260802_iq_priority_online` | online RF filtering의 priority population 감소 |
| `zereco_260803_iq_priority_interference` | priority가 normal-op selection에 주는 간섭 |
| `zereco_260804_piq_sweep` | strict finite partition의 stall 비용 |
| `zereco_260805_piq_nonstall_sweep` | non-stall fallback이 partition blocking을 줄임 |
| `zereco_260805_random_iq_comparison` | oldest-first vs random-physical의 scheduling headroom 차이 |
| `zereco_260806_random_piq_policy_sweep` | random-physical에서의 P-IQ 효과와 finite ratio sensitivity |

### 8.3 집계 원칙

- 완료된 공통 SimPoint만 configuration 간 비교한다.
- Workload 내부 latency는 SimPoint-weighted total cycles / weighted event count로 계산한다.
- IPC speedup은 workload별 IPC ratio의 geometric mean, latency는 workload-equal mean.
- Coverage는 correct·timely deliveries / candidate loads, accuracy는 correct / launched.
- 결과 인용 전 `PARAMS.out`, 완료 상태, 무결성 counter를 확인한다.
- **1차 지표는 H2P resolution latency다.** Claimed saved cycle의 13%만 총 cycle 감소로
  실현되므로(OoO 흡수 + 해당 branch가 실제로 틀린 경우에만 발현) IPC는 약한 대리 지표다.

## 9. 예상과 달랐던 결과 (연구 방향에 영향을 준 것)

| # | 발견 | 영향 |
|---|---|---|
| 1 | **cache-fill 경로가 RF 경로보다 이득이 큼** | motivation을 "경로 둘"로 재구성 (§3.4) |
| 2 | **대역폭·wrong-path 모두 비제약** | confidence gating·전용 port 계획 폐기, negative result로 인용 |
| 3 | **예측 가능성 ↔ run-ahead 역상관** | RF path 이득이 load당 ≤4 cycle에 구조적으로 묶임 (ARCHITECTURE §6.9) |
| 4 | **RFP의 2차 효과** — RS 점유 완화가 select 대기까지 단축 (randq 2.42 → 1.66 cy) | RFP 몫이 randq에서 더 큼 |
| 5 | **RF filtering의 존재 이유 상실** — bounded partition이 인구를 자기조절 | 기각 후보 (계획서 D-1) |
| 6 | partition 보존율의 scheduler 의존성 (randq 83% vs OF 52%) | 논문 사다리를 randq 단일로 갈지 결정 필요 (D-2) |
| 7 | IPC 실현률 13% | 1차 지표를 H2P resolution latency로 |

예상과 일치한 것: dependency 지배(71%), branch 단독 priority의 무의미함(0.10 cy),
P-IQ headroom의 조직 의존성, Target-Load 선별의 예측기 효율(122×), validate-then-use의 무해성.

## 10. 현재 모델의 한계와 Open Items

- **값 정확성은 모델링하지 않는다.** 주소 검증은 실제로 수행하지만 값은 trace에서 오므로
  stale value / wrong value의 주입과 recovery 비용은 세기만 한다. 현재 stale 창은 useful의
  8.6%이며 **line(64B) 입도 상한**이다 — 논문 게재 전 byte 겹침으로 정제해야 한다 (D-4).
- **Store forwarding을 포기했다.** RFP 논문은 store data를 prefetch에 forwarding하지만 ZERECO는
  abstain한다. 포기 모집단은 PT-hit load의 10.6%, 그중 즉시 forwarding 가능한 것은 12.1% (D-3).
- **Speculative wakeup은 완료 시점 등가로 모델링한다.** 시뮬레이터의 wakeup이 비투기적이라
  RFP-inflight bit로 정렬할 대상이 없다. 논문 mechanism/storage 절에는 하드웨어 원형을
  기술하고, Methodology에 이 등가성을 한 문단 명시한다 (ARCHITECTURE §11.1).
- **Identification의 storage가 지배적이다.** Block Metadata Cache + RIW가 ~33KB로 전체 ~50KB의
  대부분이다. TEA에서 상속한 구조라는 점과, mechanism 고유 비용(PT 12KB + queue 0.6KB)을
  분리해 서술해야 한다.
- **Random-physical은 sensitivity 도구가 아니라 PUBS의 대표 baseline이다.** 다만 oldest-first를
  대체하는 최종 성능 기준으로 쓸지는 서사 결정 사항이다 (D-2).
- **P-IQ가 줄일 수 있는 것은 selection과 producer scheduling 지연뿐이다.** DRAM
  bandwidth-bound workload(pr)에서는 chain 우대가 MLP 생성 load를 밀어내 −1.3%p까지 손해다.
- **Sweep 미완**: PT 크기 × scope (S-1, complexity 주장의 본 그림), confidence 폭 (S-2),
  partition 예약률 (S-3), 전체 SimPoint 확대 (S-5, **weight 가중 집계**로).

세부 결정 항목과 sweep 계획은 [zereco_RFP_IMPLEMENTATION_PLAN.md](zereco_RFP_IMPLEMENTATION_PLAN.md) §6.
