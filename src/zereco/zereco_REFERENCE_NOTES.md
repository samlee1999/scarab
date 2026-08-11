# ZERECO Reference Paper Notes

> 목적: ZERECO 설계·구현에 필요한 4개 reference 논문의 mechanism-level 정독 노트.
> 논문별로 (a) 무엇을 하는가, (b) ZERECO가 무엇을 차용/변경하는가, (c) 구현 시 반드시 알아야 할 수치를 남긴다.
>
> Last updated: 2026-08-11

---

## 1. RFP — "Register File Prefetching" (ISCA 2022, Intel Labs)

Shukla, Bandishte, Gaur, Subramoney. ZERECO의 RF prefetch 메커니즘의 직접적 기반.

### 1.1 핵심 주장과 motivation 수치

논문의 출발점은 **"memory wall은 단일 벽이 아니라 여러 latency wall의 집합"** 이다.

| 관찰 | 수치 |
|---|---|
| Demand load 중 L1 hit 비율 | **92.8%** (L2 1.6%, LLC 0.2%, Memory 0.3%, MSHR hit 3.4%) |
| Memory→LLC oracle prefetch headroom | 13.3% |
| **L1→RF oracle prefetch headroom** | **9%** |
| Tiger Lake L1 latency | 5 cycles (main memory 대비 40× 낮음) |

L1 latency는 memory 대비 40배 작지만 **접근 횟수가 압도적**이라 성능 영향이 memory와 비슷한 크기다.
Figure 3: LLC miss가 만든 critical path 위에 그 miss의 **address를 계산하는 L1-hit load들**이 모두 올라가 있음 →
critical path를 줄이려면 LLC miss뿐 아니라 그 dependence chain의 L1-hit load도 가속해야 한다.

> **ZERECO 연결점**: 사용자의 과거 실험에서 Target Load 대부분이 L1 hit이었던 것은 RFP의 이 관찰과 정확히 일치한다.
> L1 hit이 지배적일수록 cache prefetch는 줄일 latency가 없고 RF prefetch만이 load-to-use 직렬화를 제거할 수 있다.
> §5.1 "cache prefetch가 아니라 RF prefetch인 이유" 논거를 이 데이터로 강화할 수 있다.

### 1.2 왜 기존 Address Prediction(DLVP/Composite/EPP)이 아니라 RFP인가

RFP는 prior AP를 4가지로 반박하며, 이는 ZERECO의 설계 결정에도 그대로 적용된다.

1. **Flush 비용** — AP/VP는 wrong address 시 full pipeline flush → 높은 confidence 요구 → coverage 급감.
   RFP는 flush가 없으므로 **낮은 confidence로도 공격적으로 prefetch 가능**.
2. **Front-end launch의 slack 소멸** — uop-cache/LSD 때문에 fetch→allocate 구간이 짧아져 DLVP식 fetch-time launch는 시간이 안 됨.
   추가로 DLVP는 L1 latency 1 cycle을 가정했으나 실제는 5 cycles.
3. **두 번의 L1 access** — AP는 (데이터 읽기 + 검증) 2회 접근이 필요해 L1 bandwidth를 잡아먹음.
   **RFP는 rename 이후 launch하므로 정상 load와 동일한 memory disambiguation/store check를 통과 → 두 번째 검증 access 불필요.**
4. **In-flight store 충돌** — fetch 시점 launch는 in-flight store 전체를 추적해야 해 비현실적. Rename 이후면 기존 LSQ 로직 재사용.

DLVP coverage 붕괴 과정 (Figure 16) — ZERECO가 fetch-time launch를 택하면 안 되는 이유의 정량적 근거:

```
address-predictable loads    ~ RFP와 동등
→ high-confidence 필터        49%
→ no-FWD(store forward) 필터  45%
→ L1 port 가용성 제약         22%
→ allocate 전 데이터 도착      11%   ← 최종 value prediction 성공률
```

RFP는 같은 조건에서 **43.4%** (DLVP의 3.8배).

### 1.3 RFP 메커니즘 (구현 관점)

**Prefetch Table (PT)**
- static load **PC로 indexing**, 8-way set-associative, 1K~2K entry
- entry 필드: tag(16b), confidence(3b), utility(2b), stride(5b), **inflight counter(7b)**, PAT pointer(6b), page offset(12b)
- **retire 시점에 training** (stride 계산이 단순해짐). stride 반복 시 confidence를 **1/16 확률로** 증가(포화 속도 조절),
  stride 변화 시 confidence·utility reset, utility 낮으면 evict.
- **inflight counter**: 같은 PT entry에 대응하는 outstanding load 수. **allocate 시 증가, commit 시 감소, branch mispredict 시 squash된 load마다 감소.**
- 예측 주소 = `base address + stride × inflight counter`

**Page Address Table (PAT)** — 면적 최적화
- 64-entry 4-way. PT는 full VA 대신 (PAT pointer 6b + page offset 12b)만 저장 → **저장공간 ~50% 절감, 성능 손실 0.09%**
- page를 넘어가면 misprediction, PAT entry 교체 후 재학습

**Launch 시점과 packet**
- PT lookup은 rename 이전에 가능(timing critical path 아님). **Prefetch packet은 rename 직후 생성.**
- packet 내용: **predicted virtual address + load의 destination physical register id (prfid)**
- prfid는 writeback 때만 필요하므로 엄밀히는 나중에 공급해도 됨
- **RFP queue는 FIFO, 오래된 요청 우선**. L1 port 중재에서 **demand load보다 항상 낮은 우선순위**

**In-flight store / coherence 처리 (correctness의 핵심)**
- RFP가 LSQ+L1에 issue될 때 **older store를 youngest-first로 스캔**하여 주소 비교
- 주소 일치 → store 완료를 기다렸다가 **store data를 사용**
- store 주소 미확정 → **Memory Disambiguation predictor**로 대기/무시 결정
- MD 예측이 틀렸는데 **load가 아직 dispatch 전이면 flush 불필요** — load가 prefetch data를 쓰지 않고 LSQ/cache를 다시 보면 됨
- 이 전체 시퀀스가 **conventional load pipeline과 동일** → **predicted address만 맞으면 데이터도 반드시 correct** ⇒ 검증용 2차 L1 access 불필요
- coherence flush도 in-flight store와 동일하게 처리. **dispatch 전까지 RFP는 load의 proxy로 취급**되어 memory barrier 재배치도 자동 금지

**Pipeline simplification**
- RFP가 **L1 miss면 하위 레벨까지 진행** (일반 demand load처럼). 성능 기여는 +0.02%로 미미
- **DTLB miss면 RFP drop** (TLB miss는 길어서 run-ahead가 남지 않음). 성능 영향 거의 없음

### 1.4 RFP 타이밍 — `RFP-inflight` 비트 (가장 중요한 부분)

baseline OOO scheduling pipeline은 **3 cycles**: wakeup(1) + select(1) + PRF read/scoreboard(1) → execute.

- RS의 load entry에 **`RFP-inflight` 1비트** 추가. prefetch가 실행을 시작했는지를 load에게 알림.
- **설정 시점이 결정적**: packet 생성 시점에 설정하면 안 됨(port 못 얻어 drop될 수 있음). 너무 늦어도 안 됨.
  → **prefetch가 LSQ/L1 중재를 이겨 첫 L1 lookup을 시작하는 cycle**에 설정 (Addr 계산 stage 직후).
- 이 시점은 **RFP 완료 3 cycle 전**이고, **dependent의 scheduling pipeline도 3 cycles** — 이 일치는 우연이 아니다.
  ⇒ load wakeup 시점에 비트가 이미 set이면 **dependent가 실행에 도달하는 순간 RFP가 막 완료**되어 stall이 없다.

동작 분기:
| RFP-inflight 상태 | dependent wakeup |
|---|---|
| unset | 기존대로 load wakeup 후 **5 cycles**(L1 latency) 뒤 |
| load wakeup 시점에 set | **load와 back-to-back** — load가 1-cycle op인 것처럼 동작, **전체 load latency 절약** |
| load wakeup 후 늦게 set | **부분 절약** |
| RFP가 L1 miss | 투기적으로 깨운 dependent **cancel 후 하위 캐시 데이터 도착 시 재issue** |
| **predicted address 불일치** | dependent **cancel → load dispatch 시 재issue**. load는 L1을 다시 access |

> **중요**: RFP의 dependent wakeup은 **address validation보다 앞선 투기적 wakeup**이다.
> Figure 9에서 dependent의 Execute는 load의 Addr 단계 직후(첫 L1 cycle)에 위치한다.
> 저자들은 "cancel/redispatch는 이미 OOO pipeline에 존재하는 메커니즘(hit-miss speculation)이므로 새 pipeline 변경이 아니다"라고 방어한다.

**Timeliness의 근거**: trace 상 **63% of loads는 allocate 시점에 operand가 준비되지 않음** → rename 이후 launch해도 충분한 run-ahead.
나머지 37%도 scheduling pipeline **최소 3 cycles**의 여유는 있음.

### 1.5 저장 비용 (Table 1)

| 구조 | 크기 |
|---|---|
| Prefetch Table (1K~2K entry) | **6.5KB ~ 12KB** |
| Page Address Table (64 entry) | 352 bit |
| RFP-inflight (128 entry RS) | 128 bit |
| RFP FIFO queue | **64 entry** |

### 1.6 평가 결과

Baseline: Intel Tiger Lake급 — 5-wide, 4GHz, ROB 352, LQ 128, SQ 72, **IQ 125**, load port 2개, L1D 48KB/12-way/**5 cycle**, L2 1.25MB/15cy, LLC 3MB/40cy, TAGE/ITTAGE. 65개 workload.

| 항목 | 수치 |
|---|---|
| **IPC speedup (baseline)** | **3.1%** (oracle headroom 9%의 34%) |
| Coverage (useful prefetch / all loads) | **43.4%** |
| Prefetch packet **injected** | 72% of loads |
| Prefetch **executed** | 48% |
| → injected됐지만 drop | **24%** (대부분 L1 bandwidth 부족으로 늦어져 load가 먼저 출발) |
| **완전히 latency를 숨긴 비율** | **34.2%** (load dispatch 전에 prefetch 완료) |
| 부분적으로만 숨김 | 9.2% (= 43.4 − 34.2) |
| **wrong prefetch** | **~5%** of loads (extra L1 bandwidth만 소모, load latency에는 영향 없음) |
| L1 전용 port를 줄 경우 | speedup **4.0%**, coverage 57.4% (+16.1% 추가 실행) |
| Baseline-2x (10-wide, 자원 2배) | speedup **5.7%**, coverage 53.7% |
| vs Composite VP | RFP 3.08% / coverage 43.4% vs VP 2.20% / 34.2% |
| **VP + RFP 결합** | **4.15%**, coverage 54.6% (상보적) |
| EPP + Composite VP | 2.05% (EPP는 VP 단독보다 오히려 손해) |

**Sensitivity**
- **confidence counter 폭**: 1-bit → wrong 5%, 4-bit → wrong 0.7%이지만 **coverage 하락**. misprediction이 싸므로 **1-bit로 충분** (기본값)
- **PT entry 수**: 1K→16K는 미미한 개선, 16K 이상은 개선 없음
- **L1 latency 5→6 cycles**: RFP 이득 0.5%→3.6%로 증가
- **context(path-based) predictor 추가**: +0.3%뿐 → **stride만으로 충분**
- TLB miss drop: 무시할 수준 / L1 miss까지 진행: +0.02%

### 1.7 ZERECO에 직접 관련된 문장 (논문 positioning에 활용)

RFP 저자들이 **명시적으로 future work로 남긴 두 가지**가 ZERECO의 contribution과 정확히 일치한다:

1. §5.1 말미:
   > "These results highlight that some prefetches are more critical for performance and that not all prefetches have a high impact on performance. This is an important observation given the scarce L1 bandwidth. ...
   > **We leave the research on targeted prefetching for specific load instructions for future work.**"

2. §6 Other Related Work:
   > "CRISP [47] leverages criticality based scheduling to execute delinquent loads and their load slices as early as possible. Criticality based instruction scheduling policies reduce the scheduling pipeline latency for critical instructions, while RFP is complementary to these works and hides the L1 cache access latency.
   > **Intelligently combining RFP with criticality-based solutions can further reduce end-to-end pipeline latency for critical loads and their load slices, and result in even higher performance.**"

⇒ ZERECO = (targeted RF prefetch for branch-resolution-critical loads) + (criticality-based priority scheduling). **RFP 저자가 제안한 미해결 조합**을 H2P branch resolution이라는 구체적 criticality 정의로 실현한 것.

> **주의 — related work gap**: CRISP (Litz, Ayers, Ranganathan, "CRISP: Critical Slice Prefetching", ASPLOS 2022)는
> ZERECO와 가장 가까운 미확보 논문이다. `reference/`에 없으므로 확보 후 차별점을 명시해야 한다.

### 1.8 Scarab 구현 시 결정적 gap

현재 Scarab의 load wakeup은 **비투기적**이다.
- `dcache_stage.c`에서 실제 캐시 접근 결과로 `done_cycle`이 정해진 뒤 `wake_up_ops()`가 호출됨
- **hit-miss predictor 없음**, dependent의 speculative wakeup 없음
- `op->replay` / `replay_cycle` / `replay_count` 필드는 `op.h`에 존재하나 **core 코드 어디에서도 TRUE로 설정하지 않음** (vestigial)

함의:
1. RFP의 `RFP-inflight` 기반 조기 wakeup은 Scarab에 **그대로 이식할 수 없다.** 대신 `done_cycle`을 조정하는 형태로 근사해야 한다.
2. 현재 oracle(`dcache_stage_try_main_chain_load_oracle`)은 demand load가 dcache stage에 도착한 뒤 1 cycle에 완료시키므로
   baseline(`DCACHE_CYCLES`=5 등) 대비 L1 접근 latency를 절약한다. 이는 **timely RFP의 이득과 근사적으로 동등**하다.
3. **wrong-address 시 dependent cancel/reissue 비용을 모델링하려면 speculative wakeup + replay 인프라를 새로 만들어야 한다.**
   RFP 자체가 wrong prefetch를 "load latency에 영향 없음, extra L1 bandwidth만 소모"로 규정하므로,
   ZERECO도 **validate-then-use(보수적)** 설계를 택하면 이 인프라 없이도 정직한 모델이 된다.

---

## 2. TEA — "Timely, Efficient, and Accurate Branch Precomputation" (MICRO 2024)

Deshmukh, Cai, Patt (UT Austin). pp. 480–492. **Scarab + Ramulator + McPAT로 평가** (우리와 동일 시뮬레이터).
ZERECO는 TEA의 **identification 하드웨어만** 차용하고 helper thread는 버린다.

### 2.1 핵심 통찰 — timeliness 제약을 완화한다

기존 precomputation의 timeliness 제약은 *"branch가 fetch되기 전에 결과가 도착해야 한다"*(그래야 BP를 override)였다. 이는 일반적으로 달성 불가능하므로 선행 연구는 단순한 control flow의 branch만 노렸고 coverage가 붕괴했다.

TEA의 전환: **branch가 fetch된 후라도 main-thread branch의 실행이 끝나기 전에만 도착하면 유용하다** — override 대신 **early misprediction flush**를 트리거하면 되기 때문이다. 이 한 번의 완화로:
- 수천 instruction에 걸친 **긴 chain**을 복잡한 control flow를 넘어 추적 가능
- **99.3% precomputation accuracy** (chain을 속도 때문에 잘라낼 필요가 없어짐)
- **76% misprediction coverage**
- **on-core 실행** 가능 (별도 engine 불필요)

**결과: 8-wide OoO baseline 대비 geomean IPC +10.1%**

### 2.2 H2P Branch Table (HBT) — §IV-B

| 속성 | 값 |
|---|---|
| Entry 수 | **256** (8-way set associative → 32 set) |
| Index | branch PC |
| 상태 | **3-bit saturating counter** + tag |
| 저장공간 | **0.2 KB**, 1-cycle access |
| 추적 대상 | **direction과 target misprediction 모두** (direct/indirect 모두) |

- **할당**: branch가 misprediction할 때 생성, counter를 **1로 초기화**
- **분류 조건**: entry가 있고 **counter > 1** (= 최소 2회 misprediction 관측)
- **Decay: 50,000 instruction마다 모든 counter를 1씩 감소** — 즉 **0.02 MPKI 미만**의 branch는 0으로 수렴
- **교체**: counter == 0인 entry 우선

**튜닝 지침 (중요)**: decrement 주기를 sweep한 결과 — *"Marking more branches as H2P improves misprediction coverage and provides better performance. This begins to drop off only when highly accurate branches are marked as H2P as it starts to hurt timeliness significantly."*
⇒ HBT는 **관대하게(permissive)** 설정하는 것이 유리하다.

**중요**: 모든 H2P branch와 그 모든 dynamic instance의 chain이 **단 하나의 Source List로 한 번의 walk에서 동시에 추적**된다. 병렬 chain tracker가 필요 없다.

> ⚠️ **저장소 불일치**: `CLAUDE.md`는 Scarab HBT(`src/bp/hbt.h/c`)를 **1024-entry**로 기술하나 논문은 **256 entry 8-way**다. 3-bit counter와 `>1` threshold는 동일. 비용을 보고할 때 정합시켜야 한다.

### 2.3 Fill Buffer — §IV-C

| 속성 | 값 |
|---|---|
| Entry 수 | **512** |
| 단위 | **micro-op** (x86이므로; fixed-length ISA면 instruction 단위로 충분) |
| Entry 크기 | **16 B** → 총 **8 KB** |
| 포트 | **단일 access port** |
| 채우는 시점 | **Retire 시, program order** |

**Entry 필드**: valid bit / **decoded uop bytes** (읽고 쓰는 register와 **해석된 실제 memory address** 포함) / PC / **chain-bit**

**Walk 동작:**
- **Trigger: Fill Buffer가 가득 차면** youngest부터 walk 시작
- **전용 state machine**이 수행, **약 500 cycle** 소요 (512 entry / 단일 포트 ≈ 1 entry/cycle)
- **종료**: Fill Buffer의 oldest entry에 도달할 때까지
- **Walk 중 retire된 instruction은 폐기된다** — TEA는 retired stream의 **샘플링** 식별자이지 완전한 식별자가 아니다
- **민감도**: *"performance is not very sensitive to the duration of the Backward Dataflow Walk"*, *"The Fill Buffer size does not affect performance significantly (~1% change)"*
  ⇒ **Fill Buffer를 줄이고 walk를 느리게 해도 거의 무료.** chain 깊이는 window 크기가 아니라 **walk 반복 횟수**에서 온다.

### 2.4 Backward Dataflow Walk 알고리즘 — §III-A

**Source List = walk의 유일한 상태**, 두 부분으로 구성:
- **register 성분**: architectural register 개수만큼의 **bit vector** (x86에서는 RFLAGS도 1급 항목)
- **memory 성분**: **16-entry memory address buffer**

```
for i = youngest downto oldest:
    u = FillBuffer[i]
    if u.chain_bit == 1:              # H2P branch 또는 직전 pass에서 표시된 uop
        SourceList += u.sources
        continue
    if u.dst_reg ∈ SL.regs or u.mem_addr ∈ SL.mem:
        u.chain_bit = 1               # chain member로 표시
        SourceList -= u.dst
        SourceList += u.sources       # register와 memory address 모두
```

**결정적 semantics — load는 chain head가 아니다.**
`mov r1,[r4+r5]`가 chain에 들어가면 **address 생성 register(R4, R5)와 memory location `[R4+R5]` 자체를 모두** Source List에 넣는다. Walk는 그 주소에 쓴 **store를 계속 찾아 올라간다.** 이것이 TEA가 함수 호출 경계를 넘을 수 있는 이유다(IBDA는 불가능).

**Memory dependence 해소 (§III-D)**: store→load 매칭은 Fill Buffer에 기록된 **실제 retired effective address**로 수행한다. 동기: 함수 본문의 H2P branch는 입력 변수가 **push/pop을 통해 메모리로 전달**되므로, memory dependence 없이는 chain이 call 경계에서 끊긴다.
저자들이 밝힌 단점: *"memory addresses corresponding to correlated load-store pairs can change over time. Thus, incorporating memory dependencies sometimes increases the precomputation thread size without improving its accuracy."*

**Loop-carried dependence는 특별 처리가 없다.** `add r5,#1`이 R5를 생산·소비하면 bit vector에서 R5가 계속 live로 남고, walk가 buffer 안의 모든 이전 dynamic instance를 자연히 집어낸다.

### 2.5 🔴 Multi-pass chain 성장 — §III-C, Fig. 2

chain-bit는 **fill 시점에 미리 세팅**되기도 한다. 두 경우: (a) HBT가 H2P라고 답한 branch, (b) **직전에 TEA thread로 실행된 uop**.
(b) 덕분에 512-entry window를 벗어난다.

| Pass | BDW 시작점 | 발견된 chain |
|---|---|---|
| 1회차 | H2P `D5 jne loop`만 | `D3 D4 D5` (3 uop) |
| 2회차 | `D3`, `D4` (사전 표시됨) | `B3 C4 D3 D4 D5` |
| 3회차 | `B3`, `C4` | `A0 A1 B3 C4 D3 D4 D5` |

> *"The TEA thread thus contains dependence chains that span **thousands of instructions** even though the Fill Buffer can only hold 512 instructions."*

**pass마다 dataflow 한 단계씩 깊어진다.** Walk 비용은 pass당 O(512)로 고정.

**Aging**: chain-bit가 지속되므로 노화가 필요하다 — **Block Cache의 bit-mask를 500K instruction마다 리셋**하는 것이 최적이었다.

### 2.6 Block Cache — §III-E, §IV-C

| 속성 | 값 |
|---|---|
| Entry | **512** + **256 "zero-tag"** (tag 전용) |
| Associativity | 8-way, **19 KB** |
| Tag | **40-bit** = basic-block segment의 첫 instruction PC |
| Data | decoded chain uop **평균 4 B**, entry당 **최대 8 uop** |
| Bit-mask | **32 bit** |
| Banking | **2 bank**, I-cache처럼 연속 cache line을 분산 |

**Zero-tag의 존재 이유**: Block Cache **miss는 TEA thread를 종료시킨다.** Zero-tag가 없으면 경로상의 정상적으로 비어 있는 basic block이 miss로 보여 thread를 죽인다. Zero-tag는 *"이 block은 비어 있음이 확인됐다, 더 뒤에 chain uop이 있을 수 있으니 종료하지 마라"*는 표시다.

**Multi-path bit-mask OR (§III-E)**: 같은 basic block이 서로 다른 경로에서 서로 다른 chain을 만들면 **bitwise OR로 합친다** (경로 A-B-D는 `1000`, A-C-D는 `0100` → 저장은 `1100`).
근거: *"Saving the longest or the most recent dependence chain produces incorrect results when the saved dependence chain does not match the actual control flow... Saving all possible dependence chain versions is not viable in terms of storage as each additional branch... exponentially increases the number of possible chains."*
비용: 각 경로에 불필요한 instruction이 하나씩 추가되어 **timeliness가 나빠진다.**

**Frontend lookup**: decoupled BP의 fetch address가 **Block Cache와 I-cache로 병렬 전송**된다. Hit이면 TEA thread 시작. 읽어낸 uop은 rotate 후 **shadow Rename으로 직행**(Block Cache가 decoded uop을 저장하므로 decode 단계 없음).

### 2.7 🔴 ZERECO가 그대로 재사용할 datapath

> **bit-mask는 작은 queue에 push되어 main thread로 전달되고, main thread는 그 mask로 자기 in-flight instruction에 "이 uop은 TEA chain에 속한다"고 표시한다** (§IV-C, §IV-D).

**이 경로가 곧 ZERECO의 "main-thread op에 metadata 부착"이다.** 이미 명세되고 비용이 산정되어 있다.
TEA는 이 표시를 다음 BDW의 추가 시작점으로 쓰지만, ZERECO는 같은 전달 메커니즘으로 RF-prefetch/priority 태그를 붙이면 된다.

⚠️ **단, 검증 필요**: TEA에서 "이미 chain에 속함"의 의미는 *"helper thread로 실행되었다"*이다. Helper thread가 없는 ZERECO에서는 *"Block Cache lookup에서 태그되었다"*로 바뀐다. 같은 Block-Cache-hit 이벤트이므로 동작하지만, **Fig. 2의 다단계 성장 성질이 보존되는지 확인해야 한다.**

### 2.8 백엔드 공유 — §IV-E (ZERECO가 버리는 부분)

- **192 RS + 192 physical register를 TEA thread에 예약** (baseline 352 RS / 400 PR 중) → 활성 시 main thread에는 160 RS / 208 PR만 남음
- Issue는 8-wide이고 **TEA를 엄격히 우선**한다
- **TEA uop은 ROB에 들어가지 않는다.** RS entry의 1비트로 식별하고 실행이 끝나면 폐기
- ROB 없이 PR을 해제하기 위해 **PR map table (400 PR × (Valid 1b + RefCount 5b) = 2400 bit)** 추가. 5비트라 **31명 초과 reader면 overflow → 잘못된 precomputation** (드물다고 인정)
- **TEA store는 D-cache에 쓸 수 없다** → **16-entry × 32B store data cache** (512 B)

**우선순위 정당화 (ZERECO가 그대로 인용 가능):**
> *"H2P branch mispredictions are almost always on the critical path of execution, even for applications with relatively lower branch MPKI. Prioritizing their execution, even at the cost of other instructions, provides better performance."*

**TEA load 처리 (RFP 설계에 참고할 선례):**
> *"This includes TEA thread loads as it is similar to a prefetch that speculatively brings data into the D-cache. No ordering needs to be enforced on the loads and they are **not allocated Load Queue entries**. ... TEA thread loads that miss in the D-cache **carry their destination PR in the MSHR entry**."*

⇒ TEA load는 prefetch가 아니라 **physical register에 쓰는 실제 load**이되 LQ 할당과 memory ordering에서 해방된다. MSHR이 destination PR을 실어 나른다. ZERECO의 RFP가 LQ entry 없이 PR에 write할 때 참고할 선례.

### 2.9 Early misprediction flush — §IV-F

핵심은 **timestamp 동기화**다. TEA branch는 main BP가 부여한 branch-ID/timestamp를 **상속**하므로, queue도 scan도 BP override port도 필요 없다. 기존 flush 기계가 timestamp로 인덱싱하므로 **direction과 target 모두**, **out-of-order/nested resolution**까지 공짜로 지원된다.

- **Case 1 (main branch가 backend에 있음)**: 통상적인 partial flush. 복구된 RAT를 **main RAT과 shadow RAT 양쪽에 복사**
- **Case 2 (main branch가 아직 frontend에 있음)**: TEA가 너무 앞서간 경우. **각 frontend stage 앞에 comparator를 추가**해 timestamp로 younger만 죽인다. **Fetch Queue도 부분 flush되며, 이때 그 branch의 misprediction penalty를 전부 절약한다.** main RAT은 복구 불필요, **shadow RAT만 checkpoint**해서 복구

**검증 훅**: TEA branch가 resolve되면 대응하는 main branch의 **in-flight branch queue entry에 precomputed direction/target을 기록**한다. Main branch가 나중에 실행을 마치면 그 entry를 읽어 TEA 계산이 맞았는지 확인하고, 틀렸으면 두 번째 flush를 낸다 (**< 0.05%**, 추가 flush **< 0.001 PKI**).

### 2.10 Poison bit — §IV-G

**main RAT에 architectural register당 1비트** 추가.
- TEA thread 시작 시 전부 0
- chain에 **속하지 않은** main-thread instruction이 쓰는 AR → poison **set**
- chain에 **속한** main-thread instruction이 쓰는 AR → poison **clear**
- **위반**: chain에 속한 instruction이 **poisoned register를 읽으면** TEA의 chain이 틀렸음이 증명된다

근거: *"reading from a poisoned register means that the dependence chain instruction needed a result produced by a non-dependence chain instruction (after the TEA thread started), which is incorrect by definition."*

대응: 아직 실행되지 않았고 위반 instruction보다 younger인 TEA branch는 **flush 트리거를 금지**하고, 나머지는 **점진적으로 drain**한다.

### 2.11 비용 — Table I/II, §IV-H

**Baseline core**: 3.2 GHz, **8-wide issue**, FE latency **12 cycle**, **ROB 512**, **RS 352**, retire 16-wide, **12 execution port** (6 ALU, 2 LD, 2 LD/ST, 2 FP), **PR 400**, LQ 256, SQ 192, **64KB TAGE-SC-L**, Fetch Queue **128**, BTB 4K, L1I 32KB/4cy, **L1D 48KB 12-way/4cy**, **LLC 1MB/18cy**, DDR4-2400R.
**최소 fetch-to-resolution latency = 15 cycle.**

**추가 SRAM ≈ 27.9 KB**: Block Cache 19KB + Fill Buffer 8KB + HBT 0.2KB + store data cache 0.5KB + PR map 0.3KB + shadow RAT

| 항목 | 값 |
|---|---|
| **면적** | 코어 전체의 **~3.5%** (그중 2%p 이상이 Block Cache + Fill Buffer) |
| **Peak power** | **+8.5%** (TEA frontend가 6%p 이상 차지) |
| **Energy** | **−2%** (실행 시간·wrong-path fetch 감소가 상쇄) |
| **Dynamic instruction 증가** | **+31.9%** (Slipstream +85%, Branch Runahead +34%와 비교) |

비교 기준: *"a true 16-wide OoO core costs ~10% more area for only 2.8% performance."*

### 2.12 평가 결과

**환경**: Scarab(x86-64) + Ramulator + McPAT, **SPEC CPU2017 ref + GAP(g=19, n=300)**, **MPKI < 0.5 벤치마크 제외**(FP 5개 제거) → 18개, **SimPoint 최대 5개/벤치, 200M instruction + 200M warmup**.

**성능 (Fig. 5): geomean +10.1%.** 최고 bfs +27.8%, cc +24.5%, mcf +19.4%, tc +18.9%. 최저 x264 +0.7%.

**승자의 이유 (§V-B) — ZERECO에 매우 중요:**
> *"mcf, bfs, cc and tc all have many mispredictions, most of which are on the critical path... **They also have many loads that are guarded by H2P branches and hit in the LLC or go out to main memory. Resolving these branches early allows correct path loads to begin execution sooner and improves the amount of memory level parallelism.**"*
> *"perlbench and nab do not have a high branch MPKI but show substantial improvement as they have **many long latency loads in the shadow of a few H2P branches**."*

**Coverage 분해 (Fig. 7, 전체 misprediction 대비 평균):** Early **76.5%** / Late ~8% / Incorrect ~6% / **chain 없음 ~9.5%**
"chain 없음"은 **Block Cache 용량 한계**다 — deepsjeng 27%, perlbench 24%, x264 22.5%, gcc 20%.

**Timeliness (Fig. 10c): branch당 평균 22 cycle의 misprediction penalty 절약.** 최대 pr 67.5, nab 59, bc 46.5 / 최소 tc 5, leela 6.5.
**"TEA thread branch의 ~76%가 최소 1 cycle 이상 절약"**, **"1% 미만(0.7%)만 잘못 계산"**.

**vs Branch Runahead (Fig. 8): TEA 10.1% vs BR 7.3%.**
복잡한 control flow에서 TEA가 압도(perlbench 6.3 vs 0.3, nab 10.4 vs 0.1, xalancbmk 10.0 vs 1.2). 단순 control flow(GAP)에서는 BR이 sssp(20.7 vs 13.0), bc(12.3 vs 9.4), bfs(29.2 vs 27.8)로 우세.
BR이 이기는 이유: **merge-point prediction으로 loop 내 독립 branch를 식별**해 여러 instance를 병렬 issue할 수 있다. TEA는 main BP를 따르므로 불가능.

**Feature ablation (Fig. 10):**
| 구성 | Accuracy | Coverage |
|---|---|---|
| TEA 전체 | **99.3%** | **76%** |
| − only loops (BR식 loop 한정) | 94.8% | 62.5% |
| − no masks (multi-path OR 제거) | 92.4% | 66% |
| − no mem (memory dependence 제거) | 95.4% | 70.5% |
| **모든 feature 제거** | — | **39%** |
| BR thread | 83.7% | **29%** |

⇒ **multi-path mask가 가장 중요하고 memory dependence가 가장 덜 중요하다.**

**On-core vs 전용 engine (Fig. 9): 10.1% vs 12.8%.** 전용 engine(192 RS + 192 PR + **16 전용 execution unit**)의 이득이 면적/전력 대비 크지 않다고 판단. 백엔드를 main core만큼 키워도 12.8%로 동일.

### 2.13 🔴 ZERECO에 대한 직접적 위협 두 가지

**(1) TEA는 prefetch 부수효과를 측정해서 1.2%밖에 안 된다고 보고했다 (§V-B):**
> *"The TEA thread also has the side-effect of prefetching loads that are part of H2P branch dependence chains. We turned off early resolution in the TEA thread to measure how much this skews performance, and **it only provides an overall 1.2% performance gain**."*

**반박 논거 (반드시 논문에 명시할 것):**
- 그것은 **D-cache 레벨 prefetch**이지 **register-file prefetch가 아니다.** ZERECO는 값을 **issue 이전에 physical register로** 전달해 AGU→L1→writeback 구간을 main-thread chain에서 제거한다. TEA의 1.2%는 그 효과의 상한이 아니다.
- 그 측정은 **TEA thread의 frontend/backend 비용을 전부 지불한 상태**에서 이루어졌다(+31.9% dynamic instruction, 192 RS/PR 분할, +8.5% peak power). ZERECO는 그 비용을 내지 않는다.
- RFP 논문의 L1-hit 92.8% 관찰과 결합하면, D-cache prefetch가 못 줄이는 L1 hit latency가 정확히 ZERECO의 표적이다.

**(2) TEA는 CRISP의 priority scheduling을 명시적으로 기각했다 (§II):**
> CRISP [19]는 *"identifies H2P branch and long latency load dependence chains on the program's critical path and prioritizes their execution in the backend. **This provides limited benefit as it only allows dependence chains to be scheduled to the execution units a few cycles earlier.**"*

⇒ ZERECO의 P-IQ 기여를 "chain op을 몇 cycle 일찍 issue한다"로 서술하면 **TEA 저자가 이미 무의미하다고 라벨링한 것**이 된다.
**P-IQ는 반드시 secondary/enabler로 위치시키고, primary는 RF prefetch가 load를 chain critical path에서 통째로 제거하는 것(수십 cycle 규모)이어야 한다.**

> ⚠️ **2026-08-11 연구자 판단**: 이 1.2% 수치는 **실제 시뮬레이션 결과 사실이 아님을 확인했다** (증명 자료는 추후 제시 예정).
> 그리고 ZERECO가 TEA를 공략하는 주된 축은 성능이 아니라 **complexity**다 — helper thread frontend(shadow FTQ/Fetch/Rename/RAT), 192 RS+192 PR 정적 예약, ROB 없는 PR 해제를 위한 PR map queue와 5-bit refcount overflow 위험, store data cache, poison bit, frontend 각 stage의 timestamp comparator, +31.9% dynamic instruction, +8.5% peak power를 **전부 제거**하고 identification 하드웨어(≈27.2KB)만 남긴다는 것이 논지다.

**긍정적 프레이밍**: TEA의 timeliness 지표는 **branch당 평균 22 cycle 절약**이다. LLC가 18 cycle, frontend가 12 cycle, 최소 fetch-to-resolution이 15 cycle이므로, **RF prefetch로 LLC hit 하나를 chain에서 제거하면 TEA의 branch당 전체 절약과 같은 자릿수**다 — helper thread 없이, 192 RS/PR 예약 없이, +31.9% dynamic instruction 없이, flush 기계 없이.

### 2.14 TEA가 답하지 않아 ZERECO가 측정해야 하는 것

- **논문에 "Target Load"라는 용어가 없고 chain 구성 분해(load 비율, chain 길이 분포)가 전혀 없다.** 직접 측정해야 한다.
  간접 증거는 강하다: 모든 예시가 load-headed이고(Fig. 2의 7-uop chain 중 load 2개 ≈ 29%), §V-B가 "LLC/memory로 가는 load", "long latency loads in the shadow of a few H2P branches"를 승리 요인으로 든다. IBDA는 *"single load, followed by a few arithmetic operations, leading up to a branch"* 형태만 다룬다고 기술된다.
- **chain 안에서 Target Load를 구분하는 방법.** TEA의 32-bit mask는 chain 멤버를 균일하게 표시할 뿐이다. ZERECO는 "latency를 지배하는 load"와 "chain 안의 ALU op"를 구분할 추가 비트/필드가 필요하다.
- **helper thread 없는 chain-bit pre-seeding의 성장 성질** (§2.7 경고 참조).
- Chain 길이 관련해 논문이 주는 유일한 수치는 **TEA thread가 전체 dynamic instruction의 31.19%** (feature 제거 시 21%)이고, benchmark별로는 nab 10.25% ~ sssp 56.79%다.

### 2.15 ZERECO가 TEA에서 가져오는 것과 버리는 것

**가져옴 (총 ≈27.2 KB, 코어 면적 ~2%):**
| 필요 | TEA 구조 | 비용 |
|---|---|---|
| H2P 식별 | HBT 256-entry 8-way, 3-bit, threshold>1, 50k decay | 0.2 KB |
| Retired window | Fill Buffer 512 uop × 16 B, 단일 포트 | 8 KB |
| Slice 추출 | BDW state machine ~500cy, AR bit-vector + 16-entry addr buffer | 로직 미미 |
| BB 단위 slice 저장 | Block Cache 512 + 256 zero-tag, 8-way, 40b tag, 32b mask | 19 KB |
| **main-thread op 태깅** | **bit-mask → 소형 queue → main thread 표시** | 이미 존재 |

**버림**: shadow FTQ, TEA Fetch, TEA Rename, Shadow RAT, PR map queue(2400b), store data cache(512B), **192 RS + 192 PR 예약**, **+31.9% dynamic instruction**, **TEA frontend의 6%p 전력**, poison bit, early-flush comparator.

## 3. PUBS — "Performance Improvement by Prioritizing the Issue of the Instructions in Unconfident Branch Slices" (MICRO 2018)

Hideki Ando (Nagoya Univ.), 단독 저자. MICRO-51 pp. 82–94. ZERECO P-IQ의 직접적 선행 연구.

### 3.1 한 줄 요약

PUBS는 **select logic을 전혀 수정하지 않는다.** 통합 IQ 64 entry 중 **head에 가장 가까운 물리적 6개 entry**를 priority entry로 예약하고, unconfident branch slice에 속한 instruction을 그 entry에 dispatch한다. 기존 select logic이 이미 **position-based fixed priority**(head에 가까울수록 우선)이므로, 예약 자체가 곧 우선순위가 된다. IQ 측 하드웨어 변경은 **free list를 둘로 쪼갠 것이 전부**이며, IQ entry당 추가 비트는 **0**이다.

### 3.2 🔴 결정적 발견 — PUBS의 baseline은 random queue다

Sec. III-B1은 IQ 조직을 shifting / circular / random 세 가지로 분류하고 앞의 둘은 현대 프로세서에서 쓰이지 않는다고 정리한 뒤 다음과 같이 명시한다.

> "although all processor vendors do not publish their IQ organization, **the random queue alone or with an age matrix are used in modern processors** [11]–[13].
> **In this paper, we assume a random queue without an age matrix as the base organization, and compare the performance of a processor with our scheme to this.**"

즉 P-IQ의 정통 선행 연구가 **random queue를 정당한 현대 baseline으로 채택**했다.
⇒ ZERECO도 **모든 arm을 random-queue로 통일**하면 baseline을 섞지 않고도 P-IQ 효과를 보일 수 있다. Methodology를 훼손하지 않는 정당한 인용 근거.

Random queue를 쓰는 이유도 논문이 직접 설명한다: shifting queue는 age 순서라 criticality와 select 우선순위가 일치해 IPC가 높지만 compaction 회로가 IQ critical path에 들어가 delay가 크게 늘어 **더 이상 쓰이지 않는다**. Random queue는 "hole"에 그냥 채워 넣으므로 단순·저지연이지만 **issue priority가 무작위로 주어진다**.

### 3.3 Age matrix와의 비교 (oldest-first 반론에 대한 PUBS의 답)

PUBS는 §V-G에서 age matrix(AGE)와 직접 비교한다. **단, 이 age matrix는 full oldest-first가 아니다**:

> "It picks **a single oldest instruction** from ready instructions. This instruction is given the highest priority; **the other instructions to be issued are selected using the conventional select logic.**"

IW=4에서 4개 grant 중 **1개만** age 순, 나머지 3개는 position 기반이다.

| Configuration | D-BP geometric-mean IPC 증가 |
|---|---|
| PUBS (random queue) | **7.8%** |
| AGE (random queue + age matrix) | 6.5% |
| **PUBS + AGE** | **10.2%** |

⇒ **age matrix 위에서 PUBS의 marginal 이득 = 3.7pp** (7.8%의 절반 이하). 리뷰어가 쓸 숫자.

PUBS는 age matrix가 branch slice를 이미 우대함을 인정한다:
> "The effectiveness is larger in D-BP than in E-BP ... even though the age matrix does not consider branch misprediction. **This infers that branch slices often include an oldest ready instruction.**"

PUBS의 최종 방어는 IPC가 아니라 **delay**다. 16nm PTM HSPICE 트랜지스터 레벨 설계로 age matrix가 **IQ delay를 13% 증가**시킴을 보이고, 이를 clock period에 반영하면 PUBS가 AGE 대비 D-BP에서 11.1% 앞선다고 주장한다. 다만 상용 프로세서들이 실제로 age matrix를 쓴다는 사실은 정직하게 인정한다.

### 3.4 🔴 Stall vs Non-stall — ZERECO 실험과 정반대 결과

PUBS의 **기본 정책은 strict stall**이다 (Table II: "stall if no priority entry is available"). 그리고 둘을 직접 비교했다 (Fig. 10):

| Priority entries | **Stall** | Non-stall |
|---|---|---|
| 2 | **−3% (baseline보다 손해)** | +1.7% |
| 4 | +6.3% | +2.3% |
| **6 (최적)** | **+7.8%** | +3.0% |
| 8 | +7.2% | +3.2% |

> "In the case of the non-stall policy, although the dispatch is not stalled even if the priority entry is not available, **prioritizing the unconfident branch slice instructions is opportunistic, and is thus carried out only partially. As found in the evaluation results, the negative effect is stronger, and the stall policy is hence better.**"

**ZERECO 실험은 non-stall이 더 좋다는 반대 결과를 얻었다. 이 불일치는 반드시 설명해야 한다.**
가장 설득력 있는 설명은 **IQ 조직의 차이**다:
- PUBS는 **unified IQ 64 entry**. Priority 6개가 차도 전체 IQ 관점에서 국소적 사건이다.
- ZERECO는 **distributed RS bank**. 특정 bank의 priority partition이 full이면 in-order dispatch가 막혀 **전체 frontend가 정지**한다. Bank imbalance 때문에 stall 확률이 훨씬 높다.
- 추가로 ZERECO의 H2P population과 P-IQ ratio(15%)가 PUBS(9.4%)와 다르다.

또한 stall 정책은 **취약하다**: 6→2로 undersize하면 +7.8%에서 −3%로 10pp 이상 급락한다. Non-stall은 단조롭고 절대 음수가 되지 않지만 3% 근처에서 포화한다.

### 3.5 🔴 PUBS는 distributed IQ를 평가한 적이 없다

Sec. III-C2 전체가 다음 한 문장이다:
> "our PUBS scheme can be applied to a distributed IQ, where each IQ is partitioned into priority and normal entries."

**데이터 0, sizing 0, stall 동작 분석 0.** ⇒ ZERECO의 distributed finite P-IQ는 PUBS 대비 **실제로 미개척 영역**이며, 이것이 P-IQ 쪽 novelty의 핵심 근거가 된다.

### 3.6 Slice 식별 방식 — backward walk가 아니다

PUBS는 **decode 단계에서 세 개의 PC-indexed table**로 점진적 학습을 한다. Backward walk도, rename-time slice bit propagation도 아니다.

| Table | Index | 내용 |
|---|---|---|
| `def_tab` (64행, tagless) | **logical destination register 번호** | 그 register를 마지막으로 쓴 instruction의 PC |
| `brslice_tab` (128-set 8-way, 8b hashed tag) | slice instruction의 PC | 연관된 branch의 `conf_tab` 포인터 |
| `conf_tab` (128-set 8-way, 4b hashed tag) | branch PC | **6-bit saturating resetting counter** |

링크 구축: branch decode 시 `def_tab`에서 producer PC(A)를 얻어 `brslice_tab[PC_A] = PC_br` 기록. 다음에 A가 decode되면 `brslice_tab[PC_A]`에서 `PC_br`을 읽고 A의 producer B에 대해 `brslice_tab[PC_B] = PC_br` 기록. 반복.

**중요한 귀결 (논문이 명시하지 않은 것 포함):**
- **깊이가 dynamic occurrence마다 한 단계씩만 자란다.** 깊이 k slice를 완성하려면 그 코드가 ~k번 실행돼야 한다. 루프에는 잘 맞고 cold code에는 안 맞는다.
- **membership이 static PC 단위**라 그 PC의 모든 dynamic instance가 우선순위를 받는다.
- **instruction당 branch 포인터가 하나** — 여러 branch에 기여하는 producer는 마지막 것만 남는다.
- **memory dependence를 전혀 다루지 않는다.** `def_tab`이 logical register로만 indexing되므로 slice는 **load에서 끊긴다.** 논문에 memory dependence 언급 자체가 없다.
  ⇒ ZERECO가 store→load edge를 다루는 것은 명확한 차별점.
- wrong-path decode가 `def_tab`을 오염시키는 문제를 다루지 않는다.

### 3.7 Confidence 추정 — 극도로 coarse

- 별도 `conf_tab` 사용 (branch predictor 내부 confidence 재사용 아님 → predictor-agnostic)
- **6-bit resetting counter.** 예측 적중 시 +1, **한 번이라도 틀리면 0으로 리셋.** counter == 63(최댓값)일 때만 confident.
  ⇒ **63번 연속 적중해야 우선순위에서 빠진다.**
- 결과: **전체 dynamic branch의 71%가 unconfident로 분류**된다 (6-bit 기준).
- Sensitivity (Fig. 11): 2b→3.4%(28%), 4b→7.0%(52%), **6b→7.8%(71%)**, 8b→7.8%(82%), **"blind"(모든 branch를 unconfident 취급, conf_tab 제거)→6.8%(100%)**

> **PUBS는 사실상 H2P 메커니즘이 아니다.** "branch slice instruction은 일반적으로 더 critical하다"는 coarse한 명제에 약한 confidence 필터를 얹은 것이다. conf_tab 전체의 기여가 7.8% 중 1pp에 불과하다.
> ⇒ ZERECO가 진짜 H2P identifier(소수의 static branch만)를 쓰는 것은 명확한 차별점이다. 단, PUBS의 sensitivity는 **coverage가 클수록 좋다**는 방향이므로, "왜 ZERECO에서는 precision이 도움이 되는가"에 답해야 한다. 답: **per-bank P-IQ capacity가 작아 precision이 더 중요해진다.**

### 3.8 🔴 Mode switch — reserved capacity가 memory-bound workload를 망친다

- **LLC MPKI를 10,000 cycle 간격으로 감시하여 3.0 이상이면 PUBS 자체를 비활성화**한다.
- 비활성화 시 두 free list를 entry 비율로 가중한 난수로 선택해 partition을 통계적으로 해소한다.
- 근거: "reducing branch misprediction penalty is less important in a situation where LLC misses occur frequently, because the LLC miss penalty is huge (hundreds of cycles)."
- **효과 (Fig. 12): mode switch를 끄면 `mcf` ≈−3%, `soplex` ≈−2%로 회귀한다.**

> **64 entry 중 6개(9.4%)만 예약해도 memory-bound workload에서 실제 성능 손실이 발생**했고, PUBS는 이를 막기 위해 동적 비활성화가 필요했다.
> ZERECO는 **15%를 per-bank로 예약**하므로 노출이 더 크다. 같은 실패 모드를 예상하고 대비해야 한다.

### 3.9 Priority scheduling으로는 memory latency를 없앨 수 없다 — PUBS 스스로의 인정

이것이 ZERECO의 RF prefetch를 정당화하는 가장 강력한 인용이다.

- §III-B3: 위 mode switch 근거 문장
- §V-B: "the frequency of LLC misses is the strongest factor. The higher this frequency is, **the more computation slices become critical**, because the LLC miss penalty is very long (300 cycles)."
- Fig. 9 산점도: memory-intensive(파란 점)가 같은 branch MPKI에서 체계적으로 낮은 speedup
- PUBS는 slice의 load를 가속하거나 prefetch하는 어떤 메커니즘도 제안하지 않는다.

> PUBS의 입장은 **"slice가 memory-bound면 priority scheduling은 무용하니 꺼버린다"** 이다.
> ZERECO는 정확히 그 구멍을 RF prefetch로 메운다. PUBS의 mode-switch 절이 인용처다.

### 3.10 평가 환경과 결과

**Simulator: SimpleScalar 3.0a, Alpha ISA.** SPEC CPU2006 (wrf 제외), gcc 4.5.3 -O3, ref input, 16B skip 후 100M instruction.
Branch MPKI ≥ 3.0인 프로그램을 **D-BP**(difficult), 나머지를 **E-BP**로 분류. D-BP 9개: astar, bzip2, gobmk, mcf, omnetpp, perlbench, sjeng, bwaves, soplex.

**Core (ARM Cortex-A72 기반, 의도적으로 mobile급):** 4-wide, ROB 128, **IQ 64**, LSQ 64, PRF 128+128, perceptron BP(34-bit history, 256-entry weight table), **misprediction state recovery 10 cycles**, FU: 2 iALU / 1 iMULT / 2 Ld-St / 2 FPU, L1D 32KB 2-port **2-cycle**, **L2(=LLC) 2MB 12-cycle**, memory **300 cycle**, stream prefetcher(L2로) 포함.

**주요 결과:** D-BP geometric mean **+7.8%**, E-BP는 부작용 없음(≈0.5%). 최대 sjeng **+19.2%**, 최소 mcf **+0.3%**.

**Processor size scaling (Fig. 16):** small→huge로 갈수록 PUBS 효과 증가 (≈4% → ≈15%). 이유: "**The larger the window size ... the more the issue conflicts occur**, thereby increasing the effectiveness of PUBS and AGE. In contrast, the more the issue width and the number of function units are increased, the less issue conflicts occur."
⇒ 이것이 "이득이 진짜 select contention에서 온다"는 가장 강한 간접 증거다.

**동일 면적을 branch predictor에 투자한 경우 (Fig. 13):** 4.0KB 대신 8.4KB를 predictor에 더 써도 GM ≈2%뿐 (PUBS 7.8%). "PUBS is worth introducing for more reasons than just increasing the branch predictor."

**하드웨어 비용 (Table III): 총 4.0KB** (`def_tab` 0.1 + `brslice_tab` 2.5 + `conf_tab` 1.4). **IQ entry당 추가 비트 0.**

### 3.11 ⚠️ PUBS가 priority-bit 방식을 선제 공격한다 (§III-C1)

> "To the best of our knowledge, such a select logic has not been proposed in the literature, and **we believe that it would be very difficult to implement without an extraordinary breakthrough.** One possible but straightforward implementation is that many IQS-to-1 MUXes are placed between the wakeup logic and conventional position-based select logic... **the huge fan-out of request signals and huge fan-in of MUXes significantly increase the delay, and thus it is unpractical.**"

⇒ ZERECO가 P-IQ를 **별도 물리 partition + 자체 소형 select tree + fixed-priority merge**로 구현하면 PUBS의 논리와 정렬되어 이 문단을 지지 근거로 인용할 수 있다.
반대로 기존 select arbiter에 priority 입력을 추가하는 형태라면 **이 문단을 명시적으로 반박해야 한다.** (반박 논지: 작고 고정 크기인 physically-contiguous partition은 6-to-IW와 58-to-IW 두 arbiter의 2단 merge로 충분하며, Ando가 기각한 IQS-to-1 MUX sort가 아니다.)

### 3.12 PUBS ↔ ZERECO 차별화 요약

| 축 | PUBS | ZERECO의 입장 |
|---|---|---|
| IQ 조직 | **Unified 64 entry.** Distributed는 한 문장 주장뿐, 평가 0 | **Distributed per-bank** — 미개척 영역 |
| Priority 구현 | Head 물리 partition, select logic **무수정** | 동일 철학이면 인용, priority-bit면 §III-C1 반박 필요 |
| P-IQ 크기 | 6/64 = **9.4%**, 단일 값, 모델별 재튜닝 없음 | 15% — 더 큰 capacity 손실 노출 |
| Full 정책 | **Stall (기본).** stall 7.8% ≫ non-stall 3.0% | **non-stall이 유리** — distributed에서 stall 비용이 큰 것으로 설명 |
| Slice 식별 | Decode-time PC table, occurrence마다 1단계씩 성장 | Retire-time backward walk — 정확·per-instance·depth-bounded |
| Memory dependence | **전혀 없음** (slice가 load에서 끊김) | store→load 처리 — 무료 차별점 |
| H2P 식별 | 6-bit resetting counter, **dynamic branch의 71%** | 소수 static H2P branch만 |
| Baseline | **Random queue** (명시). AGE는 cycle당 1개만 oldest | Random queue 통일이 정당하며 인용 가능 |
| Memory latency | **불가능하다고 인정하고 mode switch로 비활성화** | RF prefetch로 그 구멍을 메움 — PUBS가 세팅해준 story |
| 비용 | 4.0KB, IQ entry당 0 bit | 넘어야 할 기준선 |

## 4. Branch Runahead — "An Alternative to Branch Prediction for Impossible to Predict Branches" (MICRO 2021)

Pruett & Patt (UT Austin HPS). pp. 804–815. **Scarab로 평가** — 우리와 동일 시뮬레이터, 동일 baseline predictor, 동일 benchmark 필터.

### 4.1 🔴 우리 실험 환경과 직접 비교 가능

| 항목 | Branch Runahead | ZERECO |
|---|---|---|
| Simulator | **Scarab** (x86, PIN, wrong-path 모델링, Ramulator) | 동일 |
| Baseline predictor | **64KB TAGE-SC-L** | 동일 |
| Workload | SPEC17 int-speed + SPEC06 int + **GAP**, **MPKI > 2 필터** → 17개 | 정합 필요 |
| SimPoint | 벤치당 1–5 region, **region당 200M instruction** | 동일 방식 |
| Core | 4-wide, **ROB 256, RS 92**, 3.2GHz, L1D 32KB 3cy 2-port, L2 2MB 18cy | Golden Cove 기반 — **IPC delta 비교 전 정규화 필요** |

### 4.2 Motivation — predictor를 키워도 안 된다는 정량 근거 (Fig. 1)

**벤치마크별 가장 예측하기 어려운 32개 branch만**의 misprediction rate:

| | amean |
|---|---|
| 64KB TAGE-SC-L | **10.6%** |
| **MTAGE-SC (무제한 저장공간)** | **9.3%** |
| Dependence Chains (slice 재실행) | **4.6%** |

> **저장공간이 무제한인 MTAGE-SC조차 11% → 9%, 겨우 18% 개선.** Dependence chain은 55% 개선.

**80KB TAGE-SC-L**(baseline 64KB + Mini BR 17KB와 동일 면적)은 MPKI를 **0.8%**, IPC를 **0.3%**만 개선한다. 타깃 branch에 대해서는 *"negligible effect"*.
⇒ ZERECO §2.1의 "accuracy 대신 penalty를 줄인다"는 논지에 쓸 수 있는 가장 강력한 인용.

단, 정직한 데이터도 있다: 17개 중 3개(gobmk_06, sjeng_06, omnetpp_06)는 **dependence chain 방식이 MTAGE-SC보다 나쁘다.**

### 4.3 🔴 Chain 길이 — ZERECO의 slice 설계에 직접 적용

**Fig. 2: 평균 dependence chain 길이 = 5.8 µop** (hard cap 16). 최악 sssp 9.9, cc 8.7. *"the average length of a dependence chain is fewer than 8 micro-operations."*

**Fig. 13 Max Chain Length sweep (가장 중요한 sweep):**
| Cap | MPKI 상대 변화 |
|---|---|
| **8 µop** | **−21.5%** |
| 16 µop (기본) | 0 |
| 32 µop | −0.7% |

⇒ **H2P branch의 유용한 backward slice는 거의 전부 9–16 µop 구간에 있다.** 8로 자르면 이득의 21.5%가 날아가고, 16을 넘겨도 얻는 게 없다.
ZERECO의 Block Metadata Cache sizing과 walk depth 제한에 직접 반영할 수치.

### 4.4 Chain 식별 (CEB)

- **Trigger: H2P branch가 retire할 때.** 컴파일러·프로파일링·ISA 변경 없음
- **Chain Extraction Buffer (CEB): 최근 retired µop 512개 원형 버퍼** (2KB, 4B/µop) + **CEB store buffer**(store 주소 보관)
- **cycle 단위로 반복 backward walk** (Hashemi et al. Continuous Runahead에서 차용)
- **Memory dependence**: load를 만나면 그 주소를 **CEB store buffer의 주소들과 비교**해 대응 store를 찾으면 chain에 추가
- **종료 조건 단 두 가지**: (1) **같은 branch의 두 번째 dynamic instance**를 만남 → loop-carried chain, (2) affector/guard branch를 만남
- 종료 시 chain에 **`<PC, outcome>` 태그**를 붙여 dependence chain cache에 설치

**Chain 제약 (단순성 보장):** 16 µop 미만 / integer divide·floating point 없음 / **control-flow instruction 없음** / **store 없음**(store-load 쌍을 move-elimination으로 제거)

**추출 latency와 그 무의미함 — ZERECO에 중요:**
> *"Chain extraction takes place **one chain at a time, off the critical path**, and is not latency sensitive."*
> 모델링 latency = 512/4 ≈ **128 cycle**. *"We experimented with much longer latency (**1000s of cycles**) and found **no sensitivity**. This is because chain extraction very rarely produces a chain that is not currently in the chain cache."*

⚠️ **단, 이 무감도는 chain이 캐시되어 재사용되기 때문이다.** ZERECO의 retire-time walk도 Block Metadata Cache에 저장·재사용되므로 전이되지만, per-dynamic-instance walk라면 전이되지 않는다.

### 4.5 실행 엔진 (DCE) — 별도 유닛

**메인 OoO 백엔드가 아니라 전용 실행 유닛**이다 (Core-Only 변형만 예외).

| | **Core-Only (9KB)** | **Mini (17KB)** | Big (무제한) |
|---|---|---|---|
| PRF | **0 — core와 공유** | 64 × 8-entry (4KB) | 1024 × 8 |
| RSV | **0 — core와 공유** | 64 × 32-entry (4KB) | 1024 × 32 |
| FU | **core와 공유** | 전용 ALU | 전용 ALU |

**동시 실행**: "instruction window" = local register file 개수 = **동시 live dynamic chain instance 수**. Mini 64개, 최적값 **128개**.

**Live-in 확보**: register file snapshot이 없다. **core의 physical register file에서 직접 읽어** 새로 할당한 local RF로 복사한다. 동기화 시점은 **core의 branch misprediction** — *"Branch mispredictions present a convenient time to perform this synchronization, as the core backend and frontend are synchronized."*

**Load 처리**: DCE는 **core와 D-Cache/D-TLB를 공유**하되 **main thread가 포트 우선권**을 갖고 DCE는 남을 때만 쓴다. 실제 L1/L2/DRAM latency를 그대로 겪는다. **store가 chain에 없으므로 main thread 데이터 오염 걱정이 없다.**

### 4.6 Loop-carried self-sustaining chain — 핵심 메커니즘

추출이 *같은 branch의 두 번째 instance*에서 끝나므로, 추출된 chain은 **정확히 한 loop iteration 분량의 backward slice**이고 induction µop(`ADD P3 <= P3+4`)을 포함한다. Chain의 live-out이 다음 iteration의 live-in으로 이어진다.

**반복 방법 — 태그 체이닝**: chain이 끝나면 *자기가 계산한 branch 주소와 outcome*으로 chain cache를 인덱싱해 다음 chain을 시작한다. 태그가 `<A, *>`(wildcard)면 A의 어떤 방향에도 재트리거되므로 **자기 자신을 무한히 재시작**한다. *"executed continuously, as if it were in a loop... completely asynchronously from the core."*

**dynamic instance 구분 방법 (Fig. 8)**: sequence number나 instance ID가 없다. **instance N의 source register file = instance N−1의 local register file** — register file bank의 명시적 producer→consumer 사슬로 구조적으로 표현된다.

**Chain 시작 정책 3가지와 그 영향 (Fig. 11):**
| 정책 | amean MPKI 개선 |
|---|---|
| Non-speculative (선행 chain 완료 후 시작) | ~20% |
| Independent-early (wildcard chain은 트리거가 issue되면 시작) | ~34.5% |
| **Predictive (3-bit counter로 트리거 방향까지 예측)** | **~43.5%** |

> *"**Chain Initiation is the most important factor towards improving timeliness.**"* — scheduling 정책만으로 2.2배 차이.

### 4.7 Outcome 소비 — prediction override 방식

- **branch당 Prediction Queue 16개** (Mini는 각 256 entry). **동시에 16개 branch만** prediction 경로에 올릴 수 있다
- slot은 **chain 시작 시 할당**된다 (완료 시가 아니라) — program order를 점유하기 위해
- **fetch 시 TAGE-SC-L 대신 이 결과를 사용한다** (Fig. 7의 MUX). 즉 **predictor override**
- 늦으면: *"the slot is marked as consumed, even though it has not yet been filled"* → TAGE가 대신 예측. 나중에 채워져 **recovery 시에는 쓰일 수 있다**
- Recovery: branch마다 **core fetch pointer를 checkpoint**해 복구

### 4.8 🔴 결과가 틀릴 수 있고, 틀리면 full flush다

> *"Dependence chains are **not guaranteed to be correct**, and occasionally diverge from the main thread."*

세 가지 불건전 가정: (1) **loop 계속 가정** — loop가 끝나도 chain은 계속 loop라고 믿는다, (2) **biased branch가 계속 biased일 것**, (3) **store-load aliasing이 유지될 것** (move-elimination 했으므로 aliasing이 바뀌면 조용히 틀린 값 생성).

**별도 검증 메커니즘이 없다.** DCE 결과는 prediction으로 소비되므로 틀리면 **평범한 branch misprediction이고 full pipeline flush 비용을 전부 낸다.** 2-bit throttle counter(DCE가 TAGE보다 지속적으로 나쁘면 무시)가 유일한 2차 방어선이다.

> **이것이 ZERECO와의 가장 근본적인 차이다.** Branch Runahead는 *틀릴 수 있고 full flush를 유발하는 prediction*을 만든다. **early-resolution 이득이 0이다.** 이득 전부가 "misprediction을 줄인다"이지 "misprediction을 싸게 만든다"가 아니다.

### 4.9 🔴 Timeliness가 최대 약점이고 원인이 load latency다 — ZERECO 논지의 최강 근거

**Fig. 12 (타깃 branch instance 대비 amean):**
| 범주 | 비율 | 의미 |
|---|---|---|
| Inactive | **21%** | 아직 chain이 활성화되지 않음 (첫 동기화 misprediction 전) |
| **Late** | **35%** | chain은 활성인데 결과가 fetch 후 도착 |
| Throttled | 8% | throttle counter가 음수 |
| Incorrect | ~2% | 사용했는데 틀림 |
| Correct | ~34% | 사용했고 맞음 |

⇒ **시의적절한 coverage는 ~36%뿐**이고, 손실의 ~64%가 **correctness가 아니라 timeliness**다.

**Late의 원인을 저자들이 직접 명시한다:**
> *"The **late** category refers to predictions which have active chains, but are generated **too late to be useful** for the core. **This generally happens when the dependence chain contains too many long latency operations.**"*
> *"**Timeliness is the most difficult issue Branch Runahead faces**, with late predictions making up the largest category outside of correct predictions."*

Late 최악: **astar_06 ≈63%**, omnetpp_06 ≈48%, tc ≈48%, mcf_17 ≈45%.
astar_06은 **Inactive가 거의 0%인데 Late가 63%** — 순수하게 load latency에 묶인 사례다.

> **문헌상 "slice 안의 직렬화된 dependent load가 병목이지 slice 발견이 병목이 아니다"를 보여주는 가장 강한 증거다.** ZERECO의 Target Load RF prefetch가 정확히 이 Late bucket을 노린다.

### 4.10 🔴 Load address 예측을 명시적으로 하지 않는다 — ZERECO에 열린 문

Branch Runahead는 **load address prediction도 load value prediction도 전혀 하지 않는다.** 주소 산술을 재실행하고 실제 load를 공유 D-cache로 보낸다(그것도 main thread보다 낮은 포트 우선순위로).

Related work에서 스스로 인정한다 — Gupta et al. [14]는 *"dependence chains that contain **one load instruction with a predictable address**"* 를 노리며, BR은 이를 *"effective for a subset of branches"* 라고 기각하고 더 일반적인 기법임을 주장한다.

동시에 **BR 자신이 추출 시점에 store→load address aliasing을 탐지해 move-elimination한다** — 즉 이 slice들 안의 주소 관계가 exploit할 만큼 안정적이라는 증거이며, address prefetch 접근을 **뒷받침**한다.

### 4.11 백엔드 공유의 대가 — 측정된 수치

| 구성 | MPKI 개선 | **IPC 개선** |
|---|---|---|
| 80KB TAGE-SC-L | 0.8% | 0.3% |
| **Core-Only (9KB, PRF/RS/FU를 core와 공유)** | 37.5% | **8.2%** |
| **Mini (17KB, 전용)** | 43.6% | **13.7%** |
| Big (무제한) | 47.5% | 16.9% |

⇒ **백엔드 공유로 IPC 이득의 약 40%를 잃는다.** sssp에서는 Core-Only가 **−7.5%로 오히려 손해**다.
저자들이 밝힌 원인: *"The trade-off comes down to **cost vs chain level parallelism**."*

> **ZERECO에 유리한 해석**: 이 손실의 원인은 **여러 dynamic chain instance를 병렬 실행하려는 window starvation**이다 (Window Size sweep: 32개로 줄이면 −4.0%).
> ZERECO는 백엔드를 공유하지만 **chain-level parallelism이 필요 없다** — 이미 in-flight인 하나의 instance만 가속한다. 따라서 Core-Only의 페널티는 ZERECO가 내는 페널티가 아니다.

### 4.12 비용과 부가 결과

**저장공간**: Core-Only 9KB / **Mini 17KB** (chain cache 2KB + PRF 4KB + RSV 4KB + prediction queue 4KB + HBT 1KB + CEB 2KB). 튜닝하면 Big도 27KB로 구현 가능.
**면적** (McPAT 22nm): DCE **0.38 mm² = 코어의 2.2%** (Core-Only는 1.4%). 참고로 64KB TAGE-SC-L이 0.73 mm².
**Clock**: *"The only component on the critical path is a MUX"* — TAGE와 DCE prediction queue 사이의 MUX뿐.
**실행 오버헤드 (Fig. 3): µop +34.3%, load µop +40.5%.** 최악 tc는 load **+142%**.
**에너지**: gmean −5%~−6.5% (실행 시간 단축 덕). 단 이득이 없는 벤치마크(deepsjeng, sjeng, gobmk, omnetpp, pr)에서는 **최대 +8%**.

**Merge point predictor 정확도 92%**, **chain의 78%가 affector/guard branch의 영향을 받는다** (Fig. 5) — 이 slice들이 실제 control flow에 깊이 박혀 있다는 뜻이며, decoupled engine은 이를 재구성해야 하지만 **main-thread 메커니즘은 공짜로 얻는다.**

### 4.13 ZERECO positioning — 한 문장

> Branch Runahead는 H2P backward slice를 main thread에서 분리해 self-sustaining loop로 반복 실행함으로써 더 이른 — 그러나 여전히 **틀릴 수 있고 ~36%만 시의적절한** — prediction을 만든다.
> ZERECO는 slice를 main thread 안에 두고 branch가 **더 일찍 resolve되게** 한다. Run-ahead 거리를 포기하는 대신 **correctness를 공짜로 얻고**, divergence 기계장치가 전혀 필요 없으며, **Branch Runahead 스스로 자신의 지배적 실패 원인으로 지목한 load latency를 직접 공격한다.**

ZERECO가 삭제하는 BR의 하위 시스템 전체: prediction queue 3-pointer 구조, per-branch FIFO, merge-point predictor, affector/guard 추적, 128-entry 4-way WPB, poison propagation, 2-bit throttle, live-in 복사/동기화, global rename.

---

## 4.5 CRISP — "Critical Slice Prefetching" (ASPLOS 2022)

Litz, Ayers, Ranganathan (UC Santa Cruz / Google). **Scarab으로 평가** — 우리와 동일 시뮬레이터.
**ZERECO의 P-IQ 절반과 직접 중복되는 가장 위험한 선행 연구.**

### 4.5.1 🔴 결론 두 가지

1. **CRISP는 register file을 전혀 건드리지 않는다.** 논문 14쪽 전체에 "register file", "physical register", "PRF", "rename" 개념이 없다. ⇒ **ZERECO의 RF prefetch는 완전히 무주공산이다.**
2. **그러나 CRISP는 "H2P branch의 backward slice를 만들어 issue priority를 준다"를 이미 했다.** 이것이 ZERECO의 (a)+(c)다. 2022년에 발표되었고 `lbm`에서 **branch slice만으로 +19.3% IPC**, 평균 +2.5%다.

> ⚠️ **"H2P branch의 backward slice를 식별해 issue를 우선한다"를 novel contribution으로 주장하면 안 된다.** CRISP를 아는 리뷰어가 즉시 기각한다.

### 4.5.2 CRISP의 실체 — prefetcher가 아니다

제목의 "prefetch"는 은유이며 논문 자신도 따옴표를 친다.
> §1: *"Our technique then **"prefetches"** these critical instructions by extending the processor's **instruction scheduler to prioritize critical instructions**."*
> §5.2: *"CRISP **only reorders memory accesses without reducing cache misses**."*

별도 prefetch request 없음, prefetch queue 없음, cache 삽입 정책 없음, address/value prediction 없음, helper thread 없음. **demand load 자체를 scheduler가 더 일찍 뽑을 뿐**이고, 데이터는 정상 load pipeline을 통해 L1D와 destination register로 간다.

### 4.5.3 식별 방식 — 오프라인 프로파일링 + FDO + 새 ISA prefix

| 항목 | 내용 |
|---|---|
| 프로파일링 | DynamoRIO Memtrace 또는 Intel PT (**memory dependence 추적에는 PTWrite 필요, Kaby Lake 이상**) |
| 트레이스 | **100M instruction, 5GB (압축 1.6GB)**, 분석 **~100초** |
| 배포 | AutoFDO / BOLT / Propeller **post-link 재작성** |
| 표시 방법 | **새 x86 instruction prefix 1바이트** + **decoder 변경** |
| 적응성 | **static per-PC.** phase 적응 없음, per-instance 필터링 없음 |

⇒ TEA가 "lightweight compiler solution"이라 부른 것은 **부정확하다.** 실제로는 새 ISA prefix + decoder + scheduler를 요구하는 HW/SW co-design이다. 우리 논문에서 이 라벨을 반복하면 안 된다.

**Critical load 정의**: LLC miss ratio > 20%, 전체 load의 5% 이상, MLP < 5. **H2P branch 정의: misprediction rate > 15%.**
**전역 제약**: critical instruction 비율이 **5~40%** 여야 한다. 너무 많으면 *"the scheduler will have no opportunity to prioritize any instruction"* — 자기무력화한다.

### 4.5.4 Slice 구축 — memory dependence를 따라간다 (IBDA 대비 핵심 우위)

소프트웨어 backward dataflow walk. `frontier` 워크리스트를 유지하며 역순으로 ancestor를 추적한다.
> *"IBDA … only consider **register dependencies**, our proposed software technique observes **dependencies through memory**, a critical capability enabling precise and comprehensive load slices."*

⚠️ **이것이 CRISP가 IBDA를 이기는 주 논거이므로, ZERECO의 backward walk가 store→load를 처리하지 못하면 같은 논거로 공격당한다.** TEA식 walk는 memory dependence를 다루므로 문제없지만 구현에서 반드시 확인해야 한다.

**Slice 크기 (Fig. 4)**: `pop` 5.5 ~ `moses` 4,400, **평균 ≈ 550 instruction.** ROB/RS를 훨씬 초과한다.
> *"load slices can contain **thousands of instructions exceeding the ROB and reservation station size**… if the instructions of a load slice fill all slots of the reservation station, there exist no opportunities for the scheduler to prioritize."*

**따라서 critical-path DAG 필터가 필수다** — leaf→root 경로 latency를 FU latency 표 + load는 profiled AMAT로 계산해 critical path 위의 instruction만 승격한다.

### 4.5.5 Scheduler 수정 — 거의 공짜다 (우리가 넘어야 할 기준선)

**Baseline: age-matrix 기반 6-oldest-ready-first RAND scheduler.**
CRISP의 추가분 전부:
- IQ entry당 **priority 1비트** (96-entry RS면 96비트)
- ready & prioritized인 것들로 **PRIO vector** 생성 → age mask와 AND → n-bit NOR 축약
- **oldest-prioritized와 oldest 사이를 고르는 MUX 1개**
- **critical path 증가: logic level 1개 + MUX 1개** (transmission gate로 구현 시 logic level 추가 없음)
- **새 테이블 0개.** IST 없음, delinquent load table 없음, slice cache 없음

⇒ **ZERECO의 P-IQ가 reserved partition 방식이라면, "왜 거의 공짜인 CRISP의 1비트 picker를 쓰지 않는가"에 답해야 한다.** 답변 방향: reservation은 non-starvation과 issue bandwidth를 *보장*하지만 priority-select는 기회적이다. 단, **area/timing 비교를 제시할 의무가 생긴다.**

### 4.5.6 평가

**Scarab.** Skylake급: 6-wide, **ROB 224, RS 96 (unified)**, 4 ALU/2 Load/1 Store, TAGE, BTB 8K, L1I 32KB 3cy, **L1D 32KB 4cy**, **LLC 1MiB/core 36cy**, DDR4-2400 **1채널**, BOP+Stream prefetcher, FDIP 128 FTQ. **L2가 없다** (L1 → LLC 직행).
Workload: SPEC CPU2017 memory-intensive + HPCG + Tailbench 3종. **200M instruction. GAP 없음, 멀티코어 없음, SMT 없음.**

| 구성 | 평균 IPC 개선 |
|---|---|
| IBDA-1K / 8K / 64K / INF | 1.0% / 3.6% / 4.4% / 6.2% |
| **CRISP (전체)** | **+8.4%** (최대 `lbm` +38%) |
| CRISP **load slice만** | ~6.4% |
| **CRISP branch slice만** | **~2.5%** (최대 `lbm` **+19.3%**) |

**branch slice만의 이득은 19개 중 10개 앱에서 ≈0이다.** 효과는 `lbm`, `namd`, `pop`, `deepsjeng`, `nab`에 집중된다.

🔴 **주목**: CRISP의 baseline은 **oldest-ready-first**인데도 branch slice만으로 평균 2.5%를 얻었다.
⇒ 우리 실험의 "oldest-first에서 P-IQ headroom이 작다"는 결과와 **모순되지 않는다** (2.5%는 작다). 하지만 `lbm` 같은 workload에서는 크다는 것도 사실이다.

**Sensitivity**: RS/ROB를 키워도 이득이 단조 증가하지 않는다 (64RS/128RS/150RS → 5.8%/5.8%/5.0%). Threshold T에 매우 민감하다 (T=5%/1%/0.2% → 3.2%/8.4%/5.1%).

**코드 footprint**: prefix 1바이트 → static +0.3% 평균, **dynamic +5.2% 평균** (최대 9.8%), **icache MPKI 최대 +2.6%**.

### 4.5.7 CRISP가 스스로 밝힌 한계 — ZERECO의 공격 지점

1. *"CRISP only reorders memory accesses **without reducing cache misses**."* — MPKI 불변
2. **자기무력화**: critical 비율이 너무 높으면 우선순위를 줄 대상이 없어진다 (5~40% 필요)
3. **Slice가 window를 초과한다** → critical-path 필터 필수
4. **Reach가 RS 96 / ROB 224에 하드 바운드**된다. slice 안에 LLC miss가 있으면 **DRAM latency 하나를 이길 수 없다**
5. Threshold를 앱마다 튜닝해야 하고 재프로파일링이 future work
6. **branch MPKI, resolution latency, H2P coverage를 전혀 측정하지 않는다.** branch precomputation 연구와 비교한 적도 없다 (Branch Runahead를 §7에서 한 문장으로 기각만 함)

**§7에서 CRISP 자신이 넘겨준 논거:**
> *"**Value prediction is complementary to CRISP as breaking dependency chains exposes more ILP**, generating additional opportunities for criticality-based scheduling."*
⇒ dependence chain을 끊는 것은 CRISP와 직교한다고 스스로 인정한다. RF prefetch가 정확히 그것이다.

### 4.5.8 🔴 ZERECO의 재프레이밍 — 무엇이 진짜 novel인가

| 축 | CRISP | ZERECO | 판정 |
|---|---|---|---|
| **RF prefetch** | 없음 (register file 무관) | Target Load 값을 PRF에 직접 공급 | ✅ **완전 무주공산** |
| slice를 **짧게** 만듦 | 못 함 (더 *일찍* 실행할 뿐) | 예측 가능한 producer를 critical path에서 제거 | ✅ CRISP §7이 직교성 인정 |
| slice 식별 시점 | **오프라인 프로파일 + FDO + ISA prefix** | **retire-time 하드웨어**, 프로파일·ISA 변경 불필요 | ⚠️ CRISP §3.5가 "하드웨어만으로는 불충분"을 반박함 → TEA로 재반박 필요 |
| **slice pruning 기준** | **추정 latency** (critical-path DAG) | **예측 가능성** (RF-covered면 priority에서 제외) | ✅ **여기가 scheduling 쪽의 진짜 novelty** |
| Window 한계 | RS 96 / ROB 224에 하드 바운드 | RFP 값은 ROB 밖에서도 유효 | ✅ 정량화 가능 |
| Branch resolution 지표 | **측정 안 함** | 정면 측정 | ✅ 데이터 공백 |
| H2P slice + issue priority | **이미 함 (2022)** | 동일 | ❌ **novelty 주장 불가** |
| Scheduler 비용 | 1비트/IQ + logic level 1 + MUX 1 | reserved partition | ⚠️ area/timing 비교 의무 |
| Workload | GAP 없음 | GAP 포함 | ✅ 소폭 |

**결론**: scheduling 쪽 novelty는 "branch slice에 priority를 준다"가 아니라
**"RF coverage로 slice 중 어느 부분에 priority가 필요한지 걸러낸다"** 이다.
즉 아키텍처 문서 §7.1의 online RF filtering이 P-IQ 자체보다 더 중요한 기여다. 논문 서술의 무게중심을 그쪽으로 옮겨야 한다.

### 4.5.9 실행 권고

1. **CRISP를 Scarab에 baseline으로 구현할 것.** 동일 시뮬레이터이고 변경이 작다(priority 비트 + picker). 리뷰어가 반드시 묻는다. Oracle slice + priority picker의 idealized-CRISP를 baseline으로 두면 RF prefetch delta가 반박 불가능해진다.
2. **TEA의 "a few cycles earlier" 표현을 반복하지 말 것.** CRISP는 `lbm`에서 branch slice만으로 19.3%를 얻는다. 대신 *"scheduling window와 slice 내부 memory latency에 이중으로 바운드된다"* 로 쓸 것.
3. **TEA의 "compiler solution" 라벨도 반복 금지.** CRISP는 ISA prefix + decoder + scheduler 변경을 요구한다.
4. **핵심 그림**: H2P branch slice 중 **critical path에 LLC-missing load가 있는 비율**을 측정할 것. 그것이 CRISP가 구조적으로 못 고치고 RF prefetch가 고치는 영역이며, 논문 전체의 정당화가 된다.

---

## 5. 현재 Scarab 구현과 논문의 차이 (코드 확인 결과)

### 5.1 P-IQ가 PUBS의 free-list 분할 방식이 아니다 — 개선 여지

[`node_issue_queue.cc:112-146`](node_issue_queue.cc)의 `node_issue_queue_allocate_rs_entry()`는
RS당 **단일 circular FIFO free list**(`free_entry_ids` / `free_entry_head` / `free_entry_tail`)에서 entry ID를 꺼낸다.
따라서 physical entry ID는 priority class와 무관하게 재활용 순서대로 배정된다.
P-IQ는 `zereco_priority_op_count` vs `zereco_priority_rs_limit` **카운터 기반 논리 partition**이고,
우선순위는 [`node_issue_queue.cc:273-280`](node_issue_queue.cc)의 `node_issue_queue_precedes()`에서
**priority-bit comparator**를 baseline 정렬보다 먼저 적용해 구현된다.

**이것은 PUBS §III-C1이 "unpractical"이라고 기각한 바로 그 방식(priority mark를 select logic이 참조)에 해당한다.**
반면 PUBS의 실제 구현은 *"we divide the free list of the IQ into two lists"* 뿐이고 select logic은 무수정이다.

**제안하는 수정**: free list를 **low ID 구간(priority) / high ID 구간(normal)** 으로 분할한다. 그러면
- `RANDOM_PHYSICAL` scheduler는 이미 `rs_entry_id` 오름차순으로 정렬하므로 **comparator 없이 position만으로 priority가 성립**한다.
- 하드웨어 주장이 "select logic 무수정, free list만 분할"로 강해지고 PUBS §III-C1을 반박 대신 **인용**할 수 있다.
- 현재 comparator는 그 경우 중복이 되므로 제거하거나 검증용으로만 남긴다.

`OLDEST_FIRST`에서는 position이 무의미하므로 comparator가 여전히 필요하지만, 그 구성은 애초에 PUBS의 zero-delay 논거가 적용되지 않는 영역이다.

### 5.2 ZERECO의 `RANDOM_PHYSICAL` = PUBS의 random queue

`NODE_ISSUE_QUEUE_SCHEDULE_SCHEME_RANDOM_PHYSICAL`은 `(rs_id, rs_entry_id)` 오름차순으로 선택한다.
이는 PUBS가 정의한 **random queue + position-based fixed priority select**와 동일한 조직이다.
⇒ ZERECO의 random-physical은 인위적 sensitivity 도구가 아니라 **PUBS가 현대 프로세서 대표 조직으로 채택하고 정당화한 바로 그 baseline**이다.
논문에서 이렇게 서술하면 baseline을 섞지 않고도 P-IQ 효과를 정당하게 제시할 수 있다.

### 5.3 Load wakeup이 비투기적 — RFP 이식의 제약

[`dcache_stage.c`](dcache_stage.c)에서 `wake_up_ops()`는 실제 캐시 접근 결과로 정해진 `done_cycle`에 호출된다.
hit-miss predictor도, dependent의 speculative wakeup도, cancel 경로도 없다.
`op->replay` / `replay_cycle` / `replay_count`는 [`op.h:203-205`](op.h)에 선언만 되어 있고 core 코드에서 TRUE로 설정하지 않는다.
⇒ §1.8 참조. Validate-then-use 설계는 자연스럽게 얹히고, speculative-use는 인프라 신설이 필요하다.

### 5.4 PRF 모델은 존재한다

`reg_renaming_scheme=1`, `reg_table_integer_physical_size=280`, `reg_table_vector_physical_size=332` (PARAMS.golden_cove).
[`map_rename.c`](map_rename.c)의 `reg_file_write_dst()` / `reg_file_produce_dst()`가 RFP의 **rename-time prfid 바인딩** 지점에 대응한다.
