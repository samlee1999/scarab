# TODO — 할 일

> Last updated: 2026-09-08 · branch `test`
> **이 문서 = 할 일만.** 설계와 확정된 결과는 [zereco_CRITPATH_DESIGN.md](zereco_CRITPATH_DESIGN.md).
> 기준 실험: `260908_critpath_comparison` (67 simpoint, 352 머신). 라이브 디스크립터: `scarab-infra/json/zereco_dbg.json`.

---

## 0. 설계 대비 미구현 · 미결정 항목

| # | 항목 | 현재 코드 | 상태 · 결정 |
|---|---|---|---|
| **D-12** | **critical 필터의 단위** | 정적 PC별 멤버십(brslice_tab), decay 100K | ES(edge 통일 후): full 멤버 중 critical이 거르는 것 **6~7%**(priority op 6.1~7.5%, Target Load 3.5~4.2%; Datacenter 8~12%). 교수님 목표 20%에 못 미침. 원인은 정적 PC 멤버십의 누적(producer flip 22%, 전파의 99.9%가 refresh). 선택지: (a) **instance 단위 priority** — commit 때가 아니라 dispatch/issue 시점에 LPR 정보를 dynamic op에 직접 부착해 그 instance의 critical producer만 우대, (b) depth 제한(D-4), (c) confirm threshold를 높여 "자주 critical인 PC"만 유지, (d) 정적 PC 필터의 한계를 인정하고 서술 변경 — **사용자 결정** |
| **D-10** | partition 예약률 확정 | 25% (88 entry) | C4: 실측 상주 priority op 13.6개 = partition의 15%, fallback 8%, Datacenter만 full cycle 12.5%. 줄일 여지 있음(15~20%) — 축소 시 fallback 증가와 맞바꿈. **사용자 결정** |
| **D-11** | **edge 집합** | knob `zereco_critpath_mem_edge` (1 = store→load forwarding edge 포함, 0 = register-only) | ES 결과: mem edge는 멤버 +4%p, IPC +0.3%p, 필터링 6.0 → 7.3%. 하드웨어 충실도는 0(PRF scoreboard만), 성능은 1(LPR = SQ entry 확장 필요). **사용자 결정** — 논문에서 어느 쪽을 기본으로 둘지 |
| **D-1** | wrong-path 명령어의 priority | frontend 태깅이 `op->off_path`면 조기 반환 → off-path 멤버는 priority bit 없음 | 하드웨어는 fetch 시점에 on/off-path를 모르므로 wrong-path 멤버도 priority entry를 점유해야 한다. 현재 결과는 그 경쟁이 빠져 **낙관적**(partition 압박 과소평가). `decoupled_frontend.cc` critpath 분기에서 `off_path` 조건 제거 → 낙관 폭 측정(실험 B-5) |
| D-2 | Δ-window (`\|t_last − t_second\| < Δ`면 양쪽 producer 삽입) | Δ=0 | 원안 유지. 후순위 |
| D-4 | depth 제한 | 파라미터만 존재(`zereco_critpath_priority_max_depth` 0 = 무제한) | 인구를 줄이는 유일한 지렛대. D-12 (b)와 연결 — 실험 B-3 |
| D-5 | owner 충돌 | 단일 owner pointer, overwrite | 2-slot 승격 여부. 후순위 |
| D-6 | brslice_tab 하드웨어 예산 | 4096 × 8-way (계측 크기); 실측 상주 멤버 PC 약 1.2K | 실제 예산(예: PUBS 128×8 = 1K)으로 축소해 민감도 측정 |

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
| **TL** | **멤버십 누적 타임라인** (`260909_critpath_timeline`): both/crit vs both/full, `--zereco_critpath_timeline_interval 100000` → 각 run의 `critpath_timeline.csv`(100K commit마다 누적 op/inst/cycle, 멤버 commit, root commit, 상주 멤버 PC; warm-up 포함 cycle 0부터) | **완료** → DESIGN.md C7. 결론: build-up 구간 없음(첫 200K 명령어부터 동일), critical이 오히려 1~2%p 높음 → edge 집합 불일치 발견(D-11) |
| **B-3** | depth 제한 sweep (∞/8/4/2/1) — D-4, D-12(b) | 대기 |
| **B-5** | D-1 수정 후 재측정 (wrong-path priority) — 낙관 폭 보고용 | 대기 |
| — | partition 15/20% 재확인 — D-10 | 사용자 결정 후 |
| — | Golden Cove 186 머신 sweep (`zereco_dbg_gc186_sweep.json`) | 보류 |
| — | `LEGACY_WALK_NEEDED()`가 false일 때 Fill Buffer/walk 메모리 할당 자체도 생략 (host 메모리) | 선택 |

## 4b. 교수님 피드백 (2026-09-09)

1. **SPEC17에서 TEA와의 IPC 격차**(TEA +22% vs both/crit +6.6%, 특히 leela/mcf/omnetpp/xz)를 줄일 것.
2. **criticality-aware라 부르려면 critical op 필터링이 실제로 보여야** — 지금은 full slice 대비 6%도 못 거름. 20% 내외를 목표로. → 먼저 TL 실험으로 누적 양상 확인 후 D-12 선택.

## 5. 논문 서술 시 유의

- 머신 = 352 entry(`PARAMS.golden_cove_rs352`), partition %의 분모 352. TEA는 TEA thread용 RS·PRF 192를 추가로 갖는 544-entry 구성이라 자원이 같지 않음을 명시.
- 1차 지표는 H2P resolution latency (IPC 실현률은 그보다 낮음).
- critical vs full slice 결과(C3)는 정적 PC 단위 필터의 한계로 정직하게 서술하거나 D-12에 따라 설계를 바꾼 뒤 서술.
- xgboost·tc 표본 성격은 methodology에 적지 않음(사용자 방침) — 결과 해석에서만 유의.
