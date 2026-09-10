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
| **D-1** | wrong-path 명령어의 priority | frontend 태깅이 `op->off_path`면 조기 반환 → off-path 멤버는 priority bit 없음 | 하드웨어는 fetch 시점에 on/off-path를 모르므로 wrong-path 멤버도 priority entry를 점유해야 한다. 현재 결과는 그 경쟁이 빠져 **낙관적**(partition 압박 과소평가). `decoupled_frontend.cc` critpath 분기에서 `off_path` 조건 제거 → 낙관 폭 측정(실험 B-5) |
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

## 4a. 필터링 강화 실험 후보 (B-3 이후 진행 — 2026-09-10 사용자 확정)

문제 정의: 한 instance에서는 producer 하나만 critical인데, 같은 PC가 반복 실행되며 instance마다 다른 producer를 지목하고(producer flip 22%) 그 **합집합이 누적**된다.
"한 번이라도 critical이면 영구 멤버"이므로 critical 규칙이 full 규칙으로 수렴한다. 대책은 전부 **반복성·일관성을 요구**하는 방향이다.

| # | 방향 | 내용 | 비용 |
|---|---|---|---|
| **D** | **더 강한 refresh** — **완료** → DESIGN.md C10 | **처음으로 효율을 올린 knob**: 1b/10K에서 가속 대상 51.2 → 44.9%, IPC −0.06%p, 효율 +13%, 필터링 3.1 → 6.8%(SPEC17 10.8%). 아직 포화 안 함 → 더 공격적인 점(decay 5K/2K, 1bit) 추가 필요 | **다음 배치 기본값을 1b/10K로** 두고 그 위에 A/B/C·D-2를 얹는 것을 권고 — 사용자 확인 |
| **A** | **edge confidence** | entry의 `last_producer_pc`에 2-bit confidence를 붙여 같은 producer가 연속 지목될 때만 증가·바뀌면 리셋, 포화 시에만 전파 | entry당 2 bit, 코드 ~20줄. **모집단이 가장 크다**: producer flip이 멤버 commit의 22.5% |
| **C / D-2** | **tie 정책 (한 knob으로 묶어 함께 실험)** | slack < Δ인 tie에서 0 = 현재(먼저 관측된 쪽) / 1 = **양쪽 삽입(D-2)** / 2 = **둘 다 제외(C)**. wake 훅에서 runner-up producer PC도 기록 필요 | LPR 하나를 가속한 이득은 slack에 묶이므로 tie에서 현재 정책은 이미 거의 0을 얻는다 → **C = 거의 공짜 필터링, D-2 = 성능 향상 후보**. 모집단 멤버 commit의 5.2%. `zereco_critpath_tie_policy` + `_tie_window`, ~20줄 |
| **B** | **confirm / executed 비율** | entry에 실행 횟수를 함께 세고 비율이 임계 이상일 때만 멤버 유지. "자주 실행되지만 드물게 critical"한 hot PC를 겨냥. Phase A2에서 confirm **절대값** threshold가 무력했던 이유를 설명 | counter 1개(4~6 bit) |
| — | **D 다음 배치**: A / B / C·D-2를 각각 기본 off인 독립 knob으로 구현해 한 빌드로. critical 규칙만 돌리고 full은 기준점 재사용 → A 1 + B 2(임계 두 점) + C 1 + D-2 1 = 5 config | D 결과 확인 후 |
| **E** | **H2P branch별 chain 분리** | entry당 owner 하나 + overwrite라 한 branch 기준으로도 여러 path가 섞임. owner별 분리 | 저장 비용 큼, 후순위 |

## 4b. 교수님 피드백 (2026-09-09)

1. **SPEC17에서 TEA와의 IPC 격차**(TEA +22% vs both/crit +6.6%, 특히 leela/mcf/omnetpp/xz)를 줄일 것.
2. **criticality-aware라 부르려면 critical op 필터링이 실제로 보여야** — 지금은 full slice 대비 6%도 못 거름. 20% 내외를 목표로. → 먼저 TL 실험으로 누적 양상 확인 후 D-12 선택.

## 4c. 데이터 품질 (2026-09-10 발견)

- **deepsjeng 133677, 164928은 퇴화한 trace다.** 352 머신 IPC 0.973 / GC186 1.000으로 고정, H2P misprediction 0, uop/instruction이 정확히 4.0(정상 1.2), chain 멤버 0. 두 파일 크기도 거의 같다(141,775,971 / 141,776,125 bytes). **67 표본에는 포함되지 않아 현재 결과에는 영향 없음.** 108 표본으로 돌아갈 경우 결과와 무관한 기준(분기 활동 0 / IPC 고정)으로 제외하고 methodology에 명시.
- TEA와의 격차가 큰 simpoint를 결과를 보고 제외하는 것은 **하지 않는다**(선택적 보고). 격차는 SPEC17 전반에 퍼져 있어 효과도 없다 — 어떤 workload를 빼도 suite 격차 14~17%p 유지.

## 5. 논문 서술 시 유의

- 머신 = 352 entry(`PARAMS.golden_cove_rs352`), partition %의 분모 352. TEA는 TEA thread용 RS·PRF 192를 추가로 갖는 544-entry 구성이라 자원이 같지 않음을 명시.
- 1차 지표는 H2P resolution latency (IPC 실현률은 그보다 낮음).
- critical vs full slice 결과(C3)는 정적 PC 단위 필터의 한계로 정직하게 서술하거나 D-12에 따라 설계를 바꾼 뒤 서술.
- xgboost·tc 표본 성격은 methodology에 적지 않음(사용자 방침) — 결과 해석에서만 유의.
