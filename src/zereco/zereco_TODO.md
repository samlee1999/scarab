# TODO — 할 일

> Last updated: 2026-09-15 · branch `test`
> **이 문서 = 할 일만.** 설계와 확정된 결과는 [zereco_CRITPATH_DESIGN.md](zereco_CRITPATH_DESIGN.md).
> 기준 설정 = 코드 기본값(2026-09-14): register edge만, brslice_tab 1K·멤버 전용, refresh 1b/10K, 필터 A 임계 3 + depth ≤ 1 면제 (DESIGN 결정 표).
> **baseline = `260915_zereco_golden_cove_original/baseline_nopref`**(사용자 결정 2026-09-16: 메커니즘 전부 끔 + stream prefetcher 끔, TEA run과 조건을 맞춤). 예전 `baseline_randq`(prefetcher 켬) 대비 차이와 비교표는 DESIGN C20.
> **머신 전환(2026-09-15, 교수님 지적):** `PARAMS.golden_cove_original`(RS 97/70/19, PRF 280/332, issue 6 / retire 8, dcache 3R/2W). 새 머신의 사다리 결과: `/home/lee/simulations/260915_zereco_golden_cove_original` (descriptor `scarab-infra/json/zereco_dbg_ipc.json`, DESIGN C18). 이전 머신(`golden_cove_rs352`) 결과: `json/zereco_dbg.json` (`260914_zereco_new_baseline`), TEA 비교 `/home/lee/simulations/260913_TEA` (DESIGN C14).
> zereco run은 test 브랜치에서 빌드·실행한다. 다른 브랜치에 있는 동안 `./sci --sim zereco_dbg`를 부르면 그 브랜치 코드로 빌드된다.

---

## 1. 다음 실험

| # | 내용 | 상태 · 메모 |
|---|---|---|
| **N-1** | **oldest-first scheduler에서의 critical slice** (`260915_zereco_new_baseline_oldest_first`, descriptor `json/zereco_dbg_ipc.json`, 3 config × 67, 2026-09-15 사용자 요청): P-IQ only / RFP only / P-IQ + RFP, 모두 기준 critical-path 설정(1b/10K + 필터 A3-e1)에 `--node_issue_queue_schedule_scheme 0` | **descriptor 설정 완료, 실행 대기.** baseline = `260901_critpath_phaseA/baseline_of`(scheduler 외 flag가 260905 baseline과 같고, 같은 시기 random-queue run이 260905와 67 simpoint cycle 일치 — 재사용 가능, 유도). random-queue 짝은 P-IQ + RFP만 있다(260914 `piq_rfp_critical_slice`) — 260914의 단독 행은 4b/100K·필터 A 없음 |
| **N-2** | **필터 A 최적점**: A2-e1(임계 2, depth ≤ 1 면제) — filtering 여유(A3-e1 25.6% vs 목표 20%)를 성능으로 바꾼다. 같은 배치에 A2-e2(depth ≤ 2 면제) 권장 — 면제 깊이가 임계보다 센 지렛대다(A3-e1에서 남은 차단의 52%가 depth 2, GAP은 87%) | 예상(유도): A2-e1 IPC +0.05~0.10%p, filtering −1%p. config: 기준 + `--zereco_critpath_edge_conf_min 2` / 추가로 `--zereco_critpath_edge_conf_exempt_depth 2`. 결과로 A 최종값을 정한다 |
| **N-3** | **mcf 82875 / 28781 진단** — A3-e1의 전체 손실 −0.75%p 중 mcf가 0.47%p(63%), 나머지 workload는 각 0.08%p 이하(Cumulative, C13). Periodic(C15)으로는 −0.69%p 중 mcf 0.39%p(56%), 다음이 clang 0.11·xgboost 0.08%p. 예산 sweep(C16)의 모든 점에서도 mcf의 crit − full이 −4.4~−4.8%p로, crit/full 순서를 정하는 요인이다. critical vs A3-e1에서 brslice_tab 멤버(PC, depth, edge 신뢰도, producer flip 빈도)를 끝에 dump해 어떤 멤버가 빠지는지 비교 | 진단 전용 knob 필요(코드). 결과에 따라 "번갈아 오는 두 producer를 둘 다 따라가는" 식의 A 보완 검토 |
| (선택) | **owner 기준 chain 제거** — 기본은 하지 않는다(DESIGN 결정 표, 2026-09-14). 시험한다면 owner branch의 HBT counter가 **0**이 됐을 때만 제거(강등 뒤 50K 동안 오예측 없음) — 3-bit counter의 여유를 hysteresis로 써서 강등·복귀를 반복하는 branch의 chain을 지우지 않게 | refresh sweep에 조건 하나 추가(knob, 코드 몇 줄). 비교: refresh만 / refresh + owner 제거 / owner 제거만 |
| (선택) | **refresh를 끈 짝** (`--zereco_critpath_decay_interval 0`) — 필터 A가 있는 상태에서 refresh의 추가 기여 측정 | 제안만 됨, 미결정 |
| (선택) | **후보 G — H2P 그림자 priority 차단**: 아직 resolve되지 않은 H2P branch 뒤에서 fetch된 op에는 priority bit를 주지 않는다. off-path 켬(C11)에서는 priority 자격 dispatch의 67.8%가 wrong-path라 하드웨어 동작에서 줄일 몫이 크다 | 평가 모드가 oracle이라 우선순위 낮음(사용자: A/B/C 이후 방향이 없으면 시험). 구현은 frontend의 "in-flight 미해결 H2P branch 수" 카운터 하나 |
| 보류 | Golden Cove 186 머신 sweep (`zereco_dbg_gc186_sweep.json`) | 이 descriptor는 critical-path 세부 flag를 주지 않아 이제 새 기본값으로 돈다 |

## 2. 미결정 설계 항목

| # | 항목 | 현재 | 상태 |
|---|---|---|---|
| D-10 | P-IQ partition 예약률 | 25% (88 entry) | C4: 상주 priority op는 partition의 15%, fallback 8%(oracle). 15~20%로 줄일 여지 — 축소 시 fallback 증가와 맞바꿈. C16: 25 → 20 / 15 / 10%에서 Both crit IPC −0.13 / −0.39 / −0.69%p, fallback 8 → 12 / 17 / 25%. **사용자 결정** |
| D-2 | Δ-window (`\|t_last − t_second\| < Δ`면 **양쪽** producer 삽입) | 미구현. Δ=0이고 wake 훅이 strict 비교라 동률이면 먼저 관측된 producer가 LPR | 멤버를 늘리므로 filtering 방향과 반대. 정확도 관점의 후보, 후순위(대상 모집단은 멤버 commit의 5.2%) |
| E | H2P branch별 chain 분리 (owner 여러 칸) | entry당 owner 1칸(통계용), 공유 멤버는 마지막에 지목한 branch로 덮어씀 — 재지목의 11.2%(A3-e1) | 저장 비용 때문에 보류 |

## 3. 계측 · 코드

- (선택) **address-generation slice 통계** — Target Load의 가속 경로가 주소 계산 쪽인지 측정.

  | 카운터 | 답하는 것 |
  |---|---|
  | Target Load의 LPR이 주소 operand producer인지 데이터 operand producer인지 | 가속 경로가 주소 쪽인가 |
  | Target Load의 주소 chain 길이 | 주소 계산이 몇 단계인가 |
  | priority 받은 주소 chain이 load의 AGU 도달을 얼마나 앞당기나 | 효과 크기 |
  | RFP 실패(low confidence 43%) Target Load 중 주소 chain 가속만으로 회수되는 비율 | 두 메커니즘 상보성의 직접 증거 |

  힌트: `op->oracle_info.src_info[]`/`op->table_info`로 주소 source 집합을 판정한 뒤 `op->critpath_last_src`와 대조.
- `RFP_INFLIGHT_UNDERFLOW` (PT 축출 후 같은 PC 재할당 시 0 카운터 감소) — 무해(injected의 0.02%), 미수정.
- full-slice 모드의 priority op 분류는 commit 시점 shadow 멤버십 기준(bit는 fetch 시점) — 소수 오분류 가능, 비율 통계에는 무시할 수준.
- (선택) `LEGACY_WALK_NEEDED()`가 false일 때 Fill Buffer/walk 메모리 할당 자체도 생략 (host 메모리).
- PRF가 작은 머신에서 `datacenter/gcc/939`가 교착한다(frontend watchdog "No forward progress") — GC186 baseline(PRF 280)과 TEA PRF 400 run에서 발생, PRF 592 머신에서는 없음. 원인 미확인. **새 머신(RS 186, PRF 280)에서 재발할 것으로 보고 C18 실험에서는 제외했다(사용자 결정 2026-09-15).** 단서: GC186에서는 baseline 포함 5개 config 중 4개가 retire 기준 같은 지점(I=16,308,185, O=20,751,145)에서 멈췄다(타이밍 무관, 나머지 1개는 I=16,014,036). TEA PRF 400은 I=14,726,427. rs352 run의 timeline상 그 직후 구간은 uop/명령어 2.2로 uop가 많은 명령어가 몰린 곳. 코드 검토로 배제한 것: rename 문턱(op당 목적지 4·2 × issue 폭 ≪ 빈 PRF), decode 복구 시 레지스터 누수(복구가 rename보다 먼저 걸림, 여유 0 cycle), 명령어 단위 retire(uop 단위임).

## 4. 워크로드 · 방법론

- **67 simpoint**(workload당 5, clang 4, gcc 3; 14 workload) — TEA 비교와 동일 표본. weight 가중 집계, 효과 기준 선별 금지. **새 머신(C18~)은 gcc/939를 뺀 66개**: gcc는 2766·907만 남는데, 이 둘은 gcc 실행의 14.7%에 해당한다(939를 포함한 3개는 46.0%, 939 혼자 31.3%). gcc 결과를 해석할 때 유의한다.
- **IPC 지표 = Periodic만**(사용자 결정 2026-09-16): 모든 통계는 warm-up 뒤 10M~20M 구간만 쓴다. DESIGN C1~C13은 Cumulative(warm-up 포함 20M)로 잰 옛 값이고, C14부터 Periodic이다.
- frontend watchdog으로 죽는 simpoint는 원인 추적 없이 제외한다. 원본 Golden Cove 머신에서 죽은 것: gcc/939, clang 1270·1305·2249, xgboost 3311 (clang 2249와 xgboost 3311은 TEA를 꺼도 죽는다).
- 다른 목록: 108 simpoint(`json/zereco_dbg_rs352.json`), Golden Cove 186용 105 simpoint(`json/zereco_dbg_gc186_sweep.json`) — 보류. 108로 돌아갈 경우 deepsjeng 133677·164928은 퇴화한 trace(H2P misprediction 0, uop/instruction 4.0, chain 멤버 0)라 결과와 무관한 기준으로 제외하고 명시한다.

## 5. 논문 서술 시 유의

- 머신: DESIGN C1~C17은 352 entry 머신(`PARAMS.golden_cove_rs352`; main 한도는 정확히 353, partition %의 분모 352) 결과다. 논문 수치는 원본 Golden Cove 머신(main RS 186) 결과(C18~)로 바꾼다.
- TEA 비교 수치는 원본 Golden Cove 머신 결과(C19·C20)를 쓴다. `260913_TEA`(C14)는 옛 352-entry 머신 결과다. 새 TEA는 oracle trigger 설정만 쓴다 — 논문대로 모든 H2P에 띄우면 baseline보다 11% 느리다(C19).
- 평가 모드: wrong-path 명령어는 priority를 받지 않는다고 가정(oracle)함을 명시하고, 하드웨어 동작(C11: IPC −0.85%p)을 민감도로 제시한다. 시뮬레이터에서만 가능한 결과는 상한(limit study)으로 표기한다.
- 1차 지표는 H2P resolution latency (IPC 실현률은 그보다 낮음).
- critical vs full slice: C3의 6~7%는 refresh·필터 A 이전의 정적 PC 단위 한계이고, 기준 설정(A3-e1)에서는 full 대비 25.6%다(C13). 그 대가로 IPC는 Both crit이 full보다 0.6%p 낮다(필터 A 비용, C15). P-IQ 구획이나 brslice_tab을 줄여도 뒤집히지 않는다(C16). 대신 자원 절감은 측정됐다: 상주 멤버 PC −63%, priority 후보 −26%(C16).
- xgboost·tc 표본 성격은 methodology에 적지 않는다(사용자 방침) — 결과 해석에서만 유의.
