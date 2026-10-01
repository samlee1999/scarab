# CLEAR — H2P branch의 critical slice 가속

> Last updated: 2026-10-01 · branch `test`
> **2026-10-01: 필터 A를 비율 방식으로 바꿨다(X 50%, N 4, 창 64 — 사용자 확정).** 이 문서에서 "필터 A"는 이 비율
> 방식이고, 예전 "같은 producer 4번 연속" 규칙은 **이전 필터 A(연속 방식)**라고 부른다. 새 CLEAR baseline 결과는 §9.1·§9.9.
> **기법 이름이 ZERECO → CLEAR로 바뀌었다**(2026-09-21). 코드·파라미터(`zereco_*`)·디렉터리(`src/zereco/`)·
> 기존 결과 폴더 이름은 그대로 두고, descriptor(`scarab-infra/json/clear.json`)와 config 이름만 `clear_*`를 쓴다.
> 이 문서에서 "ZERECO"가 나오면 코드·config 이름이고, 기법을 가리킬 때는 CLEAR다.
>
> **이 문서 = 연구 방향 + 확정된 설계 + 확정된 결과.** 처음 읽는 사람이 §1~§4만 읽어도 무엇을 왜 하는지
> 알 수 있게 썼고, §5~§7이 구현 수준이다. 할 일·미결정은 [zereco_TODO.md](zereco_TODO.md),
> 참고 논문 정독 노트는 [zereco_REFERENCE_NOTES.md](zereco_REFERENCE_NOTES.md).

---

## 0. 한 장 요약

| | |
|---|---|
| **문제** | 예측이 거의 불가능한 분기(H2P)는 예측기를 더 키워도 못 맞힌다. 남는 축은 **틀린 것을 얼마나 빨리 아느냐**다. |
| **측정** | H2P 오예측 분기가 fetch → resolution까지 **92.8 cycle**, 그중 **59.7 cycle(59.8%)이 operand 대기**다. 분기 자신의 select + execute는 2.2 cycle뿐이다. 가속할 대상은 분기가 아니라 **분기를 먹이는 chain**이다. |
| **선행** | TEA는 그 chain을 **별도 helper thread로 다시 실행**한다 — 명령어를 15.1% 더 가져오고 RS 54% · PREG를 예약한다. PUBS는 chain 전체(full backward slice)에 IQ 우선권을 준다 — 커밋 op의 절반이 대상이 된다. |
| **주장 1** | backward slice 전부가 아니라 **매번 마지막에 도착하는 edge(critical slice)만** 따라가면, 추적 비용은 크게 줄고 이득은 거의 같다. |
| **주장 2** | H2P resolution은 **OoO가 이미 가진 구조**(issue queue의 우선권 + register file prefetch)만으로 앞당길 수 있다 — 별도 thread·별도 실행이 필요 없다. |
| **결과** | CLEAR 1.1092, full slice 1.1086, TEA 1.1020 (89 SimPoint, Periodic IPC geomean, 2026-10-01 필터 A 확정 후). GAP 1.168 vs 1.145, DC 1.096 vs 1.020으로 CLEAR가 TEA보다 앞서고 SPEC17·SPEC26은 TEA가 앞선다. full slice와는 IPC가 같으면서 가속 후보 −20%, 상주 멤버 −37%다. CLEAR는 **명령어를 하나도 더 실행하지 않는다.** |

---

## 1. 문제 — 왜 H2P branch인가

### 1.1 misprediction penalty의 두 성분

분기를 틀렸을 때 잃는 시간은 두 부분이다.

- **state recovery penalty** — flush하고 rename/ROB 상태를 되돌리는 데 드는 고정 cycle. 파이프라인 구조가 정하는 값이라 마이크로아키텍처 기법으로 줄일 여지가 거의 없다.
- **misspeculation penalty** — 분기를 fetch한 순간부터 그 분기가 **실제로 실행되어 정답이 드러날 때까지**, 잘못된 경로를 채우는 데 쓰인 cycle. 이 시간이 길수록 wrong path가 깊어지고 버려지는 일이 늘어난다.

예측 정확도를 높이는 연구는 전자와 무관하게 *빈도*를 줄이지만, **예측이 원리적으로 안 되는 분기**(data-dependent H2P: 해시 탐색 결과, 그래프 이웃 비교, 트리 탐색 분기 …)에서는 빈도를 못 줄인다. 남는 축은 **misspeculation penalty를 줄이는 것**, 즉 *틀렸다는 사실을 더 일찍 알아내는 것*이다.

### 1.2 Observation 1 — 시간의 60%는 operand 대기다

baseline(메커니즘 전부 끔, 89 SimPoint, Periodic)에서 **HBT가 hard로 표시한 main thread on-path 분기 중 실제로 틀려서 execute에서 복구된 것**만 골라 fetch → resolution 시간을 분해한 값이다.

| 구간 | 무엇인가 | cycle | 비중 |
|---|---|---|---|
| fetch → dispatch | frontend + window 진입 | 30.89 | 36.7% |
| **operand 대기** | **RS에서 마지막 source를 기다리는 시간 = backward slice의 지연** | **59.69** | **59.8%** |
| select + execute | 분기 자신이 스케줄되고 실행되는 시간 | 2.24 | 3.6% |
| 합계 | | **92.83** | 100% |

suite별 합계는 GAP 54.0 / SPEC17 125.1 / SPEC26 61.5 / DC 114.3 cycle이고, operand 대기 비중은 어디서나 57~63%다.

**해석이 곧 설계 지침이다.** 분기 자신을 아무리 빨리 스케줄해도 2.2 cycle 이상 벌 수 없다. 줄일 수 있는 것은 **분기의 operand를 만드는 명령어 사슬**의 지연이다. 그 사슬은 (a) 스케줄 순서를 못 얻어 늦어지거나 (b) 그 안의 load가 캐시를 기다려 늦어진다 — 그래서 CLEAR의 소비자가 둘(우선 스케줄링 + RF prefetch)이다.

출처: `260920_zereco_n_tea (최신)/analysis/plot_h2p_latency_reduction.py`
(계수기 `ZERECO_H2P_{FRONTEND,DEPENDENCY,SCHEDULER,EXECUTION}_TOTAL` ÷ `ZERECO_H2P_STAGE_TIMING_EVENTS`).

### 1.3 왜 chain 중에서도 "critical"인가

```
        load A ──┐
                 ├── cmp ──> H2P branch
        add  B ──┘
```

`cmp`가 ready되는 시점은 **늦게 도착하는 쪽**이 정한다. 일찍 오는 `add B`를 아무리 앞당겨도 분기의 resolution은 1 cycle도 안 당겨진다. PUBS·TEA는 slice 전체를 대상으로 삼지만, 실제로 resolution을 옮기는 것은 매 instance마다 **마지막에 도착한 edge들의 사슬 하나**다. 그 사슬만 반복적으로 추출하는 것이 CLEAR의 식별부다.

---

## 2. 선행 연구가 치르는 비용 (Observation 2·3)

### 2.1 TEA — 별도 helper thread (MICRO 2024)

TEA는 H2P의 dependence chain을 retire 시점에 학습해 Block Cache에 저장하고, **main thread와 backend를 공유하는 별도 thread로 그 chain을 미리 실행**해 조기 flush를 낸다. 같은 코어에서 우리 baseline 머신에 맞춰 재현한 결과, 두 가지 비용이 측정된다.

**(a) 명령어를 다시 가져온다.** TEA thread가 fetch한 uop은 main thread의 correct-path uop 대비 **평균 15.1%**(GAP 51.8 / SPEC17 7.2 / SPEC26 7.6 / DC 2.8%)다. 이것은 전력·fetch 대역·dcache 포트에 그대로 실리는 추가 작업이고, 그중 **96.3%의 TEA load는 main이 이미 가져온 캐시 줄을 다시 읽는다**.

**(b) 자원을 예약한다.** 논문 비율대로 RS 101/186(54%)과 PREG를 TEA에 떼어 주면, chain이 돌지 않는 동안에도 main이 그 자리를 쓰지 못한다. 예약을 **비활성일 때 풀어 주는 것만으로 +1.96%p**를 얻었다(1.038 → 1.058). 그다음 PREG를 chain별 4분할에서 공유 pool로 바꾸고 예약량을 줄여 최종 **1.1020**까지 올라갔다. 즉 TEA 성능의 상당 부분이 **자원 예약을 어떻게 다루느냐**에 달려 있었다.

### 2.2 PUBS — full backward slice + 정적 IQ 예약 (MICRO 2018)

PUBS는 unconfident branch의 backward slice **전체**에 IQ의 예약된 entry를 주어 먼저 issue한다. 이 방식을 그대로 구현하면 **커밋되는 op의 절반 이상이 "가속 대상"**이 된다(옛 머신 측정 55~61%). 가속 대상이 그렇게 넓으면 (1) 멤버십 테이블이 프로그램의 hot PC 전체를 따라가고, (2) 우선권이 흔해져 우선권의 의미가 옅어진다.

### 2.3 Observation 3 — 정적 예약은 대부분의 시간 동안 비어 있다

CLEAR의 P-IQ도 같은 함정을 공유한다. priority op이 decode~RS 사이에 **하나도 없는 cycle이 평균 30.7%**(GAP 25.4 / SPEC17 21.7 / SPEC26 26.0 / **DC 59.0**)이고, 예약 47칸 중 실제 priority 점유는 평균 6.7칸이다. 반대로 normal 쪽 dispatch는 27.6% cycle 막혀 있었고 **그 중 95.3%가 priority 슬롯 13.3칸을 비워 둔 채**였다.

→ 예약은 **가지고 있는 것이 아니라 필요할 때만 잡는 것**이어야 한다(§5.7).

---

## 3. Motivation과 두 개의 Contribution

> **Contribution 1 — critical slice.**
> backward chain 전체를 추적하는 대신 **매 instance 마지막에 도착한 edge만** 반복적으로 확인해 따라간다.
> 근거: 이득을 만드는 것은 사슬 하나뿐이고(§1.3), 추적 대상을 줄이면 테이블·우선권 자원이 같이 준다.
> 측정된 절감(같은 조건의 full slice 대비, 옛 머신): 상주 멤버 PC **−63%**, priority 후보 **−26%**, Target Load **−29%**.
> 대가는 IPC −0.7%p 수준이다(§9.6).

> **Contribution 2 — 기존 OoO 구조만으로 resolution을 앞당긴다.**
> 별도 thread·별도 fetch·별도 실행 없이, (a) issue queue의 **우선 스케줄링**과 (b) **register file prefetch**
> 두 가지 기존 구조로 §1.2의 두 지연 원인을 직접 공격한다. 추가로 실행되는 명령어는 **0개**다.
> 그리고 그 우선권을 위한 예약마저 **필요 없는 동안에는 반납한다**(§5.7).

---

## 4. 기법 개요

```
         ┌──────────── 학습 (commit, on-path만) ─────────────┐
         │                                                   │
  HBT ──>│ H2P 분기가 commit → brslice_tab의 root(depth 0)    │
 (A1)    │        ↑                                          │
         │ 멤버가 commit → 자기 LPR(마지막에 도착한 source)의 │
         │        producer를 한 단계 위로 멤버 등록 (A2·A3)   │
         │ 멤버 load면 Prefetch Table에 stride 학습 (A4)      │
         └───────────────────────────────────────────────────┘
                              │ brslice_tab (1K entry)
                              ▼
  fetch/decode: PC로 조회 → priority bit 부착
        │
        ├─(A5) dispatch: RS의 priority 구획에 배치 → select에서 먼저 issue
        │        └ 구획은 priority op이 파이프라인에 없으면 normal에 개방 (A7)
        └─(A6) rename 직후: 멤버 load면 주소를 예측해 데이터를 destination
                 physical register로 미리 가져옴 (validate-then-use)
```

학습은 **commit 시점에만** 일어난다 — on-path 명령어만 테이블을 건드리므로 wrong-path 오염이 구조적으로 불가능하다. 소비는 **frontend/rename 시점**에 PC 조회 한 번으로 끝난다.

---

## 5. 상세 설계

### 5.1 A1 — H2P 판별 (HBT)

| | |
|---|---|
| 구조 | **HBT** 256 entry, **8-way set-associative**, 3-bit saturating counter (TEA 논문 Table II / §IV-B 사양, 0.2KB) |
| 갱신 | retire 시 **오예측일 때만** — entry가 없으면 counter 1로 할당, 있으면 +1(포화 7). 맞히면 아무 일도 없음 |
| decay | 50K retire instruction마다 전 counter −1. **counter 0인 way가 교체 1순위** |
| 판정 | **counter ≥ 1 = 추적 대상**(= entry가 있다), **counter > 1 = H2P = 가속 대상** ← 임계 둘 |
| 쓰임 | commit하는 분기가 추적 대상이면 brslice_tab의 **root(depth 0)** 로 삽입·재확인 / 소비 시점에 owner의 H2P 여부 판정 |

**chain seed는 `counter ≥ 1`(= HBT에 entry가 있는 분기)이 확정 설계다**(`zereco_critpath_insert_gate 1`, 2026-09-23). 가속은 그보다 높은 임계(counter > 1)로 따로 잠근다 — §5.9. 모든 분기를 seed로 넣으면(gate 0) 1K 테이블이 무너져 오히려 잃는다(§9.8).

### 5.2 A2 — critical producer 찾기 (LPR)

**LPR(Last Producer Register)** = 어떤 명령어를 ready로 만든 **마지막 source**의 physical register. 이것이 본 연구 고유 부분이다.

```
rename   : destination preg 할당 → PRF scoreboard[preg] = 내 PC
wakeup   : source마다 producer tag가 broadcast → M bit set → DELAY shift → R bit
           R이 가장 마지막에 선 source의 preg를 RSE의 LPR 필드에 래치
issue    : LPR을 ROB entry(LPR tracker)로 고정
commit   : LPR → PRF scoreboard 역참조 → producer PC
```

PUBS와의 차이 셋:

1. **logical → physical register.** rename이 WAW를 이미 해소했으므로 LPR은 언제나 *실제 dynamic producer*를 가리킨다.
2. **decode-time → commit-time 학습.** on-path만 학습하므로 wrong-path 오염이 없다.
3. **모든 source → last-ready source만.** 이것이 critical path 필터 자체다.

### 5.3 A3 — critical chain 멤버십 (brslice_tab)

| | |
|---|---|
| 구조 | **brslice_tab** 1K entry (128 set × 8-way LRU), **멤버 전용 할당** |
| entry | `{valid, tag, in_slice, depth, confirm(1b), 실행 카운터(6b), 입력 자리별 카운터 4 × 6b}` + 통계용 필드 |
| 전파 | commit 때 멤버 하나가 자기 LPR producer를 **한 단계** 위로 멤버 등록(dynamic instance당 1 level, PUBS와 같은 cadence) |
| 필터 A | depth ≥ 2 멤버는 **이번에 마지막으로 도착한 입력이 최근 실행의 50% 이상에서, 그리고 최소 4번** 마지막으로 도착했을 때만 그 producer로 전파한다. 실행 수가 64에 닿으면 모든 카운터를 절반으로 줄인다. depth 0·1은 면제 (2026-10-01 확정) |
| refresh | 1-bit confirm: 지목될 때 set, **10K commit마다 sweep**해 clear. 이미 clear인 멤버는 탈퇴(수명 10K~20K) |

**필터 A — 무엇을 하나.** 소비자(이미 멤버인 명령어)는 입력 자리마다 "최근 실행 중 몇 번 마지막으로 도착했나"를 센다. commit 때
마지막으로 도착한 입력의 카운터와 실행 카운터를 1씩 올린다. 실행 카운터가 64에 닿으면 모두 절반으로 줄여, 비율은 그대로 두고 오래된
실행의 무게만 줄인다(창 = 그 멤버의 최근 32~63번 실행). 전파 직전에 **그 입력의 카운터 ≥ 4이고 카운터 × 2 ≥ 실행 수**이면 이번
instance의 실제 producer로 전파(등록 또는 재확인)한다. 그러니 필터 A는 **멤버로 넣을지와 멤버로 남길지를 함께 정하고**, 빼는 일은 refresh가 한다.

**필터 A — 왜 이 규칙인가.**
- **연속이 아니라 비율이어야 한다.** 이전 필터 A(같은 producer 4번 연속)는 다른 입력이 한 번만 끼어들어도 처음부터 다시 센다.
  그래서 주된 입력마저 막힌다 — S S S F가 반복되면 둘 다 영원히 막힌다. 멤버 소비자의 마지막 도착 producer는 직전 실행과 19.9%나
  바뀌고(baseline과 가속 run이 같다 — 가속이 만든 현상이 아니다), 이전 필터 A는 판정의 27.3%를 막았다. 비율 방식은 3.3~4.0%만 막는다.
- **X = 50%: 다수결.** 50% 이상인 입력은 한 소비자에 많아야 하나다(정확히 반반일 때만 둘). 즉 형제를 둘 다 따라가지 않고도 —
  끼어드는 형제 때문에 주된 입력까지 막던 문제만 고쳐서 — full slice와 같은 IPC를 낸다(§9.9). X를 낮추면 가끔 마지막인 입력까지
  받아 자원만 늘고, 올리면 주된 입력이 가끔 끊길 때 막혀 IPC만 잃는다. 주된 입력에 다른 입력이 끼어드는 비율별로, 주된 입력이
  전파되는 몫(실제 필터 코드를 그대로 잘라 만든 단위 테스트, 무작위 끼어들기):

  | 끼어드는 비율 | 0% | 5% | 10% | 20% | 30% | 40% |
  |---|---|---|---|---|---|---|
  | 이전 필터 A (연속 4회) | 100 | 86.6 | 73.0 | 51.2 | 34.2 | 22.7 |
  | **X 50%** | 100 | 100 | 100 | 100 | 100 | 97.0 |
  | X 62.5% | 100 | 100 | 100 | 99.9 | 91.0 | 34.2 |
  | X 75% | 100 | 100 | 100 | 86.1 | 13.5 | 0.1 |
  | X 87.5% | 100 | 98.7 | 71.7 | 2.7 | 0 | 0 |

  X 100%는 쓸 수 없다 — 카운터를 정수로 절반씩 줄이므로 한 번 끊긴 부족분 1이 영원히 남는다.
- **N = 4: 표본이 적을 때의 보호.** 첫 관찰은 1/1 = 100%라 비율만 보면 초반에는 무조건 통과한다. 최소 4번을 요구하면
  늘 같은 입력이 늦는 edge는 이전 필터 A와 같이 4번째에 통과하고, 10%만 늦는 입력은 4번 쌓일 즈음(약 40번 실행) 비율이 이미 낮아 막힌다.
  창 64에서는 warm-up 뒤 실행 수가 32~63이라 N은 정상 상태에서는 거의 작동하지 않는다(N 2/4/8의 IPC 차 ±0.04%p).
- **창은 시간이 아니라 그 멤버의 실행 횟수다.** 시간(commit 수) 창에서 "N번"을 세면 자주 도는 소비자는 가끔 늦는 입력도 통과시키고(사실상
  필터 꺼짐), 드물게 도는 소비자는 늘 늦는 입력도 영원히 못 통과시킨다. 실행 횟수 창이면 모든 소비자에 같은 기준이 걸리고, phase가
  바뀌어도 절반 줄이기가 옛 기록을 지우므로 별도 refresh가 필요 없다. 옛 비율이 남아 있어도 판정은 이번에 실제로 마지막 도착한 입력에만
  적용되므로 잘못 넣지 않는다. 대가로 phase가 바뀌어 다른 입력이 늘 늦게 되면, 그 입력은 X 50%에서 32~33번 실행 뒤에 통과한다(이전 필터 A는 4번) —
  반복성을 확인하는 필터의 목적에 맞는 지연이다.
- **하드웨어.** 입력 자리 카운터는 LPR을 래치할 때 이미 정해지는 입력 자리 번호(2 bit, ROB에 실림)로 고른다. 판정은 `카운터 × 2 ≥ 실행 수`
  (1-bit shift + 비교기 하나)이고 나눗셈이 없다. commit 경로에서 entry 하나를 읽고 쓰는 일이라 timing에 영향이 없고, 이전 필터 A의
  producer PC 태그가 필요 없어진다. 입력 자리는 4칸이면 충분했다(다섯 번째 이후 자리가 마지막 도착인 경우 0건).

**필터 A에서 depth ≤ 1을 면제하는 이유** — x86 조건 분기는 flags(Scarab에서는 ZPS 레지스터 하나)만 읽으므로 `branch → cmp` edge는 거의 바뀌지 않는다. 흔들리는 곳은 그 위, **비교 명령의 두 피연산자 중 누가 늦게 오느냐**다(이전 필터 A 측정: 막는 전파의 54.3%가 depth 1, depth 0은 0.8%). 면제 없이 필터를 걸면 chain이 아예 시작되지 않는 workload가 생긴다.

**refresh가 하는 일** — 멤버는 consumer가 자기를 LPR로 지목할 때마다 재확인된다. full 규칙은 모든 producer를 매번 재확인하지만 critical 규칙은 **실제로 마지막에 도착한 producer만** 재확인한다. 수명을 짧게 두면 "한 번 LPR이었던" PC는 떨어지고 "계속 LPR인" PC만 남는다 — 멤버 조건이 *누적(합집합)*이 아니라 **반복성**이 된다. refresh가 없으면 시간이 지나면서 critical 규칙이 full 규칙으로 수렴한다.

### 5.4 A4 — Target Load 판정 (Prefetch Table)

chain 위의 load 중 **주소가 규칙적인 것**만 RF prefetch 대상(Target Load)으로 삼는다.

| | |
|---|---|
| 구조 | **PT** 1K entry, 8-way, load PC별 stride predictor + 1-bit confidence(증가 확률 1/16) |
| 할당 | commit 때 **chain 멤버 load만** 할당·갱신 |
| 조회 | rename 시점 (예측 주소 생성) |

### 5.5 A5 — priority scheduling (P-IQ)

| | |
|---|---|
| 구조 | 각 RS의 일정 비율을 **priority 구획**으로 예약 (기준 **15% = 29/186**, RS별 15/11/3) |
| 우선권의 실체 | **구획에 들어간 op만** select에서 우선권을 갖는다(`zereco_iq_priority_bit`) |
| 부착 시점 | decoupled frontend가 FTQ에서 op을 꺼낼 때(rename 이전) brslice_tab을 PC로 조회 |
| 자리 없음 | **non-stall fallback**(기본): normal entry로 들어가고 **priority bit를 잃는다**. stall policy(0)도 선택 가능 |

중요한 점: 구획은 **물리적 분할이 아니라 dispatch admission limit + 클래스별 점유 카운터**다. 한도를 바꿔도 이미 들어간 op을 쫓아내지 않고 신규 진입만 막는다. 그래서 한도를 동적으로 바꾸는 비용이 0이다(§5.7). RS entry는 retire가 아니라 **select 시점**(`OS_SCHEDULED`/`OS_MISS`)에 반납된다.

### 5.6 A6 — RF prefetch (RFP)

| | |
|---|---|
| 발사 | rename 직후, Target Load의 예측 주소로 prefetch 패킷 생성 |
| 경로 | in-flight store scan(forwarding) → 여분 포트로 L1 probe → 데이터를 load의 **destination physical register**에 직접 기록 |
| 검증 | **validate-then-use**: AGU가 실제 주소를 낸 뒤 주소 일치·older store·stale 검사를 통과해야 사용 |
| store forwarding | **모드 1** — 예측 주소가 forwarding store의 주소와 맞으면 store 완료 +1 cycle에 RF로 전달 |
| L1 miss | 하위 계층 fill을 진행한다 (fill path가 RFP 이득의 지배 성분) |

### 5.7 A7 — 예약의 동적 해제 (2026-09-19 교수님 지시)

**추적 카운터 하나**로 "지금 priority op이 파이프라인에 있는가"를 안다.

```
decode      : critical-slice uop이 디코드되면 +1      (decode_stage_process_op)
RS 이탈     : 그 op이 select되면 −1                   (node_issue_queue, ready list clear)
소멸        : flush 등으로 죽으면 −1                  (free_op — 모든 op이 지나는 단일 지점)
매 cycle    : 카운트 0이 dwell cycle 연속되면 → 예약 해제(구획을 normal에 개방)
              1 이상이 되면 → 즉시 재장악
```

- **왜 decode인가.** x86 uop은 자기 주소가 없고 priority bit은 (instruction PC, uop index)의 성질이므로, 기계가 그것을 알 수 있는 **가장 이른 지점이 decode**다. 그리고 decode는 dispatch보다 충분히 앞서 있어, 해제 중 normal op이 잡고 있던 자리가 비워질 시간이 있다.
- **dwell(유예)은 두지 않는다 — 첫 빈 cycle에 바로 해제한다**(`zereco_piq_release_dwell 0`, 2026-09-22 확정). 설계 당시에는 "구획에 들어온 normal op이 빠져나가는 데 약 12 cycle이 걸리고 그 사이 도착한 priority op이 fallback해서 우선권을 잃는다"를 유예의 근거로 삼았지만, **측정으로 반증됐다**(§9.7): 해제는 **normal 클래스의 admission 한도만** 올리고 priority 클래스의 한도는 그대로 두므로, 해제 중에 도착한 priority op도 물리 RS가 꽉 차지 않는 한 자기 자리를 받는다. dwell 0과 16의 fallback 비율은 소수 둘째 자리까지 같고(25%에서 12.49%) IPC 차이는 ≤0.08%p다. **해제의 비용이 0이므로 유예 카운터도 필요 없다.**
- **재장악은 언제나 즉시**다. 해제·회수 모두 그 cycle의 카운터 값만 본다.
- **물리 free-entry guard가 필수**다. 예약을 풀면 normal이 RS를 전부 채울 수 있어, `node_issue_queue_allocate_rs_entry()`가 빈 free list에서 ASSERT로 죽는다. 한도 검사와 별개로 `rs->free_entry_count > 0`을 항상 본다.
- 무결성 불변식도 동적 한도 기준으로 바뀐다 — **정의는 `node_piq_partition_mismatch()` 한 곳에만 있다**(§7.3).

### 5.8 설계상 알려진 성질

- **walk가 저절로 끝난다.** dispatch 시점에 모든 source가 이미 ready였던 명령어는 LPR 이벤트가 없어 전파하지 않는다 — operand 대기가 실재하는 경계(frontier)에서 멈춘다.
- **register edge만 따라간다.** PRF scoreboard로 볼 수 있는 것이 그것뿐이다. store → load 의존을 포함하면 멤버 +4%p, IPC +0.3%p이지만 LPR 필드가 SQ entry를 가리키는 확장이 필요해 채택하지 않았다(`zereco_critpath_mem_edge 0`).
- **chain의 크기**(옛 측정, register edge, 1K 테이블): H2P 한 번 commit당 멤버 약 8.3개, 평균 depth 약 4.7. 단계당 1.8개라 대부분 선형이고 가끔 갈라진다 — 커밋 op의 67.9%가 waking source 하나뿐이라는 사실과 정합한다. suite 편차가 크다(GAP 6개/3.2단계 ↔ DC 13개/6.3단계).
- **필터링 상한은 프로그램이 정한다.** producer가 하나뿐이면 critical 규칙과 full 규칙이 같은 곳으로 전파하므로 두 규칙이 원리적으로 구분되지 않는다. 갈릴 수 있는 모집단은 경쟁 도착이 있는 멤버 op(약 18%)뿐이고, 나머지 필터링은 refresh와 필터 A의 **반복성 요구**에서 나온다.
- **uop granularity (2026-09-20 결정).** x86에서 uop은 주소가 없으므로 uop 단위 테이블의 키는 (instruction PC, uop index)다. **논문에는 PC + uop index로 접근한다고 쓰고**(그림 `figures/fig7_brslice_tab_uop.*`), 잘 도는 시뮬레이터 구현(macro-op PC 단위, 형제 uop 전부 승격)은 바꾸지 않는다. HBT와 RFP PT는 multi-uop 모호성이 사실상 없어 PC 색인 그대로 둔다.

### 5.9 A8 — 소비 게이트: 추적과 가속의 분리 (2026-09-23 확정)

하나의 멤버십 비트가 **"추적 중"과 "가속 대상"을 겸하면**, 추적 범위를 넓히는 순간 가속 범위도 같이 넓어진다.
소비 게이트는 그 둘을 떼어 놓는다 — 그리고 **HBT counter 하나에 임계 둘**이면 구조가 하나도 늘지 않는다.

```
  T_track = counter ≥ 1   (= HBT에 entry가 있다)   → brslice_tab에 chain을 만든다
  T_accel = counter > 1   (= H2P)                  → 그 chain을 가속한다

  fetch : brslice_tab[PC] → in_slice + owner ──▶ HBT[owner] counter > 1 ?
                                                    예 → priority bit / 아니오 → 일반 op
```

**게이트는 "자원을 쓰는 지점"에만 건다.** 준비는 넓게, 소비는 좁게 하는 것이 요점이다.

| | 넓게 (T_track을 따름) | 좁게 (owner가 지금 H2P일 때만) |
|---|---|---|
| brslice_tab 멤버 등록·전파 | ✓ | |
| Prefetch Table의 stride 학습 | ✓ | |
| RS의 priority 자리 차지 | | ✓ (frontend 조회) |
| L1 포트를 쓰는 prefetch 발사 | | ✓ (`rfp_rename_launch`) |

왼쪽 둘은 **준비**라 실행 자원을 쓰지 않는다(테이블 칸만 차지). 오른쪽 둘은 RS 구획·캐시 포트를 실제로 쓴다.
설계 초기에 RFP 게이트를 **Target Load 지명(= PT 학습)**에 걸었다가 되돌렸다 — 그러면 승격 순간 chain은 준비돼
있는데 주소 예측기가 비어 있어 stride를 처음부터 배우고, 강등 뒤에도 PT entry가 남아 prefetch가 한동안 계속된다.
양쪽 다 어정쩡해진다. 측정으로도 이 선택이 이득의 원천이었다(§9.8: Target Load hit 40.5 → 46.7%).

**owner 보호 규칙.** entry당 owner가 한 칸이라 한 PC가 두 chain에 걸리면 마지막 지목자가 owner가 된다. owner가
가속 여부를 정하게 된 이상, **H2P owner를 비-H2P 후보가 빼앗지 못하게** 막는다(`zereco_critpath_owner_protect`).
그 owner가 나중에 강등되면 다음 후보가 자연스럽게 인수한다(자기 치유). PUBS는 `D_c`가 한 칸이고 이 보호가 없다.

**부수 효과**: 게이트가 없으면 강등된 분기의 slice가 refresh가 벗겨낼 때까지 계속 가속된다. 게이트는 **강등 즉시
중단, 승격 즉시 재개**로 만든다. 측정상 그 잔여 가속은 성능에 기여하지 않았다 — 가속 대상을 3.1% 줄여도 IPC가
그대로였다(§9.8).

---

## 6. 확정된 설계 결정

critical-path 관련 결정은 2026-09-14부터 **코드 기본값**(`core.param.def`)이다 — `zereco_critpath_profile`·`zereco_critpath_priority`·`rfp_target_critpath`만 켜면 아래 설정으로 돈다.
필터 A(2026-10-01 확정)도 같은 날 코드 기본값이 됐다(`zereco_critpath_edge_ratio 50`, `_min_count 4`, `_window 64`).
이전 필터 A로 돌리려면 `--zereco_critpath_edge_ratio 0`을 준다. 그 전에 만든 config 중 이 플래그가 없는 CLEAR config는 이제 새 필터 A로 돈다.

| 항목 | 결정 | 한 줄 근거 |
|---|---|---|
| HBT | **256 entry, 8-way** — 오예측 때만 counter 1로 할당, 50K retire마다 −1, counter 0이 교체 1순위 | TEA 논문 Table II / §IV-B 사양. 교체 전(1024 direct-mapped) 대비 IPC 중립(+0.09%p)이고, 진짜 H2P의 11.8%를 충돌로 놓치지만 그들의 실행량은 0.76%뿐이다(§9.8) |
| **chain seed (T_track)** | **counter ≥ 1** (`insert_gate 1`) = HBT에 entry가 있는 분기 | §9.8: gate 2 대비 +0.23%p. 정적 분기의 75%는 한 번도 틀리지 않아 추적할 가치가 없고, 전부 넣으면(gate 0) 1K 테이블이 무너져 −0.17%p |
| **소비 게이트 (T_accel)** | **counter > 1**, 자원을 쓰는 두 지점에만 (`consume_h2p_only 1`) | §5.9. 같은 counter의 두 번째 임계라 구조가 늘지 않는다 |
| owner 보호 | **켬** (`owner_protect 1`) | H2P owner를 비-H2P 후보가 빼앗지 못하게. gate 1에서 1.17/1K op 발동 |
| slice 규칙 | **critical (last-ready edge)** | Contribution 1 |
| edge 집합 | **register edge만** | PRF scoreboard로 볼 수 있는 범위와 일치 |
| 학습 시점 | **commit (on-path)** | wrong-path 오염 구조적 차단 |
| brslice_tab | **1K entry, 128 × 8-way, 멤버 전용 할당** | §9.8(f): 확정 설계(gate 1)에서 **512~4K 사이 IPC 차 0.04%p**. 2K에서 축출이 0이 되어도 이득이 없고 512에서도 −0.04%p뿐이다. PUBS와 같은 예산이고, 전체 PC 할당은 테이블이 chain이 아니라 프로그램 크기를 따라간다 |
| refresh | **confirm 1 bit, 10K commit sweep** (수명 10~20K) | 효율을 올린 유일한 knob(가속 대상 −6.3%p, IPC −0.06%p). 수명이 길면 critical이 full로 수렴한다 |
| 반복성 필터 | **필터 A: 비율 방식 X 50%, N 4, 창 64 + depth ≤ 1 면제** (2026-10-01 사용자 확정). 이전 필터 A(연속 4회)·B·C·hysteresis 불채택 | §5.3·§9.9: 이전 필터 A 대비 +0.60%p로 full slice와 동률(+0.06%p)이면서 가속 후보는 full의 80%, 상주 멤버 63%, brslice_tab 멤버 축출 1/6. X를 올리면 IPC만 잃고 내리면 자원만 는다. N은 무관 |
| depth 제한 | **없음** | 인구와 이득이 같은 비율로 준다(효율 불변) |
| chain 제거 | **refresh만** — root가 H2P에서 강등돼도 chain을 일괄 삭제하지 않는다 | 경계선 분기가 강등·복귀를 반복하면 chain을 매번 다시 쌓게 된다. refresh의 층별 연쇄 탈퇴가 유예 기간 역할을 한다 |
| Prefetch Table | **1K entry, 8-way, 1-bit confidence(p=1/16)** | 512~∞에서 0.1%p 이내. 병목은 주소 예측 가능성(저신뢰 43%) |
| RFP store forwarding | **모드 1** | 옛 보수 모델이 버리던 Target Load 19%의 절반 회수 |
| RFP L1 miss | **하위 계층 fill 진행** | fill path가 RFP 이득의 지배 성분 |
| P-IQ dispatch policy | **non-stall fallback** | §9.7: 예약 10~40% 전 구간에서 stall보다 낫고, 최고끼리도 1.7%p 앞선다 |
| P-IQ 구획 비율 | **15% (29/186 = 15/11/3)** (2026-09-22 사용자 결정) | §9.7: 작을수록 좋다는 단조 추세가 두 dwell에서 같게 나왔다(10% 1.1011~1.1013, 15% 1.1003, 40% 1.0871). 10% 미만은 미측정이며 0%에서는 P-IQ가 사라지므로 어딘가에 꼭짓점이 있다 |
| P-IQ 예약 해제 | **동적 해제, 유예 없음 (dwell 0)** | §5.7·§9.7: dwell 0/16이 성능 동일(≤0.08%p, 움직임은 전부 xgboost). 해제 비용이 0이므로 유예 카운터를 없앴다 |
| scheduler | **oldest first** (`node_issue_queue_schedule_scheme 0`) | 표준 config 기준. P-IQ의 select 우선권은 그 위에 얹힌다 |
| wrong-path priority | **끔 — oracle** (`zereco_critpath_priority_offpath 0`) | 평가 모드. 하드웨어 동작(off-path 켬)은 IPC −0.85%p였고 민감도로 제시한다 |
| filtering 지표 | **commit 기준** (priority bit를 달고 commit된 op) | oracle에서는 dispatch 기준과 같은 값 |
| 옛 Fill-Buffer walk | **완전 배제** (`LEGACY_WALK_NEEDED()`) | Target Load 지명을 오염시켰다 |

---

## 7. 구현 — 코드 지도

### 7.1 파일과 훅 지점

| 요소 | 상태 | 코드 |
|---|---|---|
| **A1** HBT | **재작성** | `bp/hbt.c` — 256 entry 8-way(`hbt_entries`/`hbt_assoc`), 오예측 때만 할당(counter 1), counter 0 우선 교체, decay 50K. `hbt_lookup_state()`가 miss/cold/hard 3분류를 준다 |
| **A2** LPR 관측 | 구현 | `cmp_model.c:384` `critpath_note_wake()` — wakeup의 max에 argmax를 얹는다. producer PC는 wake 시점에 포착(소비자 commit 전에는 preg가 재할당되지 않으므로 scoreboard 역참조와 등가) |
| **A3** brslice_tab | 구현 | `zereco/critpath.c` — commit 훅 `critpath_note_retire()`에서 seed → 필터 A(카운터 `critpath_ratio_observe`, 판정 `critpath_ratio_passes`, 방식 선택 `critpath_edge_filter`) → 전파(`critpath_propagate_to`) → refresh(`critpath_decay_table`). 이전 필터 A(`critpath_edge_observe/_passes`)는 `zereco_critpath_edge_ratio 0`일 때 쓰이고, 비율 방식일 때도 같은 instance의 판정을 계산해 둘이 갈린 횟수를 센다 |
| **A4** Target Load → PT | 구현 | `critpath_note_retire()` → `rfp_note_target_load()` (멤버 load만 PT 할당/refresh). **소비 게이트를 걸지 않는다** — 학습은 추적만큼 넓어야 한다(§5.9) |
| **A5** P-IQ | 재사용 + 입력 교체 | 부착 `decoupled_frontend.cc:608` `apply_main_chain_block_tag()` → `critpath_is_member()` / 구획·fallback·select `node_issue_queue.cc` / 구획 크기 `exec_ports.c` |
| **A6** RFP | 재사용 + store forwarding | `zereco/rfp.c` |
| **A7** 예약 동적화 | 구현 | 카운터 `zereco/critpath.c`의 `zereco_piq_inflight_*` / 태그 `decode_stage.c:242` / 이탈 `node_issue_queue.cc:443` / 소멸 `op_pool.c:168`(`free_op`) / 샘플 `node_issue_queue.cc:726` / 판정 `zereco_piq_reservation_engaged()` → `node_dispatch_find_emptiest_rs_for_piq_class()` |
| H2P resolution profiler | 재사용 | `zereco/h2p_mispred_latency.c` (`zereco_h2p_mispred_latency_profile 1`) |
| **full-slice 비교 모드** | 구현 | `zereco_critpath_full_slice 1`: 전파만 다르다 — 모든 register source의 producer로 전파, frontier 종료·필터 A 없음. 같은 실행에서 shadow critical table이 멤버/priority op/Target Load를 critical·non-critical로 분류한다 |

**훅 지점에 얽힌 함정 두 개** (실제로 run을 날린 사례):

1. `apply_main_chain_block_tag()` **내부**에 in-flight 태그를 넣으면 critpath 경로의 early return 때문에 절대 실행되지 않는다 → 태그는 **호출 지점**(지금은 `decode_stage_process_op()`)에 둔다.
2. 카운터 반납은 `free_op()` 한 곳에 두면 flush 별도 처리가 필요 없다 — 모든 op이 지나는 단일 소멸 지점이기 때문이다.

### 7.2 파라미터

```
켜는 스위치   zereco_critpath_profile (chain 구축), zereco_critpath_priority (A5 입력),
              rfp_target_critpath (A4 입력), zereco_piq_enable, rfp_enable    — 모두 기본 꺼짐
기준 설정     _mem_edge 0, _table_sets/_assoc 128/8, _member_only_alloc 1, _confirm_bits 1,
              _decay_interval 10000, _edge_conf_exempt_depth 1,
              _insert_gate 2(H2P), _priority_max_depth 0(무제한), _priority_offpath 0(oracle)
필터 A        zereco_critpath_edge_ratio 50, _edge_ratio_min_count 4, _edge_ratio_window 64
              — 2026-10-01 확정, 코드 기본값. _edge_ratio 0이면 이전 필터 A
P-IQ          zereco_piq_entry_percent 25, _dispatch_policy 1(non-stall),
              _reservation_dynamic 1, _release_dwell 16
RFP           rfp_pt_entries 1024, _store_forward 1, _l1_miss_policy 1
이전 필터 A    _edge_conf_min 3 — _edge_ratio 0일 때만 판정에 쓰인다(그 밖에는 비교 통계만)
비교·연구용   _full_slice, _edge_conf_decay(hysteresis), _ratio_min(B), _slack_min(C),
              _timeline_interval                                            — 모두 기본 꺼짐
```

### 7.3 무결성 검사

- **P-IQ 구획 불변식은 `node_stage.c:149` `node_piq_partition_mismatch()` 한 곳에만 있다.** 동적 모드에서 normal의 상한은 `zereco_normal_rs_limit`이 아니라 `main_rs_limit`이다.
  (이 불변식이 `node_stage.c`와 `node_issue_queue.cc` 두 곳에 각각 적혀 있어, 동적화 때 한쪽만 고쳐 143 run이 전멸한 적이 있다.)
- **P-IQ class 용량은 `node_piq_class_room()` 한 곳에서만 정의한다**(2026-09-26). dispatch 탐색과 non-stall 검사가
  각자 계산하던 시절, 동적 예약에서 해제 → 재장악 사이에 normal op이 자기 한도를 넘어 남아 있는 상태(축출하지 않으므로)를
  검사 쪽이 고려하지 않아 `limit − count`가 unsigned로 wrap(≈ 4.3×10⁹)했다. non-stall에서는 거짓 assert로
  `260925_clear_precision_full`의 2 run(blender 63365, tc 30174)이 죽었고, stall policy에서는 assert 없이
  `ZERECO_PIQ_UNUSED_NORMAL_SLOTS_DURING_PRIORITY_STALL`·`_PRIORITY_STALL_WITH_UNUSED_NORMAL_CYCLES`만 오염됐다
  (`260921_CLEAR_rs_sweep`의 stall 10/15/20%, 5 SimPoint — 이 두 통계는 보고에 쓰지 않았다). **dispatch 결정 자체는
  처음부터 옳았다** — 틀린 것은 검사뿐이었고, 리팩터는 dispatch를 바꾸지 않는다. 같은 규칙을 두 곳에 적어서 생긴
  사고가 이번이 두 번째다(첫 번째: `node_piq_partition_mismatch()`로 합친 불변식).
- 통계 계수기 `ZERECO_PIQ_PARTITION_INTEGRITY_MISMATCHES`, `ZERECO_IQ_*_INTEGRITY_MISMATCHES`, `RFP_DEMAND_DELAYED_BY_PREFETCH_OPS`는 모든 확정 run에서 0이다.
- `RFP_INFLIGHT_UNDERFLOW`만 injected의 0.02% 수준(PT 축출 후 같은 PC 재할당 시 0 카운터 감소, 무해).
- **소비자를 모두 끄면 cycle-identical**이다 — `zereco_critpath_profile`만 켠 run은 끈 run과 cycle까지 같다. 동적 예약도 `_reservation_dynamic 0`이면 정적 코드와 bit-identical이다(89/89 확인).

---

## 8. 평가 방법론

| | |
|---|---|
| **머신** | `PARAMS.golden_cove_original` — RS 97/70/19 = **186**, PRF 280 int / 332 vec, issue 6 / retire 8, ROB 512, LQ/SQ 192/114, dcache 3 read / 2 write × 1 bank, mem_req buffer 32, realistic return stack, FDIP on. BTB 12288(4-way → 유효 8192), MLC 유효 1 MiB, LLC 유효 2 MiB |
| **표본** | **89 SimPoint / 31 workload** — GAP 6, SPEC17 11, SPEC26 8, DC 6. descriptor `scarab-infra/json/clear.json`(TEA는 `json/tea.json`, 목록 동일) |
| **baseline** | `260917_zereco_n_tea/baseline_nopref` — 메커니즘 전부 끔, prefetcher 끔. **고정이며 다시 돌리지 않는다**(`results.py`의 `BASELINE_EXP`) |
| **구간** | **Periodic만** — warm-up 뒤 10M~20M 명령어. `*.stat.0.out`의 첫 숫자 열 |
| **집계** | workload 안에서 SimPoint weight 재정규화 → 가중평균. suite·Average IPC는 **workload speedup의 geomean**, 비율 지표는 workload 간 산술평균, 이벤트 비율은 가중합의 비 |
| **검증** | 모든 run이 "Scarab finished at" + 20M 도달, config당 Scarab 리비전 하나, PARAMS.out이 머신·config flag와 일치, descriptor의 SimPoint 집합과 정확히 일치 (`results.py`) |
| **평가 모드** | wrong-path op에는 priority를 주지 않는다(oracle). 시뮬레이터에서만 가능한 결과는 상한(limit study)으로 표기 |

**표준 config**

| 이름 | 무엇 |
|---|---|
| **`260923_clear_gate1_consume`** | **CLEAR 표준 (2026-09-23~)** — P-IQ 15% non-stall + RFP, critical slice, oldest-first, 예약 동적 해제 dwell 0, **HBT 256×8**, **seed gate 1 + 소비 게이트 + owner 보호**, prefetcher on. 결과는 `260923_CLEAR_consume_gate/260923_clear_gate1_consume`, **1.1033** |
| `260923_clear_base` | 같은 설정에 **게이트만 끈 것**(seed gate 2) = 직전 표준. `260923_CLEAR_hbt256`, 1.1012 |
| `clear_rs15_nonstall_dwell0` | 그 이전(옛 HBT 1024 direct-mapped). `/home/lee/simulations/CLEAR_baseline`, 1.1003 |
| `clear_rs25_nonstall` (= 구 `zereco_critical_oldest`) | 이전 표준(~2026-09-21) — 25% non-stall, dwell 16. 결과는 `260920_zereco_piq_dynamic/piq_dyn_dwell16`, 1.0972 |
| `TEA_preg102_vec16` | **TEA 기준** — RS 예약 해제 + 공유 PREG pool(102 int / 16 vec), oracle trigger, 복구 1/7, prefetcher on. 폴더 `260920_zereco_n_tea (최신)/` |
| `OoO_baseline` | 위 baseline의 복사본 |

---

## 9. 확정된 결과

모든 수치는 §8 조건(89 SimPoint, Periodic, baseline_nopref 대비 geomean)이다.
§9.2~§9.8은 필터 A 확정(2026-10-01) 전의 설정, 즉 이전 필터 A(연속 4회)로 잰 결과다.

### 9.1 최종 비교

| | GAP | SPEC17 | SPEC26 | DC | **Average** |
|---|---|---|---|---|---|
| **CLEAR** (필터 A, `261001_clear_std_rfp_pt_r50_n4`) | **1.1677** | 1.0982 | 1.0918 | **1.0961** | **1.1092** |
| full slice (`260930_full_tab1024` = `260926_clear_precision_full`) | 1.1677 | 1.0979 | 1.0890 | 1.0971 | 1.1086 |
| **TEA** (`TEA_preg102_vec16`) | 1.1452 | **1.1256** | **1.1021** | 1.0197 | 1.1020 |
| CLEAR, 이전 필터 A (`260923_clear_gate1_consume`, 직전 표준) | 1.1698 | 1.0899 | 1.0826 | 1.0911 | 1.1033 |

- **새 CLEAR baseline = 260923 표준 + 필터 A(X 50%, N 4, 창 64)**, 표준 크기 표(HBT 256×8, brslice_tab 1K, PT 1K), RF prefetch 발사 범위는 이전과 같다(`rfp_launch_member_only 0`). member-only를 켜면 1.1045다(채택 여부는 TODO N-7에서 결정 대기).
- **Average는 CLEAR가 TEA보다 0.72%p 앞선다**(1.1092 vs 1.1020). **GAP(+2.25%p)과 DC(+7.64%p)는 CLEAR가 크게 앞서고**, SPEC17(−2.74%p)·SPEC26(−1.03%p)은 TEA가 앞선다.
- **full slice와는 IPC가 같다**(+0.06%p, Cumulative도 +0.06, 워크로드 14:15). 비용 차이는 §9.9.
- **비용은 비대칭이다.** TEA는 같은 결과를 내기 위해 명령어를 평균 15.1% 더 fetch·실행하고 RS·PREG를 예약한다. CLEAR가 추가로 실행하는 명령어는 0개다.
- 그림·스크립트: 이전 표준 기준은 `260920_zereco_n_tea (최신)/analysis/{plot_suite_ipc_speedup.py, plot_workload_ipc_speedup.py}`. 새 baseline 분석은 `261001_CLEAR_edge_ratio_finite/analysis/`(§9.9)이고, 비교 모음은 `261001_COMPARSION/`이다.

### 9.2 메커니즘 분해 (사다리, `260917_zereco_n_tea`)

| config | GAP | SPEC17 | SPEC26 | DC | Average |
|---|---|---|---|---|---|
| P-IQ only, critical / full (random queue) | 1.0847 / 1.0845 | 1.0421 / 1.0434 | 1.0143 / 1.0216 | 1.0138 / 1.0154 | 1.0373 / 1.0400 |
| RFP only, critical / full (random queue) | 1.1439 / 1.1412 | 1.0518 / 1.0591 | 1.0403 / 1.0423 | 1.0706 / 1.0740 | 1.0697 / 1.0730 |
| **P-IQ + RFP, critical / full (oldest first)** | **1.1670** / 1.1647 | 1.0782 / 1.0876 | 1.0728 / 1.0853 | 1.0863 / 1.0903 | **1.0950** / 1.1021 |
| TEA (자원 예약 개선 전, `tea_oracle_pref_rc1_7`) | 1.0938 | 1.0411 | 1.0386 | 0.9803 | 1.0383 |

- **RFP가 주 성분이다** — 단독 1.0697(특히 GAP 1.1439, DC 1.0706). P-IQ 단독은 1.0373이고, 둘을 함께 켜면 1.0950이다.
- **critical과 full의 격차는 단독 메커니즘에서는 작다**(P-IQ −0.27%p, RFP −0.33%p). 둘을 합치면 −0.71%p로 벌어진다(§9.6).
- 마지막 행은 **자원 예약을 고치기 전의 TEA**다 — 같은 TEA 코드가 예약 정책만 바꿔 1.0383 → 1.1020이 됐다(§9.5).

### 9.3 H2P resolution latency — 어디를 줄였나

| (cycle, Average) | fetch → dispatch | operand 대기 | select + execute | 합계 |
|---|---|---|---|---|
| baseline | 30.89 | 59.69 | 2.24 | 92.83 |
| **CLEAR** | 28.10 | **48.02** | **1.21** | **77.32** |
| 상대 | 91.0% | **80.4%** | **53.8%** | **83.3%** |

- **설계 의도대로 맞았다**: 줄어든 15.5 cycle 중 11.7 cycle(75%)이 operand 대기다.
- select + execute가 절반이 된 것은 P-IQ가 분기 자신에게도 우선권을 주기 때문이지만, 절대량이 2.2 cycle이라 전체 기여는 작다 — §1.2의 관찰과 일치한다.
- fetch → dispatch도 2.8 cycle 줄었다. 이 구간은 가속 대상이 아니므로 2차 효과로 보이지만, 원인은 측정하지 않았다.
- 표준 config(`clear_rs15_nonstall_dwell0`) 기준이다. 이전 표준(25%·dwell 16)에서는 28.08 / 48.48 / 1.19 = 77.76 cycle로 거의 같았다.

### 9.4 예약의 동적 해제 (`260920_zereco_piq_dynamic`, 4 config × 89, 전부 정상 완료)

| | GAP | SPEC17 | SPEC26 | DC | Average |
|---|---|---|---|---|---|
| 정적 예약 (기존) | 1.1670 | 1.0782 | 1.0728 | 1.0863 | 1.0950 |
| 해제, dwell 0 | 1.1683 | 1.0810 | 1.0744 | 1.0849 | 1.0964 |
| 해제, dwell 16 | 1.1682 | 1.0810 | 1.0744 | 1.0891 | 1.0972 |
| 해제, dwell 64 | 1.1683 | 1.0806 | 1.0742 | 1.0848 | 1.0962 |

(이 표의 dwell 차이는 이후 xgboost 잡음으로 밝혀졌다 — §9.7. 확정된 것은 **해제 자체**이고, 유예는 두지 않는다.)

- **효과는 +0.22%p로 작다.** 그리고 그 작음 자체가 결과다 — **CLEAR의 정적 예약은 애초에 병목이 아니었다.**
  47칸을 cycle의 25~35% 동안 열어 줘도 normal 점유는 139칸 중 47.2 → 48.6(+1.4칸)이다. normal 구획은 평균 34%만 차 있고, 실제 병목은 메모리 대기(ROB head D-cache 23.7% + LLC 13.3%)다.
- 남은 dispatch 정지도 총량 부족이 아니라 **RS 편중**이다: RS0 full 17.7% / RS2 full 12.5%인데 RS1은 0.31%, 그리고 RS2의 priority 예약 5칸은 점유 0.03칸으로 사실상 전량 낭비다.
- 부작용 없음: fallback 8.61% 불변, priority dispatch 정지 0.93% 불변.
- 검증: 회귀 config가 이전 실험과 89/89 cycle 동일, 무결성 계수기 0, 해제 cycle이 dwell에 따라 단조 감소(35.1 → 24.9 → 19.2%).
- **대비 — 같은 정책으로 TEA는 +1.96%p다.** TEA의 54% 예약은 실제로 main을 굶겼고, 우리의 25% 예약은 그렇지 않았다. 이것이 "기존 구조에 얹는다"는 주장의 직접 근거다.

### 9.5 TEA가 치르는 비용

**(a) 자원 예약** (`260918_TEA_rs_reservation_sweep`, `260918_TEA_optimization`, `260921_TEA_rs_sweep`)

| 단계 | Average |
|---|---|
| 정적 예약 RS 54% + chain별 PREG 4분할 (논문 비율) | 1.0383 |
| RS 예약을 inactive일 때 해제 | 1.0580 (**+1.96%p**) |
| + PREG를 공유 pool로 (GP 136, VEC 16) | 1.0573 (변화 없음) |
| + PREG 예약량 축소 119 → **102** | 1.0815 → **1.1020** |

**이득은 두 군데서만 나왔다**: RS 예약을 푸는 것(+1.96%p)과 PREG 예약량을 줄이는 것(+4.4%p). 구조를 공유 pool로 바꾼 것 자체는 IPC를 바꾸지 않았고(1.0580 → 1.0573), 예약량을 줄일 수 있게 만든 전제였다.

RS 예약 비율 sweep(PREG 102/16 고정, 측정값):

| RS 예약 | 54% (101) | 40% (74) | 30% (56) | 20% (37) | 10% (19) |
|---|---|---|---|---|---|
| Average | 1.1020 | 1.1048 | 1.1057 | **1.1062** | 1.1025 |
| GAP | 1.1452 | 1.1470 | 1.1475 | **1.1490** | 1.1323 |
| DC | 1.0197 | 1.0197 | 1.0209 | 1.0203 | 1.0188 |

측정 최적은 20%이고 10%에서 GAP이 무너진다. 20%와 40%의 차이는 0.14%p(승패 10:21, 중앙값 0.11%p)로 잡음 수준이다.
근거: active cycle의 **100%에서 동시 chain이 정확히 1개**(oracle trigger) — chain별 4분할은 3/4가 항상 죽어 있다. VEC 예약도 PREG stall 시 요구량이 0.05개라 사실상 죽은 자원이다.

**(b) 추가 명령어** — TEA thread가 fetch한 uop ÷ main correct-path uop (측정):

| GAP | SPEC17 | SPEC26 | DC | Average |
|---|---|---|---|---|
| 51.8% | 7.2% | 7.6% | 2.8% | **15.1%** |

**(c) 띄운 chain의 낭비** (`260915_TEA_golden_cove_original` 전수 sweep, 정적 예약 시절 설정): TEA load의 **96.3%는 main이 이미 채운 캐시 줄을 다시 읽고**, 띄운 chain의 **39.6%는 목표 H2P를 풀기 전에 끝나며**, early flush의 **22.7%는 main이 이미 복구한 뒤에 도착한다**. 표본과 설정이 (a)·(b)와 달라 같은 표에 넣지 않는다.

> ⚠️ `260920_zereco_n_tea (최신)/analysis/table_tea_instruction_share.*`와
> `260921_TEA_rs_sweep/analysis/workload_ipc_speedup_adjusted.*`는 사용자 요청으로 **수치를 가공한 학습용 산출물**이다
> (tc를 40.7로 교체 + SPEC17/26/DC에 +10%p / suite geomean을 목표값에 맞춰 상수배). **논문 사용 금지**이고, 위 표의
> 15.1%는 가공 전 측정값이다. 원본 run 디렉터리(`*.stat.0.out`)는 어느 경우에도 수정하지 않는다.

### 9.6 critical slice vs full slice

같은 조건(P-IQ + RFP, oldest first)에서 IPC는 **critical 1.0950 vs full 1.1021**로 full이 0.7%p 앞선다. 대신 추적 비용이 다르다(옛 머신 동일 조건 측정):

| | critical (A3-e1) | full slice |
|---|---|---|
| brslice_tab 상주 멤버 PC | 80 | 215 (**−63%**) |
| priority 후보 (commit op 대비) | 38.5% | 51.7% (**−26%**) |
| Target Load (commit op 대비) | 8.17% | 11.47% (**−29%**) |

- **격차의 대부분은 mcf 하나다.** 예산(구획 10~25%, 테이블 128~1K)을 어떻게 바꿔도 crit − full은 −0.34 ~ −0.62%p로 유지되고, 그중 절반 이상이 mcf에서 나온다. GAP에서는 모든 점에서 critical이 앞선다.
- 원인은 이전 필터 A였다: 이전 필터 A만 끄면 critical이 full과 같거나 약간 앞선다(+0.07%p). 그 대신 절감이 −63% → −23%로 줄어든다. 즉 당시 교환비는 "비용 −63% ↔ IPC −0.7%p"였다.
- **2026-10-01: 필터 A를 비율 방식으로 바꿔 이 교환비를 바꿨다(§9.9).** 표준 크기 표에서 IPC는 full과 같고(+0.06%p), 가속 후보 −20%, 실제 가속 −14%, 상주 멤버 −37%, brslice_tab 멤버 축출 1/6, PT 축출 1/2이다.

---

### 9.7 P-IQ 예약 비율 × dispatch policy (`260921_CLEAR_rs_sweep`, 14 config × 89, dwell 16)

| 예약 | 10% | 15% | 20% | 25% | 30% | 35% | 40% |
|---|---|---|---|---|---|---|---|
| **non-stall** | **1.1013** | 1.1003 | 1.0983 | 1.0972 | 1.0946 | 1.0917 | 1.0871 |
| stall | 1.0276 | 1.0601 | 1.0723 | 1.0786 | 1.0832 | **1.0844** | 1.0808 |
| fallback (non-stall) | 35.7% | 24.8% | 18.5% | 12.5% | 8.5% | 5.5% | 4.1% |
| priority dispatch 정지 cycle (stall) | 31.2% | 23.5% | 19.2% | 15.4% | 11.9% | 8.4% | 6.7% |
| normal dispatch 정지 cycle (non-stall) | 16.8% | 17.6% | 18.8% | 20.5% | 22.7% | 25.1% | 27.9% |

- **non-stall이 전 구간에서 우세**하고, 최고끼리 비교해도 1.1013 vs 1.0844로 1.7%p 차이다. stall 10%는 priority op이 dispatch에서 cycle의 31.2% 막혀 1.0276까지 무너진다.
- **non-stall에서는 예약이 작을수록 좋다 — 단조다**(40% → 10%로 +1.42%p). 10%에서 fallback이 35.7%, 즉 우선권 후보 3분의 1이 우선권을 잃는데도 최고 성능이다.
- **즉 이득을 만드는 것은 구획의 크기가 아니다.** 예약을 키우면 priority 점유는 4.0 → 8.6칸으로 조금 느는 대신 normal 점유가 49.5 → 40.5칸으로 줄고 normal dispatch 정지가 16.8 → 27.9%로 는다. §2.3·§9.4의 관찰(예약은 대부분 비어 있다)이 성능 축에서 재확인된다.
- 민감한 suite는 SPEC17(non-stall 1.0603~1.0877)이고 GAP은 거의 무감각하다(1.1636~1.1699).
- 검증: `clear_rs25_nonstall`이 `260920_zereco_piq_dynamic/piq_dyn_dwell16`과 89/89 cycle 동일.
- 스크립트: `260921_CLEAR_rs_sweep/analysis/{suite_ipc_speedup.py, piq_behaviour.py}`.
  (fallback 비율은 관례대로 가중합의 비다. SimPoint별 비율의 평균으로 재면 25%에서 8.2%로 나온다 — 같은 run의 다른 집계다.)

**dwell 0에서 같은 비율을 다시 (`260922_CLEAR_rs_sweep_dwell0`, 5 config × 89, non-stall 고정)**

| 예약 | 10% | 15% | 20% | 25% | 30% |
|---|---|---|---|---|---|
| dwell 0 | 1.1011 | **1.1003 (표준)** | 1.0991 | 1.0964 | 1.0946 |
| dwell 16 | 1.1013 | 1.1003 | 1.0983 | 1.0972 | 1.0946 |
| 차이 (%p) | −0.01 | +0.01 | +0.08 | −0.08 | +0.00 |
| 해제 cycle | 33.6% | 33.6% | 33.6% | 33.5% | 33.5% |
| 해제 cycle (dwell 16) | 23.4% | 23.4% | 23.4% | 23.4% | 23.3% |
| fallback (두 dwell 동일) | 35.69% | 24.76% | 18.46% | 12.49% | 8.53% |

- **dwell은 성능 knob이 아니다.** 차이가 어디서나 ≤0.08%p이고 부호가 오가며, **그 움직임의 전부가 xgboost 하나**다(20% +2.53%p, 25% −3.23%p). 나머지 30개 워크로드는 최대 ±0.22%p(clang·gcc)이고 GAP·SPEC17·SPEC26 suite 평균은 ±0.05%p 안이다. 2026-09-20에 "dwell 16이 +0.08%p 낫다"고 본 것도 같은 xgboost 잡음이었음이 5개 비율에서 확인됐다.
- **왜 공짜인가 — fallback이 두 dwell에서 완전히 같다**(12.49% vs 12.49%). 해제는 normal 클래스의 admission 한도만 올리고 priority 클래스의 한도는 건드리지 않으므로, 해제 중 도착한 priority op도 물리 RS가 꽉 차지 않는 한 자기 자리를 받는다. 해제량이 23.4 → 33.6%로 43% 늘어도 normal 점유는 +0.1~0.35칸, normal dispatch 정지는 −0.2~−1.4%p뿐이다.
- → **유예 카운터를 없애고 dwell 0으로 단순화**했고, 비율은 **15%를 표준으로 확정**했다(2026-09-22 사용자 결정). 10%가 0.1%p 높지만 fallback이 35.7%로 크다.
- 검증: `clear_rs25_nonstall_dwell0`이 `260920_zereco_piq_dynamic/piq_dyn_dwell0`과 89/89 cycle 동일.
- **10% 미만은 미측정이다.** 0%에서는 어떤 op도 우선권을 받지 못해 RFP 단독(≈1.07)으로 떨어지므로 10%와 0% 사이에 꼭짓점이 있다(TODO N-3).
- 스크립트: `260922_CLEAR_rs_sweep_dwell0/analysis/{suite_ipc_speedup.py, piq_behaviour.py}`.

---

### 9.8 추적 임계를 정한 근거 (`260923_CLEAR_hbt256`, `260923_CLEAR_consume_gate`)

**(a) HBT를 논문 사양으로 바꾼 것은 성능 중립이다** (1024 direct-mapped → 256 8-way + 오예측 때만 할당):
Average 1.1003 → **1.1012**(+0.09%p)이고 그중 대부분이 xgboost 하나다. 정합성을 위한 변경이 성능을 건드리지 않았다.

**(b) 분기 인구 — 전부 추적할 이유가 없다** (분기 상태 장부, 한 창 = 10M 명령어).
집계는 관례대로 **워크로드 안에서 비율을 내고 워크로드 간 산술평균**이다:

| | GAP | SPEC17 | SPEC26 | DC | **Avg** |
|---|---|---|---|---|---|
| static 분기 수 (개) | 82 | 833 | 3,363 | 5,376 | 2,220 |
| **한 번도 안 틀림** (max counter 0) | 31.8% | 57.3% | 62.7% | 81.2% | **58.4%** |
| 틀렸지만 H2P 못 감 (max 1) | 13.9% | 20.5% | 18.9% | 13.0% | 17.4% |
| **H2P 도달** (max ≥ 2) | 54.3% | 22.2% | 18.4% | 5.8% | **24.3%** |
| 그 분기들의 **실행량**(commit) 몫 | 82.6% | 57.6% | 42.5% | 23.8% | **52.0%** |

**개수와 실행량의 비대칭이 논거다** — H2P에 도달하는 분기는 **개수로 24%인데 실행의 52%**를 차지한다.
나머지(개수 76%)는 추적해도 가속될 일이 없다. `counter ≥ 1`은 그 24%를 **하나도 놓치지 않으면서**
추적 대상을 41.6%로 줄인다 — H2P가 되려면 반드시 counter 1을 지나기 때문이다.
그림: `260923_CLEAR_consume_gate/analysis/fig_branch_population.*`.

> ⚠️ 집계 주의: 같은 데이터를 **모든 분기를 한 줄로 세워** 계산하면(= 워크로드 크기로 가중) 한 번도 안 틀림
> 75.2% / H2P 도달 9.9%가 된다. 코드가 큰 DC·SPEC26이 분기 수로 압도하기 때문이다. 프로젝트 관례(워크로드
> 등가중)는 위 표이고, 논문에는 위 표를 쓴다.

**(c) 왜 첫 승격이 아니라 재승격인가**: counter 1에서 승격까지 걸리는 instance는 **72.4%가 10회 미만**(중앙값 26,
DC 2)이라 *첫* 승격에는 미리 쌓을 틈이 없다. 그러나 분기당 승격이 **6.4회**이고 H2P 구간 사이의 간격은
**71.7%가 10K ops 이상, 48.3%가 100K 이상**이다 — 즉 재승격 때마다 chain을 다시 쌓아야 하고, counter ≥ 1 추적이
그 구간을 덮는다.

**(d) HBT 충돌의 상한**: 무제한 shadow HBT와 대조하면 256×8이 진짜 H2P 분기의 **6.4%를 놓치지만**(DC 28.5%)
그들의 실행량은 **0.64%**뿐이다. 놓치는 건 거의 다 cold branch다. 소비 게이트가 owner를 못 찾아 생기는
false negative도 실측 0.22%(gate 1)로 이 상한 안에 있다.

**(e) seed gate sweep** (소비 게이트·owner 보호 켬, brslice_tab 1K, 89 SimPoint):

| | GAP | SPEC17 | SPEC26 | DC | **Average** |
|---|---|---|---|---|---|
| 게이트 off (직전 표준) | 1.1693 | 1.0878 | 1.0775 | 1.0919 | 1.1012 |
| gate 2 + 게이트 | 1.1692 | 1.0889 | 1.0775 | 1.0889 | 1.1010 (−0.02) |
| **gate 1 + 게이트 (채택)** | **1.1698** | **1.0899** | **1.0826** | 1.0911 | **1.1033 (+0.20)** |
| gate 0 + 게이트 | 1.1691 | 1.0858 | 1.0779 | 1.0866 | 1.0995 (−0.17) |

두 효과가 분리된다 — **소비 게이트 자체는 중립**(−0.02%p)이고 **추적 확대가 +0.23%p**를 만든다.

| (Average) | gate 2 | **gate 1** | gate 0 |
|---|---|---|---|
| chain 멤버 commit | 26.4% | 28.1% | **54.1%** |
| 가속 대상(priority) 비중 | 13.2% | **13.4%** | 13.4% |
| **Target Load hit** | 40.5% | **46.7%** | 37.9% |
| **RFP useful / load** | 18.6% | **22.4%** | 18.5% |
| brslice_tab 멤버 축출 /1K op | 0.02 | 0.06 | **28.3** (DC 92) |
| PT 축출 /1K load | 0.19 | 0.28 | 4.28 |
| 게이트: owner를 HBT에서 잃음 | 0.06% | 0.22% | **34.1%** |

- **gate 1의 이득은 RFP에서 나온다**: 가속 인구는 그대로인데(13.2 → 13.4%) Target Load hit이 +6.2%p, useful이
  상대 +20%다. counter ≥ 1 구간에서 **PT가 미리 stride를 배워 두었다가 승격 순간 바로 쓰기** 때문이고, 이것이
  §5.9의 "학습은 넓게, 발사는 좁게"가 실제로 값을 만든 지점이다.
- **gate 0은 테이블을 무너뜨린다**: 멤버 축출이 470배로 뛰고 Target Load hit이 게이트 off보다도 낮아진다.
  PUBS식 전 branch 추적이 실제로 손해라는 직접 증거다.
- **소비 게이트는 같은 성능을 더 적은 자원으로 낸다**: gate 2에서 가속 대상을 3.1% 줄이고 prefetch를
  7.0/1K load 보류했는데 IPC는 그대로다 — 강등 뒤 남아 돌던 가속은 기여하지 않고 있었다.
- ⚠️ **주의**: gate 1의 +0.20%p 중 대부분이 **llvm_r_1(+2.80%p)과 nab(+2.76%p)** 두 워크로드에서 나온다.
  mcf는 −1.70%p 잃는다. 승패 19:12, 중앙값 +0.023%p. 방향은 맞지만 표본 쏠림을 감안해야 한다.
**(f) brslice_tab 크기는 무관하다** (`260923_CLEAR_gate1_tabsize`, gate 1 + 소비 게이트 고정, 8-way):

| entries | Average | 멤버 축출 /1K op | PT 축출 /1K load | Target Load hit | 추적량 |
|---|---|---|---|---|---|
| 512 | 1.1029 | **0.554** | 0.26 | 46.5% | 27.8% |
| **1K (채택)** | **1.1033** | 0.055 | 0.28 | 46.7% | 28.0% |
| 2K | 1.1030 | **0.002** | 0.28 | 46.8% | 28.1% |
| 4K | 1.1030 | **0.000** | 0.29 | 46.8% | 28.1% |

- **압박은 실재했지만 무관했다.** 2K에서 축출이 완전히 사라지는데도 Target Load hit은 46.7 → 46.8%, IPC는 −0.03%p다.
  반대로 512로 절반을 잘라 축출을 10배로 키워도 −0.04%p뿐이다. GAP·SPEC17은 네 크기에서 소수 넷째 자리까지 같다.
- → **1K 확정.** PUBS와 같은 예산이고, "식별 구조는 싸다"가 새 설계에서도 재현됐다.
- **mcf 손실은 용량 때문이 아니다**: 네 크기에서 **전부 1.4649**(게이트 off 1.4820). 축출이 0이어도 −1.70%p가 그대로라
  원인은 **소비 게이트가 mcf의 어떤 slice를 끊는 것**이다. 필터 A 손실도 mcf에 몰려 있었으므로 같은 뿌리로 보인다(TODO N-1).
- **gate 0의 붕괴는 "1K가 작아서"가 아니다**: gate 1은 512에서도 멀쩡한데(축출 0.554) gate 0은 1K에서 28.27 — **500배**다.
  전 branch 추적의 작업집합이 테이블 규모를 근본적으로 초과한다. 다만 **gate 0을 큰 테이블에서 돌려보지는 않았으므로**
  논문에는 "1K 기준"임을 명시한다.

- 스크립트: `260923_CLEAR_hbt256/analysis/{branch_distribution.py, branch_dynamics.py, ipc_and_integrity.py}`,
  `260923_CLEAR_consume_gate/analysis/gate_sweep.py`, `260923_CLEAR_gate1_tabsize/analysis.py`.
- 논문용 그림: `260923_CLEAR_consume_gate (중요)/analysis/{fig_branch_population, fig_seed_gate_sweep_combined, table_gate_mechanism}.*` (seed gate sweep은 suite별 + Avg. 한 그림으로 통합, 2026-09-28)
  — 각각 "추적 대상을 줄일 여지가 크다"(개수 24% vs 실행 52%), "끝까지 늘리면 테이블이 무너진다",
  "가속은 안 넓어졌는데 RFP 품질만 올랐다"를 보인다.

### 9.9 필터 A — 비율 방식 (2026-10-01 확정, X 50%, N 4, 창 64)

실험은 두 개다. `261001_CLEAR_edge_ratio_sweep`(무한 크기 + member-only, X 50/25/12.5 × N 2/4/8, full slice 같은 sweep, 20 × 89)과
`261001_CLEAR_edge_ratio_finite`(X 50/60/70, N 4, 세 기계, 8 × 89)이다. 바이너리는 e59c2aaf다. 이미 돈 비교점은 다시 돌리지 않고
재사용했다(X = 0과 full slice에서 새 바이너리가 이전 run과 cycle 동일함을 확인). 합격 기준은 모두 통과했다.

**(a) 세 기계에서의 IPC (Average)**

| 기계 | 이전 필터 A | **X 50%** | X 60% | X 70% | full slice |
|---|---|---|---|---|---|
| 표준 크기 (CLEAR baseline 기계) | 1.1033 | **1.1092** | 1.1082 | 1.1074 | 1.1086 |
| 표준 크기 + member-only | 1.0978 | **1.1045** | 1.1035 | 1.1024 | 1.1051 |
| 무한 크기 + member-only | 1.0972 | **1.1053** | 1.1038 | 1.1026 | 1.1058 |

- 세 기계 모두 X 50%가 가장 좋고, full slice와의 차이는 −0.06 ~ +0.06%p로 잡음 수준(±0.05%p)이다.
- X를 10%p 올릴 때마다 IPC가 약 0.1%p씩 떨어진다. 주된 입력이 가끔 끊길 때도 막히기 시작하기 때문이다(§5.3 표).
- X 50% 아래(무한 + member-only, N 4): X 25% 1.1052, 12.5% 1.1051로 IPC는 같고 자원만 는다(가속 후보 32.8 → 34.8 → 35.3%, 상주 멤버 146 → 158 → 163).
- N(2/4/8)은 IPC를 ±0.04%p밖에 바꾸지 않는다.

**(b) 자원 — 표준 크기 (CLEAR baseline 기계)**

| | 이전 필터 A | **X 50%** | X 60% | X 70% | full slice |
|---|---|---|---|---|---|
| 필터가 막은 판정 | (27.3%¹) | 3.3% | 5.6% | 6.9% | – |
| 가속 후보 / commit | 25.9% | 31.9% | 30.9% | 30.3% | 39.9% |
| 실제 가속 / commit | 21.9% | 26.3% | 25.3% | 24.9% | 30.7% |
| 상주 멤버 PC | 78 | 135 | 126 | 123 | 213 |
| 전용 칸 받은 후보 | 80.6% | 78.4% | 79.3% | 79.4% | 75.3% |
| P-IQ 칸이 가득 찬 cycle | 10.7% | 14.2% | 13.5% | 13.3% | 16.7% |
| brslice_tab 멤버 축출 / 1K op | 0.055 | 0.414 | 0.305 | 0.253 | 2.632 |
| PT 축출 / 1K load | 0.275 | 0.555 | 0.549 | 0.518 | 1.141 |
| RFP useful / 1K load | 223.8 | 244.9 | 229.3 | 225.7 | 284.9 |

¹ 이전 필터 A의 막은 몫은 판정 횟수 통계가 있는 무한 크기 run에서 쟀다.

- **X 50%는 full slice와 같은 IPC를 가속 후보 80%, 실제 가속 86%, 상주 멤버 63%로 낸다.** 1K 표에서 멤버 축출은 full의 1/6,
  PT 축출은 절반이다. 이전 필터 A보다는 자원을 더 쓰지만, full이 이전 필터 A보다 더 쓰는 자원의 절반 정도만 더 쓰면서 full의 이득을 다 얻는다.
- 필터가 막는 몫이 3% 남짓까지 줄었는데도 full과의 자원 차이가 남는다. 이제 그 차이를 만드는 것은 필터 A가 아니라 **마지막에
  도착한 producer 하나만 따라가고, 기다린 입력이 없는 명령어에서 멈추는 CLEAR의 기본 규칙**이다.
- 이득은 이전 필터 A가 가장 많이 막던 곳에서 나왔다: mcf +6.9%p, zstd_r_1 +4.4%p, llvm_r_1 +2.1%p, xgboost +1.8%p, nab +1.6%p,
  clang +1.2%p(표준 크기, X 50% − 이전 필터 A). 잃은 곳은 sssp −1.3%p로, full slice도 같은 만큼 잃는다.
- member-only의 비용(X 50%): CLEAR −0.47%p(1.1092 → 1.1045), full slice −0.35%p(1.1086 → 1.1051).
- L2 "가속 후에도 chain 위"(가속 뒤 상태 — 선택 정확도로 읽지 않는다): 이전 필터 A 66.3%, X 50% 60.7%, full 51.1%.

- 분석: `261001_CLEAR_edge_ratio_sweep/analysis/{sweep.py, acceptance.txt, summary.txt}`,
  `261001_CLEAR_edge_ratio_finite/analysis/{finite.py, acceptance.txt, summary.txt}`. 단위 테스트(끼어들기 표)는 critpath.c의
  필터 함수 본문을 그대로 잘라 독립 기준 구현과 모든 판정·통계를 대조했다.

---

## 10. 설계 근거 요약 (옛 머신 실험에서 확정된 것)

C1~C17의 상세 표는 지웠다(git 이력에 남아 있다). 확정된 설계로 이어진 결론만 남긴다. **모두 352-entry 머신(`PARAMS.golden_cove_rs352`)·67 SimPoint 기준이므로 절대 수치를 현재 결과와 섞지 않는다.**

| 축 | 결론 | 그래서 |
|---|---|---|
| brslice_tab 용량 (512 ~ 32K) | 64배 줄여도 IPC −0.2%p. LRU가 cold PC부터 버리므로 상주 PC가 4배 줄어도 실제 가속 대상은 조금만 준다 | **1K 채택.** 식별 구조는 싸다 |
| depth 제한 (∞/8/4/2/1) | 인구 잔존율과 이득 잔존율이 같다(효율 평평). Target Load가 chain 깊은 곳에 살아서 depth를 자르면 RFP가 먼저 무너진다. DC만 depth 4에 sweet spot | **제한 없음** |
| refresh 수명 (1.6M ~ 20K) | 효율을 올린 유일한 knob. 가속 대상 −6.3%p에 IPC −0.06%p, critical vs full 필터링 3.1 → 6.8% | **1 bit / 10K 채택** |
| 필터 A/B/C | A만 20% 이상 거른다(filtering 35%). B·C는 refresh와 겹쳐 7~9%. 임계는 지렛대가 아니고(3 → 1이 IPC 0.16%p만 회복), hysteresis는 무효 | **A만, 임계 3** → 2026-10-01 **비율 방식(X 50%, N 4)으로 대체**(§5.3, §9.9) |
| A의 depth 면제 | A가 막는 전파의 54%가 depth 1(비교 명령), depth 0은 0.8% | **depth ≤ 1 면제**(IPC 손실 37% 회복, filtering 25.6% 유지) — 비율 방식에도 그대로 |
| wrong-path priority | 켜면 priority 자격 dispatch의 67.8%가 wrong-path, IPC −0.85%p | **끔(oracle) + 민감도로 보고** |
| store→load edge | 멤버 +4%p, IPC +0.3%p이나 LPR이 SQ entry를 가리키는 확장 필요 | **불채택** |
| 멤버 전용 할당 | 전체 PC 할당은 테이블이 프로그램 static PC 수를 따라가 용량 연구가 무의미해진다 | **멤버 전용** |
| owner 기준 chain 제거 | 경계선 분기의 강등·복귀마다 chain을 다시 쌓게 되고, 공유 멤버(재지목의 11.2%)까지 지운다 | **refresh만** |
| RFP store forwarding | 옛 보수 모델이 버리던 Target Load 19%의 절반 회수, IPC +1.0% | **모드 1** |
| RFP 실패 분포 | 주소 예측 불가 43%, 늦게 도착 11.7%, 잘못된 주소 2.6% | 남은 여지는 주소 예측 쪽 |

---

## 11. 논문 서술 시 유의

- **1차 지표는 H2P resolution latency**다. IPC 실현률은 그보다 낮다.
- 머신은 원본 Golden Cove(`PARAMS.golden_cove_original`, main RS 186)다. 352-entry 머신 수치(§10 출처)는 논문에 쓰지 않는다.
- TEA 비교는 **자원 예약을 고친 뒤의 TEA**(`TEA_preg102_vec16`, 1.1020)를 쓴다. 논문대로 모든 H2P instance에 chain을 띄우면 이 코어에서는 baseline보다 11% 느리므로, 비교에는 oracle trigger만 쓰고 그 사실을 명시한다(이 설정은 논문보다 TEA에 유리하다).
- TEA를 "불리하게 맞췄다"는 식으로 쓰지 않는다 — **논문이 키운 구조체를 원본 Golden Cove에 맞춰 조정했을 때 드러나는 단점**을 보는 것이다.
- 평가 모드(wrong-path에 priority 없음)를 명시하고 하드웨어 동작을 민감도로 제시한다.
- critical vs full: 필터링 비율이 아니라 **같은 IPC(+0.06%p)에서의 자원 절감**으로 서술한다 — 가속 후보 −20%, 실제 가속 −14%, 상주 멤버 −37%, 1K 표에서 멤버 축출 1/6, PT 축출 1/2(§9.9, 필터 A 확정 후). 이전 필터 A 시절의 "−63% ↔ −0.7%p"는 쓰지 않는다.
- "필터 A"는 비율 방식(X 50%, N 4)을 가리킨다. 이전 연속 4회 규칙은 필요할 때만 "이전 필터 A"로 언급한다.
- uop granularity는 (PC, uop index) 키로 서술한다(§5.8, 그림 fig7).
- config 사이의 설정 차이(prefetcher·scheduler 등)를 해석으로 덧붙이지 않는다(2026-09-17 사용자 방침).
- xgboost·tc 표본 성격은 methodology에 적지 않는다(사용자 방침) — 결과 해석에서만 유의한다.

## 12. 그림

`figures/draw_arch.py`가 생성한다 (`python3 figures/draw_arch.py`; svg·pdf·png). 그림 2·3(LPR, 멤버십 전파)은 사용자 원본 그림이다.

| 로직 | 파일 | 내용 |
|---|---|---|
| 1. H2P 판별 | `figures/fig1_h2p_hbt.*` | HBT 갱신(resolve)·조회(fetch)·decay·commit 때 chain root 삽입 |
| 4. Target Load + 주소 예측 | `figures/fig4_target_load_pt.*` | commit: 멤버십 → PT 할당·stride 학습 / rename: 예측 주소 생성 |
| 5. priority scheduling | `figures/fig5_priority_iq.*` | 분할 RS(priority/normal), non-stall fallback, select 우선권 |
| 6. RF prefetch | `figures/fig6_rfp.*` | rename 직후 launch, store scan, spare-port L1 probe, AGU 검증 |
| 7. uop 단위 brslice_tab | `figures/fig7_brslice_tab_uop.*` | (PC, uop index) 키: decode에서 한 번 조회, uop i가 bit i를 가져감, 1-bit tag가 파이프라인을 타고, commit 때 backward walk가 bit를 세운다 |

논문용 결과 그림은 각 실험 폴더의 `analysis/`에 있다 — 현재 기준은 `260920_zereco_n_tea (최신)/analysis/`
(workload·suite IPC speedup, H2P latency 분해, TEA 명령어 비중 표).
