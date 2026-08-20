# ZERECO: H2P Branch Resolution Acceleration — Microarchitecture Specification

> Last updated: 2026-08-19 · 대상 구현: timed RFP + P-IQ (Phase 0~4 완료)
>
> **이 문서의 관점은 하드웨어 마이크로아키텍처다.** 논문의 Mechanism / Implementation
> 절은 이 문서를 원본으로 쓴다. 시뮬레이터(Scarab) 구현과의 대응은 §11 한 곳에만 모아
> 두었고, 본문은 "어떤 구조가 어느 stage에서 몇 cycle에 무엇을 하는가"로만 기술한다.
>
> - 실험 수치·검증 대장·미결정 사항: [zereco_RFP_IMPLEMENTATION_PLAN.md](zereco_RFP_IMPLEMENTATION_PLAN.md)
> - 연구 서사·동기: [zereco_README.md](zereco_README.md)
> - 인용 논문 정리: [zereco_REFERENCE_NOTES.md](zereco_REFERENCE_NOTES.md)
> - 코드 실물: [rfp.h](rfp.h) / [rfp.c](rfp.c)

---

## 1. Introduction

현대의 branch predictor는 긴 global/local history, 여러 prediction component, 큰 storage
budget을 활용한다. 그럼에도 최근에 load된 runtime value에 outcome이 의존하는 branch는
여전히 예측하기 어렵다. 이러한 Hard-to-Predict(H2P) branch는 history와 table을 추가해도
branch outcome을 결정하는 실제 data가 predictor에 보이지 않기 때문에, predictor complexity
증가 대비 accuracy 개선이 제한적이다.

ZERECO는 이 문제를 branch prediction accuracy가 아니라 **misprediction detection latency**의
관점에서 접근한다. H2P branch의 prediction failure 자체를 없애는 대신, branch가 pipeline에
들어온 뒤 actual outcome을 계산하기까지의 시간을 줄여 misprediction을 더 일찍 발견한다.

> **ZERECO는 committed execution으로부터 H2P branch의 backward slice를 학습하고, 그 안의
> predictable Target Load를 두 개의 delivery path(Register File 직접 전달 / lower-level
> cache fill)로 가속하며, 남은 slice에는 bounded issue priority를 부여해 branch를 조기에
> resolve한다.**

핵심 contribution:

1. H2P branch의 긴 fetch-to-resolution에서 branch execution 자체가 아니라 **operand
   dependency resolution**이 지배적 reducible component임을 밝힌다 (실측 29.94 / 42.02 cycle = 71%).
2. 그 dependency wait의 causal source가 backward slice 안의 Target Load latency이며,
   lightweight PC-local history로 예측 가능한 subset이 존재함을 보인다.
3. Branch-resolution criticality를 예측기 하나로 선별하고, 그 결과를 **두 개의 delivery
   path**로 나눠 쓰는 구조를 제안한다: L1에 있는 line은 destination physical register로
   직접 전달하고(RF path), L1에 없는 line은 하위 계층에서 L1로 끌어올린다(fill path).
4. RF prefetch가 처리하지 못한 residual slice만 **finite non-stalling Priority IQ**로
   가속하는 hybrid backend를 제안한다.

---

## 2. Motivation and Key Insights

### 2.1 왜 accuracy가 아니라 penalty를 줄이는가

Misprediction 손실은 (빈도) × (event당 penalty)로 결정된다. Prior work 대부분은 첫 항을
줄인다. 그러나 아직 load되지 않은 data에 direction이 의존하는 branch에는 근본적 한계가 있다.
ZERECO는 두 번째 항을 공격한다: prediction failure를 피할 수 없다면, 더 일찍 발견한다.

### 2.2 Fetch-to-resolution은 줄일 수 있는 구간이다

```text
branch fetch ──▶ branch resolution (misprediction detection) ──▶ recovery/redirect ──▶ first correct-path fetch
             └────────── misspeculation interval (PUBS) ─────┘   └── 고정 파이프라인 비용 ──┘
```

앞 구간만이 dataflow로 줄일 수 있다. 뒤 구간은 recovery 하드웨어의 고정 비용이다. 실측
(oldest-first baseline, H2P recovery event 15.3M회 평균): fetch→resolution **42.02 cycle**,
resolution→recovery 16.00 cycle, recovery→첫 correct-path fetch 1.01 cycle. 즉 전체 59.03
cycle 중 **71.2%가 dataflow로 공격 가능한 구간**이다.

### 2.3 Dependency wait이 그 구간을 지배한다

Fetch-to-resolution을 네 구간으로 분해한다. 각 구간의 정의는 §12의 profiler와 동일하다.

| 구간 | 정의 | H2P 평균 (oldest-first baseline) |
|---|---|---:|
| `frontend` | branch fetch → ROB allocation | 10.99 cy |
| `dependency` | ROB allocation → 마지막 source operand ready | **29.94 cy** |
| `scheduler` | operand-ready → FU select | 0.10 cy |
| `execution` | select → outcome 확정 | 1.00 cy |
| **fetch → resolution** | | **42.02 cy** |

이 분해가 mechanism의 범위를 결정한다. **Branch 자체에 priority를 주는 것은 무의미하다**
— branch가 ready된 뒤 select를 기다리는 시간은 0.10 cycle뿐이다. Branch를 ready로 만드는
older producer 전체를 가속해야 한다. 이것이 P-IQ가 slice-wide marking을 하는 이유다.

### 2.4 Target Load latency가 causal bottleneck이다

**Target Load** = H2P branch의 dynamic on-path backward slice에 속한 load. 이 load의 service
latency는 branch로 이어지는 모든 younger consumer를 지연시킨다. 상관관계뿐 아니라
controlled intervention이 근거다: 선택된 Target Load의 latency만 줄였을 때 dependency wait
(29.94 → 25.50 cy)과 fetch-to-resolution(42.02 → 37.52 cy)이 함께 감소하고 IPC도 오른다.

### 2.5 Target Load에는 predictable stream과 irregular tail이 공존한다

Target-Load access는 static load PC에 균등 분산되지 않는다. 소수 PC가 dynamic access의 큰
비중을 차지하고, 그중 다수가 stable stride를 보인다. 동시에 irregular access도 남으므로
predictor는 **abstain할 수 있어야 한다**. 실측: launch한 prefetch의 address accuracy 92.7%,
그러나 전체 load 대비 최종 coverage는 19.9%(전체 load 분모).

높은 conditional accuracy + 불완전한 coverage → selective hybrid design.

### 2.6 하나의 criticality 예측기, 두 개의 delivery path

RF prefetch가 저장하는 것은 "언제 무엇을 읽어야 branch가 빨라지는가"이다. 그 정보를 얻은
뒤 데이터가 이미 L1에 있으면 register file로 직접 전달하는 것이 최선이고, L1에 없으면
같은 요청을 하위 계층으로 보내 line을 L1로 끌어올리는 것이 최선이다. 두 경로의 이득 구조는
완전히 다르다.

| Delivery path | 조건 | load당 이득 | 상한 |
|---|---|---|---|
| **RF path** | probe가 L1 hit | `min(V − P, 4)` cycle, 평균 2.7 | **L1 hit latency − RF hit latency = 5 − 1 = 4 cycle** (§6.9) |
| **Fill path** | probe가 L1 miss | (MLC/LLC/DRAM latency) − (L1 hit latency) | 11 / 31 / 수백 cycle |

실측에서 fill path가 지배적이다 (RFP 단독 +5.90% 중 drop 정책 대비 +4.15%p; L1D read miss
126.0M → 95.8M). 이 사실은 "cache prefetch로는 불충분"이라는 단순 서사를 쓸 수 없게 만들며,
대신 **criticality 예측기를 공유하는 두 경로**라는 서술이 정직하다.

### 2.7 P-IQ는 primary가 아니라 complementary다

Priority scheduling은 load가 issue된 뒤의 cache/memory service latency를 없앨 수 없다.
반대로 RF prefetch는 address stream이 불규칙하거나 non-load producer가 병목인 slice를
가속하지 못한다. ZERECO는 서로 다른 residual bottleneck에 서로 다른 mechanism을 쓴다.

또한 P-IQ의 headroom은 **baseline issue queue 조직에 의존한다**. Age-based selector는 이미
older producer를 우대하므로 바꿀 수 있는 winner가 적다(순수 몫 +0.24~0.46%). Age advantage가
없는 random-queue 조직 — PUBS가 현대 대표 조직으로 채택·정당화한 baseline — 에서는 순수 몫이
+2.06~2.48%로 커진다.

---

## 3. Background and Design Gap

### 3.1 H2P branch와 backward slice

H2P branch는 한 번 low-confidence였던 branch가 아니라, data dependence 또는 복잡한 control
behavior로 **반복적으로** prediction failure를 내는 static branch다.

Dynamic backward slice = branch와, branch operand를 직간접으로 생성하는 모든 older
instruction. Basic-block 경계를 넘고, register dependence와 memory dependence를 모두 포함하며,
relevant path가 runtime control flow와 address에 따라 달라진다. 따라서 ZERECO는 compiler가
만든 고정 slice가 아니라 **committed dynamic execution**에서 slice를 학습한다.

### 3.2 일반 cache prefetch로 충분하지 않은 이유

Cache prefetcher는 data를 core 가까이 옮기지만, demand load는 여전히 operand readiness →
address generation → issue → cache access → destination register write → dependent wakeup을
거친다. ZERECO의 목표는 cache locality가 아니라 **branch operand의 register-ready 시점**이다.
단, §2.6이 말하듯 L1 miss인 경우에는 line을 L1로 끌어올리는 것 자체가 훨씬 큰 이득이며,
ZERECO는 두 경우를 같은 예측기로 처리한다.

### 3.3 일반 issue priority로 충분하지 않은 이유

Conventional selector는 age 또는 physical queue position을 쓴다. 어떤 ready instruction이
unresolved H2P branch의 dependence chain에 속한다는 사실은 고려하지 않는다. 연속 producer가
각각 한 cycle씩 밀리면 그 지연은 chain을 따라 누적된다. 그래서 branch 하나가 아니라
**residual slice 전체**를 표시해야 한다 (§2.3의 0.10 cycle이 이 논거의 실측판이다).

### 3.4 Prior work와의 관계

| Prior work | 차용하는 기반 | ZERECO가 채우는 gap |
|---|---|---|
| **TEA** (MICRO'24) | Runtime H2P identification, Retired Instruction Window, backward dataflow walk, block-granular chain metadata | TEA는 별도 precomputation thread를 실행한다. ZERECO는 identification metadata만 재사용하고 **original main-thread instruction**을 가속한다. |
| **RFP** (ISCA'22) | PC-indexed address prediction, in-flight-aware future address 생성, post-rename launch, validate-then-use | RFP는 general load 전체를 대상으로 한다. ZERECO는 H2P resolution을 지연시키는 Target Load만 선택하고 residual-slice scheduling과 결합한다. RFP 논문 §6이 직접 *"combining RFP with criticality-based solutions can further reduce end-to-end pipeline latency for critical loads and their load slices"* 라고 이 방향을 열어 두었다. |
| **PUBS** (MICRO'18) | Unconfident branch slice에 대한 highest issue priority, free-list 분할식 partition | Scheduling은 load service latency를 못 숨긴다. ZERECO는 predictable load latency를 먼저 줄이고 residual slice에만 non-stalling bounded priority를 적용한다. |
| **Branch Runahead** (MICRO'21) | 예측 불가 branch의 dynamic dependence-chain construction | 별도 engine에서 outcome을 precompute하지 않는다. Main thread의 post-fetch resolution만 줄인다. |

ZERECO가 채우는 gap은 또 하나의 branch precomputation mechanism이 아니라, **branch-resolution
critical load의 data delivery**와 **남은 dependence chain의 selective scheduling**을 결합하는
main-thread architecture다.

---

## 4. Architecture Overview

### 4.1 Design goals

1. **Persistent H2P branch만 타깃**으로 한다 — easy branch가 acceleration resource를 점유하지 않는다.
2. **Committed execution으로 학습**한다 — wrong-path instruction이 persistent metadata를 오염시키지 않는다.
3. **Computation을 복제하지 않는다** — original main-thread slice가 유일한 architectural execution이다.
4. **불확실하면 abstain**한다 — low-confidence, store-dependent, late request는 normal demand path로 돌아간다.
5. **Backend interference를 bounded로 유지**한다 — finite priority partition + non-stalling fallback.

### 4.2 세 엔진과 pipeline 배치

```text
                       ┌───────────────── OFF THE CRITICAL PATH (retire side) ─────────────────┐
                       │                                                                       │
   retire ───────────▶ │  H2P Branch Table ──▶ Retired Instruction Window ──▶ Backward-Walk    │
        │              │        (HBT)              (RIW, 512 uop)              Engine (BWE)    │
        │              │                                                            │          │
        │              └────────────────────────────────────────────────────────────┼──────────┘
        │                                                     ┌──────────────────────┴──────────┐
        │                                                     ▼                                 ▼
        │                                       Block Metadata Cache (BMC)         Prefetch Table (PT)
        │                                       [chain / cand / eff-prio mask]      [Target Load PC 집합]
        │                                                     │                                 │
        │                                        (block-start PC lookup)          (load PC lookup)
        │                                                     ▼                                 ▼
  ┌─────┴───────────────────────────────────────────────────────────────────────────────────────────────┐
  │  Fetch ──▶ Decode ──▶ Rename ──▶ Dispatch ──▶ Issue Queue ──▶ Select ──▶ AGU/Exec ──▶ Dcache ──▶ ROB │
  │              │           │           │            │                          │                       │
  │        chain/prio    PT lookup    P-IQ 파티션   priority-first             validate                  │
  │        bit 부착      packet 생성   admission     selector                  then-use                  │
  └──────────────────────────────┬──────────────────────────────────────────────────────────────────────┘
                                 ▼
                        RFP Queue (64, FIFO)
                                 │  (lowest-priority L1 read port)
                                 ▼
                     L1D ──hit──▶ 데이터를 destination PRF entry로
                        └─miss──▶ MSHR ▶ MLC/LLC/DRAM ▶ L1 fill (+PRF)
```

세 engine의 역할:

| Engine | 위치 | 무엇을 만드는가 | critical path 여부 |
|---|---|---|---|
| **Identification** (HBT + RIW + BWE + BMC) | retire side, 완전 비동기 | "이 static op는 H2P slice 소속인가 / 이 static load PC는 Target Load인가" | ✗ (frontend lookup만 병렬) |
| **Delivery** (PT + RFP Queue + L1 port) | rename → dcache | "이 dynamic load가 읽을 주소는 무엇이고, 데이터를 미리 어디까지 가져다 놓을 수 있는가" | ✗ (demand보다 낮은 우선순위) |
| **Scheduling** (P-IQ) | dispatch → select | "남은 slice op가 select에서 이기게 한다" | ✓ (partition + comparator) |

---

## 5. H2P Slice Identification Engine

Identification은 전부 **retire 이후**에 일어난다. 어떤 단계도 main pipeline을 stall시키지
않으며, frontend에는 단일 lookup(§5.6)만 노출된다.

### 5.1 H2P Branch Table (HBT)

| 항목 | 값 |
|---|---|
| 구성 | 1024 entry, direct-mapped, branch PC indexed |
| Entry | tag + 3-bit saturating counter |
| 갱신 | architecturally resolved branch — misprediction 시 +1, correct 시 −1 |
| H2P 판정 | counter > 1 |
| Decay | 50,000 retire마다 전 entry 1 감소 |

Decay가 phase adaptation을 담당한다. HBT는 direction/target을 **예측하지 않는다** — chain
learning과 acceleration을 적용할 branch만 고른다.

### 5.2 Retired Instruction Window (RIW)

Backward walk가 볼 수 있는 최근 committed dataflow의 circular window.

| 항목 | 값 |
|---|---|
| 크기 | 512 uop (FIFO, retire 순서) |
| Entry 필드 | static PC, block-start PC + block-relative slot index, source register id (≤4), destination register id (≤2), op type, memory type(LD/ST), 확정된 virtual address, HBT H2P bit |
| Write | retire 1개당 1 entry, in-order |

Committed instruction만 담기 때문에 walk를 위해 speculative checkpoint를 둘 필요가 없다.
Wrong-path instruction은 애초에 window에 들어오지 않는다 (goal 2).

### 5.3 Backward-Walk Engine (BWE) — trigger와 cycle 동작

**Trigger.** RIW가 가득 찬 상태에서 새 retire가 들어와 head entry를 덮어써야 하는데 그
head entry가 H2P branch이면, 그 순간 walk를 시작한다. 즉 "가장 오래된 H2P branch가 window
밖으로 밀려나기 직전"이 trigger이며, 이 시점에는 그 branch의 producer들이 이미 window
안에 최대한 많이 모여 있다.

**Snapshot.** Trigger 시 window의 511개 entry(밀려나는 H2P 자신은 제외 — 그보다 older인
producer가 없어 chain을 만들 수 없다)를 walk engine의 snapshot buffer로 복사한다.

**Walk 예산.** Engine은 500 cycle 동안 BUSY 상태를 유지한다. 이 동안 RIW write는 gating
되며, walk 완료 시 RIW는 비워지고 다시 채워진다.

> ⚠ **이 gating이 identification을 "sampled"로 만든다.** Walk 500 cycle 동안 retire된
> instruction은 window에 남지 않는다. 그래서 **주소 학습을 walk에서 하면 안 된다** —
> snapshot 경계를 넘는 delta가 k×stride로 보여 `base + stride × inflight` 점화식이 깨진다.
> PC 멤버십(집합 판정)만이 sampling에 강인하다. 이것이 §6의 훈련 2단 분리의 근거다.

**Walk 알고리즘 (snapshot 안의 각 H2P마다 독립 수행).** Live-in set은 두 부분이다.

- **Register live-in bit vector**: `NUM_REG_IDS` bit (2 × 64-bit word). x86 backend의
  architectural + temporary register id를 전부 덮는다.
- **Memory live-in address list**: committed virtual address의 소집합.

```text
  live_reg  ← trigger branch의 source register
  live_mem  ← ∅            (trigger가 load면 그 VA를 추가)
  mark[trigger] ← 1

  for i = trigger−1 downto 0:                    # youngest → oldest
      dep ← 0
      for each dest register d of op[i]:
          if d ∈ live_reg:  live_reg ← live_reg \ {d};  dep ← 1
      if op[i] is STORE and op[i].VA ∈ live_mem:
          live_mem ← live_mem \ {op[i].VA};      dep ← 1
      if dep:
          mark[i] ← 1
          live_reg ← live_reg ∪ sources(op[i])
          if op[i] is LOAD:  live_mem ← live_mem ∪ {op[i].VA}
```

한 iteration = RIW entry 한 개 검사 = 사실상 (dest register id) × (live vector) 비교와
bit clear 한 번. 512 entry × H2P 개수를 500 cycle 예산 안에서 처리한다. Store→load
연결은 **committed VA 정확 일치**로 한다 — speculative memory dependence predictor가
필요 없다 (retire 시점이므로 주소가 확정되어 있다).

Snapshot 안에 H2P가 여러 개면 각각 독립적으로 walk한다. 이렇게 해야 여러 slice가 producer를
공유해도 ownership이 보존되고, §7.1의 slice-level RF filtering을 정확히 적용할 수 있다.

### 5.4 Target Load 판정

Walk가 끝나면 `mark[i] = 1 && op[i].mem_type == LOAD`인 모든 op가 **Target Load**다.
판정 결과는 두 곳으로 나뉘어 간다.

1. **Static PC** → Prefetch Table에 소유권 부여 (§6.1). 이미 entry가 있으면 utility를 올려
   eviction으로부터 보호한다. 이것이 PT에 대한 **유일한 allocation 경로**다.
2. **Slice-local 위치** → chain mask / priority-candidate mask의 bit (§5.5).

정의상 Target Load는 "branch operand 생성에 기여한 load"이며, 여기에는 branch condition
값을 읽는 load뿐 아니라 **그 load의 주소를 계산하기 위해 먼저 읽어야 하는 load**(pointer
chasing의 중간 단계)도 포함된다.

> Target Load 판정은 **static PC 입도**로 소비된다. Dynamic 정확도는 93.1% — RF-covered
> load의 93.1%가 실제로 H2P slice 소속으로 확인되었다.

### 5.5 Slice metadata의 block 분해

Slice를 그대로 저장하면 frontend에서 쓸 수 없다. Frontend는 instruction block 단위로
움직이므로 slice를 **block fragment**로 쪼갠다.

```text
  slice (H2P branch까지의 marked op 집합)
      └─ block fragment: { block_start_pc, 64-bit chain mask, 64-bit candidate mask, total_ops }
```

Block = frontend가 fetch한 순차 uop 열로, control-flow instruction에서 끝난다. Bit position
= block 안에서의 uop slot index (0~63). Slot index는 walk가 아니라 **frontend가 그 op를
fetch할 때 매긴 좌표**를 RIW가 그대로 들고 있는 것이다. 그래서 mask의 bit 위치가 다음
occurrence의 frontend 좌표와 정확히 맞는다.

- **chain mask**: slice 소속 op
- **priority-candidate mask**: scheduling 가속 대상 op
  - scope 0 (기본): slice 전체 = chain mask
  - scope 1: 가장 oldest slice member부터 **마지막 Target Load까지**만 — load-to-branch
    tail을 normal priority로 남기고 주소·데이터 경로만 가속하는 변종

### 5.6 Block Metadata Cache (BMC)와 frontend tagging

| 항목 | 값 |
|---|---|
| BMC | 1024 entry, block-start PC indexed |
| Entry | tag(block start PC) + chain mask(64b) + candidate mask(64b) + **effective priority mask**(64b) + total_ops_in_block |
| Empty-block tag store | 256 entry — "이 block에는 chain op가 없다"를 기억해 재조회를 없앤다 |
| Merge 정책 | 여러 slice가 같은 block을 쓰면 mask를 **OR** 누적 |
| Per-slice 보관 | Slice별 chain 사본 1024 entry(H2P PC indexed) — RF filtering의 ownership 계산용 |

**Frontend tagging — cycle 동작:**

```text
  cycle F      : block의 첫 uop가 나오면 block_start_pc로 BMC를 1회 조회한다.
                 (fetch/decode와 병렬. miss면 그냥 normal execution — correctness 영향 없음)
                 hit이면 {chain, candidate, effective, total_ops}를 block latch에 래치
  cycle F..F+k : block 안의 각 uop에 slot index i를 부여하고
                   chain_bit    ← chain_mask[i]
                   priority_bit ← effective_mask[i]      (policy 2, RF filtering 적용)
                                  candidate_mask[i]      (policy 1, filtering 없음)
                 를 uop metadata에 부착한다. i ≥ total_ops이면 부착하지 않는다.
  CF terminator: latch를 무효화한다 (다음 block에서 재조회)
```

부착되는 것은 uop당 2 bit다. 이 bit들은 rename/dispatch/select까지 uop와 함께 흐른다.

**Off-path 처리.** Wrong-path uop에는 tag를 붙이지 않는다 — identification metadata의
consumer가 P-IQ와 통계뿐이고, wrong-path op에 priority를 줄 이유가 없다. (반면 RF prefetch는
off-path에서도 launch한다 — §6.3.)

### 5.7 RF-coverage feedback (effective priority mask 생성)

Walk가 끝날 때마다 slice별로 "이 slice의 Target Load가 전부 RF-covered였는가"를 판정하고
(§7.1), 그 결과로 BMC의 effective mask를 재계산한다.

```text
  for each live slice S:
      for each block fragment f of S:
          if S.priority_enabled:  enabled[f.block]   |= f.candidate_mask
          else:                   suppressed[f.block] |= f.candidate_mask

  for each block B:
      B.effective_mask ← B.candidate_mask & ~(suppressed[B] & ~enabled[B])
```

핵심 invariant: bit는 **그 bit를 소유한 현재 slice가 전부 suppress할 때만** 꺼진다.
RF-uncovered slice가 하나라도 그 producer를 필요로 하면 priority가 유지된다 (shared-producer
safety). Ownership이 불명이거나 evict된 bit는 기본적으로 priority를 유지한다.

---

## 6. Target-Load Register File Prefetch

RFP path는 **retire에서 학습하고, rename에서 발사하고, dcache port의 빈틈으로 probe하고,
load 자신의 AGU에서 검증한다.** 네 시점이 각각 별개의 하드웨어 동작이다.

### 6.1 Prefetch Table (PT)

| 항목 | 값 |
|---|---|
| 구성 | 1024 entry, 8-way set associative, **load PC** indexed |
| Allocation 권한 | **Backward-Walk Engine만** (scope 0). 즉 PT 멤버십 ≡ Target Load 판정 |
| Victim | utility 최소 → 동률이면 LRU |

Entry 필드와 그것을 쓰는 stage:

| 필드 | 폭 | 의미 | Writer |
|---|---:|---|---|
| `valid` / `tag` | 1 / 16b | 이 way가 담당하는 static load PC | **walk** (allocate) |
| `utility` | 2b | 축출 저항력. Target Load로 재확인되거나 stride가 유지되면 ↑, stride가 깨지면 0 | walk + retire |
| `base_va` (+`has_base`) | 48b + 1b | **가장 최근에 retire한** 인스턴스의 주소 | **retire** |
| `stride` (+`has_stride`) | 16b + 1b | 연속 두 retire 주소의 delta | **retire** |
| `confidence` | 1b | 같은 stride가 반복될 때 1/16 확률로 ↑, 깨지면 0. saturate = 발사 자격 | **retire** |
| `inflight` | 7b | rename은 했으나 아직 commit/squash되지 않은 인스턴스 수 | **rename**(+1) / retire·squash(−1) |

`base_va`가 "마지막으로 **retire한**" 주소라는 점이 핵심이다. 아직 in-flight인 인스턴스
`inflight`개가 그 사이에 있으므로, 지금 rename되는 인스턴스의 주소는

```text
        predicted VA = base_va + stride × inflight
```

이다. `inflight`가 없으면 window에 같은 PC가 수십 개 떠 있을 때 전부 같은 주소를 예측하게
된다 (실측: GAP bc의 한 load PC가 동시에 ~49 인스턴스). 7-bit 폭은 이 관측에서 나왔다.

### 6.2 Retire — 주소 학습 (cycle 동작)

Load가 architectural하게 commit되는 cycle:

```text
  1) PT lookup (load PC)                      ── miss면 종료 (scope 0에서는 Target Load가 아님)
  2) inflight--                                ── 이 인스턴스가 확정되었으므로 거리에서 뺀다
  3) delta ← VA − base_va
  4) if delta == stride:                       ── 같은 stride 반복
         with prob 1/16:  confidence++         ── 포화까지 평균 ~17회 연속 반복 필요
         utility++
     else:                                     ── stride가 깨짐
         stride ← delta;  confidence ← 0;  utility ← 0
  5) base_va ← VA
```

Confidence를 확률적으로 올리는 것이 abstain 정책의 실체다. 짧은 규칙성에 속지 않는다.
실측 stride reset 65.9M회 vs confidence saturation 1.77M회 — **37 : 1**로 거절이 압도적이다.
이 비율 자체가 "irregular tail이 실재한다"는 §2.5의 정량 근거다.

> 학습이 walk가 아니라 retire인 이유는 §5.3의 sampling 경고와 같다. Walk는 **어느 PC를
> 추적할지**만 정하고(sampling에 강인한 집합 판정), retire는 **그 PC의 주소 스트림**을
> 학습한다(모든 인스턴스를 봐야 하는 점화식).

### 6.3 Rename — launch (cycle 동작)

Rename cycle R에서 세 가지가 동시에 확정된다: destination physical register(prfid),
older store와의 true dependence, 그리고 이 load의 dynamic identity. 그래서 여기가 발사점이다.

```text
  cycle R:
    (a) destination PRF entry 할당 (prfid 확정)
    (b) memory dependence 확정
    (c) PT lookup (load PC)                              ── 8-way 비교
    (d) inflight++                                        ── off-path/abstain 인스턴스도 포함해서 센다
    (e) 발사 관문 3단:
          gate-1  PT hit                     ← "이 PC는 Target Load인가" (criticality)
          gate-2  older store true-dep 없음   ← 있으면 abstain (§6.7)
          gate-3  confidence 포화             ← "주소 스트림이 규칙적인가" (predictability)
    (f) predicted VA ← base_va + stride × inflight        ── 7-bit 곱셈 (shift-add tree)
    (g) packet {predicted VA, prfid, load id, mem size} → RFP Queue tail
```

**(d)가 관문보다 앞에 있는 것이 의도적이다.** In-flight counter는 "마지막 retire로부터의
거리"를 세는 것이므로, abstain하거나 off-path인 인스턴스도 세어야 이후 모든 예측이 어긋나지
않는다. 실제 하드웨어도 wrong-path allocation을 센다.

**Off-path launch.** Wrong-path load도 발사한다. 하드웨어는 rename 시점에 wrong path임을
알 수 없다. 이것이 실제로 비용인지 측정했고 — off-path probe를 66% 억제하는 오라클이 IPC를
전혀 바꾸지 않았다 — **wrong-path prefetch의 측정 가능한 비용은 0**이다 (§12).

### 6.4 RFP Queue와 L1 port 중재 (cycle 동작)

| 항목 | 값 |
|---|---|
| 구성 | 64 entry FIFO |
| Drain width | cycle당 최대 2 packet |
| Port 정책 | **demand load가 쓰고 남긴 L1 read port만** 사용 (최저 우선순위) |
| Head-of-line | port를 못 얻으면 head를 유지하고 다음 cycle 재시도 (paper의 oldest-first 순서 보존) |

매 dcache cycle, demand load들의 port 중재가 끝난 **뒤에** drain이 돌며 head부터 검사한다:

```text
  for 최대 2 packet, head부터:
      ① 소유 load가 squash되었는가        → tombstone, 폐기
      ② 소유 load가 이미 AGU에 도달했는가  → 숨길 latency가 없음, 폐기
      ③ (선택) queue 체류 시간 초과        → 폐기
      ④ predicted VA가 store queue의 미완료 store와 겹치는가 → 폐기 (§6.7)
      ⑤ 해당 bank의 L1 read port 획득 시도  → 실패면 이번 cycle 중단 (head 유지)
      ⑥ L1 tag+data read (probe)
             hit  → data_ready ← now + L1_hit_latency(5)
             miss → §6.5
```

`RFP_QUEUE_ENTRIES`는 제약이 아니다: 실측 평균 점유 1.35/64, full 0.06% cycle.
launch→probe 평균 지연 **1.95 cycle** (on-path probe 81.5M회 기준).

**Replacement 갱신.** Probe는 L1 replacement state를 갱신한다. Covered load는 자기
demand access를 생략하므로 probe가 그 touch를 **대체**하는 것이고, 이렇게 해야 L1 접근
횟수가 baseline과 보존된다 (bandwidth-neutral).

**대역폭은 제약이 아니다.** L1 read port 사용률은 demand 기준 33%로 2/3가 유휴다.
전용 read port를 추가하는 변종을 만들어 측정했더니 coverage +1.9%p에 IPC는 +0.03%p —
**negative result로 논문에 싣는다** (§12).

### 6.5 L1 miss → lower-level fill (fill path)

Probe가 L1에서 miss하면 **demand miss와 동일하게** 하위 계층으로 진행한다.

```text
  MSHR 할당 시도 → 실패면 폐기 (load가 스스로 line을 가져온다)
  성공하면 line-granular request를 MLC → LLC → DRAM으로 발행
  fill 도착 시:
      ① L1에 line을 채운다 (정상 fill 경로 — pollution/coalescing/MSHR 점유가 전부 측정에 반영)
      ② 요청한 load가 아직 살아 있고 아직 AGU에 도달하지 않았으면 data_ready ← fill cycle
```

②의 조건이 실제로 성립하는 경우는 드물다 — 요청의 **96%는 load가 fill보다 먼저 AGU에
도달**한다. 즉 이 경로는 실질적으로 **순수 cache prefetch**로 동작하며, 이득은 RF 전달이
아니라 **load가 L1에서 hit하게 되는 것**에서 나온다. 그럼에도 이 경로가 RFP 이득의 지배
성분이다 (§2.6).

### 6.6 AGU — validate-then-use (cycle 동작)

Load가 주소 계산을 마치고 dcache stage에 처음 도달하는 cycle을 V라 한다. **여기가
validation 지점이며, 검증을 위한 두 번째 cache access는 없다.**

```text
  cycle V:
    ① predicted VA == 실제 VA ?                → 불일치면 prefetch 폐기, 정상 demand 경로
                                                  (낭비된 것은 probe 1회의 L1 접근뿐)
    ② store queue에 겹치는 미완료 store ?       → 있으면 폐기, 정상 demand 경로
    ③ 데이터가 도착했거나 도착 예정인가 ?        → 아니면 정상 demand 경로

    ④ timeliness 산술:
          deliver     = max(data_ready, V + RF_hit_latency)        , RF_hit_latency = 1
          demand_done = V + L1_hit_latency + extra_ld_latency      , L1_hit_latency = 5
          if deliver ≥ demand_done:  이득 없음 → 정상 demand 경로 (LATE)
          saved = demand_done − deliver

    ⑤ covered:  load는 **cache에 접근하지 않고** 완료된다 (probe가 그 접근을 이미 썼다)
                done/wake cycle ← deliver
                dependent wakeup 브로드캐스트
```

분류:

| 분류 | 조건 | 의미 |
|---|---|---|
| **FULL** | `data_ready ≤ V` | AGU 도달 시점에 데이터가 이미 register file에 있다 |
| **PARTIAL** | `V < data_ready < demand_done` | 아직 도착 중이지만 demand access보다는 빠르다 |
| **LATE** | `data_ready ≥ demand_done` | 늦어서 이득이 없다 |

**Validate-then-use의 가장 중요한 성질**: prefetch가 틀려도 **demand path가 늦어지지 않는다.**
Prefetch는 load의 정상 실행을 대체할 뿐 앞을 막지 않는다. 그래서 오예측의 비용은
"낭비된 probe 1회"가 전부이고, flush나 replay가 없다. 실측 `demand delayed by prefetch = 0`이
이 invariant의 증명이다.

### 6.7 Store 처리 — 보수적 abstain

RFP 논문은 store data를 prefetch에 forwarding한다(§3.2.1). ZERECO는 그 대신 **abstain**한다:
rename에서 true store dependence가 보이면 발사하지 않고, probe/validate 시점에도 store queue를
스캔해 겹치면 폐기한다.

| 하드웨어 케이스 | 처리 | 근거 |
|---|---|---|
| older store 있으나 주소 미확정 | **abstain** | 보수적. 실제 하드웨어는 MD predictor로 통과시킬 수 있으나 오판 시 flush |
| 주소 일치, store data 아직 미완료 | abstain | 논문은 store를 기다렸다 forwarding |
| 주소 일치, store data 확정 | abstain | 논문은 즉시 forwarding — **여기가 우리가 포기한 이득** |
| older store 없음 | 정상 발사 | |

포기 모집단: PT-hit load의 **10.6%** (전체 load의 6.3%), 그중 store data가 이미 확정되어
즉시 forwarding 가능한 것은 **12.1%**. 이득 상한이
작아 현재는 abstain을 유지하고 모집단을 보고한다.

### 6.8 Squash와 recovery

```text
  branch misprediction / exception:
      squash된 load마다 PT.inflight--          ← "counter is decremented for each squashed load"
      해당 packet은 queue에서 tombstone 처리
      destination PRF는 정상 rename recovery로 회수 (prefetch가 추가 제약을 만들지 않는다)
```

Persistent state(HBT / slice metadata / PT의 base·stride·confidence)는 **committed
instruction으로만** 갱신된다. Squash는 inflight counter만 되돌린다.

### 6.9 왜 RF path의 이득이 load당 4 cycle에 묶이는가 (핵심 timing 분석)

`deliver ≥ V + 1`이고 `demand_done = V + 5`이므로

```text
        saved = demand_done − deliver = min(V − P, 4)          , P = probe cycle
```

즉 **RF path의 load당 이득 상한은 L1 hit latency − RF hit latency = 4 cycle**이다. 실측 평균
2.70 cycle. 그리고 FULL이 되려면 `data_ready = P + 5 ≤ V`, 즉

```text
        V − P ≥ 5   ⟹   V − R ≥ 5 + 1.95 ≈ 7 cycle          (rename→AGU 창)
```

**rename→AGU 창의 유도.** 이 창은 직접 계측하지 않았고 timeliness 산술에서 역산한다
(카운터 값은 전부 `rfp_only`, 260818):

| 분류 | 계측된 양 | 유도 |
|---|---|---|
| PARTIAL (47.37M) | `ready − V` 평균 2.91 cy | `V − P = 5 − 2.91 = 2.09` → **`V − R ≈ 4.0 cy`** |
| FULL (21.83M) | `V − ready` 평균 18.55 cy | `V − P = 23.55` → **`V − R ≈ 25.5 cy`** |

교차검증: `21.83M × 4 + 47.37M × 2.09 = 186.3M` vs 실측 `saved` 총합 `186.5M` — 일치한다.

**이 창이 예측 가능성과 역상관한다.** Confidence 포화는 주소가 `base + i × stride`인 load를
고르는데, 그 주소는 1-cycle induction 재귀가 만든다. Induction 체인은 rename 전진보다 빠르므로
(bc 실측: 루프 10.4 uop / rename 8-wide ⇒ ~1.3 cycle에 한 iteration을 rename하는 동안 induction은
1 cycle에 한 iteration 생산) **RFP-eligible load는 rename 시점에 주소 operand가 이미 ready**다.
그러면 rename→AGU 창은 scheduling pipe 깊이뿐(유도값 4.0 cycle)이라 7 cycle 요구조건에
구조적으로 미달한다 → PARTIAL.

반대로 창이 긴 load(유도값 25.5 cycle)는 주소가 **다른 load의 결과**로 계산되는 load인데,
그런 주소는 iteration의 affine 함수가 아니므로 stride가 깨지고 confidence 관문에서 탈락한다.
실측 stride reset : confidence 포화 = 65.88M : 1.77M = **37 : 1**이 이 탈락의 정량이다.

실측 FULL : PARTIAL = 21.83M : 47.37M = **1 : 2.17**. RFP 논문의 63% operand-not-ready /
37% ready 분포가 우리 쪽에서 뒤집혀 있는 것은 **eligibility filter가 operand-ready subset을
고르기 때문**이다. 이 역상관은 자연법칙이 아니라 **stride/PC-local 예측기 계열의 구조적
성질**이며, data-dependent 주소를 맞히는 예측기(indirect prefetcher 계열)라면 깨진다 —
related work와의 경계를 긋는 데 쓸 수 있는 관찰이다.

> **직접 확증이 필요하면**: rename hook에서 "모든 source operand ready" 비율을 전체 load와
> PT-eligible load로 나눠 세면 된다(카운터 2개). RFP 논문 §3.2의 63/37 분포와 직접 대응하며,
> 가설의 예측은 "eligible load의 operand-ready 비율이 전체 load보다 뚜렷이 높다"이다.

> **이 분석이 §2.6의 "두 경로" 서사를 정당화한다.** RF path는 구조적으로 load당 ≤4 cycle에
> 묶여 있고, fill path는 그 상한이 없다. 같은 예측기로 두 경로를 다 쓰는 것이 옳은 설계다.

### 6.10 End-to-end 예시 (cycle table)

L1 hit인 Target Load 하나. §6.9에서 유도한 평균값을 그대로 넣었다.

| Cycle | RF path (PARTIAL, 전형) | 관여 구조 |
|---:|---|---|
| R+0 | PT hit → inflight 1→2 → `pred = base + 2×stride` → packet {pred, prfid} enqueue | PT, RFP Queue |
| R+2 | 남은 L1 read port 획득, tag+data read → **hit** → `data_ready = R+7` | RFP Queue, L1D |
| R+4 | load의 주소 operand ready → AGU → **V = R+4** | RS/AGU |
| R+4 | validate: 주소 일치, store 충돌 없음. `deliver = max(R+7, R+5) = R+7`, `demand_done = R+9` → **saved 2 cycle (PARTIAL)** | validation logic |
| R+7 | destination PRF write, dependent wakeup 브로드캐스트 | PRF, wakeup |
| — | baseline이었다면 R+9에 wakeup | |

| Cycle | Fill path (L1 miss) | 관여 구조 |
|---:|---|---|
| R+0 | 동일 | PT, RFP Queue |
| R+2 | probe → **miss** → MSHR 할당, MLC 요청 | MSHR |
| R+18 | MLC fill 도착 → L1에 line 삽입 | L1D fill |
| R+n | load의 AGU 도달. 대부분은 fill보다 늦으므로 RF 전달은 없고 **L1 hit**으로 완료 | — |
| — | baseline이었다면 MLC까지 16 cycle을 추가 지불 | |

실측으로 fill path 요청 18.01M건 중 RF 전달까지 성공한 것은 1.85%뿐이고, 나머지는 load가
fill보다 먼저 도착하거나 다른 요청과 coalesce된다. 그럼에도 이 경로가 이득의 지배 성분인
이유는 이득의 크기가 다르기 때문이다 (§2.6).

## 7. RF-Filtered Priority Issue Queue (P-IQ)

### 7.1 Slice-level RF filtering

RF filtering은 개별 load가 아니라 **H2P slice 단위**로 한다. Slice 안의 Target Load 하나만
uncovered여도 branch resolution은 여전히 지연되기 때문이다.

```text
  walk 완료 시, 그 slice의 Target Load들의 RF-coverage 결과로:
      Target Load 없음        → priority 유지 (RF가 처리할 수 없는 slice)
      하나라도 uncovered      → priority 유지, streak ← 0
      전부 covered            → streak++ ; streak ≥ 2 이면 priority 억제
```

Filtering은 **retroactive하지 않다.** 이미 backend에 들어간 op의 priority를 바꾸지 않고,
다음 occurrence에만 반영한다. RF-coverage 주장 자체도 별도 관문이 있다: latency 이득은 항상
취하되, "covered"라고 **주장**하려면 saved cycle이 문턱을 넘어야 한다 (한계적으로만 도움받은
load는 여전히 critical path에 있으므로 slice의 priority를 뺏으면 안 된다).

> ⚠ **실험 결과 이 mechanism은 기각 후보다.** 후보를 9.1% 덜어내도 fallback률(11.1% → 11.4%)과
> partition-full cycle이 사실상 불변이고 IPC는 오히려 소폭 낮다(+6.22% → +6.15%). §7.3의
> bounded non-stall partition이 이미 인구 자기조절을 수행하기 때문이다. 결정은 계획서 D-1.

### 7.2 Partitioned issue queue

각 distributed issue queue / reservation station bank는 entry를 두 class로 나눈다.

| 항목 | 값 |
|---|---|
| 예약 비율 | main capacity의 `P%` (실험 기본 20%) |
| Golden Cove 구성 | RS0 285 / RS1 204 / RS2 55 → 20%에서 57 / 41 / 11 = 109 of 544 |
| Admission | Priority op는 Priority 구획에만, Normal op는 Normal 구획에만 |
| 제약 | FU compatibility는 두 class 모두에서 보존 |

**하드웨어 구현은 PUBS의 free-list 분할이다.** IQ free-list를 두 구간으로 나누고 Priority op에
낮은 physical entry ID를 할당하면, 기존 fixed-priority encoder(낮은 ID 우선)가 **select logic
수정 없이** class 우선순위를 그대로 구현한다. 매 cycle 전체 queue를 priority bit로 정렬할
필요가 없다. 추가되는 것은 class별 occupancy counter와 admission gating뿐이다.

### 7.3 Non-stalling admission

Strict partition은 scheduling optimization을 dispatch bottleneck으로 바꾼다. ZERECO는
**one-way non-stalling fallback**을 쓴다.

```text
  Priority candidate + 호환 Priority 공간 있음  → Priority 구획 진입, priority 유지
  Priority candidate + Priority full + 호환 Normal 공간 있음
                                                → Normal 구획으로 fallback, **priority 영구 상실**
  어느 쪽도 공간 없음                            → 통상적인 in-order dispatch stall

  Normal op는 절대 Priority 구획을 빌리지 않는다 (one-way)
```

Fallback이 priority를 영구히 잃는 것이 중요하다. 이것이 없으면 Priority class가 결국
전체 queue가 되어 partition이 무의미해진다. 실측 fallback률 11~12% — 즉 **partition은
bursty하게만 압력을 받고**, 그 순간에만 인구를 스스로 깎아낸다. 이 자기조절이 §7.1의
filtering을 불필요하게 만드는 메커니즘이다.

### 7.4 Priority select (cycle 동작)

```text
  cycle D  (dispatch): ROB → IQ. class 결정 + partition admission + (필요시) fallback
  cycle W  (wakeup)  : producer 브로드캐스트로 ready
  cycle S  (select)  : 각 FU의 selector가 ready 후보 중 하나를 고른다
                         비교 순서:  ① Priority class 여부
                                     ② 같은 class 안에서는 baseline 정책
                                          - oldest-first  : 낮은 op 나이
                                          - random-physical: 낮은 physical entry ID (PUBS 대표 조직)
```

Selector가 보는 것은 entry의 class bit 하나뿐이다(또는 §7.2의 free-list 분할이면 그것도 불필요).

### 7.5 P-IQ headroom은 scheduler 조직에 의존한다

| 조직 | RFP 단독 | +P-IQ 상한(무제한 marking) | +P-IQ 20% partition | partition 보존율 |
|---|---:|---:|---:|---:|
| oldest-first | +5.90% | +6.43% | +6.15% | 52% |
| random-queue (PUBS 대표) | +6.75% | +9.40% | +8.95% | **83%** |

Age-based selector는 이미 older producer를 우대하므로 P-IQ가 바꿀 winner가 적다. Age
advantage가 없는 조직에서는 slice priority가 select 대기를 2.42 → 0.63 cycle로 줄인다.
이것은 "P-IQ가 모든 scheduler보다 낫다"는 주장이 아니라, **P-IQ의 causal potential과
그 조건**을 보여주는 것이다.

### 7.6 Interference — 알려진 부작용

DRAM bandwidth-bound workload(pr)에서 oldest-first + P-IQ는 −1.3%p다. Chain op 우대가
MLP를 만드는 독립 miss load를 select에서 밀어내기 때문이다(normal displaced ~80M op-cycle).
Partition으로도 filtering으로도 풀리지 않는다. PUBS 자신이 인정한 "scheduling은 memory
latency를 없앨 수 없다"의 실측판이며, limitation으로 서술한다.

---

## 8. 하나의 dynamic H2P occurrence — 전체 timeline

Target Load 두 개를 `TLa`, `TLb`라 한다 (L1/L2는 cache level을 뜻하므로 구분한다).

```text
[과거]  ── HBT counter > 1 로 branch B가 H2P로 분류
        ── RIW가 가득 차며 B의 오래된 인스턴스가 밀려남 → BWE trigger
        ── 500 cycle walk: B의 backward slice 복원, Target Load {TLa, TLb} 판정
              · TLa, TLb의 PC → Prefetch Table 소유권
              · slice → block fragment mask → Block Metadata Cache에 OR 누적
        ── TLa, TLb가 retire할 때마다 base/stride/confidence 학습 (수십~수백 인스턴스)

[현재 occurrence]
  F      : frontend가 block-start PC로 BMC 조회 → slice mask 래치
           slice 소속 uop에 chain/priority bit 부착
  R      : TLa rename — prfid 할당, PT hit, inflight++, 관문 3단 통과
           packet {base + stride×inflight, prfid} → RFP Queue
  R+2    : 남은 L1D read port로 probe
              hit  → data_ready = R+7
              miss → MSHR → MLC/LLC/DRAM → L1D fill
  D      : slice op들이 dispatch — priority bit가 있으면 Priority 구획으로,
           꽉 찼으면 Normal 구획으로 fallback (priority 영구 상실)
  S      : ready가 된 slice op들이 selector에서 non-slice op보다 먼저 선택됨
           → 주소 계산 producer가 앞당겨짐 → TLa의 AGU가 앞당겨짐
  V      : TLa가 AGU 도달 → validate → covered → deliver 시점에 dependent wakeup
  ...    : TLb도 동일 경로. 두 load가 모두 앞당겨지면 branch operand가 앞당겨짐
  B_exec : branch B가 조기에 resolve → misprediction이면 recovery가 그만큼 일찍 시작
  B_ret  : B가 retire → HBT 갱신, RIW 기록, (trigger되면) 다음 walk
           TLa/TLb가 retire → PT 주소 학습, inflight--
           slice의 RF-coverage 결과 → 다음 occurrence의 effective priority mask
```

세 엔진이 서로 다른 시간축에 산다는 점이 중요하다. Identification은 **과거의 committed
execution**에서 학습하고, delivery는 **현재 occurrence의 rename~AGU 구간**에서 동작하며,
scheduling은 **현재 occurrence의 dispatch~select 구간**에서 동작한다. 세 축이 겹치지 않으므로
어느 것도 다른 것을 stall시키지 않는다.

---

## 9. Storage Budget과 Timing/Energy

### 9.1 Storage

| 구조 | 구성 | Entry 폭 | 소계 |
|---|---|---:|---:|
| H2P Branch Table | 1024 × direct-mapped | 16b tag + 3b counter | ~2.4 KB |
| Retired Instruction Window | 512 uop | PC 좌표 + src/dest reg id + type + VA + H2P bit ≈ 16 B | 8 KB |
| Backward-Walk Engine | live-in reg bit vector 2×64b + memory live-in list + snapshot buffer(RIW와 공유) | — | < 1 KB |
| Block Metadata Cache | 1024 × (chain + candidate + effective mask) | 16b tag + 3×64b + 6b | ~25 KB |
| Empty-block tag store | 256 | 16b tag | 0.5 KB |
| Per-slice chain 사본 | 1024 × block fragment 목록 | ownership 계산용 | (설계 선택; 압축 여지 있음) |
| **Prefetch Table** | 1024 × 8-way | 16b tag + 48b base + 16b stride + 1b conf + 2b util + 7b inflight + 4b valid/has ≈ 12 B | **12 KB** |
| RFP Queue | 64 | 48b VA + 10b prfid + 8b id + 3b size ≈ 9 B | 0.6 KB |
| P-IQ occupancy counter | RS당 2 × 9b | — | 무시 가능 |
| Per-uop bit | ROB/IQ entry당 chain/priority/piq-entry/fallback + **RFP-inflight 1b** | 5b | 512 × 5b ≈ 0.3 KB |

**총계 ≈ 50 KB.** 지배 성분은 Block Metadata Cache + RIW(= identification 경로)이고, 이는
TEA에서 상속한 구조다. 논문에서는 (a) 이 비용이 identification 계열 선행연구와 같은 급이며,
(b) 실제 mechanism 고유 비용은 PT 12 KB + queue 0.6 KB임을 분리해 서술해야 한다.

### 9.2 Timing

| 경로 | 요구 | 근거 |
|---|---|---|
| BMC lookup | instruction block delivery와 **병렬**, miss면 normal execution | correctness-critical frontend path를 늘리지 않는다 |
| PT lookup | rename 단계에 8-way 비교 1회 | rename은 이미 RAT 다중 포트 접근을 하는 단계 |
| 주소 산술 | `base + stride × inflight`, inflight ≤ 127 | 7-bit 곱셈 = shift-add tree, rename cycle 안에서 처리 가능 |
| RFP probe | demand보다 **낮은** 우선순위 | 절대 demand를 지연시키지 않는다 (실측 0) |
| P-IQ select | class bit 1개의 우선 비교, 또는 free-list 분할로 **selector 무수정** | §7.2 |
| BWE walk | retire side, 500 cycle 비동기 | main pipeline과 무관 |

### 9.3 Energy / bandwidth

RFP는 translation, L1 read port, PRF write bandwidth를 소비한다. 상쇄 요소는
**covered load가 자기 L1 access를 생략한다**는 것 — probe가 그 access를 대신하므로 정확한
prefetch는 L1 접근 수에 중립이다. 순증가는 (a) 주소가 틀린 prefetch의 probe와 (b) off-path
prefetch뿐이다. 실측 L1 read port 사용률은 demand 33%, off-path probe 비율 **64.1%**지만
baseline 기계 자체가 off-path 58.9%라 초과분은 ~5%p이며, 유휴 port로 흡수되어 IPC 영향이
측정되지 않는다.

---

## 10. Correctness Invariants

하드웨어가 반드시 보장해야 하는 것:

| Invariant | 내용 |
|---|---|
| **Committed training** | Squashed instruction은 HBT, slice metadata, PT의 base/stride/confidence를 갱신하지 않는다. Squash는 inflight counter만 되돌린다. |
| **One prediction per instance** | Port 중재 실패로 재시도해도 한 dynamic load는 예측을 한 번만 받고 학습에 한 번만 기여한다. |
| **In-flight 정확성** | Rename에서 +1, retire 또는 squash에서 정확히 −1. Abstain·off-path 인스턴스도 센다. Entry가 축출되면 그 카운트는 소멸하되 underflow는 없어야 한다. |
| **PRF lifetime safety** | Packet이 완료 또는 squash될 때까지 destination mapping이 유효해야 한다. |
| **Validate-then-use** | 주소 일치 + memory order 합법이 확인된 value만 accept한다. Prefetch는 **demand path를 지연시키지 않는다** (측정치 0). |
| **Shared-slice safety** | 어느 active RF-uncovered slice도 필요로 하지 않을 때만 shared producer의 priority를 제거한다. |
| **Compatible admission** | P-IQ와 fallback placement는 FU connectivity를 보존한다. |
| **One-way partition** | Normal op는 Priority 구획을 사용하지 않는다. Fallback한 op는 priority를 영구히 잃는다. |
| **Metadata aging** | HBT decay + PT utility + BMC replacement가 obsolete state의 영구 marking을 막는다. |

무결성 카운터 6종(shadow-selection / physical-entry / partition / non-stall / demand-delayed /
stale-deref)이 전 실험·전 config에서 0임을 확인했다.

---

## 11. Hardware ↔ Simulator 대응

이 절만이 시뮬레이터 관점이다. **논문 본문에는 왼쪽 열을 쓰고, Methodology에 오른쪽 열의
근거를 한 문단으로 명시한다.**

### 11.1 의도적 모델링 대체

| 하드웨어 원형 | 시뮬레이터 구현 | 정당화 |
|---|---|---|
| **RFP-inflight bit + speculative wakeup 정렬** (IQ entry당 1b) — consumer가 prefetch 성공을 가정하고 깨어나고, 실패 시 selective replay | `done_cycle` 산술로 등가 처리 | Scarab의 wakeup은 "결과 확정 후 미래 ready cycle 통보"라 정렬 대상이 없다. Steady state의 타이밍은 동일하고 baseline에도 같은 규칙이 적용되므로 공정하다. **논문 mechanism·storage 절에는 원형을 기술한다.** |
| **store → prefetch data forwarding + MD predictor + 오판 flush** | **abstain** (rename에서 dep 검사 + probe/validate에서 store queue 스캔) | 시뮬레이터가 rename에서 완전한 memory disambiguation을 제공해 MD 오판이 구조적으로 발생 불가. 포기 모집단(PT-hit load의 10.6%)을 숨기지 않고 보고한다. |
| **wrong value / stale value 주입과 recovery** | 검출만 하고 주입하지 않음 (store write 타임스탬프 테이블) | 값이 trace에서 오므로 잘못된 값을 실행에 흘릴 수 없다. "실제 하드웨어라면 recovery가 필요했을 창"의 빈도를 세는 것으로 대체 — 현재 useful의 8.6%, 단 **line(64B) 입도 상한**이며 byte-overlap 정제가 필요하다(D-4). |
| **DTLB port / translation miss drop** | 모델 없음 (L1을 VA로 접근) | Baseline도 동일 조건. 하드웨어 서술에는 "DTLB miss 시 drop"을 원형으로 포함한다. |
| **PT storage 압축, context predictor** | 생략 | timing 무관 / 우리 구성에서 이득 미미. Storage 계산에만 인용한다. |
| **Backward walk의 per-op 파이프라인** | 고정 500-cycle 점유 | Walk는 retire side라 timing에 영향이 없고, 500 cycle의 gating 효과(sampled training)는 오히려 보수적이다. |
| **Off-path covered fast path** | 미적용 | oracle 경로와 동일 기준을 유지해 비교 가능성을 지킨다. 대역폭 통계는 off-path를 포함한다. |

### 11.2 구조 ↔ 코드

| 하드웨어 구조 | 코드 |
|---|---|
| H2P Branch Table | `bp/hbt.c` (1024 entry, 3-bit, threshold >1, decay 50K) |
| Retired Instruction Window | `fill_buffer.c` (`FILL_BUFFER_SIZE` 512) |
| Backward-Walk Engine | `dependency_chain_cache.c: build_dependency_mask_for_target()` (`BACKWARD_WALK_CYCLES` 500) |
| Target Load 판정 → PT 소유권 | `dependency_chain_cache.c: commit_dependency_chain_entry()` → `rfp_note_target_load()` |
| Block Metadata Cache | `dependency_chain_cache.c: commit_block_cache_masks()` (`BLOCK_CACHE_SIZE` 1024) |
| Effective priority mask 재계산 | `dependency_chain_cache.c: rebuild_iq_priority_masks()` |
| Frontend tagging | `decoupled_frontend.cc: apply_main_chain_block_tag()` |
| Prefetch Table | `zereco/rfp.c` PT 절 (`RFP_PT_ENTRIES` 1024, `RFP_PT_ASSOC` 8) |
| Retire 학습 | `rfp_retire_train()` ← `node_stage.c` |
| Rename launch | `rfp_rename_launch()` ← `map_stage.c` (prfid 확정 직후) |
| RFP Queue / L1 중재 | `rfp_queue_drain()` ← `dcache_stage.c` (demand loop 뒤) |
| Fill path | `rfp_send_to_lower_levels()` / `rfp_fill_done()` |
| Validate-then-use | `rfp_try_validate()` ← `dcache_stage.c` (port 검사 앞) |
| Squash 시 inflight 회수 | `rfp_note_op_freed()` ← `op_pool.c` |
| P-IQ partition 크기 | `exec_ports.c` (`ZERECO_PIQ_ENTRY_PERCENT`) |
| P-IQ admission / fallback | `node_issue_queue.cc: node_issue_queue_dispatch()` |
| Priority select 비교기 | `node_issue_queue.cc: node_issue_queue_precedes()` |
| H2P resolution profiler | `zereco/h2p_mispred_latency.c` |

### 11.3 주요 파라미터 기본값

```text
  Prefetch Table   : 1024 entry / 8-way,  confidence 1 bit (p = 1/16)
  RFP Queue        : 64 entry,  drain 2/cycle,  RF hit latency 1 cycle
  Scope            : 0 (Target Load만; 1 = vanilla RFP)
  L1 miss policy   : 1 (하위 계층 진행)          ← drop 대비 +4.15%p
  Port priority    : 0 (demand가 남긴 port)      ← 전용 port는 negative result
  Port fail policy : 0 (head 유지, 다음 cycle 재시도)
  P-IQ             : 예약 20%,  non-stall fallback,  select-priority on
  Identification   : HBT 1024/3b, RIW 512, walk 500 cycle, BMC 1024
```

---

## 12. 실측 특성 요약

상세 표와 실험 이력은 [zereco_RFP_IMPLEMENTATION_PLAN.md](zereco_RFP_IMPLEMENTATION_PLAN.md) §4~5.
이 절은 §1~§10의 설계 주장에 대응하는 근거만 모은다.

| 설계 주장 | 실측 |
|---|---|
| dependency가 지배 성분 (§2.3) | 29.94 / 42.02 = **71%** |
| branch 단독 priority는 무의미 (§2.3) | H2P branch의 select 대기 **0.10 cycle** |
| Target Load latency가 causal (§2.4) | RFP 적용 시 dependency 29.94 → 25.50, f→r 42.02 → 37.52, IPC +5.90% |
| predictable subset이 실재하되 부분적 (§2.5) | address accuracy **92.7%**, 최종 coverage 19.9%(전체 load 분모), stride reset : conf 포화 = **37 : 1** |
| criticality 선별의 예측기 효율 (§6.1) | 동등 성능에 PT 압력 **122×** 차이 (scope 0: 155K alloc / scope 1: 18.9M, 스래싱) |
| Target Load 선별의 정확도 (§5.4) | covered load의 **93.1%**가 H2P slice 소속 |
| 깔때기 (전체 load 분모) | PT-hit 59.2% → injected 29.0% → executed 21.5% → **useful 19.9%** |
| fill path 지배 (§2.6) | drop +1.75% vs 진행 +5.90%, L1D read miss 126.0M → 95.8M (−24%) |
| RF path의 4-cycle 상한 (§6.9) | FULL : PARTIAL = 21.83M : 47.37M = **1 : 2.17**, 평균 saved **2.70** cycle, rename→AGU 4.0(PARTIAL) / 25.5(FULL) *(유도값)* |
| validate-then-use의 무해성 (§6.6) | demand delayed by prefetch = **0** |
| 대역폭은 제약 아님 (§6.4) | 전용 port: coverage +1.9%p, IPC **+0.03%p** (negative result) |
| wrong-path prefetch 비용 없음 (§6.3) | probe 66% 억제 오라클이 IPC **완전 동일** |
| queue는 제약 아님 (§6.4) | 평균 점유 1.35/64, full 0.06% cycle |
| bounded partition이 filtering을 포섭 (§7.1) | 후보 −9.1%에도 fallback률 11.1 → 11.4%, IPC +6.22 → +6.15% |
| P-IQ headroom의 조직 의존성 (§7.5) | 순수 몫 OF +0.24~0.46% vs randq **+2.06~2.48%** |
| RFP의 2차 효과 | RS 점유 완화가 select 대기 단축 (randq 2.42 → 1.66 cycle) |
| interference (§7.6) | pr (DRAM BW-bound)에서 OF+P-IQ −1.3%p |

**Profiler 정의** (§2.3 표의 출처): H2P branch가 exec에서 recovery를 일으킨 event마다
`frontend = ROB allocation − fetch`, `dependency = 마지막 operand ready − ROB allocation`,
`scheduler = FU select − operand ready`, `execution = outcome 확정 − select`를 기록하고,
recovery 이후 첫 correct-path fetch까지를 별도로 계측한다.

> **계측 주의 (2026-08-19 확인).** `RFP_LAUNCH_TO_PROBE_AVG`의 분모가 `RFP_EXECUTED`
> (= on-path L1 **hit** probe)로 되어 있어, 분자(on-path probe 전체 = hit + miss)와
> 모집단이 어긋난다. `l1_miss_policy 1`에서 on-path probe의 8.3%가 miss이므로 보고값이
> 2.12로 부풀려진다. 본문의 **1.95 cycle**은 `LAUNCH_TO_PROBE_TOTAL / (on-path probe)`로
> 다시 계산한 값이다. 시뮬레이션 타이밍에는 영향이 없는 순수 보고 문제이나, 다음 sweep 전에
> 분모를 on-path probe 카운터로 고쳐야 한다.

> **1차 지표는 H2P resolution latency다.** Claimed saved cycle의 13%만 총 cycle 감소로
> 실현된다 — OoO가 나머지를 흡수하고, 이득은 해당 branch가 실제로 틀린 경우에만 발현된다.
> IPC는 약한 대리 지표이므로 resolution latency를 primary로 보고한다.

---

## 13. Scope

### 13.1 ZERECO가 제안하는 것

Committed H2P-slice learning + criticality 기반 Target-Load prefetch(두 delivery path) +
bounded non-stalling priority scheduling을 결합한 **main-thread branch-resolution architecture**.
목적은 branch operand를 더 일찍 ready로 만들어 direction/target error를 조기에 드러내는 것이다.

### 13.2 ZERECO가 제안하지 않는 것

- 더 크거나 새로운 branch-direction predictor가 아니다.
- Helper-thread architecture가 아니며 branch slice를 duplicate execution하지 않는다.
- General-purpose cache prefetcher가 아니다 — request 선택 기준이 branch-resolution criticality다.
- Pure value prediction이 아니다 — predicted address에 대해 coherent memory hierarchy에서 값을 가져온다.
- P-IQ가 모든 scheduler보다 우수하다는 주장이 아니다 (§7.5).

### 13.3 남은 open item

계획서 §6의 D-1~D-5, S-1~S-5. 특히 아키텍처 서술에 직결되는 것:

1. **RF filtering의 거취** (D-1) — bounded partition이 포섭한다면 §7.1은 ablation/negative
   result로 재배치하고, `covered_min_saved_cycles`와 coverage feedback loop의 소비자가
   사라진다는 점을 함께 정리해야 한다.
2. **stale-value 창의 byte 입도 정제** (D-4) — 현재 8.6%는 line 입도 상한이다.
3. **PT 크기 × scope sweep** (S-1) — "vanilla RFP가 몇 K entry를 써야 ZERECO 1K와 같아지는가"가
   complexity 주장의 본 그림이다.
4. **partition 예약률 sweep** (S-3) — 제안 설계의 유일한 자유 파라미터.

---

## References

- **PUBS**, "Performance Improvement by Prioritizing the Issue of the Instructions in Unconfident Branch Slices," MICRO 2018. [PDF](</home/lee/scarab/reference/[2018, MICRO] PUBS.pdf>)
- **Branch Runahead**, "Branch Runahead: An Alternative to Branch Prediction for Impossible to Predict Branches," MICRO 2021. [PDF](</home/lee/scarab/reference/[2021, MICRO] Branch Runahead_Pruett,Y.Patt.pdf>)
- **RFP**, "Register File Prefetching," ISCA 2022. [PDF](</home/lee/scarab/reference/[2022, ISCA] Reg File prefetching.pdf>)
- **TEA**, "Timely, Efficient, and Accurate Branch Precomputation," MICRO 2024. [PDF](</home/lee/scarab/reference/[2024, MICRO] Timely_Efficient_and_Accurate_Branch_Precomputation.pdf>)
