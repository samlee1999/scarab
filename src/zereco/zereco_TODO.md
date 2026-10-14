# TODO — 할 일

> Last updated: 2026-10-13 · branch `test`
> **이 문서 = 할 일만.** 연구 방향·설계·확정된 결과는 [zereco_CRITPATH_DESIGN.md](zereco_CRITPATH_DESIGN.md).
>
> - 기법 이름 **CLEAR**(구 ZERECO). descriptor는 `scarab-infra/json/clear.json` → `./sci --build-scarab clear`, `./sci --sim clear`.
> - **CLEAR baseline** = `json/clear.json`의 `261012_clear`(2026-10-12 결정, **2026-10-13 사용자 확정**): 필터 A(비율 X 50%, N 2, 창 128, 분기만 면제) + member-only + 학습 게이트 + PT owner 확인 + 멤버 추가 순간의 PT 할당. full slice 짝 `261012_full`. 결과 `261012_New_CLEAR_baseline`: **CLEAR 1.1048, full slice 1.1049, TEA 1.1020**(DESIGN §0·§9.1). 플래그는 DESIGN §7.2.
> - 직전 표준 `261001_clear`(2026-10-01, member-only 끔)는 1.1092였다(DESIGN §9.1 표의 마지막 줄).
> - 비교 모음은 `/home/lee/simulations/261001_COMPARSION`(7 폴더, 출처는 `SOURCES.md`, 분석은 `analysis/`).
> - TEA 기준 `260920_zereco_n_tea (최신)/TEA_preg102_vec16`(1.1020). TEA 코드는 `tea-random-queue` 브랜치에서만 만진다.
> - baseline `260917_zereco_n_tea/baseline_nopref` 고정, 다시 돌리지 않는다. 표본 89 SimPoint, 통계는 Periodic만.
> - **시뮬레이션 실행은 사용자가 한다.** 요청받지 않으면 run하지 않고, 실행 중인 배치의 완료 여부도 추적하지 않는다.
> - 브랜치를 바꾼 뒤에는 `rm -rf src/pin/pin_exec/obj-intel64`(옛 브랜치 `.d`가 남아 빌드가 깨진다).

---

## 1. 결정 대기 (사용자)

| # | 내용 | 메모 |
|---|---|---|
| **B-1** | **코드 기본값을 새 baseline에 맞출지** — baseline이 확정됐다(2026-10-13, 사용자: "clear.json을 새 CLEAR baseline으로"). 이전 결정은 "New baseline 확정되면 정하자" | 지금 기본값은 2026-10-01 그대로다: min_count 4, window 64, exempt_depth 1, member-only·learn_owner_gate·rfp_target_owner_gate·rfp_target_on_add 모두 끔. 기본값을 바꾸면 이 플래그가 없는 옛 config의 동작이 바뀐다 |
| **P-1** | **논문 그림·지표를 새 baseline으로** — DESIGN §0·§8·§9.1 수치는 2026-10-13에 바꿨다. 남은 것: `260920_zereco_n_tea (최신)/analysis/`의 suite·workload IPC와 H2P latency 분해 그림(이전 표준 1.0972 기준), 그림 4(PT 할당 시점), 기록 장치 지표(slice 몫 DESIGN §9.6, "가속 뒤 chain 위" §9.10) | 워크로드별 IPC 그림은 `261012_New_CLEAR_baseline/analysis/workload_ipc_new_baseline.*`로 그렸다. TEA·CLEAR 워크로드별 그림 `workload_ipc_clear.*`(TEA는 rs74 실측, 막대 위 값은 셋째 자리 올림)도 같은 곳에 있다. 기록 장치 지표를 새 baseline으로 다시 재려면 기록 장치를 켠 run(CLEAR·full, 2 × 89)이 필요하다 |
| **N-4** | **CLEAR 선택 품질 지표 — 결정 대기** (채점 run `261006_CLEAR_baseline_scoring`은 2026-10-06 완료, 2026-10-07 논의를 사용자가 보류) — 두 지표 틀: ① 진짜 critical을 가속했나(높을수록 좋음), ② 가속할 필요가 없는 것을 가속했나(낮을수록 좋음) | 집합(µop instance, H2P 분기 제외): B = baseline에서 critical, A = 가속, C = 가속 run에서 critical, K = priority bit. 수치(옛 기계 — 10-01 baseline, 무한 표, member-only 켬; CLEAR / Full): ① 식별 B∧K/B 82.6 / 86.8, 가속 B∧A/B 65.2 / 66.3, 가속 후에도 critical B∧A∧C/B 62.4 / 63.3, 진짜 놓침 B∧~A∧C/B 33.0 / 32.4. ② ~B∧A/A ≤ 33.0 / 46.5 — 상한이다(형제가 가속된 뒤 critical이 됐다가 다시 밀려난 정당한 가속과 불필요한 가속이 ~B∧A∧~C 한 칸에 섞인다). 남은 결정: ①의 대표 지표, ②를 상한으로 쓸지 마지막·두 번째 입력의 도착 간격을 trace에 더해 좁힐지(새 run). 제안만 한 그림: 경로 안정성 95.8%(B∧A∧C/B∧A), ①·②의 워크로드별 그림. 새 baseline으로 다시 재려면 trace를 켠 OoO·CLEAR·full run(3 × 89, OoO도 HBT 설정을 맞춤)이 필요하다 |
| **N-13** | **PUBS 비교 실험 — 1차(17 config) 완료, 2차 5 config 실행 대기 (2026-10-14)** — PUBS P-IQ 비율 35% 확정(사용자). 2차: PUBS 35% oldest-first × brslice_tab 1K/2K/4K, full slice의 짧은 tag(brslice_tab·PT 6·4 bit, HBT 8). 결과는 `261014_PUBS/analysis/`. — `json/clear_test.json` → 실험 `261014_PUBS`, 17 config × 89: PUBS 14개 = {full, hashed} tag × P-IQ {10, 15, 20, 25, 30, 35, 40}% + CLEAR·full slice의 hashed tag 버전(`261014_clear_hashed`, `261014_full_hashed`, 사용자: full slice의 표 부담이 aliasing으로 드러나게) + 대조군 `261014_clear`(= 261012_clear, 새 바이너리가 새 baseline과 89/89 cycle 동일해야 함). PUBS 그대로: random queue(scheme 1, 원문의 base — 2026-10-14 사용자; CLEAR 3개 줄은 oldest-first), 모든 분기의 slice 연결(insert_gate 0), full slice, 학습 게이트·owner 보호·refresh 없음, 가속만 conf_tab(1K, 6 bit, retire 학습)이 unconfident일 때, priority 구획 정적 + stall, RF prefetch 없음. H2P 통계도 conf_tab 기준(사용자). 바이너리 68e147ec | 합격: PUBS_CONF_UPDATES = HBT_RETIRED_BRANCH_UPDATES, RFP 0, full tag 줄의 *_ALIAS_HITS 0. 남은 선택: stall 대신 non-stall도 볼지, mode switch(LLC MPKI) 추가 여부. 분기 root가 brslice_tab 자리를 차지하는 차이(원문은 분기를 넣지 않음)는 그대로 둔다(사용자 2026-10-14). refresh가 없어 CRITPATH_LIVE_*_AT_SWEEP은 0이다 |
| **N-12** | **PUBS의 H2P 기준(conf_tab)을 CLEAR에 적용 — 사용자 검토 중 (2026-10-13)** — conf_tab이 HBT 역할을 하지만, critical chain 생성 조건(seed, 학습 게이트, PT owner 확인, 소비 게이트, owner 보호)을 conf_tab 형태에 맞게 다시 정해야 한다. 사용자가 더 고민한 뒤 정한다. 출처 전환 스위치(`h2p_source`)와 conf_tab은 PUBS 비교용으로 2026-10-14에 구현됐다(N-13) — 지금 conf_tab 모드의 "추적 대상"은 임시로 unconfident와 같다 | PUBS 원문(§III-A1, Table II): JRS resetting counter(맞히면 +1, 틀리면 0) 6 bit, 최대값(63)이 아니면 unconfident. 실행되는 모든 분기에 할당(맞혔으면 최대값, 틀렸으면 0으로 시작), 1K entry(128 × 8, 4-bit hashed tag), execute 때 갱신·decode 때 조회, 표에 없으면 confident. 신뢰도와 무관하게 모든 분기의 slice를 잇는다(§III-A2). 6 bit에서 동적 분기의 71%가 unconfident(분기 MPKI ≥ 3 프로그램, §V-D). mode switch(10K cycle마다 LLC MPKI < 3.0일 때만 켬)는 별개 정책. HBT와 달리 기준이 시간이 아니라 그 분기의 실행 횟수이고(한 번 틀리면 63번 연속 맞힐 때까지), 준비 단계(counter 1)가 없다. Claude 제안(2026-10-13): unconfident 하나로 두 임계 대체, retire 갱신, PUBS 크기 + full tag, mode switch는 첫 실험에서 제외, 두 기준의 instance 몫·오예측 몫 통계 추가 |

## 2. 다음 실험 후보

| # | 내용 | 메모 |
|---|---|---|
| **N-9** | **유형 2 격차 분해** — member-only에서 full slice가 앞서는 omnetpp·xgboost(zstd)의 격차가 P-IQ와 RF prefetch 중 어디서 나오는가. CLEAR·full × {P-IQ만, RFP만}, 4 config × 89 | DESIGN §9.6(c). member-only가 표준이 됐으므로(2026-10-12) 새 baseline 결과에서 격차가 남는지부터 본다. member-only를 끄면 대부분 사라지므로 RFP 쪽으로 보이지만, 분해 run 없이 확정할 수 없다 |
| **N-3** | **10% 미만 예약 비율** — 두 dwell 모두에서 "작을수록 좋다"가 10%에서 멈추지 않았다. 0%에서는 RFP 단독(≈1.07)으로 떨어지므로 10%와 0% 사이에 꼭짓점이 있다. 2/4/6/8%, 4~5 config × 89 | 표준 15%를 바꾸자는 것이 아니라 "예약은 적을수록 좋다" 주장의 곡선 꼭짓점을 아는 것이 목적이다. descriptor는 `clear_rs<pct>_nonstall_dwell0` 이름 그대로 쓰면 `results.py`가 인식한다 |
| **N-10** | (선택) **instance별 dynamic full slice** — 기록 장치의 walk를 LPR 한 줄이 아니라 모든 register producer로 넓혀, PC 단위가 아닌 실제 slice를 분모로 slice 몫을 잰다 | 지금 slice 몫(DESIGN §9.6(b))은 PUBS식 규칙이 PC 단위로 표시한 slice다. 논문에서 그 정의로 충분하면 필요 없다. 계측(cycle 불변) + CLEAR·full 재실행 |
| (선택) | **소비 게이트의 mcf −1.70%p** — 테이블 크기와 무관하게 1.4649(게이트 off 1.4820)였다. 같은 시기 이전 필터 A 몫은 필터 A로 회복됐고 이 몫만 남았다 | 필터 A 기계에서 게이트 off와 다시 비교해 남아 있는지부터 본다(1 config × 89) |
| (선택) | **gate 0 × 큰 테이블** — gate 0의 붕괴가 용량 탓이 아님을 못 박으려면 4K에서 한 번(1 config × 89) | 방어용. 논문에 "1K 기준"이라고만 적어도 된다 |
| (선택) | **H2P 그림자 priority 차단** — 아직 resolve되지 않은 H2P 뒤에서 fetch된 op에는 priority bit를 주지 않는다 | 평가 모드가 oracle이라 우선순위가 낮다. 구현은 frontend의 "in-flight 미해결 H2P 수" 카운터 하나다 |
| (선택) | **Fig C — 테이블 크기 무감각성** 시각화 | "식별 구조는 싸다"의 비용 논거. 데이터는 `260923_CLEAR_gate1_tabsize/tabsize.txt` |

## 3. 코드 작업

- **커밋 (사용자가 요청할 때)** — 2026-10-02 이후 변경이 전부 미커밋이다: 기록 장치(`CRITPATH_PREC_ACCEL_ROOT`, trace, 나이 bin), 분기 상태 장부(run-up·cool-down), 비교 명령 필터(`_edge_conf_exempt_depth`), 추적 계층(`_track_slice`, 불채택), 학습 게이트(`_learn_owner_gate`), `rfp_target_owner_gate`, `rfp_target_on_add`(producer의 load bit: `op.h`·`map.c`·`map.h`·`op_pool.c`), `hbt_decay_interval`(`bp.param.def`, `bp/hbt.h`·`hbt.c`, `branch_state`), 표의 tag(`zereco/table_tag.h`, `table_tag_hashed` + 폭 `zereco_critpath_tag_bits`·`hbt_tag_bits`·`rfp_pt_tag_bits`, `*_LOOKUPS`·`*_ALIAS_HITS` 통계 — `critpath.c`·`bp/hbt.c`·`hbt.h`·`rfp.c`·`rfp.h`·두 stat.def·두 param.def), H2P 출처 전환과 PUBS conf_tab(`bp/h2p_source.h`, `bp/pubs_conf.c/.h`, `bp/bp.c`, `node_stage.c`, `critpath.c`, `h2p_source`·`pubs_conf_*`, `PUBS_CONF_*`). 현재 바이너리 68e147ec(2026-10-14).
- (선택) **테스트를 repo로** — 단위 테스트(`test_track.c`, 실제 critpath.c·branch_state.c·critpath_precision.c를 include)와 차분·stress 테스트(`diff_test.c`, git HEAD의 critpath.c와 비교)가 scratchpad `track_test/`에만 있다.
- **기록 장치 per-PC CSV의 구간 맞추기** — 카운터(`CRITPATH_PREC_*`)는 fetch 명령어가 1,000만일 때(warm-up dump) 리셋되고, per-PC CSV는 commit 명령어가 1,000만일 때부터 센다(`critpath_precision.c`의 `inst_count[proc_id] < FULL_WARMUP`). 대부분 65,536 µop 이하 차이지만 DC gcc 2766은 CSV가 103만 µop(6.7%)을 더 센다. 고치려면 그 조건을 `!warmup_dump_done[proc_id]`로 바꾼다(다음 run부터). CSV는 case 분포 그림과 PC 포함 검사에만 쓰여 결론에 미치는 영향은 작다.
- **TEA 브랜치 HBT** — CLEAR 쪽은 256 entry 8-way로 바꿨지만 **TEA 브랜치는 아직 256 direct-mapped**다. H2P 정의를 완전히 맞추려면 TEA 쪽도 8-way로 바꾸고 재실행해야 한다(사용자: 나중에).
- (선택) **HBT 크기 민감도** — `--hbt_entries/--hbt_assoc` sweep. 256×8은 DC에서 상주 252/256으로 포화이고, 할당 실패가 평균 29.6%다(놓치는 H2P 실행량은 0.64%, DESIGN §9.8(d)).
- (선택) **address-generation slice 통계** — Target Load의 가속 경로가 주소 계산 쪽인지 측정한다.

  | 카운터 | 답하는 것 |
  |---|---|
  | Target Load의 LPR이 주소 operand producer인지 데이터 operand producer인지 | 가속 경로가 주소 쪽인가 |
  | Target Load의 주소 chain 길이 | 주소 계산이 몇 단계인가 |
  | RFP 실패(저신뢰 43%) Target Load 중 주소 chain 가속만으로 회수되는 비율 | 두 메커니즘 상보성의 직접 증거 |

  힌트: `op->oracle_info.src_info[]`/`op->table_info`로 주소 source 집합을 판정한 뒤 `op->critpath_last_src`와 대조한다.
- (선택) `LEGACY_WALK_NEEDED()`가 false일 때 Fill Buffer/walk 메모리 할당 자체도 생략한다(host 메모리).

**알려진 현상 — 고치지 않기로 함**
- `RFP_INFLIGHT_UNDERFLOW`: PT 축출 후 같은 PC 재할당 시 0 카운터 감소. injected의 0.02%로 무해하다.
- 소비 게이트(`critpath_owner_blocks`)는 in-flight 수를 세기 **전에** 빠져나간다. 그래서 분기가 다시 H2P가 된 직후 몇 번의 prefetch 주소가 틀어질 수 있다. 사용자가 심각하지 않다고 판단했다(2026-09-30). member-only 게이트는 센 **뒤**에 둔다.
- 시뮬레이터의 brslice_tab은 PC로만 찾는다(한 명령어의 uop들이 entry 하나를 공유). 논문은 (PC, uop index)로 서술한다 — 2026-10-12 그대로 두기로 재확인(DESIGN §5.8).
- full-slice 모드의 priority op 분류는 commit 시점 shadow 멤버십 기준(bit는 fetch 시점)이라 소수 오분류가 있다. 비율 통계에는 무시할 수준이다.

## 4. 미결정 설계 항목

| # | 항목 | 현재 | 상태 |
|---|---|---|---|
| **D-2** | **Δ-window** — 마지막 입력과 Δ cycle 안에 도착한 입력의 producer도 함께 멤버로 넣는다 | 미구현(Δ = 0, 동률이면 먼저 관측된 producer가 LPR) | **다음 후보.** 겨냥하는 것은 유형 1(DESIGN §9.6(c))이다 — X 50% 다수결이 받지 않는 소수 입력 중 근접 도착하는 것. mcf는 두 입력이 2 cycle 안에 도착하는 경우가 43%다(평균 27%). 주의: 입력을 넓게 받으면 sssp·llvm이 1.3~2.0%p를 잃는다(X 25% 결과). Δ는 작게 두고 그 손실이 생기는지 같이 본다. full slice는 Δ의 영향을 받지 않는다. 가속이 간격을 좁히므로 고정 Δ에 걸리는 쌍은 가속 run에서 늘어난다 |
| E | H2P별 chain 분리 (owner 여러 칸) | entry당 owner 1칸(통계용), 공유 멤버는 마지막 지목 branch로 덮어씀(재지목의 11.2%) | 저장 비용 때문에 보류 |

## 5. 표본·방법론 메모

- **89 SimPoint / 31 workload** (GAP 6, SPEC17 11, SPEC26 8, DC 6). `json/clear.json`과 `json/tea.json`의 목록은 동일하다. 효과 기준 선별 금지, weight 가중 집계.
- frontend watchdog("No forward progress")으로 죽는 SimPoint는 원인 추적 없이 제외한다. 제외 이력: gcc/939, clang 1270·1305·2249, xgboost 3311, gcc_r_1 11560·17632, llvm_r_1 8151·40003·43023·6873·11654, vpr_r_1 7642(미지원 AVX-512 uop).
- 통계는 **Periodic 구간만**(warm-up 10M 뒤 10M 명령어). `*.stat.0.out`의 첫 숫자 열.

## 6. 주의

- **가공(비측정) 산출물 2건** — 사용자 요청으로 수치를 바꾼 학습용이며 **논문 사용 금지**:
  `260920_zereco_n_tea (최신)/analysis/table_tea_instruction_share.*` (tc → 40.7, SPEC17/26/DC +10%p),
  `260921_TEA_rs_sweep/analysis/workload_ipc_speedup_adjusted.*` (suite geomean을 목표값에 맞춰 상수배).
  두 스크립트 모두 상수 하나만 지우면 측정값으로 돌아간다. **원본 run 디렉터리(`*.stat.0.out`)는 수정하지 않는다.**
- scarab-infra는 `.csv`가 있는 run을 성공으로 보고 건너뛰고, 없으면 그 디렉터리를 **지우고** 다시 돌린다 — 기존 실험 폴더에 config를 추가할 때 먼저 확인한다.
- 배치가 도는 동안 다른 시뮬레이션이나 `docker_cleaner.py`를 돌리지 않는다(이미지가 사라져 남은 run이 시작하지 못한 사고가 있었다).
- 배치가 끝나면 scarab-infra가 descriptor json을 다시 읽어 `collected_stats.csv`를 쓴다 — 배치가 끝날 때까지 그 json을 바꾸지 않고, 그 descriptor로 빌드하지도 않는다.
