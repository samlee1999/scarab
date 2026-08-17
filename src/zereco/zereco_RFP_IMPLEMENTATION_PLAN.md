# ZERECO Timed RFP Implementation Plan

> Last updated: 2026-08-14
> 근거: `[2022, ISCA] Reg File prefetching.pdf` 전체 재독 (본 문서의 §, Fig, 수치는 모두 해당 논문 기준)
> 관련 문서: `zereco_REFERENCE_NOTES.md` §1 (RFP 노트), `zereco_ARCHITECTURE.md` §6 (Target-Load RF Prefetching)
> 상태: **계획 v5 — Phase 0 구현 완료, Phase 1 대기**
> v5 변경: **실험 축을 설계 시점에 전부 파라미터로 개방** (§3.10 sweep matrix) — port 실패
> 정책/port 우선순위/queue 수명 추가, resource-contention 계측 확장, **값 corner case의
> 측정 가능 범위 정리** (§2.7)
> v4 변경: **훈련 경로를 Fill Buffer/BW walk 재활용 2단 구조로 재설계** (§3.2, §3.7 — walk가
> Target Load PC를 선별하고 retire가 주소를 추적), wakeup 비투기성의 정확한 의미 (§2.2)
> v3 변경: store 케이스 전수 분류 + STA/STD 근거 (§2.3), 논문의 데이터 공백 기록 (§2.3),
> L1-miss 양정책, 논문 서술 지침 (§2.2), 충실도 재점검 (§2.6), reg_vector·PT sweep 확정 (§6)

---

## 0. 목적과 범위

현재 시뮬레이터의 RF 가속은 oracle이다: dcache 도착 시점(=AGU 이후)에 예측을 만들고 같은
cycle에 검증하므로 **lead time이 0이고, 대역폭을 쓰지 않고, timeliness가 공짜로 성립**한다.
이 계획은 RFP 논문(ISCA'22)의 구조를 그대로 가져와 다음을 실제로 모델링한다.

1. **Launch timing** — rename 직후 launch, 예측 주소로 L1을 실제로 조회
2. **대역폭 경쟁** — RFP는 demand load가 남긴 L1 read port만 사용 (최저 우선순위 FIFO)
3. **Timeliness** — prefetch 완료 cycle과 load의 검증 cycle을 비교해 full/partial/late 구분
4. **유한 predictor** — set-associative Prefetch Table (기존: 무한 hash table)
5. **Misprediction 비용** — validate-then-use이므로 flush가 없고, 비용 = 낭비된 L1 조회 1회.
   이것이 논문 설계의 핵심 장점이며 Scarab의 비투기적 wakeup 구조와 정확히 정합한다 (§2.2)

**ZERECO 차별점**: vanilla RFP는 모든 load를 훈련/launch한다. 우리는 기본적으로 H2P-chain
Target Load(`op->chain_bit`)만 대상으로 한다 (`RFP_SCOPE` 파라미터로 vanilla 비교 가능).
RFP 논문 §6 Related Work가 스스로 이렇게 쓴다:

> *"CRISP leverages criticality based scheduling to execute delinquent loads and their load
> slices as early as possible. **RFP is complementary to these works** ... intelligently
> combining RFP with criticality-based solutions can further reduce end-to-end pipeline
> latency for critical loads and their load slices."*

— RFP 저자들이 직접 "criticality 기반 선택 + RFP 결합"을 열린 방향으로 지목했다. ZERECO
positioning에 그대로 인용할 수 있는 문장이다. 또한 §5.1은 *"targeted prefetching for
specific load instructions"* 을 명시적으로 future work로 남겼다.

**유지되는 것**: 기존 oracle 경로(`h2p_chain_perfect_load` 계열)는 upper-bound 실험 재현을
위해 그대로 둔다. 새 timed 모델은 별도 파라미터(`rfp_enable`)로 게이트하고 상호 배제를
assert한다.

---

## 1. 논문 메커니즘 체크리스트 (구현 관점)

재독으로 확정한 사양. ✅ = Scarab에 그대로 이식, 🔶 = 변형/보수화, ⬜ = 생략(근거 있음).

| # | 논문 사양 (출처) | 이식 |
|---|---|---|
| 1 | **PT**: static load PC indexed, 8-way set-assoc, 1K–2K entry. Entry = tag 16b, confidence(기본 1-bit), utility 2b, stride, inflight counter 7b, 예측 주소 (§3.1, Table 1) | ✅ |
| 2 | **훈련은 load retirement에서만**. stride 반복 → confidence를 **확률 1/16로** 증가, utility 증가. saturate → RFP-eligible. stride 변화 → confidence/utility 리셋. 저utility → evict (§3.1) | ✅ |
| 3 | **Inflight counter**: load allocation에서 ++, commit에서 −−, **branch mispred로 squash된 load마다 −−**. 예측 주소 = base(마지막 retired VA) + stride × inflight (§3.1) | ✅ |
| 4 | **Launch**: PT lookup은 rename 이전에 가능(timing critical path 아님). prefetch packet {predicted VA, **prfid**}은 rename 직후 생성 → LSQ + L1로 전송 (§3.2) | ✅ |
| 5 | **RFP Queue**: 64-entry FIFO. L1 port 경쟁에서 demand load보다 **최저 우선순위**, RFP끼리는 old > young (§3.2, §3.5) | ✅ |
| 6 | Prefetch가 port를 못 얻은 사이 **load가 먼저 ready되면 prefetch는 drop**되고 load는 정상 진행 (§3.3). 실측: injected 72% 중 24%p가 이렇게 소멸 (Fig 13) | ✅ |
| 7 | **In-flight store 처리**: launch 시 older store를 youngest-first로 scan. 주소 일치 → store 완료를 기다려 store data 사용(forwarding). 주소 미상 → MD predictor로 wait/skip 결정. MD가 틀렸고 load가 이미 prefetch 값으로 dispatch됨 → pipeline flush (§3.2.1) | 🔶 **abstain으로 보수화** (§2.3) |
| 8 | **Coherency**: load dispatch 전까지 RFP는 load의 proxy — memory unit이 barrier/ordering을 load에게 하듯 강제 (§3.2.1) | 🔶 동일 논리로 abstain |
| 9 | **주소가 맞으면 재검증 L1 access 불필요** — RFP가 demand access를 대체하므로 correct RFP의 L1 lookup 수는 baseline과 동일 (§3, Bandwidth) | ✅ (§2.5) |
| 10 | **RFP-inflight bit** (RS entry당 1b): prefetch가 L1 lookup을 시작하는 순간 set = 완료 3 cycle 전 = scheduling pipe(wakeup/select/RF read) 깊이와 일치 → load wakeup 시 bit가 서 있으면 dependent를 투기적으로 깨워 RFP 완료 직후 execute 도달. mismatch/L1-miss 시 dependent cancel & re-issue (§3.3, Fig 9) | ⬜ 시뮬레이터 구현 불필요 / **✅ 논문 mechanism·저장량에는 반드시 포함** (§2.2) |
| 11 | **Validation**: load의 Addr 단계에서 예측 주소와 실제 주소 비교. 일치 → L1 access 통째로 skip. 불일치 → 정상 L1 access (§3.3, Fig 9) | ✅ |
| 12 | **DTLB miss → RFP drop** (§3.2.2). 성능 영향 무시 가능 (§5.5.5) | ⬜ Scarab에 per-access DTLB timing 없음 |
| 13 | **L1 miss → lower level로 진행** (demand load처럼) (§3.2.2). 단 실측 이득 **+0.02%** (§5.5.5) | ✅ **drop(0)/진행(1) 두 정책 모두 구현**, 실험으로 결정 (§3.4) |
| 14 | **PAT** (Page Address Table): 64-entry 4-way, 44b page frame 공유로 PT 저장량 ~50% 절감, 성능 손실 0.09% (§3.5, §5.5.4) | ⬜ storage 계산에만 사용, timing 무관 |
| 15 | Context(path-based) prefetcher: stride 대비 +0.3%뿐 (§5.5.3) | ⬜ stride만 구현 |

### 논문 핵심 수치 (검증 목표치)

| 항목 | 값 |
|---|---|
| Baseline (Tiger Lake-like, Table 2) | 5-wide, ROB 352, LQ 128, SQ 72, **IQ 125**, load port 2, L1D 48KB **5cy**, L2 15cy, LLC 40cy, L1 stride pref + L2/LLC multi-stream pref |
| Speedup / Coverage | **3.1% / 43.4%** (65개 workload geomean) |
| Injected / Executed / Useful | **72% / 48% / 43.4%** of all loads (Fig 13) |
| Full hide / Partial | 34.2%는 load dispatch 전 완료(1-cycle load처럼 보임), 9.2%는 부분 절약 (§5.2.2) |
| Incorrect prefetch | ~5% of loads (1-bit conf) — load latency 영향 없음, L1 대역폭만 낭비 (§5.2) |
| Confidence 폭 | 1-bit가 최적 (3.1%). 4-bit는 오예측 0.7%로 줄지만 coverage 하락으로 2.4% (Fig 17) |
| PT 크기 | 1K→16K: 3.1%→3.5%. 1K로 충분 (Fig 18) |
| 저장량 | PT 1K = 6.5KB + PAT 352b + queue 64 + RS bit 128b ≈ **7KB** (Table 1) |
| Dedicated port 시 | 4.0% / 57.4% — 대역폭이 1차 제약임을 보임 (Fig 14) |
| vs DLVP(AP) | DLVP 실효 coverage 11% (port 부족 + 5cy L1 + uop-cache가 fetch-런어헤드 잠식). RFP는 3.8× (Fig 16) |

---

## 2. Scarab 매핑 원칙

### 2.1 그대로 가는 것

PT 구조/훈련 규칙/inflight counter/rename-launch/FIFO queue/port 최저우선순위/AGU-시점
validation — 전부 1:1 대응 지점이 존재한다 (§4의 통합 지점 표).

### 2.2 접히는 것 — RFP-inflight bit와 투기적 wakeup

논문 baseline은 Yoaz-style hit-miss predictor + 투기적 scheduling + scoreboard
cancel/re-issue를 가정한다 (§2.5, §3.3). RFP-inflight bit의 존재 이유는 **투기적으로 깨운
dependent의 execute 도달 시점을 prefetch 완료(3cy 뒤)와 정렬**하는 것이다.

**"비투기적"의 정확한 의미 (코드 확인)**: Scarab은 dependent를 *추측으로* 깨우지 않는다.
대신 **결과가 확정되는 순간**(dcache access cycle)에 `wake_up_ops()`를 호출하고,
[`simple_wake()`(map.c:775)](../map.c#L775)가 `dep_op->rdy_cycle = MAX2(rdy_cycle,
src_op->wake_cycle)`로 **미래의 ready cycle을 건네준다**. Dependent는
`cycle_count + 1 >= rdy_cycle`에서 READY가 되어 select되고 `rdy_cycle`에 execute한다 —
즉 **데이터 도착 cycle에 back-to-back으로 실행되며, dependent의 select/RF-read는 그 앞
구간에 숨는다.**

⇒ 질문에 대한 답: 순 timing이 **"hit-miss predictor가 100% 정확하고 오예측 비용이 0인
기계"** 와 동일하다. 다만 *예측을 잘 맞히는* 것이 아니라, **결과 확정 후에 미래 시점을
통보**하는 방식으로 같은 결과에 도달하는 것이다. 그래서 cancel/replay가 구조적으로
불필요하다. 이 이상화가 빠뜨리는 비용(hit-miss 오예측 시 cancel + re-issue)은 주로 L1
**miss**에서 발생하는데 RFP는 L1 hit을 대상으로 하므로, baseline과 RFP config에 거의
동일하게 작용한다 (비교상 중립).

따라서 **RFP-inflight bit 전체가 `done_cycle` 산술 하나로 접힌다**:

```
covered 판정 시 (load의 첫 dcache 시도 = AGU 직후):
    op->done_cycle = cycle + RFP_HIT_LATENCY(1)     ← 논문 Fig 9: dependent가 Addr 직후 execute
미완료지만 주소 일치 (in-flight):
    op->done_cycle = rfp_data_ready_cycle           ← 논문 §3.3: "a fraction of the load latency is saved"
불일치/미실행:
    정상 demand 경로 (baseline과 동일)               ← 논문: dependent cancel & re-issue에 해당
```

절약분 검증: baseline load done = dcache_cycle + `DCACHE_CYCLES`(5), covered done =
dcache_cycle + 1 → **절약 4 cycle = 논문 Fig 8→9의 skip된 L1 pipe와 동일**. Scarab의
post-done 파이프 비용(select→exec)은 모든 config에 균일하게 적용되므로 상대적 절약이
보존된다. 이는 `zereco_MOTIVATING_EXAMPLE.md`의 그림 (c)/(d) 모델과도 일치한다.

**대신 잃는 것**: 논문의 "RFP L1-miss 시 투기적으로 깨운 dependent를 cancel하는 비용" 같은
2차 효과는 모델에 존재하지 않는다. Scarab baseline에도 hit-miss predictor가 없으므로
baseline과 RFP config가 같은 규칙으로 비교되며, 공정성은 유지된다.

**논문 서술 지침**: 시뮬레이터가 접었다고 논문에서도 접으면 안 된다. 논문의 mechanism 절은
RFP 논문과 동일하게 **load consumer의 speculative wakeup이 존재하는 현대적 baseline**을
가정하고, RFP-inflight bit(RS entry당 1b)와 wakeup 정렬 메커니즘을 원형 그대로 기술한다.
Methodology 절에 "시뮬레이터는 비투기적 wakeup 모델이므로 RFP-inflight 정렬의 steady-state
타이밍 효과를 load 완료 시점으로 등가 모델링했고, baseline과 RFP config에 동일 규칙이
적용되어 비교가 공정하다"를 한 문단으로 명시한다. 저장량 표에도 RFP-inflight bit
(RS entry 수 × 1b)를 포함해 계산한다.

### 2.3 보수화하는 것 — store 처리

**직관 예시** — `S1: st [0x1000]←r5` … `L1: ld r4←[0x1000]`:

- **실제 HW**: rename 땐 두 op가 같은 주소인지 모른다(주소는 AGU에서 나옴). L1 실행 시
  SQ CAM 검색으로 S1을 발견 → forwarding, 주소 미상이면 MD predictor 추측, 오판이면 flush.
  메모리 의존성 발견 = **실행 시점**의 일이라 투기·복구 하드웨어가 필요하다.
- **Scarab**: oracle이라 map 시점에 모든 주소를 안다. `thread_map_mem_dep()`이 S1을 L1의
  source로 그냥 추가 → L1은 레지스터 피연산자 기다리듯 S1 데이터를 기다린다.
  = **"완벽한 memory disambiguation을 rename에서 수행"하는 모델**. 투기/flush 불필요.

**먼저, Scarab의 store↔load 의존성 모델 (코드 확인 완료)**:

1. **윈도우 안 store→load 진짜 의존성은 map 시점에 oracle 정확도로 걸린다.**
   `stage_process_op()`([map_stage.c:220](../map_stage.c#L220))에서 `thread_map_mem_dep()`
   → [`add_store_deps()`(map.c:437)](../map.c#L437)가 **byte 단위** oracle memory hash를
   뒤져, load가 읽는 byte를 쓴 in-flight store 전부를 **`MEM_DATA_DEP` true source**로
   추가한다 (`MEM_OBEY_STORE_DEP=1`, `MEM_OOO_STORES=1` — golden_cove 기본). 따라서 load는
   producer store의 데이터가 준비되기 전에는 issue될 수 없다. **MD 투기도 memory-order
   violation flush도 모델에 존재하지 않는다** — 의존성이 항상 정확해서 위반이 불가능하다.
   (실제 HW보다 약간 이상화된 지점이지만 baseline과 RFP config에 동일하게 적용된다.)
2. [`scan_stores()`(memory.c:2419)](../memory/memory.c#L2419)는 **다른 창**을 본다: ROB가
   아니라 **memory request buffer의 in-flight store write(`MRT_DSTORE`)** — 이미 실행된
   store의 쓰기가 아직 메모리 계층을 내려가는 중인 경우.
3. Store-forwarding "빠른 경로"는 timing으로 따로 없다: dep이 풀린 load도 일반 load
   파이프(W-S-R-A + L1)를 그대로 탄다. 모든 config에 균일하므로 비교는 공정하다.

**세 세계 비교** — "older store와 겹치는 load"의 운명:

| | baseline | RFP 적용 시 |
|---|---|---|
| 실제 HW (논문) | SQ forwarding으로 데이터 수령 | RFP packet이 LSQ에서 store를 만나면 **store data를 미리 RF로 forwarding** → covered 유지 (§3.2.1 case a) / 주소 미상이면 MD 투기 (case b) / MD 오판 시 flush (case c) |
| Scarab (우리) | `MEM_DATA_DEP`으로 store를 기다린 뒤 일반 load 파이프 | **abstain: RFP 대상에서 제외** → baseline과 완전히 동일하게 동작. **느려지지 않는다, 가속만 안 받는다** |

즉 "이 케이스가 우리 모델에 없다"가 아니다 — **케이스는 존재하고 timing도 정확히 처리되며,
단지 그 load가 RFP 가속 혜택을 받지 못할 뿐**이다. 논문 설계라면 case (a)로 그 load도
covered였으므로, 우리의 coverage는 그만큼 하한(lower bound)이다.

**케이스 전수 분류 (하드웨어 관점)** — RFP 이득이 가능한가:

| # | 상황 | 논문 RFP 동작 | 이득 |
|---|---|---|---|
| 1.1 | older store 주소 미상, MDP=dependent | store 대기 → 주소 확정 후 일치하면 store data 사용 | timely면 ○ |
| 1.2 | older store 주소 미상, MDP=independent | 즉시 L1 probe | ○ |
| 2 | 주소 일치, store 데이터 준비됨 | **store data를 PRF로 forwarding** | ○ |
| 3 | 주소 일치, store가 이미 write 완료 | 동일 (L1을 읽어도 정답) | ○ |
| 2′ | 주소 일치, **store 데이터 미준비** | store 완료 대기 | timely면 ○ |
| 4 | 겹치는 older store 없음 | 즉시 L1 probe | ○ |

**논문 설계에서 RFP가 원천 불가능한 케이스는 없다.** 케이스 2·3도 "캐시 대신 store data를
쓴다"로 covered를 유지한다 — RFP의 이득은 *cache access 제거*가 아니라 *load 자신의 AGU보다
먼저 값을 PRF에 넣는 것*이기 때문이다 (baseline에서 SQ forwarding은 load가 W-S-R-A를 마치고
CAM해야 일어난다). 유일한 실패는 **MDP 오판**(1.2로 봤는데 실제 dependent) → flush.

**2′가 존재하는 이유**: x86 store는 **STA(주소) / STD(데이터) 두 uop으로 쪼개져 독립
스케줄**된다. 데이터 레지스터가 long-latency load에 의존하면 주소는 일찍 확정되고 데이터는
늦게 온다 — `ld r5←[r9]`(DRAM miss) 직후 `st [r1+8]←r5`가 그 예다.

**Scarab에는 2′가 없다**: store가 쪼개지지 않는다. 트레이스에서 확인 —
`IST r80(TMP2), r6(RSI), r7(RDI)  8@7ffff4890150` — 데이터(TMP2)와 주소(RSI,RDI) source를
모두 가진 **단일 op**이고 ready 시점은 `max(주소, 데이터)` 하나다.
⇒ 후속에 케이스 2/3 forwarding을 구현하기로 하면, **그 store op의 `done_cycle` 하나가 곧
`rfp_data_ready_cycle`** 이 되어 모델이 매우 단순하다 (L1 probe도 불필요 = 대역폭 0).

**Scarab 이진 매핑**:

| Scarab 판정 | = 케이스 | 우리 동작 |
|---|---|---|
| `MEM_DATA_DEP` **없음** | 1.2 + 4 | **launch** — 논문이 MDP로 근사하는 것을 오판 0%로 수행 |
| `MEM_DATA_DEP` **있음** | 1.1 + 2 + 3 (+2′) | **abstain** — 2·3의 forwarding 이득만 포기 |

**논문에 이 부분 데이터가 없다 (전문 재확인)**: RFP 논문은 ① store-forwarding 경로를 탄 RFP
비율, ② MD 오판 flush 빈도/성능 영향, ③ store 처리 sensitivity — 셋 다 보고하지 않는다.
§3.2.1은 *"the entire sequence ... is the same as a conventional load pipeline"* 으로 flush가
RFP 고유 비용이 아님을 논증하고(+ load 미dispatch 시 flush 불필요), §5.4/Fig 16은 DLVP가
no-FWD predictor 때문에 eligible이 **49% → 45%** 로 줄어든다는 *비교*만 제시하며
*"RFP continues to prefetch aggressively even in the presence of in-flight stores"* 를 우위로
내세운다 — 그 우위의 크기는 끝내 측정하지 않는다.
⇒ `RFP_ABSTAIN_STORE_DEP`은 **논문이 비워둔 칸을 채우는 측정**이고, 우리 MD가 oracle이라
**예측기 품질에 오염되지 않은 순수 store-dependence 빈도**를 얻는 드문 위치에 있다.

**구현 — abstain의 두 시점** (map-time dep 정보를 활용해 정밀화):

- **Launch 시**: `thread_map_mem_dep()`이 launch 훅(`reg_file_rename` 직후)보다 **먼저**
  실행되므로, load의 `src_info`에 `MEM_DATA_DEP`이 있는지 그 자리에서 안다. 있으면
  **packet enqueue를 포기** + stat `RFP_ABSTAIN_STORE_DEP`. 이 stat이 곧 **"논문이라면
  LSQ 삽입으로 처리했을 RFP 모집단"**이다. dep store의 데이터 ready 여부로
  `_FWD_READY`(논문 case a: 즉시 forwarding 가능) / `_FWD_WAIT`(case a-대기/b 근사)를
  분해 수집한다. inflight++는 abstain보다 **먼저** 수행한다 — counter는 모든 dynamic
  instance를 세야 같은 PC의 다른 instance 주소 산술이 맞는다.
- **Probe / Validation 시**: `scan_stores()`로 memory-system in-flight store write와의
  겹침 확인 → 겹치면 drop / uncovered. (기존 oracle과 동일 규칙)

**창(window) 근사가 닫히는 이유** — probe가 읽은 값을 stale로 만들 수 있는 older store를
전수 분류하면 전부 커버된다:

| older store의 상태 (load의 map 시점 기준) | 어디서 잡히나 |
|---|---|
| ① 아직 window 안 (실행 미완료) | map이 `MEM_DATA_DEP` 생성 → **launch에서 abstain** |
| ② 실행 완료, write가 메모리 계층에 in-flight (store miss 등) | **`scan_stores()`** — req buffer의 `MRT_DSTORE` 검색 |
| ③ write까지 완전 종료 | stale 위험 없음 — probe가 최종값을 읽음 |

v2의 구멍은 ①이었다: launch 검사가 없었고 validation의 `scan_stores`는 ②만 보는데, ①의
store가 그 사이 L1-hit write를 끝내면(write req를 안 만드니) 어디에도 안 걸렸다.
Launch-시점 abstain이 ①을 입구에서 막아, 남는 것은 ②(scan_stores)와 ③(무해)뿐이다.

**논문 대비 결과 방향**: forwarding으로 살렸을 case 2·3을 우리는 uncovered로 둔다 →
coverage 과소평가 = ZERECO에 불리한 방향이라 안전. MD 오판 flush 비용도 함께 사라져 일부
상쇄. 논문에는 "chain Target Load 중 store-dependent 비율은 X%(신규 stat)에 불과해
forwarding 경로를 생략한 보수 모델을 사용했다"로 쓴다.

**결정 (2026-08-14)**: **abstain으로 구현을 진행한다.** 케이스 2·3 forwarding
(`rfp_store_fwd_enable`)은 Phase 1의 `RFP_ABSTAIN_STORE_DEP` 실측을 본 뒤 필요하면 추가하는
**보류 항목**이다 — 위에 적었듯 구현 자체는 `done_cycle` 한 줄이라 나중에 붙여도 저렴하다.

### 2.4 생략하는 것

| 항목 | 근거 |
|---|---|
| DTLB-miss drop | Scarab dcache 경로에 per-access DTLB timing이 없음. 논문도 영향 무시 가능 (§5.5.5) |
| PAT | timing 무관. 논문 storage 수치(6.5KB→~3.3KB)는 우리 논문의 저장량 표 계산에만 인용 |
| Context prefetcher | +0.3% (§5.5.3) |
| 실제 데이터 이동/PRF write port | Scarab은 oracle-driven이라 값은 항상 정확. RFP는 순수 timing 이벤트 |

### 2.5 Oracle 대비 fidelity가 **올라가는** 부수 지점

현재 oracle은 covered load가 `cache_access()`를 아예 건너뛰므로 **캐시를 데우지 않는
비대칭**이 있었다 (LRU/후속 hit에 영향). Timed RFP에서는 **probe가 실제 L1 access를
수행**하므로(같은 line을 touch) demand access를 "대체"한다는 논문 주장 그대로 L1 touch 수가
보존된다. `RFP_PROBE_UPDATES_REPL=1` 기본.

또 하나: covered 판정이 `zereco_ARCHITECTURE.md` §6.4의 RF-covered 4조건 정의를 **처음으로
완전히 구현**하게 된다 — (1) 예측 생성 + (2) 주소 일치 + (3) ordering 검증 + **(4) timely
도착**. 기존 oracle은 (4)가 공짜였다.

**Pollution 뉘앙스**: L1-**hit** probe는 pollution을 만들지 않는다 — 새 line을 가져오지 않고
잔여 대역폭(port)과 replacement 상태만 소비한다. 진짜 pollution(불필요한 line이 L1을 채우고
기존 line을 쫓아내는 것)은 `RFP_L1_MISS_POLICY=1`(miss를 fill)에서만 발생한다. 이것이 두
정책 A/B 실험의 관전 포인트다: policy 1은 coverage를 늘리는 대신 오예측 시 MSHR 점유와
fill pollution을 실측 비용으로 낸다.

### 2.6 구현 충실도 재점검 — 무엇을 충실히, 무엇을 근사로

원칙: **측정 대상(coverage, accuracy, timeliness, 대역폭, PT 용량)에 직접 영향을 주는
로직만 충실히 구현하고, 나머지는 근사하거나 생략한다.**

| 구성요소 | 수준 | 이유 |
|---|---|---|
| PT 용량/associativity/훈련 규칙/confidence(1/16 확률) | **충실** | coverage·accuracy의 1차 결정 요인. sweep 대상 |
| 훈련 2단 분리 (walk=PC 선별 / retire=주소 추적) | **충실** | walk 단독 훈련은 stride·base_va를 깨뜨린다 (§3.2) |
| Inflight counter (rename++ / commit−− / squash−−) | **충실** | bc 실측 ~49 in-flight — 없으면 예측 전부 빗나감 |
| RFP Queue FIFO + L1 port 최저우선순위 | **충실** | injected→executed 손실(논문 24%p)이 곧 대역폭·timeliness 모델 |
| Validation 시점/조건, covered 판정 | **충실** | coverage 정의 그 자체 (ARCHITECTURE §6.4) |
| L1-miss 정책 (drop/진행) | **충실 — 양쪽 다** | 실험으로 결정할 설계 축 (§3.10) |
| Port 실패 정책 / port 우선순위 | **충실 — 전 조합** | 대역폭이 1차 제약이라 결과를 좌우한다 (§3.4, §3.10) |
| Stale-value 창 검출 | **계측만** | 값은 oracle이라 실행에 영향 없음; 하드웨어 위험도 보고용 (§2.7) |
| RFP-inflight bit / speculative wakeup 정렬 | **근사** — done_cycle 등가 | §2.2. 논문 서술은 원형 유지 |
| Store 처리 (forwarding/MD/flush) | **근사** — abstain 일원화 | §2.3. MDP 오판은 oracle이라 0%; 케이스 2·3 forwarding은 보류 항목 |
| Store STA/STD 분리 (케이스 2′) | **해당 없음** | Scarab store는 데이터·주소 source를 가진 단일 op (§2.3) |
| probe↔validation 사이 완료-store 창 | **생략 (문서화)** | launch-시점 store-dep abstain으로 대부분 닫힘 (§2.3). 잔여분은 scan_stores가 커버 |
| RFP packet의 LSQ 삽입 | **생략 — 단 빈도는 수집** | launch-시점 `RFP_ABSTAIN_STORE_DEP` stat이 논문의 LSQ-삽입 모집단을 측정 (§2.3, §3.9) |
| PT partial tag(16b) aliasing | **생략** — full tag 사용 | 2차 효과. 저장량 표 계산에만 16b 인용 |
| PAT / DTLB / context prefetcher | **생략** | §2.4 |
| utility(2b)/inflight(7b) 비트폭 | **하드코딩** | sweep 계획 없음 — 파라미터 표면 축소 |
| 데이터 이동 / PRF write port | **생략** | oracle-driven sim — RFP는 순수 timing 이벤트 |

### 2.7 값 정확성과 corner case — 무엇을 측정할 수 있고 무엇은 못 하는가

**우려되는 시나리오**: prefetch한 값으로 H2P branch가 resolve됐는데 그 값이 틀렸다면,
"misprediction을 mispredict"하는 사태가 난다. 잘못된 recovery를 시작하고, 진짜 결과는 나중에
드러난다.

**Scarab에서는 발생할 수 없다.** Scarab은 timing simulator이고 **값은 트레이스에서 오는
oracle**이다. RFP는 `done_cycle`만 바꾸므로 branch가 잘못된 값으로 resolve될 방법이 없다.
따라서 이 사태 자체를 직접 계측할 수는 없다. (P-IQ, oracle RF 실험도 전부 같은 전제 위에
서 있다.)

**대신 하드웨어에서 그 사태를 일으킬 조건의 빈도는 정확히 잴 수 있다.** 실제 RFP에서 잘못된
값이 쓰이는 경로를 전수 분류하면:

| 경로 | 하드웨어 결과 | 우리 모델 |
|---|---|---|
| (a) 주소 오예측 | AGU 검증이 잡아냄 → demand 경로로 폴백. **값이 틀릴 일 없음** | `RFP_WRONG_ADDR`로 계측 |
| (b) 주소는 맞지만 probe 이후 older store가 그 위치를 덮어씀 | **stale 값 → 잘못된 resolve → flush** | §2.3 abstain으로 대부분 차단. 잔여 창을 `RFP_STALE_VALUE_WINDOW`로 계측 |
| (c) 다른 코어의 coherence write | flush | 단일 코어 실험이라 해당 없음 |

즉 **위험 경로는 (b) 하나뿐**이고, launch-시점 `MEM_DATA_DEP` abstain + probe/validation의
`scan_stores`로 거의 닫혀 있다. 남는 창은 정확히 하나다 — **load의 rename 이전에 이미 retire
됐지만 write가 L1에 아직 안 내려간 store가, probe와 validation 사이에 drain되는 경우.**

`rfp_stale_value_check`(기본 ON)가 이 창을 잡는다: line별 last-store-write-cycle 표(직접
사상, O(1))를 두고 validation에서 `last_store_write[line] ∈ (rfp_probe_cycle,
validate_cycle]`인지 본다. 타이밍은 건드리지 않는 순수 계측이다. 값이 0에 가까우면 "보수적
모델이 놓친 위험이 무시할 수준"임을 수치로 보일 수 있고, 유의미하면 그 자체가 보고할 발견이다.

`RFP_STALE_VALUE_FEEDS_H2P_BRANCH`는 한 걸음 더 간다 — 그 stale 후보 load가 실제로 H2P
branch의 slice에 속하는지까지 세어, "우리 논문이 주장하는 가속 대상에서 이 위험이 몇 번
나타나는가"에 직접 답한다.

---

## 3. 상세 설계

### 3.1 자료구조 — 신규 파일 `src/zereco/rfp.h` / `rfp.c`

```c
/* Prefetch Table: RFP_PT_ENTRIES / RFP_PT_ASSOC sets × RFP_PT_ASSOC ways, per proc */
typedef struct RFP_PT_Entry_struct {
  Flag    valid;
  Addr    tag;              /* full PC 보관, 비교는 하위 16b 모델 (stat로 alias 측정) */
  uns     confidence;       /* saturate 시 eligible. 기본 1-bit */
  uns     utility;          /* 2-bit. replacement victim 선택 기준 */
  Flag    has_stride;
  int64   stride;
  Addr    base_va;          /* 마지막 retired instance의 VA */
  uns     inflight;         /* allocated-not-committed instance 수. cap 127 (7b) */
  Counter lru_touch;        /* utility 동률 시 LRU */
} RFP_PT_Entry;

/* RFP Queue: FIFO, per proc */
typedef struct RFP_Queue_Entry_struct {
  Flag    valid;
  Op*     op;               /* identity 검증 후에만 deref (Scarab mem_req 관행) */
  Counter op_num;
  Counter unique_num;
  Addr    pred_va;
  uns     mem_size;
  Counter launch_cycle;     /* lead-time 통계용 */
} RFP_Queue_Entry;
```

**Op 필드 추가** ([op.h](../op.h) zereco 블록, [op_pool.c:226](../op_pool.c) 리셋 블록에 함께):

```c
Flag    rfp_pt_counted;        /* rename에서 inflight++ 했음 (exactly-once 보장) */
Flag    rfp_launched;          /* queue에 packet 넣었음 */
Flag    rfp_validated;         /* 검증 1회 완료 (port-retry 중복 방지) */
Addr    rfp_pred_va;
Counter rfp_data_ready_cycle;  /* MAX_CTR = 미완료/미실행 */
```

### 3.2 훈련 — 2단 구조 (BW walk = PC 선별, retire = 주소 추적)

Fill Buffer / BW walk를 훈련에도 재활용한다. 단 **역할을 나눠야 한다** — walk는 "어떤 PC가
Target Load인가"를 권위 있게 정하지만, **주소 상태(base/stride) 추적에는 쓸 수 없다**:

| walk로 주소를 추적하면 생기는 문제 | 이유 |
|---|---|
| **stride 학습 붕괴** | walk 중 `fill_buffer_add()`가 retire op을 버리고([fill_buffer.c:44](../fill_buffer.c#L44)), walk 후 buffer가 리셋된다. snapshot 경계를 넘는 delta = k×stride (k 미상) → confidence 리셋 + stride thrash |
| **`base_va` 부패** | 예측식이 `base(마지막 retired VA) + stride × inflight`인데, base가 수백 instance 전이면 `inflight`가 그 간격을 메우지 못해 예측이 체계적으로 빗나간다 |
| **지연** | PT 갱신이 `BACKWARD_WALK_CYCLES`(500) + snapshot 나이만큼 늦다 |

**구체적 규모 (bc 기준, 개략)**:

```
snapshot 하나 = 512 uop ÷ 10.4 uop/iter ≈ 49 instance      ← 관측됨
walk 500 cycle × IPC≈2 ≈ 1000 uop ≈ 96 instance            ← 전부 버려짐
⇒ 약 1/3만 보고 2/3를 놓친다
```

깨지는 지점을 예측식에 대입하면 명확하다 — `pred = base_va + stride × inflight`:

- **stride**: snapshot 내부 delta = +4(정상) → 경계 delta ≈ 97×4 = 388 → stride 튐 + confidence
  리셋이 **매 snapshot마다** 반복
- **base_va**: 예측 시점에 base가 최대 ~96 instance 전 값인데 `inflight`는 window 안(~49)만
  센다 ⇒ **그 간격을 메울 수단이 없어** 예측이 체계적으로 ~400B 빗나감

지연(위 표 3행)만 있고 구멍이 없어도 같은 under-shoot가 발생한다 — 예측식이 **끊기지 않은
최신 retire 사슬**을 구조적으로 요구하기 때문이다.

반면 **"이 PC가 Target Load인가"는 집합 소속 판정이라 느리게 변하고 sampling에 강인**하다 —
1/3만 봐도 충분하며, walk가 가장 잘하는 일이다. (Fill Buffer의 drop 구간이 식별에는 무해하지만
주소 추적에는 치명적인 이유가 바로 이 차이다: 집합 판정 vs 점화식.)

⇒ **역할 분담**:

```
BW walk  (commit_dependency_chain_entry, dependency_chain_cache.c:262)
    → PT 엔트리 "할당/갱신 권한"만 담당
    for each Target Load in slice:  rfp_note_target_load(pc)
        PT hit  → utility refresh (여전히 Target Load임을 확인)
        PT miss → 엔트리 할당 {base 미설정, conf=0, inflight은 보존}

retire   (node_stage.c:1029)
    → 주소 상태 추적. rfp_retire_train(op):
        gate: RFP_ENABLE && MEM_LD && thread 0
        entry = pt_lookup(pc)
        miss → 아무것도 안 함 (할당 금지)      ← 이 자체가 scope 필터
        hit  → 아래 규칙으로 base/stride/confidence/utility 갱신 + inflight--
```

**이 구조의 이점**:

1. **scope 필터가 정확해진다.** 기존 안(`op->chain_bit`)은 Block Cache가 그 dynamic block에
   hit해야만 서는 비트라, **Block Cache miss 시 진짜 Target Load인데도 훈련에서 누락**된다.
   PT 멤버십은 PC 단위라 그 구멍이 없다.
2. **walk가 Target Load의 단일 권위 정의**가 되어 논문 서술이 깔끔하다.
3. **재활용 구조가 두 가지 일(chain 생성 + Target Load 선별)을 수행** → complexity 주장 강화.
4. 주소 추적은 매 retire이므로 `base_va`가 진짜 마지막 retired 값이고 stride가 연속
   instance를 본다 — 위 표의 세 문제가 모두 사라진다.
5. PT의 utility/LRU가 Target Load에서 벗어난 PC를 자연 aging.

**전제 확인 (코드)**: `fill_buffer_add()`는 retire 경로에서 **무조건** 호출되고
([node_stage.c:1029](../node_stage.c#L1029)), `cycle_backward_walk_engine()`은 매 cycle
**무조건** 구동된다([cmp_model.c:296](../cmp_model.c#L296)). ⇒ walk는 RFP 파라미터와 무관하게
항상 살아 있어 별도 게이팅이 필요 없다.

**옵션 (기본 OFF, `rfp_walk_bootstrap`)**: walk는 한 snapshot 안에서 같은 PC의 **연속**
instance를 여러 개 본다 (bc: ~49회). 이걸로 stride를 즉시 bootstrap해 warm-up을 앞당길 수
있다. snapshot 내부는 연속이라 위 표의 문제가 없다. 복잡도 대비 이득이 불확실하므로 기본 OFF.

**PT 갱신 규칙 (retire hit 시)**:

```
rfp_retire_train(op)  — PT hit인 경우:
    delta = va - base_va
    if has_stride && delta == stride:
        with prob 1/16: confidence = saturate++      ← 논문 §3.1 그대로
        utility = saturate++
    else:
        stride = delta; has_stride = TRUE
        confidence = 0; utility = 0                  ← 논문: 리셋
    base_va = va
    if op->rfp_pt_counted && tag 일치: inflight-- (floor 0)   ← "commit에서 감소"
```

- Retire만 훈련 → wrong-path 오염 없음 (ARCHITECTURE §8.3 committed-training invariant ✓).
  BW walk도 retire된 op만 보므로 동일하게 안전하다.
- 확률 1/16: 결정론적 재현을 위해 per-proc LCG 사용 (Scarab 관행). `RFP_CONF_INC_PROB_LOG2=4`
- entry가 rename↔retire 사이에 evict된 경우: tag 불일치 → decrement skip + integrity stat
- `base_va` 미설정 상태(walk가 방금 할당)에서는 첫 retire가 base만 채우고 delta는 만들지 않는다

### 3.3 Launch — rename 직후

**위치**: [map_stage.c:232](../map_stage.c) `stage_process_op()`의 `reg_file_rename(op)` 직후.
논문 §3.2: PT lookup은 rename 전에 가능하므로 여기 배치가 timing 논쟁을 만들지 않는다.
prfid는 이 시점에 방금 할당됐다 (`op->dst_reg_id`) — 논문의 "prefetch packet은 rename 직후
생성"과 동일 위치.

```
rfp_rename_launch(op):
  gate: RFP_ENABLE && MEM_LD && thread 0
        (off-path 포함! — 실제 HW는 path를 모름. 대역폭 현실성)
  entry = pt_lookup(pc); if miss → return
        (PT 멤버십이 곧 scope 필터 — 할당은 walk/retire만 §3.2, §3.7)
  entry->inflight = MIN(inflight+1, 127); op->rfp_pt_counted = TRUE
        (abstain보다 먼저 — counter는 모든 dynamic instance를 세야 주소 산술이 맞음)
  if op->oracle_info.src_info에 MEM_DATA_DEP 존재 → return
        (stat RFP_ABSTAIN_STORE_DEP, dep store ready 여부로 _FWD_READY/_FWD_WAIT 분해
         — 논문 §3.2.1 case (a)/(b)의 LSQ-삽입 모집단 측정. §2.3)
  if !eligible(confidence saturated) → return
  pred_va = base_va + stride × inflight            (++ 이후 값 = "N+1번째 미래 instance")
  if queue full → drop (stat RFP_DROP_QUEUE_FULL); return
  enqueue {op, op_num, unique_num, pred_va, launch_cycle=cycle}
  op->rfp_launched = TRUE; op->rfp_pred_va = pred_va
```

### 3.4 Queue drain — L1 최저 우선순위 probe

**위치**: `update_dcache_stage()` **말미** — demand op 루프가 read port를 전부 소진한 뒤.
`cmp_cores()`에서 dcache가 exec/node보다 먼저 돌고 port는 per-cycle 리소스이므로, 함수
말미 배치 = 그 cycle의 잔여 대역폭만 사용 = 논문의 "lowest priority" 그대로. demand는
구조적으로 절대 밀리지 않는다.

**`RFP_PORT_PRIORITY`로 이 전제를 실험 축으로 연다** (논문 Fig 14가 이 sweep을 검증한다 —
shared 3.1%/43.4% vs dedicated 4.0%/57.4%):

| 값 | 동작 | 배치 |
|---|---|---|
| 0 (기본) | 잔여 port만 사용. demand 절대 안 밀림 | 함수 말미 (현재 설계) |
| 1 | bank당 `RFP_DEDICATED_READ_PORTS`개의 **전용 추가 port**. demand는 못 씀 | 말미, 별도 port pool |
| 2 | prefetch 우선 — demand가 치르는 비용을 보이는 상한 | demand 루프 **이전** |

policy 0에서는 `RFP_DEMAND_DELAYED_BY_PREFETCH_*`가 **반드시 0**이어야 한다 (검증 조건).
policy 1/2에서 그 값이 곧 "coverage를 더 얻기 위해 demand가 낸 비용"이다.

```
rfp_queue_drain():  head부터 최대 RFP_DRAIN_WIDTH개:
  1. identity 검증 실패 (op->op_num/unique_num 불일치 = squash 후 재활용) → drop(stale)
  2. op->rfp_validated 이미 TRUE  → drop (stat RFP_DROP_LOAD_FIRST)
     ← 논문 §3.3 "load becomes ready → prefetch dropped". Fig 13의 24%p가 이 경로
  3. scan_stores(pred_va, size) 겹침 → drop (stat RFP_DROP_STORE_CONFLICT)  [§2.3 보수화]
  4. bank = pred_va 기준; port 획득 시도 (RFP_PORT_PRIORITY에 따라 대상 port가 다름)
       실패 시 RFP_PORT_FAIL_POLICY:
         0 (wait) : head 유지, 이번 cycle 종료 — 논문의 oldest-first 순서 보존.
                    head-of-line blocking은 RFP_HEAD_OF_LINE_BLOCKED_CYCLES로 계측
         1 (drop) : 즉시 폐기 (stat RFP_DROP_PORT_UNAVAILABLE)
         2 (skip) : head는 두고 younger entry가 앞질러 probe (stat RFP_SKIPPED_PAST_HEAD)
       RFP_QUEUE_MAX_WAIT_CYCLES 초과 시 폐기 (stat RFP_DROP_WAIT_TIMEOUT)
  5. cache_access(&dc->dcache, pred_va, &line, RFP_PROBE_UPDATES_REPL):
       hit  → op->rfp_data_ready_cycle = cycle + DCACHE_CYCLES; dequeue (stat RFP_EXECUTED)
       miss →
         policy 0 (drop):  drop (stat RFP_DROP_L1_MISS)
         policy 1 (진행):  demand miss 경로처럼 Mem_Req 생성 후 dequeue:
           - new_mem_req(MRT_DPRF, proc, line_addr, size, delay, /*op*/NULL,
                         rfp_fill_done, ...)          ← memory.h:237 시그니처 확인됨
           - 소형 per-proc pending 테이블 {unique_num, op*, op_num, line_addr}에 등록;
             rfp_fill_done()이 fill 시점에 identity 검증 후 rfp_data_ready_cycle 기록
           - MSHR 불가(mem_can_allocate_req_buffer 실패) → drop (stat)
           - 같은 line의 in-flight req 존재 → 기존 matching/coalescing에 편승
           - fill line이 실제 L1에 들어감 → 오예측 시 pollution·MSHR 점유가 실측 비용이 됨
```

### 3.5 Validation — load의 첫 dcache 시도

**위치**: [dcache_stage.c:493](../dcache_stage.c)의 oracle 호출과 같은 자리 (port 획득 **이전** —
covered load는 port를 쓰지 않는다는 논문 주장 그대로).

```
rfp_try_validate(op):
  gate: RFP_ENABLE && MEM_LD && thread 0 && !op->rfp_validated
  op->rfp_validated = TRUE                    (port-retry 재진입 방지, oracle의 pred_checked 관행)
  if !op->rfp_launched → FALSE (demand 경로)                    [stat: 후보였다면 NOT_PREDICTED]
  if op->rfp_pred_va != op->oracle_info.va → FALSE               [stat RFP_WRONG_ADDR, 논문 ~5%]
  if scan_stores(actual va) → FALSE                              [stat RFP_VAL_STORE_CONFLICT]
  if rfp_data_ready_cycle == MAX_CTR → FALSE                     [stat RFP_NOT_EXECUTED]
  covered:
    latency = (ready ≤ cycle) ? RFP_HIT_LATENCY                  [stat RFP_COVERED_FULL — 논문 34.2%]
              : (ready - cycle)                                  [stat RFP_COVERED_PARTIAL — 논문 9.2%]
    op->state = OS_SCHEDULED; done = wake = cycle + latency
    op->oracle_info.dcmiss = op->engine_info.dcmiss = FALSE
    op->zereco_rf_covered = TRUE               ← 기존 RF filtering(P-IQ)이 그대로 소비
    wake_up_ops(op, REG_DATA_DEP, wake_hook)
    (cache_access 없음, port 소비 없음 — probe가 이미 대체 access를 수행함)
    return TRUE
```

기존 oracle 본문(dcache_stage.c:1547-1561)의 완료 처리 뼈대를 재사용한다.

### 3.6 Squash — inflight counter와 queue 정리

논문 §3.1: *"On a branch misprediction, this counter is decremented for each squashed load."*

- **inflight 감소**: 단일 choke point인 [`free_op()`(op_pool.c:148)](../op_pool.c#L148)에 훅.
  모든 op는 retire든 squash든 결국 여기를 지난다. `rfp_pt_counted`가 남아 있으면
  (= retire 훈련에서 아직 감소 안 됨 = squash된 op) tag 일치 확인 후 inflight−−, 플래그 해제.
  Exactly-once가 플래그 하나로 보장되고 flush 경로(idq/decode/node/thread 5곳)를 개별
  추적할 필요가 없다. free가 squash보다 몇 cycle 늦을 수 있는 창은 inflight를 잠시
  과대평가하지만 실제 HW의 recovery 지연과 같은 성질이다.
- **Queue 정리**: lazy — drain 단계 1(identity 검증)이 stale entry를 자연 폐기.
  recovery마다 큐를 스캔하지 않는다 (HW도 packet을 능동 kill하지 않는 편이 자연스럽고,
  낭비 대역폭은 stat으로 보인다).

### 3.7 Scope 필터 — ZERECO의 차별점

Scope는 **PT 엔트리 할당 권한을 누가 갖는가**로 구현된다 (§3.2). 필터가 PT 멤버십 자체이므로
launch/validation 경로에는 별도 조건이 없다.

| | 할당 주체 | 의미 |
|---|---|---|
| `RFP_SCOPE 0` (기본) | **BW walk만** (`rfp_note_target_load`) | H2P Target Load PC 전용 = ZERECO |
| `RFP_SCOPE 1` | **retire에서 모든 load** | vanilla RFP 비교용 |

두 모드의 코드 차이는 **할당 정책 한 줄**이다. `op->chain_bit`은 RFP 경로에서 더 이상 쓰지
않으므로, frontend 태깅 게이트에 `RFP_ENABLE`을 추가할 필요도 없다 (P-IQ는 기존대로 chain_bit
사용).

**Scope 0의 논문 스토리**: Target Load static PC는 소수에 집중된다(기존 관찰). 따라서 vanilla의
1K-entry PT보다 훨씬 작은 PT로 같은 coverage가 나와야 하며, **PT 크기 sweep(논문 Fig 18 대응)이
ZERECO의 complexity 주장을 만드는 자체 그림**이 된다.

### 3.8 파라미터 (core.param.def, ZERECO 블록 뒤)

| 파라미터 | 기본값 | 근거 |
|---|---|---|
| `rfp_enable` | FALSE | timed RFP 마스터 스위치 |
| `rfp_pt_entries` / `rfp_pt_assoc` | 1024 / 8 | 논문 §3.5. **최적 크기 결정 sweep 필수** (§6 Phase 4) |
| `rfp_conf_bits` | 1 | 논문 Fig 17: 1-bit 최적. sweep 축 |
| `rfp_conf_inc_prob_log2` | 4 (=1/16) | 논문 §3.1 |
| `rfp_queue_entries` | 64 | §3.5. sweep 축 |
| `rfp_drain_width` | 2 | load port 수와 동일 상한 (실질 제약은 port) |
| `rfp_hit_latency` | 1 | RF 공급 실효 latency. oracle과 동일 값 유지 |
| `rfp_scope` | 0 | §3.7 |
| `rfp_l1_miss_policy` | 0/1 | **양쪽 구현, 실험으로 결정** (§3.4, Phase 4 A/B) |
| `rfp_probe_updates_repl` | 1 | §2.5 — probe가 demand touch를 대체 |
| `rfp_port_fail_policy` | 0 (wait) | wait / drop / skip 3안 sweep (§3.4) |
| `rfp_queue_max_wait_cycles` | 0 (무제한) | 오래된 packet의 queue 슬롯 점유 비용 |
| `rfp_port_priority` | 0 (잔여 port) | 0/1/2 sweep — 논문 Fig 14 대응 (§3.4) |
| `rfp_dedicated_read_ports` | 1 | `rfp_port_priority=1`일 때만 사용 |
| `rfp_stale_value_check` | TRUE | §2.7 corner case 계측. 타이밍 불변 |

utility(2b), inflight cap(127=7b)은 하드코딩한다 (§2.6).

**Assertion** ([dependency_chain_cache.c:22-55](../dependency_chain_cache.c#L22) 관행대로):
- `rfp_enable` ⊕ `h2p_chain_perfect_load` (동시 불가 — 같은 dcache hook을 다툼)
- `ZERECO_IQ_PRIORITY_POLICY==2`의 요구조건을 `(H2P_CHAIN_PERFECT_LOAD && oracle predictor) || RFP_ENABLE`로 확장
- `rfp_scope==0`이면 chain 태깅 활성 확인

### 3.9 통계 (zereco.stat.def) — 논문 Fig 13 재현 구조

논문의 3단 지표를 그대로 재현해 직접 대조 가능하게 한다:

```
[깔때기]  RFP_CANDIDATE_LOADS            (scope 통과한 dynamic load)
          RFP_PT_HIT / RFP_ELIGIBLE
          RFP_INJECTED                   ← 논문 72%
          RFP_EXECUTED                   ← 논문 48%   (probe가 L1 hit로 완료)
          RFP_USEFUL_FULL                ← 논문 34.2% (validation 시 이미 도착)
          RFP_USEFUL_PARTIAL             ← 논문 9.2%
          RFP_USEFUL_PCT                 ← 논문 43.4% (=coverage)
[LSQ 상호작용] RFP_ABSTAIN_STORE_DEP (+_FWD_READY / _FWD_WAIT 분해)
           ← 논문 §3.2.1 (a)/(b)로 처리됐을 모집단. "LSQ 삽입 비율" 요청 통계
[Drop 사유] RFP_DROP_QUEUE_FULL / _LOAD_FIRST / _STORE_CONFLICT / _L1_MISS / _STALE
[검증 사유] RFP_WRONG_ADDR(논문 ~5%) / RFP_VAL_STORE_CONFLICT / RFP_NOT_EXECUTED / RFP_LATE_...
[Timeliness] RFP_LEAD_TIME_TOTAL/AVG (validation − ready), RFP_SLACK 히스토그램 버킷
[비용]     RFP_PROBE_L1_ACCESSES (대역폭), RFP_WRONG_PROBE_ACCESSES (낭비분)
[무결성]   RFP_INFLIGHT_UNDERFLOW / RFP_PT_EVICT_WITH_INFLIGHT / RFP_STALE_DEREF_BLOCKED = 0 필수
```

coverage/accuracy 정의는 README §8 집계 원칙과 동일: coverage = USEFUL / CANDIDATE,
accuracy = USEFUL / (USEFUL + WRONG_ADDR 검증분).

### 3.10 실험 축 (sweep matrix)

설계 시점에 이미 "실험으로 정할 것"으로 분류한 축들. 전부 파라미터로 열려 있어 재구현 없이
sweep할 수 있다. 기본값은 **보수적인 쪽**(ZERECO에 유리하지 않은 쪽)으로 잡았다.

| 축 | 파라미터 | 값 | 무엇을 답하는가 |
|---|---|---|---|
| **predictor 용량** | `rfp_pt_entries` × `rfp_pt_assoc` | 128 / 256 / 512 / 1K / 2K / 4K | Target Load PC가 소수에 집중된다면 vanilla보다 작은 PT로 충분한가 (논문 Fig 18 대응) |
| **confidence 폭** | `rfp_conf_bits` | 1 / 2 / 3 / 4 | 정확도 vs coverage 트레이드오프 (논문 Fig 17: 1-bit 최적) |
| **queue 깊이** | `rfp_queue_entries` | 16 / 32 / 64 / 128 | queue-full 손실이 실제 제약인가 |
| **port 실패 정책** | `rfp_port_fail_policy` | wait / drop / skip | head-of-line blocking vs 순서 보존 |
| **port 우선순위** | `rfp_port_priority` (+`rfp_dedicated_read_ports`) | 잔여 / 전용 / prefetch-우선 | L1 대역폭이 1차 제약인가 (논문 Fig 14 대응) |
| **L1 miss 정책** | `rfp_l1_miss_policy` | drop / lower-level 진행 | 논문은 +0.02%라 했지만 우리 Target Load 분포에서도 그런가. policy 1만 pollution·MSHR 비용 발생 |
| **scope** | `rfp_scope` | H2P Target Load / 전체 load | ZERECO의 선별이 vanilla 대비 무엇을 얻고 잃는가 |
| **queue 수명** | `rfp_queue_max_wait_cycles` | 0 / 32 / 64 / 128 | 늦은 packet을 버리는 것이 이득인가 |

**논문에 실을 계측** (설계에 이미 반영):

1. **Resource contention** — `RFP_PORT_CYCLES_*` 4종으로 L1 read port를 demand/prefetch/idle로
   분해. `rfp_port_priority=0`에서 `RFP_DEMAND_DELAYED_BY_PREFETCH_*`가 0임을 보이면 "우리
   기본 설정은 demand를 방해하지 않는다"가 **주장이 아니라 측정**이 된다.
2. **Corner case** — §2.7. `RFP_STALE_VALUE_WINDOW`와 그중 H2P slice에 속한 비율.
3. **낭비된 대역폭** — `RFP_WRONG_PROBE_ACCESSES` / `RFP_LOWER_LEVEL_FILLS_WASTED`.
4. **깔때기 손실 분해** — oracle 상한과 timed 결과의 gap이 NOT_EXECUTED(대역폭) +
   PARTIAL(timeliness) + PT miss(용량) + QUEUE_FULL로 완전히 설명되어야 한다 (§5.6).

---

## 4. 통합 지점 요약 (파일:라인)

| 지점 | 파일 | 작업 |
|---|---|---|
| Launch 훅 | [map_stage.c:232](../map_stage.c#L232) `stage_process_op()` | `reg_file_rename()` 직후 `rfp_rename_launch(op)` |
| **PC 선별 훅** | [dependency_chain_cache.c:262](../dependency_chain_cache.c#L262) `commit_dependency_chain_entry()` | Target Load 순회 루프에서 `rfp_note_target_load(pc)` — PT 할당 권한 (§3.2) |
| 훈련 훅 | [node_stage.c:1029](../node_stage.c#L1029) retire 루프 | `fill_buffer_add()` 인근 `rfp_retire_train(op)` — PT hit일 때만 갱신 |
| Validation 훅 | [dcache_stage.c:493](../dcache_stage.c#L493) | oracle 자리와 병렬: `if (rfp_try_validate(op)) continue;` |
| Queue drain | `update_dcache_stage()` 말미 | `rfp_queue_drain()` — demand port 소진 이후 |
| Squash 훅 | [op_pool.c:148](../op_pool.c#L148) `free_op()` | counted 플래그 기반 inflight−− |
| Op 필드 | [op.h:293](../op.h#L293) zereco 블록 / [op_pool.c:226](../op_pool.c#L226) | §3.1의 5개 필드 + 리셋 |
| Assert | [dependency_chain_cache.c:22](../dependency_chain_cache.c#L22) | §3.8의 상호배제/확장 |
| 파라미터/통계 | [core.param.def:418](../core.param.def#L418) / zereco.stat.def | §3.8 / §3.9 |
| 신규 | `src/zereco/rfp.h` `rfp.c` | PT/Queue/로직 전부. CMakeLists 등록 |

기존 oracle(`dcache_stage_try_main_chain_load_oracle`, `h2p_oracle_pred_*`)은 **무수정**.

---

## 5. 검증 계획

1. **Phase 1 cycle-identical**: profile-only 모드(§6 Phase 1)에서 baseline과 총 cycle이
   완전 일치해야 한다 (기존 `zereco_h2p_mispred_latency_profile`과 같은 규율).
2. **무결성 counter 0**: §3.9의 무결성 항목. 기존 P-IQ mismatch-counter 규율 그대로.
3. **깔때기 단조성**: CANDIDATE ≥ INJECTED ≥ EXECUTED ≥ USEFUL_FULL+PARTIAL, 전 workload.
4. **마이크로벤치**: `tools/h2p_example` — LOAD A(stride +4)는 워밍업 후 covered,
   LOAD B(irregular indirect)는 WRONG_ADDR로만 잡혀야 한다.
5. **논문 shape 대조**: `rfp_scope=1`(all loads)로 SPEC 계열 실행 시 useful이 논문 범주
   수치(ISPEC17 38.7%, FSPEC17 57.3% 등)와 같은 자릿수인지. 정확 일치는 기대하지 않음
   (core/캐시 파라미터 상이) — 구조 검증 목적.
6. **Upper-bound ladder**: `oracle_rf_stride`(기존 260810) ≥ `rfp_timed(all)` ≥ 이어야 하고,
   그 gap이 stat으로 분해되어야 한다: NOT_EXECUTED(대역폭) + PARTIAL 손실(timeliness) +
   PT miss(용량) + QUEUE_FULL. **분해 합이 gap을 설명 못 하면 버그다.**
7. **offline replay 교차검증**: `src/tools/h2p_chain_load_predictor_replay.py`가 oracle
   predictor와 대조했듯, PT의 eligible/예측 일치 판정을 fill_buffer 트레이스 replay로 재현.

---

## 6. 구현 단계 (각 단계가 독립적으로 검증 가능)

| Phase | 내용 | 완료 판정 |
|---|---|---|
| **0** | **`reg_vector` 128-bit 확장 (확정 포함)** + 파라미터/Op 필드/stat 정의, rfp.h/c 뼈대, CMake | 빌드 통과, RFP 전부 비활성. reg_vector 확장으로 chain/TL 태깅 모집단 변화는 stat으로 확인 |
| **1** | PT + retire 훈련 + rename lookup/inflight 수명주기(free_op 훅 포함) + **profile-only** 예측·검증 (pred_va 기록, dcache에서 비교·stat만; timing/port 불변) | baseline(reg_vector 확장판)과 cycle-identical + 깔때기/정확도 stat 산출 |
| **2** | RFP Queue + drain + 실제 L1 probe (EXECUTED가 실측이 됨; covered fast path는 아직 OFF) | demand IPC 변화 ≈ 0 (repl-update 효과뿐), EXECUTED/INJECTED가 논문 shape |
| **3** | Validation fast path ON (`done_cycle` 단축) + `zereco_rf_covered` 연결 + assert 확장 + L1-miss policy 1 (mem_req 경로) | IPC ladder: baseline < rfp_timed ≤ oracle_stride; P-IQ filtering 동작 확인 |
| **4** | 무결성 counter 마감 + **§3.10 sweep matrix 전체 실행** (`zereco_dbg.json`의 `configurations`에 항목 추가; `simulations` 배열은 고정) | §5의 7개 항목 전부 통과 + §3.10의 계측 4종 산출 |

`reg_vector` 확장 근거: Target Load의 19%(SPEC 계열: deepsjeng 48.8%, leela 61.9%,
xz 86.5%의 H2P walk가 절단)가 태깅 단계에서 누락되어 scope=0 훈련 모집단과 coverage
분모를 왜곡한다. RFP 실험 전에 고쳐야 재실험을 피한다.

---

## 7. 결정 이력 (2026-08-14)

**확정**:

1. **L1-miss 정책**: drop/진행 양쪽 구현, 실험(A/B)으로 결정 — 논문에 "실험 근거로 선택" 기술 (§3.4, Phase 4)
2. **reg_vector 128-bit 확장**: Phase 0에 포함 (§6)
3. **PT 크기 sweep**: 최적 용량 결정 실험으로 Phase 4에 포함 (scope 0/1 각각)
4. **논문 서술 지침**: mechanism 절은 speculative wakeup + RFP-inflight bit를 원형대로 기술,
   시뮬레이터의 등가 모델링은 methodology에 명시 (§2.2)
5. **충실도 재점검**: §2.6 분류표 — LSQ 삽입/부분 태그 생략, utility·inflight 하드코딩 등
6. **Store 처리**: **abstain으로 진행** (§2.3). map-time `MEM_DATA_DEP` 기반 launch abstain
   + `scan_stores` 이중 검사, `RFP_ABSTAIN_STORE_DEP` stat 수집
7. **훈련 경로 (v4)**: Fill Buffer/BW walk 재활용 — **walk = Target Load PC 선별(PT 할당 권한),
   retire = 주소 상태 추적**. scope 필터가 `chain_bit`에서 **PT 멤버십**으로 바뀌어 Block Cache
   miss로 인한 훈련 누락이 사라진다 (§3.2, §3.7)
8. **RFP-inflight bit**: 시뮬레이터 미구현 / **논문 mechanism·저장량에는 포함**.
   Scarab의 wakeup은 "결과 확정 후 미래 ready cycle 통보" 방식이라 순 timing이 *완벽한
   hit-miss predictor*와 동일함을 §2.2에 코드 근거와 함께 명시
9. **실험 축 사전 개방 (v5)**: port 실패 정책(wait/drop/skip), port 우선순위(잔여/전용/
   prefetch-우선), queue 수명, stale-value 계측을 **Phase 0 시점에 파라미터로 정의**해
   나중에 재구현 없이 sweep 가능하게 했다 (§3.10)
10. **값 corner case (v5)**: Scarab은 값이 oracle이라 "잘못된 prefetch 값으로 branch가
    resolve되는" 사태를 만들 수 없다. 대신 **하드웨어에서 그 사태를 일으킬 유일한 조건**
    (주소는 맞고 probe 이후 store가 덮어쓰는 창)의 빈도를 계측한다 (§2.7)

**보류 (실측 후 재검토)**:

| 항목 | 재검토 시점 | 비용 |
|---|---|---|
| 케이스 2·3 store-data forwarding (`rfp_store_fwd_enable`) | Phase 1의 `RFP_ABSTAIN_STORE_DEP` 비율 확인 후 | 낮음 — dep store의 `done_cycle`을 `rfp_data_ready_cycle`로 쓰면 끝 (§2.3) |
| PT partial-tag aliasing, PAT, context prefetcher | 필요 시 | §2.4, §2.6 |

---

## 8. Phase 0 구현 기록 (2026-08-15)

빌드 확인 완료 (`./sci --build-scarab zereco_dbg`).

| 항목 | 위치 |
|---|---|
| `reg_vector` 64bit → `[(NUM_REG_IDS+63)/64]` | [dependency_chain_cache.h](../dependency_chain_cache.h), [.c:94](../dependency_chain_cache.c#L94) |
| 파라미터 17개 (전부 기본 비활성) | [core.param.def](../core.param.def) |
| Op 필드 6개 + 풀 리셋 | [op.h](../op.h), [op_pool.c](../op_pool.c) |
| 통계 ~90개 (깔때기/contention/corner case/무결성) | [zereco.stat.def](zereco.stat.def) |
| PT·queue 자료구조 + 결정론적 LCG (실구현) | [rfp.c](rfp.c) |
| 훅 7곳 배선 (본문은 Phase 1~3) | §4 표 |
| `file(GLOB CONFIGURE_DEPENDS)` | [CMakeLists.txt](../CMakeLists.txt) — 새 소스가 링크 단계에서 누락되던 문제 수정 |
| `oldest_first_chain_tag_profile` config 추가 | `scarab-infra/json/zereco_dbg.json` |

주의: `rfp_enable=1`로 켜도 Phase 1 전까지는 훅 본문이 비어 있어 아무 일도 일어나지 않는다.

**빌드/실행 관례**: 빌드는 `cd /home/lee/scarab-infra && ./sci --build-scarab zereco_dbg.json`.
실행(`./sci --sim zereco_dbg`)은 사용자가 직접 수행한다. 새 실험은 `configurations`에만
항목을 추가하고 `simulations` 배열(68 SimPoint)은 고정한다.

**실행 경로 참고** (`common/scripts/run_memtrace_single_simpoint.sh`): `PARAMS.<arch>`를
`PARAMS.in`으로 복사한 뒤 config string을 **CLI 인자로 override**한다. 따라서 `core.param.def`
기본값 → `PARAMS.golden_cove` → config string 순으로 우선순위가 올라간다. 새 RFP 파라미터는
기본값이 전부 비활성이라 golden_cove를 건드릴 필요가 없다. 결과는 SimPoint 디렉터리의
`zereco.stat.0.out`(및 `.csv`)에 나오며 `.warmup` 짝이 함께 생성된다.

---

## 9. Phase 1 결과 (2026-08-15, `260815_RFP_baseline_oldest_first`)

**검증 통과**: 3개 config가 68 SimPoint 전수에서 cycle-identical, 그리고 새 baseline이 기존
`zereco/baseline_oldest_first`와 완전 일치 — **reg_vector 수정도 Phase 1도 타이밍을 바꾸지
않았다.** §2.2의 "walk 결과를 아무도 읽지 않으면 timing 불변" 추론이 실측으로 확인됐다.

| 측정 | scope 0 (Target Load) | scope 1 (전체 load) |
|---|---:|---:|
| Target Load 비율 (PT_HIT / 전체 load) | 59.2% | 94.0% |
| injected | 29.0% | 46.7% |
| 주소 정확도 (correct / launched) | 92.6% | 95.1% |
| coverage 상한 (대역폭·timeliness 무한 가정) | 26.8% of loads / **45.4% of Target Loads** | 44.4% of loads |
| **PT 할당 / 축출** | **155K / 134K** | **18.9M / 18.9M** |

**논문에 없는 숫자 두 개**:

1. **PT 압력이 122배 차이난다.** 같은 1024-entry PT로 vanilla는 clang/gcc/deepsjeng/omnetpp에서
   **축출률 100%**(완전 스래싱)인데 ZERECO scope는 대부분 들어맞는다(omnetpp 1,673 할당 / 1 축출).
   Target Load 기준 coverage는 45.4% vs 44.4%로 동등하므로 — **훨씬 작은 predictor로 같은
   성능**이 complexity 주장의 실측 근거가 된다. 예외는 gcc(scope 0에서도 97% 축출)로, PT 크기
   sweep에서 주목할 워크로드다.
2. **store abstain 비용은 Target Load의 10.7%.** 그중 store 데이터가 이미 준비된 경우(논문
   case a, 즉시 forwarding)는 12.1%뿐이고 나머지는 store 완료를 기다려야 한다 ⇒ §2.3의
   보류 결정이 타당했고, 나중에 붙일 여지도 그만큼이다.

**설계 추론 확인**: validation 시점 store conflict가 347M load 중 **1,362건**. §2.3에서
"launch-시점 abstain이 `scan_stores`가 잡던 창을 거의 닫는다"고 한 추론 그대로다.

**기각된 우려**: `RFP_VALIDATE_REENTRY`(142.8M)를 port 경쟁으로 읽었으나 오독이었다 —
`STALL_ON_WAIT_MEM`이 기본 TRUE라 **miss 대기 op도 dcache stage에 잔류해 매 cycle 재계수**된다.
실제 L1D read port 사용률을 직접 재보니 **33.3%(2/3가 유휴)** 였다. 정확한 예측 하나당 demand
access가 사라지므로 순 대역폭 증가는 오예측분뿐(≈ +0.85%)이고, 기본 정책에서는 아무도 쓰지 않는
port cycle만 가져가므로 demand가 밀릴 수 없다.

---

## 10. Phase 2 구현 (2026-08-16)

빌드 확인 완료. 실험 디스크립터: `260816_RFP_phase2_oldest_first`, 4 config × 68 SimPoint.

**구현**: RFP Queue(tombstone FIFO) + 실제 L1 probe. `rfp_rename_launch`가 packet을 큐에
넣고(용량 초과 시 `RFP_DROP_QUEUE_FULL`), `rfp_queue_drain`이 stale/load-first/timeout/
store-conflict를 거른 뒤 port를 얻어 `cache_access`로 probe한다. **`rfp_try_validate`는 여전히
FALSE를 반환**하므로 covered load의 latency는 아직 줄지 않는다 — 다만 full/partial/late를
구분해 **coverage는 이번에 실측**된다.

| 정책 | 파라미터 | 구현 |
|---|---|---|
| port 우선순위 | `rfp_port_priority` | 0 잔여 / 1 전용 port(별도 `Ports` 풀) / 2 prefetch 우선(demand 루프 앞에서 drain) |
| port 실패 | `rfp_port_fail_policy` | 0 wait(head 유지) / 1 drop / 2 skip(younger가 앞지름) |
| L1 miss | `rfp_l1_miss_policy` | **0(drop)만 구현**; 1은 fill 콜백과 pending 테이블이 필요해 Phase 3. `rfp_init`이 1을 assert로 거부해 조용한 오작동을 막는다 |

**`rfp_probe_updates_repl` 기본값을 FALSE로 바꿨다.** `cache_access(update_repl=TRUE)`는
replacement 갱신만 하는 게 아니라 **line의 prefetch 비트를 지우고 `num_demand_access`를
증가**시킨다([cache_lib.c:230-235](../libs/cache_lib.c#L230)). 이 셋은 covered load가 자기
cache access를 생략하게 되는 시점(Phase 3)부터는 옳지만, 지금은 load가 여전히 접근하므로
**이중 계상**이 되고 `pref_throttlefb_on`(설정에서 ON)을 통해 stream prefetcher의 throttling
동작까지 바꿔 baseline 비교를 오염시킨다.

**그래서 Phase 2의 검증 조건이 강해진다 — baseline과 cycle-identical이어야 한다:**

| 조건 | 이유 |
|---|---|
| probe가 유휴 port만 사용 (`rfp_port_priority=0`) | demand가 밀릴 수 없음 |
| probe가 cache 상태를 바꾸지 않음 (`rfp_probe_updates_repl=0`) | hit rate·prefetcher 피드백 불변 |
| `rfp_try_validate`가 FALSE 반환 | load latency 불변 |

⇒ **`RFP_DEMAND_DELAYED_BY_PREFETCH_OPS == 0`이고 cycle이 baseline과 완전히 일치**해야 한다.
어긋나면 구현 버그다. (Phase 3에서 fast path와 함께 `rfp_probe_updates_repl=1`로 되돌린다.)

### 10.1 연구 목표 대비 남은 구멍 — RFP + P-IQ 결합

ZERECO의 본체는 "RFP가 primary, RF-uncovered residual slice에 P-IQ"인데 **그 결합 설정이 아직
실행 불가능하다.** RF filtering(`ZERECO_IQ_PRIORITY_POLICY=2`)은 `op->zereco_rf_covered`를 읽어
slice를 억제하는데, 이 플래그는 **여전히 기존 oracle만 설정**한다. timed RFP는 Phase 2까지
"측정만" 하므로 이 플래그를 세우지 않는다 — 세우면 실제로 빨라지지 않은 load를 covered로
보고하는 셈이 된다.

조용히 policy 1처럼 동작하는 것을 막기 위해
[dependency_chain_cache.c](../dependency_chain_cache.c)에 `ZERECO_IQ_PRIORITY_POLICY==2 &&
RFP_ENABLE` 조합을 **명시적으로 거부하는 assert**를 넣었다.

**Phase 3에서 함께 처리할 것** (셋이 한 묶음이다):
1. `rfp_try_validate`가 covered load를 실제로 완료시키고 `done_cycle`을 단축
2. 같은 자리에서 `op->zereco_rf_covered = TRUE`
3. 위 assert를 `(H2P_CHAIN_PERFECT_LOAD && oracle predictor) || RFP_ENABLE` 조건으로 완화

**그 전에 정해야 할 정의 — PARTIAL을 covered로 볼 것인가.** oracle에서는 timeliness가
공짜라 covered 판정이 "주소 일치 + store 충돌 없음"으로 끝났지만, timed 모델은 도착 시점이
FULL / PARTIAL / LATE로 갈린다. `zereco_ARCHITECTURE.md` §6.4의 네 번째 조건("dependent
wakeup을 실제로 앞당길 만큼 일찍 도착")을 PARTIAL은 부분적으로만 만족한다.

이 선택은 결과를 바꾼다 — §7.1이 "Target Load 하나라도 uncovered면 slice는 priority 유지"
이므로, PARTIAL을 covered로 치면 P-IQ 억제가 늘고 아니면 준다. `rfp_covered_requires_full`
파라미터로 열어두되, **Phase 2가 산출할 FULL/PARTIAL 분포를 보고 기본값을 정한다**:
PARTIAL이 미미하면 논쟁거리가 아니고, 상당하면 §3.10 sweep 축에 추가한다.

---

## 11. Phase 2 결과 (2026-08-16, `260816_RFP_phase2_oldest_first`)

**검증 4종 통과**: 3개 RFP config가 baseline과 cycle-identical,
`RFP_DEMAND_DELAYED_BY_PREFETCH_OPS`=0, `RFP_STALE_DEREF_BLOCKED`=0, 그리고 **Phase 1 상한
93.26M → Phase 2 실측 58.83M의 gap이 손실 채널로 99.92% 설명**된다(미설명 0.08%).

| | scope 0 (Target Load) | +dedicated port | scope 1 |
|---|---:|---:|---:|
| injected | 28.9% | 29.0% | 46.6% |
| executed | 18.3% | **22.5%** | 26.6% |
| useful | 16.9% | **21.0%** | 25.1% |
| 상한 도달률 | 63.1% | — | — |

**손실 내역** (injected 대비): LOAD_FIRST 26% / L1_MISS 11% / WRONG_ADDR 7.4% /
QUEUE_FULL 0.3%.

### 11.1 왜 PARTIAL이 FULL의 2배인가 — 예측 가능성과 run-ahead의 역상관

`saved = min(V−P, 4)` (V=validation, P=probe)이고 실측 `P ≈ R+2.5`(rename 후 2.5 cycle)이므로,
FULL이 되려면 **rename→AGU 거리가 6.5 cycle 이상**이어야 한다. 저장 cycle을 역산하면 두 무리가
선명히 갈린다:

| | rename→AGU | 원인 |
|---|---:|---|
| PARTIAL load | **~4.6 cy** | operand가 rename 시점에 이미 준비됨 — scheduling pipe만 지나고 실행 |
| FULL load | **~23.8 cy** | operand를 기다림 — 창이 넉넉 |

RFP 논문 §3.2도 같은 이분법을 보고한다(63% operand 미준비 / 37% 준비). 그런데 **우리는
PARTIAL이 67%로 훨씬 많다.** 이유는 eligible 조건이다: launch하려면 stride confidence가
saturate돼야 하고, 그런 load는 대개 **induction variable로 주소를 만드는 load**라 operand가
일찍 준비된다.

⇒ **"주소를 예측할 수 있다"와 "prefetch가 앞서 달릴 여유가 있다"가 서로 역상관**이다.
RFP 구조에 내재된 긴장이며, dedicated port가 FULL을 19.3M→26.5M(+37%)로 올려도 `V−R≈4.6`인
load는 원리적으로 4.6 cycle 이상 못 번다.

**큐는 제약이 아니다**: 평균 점유 1.35/64, full 0.06% cycle. 제약은 port(head-blocked 15.2%
cycle)이고, 그마저 평균 대기는 2.5 cycle이다. ⇒ `rfp_queue_entries` sweep은 불필요.

### 11.2 off-path 대역폭 — baseline 대비로 보면 초과분은 10%p

| | off-path 비율 |
|---|---:|
| baseline 기계의 실행된 memory op | **58.9%** |
| RFP probe (scope 0) | 68.8% |

이 기계는 **원래 wrong path가 59%**다. 69%는 "0에서 늘어난 것"이 아니라 **10%p 초과**다.
초과 원인은 RFP가 **rename에서 launch**하기 때문 — rename 시점 wrong-path 인구는 execute
시점보다 크다(실행 전에 squash되는 op도 packet은 이미 보냈다).

**정정**: 이전 서술에서 "Target Load는 squash 확률이 가장 높은 load"라 한 것은 **틀렸다.**
Target Load는 backward slice이므로 자기 H2P보다 **older**이고 그 branch의 오예측으로
squash되지 않는다. 실제 메커니즘은 **loop-carried** — 직전 iteration의 H2P가 이번 iteration의
Target Load를 날린다. 다만 이는 "H2P가 있는 코드 영역에 살기 때문"이지 slice 소속 자체의
효과가 아니며, 측정된 scope 0/1 차이(68.8% vs 63.5%)는 config 간 probe 수가 달라 교란된
비교다 — 정황일 뿐 증명이 아니다.

**수정한 통계 버그**: `RFP_LAUNCH_TO_PROBE_AVG` / `RFP_QUEUE_WAIT_AVG`의 분자가 전체 probe를,
분모가 on-path만 세어 대기시간을 off-path 비율만큼 부풀렸다(9.5 → 실제 2.54 cycle). 분자도
on-path로 게이팅.

---

## 12. Phase 3 구현 (2026-08-17)

빌드 확인 완료. 실험: `260817_RFP_phase3_oldest_first`, **7 config × 68 SimPoint**.

**드디어 IPC가 움직이는 첫 단계다.** `rfp_try_validate`가 covered load를 실제로 완료시킨다:

```
op->state = OS_SCHEDULED;  op->done_cycle = op->wake_cycle = deliver;
op->zereco_rf_covered = TRUE;      ← P-IQ의 RF filtering이 소비
wake_up_ops(op, REG_DATA_DEP, ...);
return TRUE;                       ← cache access·port 소비 없음
```

| 항목 | 내용 |
|---|---|
| **covered 판정 기준** (D1) | `rfp_covered_min_saved_cycles` (1~`DCACHE_CYCLES`). 1=어떤 절약이든 인정, 4=access를 통째로 제거한 경우만. §11.1에서 PARTIAL이 2배로 나왔으므로 이 축이 coverage를 3배 가른다 |
| **off-path launch** (D2) | `rfp_launch_offpath` 기본 **TRUE**(HW 충실). FALSE는 **오라클**이며 confidence gating이 회수할 수 있는 상한 측정 전용 — 제안이 아니다. inflight 카운트는 양쪽 모두 수행해 대역폭 효과만 분리한다 |
| **L1-miss policy 1** (D3) | `rfp_send_to_lower_levels` + `rfp_fill_done`(→`dcache_fill_line` 래핑) + pending 테이블. **coalesce된 요청은 콜백이 오지 않으므로** 슬롯 할당 시 죽은 owner를 lazy 회수한다(`RFP_PENDING_FILL_RECLAIMED`) |
| `rfp_probe_updates_repl` | **TRUE로 복귀** — covered load가 자기 access를 생략하므로 probe가 그 자리를 대신하는 것이 이제 옳다 |
| P-IQ assert | `H2P_CHAIN_PERFECT_LOAD \|\| RFP_ENABLE`로 완화. **RFP + RF-filtered P-IQ 결합이 이제 실행 가능**하다 (§10.1 해소) |

**실험 축**: `rfp_target`(메인) / `_dedicated`(대역폭이 상한인가) / `_l2`(miss 진행 가치) /
`_onpath`(오라클 상한) / `_full_only`(covered 기준 엄격단) / `rfp_all`(vanilla).

**주의**: Phase 3부터는 cycle-identical이 성립하지 않는다 — 그게 목적이다. 대신 확인할 것은
`RFP_DEMAND_DELAYED_BY_PREFETCH_OPS`=0(기본 정책)과 IPC가 baseline **이상**이라는 점,
그리고 `RFP_SAVED_CYCLES_AVG`(Phase 2 실측 2.68cy)가 실제 IPC 이득과 정합하는지다.
