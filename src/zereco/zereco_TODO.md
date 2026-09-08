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
| **D-11** | **store→load forwarding wake의 LPR 추적** | forwarding store의 wake도 LPR 후보 → store가 마지막이면 store PC로 전파(critical edge의 약 2%) | 그림의 PRF scoreboard 구조로는 불가능. (a) 유지하고 "LPR = SQ entry" 확장 + commit 때 SQ/ROB에서 store PC 읽기를 설계에 추가, (b) register-only로 한정하고 2%를 한계로 서술(구현은 `git show 4253b3c -- src/zereco/critpath.c`). **사용자 결정** |
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
| **B-3** | depth 제한 sweep (∞/8/4/2/1) — D-4, D-12(b) | 대기 |
| **B-5** | D-1 수정 후 재측정 (wrong-path priority) — 낙관 폭 보고용 | 대기 |
| — | partition 15/20% 재확인 — D-10 | 사용자 결정 후 |
| — | Golden Cove 186 머신 sweep (`zereco_dbg_gc186_sweep.json`) | 보류 |
| — | `LEGACY_WALK_NEEDED()`가 false일 때 Fill Buffer/walk 메모리 할당 자체도 생략 (host 메모리) | 선택 |

## 5. 논문 서술 시 유의

- 머신 = 352 entry(`PARAMS.golden_cove_rs352`), partition %의 분모 352. TEA는 TEA thread용 RS·PRF 192를 추가로 갖는 544-entry 구성이라 자원이 같지 않음을 명시.
- 1차 지표는 H2P resolution latency (IPC 실현률은 그보다 낮음).
- critical vs full slice 결과(C3)는 정적 PC 단위 필터의 한계로 정직하게 서술하거나 D-12에 따라 설계를 바꾼 뒤 서술.
- xgboost·tc 표본 성격은 methodology에 적지 않음(사용자 방침) — 결과 해석에서만 유의.
