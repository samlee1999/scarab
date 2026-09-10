# Critical-Path Slice Acceleration — 설계와 확정된 결과

> Last updated: 2026-09-08 · branch `test`
> **이 문서 = 설계 + 확정된 결과.** 할 일·미결정은 [zereco_TODO.md](zereco_TODO.md)에서만 관리한다.
> 참고 논문 정독 노트: [zereco_REFERENCE_NOTES.md](zereco_REFERENCE_NOTES.md)
> 기준 실험: `/home/lee/simulations/260908_critpath_comparison` (분석·그림은 그 안의 `analysis/`)

---

## A. 아이디어

**한 줄 요약** — H2P branch의 backward slice 중 **critical path에 속한 명령어만** 골라, 그 위의
명령어에는 issue priority를, 그 위의 예측 가능한 load에는 RF prefetch를 준다.

### A0. 왜 critical path인가

`cmp r1, r2` 처럼 branch의 producer가 둘이면, branch가 언제 resolve되는지는 **늦게 도착하는
쪽**이 정한다. 일찍 도착하는 쪽을 아무리 가속해도 branch resolution 시점은 그대로다.
PUBS·TEA는 slice 전체를 가속 대상으로 삼지만, 그중 실제로 resolution을 앞당기는 것은
critical path 하나다.

측정이 이를 뒷받침한다 — H2P branch의 fetch→resolution 67.5 cycle 중 dependency wait이
49.8 cycle(74%)이고, branch 자신의 select 대기는 2.6 cycle뿐이다(baseline, 67 simpoint).
가속해야 할 것은 branch가 아니라 그 operand를 만드는 chain이다.

### A1~A6. 여섯 개 핵심 로직

| # | 로직 | 하는 일 | 동작 시점 | 구조 | 출처 |
|---|---|---|---|---|---|
| **A1** | H2P 판별 | branch PC별 misprediction 이력으로 "자주 틀리는 branch" 선별 | branch resolve (갱신) / fetch (조회) | **HBT** — 1024 entry, 3-bit saturating counter, counter>1이면 H2P, 50K retire마다 decay | TEA HBT (= PUBS conf_tab과 동형) |
| **A2** | **critical producer 찾기** | 여러 source 중 **가장 늦게 ready된 source**의 producer를 식별 | wakeup (관찰) → issue (확정) → commit (사용) | **PRF scoreboard** (preg → producer PC) + **RSE의 LPR 필드** + **LPR tracker** (ROB entry별) | **본 연구 고유** |
| **A3** | critical chain 멤버십 | commit 때 "내가 chain 소속이면 내 critical producer도 소속"을 한 단계씩 전파 | commit | **brslice_tab** (PC → owner branch, depth, confirm counter) | PUBS 방식, physical register 기반으로 변경 |
| **A4** | Target Load 판정 | chain 위의 load 중 주소가 규칙적인 것 선별 | rename (조회) / commit (학습) | **Prefetch Table** — load PC별 stride predictor, 1-bit confidence(p=1/16), chain 멤버 load만 할당 | RFP 논문 |
| **A5** | priority scheduling | chain 명령어를 RS의 예약 entry에 배치해 먼저 issue | dispatch (배치) → select (실현) | **P-IQ** — RS의 25%를 priority partition으로 분할, non-stall fallback | PUBS |
| **A6** | RF prefetch | 예측 주소의 데이터를 load의 destination physical register로 미리 가져옴 | rename (발사) → store scan / L1 probe → AGU (검증) | **PT + Prefetch Queue + validate-then-use**, in-flight store가 있으면 store data forwarding, L1 miss면 하위 계층 fill | RFP 논문 |

### A2 상세 — LPR (Last Producer Register) 추적

```
rename   : destination preg 할당 → PRF scoreboard[preg] = 내 PC
wakeup   : source별로 producer tag가 broadcast되어 M bit set → DELAY shift → R bit
           R이 가장 마지막에 선 source의 preg를 RSE의 LPR 필드에 래치
issue    : LPR을 ROB entry(LPR tracker)로 고정
commit   : brslice_tab[내 PC] 조회 → chain 소속이면
           LPR → PRF scoreboard 역참조 → producer PC 획득
           → brslice_tab[producer PC].owner = 내 owner branch    (한 단계 전파)
```

PUBS와의 차이 셋: ① logical → **physical** register (rename이 WAW를 해소했으므로 항상 실제
dynamic producer), ② decode-time → **commit-time** 학습 (on-path만, wrong-path 오염 구조적
불가), ③ 모든 source → **last-ready source만** (critical path 필터).

전파 cadence는 PUBS와 같다 — dynamic instance당 한 level.

### 설계상 알려진 성질

- **walk가 저절로 끝난다.** dispatch 시점에 모든 source가 이미 ready였던 명령어는 LPR 이벤트가
  없어 전파하지 않는다 — operand wait이 실재하는 경계(frontier)에서 멈춘다.
- **store→load memory dependence.** PRF scoreboard만으로는 register edge만 따라갈 수 있다.
  현재 시뮬레이터는 forwarding store의 wake도 LPR 후보로 삼아, store가 마지막 도착이면 store PC로
  전파한다(critical edge의 약 2%). 하드웨어로 옮기려면 LPR 필드가 SQ entry를 가리키고 commit 때
  store PC를 읽는 확장이 필요하다 — 설계 반영 여부는 TODO D-11.
- **정적 PC 단위 멤버십은 누적된다.** 한 PC의 critical producer는 instance마다 바뀌므로(producer
  flip 약 22%), 시간이 지나면 LPR 규칙도 그 PC의 모든 producer를 방문한다. 그 결과 critical
  규칙과 full-slice 규칙의 멤버 집합이 거의 같아진다(C3). 필터의 단위를 바꾸는 문제는 TODO D-12.

### 확정된 설계 결정

| 항목 | 결정 | 근거 |
|---|---|---|
| chain seed 게이트 | **H2P-only** (HBT counter > 1) | 전 branch 삽입은 멤버 인구만 키움 |
| membership decay | 100K retire마다 confirm −1, 0이면 탈퇴 | phase 적응용 |
| retention threshold | 불채택 | 재확인이 노화를 압도해 인구가 줄지 않음 |
| Δ-window | Δ=0 (last producer만) | TODO D-2 |
| depth 제한 | 없음 | 인구를 줄이는 유일한 지렛대 — TODO D-4 |
| Prefetch Table | 1K entry, 8-way, 1-bit confidence(p=1/16) | 512~∞에서 성능 차 0.1%p 이내; 병목은 주소 예측 가능성 |
| slice edge 집합 | **register edge만** (`zereco_critpath_mem_edge 0`) — store→load 의존은 추적하지 않는 경우로 고정 | PRF scoreboard로 볼 수 있는 것과 일치. store edge를 포함하면 멤버 +4%p, IPC +0.3%p이나 LPR 필드가 SQ entry를 가리키는 확장이 필요하다 |
| brslice_tab 할당 정책 | **멤버 전용** (`zereco_critpath_member_only_alloc 1`) — H2P seed이거나 전파가 닿은 PC만 entry를 갖는다 | 모든 commit PC에 entry를 잡으면 테이블 점유가 chain이 아니라 프로그램 전체 static PC 수를 따라가, 용량 연구가 엉뚱한 구조를 재게 된다 |
| RFP store forwarding | **모드 1** — 예측 주소가 forwarding store의 주소와 맞으면 store 완료 시 store data를 RF로 (`rfp_store_forward 1`) | 옛 보수 모델(포기)이 버리던 Target Load 19%의 절반을 회수 (C5) |
| RFP L1-miss 정책 | 하위 계층 fill 진행 | fill path가 RFP 이득의 지배 성분 |
| P-IQ partition | **25%** (352의 88 entry), non-stall fallback | baseline RS 점유율 ≈ 25%에 맞춤. 실측 점유는 partition의 15%로 여유 있음 (C4, TODO D-10) |
| 옛 Fill-Buffer walk | 완전 배제 (`LEGACY_WALK_NEEDED()`) | Target Load 지명을 오염시켰음 |
| 백엔드 머신 | `PARAMS.golden_cove_rs352`: RS 285/204/55 = 544 − TEA 예약 192 → main **352**, dcache 2 read port × 1 bank, PRF 592, issue 8 / retire 16, LQ 256 / SQ 192, BTB 8K, MSHR 64 | 모든 config 공통. partition %의 분모 = 352 |
| 스케줄러 | random queue (`node_issue_queue_schedule_scheme 1`) | PUBS와 같은 base; P-IQ의 select 우선권은 그 위에 얹힘 |
| 표본 | **67 simpoint** (workload당 5, clang 4, gcc 3; 14 workload) | TEA 참조 실험과 동일 표본. weight 가중 → workload 간 geomean |

### 아키텍처 그림 (논문용)

`figures/draw_arch.py`가 생성 (`python3 figures/draw_arch.py`; svg·pdf·png). 그림 2·3(LPR, 멤버십 전파)은 사용자 원본 그림.

| 로직 | 파일 | 내용 |
|---|---|---|
| 1. H2P 판별 | `figures/fig1_h2p_hbt.*` | HBT 갱신(resolve)·조회(fetch)·decay·commit 때 chain root 삽입 |
| 4. Target Load + 주소 예측 | `figures/fig4_target_load_pt.*` | commit: 멤버십 → PT 할당·stride 학습; rename: 예측 주소 생성 |
| 5. priority scheduling | `figures/fig5_priority_iq.*` | 분할 RS(priority/normal free list), non-stall fallback, select 우선권 |
| 6. RF prefetch | `figures/fig6_rfp.*` | rename 직후 launch, store scan(forwarding), spare-port L1 probe, AGU 검증 |

---

## B. 코드 ↔ 아키텍처 매핑

| 아키텍처 요소 | 상태 | 코드 |
|---|---|---|
| **A1** HBT | 재사용 | `bp/hbt.c` — 1024 entry, 3-bit, threshold>1, decay 50K |
| **A2** LPR 관측 | 구현 | `cmp_model.c: cmp_wake()` 훅 — wakeup의 max에 argmax를 얹음. producer PC는 wake 시점에 포착(소비자 commit 전에는 preg가 재할당되지 않으므로 scoreboard 역참조와 등가) |
| **A3** brslice_tab | 구현 | `zereco/critpath.c` — 4096 sets × 8-way LRU, `{in_slice, depth, owner_pc, confirm}`; commit 훅(`critpath_note_retire`)에서 seed·전파·decay |
| **A4** Target Load → PT | 구현 | `critpath_note_retire()` → `rfp_note_target_load()` (멤버 load만 PT 할당/refresh) |
| **A5** P-IQ | 재사용, 입력만 교체 | **decoupled frontend**(FTQ에서 op를 꺼낼 때, rename 이전)가 brslice_tab을 PC로 조회해 priority bit 부착 → `node_issue_queue.cc`(partition·fallback·select)/`exec_ports.c`(partition 크기) |
| **A6** RFP | 재사용 + store forwarding | `zereco/rfp.c`. `rfp_store_forward`: 0 = forwarding store가 있는 load는 포기, **1** = 예측 주소가 store 주소와 맞으면 forwarding 패킷(queue·L1 port 사용 없음, store 완료 +1 cycle에 RF 도착, validation의 older-store/stale 검사 생략), 2 = launch 시 이미 실행된 store만 |
| H2P resolution profiler | 재사용 | `zereco/h2p_mispred_latency.c` (`zereco_h2p_mispred_latency_profile 1`) |
| **full-slice 비교 모드** (PUBS식) | 구현 | `zereco_critpath_full_slice 1`: 전파만 다름 — 모든 register source의 producer로 전파, frontier 종료 없음. producer PC는 rename map(`Map_Entry.pc`, PUBS의 def_tab)에서 source별로 op에 복사(`critpath_src_producer_pc[]`). 같은 실행에서 **shadow critical table**이 critical 규칙을 병행 적용해 멤버/priority op/Target Load를 critical·non-critical로 분류(`CRITPATH_FULL_MEMBER_*`, `CRITPATH_PRIORITY_OP_*`, `CRITPATH_TARGET_LOAD_*`) |
| 옛 identification (Fill Buffer + batch walk + Block Cache) | 비활성 | `LEGACY_WALK_NEEDED()`가 false면 fill·trigger·엔진 모두 skip |

파라미터: `zereco_critpath_profile`(관측·멤버십), `zereco_critpath_priority`(A5 입력), `rfp_target_critpath`(A4 입력),
`zereco_critpath_decay_interval`, `_insert_gate`, `_priority_max_depth`, `_table_sets/_assoc`, `_confirm_bits`,
`zereco_critpath_full_slice`, `zereco_piq_enable/_entry_percent/_dispatch_policy`, `rfp_enable/_pt_entries/_store_forward`.

---

## C. 확정된 결과 (`260908_critpath_comparison`)

**설정.** 352-entry 머신, random queue, 67 simpoint. 6 config = {P-IQ, RFP, P-IQ+RFP} × {critical slice, full slice}.
P-IQ 25% partition non-stall, RFP PT 1K + store forwarding 모드 1, decay 100K, depth 무제한.
baseline = `260905_critpath_phaseB/baseline_randq`, TEA = `260827_tea_baseline/tea_random_queue`
(TEA thread용 RS 192·PRF 192를 **추가로** 갖는 544-entry 머신, 옛 빌드라 IPC만 비교 가능).
집계: workload 안에서 SimPoint weight 정규화, IPC는 workload 간 geomean, latency 감소율은 workload 간 산술평균,
비율 통계는 가중 카운터를 합한 뒤 나눔. 스크립트 `analysis/analyze.py`(→ `results67.txt`), 그림 `analysis/plot_cmp67.py`, `plot_pie.py`.

### C1. IPC

| | piq/crit | piq/full | rfp/crit | rfp/full | **both/crit** | both/full | TEA |
|---|---|---|---|---|---|---|---|
| geomean (14 workload) | +4.20% | +4.06% | +6.20% | +6.11% | **+10.08%** | +9.66% | +14.21% |
| GAP (정규화 IPC) | 1.060 | 1.059 | 1.087 | 1.087 | **1.142** | 1.137 | 1.118 |
| SPEC17 | 1.032 | 1.031 | 1.035 | 1.032 | **1.066** | 1.063 | 1.221 |
| Datacenter | 1.024 | 1.021 | 1.057 | 1.058 | **1.078** | 1.074 | 1.067 |

- 두 메커니즘은 가산적이다(4.2 + 6.2 ≈ 10.1). workload별로는 bfs/cc/bc 16~23%, mcf·xgboost 13~15%, omnetpp·pr 3~4%.
- TEA는 SPEC17(leela +27%, mcf +35%, omnetpp +18%, xz +18%)에서 크게 앞서고 GAP·Datacenter에서는 both/crit이 같거나 앞선다. pr은 TEA −9.9%.
- 그림 `analysis/cmp67_ipc.pdf`.

### C2. H2P branch resolution latency (baseline: fetch→resolution 67.5 cy, dependency wait 49.8 cy, scheduler wait 2.6 cy)

| | piq/crit | rfp/crit | both/crit | both/full |
|---|---|---|---|---|
| fetch→resolution | −7.2% | −11.8% | **−17.7%** | −17.2% |
| dependency wait | −4.6% | −14.8% | **−19.5%** | −18.8% |
| scheduler wait | −88% | −21% | **−90%** | −90% |

P-IQ는 scheduler wait를 없애고, RFP는 dependency wait를 줄인다. RFP 단독으로도 GAP에서 scheduler wait가 38% 주는데,
load가 일찍 끝나 branch가 RS에 머무는 시간 자체가 줄기 때문이다. 그림 `analysis/cmp67_{f2r,dep,sched}.pdf`.

### C3. critical slice vs full slice — edge 집합을 맞춰도 정적 PC 단위에서는 6~7%만 거른다 (`260909_critpath_timeline`, edge-set run)

RFP + P-IQ 25%, 67 simpoint. `zereco_critpath_mem_edge` 0(register edge만) / 1(store→load forwarding edge 포함)을 두 규칙에 같이 적용해
critical ⊆ full을 보장한 비교. 스크립트 `analysis/analyze_edgeset.py` → `edgeset_results.txt`.

| | crit/reg | full/reg | crit/mem | full/mem |
|---|---|---|---|---|
| IPC geomean | +9.79% | +9.66% | **+10.08%** | +9.96% |
| H2P fetch→resolution / dependency wait | −17.4% / −18.9% | −17.2% / −18.8% | −17.7% / −19.5% | −17.7% / −19.5% |
| chain 멤버 (committed op 중) | 59.7% | 62.3% | 63.6% | 67.0% |
| 멤버 중 store commit | 4.4% (push/call류: register dest가 있는 store) | 4.4% | 6.4% | 7.0% |
| brslice_tab 상주 멤버 PC | 1037 | 1259 | 1169 | 1489 |
| full 멤버 중 critical 규칙이 버릴 것 | — | **6.0%** | — | **7.3%** |
| priority bit op 중 non-critical | — | 6.1% (GAP 5.1 / SPEC17 6.3 / DC 8.4) | — | 7.5% (5.4 / 7.9 / 11.6) |
| Target Load 중 non-critical | — | 3.5% (0.7 / 4.4 / 6.9) | — | 4.2% (0.6 / 5.4 / 8.1) |

- store edge를 포함하면 두 규칙 모두 멤버가 3.9~4.7%p 늘고(store의 data chain), IPC는 0.3%p 오른다. critical의 우위(+0.1%p)는 어느 edge 집합에서도 잡음 수준.
- **무엇을 세느냐로 filtering 폭이 갈린다.** priority bit는 non-stall fallback에서 지워지므로 commit 시점에 bit가 살아 있는 op = 실제로 priority RS entry를 잡은 op이다(dispatch 189,347,846 vs commit 189,347,561, 차이는 종료 시 in-flight).

  | 세는 대상 (register edge) | GAP | SPEC17 | Datacenter | 전체 |
  |---|---|---|---|---|
  | brslice_tab 상주 **PC 수** | 15.5% | 16.2% | 21.0% | 19.1% |
  | 멤버 **commit 수** | 2.7% | 4.9% | 6.6% | 4.2% |
  | **실제 priority로 issue된 op 수** | 2.7% | 4.5% | 6.2% | 4.0% |

  PC 수로는 5분의 1을 걸러내지만 실제로 우선순위를 받고 실행되는 명령어는 4~5%만 줄어든다 — 걸러지는 PC가 평균 멤버보다 약 3배 덜 실행되는 cold PC이기 때문이다. config를 따로 돌린 4.0%가 같은 실행 shadow의 6.1%보다 작은 것은 피드백 때문이다: critical만 가속하면 criticality가 다른 source로 옮겨가 멤버가 다시 늘어난다(crit 실행의 자체 테이블 1037 PC > full 실행 안 shadow 추정 1020 PC).
- **왜 필터링이 구조적으로 작을 수밖에 없는가 — dataflow가 대부분 선형이다.** 커밋되는 op의 **67.9%가 waking source가 하나뿐**이고(wake 0개 12.9%, 2개 이상 19.3%), 멤버 op으로 좁혀도 **71.4%가 도착이 하나뿐**이다. producer가 하나면 critical 규칙과 full 규칙이 같은 곳으로 전파하므로 두 규칙이 원리적으로 구분되지 않는다. 갈릴 수 있는 모집단은 **경쟁 도착이 있는 멤버 op 17.9%뿐**이다.

  그 결과 전파 이벤트 수는 멤버 commit당 critical 0.661 / full 0.900 — full이 방문하는 producer가 **1.36배**에 그친다. 그리고 그 추가 방문의 99.9%는 이미 멤버인 PC의 refresh다. 즉 필터링 상한은 두 단계로 깎인다: ① 프로그램 dataflow가 포크하지 않아 "추가 전파"가 27%뿐, ② 정적 PC 누적으로 그 추가 전파가 대부분 기존 멤버에 떨어져 최종 4%만 남는다. ②는 depth 제한으로 공격할 수 있으나 ①은 프로그램의 성질이다.

- **더 큰 문제는 절대량이다.** 어느 규칙이든 커밋 op의 **55~61%가 priority scheduling을 받는다**(crit/reg 54.9%, full/reg 57.2%, crit/mem 58.5%, full/mem 61.4%). 25% partition인데도 이렇게 되는 것은 RS 점유가 낮고 priority op가 0.47 cycle 만에 빠져나가 구획을 빠르게 회전시키기 때문이다(fallback 8%). "full 대비 몇 % 필터링"이 아니라 **가속 대상 자체를 줄이는 것**이 과제다 → TODO D-12. 그림 `analysis/edgeset_noncrit.pdf`.
- crit/mem은 `260908_critpath_comparison`의 piq_rfp_critical_slice와 simpoint별 IPC가 **정확히 동일**(타임라인 덤프가 타이밍에 무영향임을 증명). full/mem은 store edge를 새로 따라가므로 260908의 full과 다르다.

### C4. priority scheduling (25% partition = 88 entry)

| | both/crit | both/full |
|---|---|---|
| 평균 상주 priority op (RS0~2 합) | 13.6 (partition의 15.4%, RS의 3.9%) | 13.3 |
| 평균 상주 normal op | 56.2 (RS의 16.0%) | 56.8 |
| priority partition full cycle | 6.1% (GAP 4.4 / SPEC17 6.2 / Datacenter 12.5) | 5.6% |
| fallback (partition full → normal entry, bit 상실) | 8.1% | 8.1% |
| ready→issue: priority / normal / fallback | 0.47 / 2.88 / 3.21 cy | 0.50 / 2.72 / 3.44 cy |
| zero-wait issue: priority / normal | 75.9% / 54.4% | 75.1% / 55.9% |

partition이 우선권을 실제로 전달하지만(0.5 vs 2.9 cy) 예약은 남아돈다. Datacenter만 압박이 있다. 스크립트 `analysis/rs_occupancy_and_pie.py`.

### C5. RF prefetch와 Target Load의 처리 (both/crit)

funnel: load의 66.5%가 Target Load(PT hit), 그중 eligible 37.8%, injected 36.9%(forwarding 패킷 17.2% 포함), useful 41.1%(load 기준 27.4%), 주소 정확도 95.4%, PT eviction/alloc 1.0, queue full 1.1%.

| Target Load 처리 | 비율 |
|---|---|
| useful — L1에서 prefetch | 31.6% |
| useful — store forwarding | 9.5% |
| No data in time (prefetch가 load보다 늦음) | 11.7% |
| Wrong address | 2.6% |
| Queue full | 1.1% |
| Store dep. not forwardable (예측 주소 ≠ store 주소, 또는 forwarding store 둘 이상) | 0.3% |
| Low confidence (stride 예측 불가) | 43.2% |

카테고리는 파이프라인 순서의 배타적 분류이고 합은 Target Load 수와 4개 차이로 일치한다. 그림 `analysis/target_load_outcome.pdf`.
남은 손실의 대부분은 주소 예측 불가(43%)이고, store forwarding이 옛 "store abstain 19%"의 절반을 회수했다.
forwarding 없는 이전 설정(`260905_critpath_B2_partition/b2_part25`) 대비 simpoint별 IPC +1.0% 평균(−0.6 ~ +5.9%).

### C6. 무결성

`ZERECO_IQ_*_INTEGRITY_MISMATCHES`, `ZERECO_PIQ_*_INTEGRITY_MISMATCHES`, `RFP_DEMAND_DELAYED_BY_PREFETCH_OPS` 전부 0.
`RFP_INFLIGHT_UNDERFLOW`만 injected의 0.02% 수준(PT 축출 후 같은 PC 재할당 시 카운터 0에서 감소, 무해).

### C7. 멤버십 누적 타임라인 (`260909_critpath_timeline`, 4 config, 100K commit 간격, warm-up 포함)

그림 `analysis/edgeset_timeline_{share,cum,live}.pdf`. 집계는 simpoint 곡선을 workload 안에서 weight 평균, suite 안에서 산술평균.

- **build-up 구간이 없다.** 첫 200K 명령어에서 이미 네 곡선이 최종 수준에 도달하고(GAP 64~69%, SPEC17 61~72%, Datacenter 40~48%), 20M까지 평행하다. 정적 PC 멤버십은 수십만 명령어 안에 포화하므로 "full이 먼저 크고 critical이 따라붙는" 국면은 100K 해상도에서는 보이지 않는다.
- 같은 edge 집합 안에서는 처음부터 끝까지 full이 critical보다 1.3~3.6%p 위에 있고, store edge를 추가하면 두 규칙이 함께 3~5%p 올라간다.
- 처음(2 config, edge 불일치) 버전에서는 critical이 full보다 1~2%p **높았다** — critical만 store→load edge를 따라가 store의 data chain을 멤버로 만들었기 때문. 이 발견이 `zereco_critpath_mem_edge` knob의 계기.

### C8. brslice_tab 용량 sweep (`260909_critpath_brslice_tab_size`, {critical, full} × 8-way {512~32K} entry)

register-only edge, 멤버 전용 할당, P-IQ 25%, RFP PT 1K + store forwarding. 스크립트 `analysis/analyze_tabsize.py` → `tabsize_results.txt`, 그림 `analysis/tabsize_ipc.pdf`.

| entry | 512 | 1K | 2K | 4K | 32K |
|---|---|---|---|---|---|
| IPC — critical | +9.61% | +9.73% | +9.78% | +9.81% | +9.83% |
| IPC — full | +9.41% | +9.54% | +9.61% | +9.65% | +9.66% |
| critical − full | +0.20%p | +0.19%p | +0.18%p | +0.16%p | +0.17%p |
| 상주 멤버 PC (crit / full) | 256 / 266 | 432 / 458 | 656 / 717 | 866 / 981 | 1079 / 1313 |
| 멤버 축출 / 신규 멤버 (full) | 1.08 | 1.04 | 0.99 | 0.85 | 0.01 |
| 커밋 op 중 priority issue (crit / full) | 47.4 / 48.8% | 51.2 / 52.8% | 53.3 / 55.2% | 54.4 / 56.5% | 55.0 / 57.3% |
| filtering: priority op non-critical | 7.8% | 6.8% | 6.4% | 6.2% | 6.1% |

**필터링의 핵심 지표 — 실제로 priority scheduling을 받고 commit된 명령어의 비율** (그림 `analysis/prio_filtering.pdf`, 수치 `prio_filtering.txt`).

| entry | 512 | 1K | 2K | 4K | 32K |
|---|---|---|---|---|---|
| full | 48.8% | 52.8% | 55.2% | 56.5% | 57.3% |
| critical | 47.4% | 51.2% | 53.3% | 54.4% | 55.0% |
| 차이 | 1.34%p | 1.66%p | 1.89%p | 2.06%p | 2.30%p |
| **상대 필터링** | **2.7%** | **3.1%** | **3.4%** | **3.6%** | **4.0%** |
| suite별 상대 필터링 (GAP / SPEC17 / DC) | 2.7 / 1.9 / 4.9% | 2.7 / 3.2 / 4.4% | 2.7 / 3.8 / 4.4% | 2.7 / 4.4 / 4.3% | 2.7 / 4.5 / 6.2% |

- **가속 대상 기준 필터링은 4.0%다**(32K, 절대 개수로 189.6M → 182.0M). 멤버 commit 기준 4.2%와 거의 같고, 앞서 shadow table로 잰 6.1%보다 작다.
- **용량 압박은 필터링을 오히려 약화시킨다** (4.0% → 2.7%). LRU가 먼저 버리는 cold PC가 바로 critical 규칙이 걸러낼 PC이므로, 압박이 커지면 full도 그것을 잃어 두 규칙이 수렴한다. GAP은 멤버가 ~60 PC뿐이라 전 구간 평탄(2.7%), SPEC17은 4.5% → 1.9%로 붕괴, Datacenter만 4~6%를 유지한다.
- 같은 실행의 shadow table로 재면 반대로 6.1% → 7.8%로 오르는데, 이는 **shadow table 자신이 압박을 받아 자기 멤버를 놓치는 계측 오차**다. config를 따로 돌린 위 수치가 신뢰할 값이다.

- **용량은 두 규칙을 가르는 축이 아니다.** 32K → 512로 64배 줄여도 IPC는 critical −0.22%p, full −0.25%p만 잃고, 둘의 격차는 0.17 → 0.20%p로 거의 그대로다(SPEC17만 0.21 → 0.31%p). full이 무너지고 critical이 버티는 예산은 없다.
- **대신 강한 비용 결과가 나왔다.** 테이블 압박은 실재한다(512 entry에서 축출/신규 = 1.08, 상주 멤버 1313 → 266). 그런데도 성능이 거의 그대로다 — **brslice_tab은 1K entry(PUBS 예산)면 충분하고, 512 entry에서도 −0.2%p뿐**이다. 식별 구조는 싸다.
- **왜 성능이 안 떨어지나: LRU가 cold PC부터 버리기 때문이다.** 상주 멤버 PC는 4배 줄어드는데(1079 → 256) 실제로 priority를 받는 커밋 op 비율은 55.0 → 47.4%로 밖에 안 준다. C3에서 본 cold/hot 비대칭이 용량 축에서도 그대로 재현된다.
- **용량 압박이 chain을 얕게 만드는 효과는 약하다.** chain size 8.9 → 7.7, depth ≤ 2 비중 48.8 → 51.0%. 전파가 끊겨 depth 제한처럼 작동하는 효과는 있으나 2%p 수준이다.
- filtering은 압박이 커질수록 6.1 → 7.8%로 조금 오르지만 20%와는 거리가 멀다. H2P latency(−17.5 → −17.2%), RFP useful/load(26.1 → 25.7%)도 거의 불변.
- **다음 지렛대는 depth다.** 멤버 commit의 **48.8%가 depth ≤ 2**이므로 `zereco_critpath_priority_max_depth 2`는 가속 대상을 절반으로 잘라 멤버 비율 약 30%를 만든다(유도값). 목표 구간 20~30%대에 직접 닿는 유일한 knob이다 → 실험 B-3.

### C9. chain depth 제한 sweep (`260910_critpath_depth`, `zereco_critpath_priority_max_depth` ∞/8/4/2/1 × {critical, full})

brslice_tab 1K entry 고정, register-only edge, P-IQ 25%, RFP PT 1K + store forwarding. `max_depth`는 **소비만** 막는다(frontend priority bit, Target Load 지명). 전파는 chain 전체를 계속 걷는다.
정합성 확인: `crit_inf`/`full_inf`가 C8의 `crit_1k`/`full_1k`와 simpoint별 IPC **완전 일치**(Δ = 0). 스크립트 `analysis/analyze_depth.py`, 그림 `analysis/depth_tradeoff.pdf`.

**가속 대상은 목표 구간에 들어간다 — 하지만 이득도 같은 비율로 사라진다.**

| max_depth | ∞ | 8 | 4 | 2 | 1 |
|---|---|---|---|---|---|
| 가속 대상 (critical) | 51.2% | 42.8% | 35.3% | **25.2%** | 15.5% |
| IPC 이득 (critical) | +9.73% | +8.37% | +7.13% | **+4.92%** | +2.97% |
| 인구 잔존 / 이득 잔존 | 100 / 100% | 84 / 86% | 69 / 73% | **49 / 51%** | 30 / 31% |

전체로는 **인구 잔존율과 이득 잔존율이 거의 같다** — 효율(이득 ÷ 인구)이 0.190 → 0.202 → 0.195로 평평하다. depth로 인구를 절반 줄이면 이득도 절반 사라진다.

**그런데 suite별로 보면 코드가 큰 워크로드에는 sweet spot이 있다.**

| | GAP | SPEC17 | Datacenter |
|---|---|---|---|
| ∞ 효율 (IPC 이득 ÷ 가속 대상%) | 0.234 | 0.114 | 0.249 |
| depth 4 효율 | 0.196 | 0.148 | **0.393** |
| depth 4에서 인구 잔존 / 이득 잔존 | 81 / 68% | 60 / 78% | **54 / 85%** |
| depth 4의 가속 대상 | 47.8% | 33.3% | **16.4%** |

- **Datacenter는 depth 4에서 인구를 절반으로 줄이고 이득의 85%를 유지**한다(효율 1.58배). 가속 대상 16.4%로 목표 구간 아래이면서 이득 +6.45%. SPEC17도 1.30배로 완만한 sweet spot(depth 4, 가속 33.3%, 이득 78% 유지).
- **GAP은 sweet spot이 없다**(효율 0.234 → 0.196). 작은 커널이라 chain이 짧고 깊은 노드도 똑같이 기여한다.
- **depth 2 아래로는 어디서나 무너진다.** Datacenter 효율이 0.393(d4) → 0.200(d2)으로 ∞보다도 나빠진다. 경계는 depth 4와 2 사이다.
- 원인은 dependency wait에 있다: 감소폭이 −18.9%(∞) → −13.4%(d4) → −6.5%(d2) → −0.8%(d1)로 무너진다. depth 제한은 **상류 chain을 자르는데, dependency wait을 만드는 것이 바로 그 상류**다. 반면 scheduler wait 감소는 −89 → −95%로 오히려 좋아진다(가속 대상이 줄어 구획 경쟁이 완화).
- 손실의 주 성분은 RFP다: Target Load/loads 62.3 → 45.8(d4) → 31.6%(d2), useful/load 25.9 → 16.3 → 10.9%. **Target Load는 chain 깊은 곳에 산다.**
- **depth 제한은 critical vs full 필터링을 개선하지 못한다**: 상대 필터링 3.1%(∞) → 8.0%(d8) → 4.9%(d4) → 1.7%(d2). 목표 구간에서 오히려 나빠진다. 참고로 full 규칙은 여러 경로가 닿아 depth를 최소값으로 받으므로 chain이 얕게 기록되고, 그래서 d8에서 full이 거의 손실이 없다(GAP full d8 = ∞와 동일).
