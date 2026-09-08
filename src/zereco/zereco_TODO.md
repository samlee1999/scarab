# TODO — 할 일

> Last updated: 2026-09-08 · branch `test`
> **이 문서 = 할 일만.** 설계와 확정된 결과는 [zereco_CRITPATH_DESIGN.md](zereco_CRITPATH_DESIGN.md).

---

## 0. 설계 대비 미구현 · 미결정 항목

| # | 항목 | 현재 코드 | 상태 · 결정 |
|---|---|---|---|
| **D-1** | **wrong-path 명령어의 priority** | frontend 태깅 게이트가 `op->off_path`면 조기 반환 → chain 멤버라도 off-path면 priority bit **없음** | **알면서 보류.** 하드웨어는 fetch 시점에 on/off-path를 모르므로 wrong-path 멤버도 priority entry를 점유·경쟁해야 한다. 현재 시뮬레이터는 그 경쟁이 빠져 **낙관적**이고 partition 압박이 과소평가된다(baseline off-path ~59%). B-2까지 이 상태로 측정했다. 다음: `decoupled_frontend.cc` critpath 분기에서 `off_path` 조건 제거 → 낙관 폭을 재서 보고. 옛 Block-Cache 태깅도 같은 게이트였으므로 이전 결과 전부 같은 낙관 포함 |
| D-2 | Δ-window (`\|t_last − t_second\| < Δ`면 양쪽 producer 삽입) | Δ=0 | 원안 유지 중. tie 25~27%. 성능이 충분해 후순위 |
| D-4 | depth 제한 | 파라미터만 존재(0=무제한) | **B-3**에서 sweep — 인구를 줄이는 유일한 지렛대 |
| D-5 | owner 충돌 | 단일 owner pointer, overwrite | 전파의 ~30%가 overwrite, H2P-lost 5%. 2-slot 승격 여부 결정 |
| D-6 | brslice_tab 하드웨어 예산 | 4096 × 8-way (계측 크기) | 실제 예산(예: PUBS 128×8)으로 축소해 민감도 측정 |
| **D-11** | **store→load forwarding wake의 LPR 추적** | 시뮬레이터는 forwarding store의 wake도 LPR 후보로 삼아 store가 마지막 도착이면 **store PC로 전파**(critical edge의 2.3%, Phase A부터 모든 실험 동일) | 그림의 PRF scoreboard 구조로는 불가능(LPR 필드가 preg만 가리킴). 선택지: (a) 유지하고 하드웨어에 "LPR = SQ entry" 확장 + commit 때 SQ/ROB에서 store PC 읽기를 설계에 추가, (b) register-only로 한정하고 2.3%를 한계로 서술. **사용자 결정 대기.** (b)의 구현은 커밋 4253b3c에 있었고 되돌림(`git show 4253b3c -- src/zereco/critpath.c`) |
| **D-12** | **critical 필터의 단위** | 정적 PC별 멤버십(brslice_tab), decay 100K | CMP 결과: full slice 멤버 중 critical 규칙이 걸러내는 것은 **5%**, priority op의 5%, Target Load의 3%뿐 — 성능도 동일(9.93 vs 9.53%). 원인: 한 PC의 critical producer가 instance마다 바뀌어(flip 22%) 시간이 지나면 LPR 규칙도 모든 producer를 방문하고, 멤버십은 누적된다. 선택지: (a) instance 단위 priority(LPR 정보를 dynamic op에 직접 부착), (b) depth 제한(D-4)으로 인구 축소, (c) confirm threshold를 높여 "자주 critical인 PC"만 유지, (d) 정적 PC 단위 필터는 효과 없음을 인정하고 서술 방향 변경 — **사용자 결정** |
| D-10 | partition 예약률 확정 | 무한 | B-2(옛 352 머신) 20% → 상한의 92%, fallback 14%. RS 186에서는 20% = 37 entry로 절대량이 절반 — GC 재측정 후 **값 선택** — 사용자 결정 |

(D-3 retention threshold, D-7 삽입 게이트, D-8 memory dep, D-9 RS 크기(→ Golden Cove 186, 2026-09-07) → 확정, DESIGN.md로 이동)

## 1. Address-generation slice statistics (급하지 않음)

**왜** — store→load를 못 따라가므로 Target Load의 남는 가속 경로는 **주소 계산 slice**뿐인데
그 기여가 측정되지 않는다. 넣을 계측 (`critpath_note_retire`의 load 처리):

| 카운터 | 답하는 것 |
|---|---|
| Target Load의 LPR이 **주소 operand** producer인지 **데이터 operand** producer인지 | 가속 경로가 주소 쪽인지 |
| Target Load의 주소 chain 길이 | 주소 계산이 몇 단계인가 |
| priority 받은 주소 chain이 load의 **AGU 도달**을 얼마나 앞당기나 | 효과 크기 |
| **RFP 실패(주소 예측 불가) Target Load 중 주소 chain 가속만으로 회수되는 비율** | ← 두 메커니즘 상보성의 직접 증거 |

힌트: `op->oracle_info.src_info[]`/`op->table_info`로 주소 source 집합 판정 후 `op->critpath_last_src`와 대조.

## 2. 계측 공백 · 결함

| 항목 | 상태 |
|---|---|
| `RFP_INFLIGHT_UNDERFLOW` (PT 축출 후 같은 PC 재할당 시 0 카운터 감소) | 무해. injected의 0.017%, PT ∞에서 0. 미수정 |
| 관찰 테이블(brslice_tab)이 계측 크기 | D-6 |

## 3. 워크로드 · 방법론

- **frontend forward-progress watchdog**(`decoupled_frontend.cc:525`, fetch 100K cycle 정지 → assert)으로 죽는 simpoint는 **제외하고 진행**(사용자 결정 2026-09-08, 원인 추적 안 함). baseline에서도 같은 op 번호에서 죽으므로 우리 로직과 무관
  - 옛 352 머신에서 제외: clang 1270/1305/2249/62, xgboost 3311 → clang은 원래 weight의 45%, **xgboost는 23%**(지배 phase 0.759 결손 → xgboost 결과는 "지배 phase 뺀 나머지", B-2에서 비단조 잡음)
  - Golden Cove 머신에서 추가 제외: **clang 1358, gcc 414, gcc 939** (옛 머신에서는 완주했음). 남은 것: clang 7개(41%), **gcc 2개(원래 weight의 15%)** — gcc 결과는 표본이 매우 얇다
  - 186 머신 실험 목록 = 105 simpoint(`zereco_dbg_gc186_sweep.json`). **352 머신으로 복귀한 현재 `zereco_dbg.json`은 108 simpoint**(이 3개는 352 머신에서 완주)
- **tc**: 255개 중 8개(weight 7%). 균등 분포라 표본으로 타당하나 수가 적음. 최종 논문 전 전체와 대조
- **gcc**: 4개 유지 (느린 simpoint)
- **2026-09-08 결정: 앞으로 67 simpoint로 통일**(벤치마크당 5, clang 4, gcc 3 — `260827_tea_baseline` 이후 모든 실험이 공유하는 원래 목록; `zereco_dbg.json` 갱신). 108 목록은 `zereco_dbg_rs352.json`에 보관. 논문 methodology에는 "TEA 비교와 동일 표본"으로 서술 — 67 표본의 geomean이 108보다 0.15%p 높지만 그것을 선택 이유로 적지 않는다
- weight 가중 집계 유지. 효과 기준 선별 금지

## 4. 남은 실험

| # | 내용 | 상태 |
|---|---|---|
| **B-3** | depth 제한 sweep (∞/8/4/2/1), PT ∞·partition ∞ | 대기 — D-4. GC-2 후 |
| **B-4** | PT 축소 (128/256) | GC-2의 PT sweep에 256 포함 → 흡수 |
| **B-5** | D-1 수정 후 재측정 (wrong-path priority) | 대기 — 낙관 폭 보고용 |
| **GC-1** | Golden Cove(RS 186) 머신에서 Phase B 사다리 재측정 (`260907_critpath_gc_phaseB`) | **완료** → 결과는 DESIGN.md C7. 비교 스크립트 `analysis/compare_old.py` |
| **GC-2** | Golden Cove sweep: PT 256/512/4K/∞ + partition 10/15/30/40% | **보류(2026-09-08 방향 전환).** 디스크립터는 `json/zereco_dbg_gc186_sweep.json`(105 simpoint)에 보관 |
| **CMP** | 성능 비교 (`260908_critpath_comparison`) | **완료 → DESIGN.md C8.** 후속: D-10(예약률: 25%에서 partition 점유 15~16%/용량, fallback 8~12% — 과다 예약), D-12(정적 PC 단위 멤버십에서는 critical 필터가 full slice 대비 5%만 제거 — 필터의 단위를 dynamic instance/depth로 옮길지 결정) |
| — | `piq_rfp_critical_slice`(25%) vs `260905_critpath_B2_partition/b2_part25`: 의도된 차이 1건(RFP store forwarding 1)만 있음. `piq_only_critical_slice`는 B-2에 대응 config가 없어 동일성 검증 불가(B-2는 전부 RFP 포함) | CMP 결과 도착 시 방향 확인 |
| — | filtering 효과 통계(CMP에서 자동 수집): full 모드에서 shadow critical table로 멤버/priority op/Target Load를 critical·noncritical로 분류(`CRITPATH_FULL_MEMBER_*`, `CRITPATH_PRIORITY_OP_*`, `CRITPATH_TARGET_LOAD_*`), chain size(`CRITPATH_SLICE_OPS`/`CRITPATH_ROOT_COMMITS`), 테이블 상주 멤버 수(`CRITPATH_LIVE_MEMBERS_AT_SWEEP`/sweeps), zero-wait issue(`ZERECO_IQ_*_ISSUED_ZERO_WAIT`) | 분석 스크립트에 반영 |
| — | full slice 모드는 멤버 load 전부가 PT를 지명하므로 PT 1K가 thrash할 수 있음. 결과에서 `RFP_PT_EVICTIONS`/PT hit를 critical 대비 확인 | CMP 분석 시 |
| — | 축 간 상호작용 의심 지점만 2차원 확인 | 필요시 |
| — | `LEGACY_WALK_NEEDED()`가 false일 때 Fill Buffer/walk 메모리 할당 자체도 생략 (host 메모리) | 선택 |

## 5. 논문 서술 시 유의

- **현재 머신 = 352 entry 머신**(`PARAMS.golden_cove_rs352`, 2026-09-08 복귀). partition %의 분모 352. Golden Cove 186 머신 결과(C7)는 민감도/현실성 확인용
- 186 머신을 다시 쓸 때: Scarab이 ST-AGU 포트 두 개를 합치므로 RS3가 38이 아니라 19 — "총 186"으로만 적고 포트 병합은 부록/주석
- 1차 지표는 H2P resolution latency (IPC 실현률 낮음)
- xgboost·tc 표본 성격은 methodology에 적지 않음(사용자 방침) — 결과 해석에서만 유의
