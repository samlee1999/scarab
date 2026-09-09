# TODO — 할 일

> Last updated: 2026-09-08 · branch `test`
> **이 문서 = 할 일만.** 설계와 확정된 결과는 [zereco_CRITPATH_DESIGN.md](zereco_CRITPATH_DESIGN.md).
> 기준 실험: `260908_critpath_comparison` (67 simpoint, 352 머신). 라이브 디스크립터: `scarab-infra/json/zereco_dbg.json`.

---

## 0. 설계 대비 미구현 · 미결정 항목

| # | 항목 | 현재 코드 | 상태 · 결정 |
|---|---|---|---|
| **D-12** | **critical 필터의 단위** | 정적 PC별 멤버십(brslice_tab), decay 100K | C3: full slice 멤버 중 critical 규칙이 걸러내는 것은 5.6%, priority op의 5.7%, Target Load의 2.9%뿐이고 성능도 같다(10.08 vs 9.66%). 원인은 정적 PC 멤버십의 누적. 선택지: (a) instance 단위 priority — LPR 정보를 dynamic op에 직접 부착, (b) depth 제한(D-4)으로 인구 축소, (c) confirm threshold를 높여 "자주 critical인 PC"만 유지, (d) 정적 PC 단위 필터는 효과 없음을 인정하고 서술 방향 변경 — **사용자 결정** |
| **D-10** | partition 예약률 확정 | 25% (88 entry) | C4: 실측 상주 priority op 13.6개 = partition의 15%, fallback 8%, Datacenter만 full cycle 12.5%. 줄일 여지 있음(15~20%) — 축소 시 fallback 증가와 맞바꿈. **사용자 결정** |
| **D-11** | **edge 집합** (store→load forwarding edge 포함 여부) | knob `zereco_critpath_mem_edge` (1 = 기존 동작: critical은 store를 LPR로 선택 가능, full도 forwarding store로 전파; 0 = register-only, PRF scoreboard 충실) | 타임라인 실험에서 드러남: 기존 비교는 critical만 store edge를 따라가 **critical ⊄ full**이었음. ES 실험(4 config)으로 두 edge 집합에서 공정 비교 후 **사용자 결정** |
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
| **ES** | **edge 집합 통일 비교** (`260909_critpath_edgeset`): both/{crit,full} × mem_edge {0,1}, 타임라인 포함, 67 simpoint | 코드·빌드·디스크립터 완료, **실행 대기(사용자)**. 볼 것: 두 edge 집합 각각에서 non-critical 비율(멤버/priority op/Target Load), `CRITPATH_SLICE_OPS_STORE`(멤버 store commit), IPC·latency, 타임라인 |
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
