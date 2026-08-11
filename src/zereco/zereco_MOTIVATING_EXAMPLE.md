# ZERECO Motivating Example

> 논문 Motivation 그림용 명령어 스트림과 cycle 단위 파이프라인 타임라인.
> 구조는 실제 workload trace에서 추출한 dataflow를 그림용으로 단순화한 것이다 (§6 근거).
> Last updated: 2026-08-12

**보여주는 것**: H2P branch가 IQ에 들어온 뒤 execute될 때까지의 대기 시간이
**RF prefetch로 4 cycle, priority scheduling으로 5 cycle** 줄어들고, 결합하면 **9 cycle** 줄어든다 (20 → 11).

---

## 1. 명령어 스트림

기호: `◀` H2P branch · `★` Target Load · `●` H2P의 backward slice 멤버

```
      instruction                    slice   비고
  ┌────────────────────────────────────────────────────────────────────┐
  │ I1   add  r1, r1, 1              ·      독립                       │
  │ I2   add  r2, r2, 1              ·      독립                       │
  │ I3   add  r8, r8, 1              ·      독립                       │
  │ I4   add  r9, r9, 1              ·      독립                       │
  │ C1   add  r3, r3, 4              ●                                 │
  │ C2   lw   r4, 0(r3)         ★    ●      LOAD A : 주소 예측 가능     │
  │ C3   add  r5, r13, r4            ●                                 │
  │ C4   lw   r6, 0(r5)         ★    ●      LOAD B : 주소 예측 불가     │
  │ C5   bne  r6, r14, loop     ◀           H2P BRANCH                 │
  │ I5   add  r10, r10, 1            ·      독립                       │
  │ I6   add  r11, r11, 1            ·      독립                       │
  └────────────────────────────────────────────────────────────────────┘
```

- **chain 5개 / 독립 6개** — 독립 명령어가 select를 두고 경쟁하므로 priority scheduling의 효과가 드러난다.
- **H2P는 `C5` 하나.** basic block 하나로 끝나 CFG가 필요 없다.
- **`r13`, `r14`는 loop invariant**, `r3`는 loop-carried induction.

### Dependence chain

```
  C1 add ──► r3 ──► C2 lw ★A ──► r4 ──► C3 add ──► r5 ──► C4 lw ★B ──► r6 ──► C5 bne ◀ H2P
                     (stride 예측 가능)                      (예측 불가)
```

**LOAD A의 값이 LOAD B의 주소가 된다** — pointer chase. 따라서 A를 앞당기면 B의 발사도 그만큼 앞당겨진다.

### 왜 두 load가 모두 필요한가

- **LOAD A**만 있으면: RFP가 chain을 전부 덮으므로 slice-level filtering(§7.1)이
  다음 occurrence의 P-IQ marking을 **억제한다.** 그림이 자기 설계와 모순된다.
- **LOAD B**는 원리적으로 예측 불가하므로 RFP가 손댈 수 없고, **residual slice가 남아 P-IQ가 필요해진다.**

즉 이 예시는 **두 메커니즘 중 어느 하나만으로는 chain을 끝낼 수 없음**을 구조적으로 보장한다.

---

## 2. 파이프라인 모델

RFP(ISCA'22) Fig.8/9와 동일한 표기.

| 기호 | 단계 |
|---|---|
| `W` | Wakeup |
| `s` | **select 경합으로 대기** ← P-IQ가 제거하는 구간 |
| `S` | Select |
| `R` | Register Read / Scoreboard |
| `E` | Execute (ALU, 1 cycle) |
| `A` | Address generation (AGU) |
| `C` | L1 cache access (4 cycle) |
| `.` | **RFP hit으로 생략된 L1 access** ← RF prefetch가 제거하는 구간 |

- ALU 실행 1 cycle, **load 실행 5 cycle**(`A`+`CCCC`) — `PARAMS.golden_cove`의 L1D 5 cycle
- 최단 `S = W + 1`. select 경합이 있으면 `S = W + 2`
- producer가 Select된 뒤 `(실행 latency − 1)` cycle에 dependent가 Wakeup (back-to-back 스케줄링)

### RFP가 적용된 load는 사라지지 않는다

RFP hit이어도 **demand load는 `W → S → R → A`를 그대로 거친다.** 실제 주소를 계산해
예측 주소와 일치하는지 검증해야 하기 때문이다. 생략되는 것은 **`CCCC`(L1 access 4 cycle)뿐**이고,
값은 이미 destination physical register에 있다.

따라서 RFP의 효과는 **해당 load의 실효 latency가 5 → 1 cycle로 줄어드는 것**이며,
dependent는 RFP 논문 Fig.9처럼 **load의 AGU 다음 cycle에 back-to-back으로 Execute**한다.
`C1`(주소 피연산자 `r3`를 생산)도 chain에 그대로 남는다.

### 측정 구간 정의

**cycle 0 = H2P branch(`C5`)가 rename을 마치고 IQ에 들어온 cycle.**
보고하는 값은 그 시점부터 `C5`가 **Execute되는 cycle**까지다.

이 구간이 ZERECO가 줄이려는 대상과 정확히 일치한다 — profiler의 `dependency` + `scheduler` + `execution`
단계의 합이며, frontend latency와 recovery latency는 포함하지 않는다.
(in-order dispatch이므로 `C1`~`C4`는 `C5`와 같거나 이른 cycle에 IQ에 들어와 있다.)

---

## 3. Figure — cycle 단위 파이프라인

### (a) Baseline — RFP 없음, P-IQ 없음

```
                          0    5   10   15   20
                          |    |    |    |    |
  C1  add  r3, r3, 4      WsSRE
  C2  lw   r4, 0(r3)        WsSRACCCC
  C3  add  r5, r13, r4            WsSRE
  C4  lw   r6, 0(r5)                WsSRACCCC
  C5  bne  r6, r14                        WsSRE
                                              ^ H2P resolved @ 20
```

chain op 5개가 각각 select 경합으로 1 cycle씩 밀린다(`s`). 독립 명령어 6개가 그 slot을 가져간다.

### (b) + P-IQ only — chain op이 select를 즉시 획득

```
                          0    5   10   15   20
                          |    |    |    |    |
  C1  add  r3, r3, 4      WSRE
  C2  lw   r4, 0(r3)       WSRACCCC
  C3  add  r5, r13, r4          WSRE
  C4  lw   r6, 0(r5)             WSRACCCC
  C5  bne  r6, r14                    WSRE
                                         ^ H2P resolved @ 15
```

`s`가 전부 사라진다. **20 → 15 (−5 cycle).** 그러나 두 load의 실행 latency는 그대로다.

### (c) + RFP only — `C2`의 L1 access가 생략된다

```
                          0    5   10   15   20
                          |    |    |    |    |
  C1  add  r3, r3, 4      WsSRE
  C2  lw   r4, 0(r3)        WsSRA....
  C3  add  r5, r13, r4        WsSRE
  C4  lw   r6, 0(r5)            WsSRACCCC
  C5  bne  r6, r14                    WsSRE
                                          ^ H2P resolved @ 16
```

`C2`는 여전히 `W S R A`를 거쳐 실제 주소를 계산하고 예측을 검증한다.
사라지는 것은 **`....`로 표시한 L1 access 4 cycle**이며, `C3`는 `C2`의 AGU 다음 cycle에 Execute한다.
**20 → 16 (−4 cycle).** 절약분이 정확히 생략된 L1 access와 같다.

### (d) + RFP + P-IQ

```
                          0    5   10   15   20
                          |    |    |    |    |
  C1  add  r3, r3, 4      WSRE
  C2  lw   r4, 0(r3)       WSRA....
  C3  add  r5, r13, r4      WSRE
  C4  lw   r6, 0(r5)         WSRACCCC
  C5  bne  r6, r14                WSRE
                                     ^ H2P resolved @ 11
```

`s`와 `....`가 모두 사라진다. **20 → 11 (−9 cycle).**

### 요약

| 구성 | 제거되는 구간 | H2P resolve | 절약 |
|---|---|---|---|
| Baseline | — | **20 cy** | — |
| + P-IQ only | `s` × 5 | **15 cy** | **−5** |
| + RFP only | `C2`의 L1 access 4 cy | **16 cy** | **−4** |
| **+ RFP + P-IQ** | 둘 다 | **11 cy** | **−9** |

**두 절약분은 서로 다른 것에 비례한다.**

- **P-IQ**의 절약 = *chain 길이* × (op당 select 대기). 여기서는 5 op × 1 cycle = 5.
  chain이 길수록 커지지만 **op당 1 cycle이 상한**이다.
- **RFP**의 절약 = *covered load의 cache access latency*. 여기서는 L1 hit이라 4 cycle.
  **load가 miss할수록 커진다** — 같은 load가 L2 hit(16 cy)이면 baseline이 31 cycle이 되고
  RFP는 그대로 16이므로 **절약이 15 cycle**이 된다.

이 예시의 LOAD A는 L1-resident(§6)라 두 메커니즘의 절약이 비슷하게 나온다.
**`s` × 5는 P-IQ가 낼 수 있는 최댓값**(PUBS가 제시한 op당 1 cycle 가정)이므로 이 그림은 P-IQ에 유리한 쪽으로 잡힌 것이고,
실측에서 P-IQ의 기여가 RFP보다 작게 나오는 것과 모순되지 않는다.

핵심은 크기 비교가 아니라 **서로 다른 구간을 제거한다는 점**이다 —
P-IQ는 load가 issue된 뒤의 cache/memory latency를 **원리적으로** 없앨 수 없고,
RFP는 예측 불가한 LOAD B와 residual chain의 select 대기를 없앨 수 없다.

---

## 4. 변형 — H2P branch 2개

실제 trace에서는 H2P가 연달아 나오고 **같은 load 결과를 공유**하는 경우가 흔하다
(측정: 같은 값을 소비하는 두 H2P 쌍 48개 중 48개에서 재load 없음).

```
  ...
  C4   lw   r6, 0(r5)          ★    ●     LOAD B
  C5   bne  r6, r14, X         ◀          H2P BRANCH 1
  I5   add  r10, r10, 1             ·     독립
  C6   beq  r6, r15, Y         ◀          H2P BRANCH 2  (같은 r6 소비)
  I6   add  r11, r11, 1             ·     독립
```

두 branch가 **하나의 chain을 공유**하므로 chain을 한 번 가속하면 **두 resolution이 함께 앞당겨진다.**

```
Baseline
                          0    5   10   15   20
                          |    |    |    |    |
  C4  lw   r6, 0(r5)                WsSRACCCC
  C5  bne  r6, r14                        WsSRE
  C6  beq  r6, r15                        Ws sSRE
                                              ^ H2P-1 @ 20
                                               ^ H2P-2 @ 21

+ RFP + P-IQ
                          0    5   10   15   20
                          |    |    |    |    |
  C4  lw   r6, 0(r5)         WSRACCCC
  C5  bne  r6, r14                WSRE
  C6  beq  r6, r15                WsSRE
                                     ^ H2P-1 @ 11
                                      ^ H2P-2 @ 12
```

이 변형이 추가로 보여주는 것:

1. **가속 비용이 분모로 나뉜다** — RFP request 한 번, priority entry 한 묶음으로 **두 개의 H2P**를 앞당긴다.
2. **shared producer 규칙이 필요한 이유** — `C1`~`C4`는 두 slice가 공유하므로,
   한 slice가 RF-covered라는 이유로 priority를 제거하면 다른 slice가 손해를 본다
   (아키텍처 §5.4).

---

## 5. 이 예시가 증명하는 설계 결정

| 설계 결정 | 그림에서의 근거 |
|---|---|
| 두 메커니즘이 상보적 | (d) −9 cycle. 절약분이 (b) −5와 (c) −4의 합 — 서로 다른 구간을 제거하므로 겹치지 않는다 |
| RF prefetch가 필요하다 | (b)에서 `C2`, `C4`의 `CCCC`는 그대로 남는다. **scheduling은 cache latency를 원리적으로 못 없앤다** |
| P-IQ가 필요하다 | (c)에서 `s`가 그대로 남는다. LOAD B는 예측 불가라 RFP가 손댈 수 없다 |
| RFP가 primary인 이유 | RFP 절약은 covered load의 **cache latency에 비례**(L1 4cy, L2면 15cy)하지만, P-IQ 절약은 **op당 1 cycle이 상한**이다 |
| slice 단위 filtering (load 단위 아님) | LOAD A가 covered여도 LOAD B가 uncovered라 slice 전체가 여전히 지연 |
| predictor가 abstain해야 한다 | LOAD B에 request를 만들면 100% wrong prediction + 대역폭 낭비 |
| non-chain 명령어가 있어야 P-IQ가 의미 있다 | (a)의 `s` 구간은 독립 명령어 6개가 select를 가져가기 때문에 생긴다 |
| **demand load는 RFP 후에도 실행된다** | (c)의 `C2`가 `W S R A`를 유지한다 — 주소 검증이 필요하므로 생략되는 것은 `CCCC`뿐 |

---

## 6. 실측 근거

이 구조는 임의로 만든 것이 아니라 실제 trace에서 관측한 dataflow다.
(`gapbs/bc_g19_n100/4496`, 512-entry retired window)

| | LOAD A에 해당하는 load | LOAD B에 해당하는 load |
|---|---|---|
| 주소 delta | **+4가 47/47 (100%)** | **48개 전부 상이** (최빈 delta 빈도 1) |
| 라인당 접근 | **12.0회** | **1.00회** |
| touch한 4KB 페이지 | 1 | **34** |

- LOAD A는 stride가 완벽해 1-bit confidence stride predictor로 100% 커버되지만,
  **라인당 12회 접근**이라 12번 중 11번은 이미 L1에 있다 — **cache prefetch로는 얻을 것이 없고 RF prefetch만이 그 latency를 제거한다.**
- LOAD B는 예측도 locality도 없어 어떤 prefetch도 실패한다 — **남는 지렛대는 scheduling뿐이다.**

또한 filtering 없이 chain을 전부 표시하면 priority-marked 비율이 **평균 52.4%, 최대 88.3%**에 달한다.
CRISP가 밝힌 criticality scheduling의 동작 범위(5~40%)를 벗어나므로,
**RF coverage 기반 filtering은 선택이 아니라 전제조건이다.**

---

## 7. 도구

```bash
python3 src/zereco/tools/fill_buffer_walk.py <retired_stream_log_dir>   # slice 추출 + 주소 delta 분포
python3 src/zereco/tools/bc_example_detail.py                           # §6의 상세 수치
```

`tools/h2p_example.c` — 이 패턴을 재현하는 마이크로벤치마크.
`DEPTHS_N`을 줄이면 LOAD B가 L1 hit이 되고, 색인을 순차로 바꾸면 LOAD B도 예측 가능해져 **RF-only 대조군**이 된다.
