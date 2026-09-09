# TODO — 할 일

> Last updated: 2026-09-08 · branch `test`
> **이 문서 = 할 일만.** 설계와 확정된 결과는 [zereco_CRITPATH_DESIGN.md](zereco_CRITPATH_DESIGN.md).
> 기준 실험: `260908_critpath_comparison` (67 simpoint, 352 머신). 라이브 디스크립터: `scarab-infra/json/zereco_dbg.json`.

---

## 0. 설계 대비 미구현 · 미결정 항목

| # | 항목 | 현재 코드 | 상태 · 결정 |
|---|---|---|---|
| **D-12** | **가속 대상을 줄이는 방법** | 정적 PC별 멤버십, depth 무제한 | 문제는 "full 대비 몇 % 필터링"이 아니라 **커밋 op의 55~61%가 priority를 받는다**는 것(C3). 검증 끝난 것: **용량은 답이 아니다**(C8 — 상주 PC를 4배 줄여도 가속 op는 55 → 47%). 남은 후보: (a) **depth 제한**(B-3, 멤버 commit의 48.8%가 depth ≤ 2이므로 depth 2면 약 30% 예상), (b) **confirm/executed 비율 필터** — entry에 실행 횟수를 함께 세어 "자주 실행되지만 드물게 critical"한 hot PC를 겨냥. cold/hot 비대칭을 정면으로 치는 유일한 안, (c) instance 단위 priority |
| **D-10** | partition 예약률 확정 | 25% (88 entry) | C4: 실측 상주 priority op 13.6개 = partition의 15%, fallback 8%, Datacenter만 full cycle 12.5%. 줄일 여지 있음(15~20%) — 축소 시 fallback 증가와 맞바꿈. **사용자 결정** |
| **D-1** | wrong-path 명령어의 priority | frontend 태깅이 `op->off_path`면 조기 반환 → off-path 멤버는 priority bit 없음 | 하드웨어는 fetch 시점에 on/off-path를 모르므로 wrong-path 멤버도 priority entry를 점유해야 한다. 현재 결과는 그 경쟁이 빠져 **낙관적**(partition 압박 과소평가). `decoupled_frontend.cc` critpath 분기에서 `off_path` 조건 제거 → 낙관 폭 측정(실험 B-5) |
| D-2 | Δ-window (`\|t_last − t_second\| < Δ`면 양쪽 producer 삽입) | Δ=0 | 원안 유지. 후순위 |
| D-4 | depth 제한 | 파라미터 존재(`zereco_critpath_priority_max_depth` 0 = 무제한), config sweep만 하면 됨 | 가속 대상을 줄이는 최유력 지렛대 → B-3 |
| D-5 | owner 충돌 | 단일 owner pointer, overwrite | 2-slot 승격 여부. 후순위 |
| D-6 | brslice_tab 하드웨어 예산 | 4096 set × 8-way = 32,768 entry (계측 크기) | **C8로 답 나옴**: 1K entry(128 set × 8-way, PUBS 예산) −0.1%p, 512 entry −0.2%p. 논문 구성으로 **1K entry** 채택 권고 — 사용자 확인만 남음 |

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
| **B-3** | **depth 제한 sweep** (`zereco_critpath_priority_max_depth` ∞/8/4/2/1) × {critical, full}, brslice_tab 1K entry | **다음 실험.** 멤버 commit의 48.8%가 depth ≤ 2 → depth 2 제한이 가속 대상을 약 30%로 낮출 것(유도값). 목표 20~30%대에 닿는 유일한 knob. 코드 변경 불필요 |
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
