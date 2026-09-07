# Critical-Path Slice Acceleration — 설계와 확정된 결과

> Last updated: 2026-09-08 · branch `test`
> **이 문서 = 설계 + 확정된 결과.** 할 일·미결정은 [zereco_TODO.md](zereco_TODO.md)에서만 관리한다.
> 참고 논문 정독 노트: [zereco_REFERENCE_NOTES.md](zereco_REFERENCE_NOTES.md)

---

## A. 아이디어

**한 줄 요약** — H2P branch의 backward slice 중 **critical path에 속한 명령어만** 골라, 그 위의
명령어에는 issue priority를, 그 위의 예측 가능한 load에는 RF prefetch를 준다.

### A0. 왜 critical path만인가

`cmp r1, r2` 처럼 branch의 producer가 둘이면, branch가 언제 resolve되는지는 **늦게 도착하는
쪽**이 정한다. 일찍 도착하는 쪽을 아무리 가속해도 branch resolution 시점은 그대로다.
PUBS·TEA는 slice 전체를 가속 대상으로 삼지만, 그중 실제로 resolution을 앞당기는 것은
critical path 하나다.

측정이 이를 뒷받침한다 — H2P branch의 fetch→resolution 69.9 cycle 중 dependency wait이
51.5 cycle(74%)이고, branch 자신의 select 대기는 2.7 cycle뿐이다. 가속해야 할 것은 branch가
아니라 그 operand를 만드는 chain이다.

### A1~A6. 여섯 개 핵심 로직

| # | 로직 | 하는 일 | 동작 시점 | 구조 | 출처 |
|---|---|---|---|---|---|
| **A1** | H2P 판별 | branch PC별 misprediction 이력으로 "자주 틀리는 branch" 선별 | branch retire (갱신) | **HBT** — 3-bit saturating counter, counter>1이면 H2P, 50K retire마다 decay | TEA HBT (= PUBS conf_tab과 동형) |
| **A2** | **critical producer 찾기** | 여러 source 중 **가장 늦게 ready된 source**의 producer를 식별 | wakeup (관찰) → issue (확정) → commit (사용) | **PRF scoreboard** (preg → producer PC) + **RSE의 LPR 필드** + **LPR tracker** (ROB entry별) | **본 연구 고유** |
| **A3** | critical chain 멤버십 | commit 때 "내가 chain 소속이면 내 critical producer도 소속"을 한 단계씩 전파 | commit | **brslice_tab** (PC → owner branch, depth, decay counter) | PUBS 방식, physical register 기반으로 변경 |
| **A4** | Target Load 판정 | chain 위의 load 중 주소가 규칙적인 것 선별 | rename (조회) / retire (학습) | **Prefetch Table** — PC별 stride predictor, 1-bit confidence(p=1/16) | RFP 논문 |
| **A5** | priority scheduling | chain 명령어를 IQ의 예약 entry에 배치해 먼저 issue | dispatch (배치) → select (실현) | **P-IQ** — 유한 partition + non-stall fallback | PUBS |
| **A6** | RF prefetch | 예측 주소의 데이터를 load의 destination physical register로 미리 가져옴 | rename (발사) → L1 probe → AGU (검증) | **PT + RFP Queue + validate-then-use**, L1 miss면 하위 계층 fill | RFP 논문 |

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

전파 cadence는 PUBS와 같다 — dynamic instance당 한 level. 실측 refresh:new = 350:1로 warm-up은
무시할 수준이다.

### 설계상 알려진 성질

**walk가 저절로 끝난다.** dispatch 시점에 모든 source가 이미 ready였던 명령어는 LPR 이벤트가
없어 전파하지 않는다 — operand wait이 실재하는 경계에서 멈춘다. 실측으로 chain 명령어의 7.8%가
이 조건으로 종료된다.

**store→load memory dependence는 추적 불가.** PRF 기반이라 register edge만 따라간다. 실측
critical edge의 **2.3%**만 store→load라 손실은 미미하다 (확정, 한계로 서술).

### 확정된 설계 결정

| 항목 | 결정 | 근거 |
|---|---|---|
| chain seed 게이트 | **H2P-only** (HBT counter > 1) | 전 branch 삽입은 멤버 인구 68 → 82%로 악화 (A2) |
| membership decay | 100K retire마다 counter −1, 0이면 탈퇴 | phase 적응용. 인구는 거의 안 줄임(A2) |
| retention threshold | **불채택** | threshold 0→8+에 인구 68.1→67.2%로 평평. 재확인이 노화를 압도(1792:1) |
| Δ-window | Δ=0 (last producer만) 유지 | tie(slack≤2) 25~27%이나 성능이 충분 — 재검토는 TODO D-2 |
| Prefetch Table | **1K entry** (512도 −0.07%p) | B-1: 크기 무관, 병목은 주소 예측 가능성 |
| L1-miss 정책 | 하위 계층 fill 진행 | fill path가 RFP 이득의 지배 성분 |
| 옛 Fill-Buffer walk | **완전 배제** (`LEGACY_WALK_NEEDED()`) | 걸려 있을 때 Target Load 지명의 42.9%를 오염시켰음 |
| 백엔드 머신 | **Golden Cove 실측치** (`PARAMS.golden_cove`, 2026-09-07 개정) — RS 97/70/19 = **186** (wikichips 97/70/38; Scarab이 ST-AGU 포트 4·9를 하나로 합쳐 RS3는 19), dcache **1 read port × 8 bank**, PRF int 280 / vec 332, LLC 8 bank, `tea_rs_reservation 0`. **이전 실험과 동일하게 유지**(사용자 결정): issue 8 / retire 16, LQ 256 / SQ 192, BTB 8K, MSHR 64 | TEA 평가용 확대 머신(RS 544−192 = 352, 2 port × 1 bank, PRF 592, LLC 1 bank)은 현실성이 부족해 폐기 — 재현용으로 `PARAMS.golden_cove_rs352` + `scarab-infra/json/zereco_dbg_rs352.json`에 보존. 모든 config 공통. **논문 RS·partition % 분모 = 186** |

---

## B. 코드 ↔ 아키텍처 매핑

| 아키텍처 요소 | 상태 | 코드 |
|---|---|---|
| **A1** HBT | 재사용 | `bp/hbt.c` — 1024 entry, 3-bit, threshold>1, decay 50K |
| **A2** LPR 관측 | **구현** | `cmp_model.c: cmp_wake()` 훅 (early-return 앞) — wakeup의 max에 argmax를 얹음. producer PC는 wake 시점에 포착(하드웨어의 scoreboard 역참조와 등가) |
| **A3** brslice_tab | **구현** (관찰 테이블 형태) | `zereco/critpath.c` — 4096 sets × 8-way LRU, `{in_slice, depth, owner_pc, confirm}`; commit 훅에서 seed·전파·decay |
| **A4** Target Load → PT | 구현 | `critpath_note_retire()` → `rfp_note_target_load()`; walk 경로는 `!RFP_TARGET_CRITPATH`로 차단 |
| **A5** P-IQ | 재사용, 입력만 교체 | `decoupled_frontend.cc`가 PC 조회로 priority bit 부착 → `node_issue_queue.cc`/`exec_ports.c` |
| **A6** RFP | 재사용 | `zereco/rfp.c` |
| H2P resolution profiler | 재사용 | `zereco/h2p_mispred_latency.c` |
| 옛 identification (Fill Buffer + batch walk + Block Cache) | **비활성** | `LEGACY_WALK_NEEDED()` 가 false면 fill·trigger·엔진 모두 skip |

파라미터: `zereco_critpath_profile`(관측), `_priority`(A5 입력), `rfp_target_critpath`(A4 입력),
`_decay_interval`, `_insert_gate`, `_priority_max_depth`, `_table_sets/_assoc`, `_confirm_bits`.

---

## C. 확정된 결과

> **주의 — C1~C6은 전부 옛 확대 머신(RS 352, 2 port × 1 bank, PRF 592, LLC 1 bank)에서 측정한 값이다.**
> 2026-09-07에 백엔드를 Golden Cove 실측치(RS 186, 1 port × 8 bank, PRF 280/332)로 바꿨고,
> 같은 사다리를 `260907_critpath_gc186`에서 다시 돌린다. 아래 수치는 그 결과로 대체될 때까지 **정성적 결론**(어떤 축이 효과가 있고 없는가)으로만 쓴다.

108 simpoint(workload당 top-8 weight, gcc 4), random-queue IQ, weight 가중 → workload 간 geomean.
분석 스크립트·그림은 각 실험 디렉터리의 `analysis/`.

### C1. Phase A — critical edge의 성질 (`260901`, 계측만·cycle-identical 확인)

- **scheduler 무관**: oldest-first와 random-queue의 slack 분포·안정성·깊이가 1%p 이내로 동일
- **slack**: chain 노드의 25~27%가 slack ≤ 2 cycle (공동 critical), 나머지는 승자 명확
- **LPR producer 안정성** 78% (static PC 기준 연속 instance)
- **critical edge**: register 97.7% / store→load 2.3%
- **chain 깊이**: 멤버 커밋의 72%가 depth ≤ 4; 평균 chain 6.6 op/branch instance
- **owner 충돌**: 전파의 ~30%가 다른 branch로 overwrite, 그중 17%가 H2P owner 상실

### C2. Phase A2 — 무엇이 인구를 줄이는가 (`260903`)

critical-path 필터만으로는 커밋 명령어의 **60~68%가 chain 멤버**다. retention threshold(68.1→67.2%)
와 decay(20K에도 59.3%)는 못 줄이고, 삽입 게이트는 조일 여지가 없다. depth 제한만 듣는다
(≤2에서 31%). **그러나 인구는 문제가 아니었다** — C3 참조.

### C3. Phase B — 가속 효과 (`260905_critpath_phaseB`, 정제 코드)

| | RFP | P-IQ(무한) | **결합** |
|---|---:|---:|---:|
| IPC | +5.13% | +4.89% | **+9.06%** |
| fetch→resolution | −11.4% | −7.3% | **−17.3%** (69.9 → 57.8 cy) |
| dependency wait | −14.2% | −5.0% | **−18.3%** |
| branch select 대기 | — | 2.68 → 0.06 cy | |

두 메커니즘은 거의 가산적이다. **RS 실제 priority 점유는 25%**(용량 대비 6%) — 커밋 기준
멤버십 60%의 절반 이하인데, 멤버가 우선 issue되어 큐에서 빨리 빠지기 때문이다. pr(DRAM-bound)
회귀 없음(+2.2%).

Target Load(load의 66%) 처리: **useful 29.2%** / low confidence 35.0% / store abstain 18.9% /
no data in time 13.0% / wrong address 2.7% / queue full 1.2%. 그림 `analysis/target_load_outcome.pdf`.

### C4. B-1 — Prefetch Table 크기 (`260905_critpath_phaseB`)

| PT | 512 | 1K | 4K | 16K | ∞ |
|---|---:|---:|---:|---:|---:|
| IPC | +9.01% | +9.06% | +9.07% | +9.08% | +9.08% |

스래싱(512·1K에서 alloc:evict 1:1)은 실재하나 성능과 무관 — confidence 포화 건수가 크기와
무관하게 거의 같다. 회전하는 것은 포화하지 못할 cold PC이고 hot 집합은 512에 상주한다.
RFP 상한(∞, priority 없음) +5.14%. **RFP를 묶는 것은 용량이 아니라 주소 예측 가능성**이다.

### C5. B-2 — priority partition 비율 (`260905_critpath_B2_partition`)

| 예약 | ∞ | 30% | 25% | 20% | 15% | 10% |
|---|---:|---:|---:|---:|---:|---:|
| IPC | +9.08% | +8.67% | +8.51% | +8.35% | +8.02% | +7.65% |
| 무한 대비 보존 | 100% | 96% | 94% | **92%** | 88% | 84% |
| fallback (priority 상실) | — | 6.9% | 10.0% | 14.2% | 20.4% | 29.9% |
| priority 구획 점유/용량 | — | 14% | 16% | 18% | 20% | 23% |
| branch select 대기 (cy) | 0.06 | 0.21 | 0.28 | 0.35 | 0.47 | 0.72 |

손실은 완만하고 단조롭다 — 예약 5%p당 약 0.15~0.35%p. **무릎이 없다**: 예약을 줄이면 fallback이
선형으로 늘고 그만큼 잃는다. 20%(= 옛 352의 70 entry)면 상한의 92%를 14% fallback으로 얻는다.
priority op의 ready→issue는 어느 비율에서도 0.4~0.6 cy로 normal(2.2~2.8 cy)보다 훨씬 짧다 —
partition이 우선권을 실제로 전달한다. pr은 전 구간 평탄(bandwidth-bound), xgboost는 비단조
(지배 phase 결손 표본의 잡음).

### C6. 코드 감사 (2026-09-04)

TEA 잔여 코드 중 타이밍이나 우리 상태에 닿는 경로를 전수 확인. 오염 2건(walk의 PT 지명,
Block-Cache 기준 chain_bit)을 수정하고 walk를 완전 차단했다. 정제 후 재실행 결과는 오염 전과
0.04%p 이내 — 오염분(지명의 42.9%)은 성능에 기여하지 않던 cold PC였다. 알면서 보류한 낙관
1건(wrong-path 명령어에 priority 미부여)은 TODO D-1.
