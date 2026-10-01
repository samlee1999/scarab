# TODO — 할 일

> Last updated: 2026-10-01 · branch `test`
> **이 문서 = 할 일만.** 연구 방향·설계·확정된 결과는 [zereco_CRITPATH_DESIGN.md](zereco_CRITPATH_DESIGN.md).
>
> - 기법 이름 **CLEAR**(구 ZERECO). descriptor는 `scarab-infra/json/clear.json` → `./sci --build-scarab clear`, `./sci --sim clear`.
> - **표준 config = 새 CLEAR baseline (2026-10-01 확정)**: 260923 표준 + **필터 A**(비율 방식 X 50%, N 4, 창 64 — `--zereco_critpath_edge_ratio 50 --zereco_critpath_edge_ratio_min_count 4 --zereco_critpath_edge_ratio_window 64`, 2026-10-01부터 코드 기본값, `json/clear.json`에도 명시). 결과 `261001_CLEAR_edge_ratio_finite/261001_clear_std_rfp_pt_r50_n4`, **1.1092** (full slice 1.1086과 동률, TEA 1.1020보다 앞섬). 이제 "필터 A"는 비율 방식을 뜻하고, 예전 연속 4회 규칙은 "이전 필터 A"다. 직전 표준 `260923_clear_gate1_consume`(이전 필터 A, 1.1033): P-IQ 15%(29/186) non-stall + RFP, critical slice, oldest-first, 예약 동적 해제 dwell 0, HBT 256×8, seed gate 1 + 소비 게이트 + owner 보호, prefetcher on — 필터 A 말고는 그대로 새 표준에 이어진다.
> - TEA 기준 **`260920_zereco_n_tea (최신)/TEA_preg102_vec16`** (1.1020). TEA 코드는 `tea-random-queue` 브랜치에서만 만진다.
> - baseline **`260917_zereco_n_tea/baseline_nopref` 고정, 다시 돌리지 않는다.** 표본 89 SimPoint, 통계는 Periodic만.
> - **시뮬레이션 실행은 사용자가 한다.** 요청받지 않으면 run하지 않고, 실행 중인 배치의 완료 여부도 추적하지 않는다.
> - 브랜치를 바꾼 뒤에는 `rm -rf src/pin/pin_exec/obj-intel64`(옛 브랜치 `.d`가 남아 빌드가 깨진다).

---

## 1. 진행 중

진행 중인 배치 없음. 설계는 필터 A까지 확정됐다(구조·임계·크기·필터). 비교 모음은 `/home/lee/simulations/261001_COMPARSION`(7 config, 출처는 그 폴더의 `SOURCES.md`)이다. 남은 것은 아래 후보들이다.

| # | 내용 | 상태 |
|---|---|---|
| **R-5** | brslice_tab 크기 sweep (`260923_CLEAR_gate1_tabsize`, 512/2K/4K × 89) | **완료.** 512~4K에서 IPC 차 **0.04%p**, 2K에서 축출 0이어도 이득 없음 → **1K 확정**. mcf 손실은 네 크기에서 동일(1.4649)이라 **용량 탓이 아님**. DESIGN §9.8(f) |
| **R-4** | 소비 게이트 + seed gate sweep (`260923_CLEAR_consume_gate`, 3 config × 89) | **완료.** gate 1 채택(1.1033). 소비 게이트 자체는 중립(−0.02%p), 추적 확대가 +0.23%p. gate 0은 테이블 붕괴로 −0.17%p. DESIGN §9.8 |
| **R-3** | HBT 256×8 교체 + 분기 상태 분포 (`260923_CLEAR_hbt256`, 2 config × 89) | **완료.** HBT 교체는 성능 중립(+0.09%p). 분기의 75%가 한 번도 안 틀리고 9.9%만 H2P 도달. 계측은 cycle 불변(89/89). DESIGN §9.8 |

## 2. 다음 실험 후보

| # | 내용 | 메모 |
|---|---|---|
| **N-1** | **mcf 진단 (우선순위 1)** — 이제 두 손실이 같은 곳을 가리킨다: 이전 필터 A 비용의 절반 이상이 mcf이고, 소비 게이트의 −1.70%p도 mcf다(테이블 크기와 무관하게 1.4649 고정 → 용량 아님). 이전 필터 A가 있고 없고에 따라 brslice_tab 멤버(PC, depth, edge 신뢰도, producer flip 빈도)를 끝에 dump해 **어떤 멤버가 빠지는지** 비교 | **대부분 해소(2026-10-01)**: 필터 A(비율 방식)로 mcf가 +6.9~7.9%p 회복해 full slice와 같아졌다(DESIGN §9.9). 이전 필터 A가 끼어드는 형제 때문에 주된 입력까지 막던 것이 원인이었다. 소비 게이트의 −1.70%p 진단은 필요하면 다시 연다 |
| **N-2** | ~~이전 필터 A 임계 재확인~~ | **닫음(2026-10-01)**: 필터 A를 비율 방식으로 바꿔 대상이 사라졌다. 면제 깊이(depth ≤ 1)는 비율 방식에도 그대로 둔다 |
| **N-3** | **10% 미만 예약 비율** — 두 dwell 모두에서 "작을수록 좋다"가 10%에서 멈추지 않았다. 0%에서는 어떤 op도 우선권을 못 받아 RFP 단독(≈1.07)으로 떨어지므로 **10%와 0% 사이에 꼭짓점이 있다**. 2/4/6/8% 4~5 config × 89 | 표준 15%를 바꾸자는 제안이 아니라 곡선의 꼭짓점을 아는 것이 목적(논문의 "예약은 적을수록 좋다" 주장을 받치는 데이터). descriptor는 `clear_rs<pct>_nonstall_dwell0` 이름 그대로 쓰면 되고 `results.py`가 이미 인식한다 |
| **N-4** | **표 1 정밀도·재현율의 채점 기준 고치기** — 지금은 가속이 켜진 run 안에서 critical을 판정한다. 그래서 가속이 도착 순서를 뒤집으면, baseline에서 critical이던 op를 제대로 가속한 경우를 실패로 센다. 문제·기존 데이터로 본 크기·고칠 방법은 아래 **N-4 상세** | 계측 추가(코드, cycle 불변) + baseline·CLEAR·Full slice 3개 × 89 재실행. 논문에 표 1을 싣기 전에 필요 |
| **N-5** | **CLEAR가 Full slice보다 느린 이유와, 작은 표에서의 이점** (260926에서 Full이 Avg +0.53%p, mcf·zstd_r_1을 빼면 +0.25%p). 네 묶음, 15 config × 89 = 1,335 run: ① **무한 크기**(HBT·brslice_tab·PT 각 2,097,152 entry = 131,072 set × 16-way. 세 표 모두 전체 PC가 태그라 크기만으로 aliasing이 사라진다) × {CLEAR, Full} × {non-stall, stall} 4개 — 추적 손실이 없을 때 Full이 P-IQ 칸 부족에 걸리는지 ② **표준 크기 stall** CLEAR·Full 2개(non-stall 짝은 pt1024와 260926_clear_precision_full) ③ **PT sweep**(8-way): CLEAR 1024 / 256 / 128 / 64 / 32 5개 + Full 256 / 128 / 64 / 32 4개(Full의 1K 점은 260926_clear_precision_full) — 방향 A: 작은 표에서 Full이 CLEAR보다 먼저 무너지는지 ④ **가속됐는데도 critical로 남은 비율**: 기록 장치가 RF prefetch 가속(`op->zereco_rf_covered`)도 기록한다. `CRITPATH_PREC_ACCEL_L2 / CRITPATH_PREC_ACCEL`(ACCEL = priority 전용 칸을 받았거나 RF prefetch로 값을 받음). 무한 4개, 표준 stall 2개, pt1024에서 잰다 | **완료 (2026-09-30)**. 합격 기준 전부 통과(`260929_CLEAR_infinite_critical_vs_full/analysis/acceptance.txt`). 결과(`analysis/summary.txt`): 무한 크기의 효과는 CLEAR +0.02, Full +0.05%p다 — aliasing은 원인이 아니다. stall은 둘 다 크게 잃지만(−4.0 / −5.6%p) stall에서는 CLEAR가 Full을 1.1%p 앞선다. PT sweep에서는 Full이 더 빨리 떨어지지만 역전은 없다(Full PT 64 ≈ CLEAR PT 1K). 가속 후에도 critical로 남은 비율은 CLEAR 65~69%, Full 50~53%다 [가속 뒤 상태로는 정확한 값 — 선택 정확도로 해석 금지] |
| **N-6** | **brslice_tab 크기와 스케줄러에 따른 CLEAR vs Full** (같은 실험 폴더 `260929_CLEAR_infinite_critical_vs_full`의 배치 2, 11 config × 89 = 979 run). ① brslice_tab 512 / 256 / 128 / 64 entry(8-way) × {CLEAR, Full} 8개: 표가 작아질 때 Full이 critical producer를 놓치는지(기록 장치의 재현율)와 그만큼 느려지는지 본다(방향 A). 1K 점은 `260929_clear_pt1024`와 `260930_full_tab1024`다. 후자는 260926 Full을 그대로 다시 돌린 것으로, RF prefetch 기록을 채우고 대조군을 겸한다. ② random-queue 스케줄링(`node_issue_queue_schedule_scheme 1`) × {CLEAR, Full} 2개. 모든 config에 기록 장치를 켠다 | **완료 (2026-09-30)**. 사용자가 tab64 두 개를 빼고 실험 이름을 `260930_CLEAR_infinite_critical_vs_full`로 바꿔 9 config로 돌렸다. 합격 기준 전부 통과(`analysis/acceptance.txt`). 결과(`analysis/summary.txt`): brslice_tab 1K→128에서 Full의 재현율은 86.6→73.1%로 떨어지고(CLEAR는 71.2→66.5%) [가속 run 자체 판정 — "가속 전 critical"·재현율로 해석 불가(N-4)], 성능은 −0.23%p(CLEAR −0.13%p) 떨어진다. 격차는 +0.53→+0.43%p로 줄지만 역전은 없다. random-queue는 둘 다 약 −2.5%p이고, 이때도 Full이 +0.43%p 앞선다. Full 표준의 '가속 후에도 critical' 비율은 51.1%다(CLEAR 66.3%) [가속 뒤 상태로는 정확한 값 — 선택 정확도로 해석 금지] |
| **N-7** | **RF prefetch를 지금의 멤버에만 발사** (사용자 결정 2026-09-30: 성능보다 논문 의도와 같은 동작이 우선). 지금은 PT에 entry가 있으면 prefetch하고, 멤버인데 owner 분기가 H2P가 아닐 때만 막는다. 그래서 CLEAR가 RF prefetch한 load의 56%가 brslice_tab 멤버가 아닌 load이고, 그중 prefetch를 받은 뒤에도 H2P 사슬 위에 있는 것은 8%다 [가속 뒤 상태로는 정확한 값 — 선택 정확도로 해석 금지]. `rfp_launch_member_only 1`이면 우선권 bit와 같은 판정(`critpath_is_member()`)을 통과한 load에만 발사한다. PT 학습과 실행 횟수(in-flight) 세기는 그대로 둔다 | **완료 (2026-09-30)**. `260930_CLEAR_rfp_member_only`, 합격 기준 전부 통과(`analysis/acceptance.txt`). 결과(`analysis/summary.txt`, 그림 `fig_rfp_member_only_ipc`): member-only에서 CLEAR는 1.1033→1.0978(−0.54%p), Full은 1.1086→1.1051(−0.35%p)이라 격차가 +0.53→+0.73%p로 벌어졌다(무한 크기에서는 +0.86%p). RF prefetch를 받은 load 중 critical 비율 31→55%, '가속 후에도 critical' 66.3→80.0%는 [가속 뒤 상태로는 정확한 값 — 선택 정확도로 해석 금지]이다. 다만 이것으로 빠진 prefetch가 원래 critical이었는지는 판단할 수 없다. 확실한 사실은 멤버가 아닌 load의 RF prefetch를 끊자 IPC가 떨어졌다는 것이다. 필터 A(X 50%)에서 다시 재면 member-only 비용은 CLEAR −0.47%p(1.1092 → 1.1045), full −0.35%p(1.1086 → 1.1051)다. **채택 여부는 사용자 결정 대기** |
| **N-8** | **필터 A 비율 방식** — 연속 4회 대신 "이번에 마지막 도착한 입력이 최근 실행의 X% 이상, 최소 N번 마지막 도착"이면 전파 | **확정(2026-10-01, 사용자): X 50%, N 4, 창 64.** 설계와 결과는 DESIGN §5.3·§9.9로 옮겼다. 남은 일: ①② 끝(2026-10-01): 코드 기본값을 X 50%로 바꿨고(주석 갱신, 바이너리 08c447f3에서 기본값 50.0 / 4 / 64 확인), `json/clear.json`은 실험 `261001_CLEAR_baseline`으로 두 config(`261001_clear` member-only 끔, `261001_clear_rfp_member_only` 켬)에 필터 A 플래그를 넣었다. ③ 끝(2026-10-01): 무한 크기 + member-only 끔 + 필터 A(`261001_CLEAR_edge_ratio_inf_pt`)는 1.1095다(이전 필터 A 1.1035, Full 1.1091). run은 `261001_COMPARSION/CLEAR_inf_member_off`에 있다 |
| (선택) | **gate 0 × 큰 테이블** — gate 0의 붕괴가 용량 탓이 아님을 못 박으려면 4K에서 한 번(1 config × 89). 논문에 "1K 기준"이라고만 적고 넘어가도 된다 | 방어용. gate 1이 512에서도 멀쩡한 것과 대비하면 이미 정황은 충분하다 |
| (선택) | **Fig C — 테이블 크기 무감각성** 시각화 | Contribution 1의 "식별 구조는 싸다" 비용 논거를 직접 받친다. 데이터는 `260923_CLEAR_gate1_tabsize/tabsize.txt`에 있다 |
| (선택) | **H2P 그림자 priority 차단** — 아직 resolve되지 않은 H2P 뒤에서 fetch된 op에는 priority bit를 주지 않는다 | 평가 모드가 oracle이라 우선순위 낮다. 구현은 frontend의 "in-flight 미해결 H2P 수" 카운터 하나 |

### N-4 상세 — 표 1 정밀도·재현율은 가속 run 안에서 채점된다 (2026-09-29 사용자 지적)

대상: `simulations/260926_CLEAR_precision/analysis/table1_core_results.*`의 정밀도·재현율(정리 문서 `PRECISION_ANALYSIS.md`).

**문제**
- 마지막 도착 producer는 **가속이 켜진 그 run의 도착 시각**으로 고른다(`critpath.c`의 `critpath_note_wake`). critical 판정은 H2P 분기에서 이 선택을 거꾸로 따라간다(`critpath_precision.c`의 `prec_walk`). 그래서 가속이 도착 순서를 바꾸면 판정도 바뀐다.
- 예: op1의 producer가 fast와 slow이고, baseline에서는 slow가 마지막에 도착한다(slow가 critical). CLEAR가 slow를 가속해 fast보다 먼저 도착하게 만들면, op1의 마지막 도착 producer는 fast가 되고 사슬도 fast 쪽으로 간다.
  - slow: 가속했는데 critical 아님 → 정밀도 손해
  - fast: critical인데 가속 안 함 → 재현율 손해
  - 한 칸이 아니라 사슬 전체가 옮겨 간다. 다른 사슬에 걸리지 않는 한 slow의 윗단(slow의 producer들)이 빠지고 fast의 윗단이 들어온다.
- 즉 지금 지표는 "baseline에서 critical이던 op를 골랐나"를 재지 못한다. 제대로 가속해 사슬에서 빼 낸 경우를 실패로 센다.
- CLEAR와 Full slice가 각자 자기 run의 critical path로 채점되므로, 표의 두 행이 서로 다른 기준을 쓴다는 문제도 있다.

**기존 데이터로 본 크기 (확정 아님)**

두 run(`260926_clear_precision`, `260927_baseline_nopref_precision`)은 commit 흐름이 같다. PC별 commit 수가 89개 중 84개 SimPoint에서 완전히 같고, 전체 10.6억 µop 중 차이는 12개다. 그래서 PC마다 같은 실행들을 두 run에서 비교할 수 있다. 아래는 두 run의 PC별 CSV(`critpath_precision.csv`)와 카운터로 계산했다.
- 뒤집힐 상대가 없는 경우가 대부분이다. brslice_tab 멤버가 소비자인 경우의 79%는 rename 때 commit 전 producer가 하나뿐이다(baseline run). 규칙이 그 하나를 고르므로 가속해도 사슬이 옮겨 갈 곳이 없다. 둘 이상인 21% 중 도착 간격이 2 cycle 이하인 경우는 4.3%, 4 cycle 이하는 6.9%다(소비자 전체 대비).
- CLEAR가 실행의 95% 이상을 가속하는 PC(가속 µop의 58%)에서는 채점 방식이 값을 바꾸지 않는다. baseline 기준으로 채점해도 정밀도가 86.8~87.6%이고, 표 1 방식 값은 87.4%다(DC mysql은 해당 PC가 없어 30개 워크로드 평균).
- PC별 critical 수가 두 run 사이에 바뀐 양은 합쳐서 baseline critical의 4.1%다. 그중 가속 PC가 잃은 양은 1.5%, 가속 안 된 PC가 얻은 양은 0.5%다.
- 같은 소비자 명령어에서 마지막 도착 producer가 직전 실행과 다른 명령어로 바뀐 비율은 baseline 19.9%, CLEAR 19.9%로 같다.
- **한계**: 나머지 42%(PC 실행의 일부만 가속)는 같은 PC 안에서 어느 실행이 critical인지를 두 run 사이에 맞춰 볼 수 없다. PC마다 가능한 최소 `max(0, a+b−n)`와 최대 `min(a, b)`를 모은 baseline 기준 범위는 결론을 내기엔 넓다(a = 가속한 실행 수, b = baseline에서 critical인 실행 수, n = 실행 수).

  | | 표 1 (가속 run 기준) | baseline 기준으로 가능한 범위 |
  |---|---|---|
  | CLEAR 정밀도 | 82.0 | 69.7 ~ 84.6 |
  | CLEAR 재현율 | 71.2 | 62.8 ~ 73.0 |
  | Full slice 정밀도 | 57.6 | 50.4 ~ 60.3 |
  | Full slice 재현율 | 86.6 | 75.6 ~ 89.9 |

**고칠 방법**
1. **계측** (`critpath_precision.c`, 측정 전용이라 cycle 불변이어야 함)
   - commit 순번을 두고, 기록이 확정될 때(`prec_retire_record`) µop마다 2비트(가속했나 / critical이었나)를 commit 순서대로 모아 run 끝에 파일로 쓴다.
   - 두 run의 µop이 같은 순서로 맞는지 확인할 값(예: 4,096 µop마다 PC를 XOR한 값)도 같이 쓴다. 출력은 SimPoint당 수 MB다.
2. **run**: baseline(`260927_baseline_nopref_precision` 설정)·CLEAR·Full slice 3개 × 89 SimPoint. 합격 기준에 기존 run과 cycle 동일(89/89)을 넣는다.
3. **분석**: run 밖에서 commit 순번으로 맞춰 본다.
   - baseline 기준 정밀도·재현율
   - 위 예의 수(가속함 & baseline에서 critical & 가속 run에서 critical 아님)와 반대 방향의 수
   - 세는 구간의 시작점을 commit 기준으로 두 run에 맞춘다(아래 "같이 고칠 것")
4. **논문 지표 결정 (사용자)**
   - baseline 기준을 주 지표로 쓰면 위 예를 맞힌 것으로 세고, CLEAR와 Full slice를 같은 기준으로 채점한다.
   - 다만 이 방식은 반대 방향으로 치우친다. 순서가 뒤집힌 뒤 CLEAR가 학습으로 fast까지 가속하면, fast는 baseline에서 critical이 아니므로 정밀도 손해로 센다.
   - 추천안은 두 방식을 나란히 싣고 위 예의 수를 같이 적는 것이다.

**같이 고칠 것 — 세는 구간의 시작점 차이**
- 카운터(`CRITPATH_PREC_*`, 표 1)는 fetch한 명령어 수가 1,000만일 때(warm-up dump) 리셋된다. PC별 CSV(case 분포 그림)는 commit한 명령어 수가 1,000만일 때부터 센다(`critpath_precision.c:214`, `inst_count[proc_id] < FULL_WARMUP`).
- 대부분 SimPoint에서 차이는 µop 65,536개 이하다. 다만 DC gcc 2766은 CSV가 103만 µop(6.7%)을 더 센다. 표 1 Average 영향은 0.03%p 이내이고, 가장 큰 차이는 DC 정밀도(79.8 → 79.6%)다.
- 코드로 고치려면 214행의 조건을 `!warmup_dump_done[proc_id]`로 바꾼다(다음 run부터 적용). 위 1의 commit 순번 기록을 쓰면 분석 쪽에서도 맞출 수 있다.

## 3. 코드 작업

- ~~HBT를 256으로~~ — **완료**(2026-09-23). 논문 사양인 256 entry **8-way**로 재작성했고 파라미터화했다(`hbt_entries`/`hbt_assoc`). **TEA 브랜치는 아직 256 direct-mapped**다 — H2P 정의를 양쪽에서 완전히 맞추려면 TEA 쪽도 8-way로 바꾸고 재실행해야 한다(사용자: 나중에).
- (선택) **HBT 크기 민감도** — 파라미터화됐으므로 `--hbt_entries/--hbt_assoc` sweep이 가능하다. 지금 256×8은 DC에서 상주 252/256으로 포화 상태이고 할당 실패가 29.6%다(다만 놓치는 실행량은 0.76%).
- (선택) **address-generation slice 통계** — Target Load의 가속 경로가 주소 계산 쪽인지 측정.

  | 카운터 | 답하는 것 |
  |---|---|
  | Target Load의 LPR이 주소 operand producer인지 데이터 operand producer인지 | 가속 경로가 주소 쪽인가 |
  | Target Load의 주소 chain 길이 | 주소 계산이 몇 단계인가 |
  | RFP 실패(저신뢰 43%) Target Load 중 주소 chain 가속만으로 회수되는 비율 | 두 메커니즘 상보성의 직접 증거 |

  힌트: `op->oracle_info.src_info[]`/`op->table_info`로 주소 source 집합을 판정한 뒤 `op->critpath_last_src`와 대조.
- `RFP_INFLIGHT_UNDERFLOW` (PT 축출 후 같은 PC 재할당 시 0 카운터 감소) — 무해(injected의 0.02%), 미수정.
- (관찰, 미수정) 기존 소비 게이트(`critpath_owner_blocks`)는 실행 횟수(`inflight++`)를 세기 **전에** 빠져나간다. 그런데 commit 때 `base_va`는 모든 실행이 갱신하므로, 분기가 다시 H2P가 된 직후 몇 번의 prefetch 주소가 아직 비행 중인 막힌 실행 수만큼 틀어질 수 있다. 새 `rfp_launch_member_only` 게이트는 이 문제를 피하려고 횟수를 센 **뒤**에 두었다. **사용자 결정(2026-09-30): 심각하지 않다고 판단해 고치지 않는다.**
- full-slice 모드의 priority op 분류는 commit 시점 shadow 멤버십 기준(bit는 fetch 시점) — 소수 오분류 가능, 비율 통계에는 무시할 수준.
- (선택) `LEGACY_WALK_NEEDED()`가 false일 때 Fill Buffer/walk 메모리 할당 자체도 생략 (host 메모리).

## 4. 미결정 설계 항목

| # | 항목 | 현재 | 상태 |
|---|---|---|---|
| D-2 | Δ-window — `\|t_last − t_second\| < Δ`면 **양쪽** producer 삽입 | 미구현(Δ=0, 동률이면 먼저 관측된 producer가 LPR) | 멤버를 늘리므로 filtering과 반대 방향이다. **N-8(필터 A 비율 방식) 다음 후보**다(2026-10-01). Full은 Δ의 영향을 받지 않는다. 번갈아 오는 근접 도착은 N-8이 먼저 잡으므로, Δ만 잡는 것은 "늘 근소하게 먼저 오는" 형제다. 멤버 소비자 중 간격 ≤2 cycle은 baseline 4.3%, 가속 CLEAR 5.7%다(가속 CLEAR에서 ≤4 cycle은 9.0%, ≤8 cycle은 12.2%). 가속이 간격을 좁히므로 고정 Δ에 걸리는 쌍이 늘어난다 |
| E | H2P별 chain 분리 (owner 여러 칸) | entry당 owner 1칸(통계용), 공유 멤버는 마지막 지목 branch로 덮어씀(재지목의 11.2%) | 저장 비용 때문에 보류 |

## 5. 표본·방법론 메모

- **89 SimPoint / 31 workload** (GAP 6, SPEC17 11, SPEC26 8, DC 6). `json/clear.json`과 `json/tea.json`의 목록은 동일하다. 효과 기준 선별 금지, weight 가중 집계.
- frontend watchdog("No forward progress")으로 죽는 SimPoint는 원인 추적 없이 제외한다. 제외 이력: gcc/939, clang 1270·1305·2249, xgboost 3311, gcc_r_1 11560·17632, llvm_r_1 8151·40003·43023·6873·11654, vpr_r_1 7642(미지원 AVX-512 uop).
- 통계는 **Periodic 구간만**(10M~20M). `*.stat.0.out`의 첫 숫자 열.

## 6. 주의

- **가공(비측정) 산출물 2건** — 사용자 요청으로 수치를 바꾼 학습용이며 **논문 사용 금지**:
  `260920_zereco_n_tea (최신)/analysis/table_tea_instruction_share.*` (tc → 40.7, SPEC17/26/DC +10%p),
  `260921_TEA_rs_sweep/analysis/workload_ipc_speedup_adjusted.*` (suite geomean을 목표값에 맞춰 상수배).
  두 스크립트 모두 상수 하나만 지우면 측정값으로 돌아간다. **원본 run 디렉터리(`*.stat.0.out`)는 수정하지 않는다.**
- scarab-infra는 `.csv`가 있는 run을 성공으로 보고 건너뛰고, 없으면 그 디렉터리를 **지우고** 다시 돌린다 — 기존 실험 폴더에 config를 추가할 때 먼저 확인한다.
- 배치가 도는 동안 다른 시뮬레이션이나 `docker_cleaner.py`를 돌리지 않는다(이미지가 사라져 남은 run이 시작하지 못한 사고가 있었다).
