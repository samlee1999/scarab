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
  - 현재 실험 목록 = **105 simpoint**. 1단계(`260907_critpath_gc_phaseB`) 결과도 이 105개로 집계
- **tc**: 255개 중 8개(weight 7%). 균등 분포라 표본으로 타당하나 수가 적음. 최종 논문 전 전체와 대조
- **gcc**: 4개 유지 (느린 simpoint)
- Simpoint 확대 시 weight 가중 집계 유지. 효과 기준 선별 금지

## 4. 남은 실험

| # | 내용 | 상태 |
|---|---|---|
| **B-3** | depth 제한 sweep (∞/8/4/2/1), PT ∞·partition ∞ | 대기 — D-4. GC-2 후 |
| **B-4** | PT 축소 (128/256) | GC-2의 PT sweep에 256 포함 → 흡수 |
| **B-5** | D-1 수정 후 재측정 (wrong-path priority) | 대기 — 낙관 폭 보고용 |
| **GC-1** | Golden Cove 머신에서 Phase B 사다리 재측정 (`260907_critpath_gc_phaseB`, 5 config) | **완료.** 동향 재현(rfp +4.5, piq +2.8, 둘 +6.9, part20 +6.1%; 옛 머신 5.2/4.9/9.1/8.3). P-IQ 이득 반감(PRF 280이 창을 먼저 묶음), part20 fallback 15→24%. 비교 스크립트 `analysis/compare_old.py`. 그림·문서 반영은 GC-2 후 |
| **GC-2** | sweep: PT 256/512/4K/∞ (rfp+piq unbounded 위) + partition 10/15/30/40% (RS3 기준 2/3/6/8 entry) | 디스크립터 세팅 완료(`zereco_dbg.json`, 9 config × 105), **실행 대기(사용자)**. PT 1K·20%·사다리 점은 GC-1 재사용(같은 바이너리 81e28f8) |
| **GC-3** | GC-1 + GC-2로 DESIGN.md C3·C5 교체, D-10(예약률) 확정, 그림 재생성 | GC-2 후 |
| — | 축 간 상호작용 의심 지점만 2차원 확인 | 필요시 |
| — | `LEGACY_WALK_NEEDED()`가 false일 때 Fill Buffer/walk 메모리 할당 자체도 생략 (host 메모리) | 선택 |

## 5. 논문 서술 시 유의

- 백엔드는 Golden Cove 실측치(RS 186 = 97/70/19, 1 read port × 8 bank, PRF 280/332, LLC 8 bank) + 이전과 동일한 issue 8 / retire 16 / LQ 256 / SQ 192 / BTB 8K / MSHR 64. partition %의 분모 **186**. 옛 352 머신 수치(C1~C6)는 논문에 쓰지 않는다
- Scarab이 ST-AGU 포트 두 개를 하나로 합치므로 RS3가 38이 아니라 19 — 구조 설명 시 "총 186"으로만 적고 포트 병합은 부록/주석
- 1차 지표는 H2P resolution latency (IPC 실현률 낮음)
- xgboost·tc 표본 성격은 methodology에 적지 않음(사용자 방침) — 결과 해석에서만 유의
