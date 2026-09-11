# TODO — 할 일

> Last updated: 2026-09-08 · branch `test`
> **이 문서 = 할 일만.** 설계와 확정된 결과는 [zereco_CRITPATH_DESIGN.md](zereco_CRITPATH_DESIGN.md).
> 기준 실험: `260908_critpath_comparison` (67 simpoint, 352 머신). 라이브 디스크립터: `scarab-infra/json/zereco_dbg.json`.

---

## 0. 설계 대비 미구현 · 미결정 항목

| # | 항목 | 현재 코드 | 상태 · 결정 |
|---|---|---|---|
| **D-12** | **가속 대상을 줄이는 방법** | 정적 PC별 멤버십 | 검증 끝: 용량 ✗(C8), depth ✗ 전체 비례 손실(C9), **refresh ✓ 효율 +13%**(C10 — 반복적으로 LPR인 PC만 남겨 critical 규칙을 실제로 선택적으로 만듦). refresh 단독 가속 대상 44.9%로 목표 20~30%엔 부족 → refresh 위에 A(edge confidence)/B(비율) 조합 |
| **D-10** | partition 예약률 확정 | 25% (88 entry) | C4: 실측 상주 priority op 13.6개 = partition의 15%, fallback 8%, Datacenter만 full cycle 12.5%. 줄일 여지 있음(15~20%) — 축소 시 fallback 증가와 맞바꿈. **사용자 결정** |
| D-2 | Δ-window (`\|t_last − t_second\| < Δ`면 **양쪽** producer 삽입) | **미구현.** Δ=0이고 `critpath_second_cycle`은 slack 통계에만 쓰인다. wake 훅이 `arrival > last_cycle`(strict)이라 동률이면 먼저 관측된 producer가 LPR로 남는다 | 필터링을 강화하려는 지금 방향과 **반대**(멤버가 늘어난다). 정확도(critical을 놓치지 않는 것)를 보려면 유효하나 후순위. 대상 모집단은 멤버 commit의 5.2%로 작다 |
| D-4 | depth 제한 | 파라미터 존재, C9에서 sweep 완료 | **부분 채택 후보**: Datacenter/SPEC17에는 depth 4가 유리, GAP에는 불리. 워크로드 무관 단일 값은 없음. 최종 구성에서 depth 4를 기본으로 할지 사용자 결정 |
| D-5 | owner 충돌 | 단일 owner pointer, overwrite | 2-slot 승격 여부. 후순위 |
| D-6 | brslice_tab 하드웨어 예산 | **1K entry (128 set × 8-way) 확정** — 2026-09-10 사용자 결정, 이후 모든 실험 고정 | C8: 32K 대비 −0.1%p, 512 entry −0.2%p. PUBS 예산과 동일 |

## 1. Address-generation slice statistics (급하지 않음)

**왜** — Target Load의 남는 가속 경로는 **주소 계산 slice**인데 그 기여가 측정되지 않는다.
넣을 계측 (`critpath_note_retire`의 load 처리):

| 카운터 | 답하는 것 |
|---|---|
| Target Load의 LPR이 **주소 operand** producer인지 **데이터 operand** producer인지 | 가속 경로가 주소 쪽인지 |
| Target Load의 주소 chain 길이 | 주소 계산이 몇 단계인가 |
| priority 받은 주소 chain이 load의 **AGU 도달**을 얼마나 앞당기나 | 효과 크기 |
| RFP 실패(low confidence 43%) Target Load 중 주소 chain 가속만으로 회수되는 비율 | 두 메커니즘 상보성의 직접 증거 |

힌트: `op->oracle_info.src_info[]`/`op->table_info`로 주소 source 집합 판정 후 `op->critpath_last_src`와 대조.

## 2. 계측 공백 · 결함

| 항목 | 상태 |
|---|---|
| `RFP_INFLIGHT_UNDERFLOW` (PT 축출 후 같은 PC 재할당 시 0 카운터 감소) | 무해(injected의 0.02%). 미수정 |
| full-slice 모드의 priority op 분류가 commit 시점 shadow 멤버십 기준 (bit는 fetch 시점) | 소수 오분류 가능. 비율 통계에는 무시할 수준 |

## 3. 워크로드 · 방법론

- **67 simpoint로 통일**(workload당 5, clang 4, gcc 3; 14 workload). `260827_tea_baseline` 이후 모든 실험이 공유하는 목록이라 TEA와 동일 표본. 논문에는 "TEA 비교와 동일 표본"으로 서술.
- frontend forward-progress watchdog(`decoupled_frontend.cc`, fetch 100K cycle 정지 → assert)으로 죽는 simpoint는 원인 추적 없이 제외(baseline에서도 같은 op 번호에서 죽음). 67 목록은 이 문제가 없다.
- weight 가중 집계 유지. 효과 기준 선별 금지.
- 다른 목록: 108 simpoint(`json/zereco_dbg_rs352.json`), Golden Cove 186 머신용 105 simpoint(`json/zereco_dbg_gc186_sweep.json`, `PARAMS.golden_cove`). 둘 다 보류.

## 4. 남은 실험

| # | 내용 | 상태 |
|---|---|---|
| **ES** | edge 집합 통일 비교 (`260909_critpath_timeline`, 4 config) | **완료** → DESIGN.md C3·C7 |
| **TL** | 멤버십 누적 타임라인 (`260909_critpath_timeline`) | **완료** → DESIGN.md C7 |
| **TAB** | brslice_tab 용량 sweep (`260909_critpath_brslice_tab_size`, 10 config) | **완료** → DESIGN.md C8. 용량은 두 규칙을 가르는 축이 아님(격차 0.17 → 0.20%p). 대신 1K entry면 충분하다는 비용 결과 확보 |
| **B-3** | depth 제한 sweep (`260910_critpath_depth`) | **완료** → DESIGN.md C9. 가속 대상은 목표 구간(d2 = 25%)에 들어가나 이득도 절반으로 감소. **단 Datacenter는 depth 4에서 인구 54%로 이득 85% 유지**(효율 1.58배), SPEC17 1.30배, GAP 없음. depth 제한은 critical vs full 필터링은 개선 못 함 |
| **B-5** | D-1 수정 후 재측정 (wrong-path priority) — 낙관 폭 보고용 | 대기 |
| — | partition 15/20% 재확인 — D-10 | 사용자 결정 후 |
| — | Golden Cove 186 머신 sweep (`zereco_dbg_gc186_sweep.json`) | 보류 |
| — | `LEGACY_WALK_NEEDED()`가 false일 때 Fill Buffer/walk 메모리 할당 자체도 생략 (host 메모리) | 선택 |

## 4a. 필터링 강화 — 두 단계로 분리 (2026-09-10 사용자 결정)

**1단계 `260910_critpath_offpath_refresh` — 완료 → DESIGN.md C11.** off-path 켬을 기준으로 확정(IPC −0.85%p = oracle 낙관), refresh는 **수명 20K(1b/10K) 유지**(4K는 SPEC17 −0.36%p로 과함), 25% 구획 유지(점유 27%, fallback 10%).: 아래 표의 crit_ref, base, r10k, r4k만 — 새 base(off-path on + refresh 설정)를 먼저 확정한다. **2단계**: 확정된 base 위에 A/B/C. 분리 이유: B와 refresh는 둘 다 decay sweep에서 멤버를 빼므로 refresh 무릎이 옮겨가면 B의 몫이 달라지고, off-path로 구획 압박이 크게 늘면 25% partition부터 다시 봐야 할 수 있다. A/B/C 코드는 이미 같은 바이너리에 있고 기본 off.

**지표 전환**: filtering = dispatch 기준. priority 자격을 가진 채 dispatch된 op(`ZERECO_PIQ_PRIORITY_ADMISSION_CANDIDATE_OPS`, on + off path)를 full vs critical로 비교. 분모는 전체 dispatch. 그래서 모든 config에서 `zereco_critpath_priority_offpath 1`.

**공통 base**: off-path priority on, refresh **1b/10K**(멤버 수명 20K — C10 최고 효율, 1 bit라 하드웨어 최소), 테이블 1K, register-only edge, depth 무제한, P-IQ 25%, RFP PT 1K + store forwarding.

| config | 목적 | 비교 대상 |
|---|---|---|
| crit_ref | 정합성: off-path 0, 기본 refresh, 필터 off | `260910_critpath_depth/crit_inf`와 simpoint별 완전 일치 |
| crit_base / full_base | ① off-path 영향 + dispatch 기준 filtering 기준점 | `260910_critpath_refresh/{crit,full}_1b_10k` (off-path 0) |
| crit/full × r10k, r4k | ② refresh 무릎 (수명 10K / 4K, decay 5K / 2K) | base(20K) |
| crit_A3 | ③ A — edge confidence 3 (같은 LPR producer 4연속) | full_base, crit_base |
| crit_B25 / B50 | ③ B — criticality ratio 25% / 50% (decay 창마다, 4회 이상 실행 PC만 판정, root 면제) | full_base, crit_base |
| crit_C3 | ③ C — slack ≥ 3 cycle만 전파 (tie 0~2 제외) | full_base, crit_base |

knob은 전부 기본 0(off)이고 off일 때 타이밍이 기존과 동일(새 필드 쓰기만). A·C는 critical 규칙의 전파만 막고, B는 decay sweep에서 멤버를 뺀다. **E(H2P branch별 chain 분리)는 저장 비용 때문에 보류, F는 제외, D-2(tie → 양쪽 삽입)는 성능 향상 후보로 남겨 둠.**

**2단계 `260911_critpath_filter_abc` — 완료 → DESIGN.md C12.** **A가 유일하게 큰 필터**(full 대비 35.4%, 효율 +27%)지만 IPC −1.20%p(SPEC17 −2.38%p). B(7.7~9.1%)·C(8.8%)는 거의 공짜지만 작다.

**A 채택, B·C 제외 (2026-09-11 사용자 결정)** — B·C는 효과가 작다. 남은 과제는 A의 IPC 손실을 줄이면서 filtering을 유지하는 것.

**3단계 `260911_critpath_edge_conf` — 완료 → DESIGN.md C13.** depth ≤ 1 면제(e1)만 효과(IPC 손실 −1.20 → −0.75%p, filtering 25.6%). 임계·hysteresis는 지렛대가 아니다. 남은 SPEC17 손실의 72%가 mcf 두 simpoint(82875, 28781).

**다음 할 일 (TEA 결과를 뽑은 뒤, test 브랜치로 돌아와서)**
- **4단계 배치 — A2-e1 (사용자 제안, 2026-09-11)**: reset 모드, 임계 2, depth ≤ 1 면제, hysteresis 없음. filtering이 목표(20%)보다 넉넉하므로(A3-e1 25.6%) 일부를 성능으로 바꾸는 방향
  - 예상(유도, C13의 임계 효과로부터): 임계만으로는 작다 — A3 → A2가 e1 없이 +0.06%p, hysteresis+e1에서 +0.10%p였다. A2-e1은 IPC 약 +0.05~0.10%p, filtering 약 1%p 감소로 예상
  - 같은 배치에 **A2-e2**(depth ≤ 2 면제) 추가 권장: 임계보다 면제 깊이가 더 센 지렛대다. A3-e1에서 남은 차단의 52%가 depth 2(GAP은 87%). A2-e1 대 A2-e2로 면제 깊이 효과, A3-e1 대 A2-e1로 임계 효과를 분리
  - config: 현재 base + `--zereco_critpath_edge_conf_min 2 --zereco_critpath_edge_conf_exempt_depth 1` / `... _exempt_depth 2`
- **A 최종 설정은 4단계 후 결정.** 참고: A3-e1은 A3-h-e1보다 IPC·filtering 둘 다 낫다 — hysteresis는 쓰지 않는다
- **mcf 82875 / 28781 진단 — 남은 손실의 핵심.** A3-e1의 전체 손실 −0.75%p 중 **mcf 하나가 0.47%p(63%)**, 나머지 workload는 각 0.08%p 이하. 어떤 멤버 PC가 이 phase의 이득을 지는지: critical vs A3-e1에서 brslice_tab 멤버(PC, depth, edge confidence, producer flip 빈도)를 끝에 dump해 비교 — 코드 필요(진단 전용 knob). 결과에 따라 "번갈아 오는 두 producer를 둘 다 따라가는" 식의 A 보완을 검토
- **주의**: zereco run은 test 브랜치에서 빌드·실행할 것. TEA 브랜치에 있는 동안 `./sci --sim zereco_dbg`를 부르면 TEA 코드로 빌드된다

**평가 모드 (2026-09-11 사용자 결정)**: 논문 평가는 **oracle(off-path 0) + commit 기준 filtering**으로 간다. 하드웨어 동작 수치는 C11에 보관(IPC −0.85%p). 논문에는 "wrong-path 명령어는 priority를 받지 않는다고 가정"을 명시하고 C11을 민감도로 제시. 이후 시뮬레이터에서만 가능한 관점의 결과(oracle/limit study)도 요청 예정 — 그런 결과는 상한(limit study)으로 표기.

**새 후보 G — H2P 그림자 priority 차단** (C11에서 발견, **A/B/C 이후 방향이 없으면 시험** — 사용자 결정): priority 자격 dispatch의 **67.8%가 wrong-path**. 하드웨어가 알 수 있는 신호로 이를 줄일 수 있다 — 아직 resolve되지 않은 H2P branch(HBT가 표시) 뒤에서 fetch된 op는 wrong-path일 확률이 높으므로 priority bit를 주지 않는다. 대가는 그 branch가 맞게 예측된 경우의 on-path op도 priority를 잃는 것. A/B/C보다 줄일 수 있는 양이 훨씬 크다(상한: dispatch 기준 priority 비율 55% → 18%). 구현은 frontend에서 "in-flight 미해결 H2P branch 수" 카운터 하나.

## 4b. 교수님 피드백 (2026-09-09)

1. **SPEC17에서 TEA와의 IPC 격차**(TEA +22% vs both/crit +6.6%, 특히 leela/mcf/omnetpp/xz)를 줄일 것.
   - **TEA 자원 동등화 후 재비교 (2026-09-11 사용자 결정, 이 branch에서는 코드 수정 금지 — TEA branch에서 수행)**: 현재 TEA는 main 352 + TEA thread 192 = RS 544(PRF도 192 추가)라 자원이 우리보다 많다. TEA 총량을 우리 머신(RS 352, PRF 592)에 맞추고 TEA thread 몫을 그 안에서 떼어 내는 구성으로 다시 돌린 뒤, 같은 67 simpoint·같은 baseline으로 격차를 다시 잰다.
2. **criticality-aware라 부르려면 critical op 필터링이 실제로 보여야** — 지금은 full slice 대비 6%도 못 거름. 20% 내외를 목표로. → 먼저 TL 실험으로 누적 양상 확인 후 D-12 선택.

## 4c. 데이터 품질 · 표본 민감도 (2026-09-10)

- **deepsjeng 133677, 164928은 퇴화한 trace다.** 352 머신 IPC 0.973 / GC186 1.000으로 고정, H2P misprediction 0, uop/instruction이 정확히 4.0(정상 1.2), chain 멤버 0. 67 표본에는 없어 현재 결과에 영향 없음. 108로 돌아갈 경우 결과와 무관한 기준(분기 활동 0 / IPC 고정)으로 제외하고 명시.
- **TEA 격차 기준 제외 민감도** (`260908_critpath_comparison/analysis/drop_maxgap.py`, 사용자 요청으로 수행): workload마다 TEA − Both crit 격차가 가장 큰 simpoint 1개씩 제외(67 → 53).
  - SPEC17 격차 15.47 → **13.76%p** (1.7%p만 줄어듦). 전체 격차 4.13 → 1.56%p로 줄지만 대부분 **GAP에서** 나옴(TEA GAP 1.118 → 1.062).
  - 부작용: bfs/259는 bfs weight의 **59.8%**(지배 phase)인데 격차 1.2%p로 최대라 제외됨 → TEA bfs 21.4 → 3.6%. pr은 우리 최고 simpoint가 제외돼 우리 +4.5 → −0.6%.
  - 좁은 변형(TEA가 IPC를 ~2배로 만든 bc/3024 +94%, mcf/25133 +123% 두 개만 제외): SPEC17 격차 14.14%p, 전체 3.49%p.
  - **SPEC17만 제외 (사용자 확정 변형, 2026-09-10)**: SPEC17 5개 workload에서만 최대 격차 simpoint 1개씩 제외(deepsjeng 7248, leela 163012, mcf 25133, omnetpp 66177, xz 23529), GAP·Datacenter 전부 유지 → 67 → 62. SPEC17 Both crit 1.066 → 1.067, TEA 1.221 → **1.205**, 격차 15.47 → **13.76%p**. GAP·Datacenter 수치는 원본과 동일. 그림 `analysis/cmp62_specdrop_ipc.pdf`(원본 `cmp67_ipc.pdf`와 파일명으로 구분)
  - **결론: 어떤 제외 규칙으로도 SPEC17 격차는 13.8~14.1%p 남는다 — 구조적이다.** 쓸 경우 67 전체 결과와 나란히 민감도 분석으로 제시.
- 극단 2개(bc/3024, mcf/25133)는 TEA 이득이 branch precomputation에서 오는지, TEA thread의 load가 main thread에 prefetch 효과를 주는 부수 효과인지 확인할 가치가 있다(후자면 논문에서 정당하게 지적 가능).

## 5. 논문 서술 시 유의

- 머신 = 352 entry(`PARAMS.golden_cove_rs352`), partition %의 분모 352. TEA는 TEA thread용 RS·PRF 192를 추가로 갖는 544-entry 구성이라 자원이 같지 않음을 명시.
- 1차 지표는 H2P resolution latency (IPC 실현률은 그보다 낮음).
- critical vs full slice 결과(C3)는 정적 PC 단위 필터의 한계로 정직하게 서술하거나 D-12에 따라 설계를 바꾼 뒤 서술.
- xgboost·tc 표본 성격은 methodology에 적지 않음(사용자 방침) — 결과 해석에서만 유의.
