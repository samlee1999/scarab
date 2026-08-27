# Reference Paper Notes

> Branch misprediction penalty, branch pre-computation, criticality-driven scheduling, load-latency
> prefetching 계열 논문들의 mechanism-level 정독 노트.
>
> **이 문서는 논문 원문의 내용만 담는다.** 각 절은 해당 논문이 스스로 말하는 것 — 문제 설정, 정량적
> 동기, 핵심 통찰, 하드웨어 구조, 단계별 동작, correctness 처리, 파라미터와 storage, 평가 방법과 결과,
> 저자가 밝힌 한계 — 으로만 구성된다. 논문 간 비교는 각 논문이 **자기 related work 절에서 직접 말한
> 것**에 한해 §x.8에만 적는다.
>
> 수치는 인쇄된 그대로 옮기고, 무엇 대비 몇인지를 함께 적는다. 논문이 표를 인쇄하지 않아 그림에서
> 읽은 값은 "판독 근사값"으로 표시한다. 논문 내부에서 수치가 서로 어긋나는 경우 양쪽을 모두 적는다.
>
> Last updated: 2026-08-27

## 논문 목록

| # | 약칭 | 제목 | 발표 | 한 줄 요약 |
|---|---|---|---|---|
| 1 | **PUBS** | Performance Improvement by Prioritizing the Issue of the Instructions in Unconfident Branch Slices | MICRO 2018 | prediction confidence가 낮은 branch의 backward slice에 IQ의 예약된 head entry를 배정해 issue를 앞당기고 misspeculation penalty를 줄인다 |
| 2 | **3D-Branch Overrider** | Opportunistic Early Pipeline Re-steering for Data-dependent Branches | PACT 2020 | data-dependent branch의 feeder load 값을 얻는 즉시 branch outcome을 계산해, execution stage의 비싼 flush 대신 저지연 front-end re-steer로 대체한다 |
| 3 | **Branch Runahead** | Branch Runahead: An Alternative to Branch Prediction for Impossible to Predict Branches | MICRO 2021 | 예측 불가 branch의 dependence chain을 런타임에 추출해 전용 엔진에서 **연속 실행**함으로써 outcome을 미리 계산한다 |
| 4 | **CRISP** | CRISP: Critical Slice Prefetching | ASPLOS 2022 | profiling으로 찾은 critical load/branch slice를 ISA prefix로 표시하고 scheduler가 그 instruction을 우선 issue하게 한다 |
| 5 | **RFP** | Register File Prefetching | ISCA 2022 | load의 주소를 미리 예측해 데이터를 Rename이 할당한 destination physical register로 직접 가져와 L1 접근 지연을 숨긴다 |
| 6 | **TEA** | Timely, Efficient, and Accurate Branch Precomputation | MICRO 2024 | H2P branch의 dependence chain을 retire 시점에 학습해 Block Cache에 저장하고, main thread와 backend를 공유하는 경량 thread로 미리 실행해 조기 flush를 낸다 |

---

## 1. PUBS — "Performance Improvement by Prioritizing the Issue of the Instructions in Unconfident Branch Slices" (MICRO 2018)

> Hideki Ando (단독 저자) · Nagoya University (ando@nuee.nagoya-u.ac.jp) · 2018 51st Annual IEEE/ACM International Symposium on Microarchitecture, pp. 82–94, DOI 10.1109/MICRO.2018.00016
> 원문: [PDF](</home/lee/scarab/reference/[2018, MICRO] PUBS.pdf>)

**Problem.** Single-thread performance는 Dennard scaling 종료 이후 10년 넘게 정체되어 있고, 그 최대 장애물 중 하나가 branch misprediction이다 (§I). 프로그램 전체의 misprediction 손실은 *misprediction 빈도* × *misprediction당 penalty cycle*에 비례하므로 공격 축은 두 개인데, 수십 년간의 연구는 거의 전부 전자(branch predictor 정확도 개선)에 몰려 있었고 후자는 "rarely studied"라고 저자는 지적한다 (Abstract, §I). 이 논문은 의도적으로 후자를 택한다. 저자는 misprediction penalty를 **state recovery penalty**(pipeline flush와 processor state 복구에 쓰이는 cycle)와 **misspeculation penalty**(mispredicted branch의 fetch부터 그 branch의 execution 완료까지, 쓸모없이 speculative execution에 소비되는 cycle)로 나누고 후자만을 표적으로 삼는다 (§I, §II-A, Fig. 1). misspeculation penalty는 다시 (1) branch가 front-end pipeline을 흘러내리는 cycle, (2) IQ에서 dependence가 해소되고 issue로 select될 때까지 기다리는 cycle, (3) branch 자신의 execution cycle로 분해되는데, pipeline 구조가 정해지면 (1)과 (3)은 줄일 수 없고 오직 (2)만이 architectural scheme으로 줄일 수 있다 (§I). 그런데 IQ의 select logic은 processor critical path 중 하나이므로 회로가 단순해야 하고, 그래서 **position-based priority**(queue head에 가까운 entry일수록 높은 priority)라는 고정 정책만 쓴다 — 즉 해당 instruction이 branch를 먹여 살리는 dependence chain에 속하는지 여부와 무관하게 select된다 (§I, §III-B1). 이것이 misspeculation penalty 관점에서의 문제다.

### 1.1 Motivation과 characterization

이 논문의 motivation은 별도의 characterization 절이 아니라 §I·§II의 분석적 논증 + §V의 evaluation 결과에 흩어져 있다. 정량 근거는 다음과 같다.

| 근거 | 수치 | 출처 |
|---|---|---|
| misspeculation penalty가 issue conflict에 얼마나 민감한가 (분석적 예시, 측정 plot 아님) | branch slice가 5-instruction dependent chain이고 각 instruction의 issue가 issue conflict로 1 cycle씩 추가 지연되면 misspeculation penalty는 **5 cycle** 증가 | §I |
| mispredicted branch의 timeline 분해 | fetch → front-end flow down → **issue wait** → exec → state recovery → redirection. 앞의 fetch~exec 종료까지가 misspeculation penalty, 뒤가 state recovery penalty이며 "The pipeline is substantially stalled during these cycles" | §I, §II-A, Fig. 1 |
| PUBS가 건드리지 **않는** 고정 성분 | base machine의 state recovery penalty = **10 cycle** | Table I, §V-A |
| LLC miss가 미치는 상대적 영향 | LLC miss penalty가 **300 cycle**(본 평가의 main memory min. latency)이라 LLC miss가 잦으면 branch slice 대신 computation slice가 critical해지고 PUBS의 이득이 사라짐 | §V-B, Table I |
| unconfident 판정의 coverage | 선택된 6-bit confidence counter에서 unconfident branch rate(unconfident branch / 전체 dynamic branch) = **71%** | §V-D, Fig. 11 |
| speedup과 branch MPKI의 상관 | compute-intensive program(red dot, LLC MPKI < 1.0)에서는 speedup이 branch MPKI와 상관을 보이고, compute-intensive가 memory-intensive(blue dot)보다 speedup이 크다 | §V-B, Fig. 9 |
| 선행 연구(Butler et al.)의 반증 논거 | ready instruction 수가 0으로 치우친 것은 저자도 확인했으나, "there are still a significant number of clock cycles where the number of ready instructions is more than two" (프로그램 의존성 큼) | §VI |
| age matrix 대신 PUBS를 쓸 회로적 이유 | 저자의 LSI layout에서 age matrix의 폭은 wakeup logic과 거의 같거나 **IQ 높이의 65%**에 달하고, HSPICE 결과 IQ delay를 **13%** 증가시킴 | §V-G1, Fig. 14 |

### 1.2 Key insights

- **줄일 수 있는 penalty 성분은 branch의 IQ issue-wait 뿐이다.** front-end flow-down과 branch execution latency는 pipeline 구조로 고정되므로, branch가 직·간접적으로 의존하는 instruction 집합(= branch slice) 전체를 최대한 일찍 issue시키는 것이 유일하게 남은 레버다 (§I, §II-A).
- **모든 branch slice를 우대하는 것은 낭비다.** mispredict될 가능성이 높은 branch의 slice만 의미가 있으므로 branch prediction confidence로 필터링하고, 그렇게 걸러진 것을 **unconfident branch slice**라 부른다 — "An unconfident branch slice is a branch slice with the associated branch that cannot be sufficiently trusted." (§I, §II-B). confidence 추정 자체는 새 기법이 아니라 **Jacobsen, Rotenberg & Smith [3] (MICRO 1996)의 saturated resetting counter를 그대로 차용**한다 (§III-A1).
- **slice 전체를 materialize할 필요가 없다.** decode 단계에서 dataflow를 한 단계씩 backward로 걷고 branch의 식별자를 consumer→producer로 전파하면, 이 한 단계 연산을 반복하는 것만으로 indirect producer까지 transitively 같은 confidence counter에 연결된다 (§III-A2). logical register의 producer 추적은 define table(def_tab) 하나로 충분하다.
- **select logic을 건드리지 않고 priority를 줄 수 있다.** position-based select logic이 이미 head 근처 entry에 최고 priority를 주고 있으므로, IQ head 쪽 소수 entry를 **priority entry**로 예약하고 dispatch 때 unconfident branch slice instruction을 그쪽으로 steering하면 critical path 비용 0으로 최고 priority를 얻는다 (§III-B2, Fig. 5). 반대로 instruction별 mark를 priority에 반영하는 유연한 select logic은 wakeup logic과 select logic 사이에 IQS-to-1 MUX를 다는 방식밖에 알려진 게 없고, request 신호의 huge fan-out과 MUX의 huge fan-in 때문에 비현실적이다 (§III-C1).
- **PUBS는 회로 중립적이다.** wakeup logic이 CAM type이든 RAM(matrix) type이든, select logic이 tree arbiter든 prefix-sum circuit이든 select 회로 자체를 손대지 않으므로 "our scheme can be applied to any circuit" (§III-B1).
- **IQ entry 예약은 capacity-sensitive program을 해칠 수 있다.** memory-level parallelism(MLP)이 성능의 주 원천인 memory-intensive 구간에서는 load를 많이 띄우는 것이 branch misprediction penalty를 줄이는 것보다 중요하므로, 관측된 LLC MPKI를 기준으로 PUBS를 끈다 (§III-B3).
- **instruction age는 criticality의 heuristic일 뿐이다.** age matrix는 일반적 criticality를, PUBS는 branch misprediction 관련 criticality를 각각 다른 관점에서 본다. 그래서 둘은 상보적이고 누적된다(PUBS+AGE) (§V-G2, §VI).

- **computation slice는 branch slice의 쌍대(dual)다.** branch가 아닌 instruction을 leaf로 삼고 그것이 직·간접적으로 의존하는 instruction들로 구성된 sub-graph이며, branch slice와 배타적일 수도 있고 겹칠 수도 있다 — branch slice의 결과가 computation slice로 흘러 들어가는 경우가 그렇다. 겹치더라도 정의와 PUBS scheme은 그대로 성립한다 (§II-B, Fig. 2). LLC miss가 잦은 프로그램에서 이 computation slice가 critical해지는 것이 뒤의 mcf/soplex 저조를 설명한다.
- **base가 random queue인 것은 나머지 두 조직이 이미 탈락했기 때문이다 (§III-B1).** *shifting queue*는 age 순 물리 정렬로 IPC가 높지만 hole을 메우는 compaction 회로가 IQ critical path에 들어가 작은 IQ(Alpha 21264, 20 entry)에서만 실용적이다. *circular queue*는 compaction이 없는 대신 남은 hole이 capacity-sensitive 프로그램에서 심각한 capacity 비효율을 낳고, instruction order의 wrap-around가 issue priority를 역전시킨다. 남은 *random queue*는 delay가 짧은 대신 issue priority가 무작위로 주어져 shifting queue보다 IPC가 낮다 — 이 IPC 손실을 메우려고 age matrix가 붙는다. PUBS는 select 회로를 건드리지 않으므로 CAM형/RAM·matrix형 wakeup, tree-arbiter/prefix-sum select 어디에도 적용된다("our scheme can be applied to any circuit").

### 1.3 Hardware structures

논문의 논리적 설명(§III-A, Fig. 3)은 표에 raw PC를 저장하는 것으로 되어 있으나, 실제 구현(§IV, Fig. 6)은 PC 대신 hashed tag와 index를 concatenate한 pointer(D_b, D_c)를 저장한다. 아래 표는 §IV의 구현 기준이다.

| 구조 | 목적 | 구성 (entry 수 / associativity / indexing / pipeline 위치) | per-entry 필드와 비트폭 |
|---|---|---|---|
| **conf_tab** (confidence estimation table) | branch별 prediction confidence counter를 보관해, 해당 branch와 그 slice가 unconfident인지 판정 (§III-A1) | 128-set × 8-way set-associative (Table II). index I_c는 branch PC(PC_br)의 하위 비트, 나머지가 tag T_c. **branch가 execute될 때 write(allocate/update), decode 때 read** (§III-A1, §III-A3) | E_c = R_c ∣ T_c ∣ confidence counter (Fig. 6). T_c = **4-bit hashed tag** (Table II; §IV의 최적 N=4), counter = **6 bits** (Table II). R_c는 Fig. 6에만 나오는 선행 필드로 본문에 정의 없음(valid/replacement 성격으로 보이며, 1 bit로 잡으면 Table III의 1.4 KB와 일치 — 유도값) |
| **brslice_tab** (branch slice table) | branch slice에 속한 각 instruction을, 그 slice가 매달린 branch의 conf_tab entry로 연결하는 pointer table (§III-A2, §III-A3) | 128-set × 8-way set-associative (Table II). index I_b는 non-branch instruction PC의 하위 비트, 나머지가 tag T_b. **decode 때 write(전파)하고 decode 때 read(판정)** | E_b = R_b ∣ T_b ∣ D_c, 여기서 D_c = T_c ∥ I_c (§IV, Fig. 6). T_b = **8-bit hashed tag** (Table II; §IV의 최적 N=8), D_c = 4 + 7 = 11 bits (유도값). hashing이 없다면 128 row일 때 I_b = 7, T_b = 55(= 62 − 7)이 되어 tag 비용이 지배적이 된다 (§IV) |
| **def_tab** (define table) | 각 logical register의 producer instruction을 추적해, decode 시점에 dataflow를 한 단계 backward로 걸을 수 있게 함 (§III-A2) | **64 row의 full(tagless) table** — logical register 하나당 1 row. logical register 수가 64로 작기 때문에 full size로 둔다 (§IV). index = decoding instruction의 logical destination register number. **decode 때 write(자기 PC/D_b 기록), decode 때 read(logical source register로 조회)** | E_d = D_b = T_b ∥ I_b (§IV, Fig. 6). 평가 파라미터 기준 8 + 7 = **15 bits** (유도값; Table III의 0.1 KB와 정합) |
| **IQ priority-entry 분할** (priority entries vs. normal entries) | select logic의 critical path를 건드리지 않고 unconfident branch slice instruction에 최고 issue priority를 부여 (§III-B2, Fig. 5) | 기존 IQ의 **head 쪽 소수 entry를 priority entry로 예약**, 나머지가 normal entry. 평가 구성은 64-entry IQ에 **priority 6 / normal 58** (Table II). base IQ는 age matrix 없는 **random queue** (§III-B1). select logic은 무변경 — 그대로 position-based fixed priority | 별도 table 없음. **IQ free list를 두 개로 분할**(priority용 / normal용)하고 dispatch 때 해당 list에서 free entry number를 얻는 것이 구현의 전부 (§III-B2). unconfident branch slice instruction은 priority entry로, 없으면 dispatch stall(default). 그 외 instruction은 normal entry로 |
| **Mode-switch logic** (LLC MPKI monitor) | memory-intensive(MLP-critical) 구간에서 예약된 priority entry가 IQ capacity를 낭비하는 것을 방지 (§III-B3) | LLC MPKI를 주기적으로 관측해 threshold 미만이면 PUBS enable, 아니면 disable. 평가 구성은 **10k-cycle interval, 3.0 LLC MPKI threshold** (Table II). 동작 지점은 instruction dispatch (어느 free list를 쓸지 결정) | 필드 미명세. disable 구간에서는 두 free list를 **entry 비율로 가중된 random number**로 골라 IQ를 균일하게 사용. "Because of the simplicity, there is no penalty for mode switching" (§III-B3) |

### 1.4 Operation — stage by stage

```
[EXECUTE]  branch 실행 완료
           conf_tab[PC_br] 조회
             entry 없음  → allocate. 예측 적중이면 counter = MAX, 아니면 counter = 0
             entry 있음  → 적중이면 counter += 1 (이미 MAX면 무동작)
                           오예측이면 counter = 0            ... saturated resetting counter [3]

[DECODE]   (a) 자기 자신 등록
           def_tab[logical dest reg] <- 자기 PC (구현상 D_b = T_b || I_b)

           (b) slice 역전파 (한 단계씩)
           if (decoding inst == branch):
               for each logical source reg s:
                   PC_A = def_tab[s]                       // producer inst A의 PC
                   brslice_tab[PC_A].D_c <- PC_br          // Fig.3 mark (2)
                   // inst A가 branch의 confidence counter에 간접 연결됨
           else:   // non-branch inst A가 decode될 때
               PC_br = brslice_tab[PC_A]                   // 자신이 slice 소속이면 pointer 획득
               for each logical source reg s:
                   PC_B = def_tab[s]                       // producer inst B
                   brslice_tab[PC_B].D_c <- PC_br          // Fig.3 mark (3)
               // 이 단계를 반복(step 3)하면 indirect producer까지 transitively 연결

           (c) unconfident 판정
           if (decoding inst == branch):
               c = conf_tab[PC]
               unconfident = (c 획득 && c != MAX)          // miss이거나 MAX면 confident
           else:
               p = brslice_tab[PC]
               if (p 미획득) unconfident = false
               else { c = conf_tab[p]; unconfident = (c 획득 && c != MAX) }

[DISPATCH] if (mode == PUBS enabled && unconfident):
               priority free list에서 entry 번호 획득 → IQ head 쪽 priority entry에 write
               if (priority entry 없음) → dispatch STALL          // default stall policy
           else if (mode == PUBS enabled):
               normal free list에서 획득 → normal entry에 write
           else:   // mode switch로 disable된 구간
               두 free list를 entry 비율 가중 random으로 선택 → IQ를 균일 사용

[ISSUE]    cycle 1: wakeup + select   (wakeup–select loop = IQ critical path, 비파이프라인)
           cycle 2: payload RAM read → function unit
           select logic은 무변경: position-based fixed priority이므로
           priority entry에 있는 unconfident branch slice instruction이 먼저 grant됨
           → branch의 IQ issue-wait cycle 감소 → misspeculation penalty 감소

[주기적]   10k cycle마다 LLC MPKI 관측; MPKI < 3.0 이면 PUBS enable, 아니면 disable
```

세부 보충:

- **confidence 판정 기준은 "MAX인가"** 이다. "If the counter value in the corresponding entry is the maximum value, the prediction of the associated branch is confident; otherwise, it is unconfident." (§III-A1) — 즉 counter bit 수를 늘릴수록 MAX 도달이 어려워져 unconfident 판정이 공격적으로 늘어난다 (§V-D).
- **conf_tab의 write 시점(execute)과 read 시점(decode)이 다르다.** 학습은 branch execution에서, 사용은 decode에서 일어난다.
- **def_tab / brslice_tab은 모두 decode 단계 안에서 read·write된다.** slice 발견은 전적으로 decode-time 연산이며, backward walk는 한 번에 한 level만 수행하고 program이 그 instruction들을 다시 decode할 때 자연스럽게 다음 level로 이어진다.
- **priority는 dispatch 때 결정되고 issue 때 실현된다.** select logic은 아무것도 새로 알 필요가 없다 — "instructions in the head entries are given the highest priority for issue" (§I, §III-B2).

### 1.5 Correctness와 speculation 제어

**PUBS는 비투기적 메커니즘이다.** 이 논문은 새로운 speculative execution이나 value prediction을 도입하지 않는다. PUBS가 바꾸는 것은 (i) instruction이 IQ의 어느 entry에 dispatch되는가와 (ii) 그 결과로 select logic이 어느 ready instruction을 먼저 grant하는가뿐이며, dataflow, memory ordering, 실행되는 instruction의 집합, recovery 메커니즘은 전혀 바뀌지 않는다. 따라서 논문에는 PUBS 자체에 대한 validation·squash·recovery 논의가 존재하지 않는다.

- **오예측(confidence estimation이 틀린 경우)의 대가는 성능뿐이다.** confident한 branch의 slice를 priority entry에 넣으면 entry를 낭비하고, unconfident한 slice를 놓치면 이득을 못 얻을 뿐 correctness에는 영향이 없다.
- **table은 설계상 lossy하다.** brslice_tab과 conf_tab은 tag를 각각 8-bit, 4-bit로 hash해 저장하는 set-associative table이므로(§IV, Fig. 7) tag aliasing과 capacity eviction이 발생할 수 있으나, 그 결과는 잘못된 우대/누락일 뿐이다. brslice_tab miss는 단순히 "unconfident branch slice에 속하지 않음"으로 처리된다 (§III-A3).
- **유일한 back-pressure는 default stall dispatch policy다.** unconfident branch slice instruction에게 줄 priority entry가 없으면 dispatch를 멈춘다 (§III-B2). 이것은 correctness 장치가 아니라 throughput throttle이며, priority entry가 적을 때 실제로 성능을 baseline 아래로 끌어내린다 (§V-C, Fig. 10 — §1.7 참조).
- **branch misprediction recovery 자체는 base와 동일**하며 Table I의 10-cycle state recovery penalty를 그대로 부담한다. PUBS는 이 성분을 줄이려 하지 않는다 (§I, §II-A).
- **mode switching에도 penalty가 없다.** free list 선택 방식만 바뀌므로 "Because of the simplicity, there is no penalty for mode switching." (§III-B3)

### 1.6 Parameters와 storage budget

**PUBS 파라미터 (Table II)**

| 항목 | 값 | 비고 |
|---|---|---|
| brslice_tab | 128-set, 8-way, 8-bit hashed tag | hash N = 8은 §IV에서 최적으로 선택 |
| conf_tab | 128-set, 8-way, 4-bit hashed tag | hash N = 4는 §IV에서 최적으로 선택 |
| Confidence counter | 6 bits | §V-D에서 2–8 bit + "blind" 모델로 sweep |
| Priority entries in IQ | 6 | §V-C에서 2/4/6/8 sweep, 6이 최적 |
| Normal entries in IQ | 58 | 64-entry IQ 기준 |
| Dispatch policy | stall if no priority entry is available for unconfident branch slice instruction | §V-C에서 non-stall policy와 비교 |
| Mode switch | 10k-cycle interval, 3.0 LLC MPKI threshold | §III-B3 |
| def_tab | 64 row, tagless full table | logical register 수 = 64 (§IV) |

**Base processor (Table I)**

| 항목 | 값 |
|---|---|
| Pipeline width | fetch/decode/issue/commit 각 4-instruction wide |
| Reorder buffer | 128 entries |
| IQ | 64 entries |
| Load/store queue | 64 entries |
| Physical registers | 128(int) + 128(fp) |
| Branch prediction | 34-bit history, 256-entry weight table perceptron; 2K-set 4-way BTB; **10-cycle state recovery penalty on misprediction** |
| Function unit | 2 iALU, 1 iMULT/DIV, 2 Ld/St, 2 FPU |
| L1 I-cache | 32KB, 8-way, 64B line |
| L1 D-cache | 32KB, 8-way, 64B line, 2 ports, 2-cycle hit latency, non-blocking |
| L2 cache | 2MB, 16-way, 64B line, 12-cycle hit latency |
| Main memory | 300-cycle min. latency, 8B/cycle bandwidth |
| Data prefetch | stream-based: 32-stream tracked, 16-line distance, 2-line degree, prefetch to L2 |

**Processor size 모델 (Table IV)** — Medium이 default

| Parameter | Small | Medium | Large | Huge |
|---|---|---|---|---|
| Fetch/decode/issue/commit width | 3 | 4 | 6 | 8 |
| IQ size | 32 | 64 | 128 | 256 |
| Load/store queue size | 32 | 64 | 128 | 256 |
| Reorder buffer size | 64 | 128 | 256 | 512 |
| Physical regs (int+fp) | 64+64 | 128+128 | 256+256 | 512+512 |
| Number of iALUs | 2 | 2 | 3 | 4 |
| Number of FPUs | 1 | 2 | 3 | 3 |

**Storage budget (Table III, §V-F)** — 총 **4.0 KB**

| H/W | def_tab | brslice_tab | conf_tab | total |
|---|---|---|---|---|
| cost (KB) | 0.1 | 2.5 | 1.4 | **4.0** |

Abstract와 §VII 결론 모두 "only 4.0KB hardware cost"로 이 수치를 인용한다. IQ priority-entry 분할은 **table storage를 추가하지 않는다** — 기존 IQ entry를 재사용하고 기존 free list를 둘로 쪼갤 뿐이다 (§III-B2). mode-switch용 MPKI monitor의 storage, 그리고 area/power 모델은 **논문 미보고**. (참고로 Table II 파라미터로 역산하면 brslice_tab 1024 entry × (T_b 8 + D_c 11) = 19,456 bit ≈ 2.375 KB, conf_tab 1024 entry × (T_c 4 + counter 6) = 10,240 bit = 1.25 KB이며, Fig. 6이 그리는 R_b/R_c 필드를 1 bit씩 더하면 정확히 2.5 KB / 1.375 KB ≈ 1.4 KB가 되어 Table III와 맞는다 — 유도값이며 논문이 명시한 계산은 아니다.)

### 1.7 Evaluation setup과 results

**Setup (§V-A)**

- **Simulator**: SimpleScalar Tool Set version 3.0a [19] 기반의 자체 시뮬레이터, **Alpha ISA**. 평가 지표는 IPC.
- **Workload**: SPEC2006 전 프로그램에서 **wrf만 제외** (현재 시뮬레이터에서 정상 실행되지 않아서). gcc ver.4.5.3 `-O3` 컴파일, **ref input**, 처음 16B instruction을 skip한 뒤 **100M instruction** 시뮬레이션.
- **프로그램 분류**: branch prediction이 어려운 **D-BP** 그룹(threshold **3.0 branch MPKI**)에 초점을 맞추고, 쉬운 **E-BP** 그룹은 필요할 때 geometric mean만 제시한다. 결과 그래프의 "GM diff" = D-BP의 geometric mean, "GM easy" = E-BP의 geometric mean. Fig. 8에 나타난 D-BP 집합은 astar, bzip2, gobmk, mcf, omnetpp, perlbench, sjeng, bwaves, soplex.
- **Sizing rationale**: function unit 수는 **ARM Cortex-A72** [20]의 값을 채택했고, IQ/ROB도 Cortex-A72의 66/128을 근거로 64/128로 잡았다 — PC processor가 아니라 mobile processor를 고른 이유는 mobile 시장이 어떤 시장보다 크기 때문 (§I, §V-A). branch predictor는 vendor들이 공개하지 않지만 **AMD가 Zen에 perceptron predictor를 쓴다고 밝혔기 때문에** [18], [22] perceptron을 채택.
- **Base IQ 조직**: age matrix 없는 **random queue** (§III-B1).
- **회로 평가 (§V-G1)**: IQ(wakeup logic = CAM type, select logic = prefix-sum circuit)를 **MOSIS design rule** [25] 하에 transistor level로 설계하고, **16nm predictive transistor model** [26] (Arizona State University, Nanoscale Integration and Modeling Group)과 **ITRS** [27]의 단위 길이당 wire 저항·용량을 가정해 **HSPICE** 시뮬레이션. 긴 wire에는 driver와 repeater를 실험적으로 최적 삽입.
- **수행한 study**: 전체 speedup (§V-B), priority entry 수 및 stall/non-stall dispatch (§V-C), confidence counter bit 수 + "blind" 모델 (§V-D), mode switch on/off (§V-E), hardware cost와 동일 비용 branch predictor 확대 비교 (§V-F), age matrix IQ와의 IPC 및 delay 반영 성능 비교 (§V-G), processor size scaling (§V-H).

**Results**

- **Headline (§V-B, Fig. 8, Abstract, §VII)**: PUBS는 base(age matrix 없는 random queue) 대비 **D-BP geometric mean 7.8% speedup**을 얻고, **E-BP에서는 adverse effect가 관측되지 않는다**. 프로그램별 편차는 커서 **최대 19.2% (sjeng)**, **최소 0.3% (mcf)**. 이 편차의 원인은 (1) branch prediction이 얼마나 어려운가, (2) branch slice와 computation slice 중 어느 쪽이 critical한가이며, (2)에서는 LLC miss 빈도가 가장 강한 요인이다 — LLC miss penalty가 300 cycle로 매우 길어 miss가 잦을수록 computation slice가 critical해진다.
- **상관 분석 (§V-B, Fig. 9)**: memory intensity threshold **1.0 LLC MPKI**로 compute-intensive(red)/memory-intensive(blue)를 나누면, compute-intensive에서는 speedup이 branch MPKI와 상관되고, speedup은 compute-intensive 쪽이 memory-intensive 쪽보다 크다. 각주 1: **astar의 branch MPKI가 비정상적으로 크지만(그래프상 40 부근) 이는 올바른 값**이며, 저자는 다른 시뮬레이터(gem5 [23])와 다른 predictor(gshare, bimode, tournament)로 교차 확인했다.
- **Priority entry 수와 dispatch policy (§V-C, Fig. 10)**: **최적 priority entry 수는 6**. stall policy와 non-stall policy의 관계는 entry 수에 따라 **뒤바뀐다** — priority entry가 2개일 때는 stall policy가 dispatch stall을 너무 자주 일으켜 **baseline 아래로 성능이 떨어지고**(figure 판독 근사값 약 −3%) 오히려 non-stall policy(약 +1.4%)가 낫다. 4/6/8 entry에서는 stall policy가 우세하다(figure 판독 근사값: 4에서 약 6.1% vs 2.4%, 6에서 약 7.9% vs 3.1%, 8에서 약 7.5% vs 3.4%). 논문 본문의 결론은 종합적 서술로, non-stall은 prioritizing이 opportunistic이라 부분적으로만 이뤄지고 "the negative effect is stronger, and the stall policy is hence better"이며, 동시에 stall policy의 열화가 "clearly seen in the case of two priority entries, where the performance is degraded from that of the baseline"이라고 스스로 명시한다. 즉 **stall policy가 모든 entry 수에서 우월한 것이 아니다**. priority entry가 과도하면 반대로 unconfident slice가 아닌 instruction들에게 IQ capacity가 부족해진다.
- **Confidence counter bit 수 (§V-D, Fig. 11)**: **최적은 6 bit이고 그때 unconfident branch rate는 71%**. counter bit이 늘수록(resetting counter이므로) unconfident 판정이 늘어 coverage는 커지지만 accuracy는 떨어지고 priority entry 부족을 헛되이 유발한다. 그럼에도 결과는 **공격적인 unconfident 추정이 더 유리함**을 가리킨다. speedup 곡선은 상당히 평탄하다 — figure 판독 근사값으로 2 bit ≈ 3.5%, 4 bit ≈ 7.0%, 6 bit ≈ 7.9%, 8 bit ≈ 7.7%, **"blind" ≈ 6.7%**. "blind"(모든 branch를 무조건 unconfident로 추정 → conf_tab 자체를 제거하고 비용 절감 가능)는 PUBS보다 speedup이 낮으므로 "Thus, conf_tab is worth introducing"이라는 것이 논문의 결론이지만, **그 차이는 약 1 percentage point 수준**이다.
- **Mode switch (§V-E, Fig. 12)**: 대부분 프로그램은 mode switch enable/disable 간 차이가 크지 않고 geometric mean도 실질적으로 다르지 않다. 그러나 **mcf와 soplex는 mode switch를 끄면 성능이 열화**된다 — 예약 priority entry 수가 매우 적음에도 이 작은 IQ capacity 비효율에 민감한 프로그램이 존재한다.
- **동일 비용 branch predictor 확대 비교 (§V-F, Fig. 13)**: PUBS 비용은 4.0KB인데, 비교를 위해 branch predictor 비용을 **8.4KB**(default predictor 비용의 2배 이상, PUBS보다도 큼) 늘려 **history length 36, weight table size 512**로 확대한 base를 만들었다. 결과는 "the performance increase with the large branch predictor is marginal on average"이며 PUBS보다 훨씬 낮다. 따라서 "PUBS is worth introducing for more reasons than just increasing the branch predictor."
- **Age matrix와의 IPC 비교 (§V-G2, Fig. 15(a))**: base 대비 IPC 증가는 D-BP 평균 **AGE 6.5%**, **PUBS+AGE 10.2%**이고, 같은 그래프의 **PUBS 단독 막대는 GM diff에서 약 7.8%**(figure 판독 근사값, §V-B의 speedup과 일치). 즉 D-BP에서 **AGE의 IPC는 PUBS보다 낮으며**, E-BP("GM easy")에서만 AGE가 PUBS보다 약간 높다. age matrix는 branch misprediction을 고려하지 않는데도 D-BP에서 E-BP보다 효과가 큰데, 저자는 이를 "branch slices often include an oldest ready instruction"으로 해석한다. PUBS와 AGE를 결합하면 IPC가 더 오르는데, 두 기법이 issue priority를 서로 다른 관점(branch misprediction criticality vs. general criticality)에서 보기 때문이다.
- **Age matrix의 delay와 실제 성능 (§V-G1, §V-G2, Fig. 14, Fig. 15(b))**: 저자의 LSI layout에서 age matrix의 폭은 wakeup logic과 거의 같고 **IQ 높이의 65%**에 달하며, HSPICE 결과 **age matrix는 IQ delay를 13% 증가**시킨다. 이 delay 증가가 clock cycle time을 그대로 늘린다고 가정하면 **PUBS는 AGE보다 D-BP 평균 11.1% 높은 성능**을 낸다(Fig. 15(b)의 "GM diff"; 같은 그래프의 "GM easy"도 figure 판독 근사값으로 9% 부근). 저자의 종합 판단은 "the delay increase is larger than the IPC increase, and thus introducing the age matrix is not beneficial if we assume that the delay increase directly lengthens the clock cycle time"이다.
- **Processor size scaling (§V-H, Fig. 16, Table IV)**: criticality-aware selection scheme(PUBS, AGE)은 processor size가 커질수록 효과가 커진다. **PUBS는 Small/Medium/Large/Huge 네 모델 모두에서 AGE보다 높은 IPC**를 달성하며("PUBS achieves a higher IPC in any processor model"), PUBS+AGE는 모든 모델에서 단독보다 낫다. 이 평가에서는 age matrix로 인한 clock cycle time 증가를 고려하지 않았다. 저자의 해석: window(IQ/LSQ/ROB/register file)가 커지면 issue conflict가 늘어 PUBS·AGE가 유리해지고, 반대로 issue width와 function unit 수가 늘면 conflict가 줄어 불리해지므로, **processor resource가 균형 있게 scaling되면 PUBS의 효과는 size와 무관하게 안정적**이다.

### 1.8 저자가 밝힌 limitation과 self-positioning

**저자가 인정한 한계**

- **유연한 priority select logic은 만들 수 없다고 본다.** random queue에서 instruction별 mark를 priority에 반영하는 select logic은 문헌에 제안된 바 없고 "very difficult to implement without an extraordinary breakthrough"라고 저자는 말한다. 직관적 구현(wakeup logic과 select logic 사이의 IQS-to-1 MUX 다발)은 이론적으로 가능하나 request 신호의 huge fan-out과 MUX의 huge fan-in이 delay를 크게 늘려 비현실적이다 (§III-C1). PUBS가 head entry 예약이라는 우회로를 택한 이유가 이것이다.
- **distributed IQ는 주장만 하고 평가하지 않았다.** 본문은 unified IQ(Intel Sandy Bridge/Haswell/Skylake, IBM POWER7/8의 main IQ)를 가정하며, AMD Zen 같은 distributed IQ에는 각 IQ를 priority/normal entry로 partition해 적용할 수 있다고만 말한다. 그리고 "our study does not describe comprehensively which type of IQ is better"라고 명시적으로 유보한다 (§III-C2).
- **priority entry 예약은 IQ capacity를 낭비한다.** 항상 가득 차 있지 않기 때문이며, 이는 매우 capacity-sensitive한 프로그램의 성능을 떨어뜨린다 — 그것이 mode switch를 도입한 이유이고, §V-E에서 mcf와 soplex가 mode switch 없이는 열화됨을 확인한다 (§III-B3, §V-E).
- **memory-intensive 프로그램에서는 이득이 거의 없다.** LLC miss penalty가 수백 cycle이라 branch slice 대신 computation slice가 critical해지기 때문이며, mcf의 speedup은 0.3%에 그친다 (§V-B).
- **stall dispatch policy는 priority entry가 부족할 때 역효과를 낸다** — 2 entry에서 baseline 이하로 떨어진다 (§V-C).
- **워크로드 제외**: SPEC2006 중 wrf는 시뮬레이터에서 정상 실행되지 않아 제외 (§V-A).
- **13% delay 수치에 대한 자기 검증**: 여러 processor vendor가 실제로 age matrix를 도입하고 있다는 사실을 저자 스스로 언급하며, "This fact does not immediately imply that our delay evaluation is incorrect, because LSI parameters, including the layout rules, wire capacitance and resistance, and the characteristics of transistors in commercial processors, can be different from the assumptions in our LSI design"이라고 방어한다. 다만 age matrix의 폭이 넓다는 것 — IQ size와 같은 수의 row·column을 가진 2차원 matrix를 request/grant global wire가 가로지른다는 것 — 은 "a firm fact"라고 주장한다 (§V-G2). 또한 age matrix와 select logic의 위치를 맞바꾼 대안 layout도 가능하지만 Fig. 14(b)의 layout이 자신의 평가에서 더 나은 선택이었다고 밝힌다 (§V-G1).

**Related work에서의 self-positioning (§VI)**

- IQ는 2000년 전후에 광범위하게 연구되었고, **Abella et al. [28]**이 종합 서베이를 수행했다.
- **Butler and Patt [29]** (MICRO 1992)가 select policy들을 조사했는데, 여기에는 random selection과 "branch paths"(이 논문이 branch slice라 부르는 것)에 높은 priority를 주는 정책이 포함된다. 그들의 결과는 integer program(SPEC89)에서는 정책들이 **거의 같은 성능**을, floating-point program에서는 **최대 20% 차이**를 보였고, 그 유사성의 원인을 clock cycle당 ready instruction 수가 0으로 심하게 치우친 데서 찾았다. 저자는 자신의 결과가 다른 이유를 **Butler et al.이 integer 연산이 가능한 full function unit을 가정해 function unit에서의 issue conflict가 발생하지 않도록 했기 때문**이라고 지목한다. 저자 역시 ready instruction 수가 0에 치우친다는 점은 확인했지만, ready instruction이 2개를 넘는 clock cycle도 상당수 존재하며 이 통계는 프로그램 의존성이 매우 크다고 말한다. — **PUBS의 문제의식 자체가 선행 연구에서 부정된 적이 있으며, 그것을 뒤집는 것이 이 절의 핵심 논변이다.**
- **DEC Alpha 21264 [10]**는 엄격한 age 기반 정책(shifting queue)을 구현한 프로세서지만, 복잡한 compaction 연산이 IQ critical path에 들어가므로 작은 IQ(21264는 20 entry)에서만 실용적이고 큰 IQ를 쓰는 현대 프로세서에서는 더 이상 쓰이지 않는다.
- **Fields et al. [30]** (ISCA 2001)은 branch misprediction까지 고려해 instruction criticality를 예측하는 방식을 제안했다 — 이것이 이상적이지만 "the scheme is difficult to implement because of its high complexity and large area." 저자는 이를 **값싼 confidence 기반 proxy를 쓰는 직접적 정당화**로 삼는다.
- **age matrix**는 여러 현대 프로세서 [11]–[13]에서 random queue와 함께 쓰인다. 회로는 **[11]에서 제안**되었고 **[7]에 유사 회로**가 제시되었다. age는 criticality의 heuristic일 뿐이고, age matrix는 IPC를 올리지만 IQ delay도 올리며, wire delay는 LSI 세대가 진행될수록 유의하게 증가하는 "long and firm trend"다 [24], [27].
- **Sassone et al. [7]**은 instruction group마다 transposed issue request line을 동적으로 할당해 age matrix의 폭을 줄인다. 단점은 group 내 request를 중재하는 arbiter가 여전히 필요하고, random queue에서는 한 group의 instruction조차 IQ 전체에 흩어져 있어 arbiter의 wire가 IQ를 수직으로 관통하므로 arbiter delay가 무시할 수 없고 효과가 반감된다.
- **Speculative precomputation [31], [32]** (Zilles & Sohi, Roth & Sohi)은 difficult branch의 결과를 계산하는 데 필요한 instruction들로 program slice를 추출해 **다른 context에서 helper thread로 fork**하고, 그 결과를 원 thread로 전달해 misprediction 자체를 회피한다. 저자의 평가: 효과적이지만 helper thread 때문에 **overhead가 크다** — SMT 구현에서는 core resource의 일부를 잡아먹어 원 thread와 충돌하고, core를 helper thread에 쓰면 core 전체 자원을 소비한다. (— 유도값: PUBS와 speculative precomputation의 이 대비는 논문이 명시적으로 쓴 문장이 아니다. 논문이 적은 것은 helper thread의 overhead뿐이다.)
- **PUBS와 age matrix의 관계는 대체가 아니라 보완**이라는 것이 저자의 최종 self-positioning이다 — 두 기법은 issue priority를 서로 다른 관점에서 보므로 PUBS+AGE가 각각보다 낫다 (§V-G2, §V-H).

### 1.9 Section map과 인용 가능한 문장

**Section map**

| § | 주제 |
|---|---|
| §I | Introduction — single-thread 성능 정체, misprediction penalty를 state recovery / misspeculation으로 분해, position-based select priority가 문제, PUBS 제안 개요 |
| §II | misspeculation penalty와 branch slice의 정의 |
| §II-A | Misspeculation penalty; Fig. 1의 mispredicted branch timeline (front-end flow down / issue wait / exec / state recovery / redirection) |
| §II-B | Branch slice와 computation slice; Fig. 2. branch slice = branch를 leaf로 하고 그것이 직·간접 의존하는 instruction들로 구성된 sub-graph, computation slice = branch가 아닌 instruction을 leaf로 하는 대응 sub-graph. 둘은 **겹칠 수 있다**(branch slice의 결과가 computation slice로 흘러가는 경우). unconfident branch slice 정의 |
| §III | PUBS scheme |
| §III-A | unconfident branch slice의 예측; Fig. 3 구조 |
| §III-A1 | branch prediction confidence 추정 — conf_tab, **saturated resetting counter [3]**, allocate/update 규칙, decode 때 "counter == MAX이면 confident" 판정 |
| §III-A2 | branch slice instruction을 confidence counter에 연결 — logical destination register로 index되는 def_tab, PC_br의 backward 1-level 전파를 brslice_tab에 기록, 반복으로 transitive 연결 |
| §III-A3 | unconfident branch slice instruction의 예측 — branch 경로(PC로 conf_tab 조회)와 non-branch 경로(brslice_tab pointer → conf_tab) |
| §III-B | unconfident branch slice instruction의 issue 우선화 |
| §III-B1 | IQ 조직 — Fig. 4(wakeup logic / select logic / payload RAM), pipelined issue(cycle 1 wakeup+select, cycle 2 payload RAM), CAM vs. RAM type wakeup [5],[7], tree arbiter [8] vs. prefix-sum [6],[9] select, select logic이 고려하는 issue priority, **shifting / circular / random queue의 taxonomy**, age matrix |
| §III-B2 | Fig. 5 — IQ head의 priority entry, normal entry, free list 2분할, priority entry 없으면 dispatch stall |
| §III-B3 | Mode switching — LLC MPKI 모니터링, PUBS enable/disable, disable 시 entry 비율 가중 random free-list 선택 |
| §III-C | PUBS 구현에 대한 논의 |
| §III-C1 | flexible priority select logic — mark 기반 priority select가 비현실적인 이유 |
| §III-C2 | distributed IQ로의 적용 (Intel Sandy Bridge/Haswell/Skylake, IBM POWER7/8은 unified; AMD Zen은 distributed) |
| §IV | Reducing cost — set-associative brslice_tab/conf_tab, tagless 64-row def_tab, E_d/E_b/E_c 필드 정의, D_b = T_b∥I_b, D_c = T_c∥I_c, hashed tag(N = 8, 4); Fig. 6 구현, Fig. 7 tag hash 생성 |
| §V | Evaluation results |
| §V-A | Methodology — SimpleScalar 3.0a, Alpha ISA, SPEC2006 minus wrf, 3.0 branch MPKI로 D-BP/E-BP 분할; Table I base 구성, Table II PUBS 파라미터 |
| §V-B | Performance — Fig. 8 base 대비 speedup, Fig. 9 speedup vs. branch MPKI vs. memory intensity 상관; 각주 1 astar MPKI 검증 |
| §V-C | priority entry 수 민감도; Fig. 10, stall vs. non-stall dispatch |
| §V-D | confidence counter bit 수 민감도; Fig. 11, speedup + unconfident branch rate, "blind" 모델 |
| §V-E | mode switch의 효과; Fig. 12 |
| §V-F | Hardware cost (Table III)와 동일 비용 branch predictor 확대와의 비교 (Fig. 13) |
| §V-G | age matrix IQ와의 비교 |
| §V-G1 | Age matrix — Fig. 14 age matrix 유무에 따른 IQ 조직, LSI layout과 HSPICE delay 평가(16nm PTM, ITRS wire parameter), IQ delay 13% 증가, 대안 layout 언급 |
| §V-G2 | 비교 결과 — Fig. 15(a) PUBS / AGE / PUBS+AGE의 IPC, Fig. 15(b) clock cycle time을 반영한 PUBS의 AGE 대비 성능, vendor들의 age matrix 채택에 대한 저자의 방어 |
| §V-H | processor size 민감도 — Table IV 네 모델, Fig. 16 |
| §VI | Related work — Abella et al. 서베이, Butler and Patt의 select policy, Alpha 21264 shifting queue, Fields et al. criticality prediction, age matrix 설계와 Sassone et al., speculative precomputation |
| §VII | Conclusions |
| — | Acknowledgments, References [1]–[32] |

**인용 가능한 문장 (verbatim)**

1. "The branch misprediction penalty is divided into two penalties: state recovery penalty and misspeculation penalty." (§I)
2. "Therefore, the select logic considers a simple priority setting policy, which is position-based priority, where higher priority is given to instructions that are closer to the head of the queue." (§I)
3. "For example, if a branch slice is composed of a dependent chain of five instructions, and the issue of each instruction is delayed by an additional one cycle due to issue conflict, the misspeculation penalty is then increased by five cycles." (§I)
4. "An unconfident branch slice is a branch slice with the associated branch that cannot be sufficiently trusted." (§I; §II-B에 같은 정의 재등장)
5. "Although there are several circuits for wakeup and select logic as described, these are orthogonal to our scheme. In other words, our scheme can be applied to any circuit." (§III-B1)
6. "When an instruction is dispatched (i.e., written) to the IQ, and if it is an instruction in an unconfident branch slice, it is dispatched into one of the priority entries." (§III-B2)
7. "Although this circuit can be implemented theoretically, the huge fan-out of request signals and huge fan-in of MUXes significantly increase the delay, and thus it is unpractical." (§III-C1)
8. "As shown in the figure, PUBS achieves a 7.8% speedup on GM in D-BP, and no adverse effect is observed in E-BP." (§V-B)
9. "This situation is clearly seen in the case of two priority entries, where the performance is degraded from that of the baseline." (§V-C)
10. "As a result, we have found that the age matrix increases the delay of the IQ by 13%." (§V-G1)
11. "As a result, the gap between AGE and PUBS+AGE becomes significant, where the IPC increase of AGE over the base is 6.5%, while that of PUBS+AGE is 10.2% on average in D-BP." (§V-G2)
12. "While this method is effective, it has a large overhead because of the need for helper threads." (§VI, speculative precomputation에 대해)

---

## 2. 3D-Branch Overrider — "Opportunistic Early Pipeline Re-steering for Data-dependent Branches" (PACT 2020)

> Saurabh Gupta · Niranjan Soundararajan · Ragavendra Natarajan · Sreenivas Subramoney — Processor Architecture Research Lab, Intel Labs, Intel Corp. (Natarajan: Intel Corporation) / Bengaluru, KA, India. PACT '20, October 3–7, 2020, Virtual Event, GA, USA. ACM ISBN 978-1-4503-8075-1/20/10, DOI 10.1145/3410463.3414628. Session 5: Best Paper.
> 원문: [PDF](</home/lee/scarab/reference/[2020, PACT] Opportunistic Early Pipeline Re-steering for Data-dependent Branches.pdf>)

**Problem.** OOO 코어가 더 큰 instruction window로 ILP를 뽑아내려 깊고 넓어지면서, 파이프라인 맨 앞의 branch predictor가 만드는 mis-speculation의 비용이 급격히 커진다 (Abstract, §1). Conditional branch가 전체 misprediction의 92%를 차지하는데(§1, §6의 워크로드 기준), TAGE-SC-L처럼 global direction/path history와 branch-local history를 추적하는 state-of-the-art 예측기는 "prior branch history와 상관관계가 약한(poorly correlated)" branch — 즉 지배 데이터 값 자체의 entropy가 높은 data-dependent branch — 에 대해서는 크기를 키워도 "only a marginal gain"만 준다 (§1, §2.1). 저자들은 이 범주가 "the single most important category of mispredictions to tackle"라고 규정한다 (§1). 기존 하드웨어 제안인 EXACT와 SLB는 각각 10KB 초과 / 약 약 8KB의 저장 용량을 요구하고, EXACT는 branch prediction 시점에 load address를 알 수 없어 "이전에 retire한 branch instance"를 proxy identifier로 쓰는 비효율을, SLB는 ISA 확장·프로파일링·컴파일러 지원과 load/store hint instruction 및 추가 compare instruction 삽입을 요구한다 — 저자들은 이 "high complexity that has impeded their adoption into commercial state-of-the-art processors"를 문제 삼는다 (§1, §2.2). 이 논문의 프레이밍은 예측기를 개선하는 대신, feeder load 하나와 단순 연산 몇 개만으로 방향이 결정되는 branch로 대상을 좁혀 **방향을 예측하는 대신 계산(compute)** 하고, 그 결과가 baseline prediction과 다르면 front-end에서 조기 re-steer를 거는 순수 하드웨어·ISA-투명 기법을 제안하는 것이다 (§1, §2.2, §3, §4).

### 2.1 Motivation과 characterization

논문의 characterization은 §3에서 100+ 워크로드(평가는 104개, §6) 위에 세워진다.

| # | 관측 | 수치 | 출처 |
|---|---|---|---|
| 1 | 전체 misprediction 중 conditional branch가 차지하는 비율 | 92% | §1 (§6의 워크로드 기준) |
| 2 | mispredicting branch를 dependence chain으로 분류했을 때 **3D-branch**(feeder load PC가 정확히 하나 + load 값과 branch 사이 연산이 단순·소수)의 비중 | **52%** | §1, §3.1, Fig. 3(a) |
| 3 | 나머지 범주 (Fig. 3(a)의 stacked bar, 위→아래) | "Branch dependent on multiple register comparison (Fig 2a)" ≈38%, "Multiple register operation in the dependency chain (Fig 2b)" ≈6%, "Too many/complex operations between load and branch" ≈5% — Fig. 3(a)에는 3D의 52% 외에 숫자 라벨이 없어 나머지는 막대 높이 판독값 | §3.1, Fig. 3(a) |
| 4 | 3D-branch가 파이프라인에 들어올 때 feeder load의 상태 (solid blue bar) | In Front-end **87.3%**, In ROB **0.5%**, Executed **3.5%**, Retired **7.2%** | §3.2, Fig. 3(b) |
| 5 | branch가 파이프라인에 들어오는 시점에 이미 load 값이 준비된 비율 (= Executed + Retired, striped green bar) | **10.7%** | §3.2, Fig. 3(b) |
| 6 | branch가 **OOO**에 들어오기 전까지로 창을 넓혔을 때 추가로 확보되는 비율 | **+1.5%** → 누적 **12.2%** (striped green bar 두 번째 막대 = 12.2%) | §3.2, Fig. 3(b) |
| 7 | 따라서 timeliness까지 반영한 **전체 기회** | 52% × 12.2% ≈ **6.7% of total mispredictions**. §1은 같은 값을 "our overall opportunity shrinks to 7% on average across the 100+ workloads"로 서술 | §1, §3.2 |
| 8 | feeder load와 3D-branch 사이의 slack이 커지는 원인 | Pipeline Flushes / Function Call / Other stalls의 세 범주. "most often pipeline flushes and function calls provide an opportunity to compute the branch direction before the branch enters the pipeline" | §3.2, Fig. 3(c) |
| 9 | load와 branch 사이 중간 연산 개수 분포 | **cmp/test 하나뿐**인 경우가 ≈82%, 1 op ≈9%, 2 op ≈5%, 3 op ≈2%, 4 op ≈1% (막대 판독값; 논문 본문은 "in most of the cases, there is only one intermediate computation (compare or a test)") | §3.3, Fig. 4(a) |
| 10 | 중간 연산의 종류 | compare, register move, 그리고 increment/decrement/addition/subtraction/shift 같은 단순 ALU 연산 — 모두 "involving the load and an immediate values". Fig. 4(b)의 범례는 Register move / Logical / Add-subtract / Shift-Rotate | §3.3, §4 (Fig. 4(b) 논의) |
| 11 | 3D-branch의 burstiness | 다음 4 cycle 안에 또 다른 3D-branch가 들어올 확률 약 20%(Fig. 4(c) 판독 ≈18–19%). 본문은 "it does not increase significantly if we increase the monitoring window to 20 cycles"라고 쓰지만, Fig. 4(c)의 막대 자체는 8/12/16/20 cycle에서 ≈29%/34%/38%/43%로 증가한다 — **본문 서술과 그림 판독값이 어긋난다** | §3.4, Fig. 4(c) |
| 12 | PC pairing vs. address/value pairing (MDP 설계 근거) | load-branch PC pair 하나가 평균 **160개 초과의 data value**와 **2800개 초과의 address**를 거치는 반면, load-store PC pair는 **최대 380개 data value / 260개 address** | §5.1 |

또한 §3은 SPEC2006 `gobmk`에서 뽑은 두 개의 실제 코드 예제를 제시해, load와 branch가 코드 배치상으로는 가깝더라도 중간 함수 호출(Example 1의 `Call F`)이나 그 사이 branch misprediction(Example 2의 `B3: je B6 – wrongly predicted taken`)이 둘의 파이프라인 진입 시점 사이를 벌려 준다는 점을 보인다 (§3).

3D-branch의 정의는 Fig. 2의 네 사례로 고정된다: (a) 두 개의 load가 CMP로 만나는 경우와 (b) 두 load가 ADD로 합쳐진 뒤 CMP되는 경우는 **3D가 아니고**, (c) `LD → R1; CMP R1,$5; JZ`와 (d) `LD → R1; ADD R1,$9 → R2; CMP R2,$4; JNZ`는 **3D이다**. 여기서 L1을 "feeder load"라 부른다 (§3, Fig. 2).

### 2.2 Key insights

- **misprediction의 절반은 "예측"이 아니라 "계산"으로 없앨 수 있는 형태다.** 전체 misprediction의 52%가 feeder load PC가 하나뿐이고 중간 연산이 단순한 3D-branch에서 나오므로, load 값만 손에 들어오면 최소한의 산술 로직으로 방향을 정확히 계산할 수 있다 — "once the load value is available, the branch direction can be computed independent of other instructions using minimal arithmetic logic" (§3, §3.1).
- **dependence chain을 순서만으로 표현할 수 있다.** 3D-branch는 정의상 load source가 하나이고 multiple-register operation을 포함하지 않으므로, chain을 `<Load PC, Op0, Op1…Opn, Br Op>`라는 순서 튜플로만 저장하면 되고 dataflow는 연산 순서에 암묵적으로 담긴다(Op0는 load 값에 의존, Op1은 Op0에 의존…). front-end에 일반적인 register renaming 상태를 복제할 필요가 없다 (§4.1).
- **계산 하드웨어는 아주 작아도 된다.** chain 대부분이 compare/test 하나뿐이고(Fig. 4(a)) 3D-branch가 몰려 오지 않기 때문에(Fig. 4(c)), 사이클당 한 연산만 in-order로 발행하는 초소형 BCU로 충분하다 — "there is marginal benefit from a more aggressive BCU design" (§3.3, §3.4, §4.4).
- **override 기회는 시간 의존적이므로 주입 지점이 하나여서는 안 된다.** load 값이 이미 준비된 경우에는 branch predictor 단계에서, 값이 늦게 도착하면 allocation queue 근처에서 다시 시도한다. 이 이중 주입이 이득의 상당 부분을 만든다 (§1, §4.1, §7.1).
- **3D-branch 탐지는 retire 시점에 ARF를 확장해 값싸게 할 수 있다.** front-end에서 dataflow를 분석하는 대신, 아키텍처 레지스터마다 1비트 "3D compliant" 마커와 생산 load PC, 누적 연산 리스트를 달아 retire 순서대로 전파한다 (§4.2, Fig. 6). 이 방식은 논문 스스로 EXACT의 predictor ID generation [9]에서 가져온 것이라 밝힌다.
- **남은 병목은 coverage가 아니라 load value의 timeliness다.** 그래서 value predictor(EVES)를 **load address predictor**로 용도 변경해 feeder load 값을 미리 prefetch하는 것이 세 최적화 중 압도적으로 큰 기여를 한다 — 3D 단독 5.4% MPKI에서 3D+LAP 12.7% MPKI로 (§5, §5.3, §7.1, Table 3).
- **conventional predictor를 키우는 것으로는 풀리지 않는다.** CBP-5 unlimited storage 예측기를 baseline으로 써도 3D-Branch Overrider는 여전히 9.2% MPKI / 1.3% IPC를 내며, 모든 워크로드 범주에서 MPKI reduction의 60–70%가 유지된다 (§1, §7.5, Fig. 10).

### 2.3 Hardware structures

Fig. 5(a)는 이 구조들이 modern OOO core 안 어디에 붙는지를 보여 준다. BT/BCU/FLT/LT/3D-Alloc Re-steer Table/Prefetch Load Tracker는 front-end 쪽에, ARF Extension은 retire(ARF) 쪽에 놓인다.

| 구조 | 목적 | 구성 (entry 수 / associativity / indexing / 파이프라인 위치) | per-entry 필드와 비트폭 |
|---|---|---|---|
| **3D-Branch Tracker Table (BT)** | 자주 mispredict하면서 3D 조건을 만족하는 branch와, 학습된 dependence chain·confidence·계산된 방향을 보관. branch가 front-end에 들어올 때 조회되어 baseline TAGE-SC-L prediction을 override할지 결정 | **128 entries, 8-way set-associative**, branch PC 하위 비트로 index, **age and usefulness-based replacement**. front-end의 BPU 옆(Fig. 5(a)). 할당/학습은 **branch retire 시점**에 ARF extension으로부터 (§4.1, §4.2) | Fig. 5(a) entry format: `BR PC │ # MSP │ CONF │ LD PC │ <Op 0, Op 1, … Op n> │ LT index │ Pred`. §4.5 기준 메타 필드 = valid 1 b + branch PC tag 8 b + per-PC misprediction 카운트 4 b + confidence 3 b + **LT index 7 b** + computed prediction 1 b = 24 b(3 B). Dependence chain = Load PC 16 b + compute operation당 1 B + immediate operand당 2 B + 최종 branch instruction 1 B ⇒ 연산 4개까지 저장 시 **15 B**. 총 **18 B/entry**, 128 entries = **2.25 KB** |
| **3D Alloc Re-steer Table** | BPU 단계 창을 놓친 override의 계산 결과를 담아 두었다가, 3D-branch가 allocation queue를 지날 때 re-steer를 재시도하게 함 | **direct-mapped, 128 entries**, **LT index로 index**. 3D-branch가 allocation queue에 들어가거나 나올 때, 명령어가 들고 다니는 LT index로 읽는다 (§4.1) | 1-bit computed branch direction + valid bit. 128 entries = **0.03 KB** |
| **ARF Extensions** | retire 시점에 3D-branch를 탐지: 정확히 하나의 feeding load와 단순 연산만 거친 branch를 식별하고, load PC와 순서 있는 연산 리스트를 BT로 보냄 | 추적 대상 아키텍처 레지스터마다 1 entry. x64는 **general-purpose register 16개 + flag register 1개 = 17 entries**. 명령어 retire 시 갱신되며 3D compliant 비트가 서 있는 동안에만 다음 레지스터로 전파. x64 conditional branch는 flag register만 읽으므로 flag register에도 3D compliant 비트를 추적 (§4.2, Fig. 6) | Fig. 5(a)/Fig. 6 format: `REG N │ LD PC │ Op 0 │ Op 1 │ … │ Op n` + **3D compliant 1 b**. **14 B/entry**, 17 entries = **0.23 KB** |
| **Feeder Load Tracker (FLT)** | load가 파이프라인에 들어올 때 그 load PC가 어떤 BT entry의 feeder인지를 판정 — BT 128 entry 전체를 CAM 검색하는 대안(scalability·power 문제)을 피하기 위한 선택 | **BT와 동일하게 구성(128 entries)**, **load PC로 index**. 새 3D-branch가 BT에 할당되려 할 때도 조회하여, 그 feeder load가 이미 여러 BT entry를 추적 중이면 해당 BT entry들의 confidence 비트로 신규 할당 여부를 결정 (§4.3) | load PC tag **8 b** + BT entry 2개를 가리키는 **14 b** (sensitivity study 결과 FLT entry당 BT entry 2개만 추적해도 대부분의 이득 유지). 128 entries = **0.34 KB** |
| **Load Tracker (LT)** | 같은 load PC의 여러 in-flight dynamic instance를 구분해, 들어오는 3D-branch를 **youngest instance**에 올바르게 묶고, load 완료 시 관련 BT entry를 찾게 함 | **128-entry set-associative**. FLT에 hit한 load가 파이프라인에 들어올 때 LT entry가 배정되고, LT index는 load를 따라 파이프라인을 내려가 back-end에서 **Load Buffer(LB)** 에 저장된다. 동시에 BT entry에도 기록되어 branch가 값을 얻는 데 쓰인다 (§4.3) | **FLT로의 index 7 b** + valid/replacement용 부가 정보 **4 b**. 128 entries = **0.17 KB** |
| **Branch Computation Unit (BCU) + Compute Buffer (CB)** | feeder load 값과 기록된 연산 chain으로 3D-branch 방향을 실제로 계산 | front-end의 BT 옆(Fig. 5(a)), 마이크로아키텍처는 Fig. 5(b). **16-entry Compute Buffer**는 단순 in-order 큐. BT의 포맷을 내부용 단순 포맷으로 확장해 채운다. **사이클당 한 연산만** 트리거하며 서로 독립인 연산도 직렬화·in-order 스케줄. Datapath: operand MUX ← bypass logic, 기능 블록 **Comparator / Shifter / Add-Sub / Logical**, latch와 bypass로 dependent operation에 결과 전달, Decoder/Demux가 Compute-done과 계산된 방향을 낸다. **가득 차면 stall하고 들어오는 3D Branch Computation 요청을 drop** (§4.4) | CB entry: operation + branch opcode 각 **1 B**, **OP1 8 B**(load 값이거나 직전 계산 결과를 가리키는 비트), **OP2 2 B**, compute-done **1 b**. 16 entries ≈ **0.18 KB**. Fig. 5(b) CB 컬럼: `OPER │ OP1 │ OP2 │ Br OP │ Compute Done`, "2 lines for 2 Operands" |
| **Pipeline extensions (fetch queue / allocation queue / Load Buffer)** | front-end에서는 각 명령어가, back-end에서는 LB가 LT index를 들고 다니게 하여 완료된 load를 올바른 BT entry·3D-branch instance에 매칭 | fetch queue, allocation queue, LB 확장 (§4.1, §4.3) | LT entry 추적을 위한 추가 **0.4 KB** (§4.5) |
| **Memory Dependence Predictor (MDP) + Value File** *(optional, §5.1)* | feeder load가 아직 완료되지 않았지만 그 load PC와 강하게 상관된 producer store PC가 식별되면, store의 데이터를 그대로 branch 방향 계산에 사용. address/value가 아니라 **store PC–load PC pairing** | store/load cache module: **8-way, 1024 entries**. Value File: **128 entries** (store 값 보관). MDP 자체는 [40](Tyson & Austin, 1997)을 인용 | store/load cache entry당 tag **8 b** ⇒ **1 KB**; Value File **1 KB**. 합계 **2 KB** |
| **Load Backslice Table (LBT)** *(optional, Load Prioritization, §5.2)* | feeder load의 backslice(= feeder load가 의존하는 명령어들)를 기록해 두었다가 그들을 우선 실행시켜 주소 계산과 load 값을 앞당김. branch를 지연시키는 대안은 뒤따르는 모든 명령어를 stall시키므로 배제 | **FLT와 유사하게 구성, 128 entries**. feeder load가 파이프라인에 들어올 때 backslice 정보를 읽어 해당 명령어들을 실행 우선순위로 올린다. dependence chain 포착 모듈은 선행 연구 [10, 12]에서 차용 | PC당 하위 **8 b**만, feeder load당 **최대 8개 PC**. 128 entries × 8 PC × 8 b = **1 KB** |
| **Load Address Predictor (LAP) + Prefetch Load Tracker** *(optional, §5.3)* | feeder load의 주소를 예측해 값을 미리 prefetch, branch가 OOO에 들어가기 전에 방향 계산과 re-steer가 가능하게 함 | **EVES value predictor [34]를 load address predictor로 morph**: load PC로 index, **last-value component + global-branch-history component**, **stride component는 disable**해 단순화. 주소 예측 가능성이 높은 feeder load PC가 파이프라인에 들어오면 load prefetch를 발생시키고, 그 요청을 **LT와 유사한 Prefetch Load Tracker(32 entries, Fig. 5(a)에 옅은 회색)** 로 추적. timeliness 기준으로 load PC를 필터링하고 confidence로 게이팅 | **384-entry data table**(load address) + 더 작은 **256-entry predictor array**. 여러 load address가 상위 32비트(MSB)를 공유한다는 관찰로 **8-entry MSB 테이블**을 두고 data table에는 **3-bit entry id**만 저장 ⇒ LAP **2.41 KB** + Prefetch Load Tracker **0.05 KB** = **2.46 KB** |

### 2.4 Operation — stage by stage

**(1) 학습 — retire 단계 (ARF Extensions, §4.2, Fig. 6).**
명령어가 retire할 때마다 destination 아키텍처 레지스터의 ARF extension entry를 갱신한다. 레지스터가 load의 destination이면 3D compliant 비트가 set되고 그 load PC가 기록된다. 이후 연산은 리스트에 순서대로 append되며, **3D compliant 비트가 서 있을 때만** load PC와 연산 리스트가 다음 레지스터로 전파된다. 비트가 reset되는 조건은 세 가지다 — (i) 그 레지스터가 **multiple register를 사용하는 compute/comparison의 destination**일 때, (ii) 중간 연산이 **복잡할 때(multiplication, division 등)**, (iii) branch에 도달하기 전 **선행 연산 수가 최대 임계값(Fig. 5(a)의 Op-n)에 도달**했을 때. 어떤 레지스터의 비트가 reset되면 "its dependent instructions will also not be 3D compliant". 같은 레지스터가 서로 다른 두 load PC의 destination이 되면 **더 최신 load PC가 ARF extension entry를 덮어쓴다**. x64에서 conditional branch는 flag register만 읽으므로, branch 직전의 compare/test가 3D compliant면 그 상태가 flag register로 전달되고, branch retire 시 flag register의 비트로 3D-branch를 판정한다. 판정되면 **branch PC, load PC, 연산 집합**을 BT에 추가한다.

Fig. 6의 예시가 이 규칙을 그대로 보여 준다.

```
[3D compliant — BT 갱신됨]                  [3D 아님 — BT 갱신 없음]
A: LD        → R0     ARF-ext R0: 1, A          A: LD        → R0     R0: 1, A
B: ADD R0,$1 → R1     ARF-ext R1: 1, A, +1      B: ADD R0,$1 → R1     R1: 1, A, +1
C: LSH R1,1  → R2     ARF-ext R2: 1, A, +1, <<1 C: LSH R1,R2 → R2     R2: 0   ← multiple register op
D: CMP R2,$6          Flag  : 1, A, +1, <<1, -$6 D: CMP R2,$6         Flag: 0
E: JZ  D              ⇒ Update 3D-Branch Tracker E: JZ  D             ⇒ No Update
```

BT에 기록할 때의 최적화 하나: **register move는 제거하고 값을 다음 명령어의 operand로 직접 넘긴다** (§4.1).

**(2) load 진입 — FLT 조회와 LT 배정 (§4.3).**
load가 파이프라인에 들어오면 그 PC로 FLT를 조회한다. hit이면 그 dynamic instance에 **LT entry**를 배정하고, LT entry에는 **FLT로의 index**를 저장한다. LT index는 (a) load를 따라 파이프라인을 내려가 back-end에서 LB에 저장되고, (b) 해당 BT entry에도 기록되어 branch가 값을 얻을 때 쓰인다. 같은 load PC의 dynamic instance가 동시에 여러 개 떠 있을 수 있으므로 이 기록이 "correctly associate the incoming 3D-branch to the youngest instance of the feeder load"를 보장한다. 또한 **LT index가 새 load에 배정되는 순간 대응하는 3D Alloc Re-steer Table entry의 valid 비트가 reset된다** (§4.1).

**(3) 계산 — load 완료 시 BCU 트리거 (§4.1, §4.4).**
load가 execution을 마치면, **대응하는 3D-branch가 파이프라인에 들어왔는지 여부와 무관하게** BCU를 트리거한다("As soon as the load completes execution, irrespective of whether the corresponding 3D-branch has entered the pipeline or not"). 순서는 LB에 저장된 LT index → 그 **LT entry에 들어 있는 FLT index** → 관심 BT entry 식별이다 (§4.3: "we use FLT index stored in the LT to identify the BT entries of interest"). BT에서 `<Load PC, Op0, …, Opn, Br Op>`를 읽어 BCU 내부 포맷으로 확장한 뒤 CB에 버퍼링하고, 사이클당 한 연산씩 Comparator/Shifter/Add-Sub/Logical을 통과시키며 bypass로 다음 연산에 값을 넘긴다. 마지막 Br Op(JZ, JNZ 등)이 계산된 방향을 만들고 Compute Done이 선다. Fig. 5(b)의 예시는 두 3D-branch가 필요로 하는 세 연산이 3 cycle에 끝나는 모습이다.

```
CB (16 entries)          OPER      OP1       OP2   Br OP  Compute Done
  Cycle 1                Cmp       Ld Val1   $5    JZ     1
  Cycle 2                Add       Ld Val2   $9           0
  Cycle 3                Cmp       Res       $4    JNZ    0
      → 두 3D-branch의 세 연산이 3 cycle에 완료 (§4.4, Fig. 5(b))
      → CB가 full이면 stall하고 들어오는 3D Branch Computation 요청을 drop
```

**(4) Override 경로 1 — branch predictor 단계 (§4.1).**
branch가 파이프라인에 들어오면 branch PC로 BT를 검색한다. hit이고 계산된 방향이 준비돼 있으면 그 값을 내놓고, baseline branch prediction과 다르면 **즉시 pipeline re-steer**를 걸어 올바른 경로에서 fetch를 시작한다 (Fig. 1/Fig. 5(a)의 "Override at BPU" / "Re-Steer Flush from BPU").

**(5) Override 경로 2 — allocation queue (§4.1).**
load 실행 지연이나 중간 계산에 드는 추가 사이클 때문에 BPU 단계의 창을 놓칠 수 있다. 이때는 **BT의 LT index를 돌아온 load data의 LT index와 비교**한다. 일치하지 않으면 BPU 단계 기회를 놓친 것이므로, LT index와 계산된 방향을 **3D Alloc Re-steer Table**로 보내 해당 entry를 채우고 valid 비트를 set한다. 3D-branch가 allocation queue에 들어가거나 나갈 때 자신이 들고 온 LT index로 이 테이블을 읽고, 계산된 방향과 예측된 방향이 다르면 **allocation stage에서 re-steer flush**를 건다 (Fig. 5(a)의 "Re-steer At Alloc" / "Re-Steer Flush from Allocation stage").

**(6) 최적화 경로들 (§5).**
- MDP: load가 아직 완료되지 않았지만 해당 load PC에 대해 강하게 상관된 producer store PC가 식별되면, 그 store의 데이터를 방향 계산에 직접 투입 (§5.1).
- Load Prioritization: feeder load가 파이프라인에 들어올 때 LBT에서 backslice를 읽어 그 명령어들을 실행 우선순위로 올려 주소 계산과 load 값을 앞당김 (§5.2).
- LAP: 주소 예측 가능성이 높은 feeder load PC가 파이프라인에 들어올 때 load prefetch를 발생시키고 Prefetch Load Tracker로 추적. prefetch된 데이터가 도착하면 방향을 계산하고 필요 시 re-steer (§5.3).

### 2.5 Correctness와 speculation 제어

**아키텍처적으로는 완전히 비투기적인 override 기법이다.** §4는 "It is to be noted that the 3D-Branch Overrider is designed as an override mechanism to the branch predictor's prediction and is not an enhancement to the branch predictor itself"라고 못박는다. 3D-branch 자체는 back-end에서 정상적으로 실행·해소되므로, override가 틀렸더라도 평소의 execution-stage branch misprediction flush가 그대로 바로잡는다. 이 기법이 바꾸는 것은 **비싼 execution-stage flush를 값싼 front-end re-steer로 대체**하는 것뿐이다. Fig. 1과 Fig. 5(a)는 세 종류의 이벤트 — "Branch Misprediction Flush", "Re-Steer Flush from BPU", "Re-Steer Flush from Allocation stage" — 를 명시적으로 구분한다. §6은 지표 정의에서 이를 다시 확인한다: "MPKI reduction here refers to the number of expensive branch-misprediction related pipeline flushes triggered from the execution stage that are saved. These are replaced instead by low-latency front-end re-steer events."

**Instance matching (§4.1, §4.3).** 같은 load PC의 dynamic instance가 동시에 여러 개 존재할 수 있으므로 LT가 branch를 youngest instance에 묶는다. LT index는 load(→LB)와 branch(→fetch/allocation queue) 양쪽을 따라다닌다. 3D Alloc Re-steer Table에서 **valid 비트는 LT index가 새 load에 배정될 때 reset되고 계산된 방향이 도착해야만 set**되며, 인덱싱 자체가 LT index로 이루어진다 — "Using LT index ensures that correct overriding prediction is used for the 3D-branch."

**Detection safety (§4.2).** ARF extension의 3D compliant 비트는 multi-register compute/compare, 복잡 연산(multiplication, division), 연산 수 임계값 초과에서 reset되고, reset된 레지스터의 dependent instruction도 3D compliant가 아니게 된다. BCU가 충실히 평가할 수 없는 chain은 애초에 BT에 기록되지 않는다.

**Speculation 제어 — confidence counter (§4.1, §5.1, §5.3).** BT의 3-bit confidence가 override와 prefetch 발행을 게이팅한다.
- FLT entry가 이미 여러 BT entry를 추적 중이면 새 3D-branch의 BT 할당 여부를 이 confidence 비트로 결정한다 (§4.3).
- MDP 경로: "To limit the wrong re-steers through this technique, besides updating the store-load pairs in MDP, we conservatively also lower the confidence counters in the 3D-branch table" (§5.1).
- LAP 경로: 파이프라인에 feeder load와 같은 주소에 쓰는 in-flight store가 있으면 prefetch된 데이터가 부정확할 수 있다. "We lower the confidence field in the BT on those feeder loads once such wrong re-steers are observed." 반대로 prefetch된 값으로 올바른 override가 나오면 confidence를 올려, "only the 3D-branches that have high confidence continue to issue load prefetches" (§5.3).

**BCU overflow (§4.4).** CB가 가득 차면 stall하고 들어오는 계산 요청을 drop한다. 이는 최적화 기회의 손실일 뿐 정확성 문제가 아니다. 논문은 "since the load value (on completion) only gets stored in the CB entries, we want to make sure the computation requests are rarely dropped"는 이유로 16 entry를 골랐다고 밝힌다.

**메모리 순서(memory ordering).** 논문은 위의 confidence 기반 완화 외에 별도의 memory-ordering 하드웨어를 기술하지 않는다. LAP prefetch가 store와 충돌해 얻은 잘못된 값 문제는 confidence 하향으로만 다룬다 (§5.3).

### 2.6 Parameters와 storage budget

**논문이 고른 값 (§4.5, §5.4, §7.3):**

| 항목 | 값 | Sensitivity로 스윕한 범위 (Fig. 9) |
|---|---|---|
| BT entries / associativity / replacement | 128 / 8-way / age and usefulness-based | 크기 64·128·256, associativity 2·4·8·16 |
| BT chain 저장 한도 | 연산 4개 + load PC + branch instruction (15 B) | — |
| 3D Alloc Re-steer Table | direct-mapped, 128 entries | — |
| ARF extension entries | 17 (x64 GPR 16 + flag 1), 14 B/entry | — |
| FLT | 128 entries, FLT entry당 BT entry 2개 추적 | — |
| LT | 128 entries, set-associative | 64·128 |
| Compute Buffer (BCU) | 16 entries, 1 op/cycle in-order | — |
| Prefetch Load Tracker | 32 entries | 16·32·48 |
| LAP | 384-entry data table + 256-entry predictor array + 8-entry MSB table | LAP Entries 256·512 |
| MDP store/load cache | 8-way, 1024 entries | — |
| Value File | 128 entries | — |
| LBT | 128 entries, PC당 8 b, feeder load당 8 PC | — |

**Baseline 3D-Branch Overrider storage 내역 (§4.5):**

| 구성 요소 | 계산 | 용량 |
|---|---|---|
| 3D-Branch Tracker Table (BT) | 18 B × 128 | 2.25 KB |
| 3D Alloc Re-steer table | (dir + valid) × 128 | 0.03 KB |
| ARF Extensions | 14 B × 17 | 0.23 KB |
| FLT | (8 b tag + 14 b) × 128 | 0.34 KB |
| LT | (7 b + 4 b) × 128 | 0.17 KB |
| CB in BCU | ~12 B × 16 | 0.18 KB |
| Pipeline extensions (fetch/alloc queue + LB의 LT entry) | — | 0.4 KB |
| **논문이 제시한 총합** | "Overall, 3D-Branch Overrider requires **3.2 KB**." | **3.2 KB** |

> **내부 불일치 주의.** 위 7개 항목을 그대로 더하면 2.25+0.03+0.23+0.34+0.17+0.18+0.4 = **3.60 KB**이고, **pipeline extensions 0.4 KB를 빼야** 정확히 3.20 KB가 된다. 논문의 총합 3.2 KB와 Table 3의 모든 합계(3.2/5.2/4.2/5.66/7.66/6.66 KB)는 0.4 KB를 포함하지 않은 값과 일치한다. 논문은 이 차이를 언급하지 않는다. (§4.5는 항목 번호도 "5) CB in BCU"와 "5) Pipeline extensions"로 중복되어 있다.)

**최적화별 추가 storage (§5.4, 모두 3.2 KB 위에 얹힌다):**

| 최적화 | 내역 | 추가 용량 |
|---|---|---|
| Memory Dependence Prediction | store/load cache 8-way 1024 entries × 8 b tag = 1 KB + Value File 128 entries = 1 KB | **2 KB** |
| Load Prioritization | LBT 128 entries × 8 PC × 8 b tag | **1 KB** |
| Load Address Prediction | LAP 2.41 KB + Prefetch Load Tracker 32 entries 0.05 KB | **2.46 KB** |

**구성별 총 storage (Table 3):** 3D = 3.2 KB · 3D+MDP = 5.2 KB · 3D+Load prioritization = 4.2 KB · **3D+LAP = 5.66 KB** · 3D+LAP+MDP = 7.66 KB · 3D+LAP+Load prioritization = 6.66 KB.

> **Abstract**만이 headline 구성을 "just **5.7KB** in additional storage"로 반올림해 인용한다(§1 contribution 2와 §9는 같은 구성의 12.7%/3.1%만 인용하고 저장 용량은 적지 않는다). Table 3은 같은 구성을 **5.66 KB**로 적는다.

논문은 "opcode and other instruction metadata is available with the ROB entry"를 가정하며, 이 가정은 [9, 28]을 따른다고 밝힌다 (§4.5).

### 2.7 Evaluation setup과 results

**시뮬레이터·코어 구성 (§6, Table 2).** in-house cycle-accurate simulator로 **Icelake-like x64 core를 3.2 GHz**에 모델링.

| 항목 | 값 |
|---|---|
| Core | 5-wide OOO, **352-entry ROB**, **128-entry Load Buffer**, **72-entry Store Buffer**, **128-entry Allocation Queue** |
| Baseline Branch Predictor | **TAGE-SC-L 64 KB** [33] + **ITTAGE** [31], **15 cycles misprediction penalty** |
| Branch Target Buffer | 8K entries |
| L1 cache | Private, 48 KB, 64 B line, 8-way; Instruction-pointer-based stride prefetcher |
| L2 cache | Private, 512 KB, 64 B line, 8-way; Streamer prefetcher |
| LLC | Shared, Inclusive, 8 MB, 64 B line, 16-way |
| Main Memory | Dual channel DDR4-2133MHz |

**워크로드 (§6, Table 1).** **104개 워크로드**, 7개 범주:

| Category | Description | Count |
|---|---|---|
| Cloud | Data analytics on Hadoop, Data streaming using Spark, BigBench, Transactional processing using Cassandra | 13 |
| Enterprise | SPECjbb, Web search, Particle rendering | 7 |
| ISPEC | ISPEC06 & ISPEC17 | 28 |
| Multimedia (MM) | Video Games, Photo-editing, Animation, Video conversion, Mediaplayer | 12 |
| SYSmark | SYSmark 2014 | 8 |
| Personal Computing | Email, Voice-to-text tools, Image converters, HTML backend workload, Geekbench | 11 |
| Web Browsing | Firefox, JavaScript workloads | 25 |

**Simpoint-like methodology [21]** 로 대표 구간을 뽑고, 벤치마크 스위트는 reference input, 실제 워크로드는 실사용 입력을 쓴다. **선택 편향은 명시적**이다: "We study workloads that are likely to be bottlenecked by branch mispredictions (had a high MPKI (>2)), and exhibit a modest fraction of these mispredictions arising from 3D-branches."

**지표 정의 (§6).** IPC gain과 MPKI reduction 모두 64 KB TAGE-SC-L + ITTAGE baseline 대비. 단, 이 기법은 predictor가 아니므로 **MPKI reduction = execution stage에서 발생하는 비싼 branch-misprediction pipeline flush 중 절약된 개수**를 뜻하며, 그것들은 low-latency front-end re-steer로 대체된다.

**Headline 결과 (Table 3, §7.1–§7.2, 모두 위 baseline 대비):**

| Configuration | MPKI Reduction | IPC Gain | Storage |
|---|---|---|---|
| 3D | 5.4% | 0.98% | 3.2 KB |
| 3D + MDP | 5.4% | 0.98% | 5.2 KB |
| 3D + Load prioritization | 5.4% | 1.4% | 4.2 KB |
| **3D + LAP (default 구성)** | **12.7%** | **3.1%** | **5.66 KB** |
| 3D + LAP + MDP | 12.8% | 3.1% | 7.66 KB |
| 3D + LAP + Load prioritization | **12.9%** | **3.6%** | 6.66 KB |

즉 **이득의 대부분은 LAP에서 나온다.** Abstract가 인용하는 "12.7% reduction in branch mispredictions resulting in 3.1% IPC gain while needing just 5.7KB"는 3D+LAP 구성이다(§1·§9는 같은 구성의 12.7%/3.1%만 인용한다).

**세부 결과:**

- **3D 단독 (§7.1).** 104 워크로드 평균 약 5.4% MPKI / 0.98% IPC. `gzip` 같은 워크로드는 IPC를 최대 **6%** 얻는다. Personal Computing 범주가 특히 민감한데, geekbench·HTML backend·compression 워크로드가 executed load로부터의 3D-branch override로 큰 이득을 본다. §3에서 다룬 `gobmk`는 **4.2% MPKI reduction → 1.44% IPC gain**.
- **re-steer가 실제로 어디서 발생하는가 (§7.1, Fig. 8(a)).** 전체 baseline misprediction 대비 re-steer된 misprediction 비율은 **3D-Branch Overrider 단독 ≈4.8%**, **3D+LAP ≈11.9%**(막대 판독값). 범주는 6가지: {Branch Predictor, Allocation Queue Write, Allocation Queue Read} × {Executed load, Prefetched load}. LAP 없이는 "most of the re-steers are from the branch prediction stage while having the flexibility to re-steer at the allocation queue does provide additional gains"이고, LAP를 붙이면 추가분의 지배적 기여가 **"Allocation Queue Write, Prefetched load"** 이다. 이 맥락에서 저자들은 "The flexibility built into the 3D-Branch Overrider to inject overrides at multiple pipeline stages in the front-end helps significantly to enhance the overall gains"라고 결론짓는다.
- **MDP (§7.1).** 기본 구성에서는 유의미한 이득이 없다(Table 3에서 3D 단독과 동일). 개별 워크로드 **445.gobmk(SPEC06) 2.1%**, **631.sjeng(SPEC17) 3.5%** IPC만 민감. store PC가 load PC에 상대적으로 가까울 때 정확하고 대부분의 기회를 포착한다. ROB·LB·SB에 매우 민감하여, 이들을 **기본 구성의 2배로 확대하면 3D+MDP가 100+ 워크로드에서 1.1% IPC gain**을 낸다.
- **Load Prioritization (§7.1).** IPC gain이 1.4%로 오르지만 MPKI 변화는 유의미하지 않다. Cloud와 Web Browsing 범주가 민감.
- **LAP (§7.1, Fig. 7(a)).** 거의 모든 범주가 MPKI를 **5% 이상** 줄이고 **web-browsing은 20% 초과**(Fig. 7(a) 판독 ≈23.7%). 범주별 3D+LAP MPKI reduction 판독값: Cloud ≈15.3%, Enterprise ≈5.7%, MM ≈10.2%, ISPEC ≈10.7%, PC ≈20.3%, SYSmark ≈5.9%, Web Browsing ≈23.7%, All 12.7%. IPC gain 판독값: Cloud ≈3.7%, Enterprise ≈1.1%, MM ≈2.0%, ISPEC ≈3.1%, PC ≈4.7%, SYSmark ≈1.3%, Web Browsing ≈3.5%, All 3.1%.
- **LAP 검증 (§7.1–§7.2, Fig. 8(b)).** LAP **prediction accuracy 99.7%** (모든 범주), **coverage는 3D-branch의 14%**. read request는 평균 약 **2.3%** 증가하고 어떤 범주도 전체 memory traffic **3.2%** 초과 증가를 보이지 않는다.
- **LAP 대조 실험 (§7.1) — 이득의 출처 증명.** 3D-Branch Overrider 위에 LAP를 얹되 **LAP 기반 re-steer는 끈** 구성(그림에 없음)은 baseline 대비 **0.93% IPC**로, **3D-Branch Overrider 단독(0.98%)보다도 나쁘다.** 원인은 L1 cache bandwidth 경합 증가. "This shows that LAP based re-steers are key to getting higher performance."
- **S-curve (§7.1, Fig. 8(c)).** Cloud 범주의 **K-means clustering이 IPC 13%** (MPKI 30% 감소)로 최대. **18개 워크로드가 4% 초과 IPC gain**을 보이며 여기에는 401.bzip2(SPEC06), hadoop-spark(Cloud), jetstream(web-browsing)이 포함된다. Fig. 8(c)에는 opera·gobmk도 라벨링되어 있고, 하위 꼬리에는 소폭의 음수 구간이 존재한다.
- **Sensitivity (§7.3, Fig. 9).** BT size(64/128/256), BT associativity(2/4/8/16), LT size(64/128), Prefetch LT size(16/32/48)는 모두 IPC를 대략 3.0–3.2% 범위(막대 판독 ≈3.01–3.16%) 안에서만 움직인다("each of the optimizations provide modest gains"). 유일하게 큰 차이는 LAP entries로, **3.1% → 3.5%**. 다만 본문은 이를 "scaling the LAP predictor to **256 entries (3.2KB)**"라고 쓰는 반면 **Fig. 9의 x축은 LAP Entries를 256과 512로 스윕하며 3.45%에 가까운 막대는 512 쪽**이고 256 쪽은 default와 같은 ≈3.1%이다 — 본문과 그림이 어긋난다. §5.4의 default LAP 역시 "256-entry predictor array"로 기술되어 있어 본문의 "256 entries"는 default와 구분되지 않는다.
- **Future cores (§7.4, Fig. 10).** baseline Icelake-like 코어를 약 1.5x/2x로 확대하면 IPC gain이 **3.1% → 3.7% → 4.3%** 로 증가한다(§1, §7.4). Fig. 10에서 MPKI reduction은 세 구성 모두 ≈12.4–12.7%로 거의 평평하다. 본문 §7.4는 "from 3.1% to 4.3% for the 2x configuration"만 언급하고, 1.5x의 3.7%는 §1과 Fig. 10에 나온다.
- **매우 큰 baseline predictor (§7.5, Fig. 10).** CBP-5 winner의 **unlimited storage** 구성을 baseline predictor로 두어도 3D-Branch Overrider는 **9.2% MPKI reduction, 1.3% IPC gain**을 낸다. 모든 워크로드 범주에서 원래 MPKI reduction의 **60–70%가 유지**된다. 중요한 맥락: 그 unlimited predictor 자체가 이미 **64 KB CBP-5 winner 대비 misprediction을 40% 줄인** 상태다.

### 2.8 저자가 밝힌 limitation과 self-positioning

**저자가 인정한 한계:**

- **Timeliness가 근본적 상한.** "it is often the case that the feeder loads occur close to the branches in the program and therefore might not have their values available in time... our overall opportunity shrinks to 7% on average across the 100+ workloads" (§1). 정량적으로는 3D-branch misprediction의 **12.2%** 만이 branch가 OOO에 들어가기 전 load 값을 확보하고, 전체 misprediction 기준 기회는 **6.7%** 다 (§3.2).
- **범위 자체가 좁게 설계됨.** multiple load에 의존하거나, dependence chain에 multiple-register operation이 있거나, load–branch 사이 연산이 너무 많거나 복잡(multiplication, division)한 branch는 처음부터 대상이 아니다 (§3, §3.1, §4.2, Fig. 2(a)/(b), Fig. 3(a)).
- **BCU의 의도적 단순화.** 독립적인 연산도 직렬화하며 사이클당 한 연산만 발행한다 — "While this simple design increases the latency to compute a branch direction, we find in practice that most branches only require a simple compare/test and hence there is marginal benefit from a more aggressive BCU design" (§4.4). CB가 full이면 들어오는 계산 요청을 drop한다 (§4.4, Fig. 5(b)).
- **chain 길이 한도.** BT는 연산 4개 + load PC + branch instruction까지만 저장하고, ARF extension은 Op-n 임계값을 넘으면 3D compliant 비트를 끈다 (§4.2, §4.5).
- **MDP의 실패.** 기본 구성에서 "does not provide significant gains"이고, ROB/LB/SB 크기에 "quite sensitive"하여 이들을 2배로 키워야 1.1% IPC가 나온다 (§7.1).
- **Load Prioritization에 대한 자기 부인.** "we realized that almost all the gains are purely from accelerating the feeder load backslice but only a small portion translated into an override from the 3D-Branch Overrider. While this is an interesting observation, **we do not claim novelty here**" (§7.1).
- **LAP의 비용과 부정확성.** 추가 load access가 memory pipeline bandwidth를 소모하므로 "we have to be very stringent and only use the prefetcher when the 3D-overrider is highly confident" (§5.3). prefetch는 baseline read request의 latency에 부정적 영향을 줄 수 있고(§7.2), 실제로 re-steer 없이 prefetch만 하면 IPC가 오히려 나빠진다(0.93% < 0.98%, L1 bandwidth 경합, §7.1). 같은 주소에 쓰는 in-flight store가 있으면 prefetch된 데이터가 부정확할 수 있어 confidence를 낮춘다 (§5.3).

**Future work / 확장 여지:** LAP는 EVES [34]로 구현했지만 "our proposal will work with any other load address predictor such as [29] as well" (§5.3). 또한 [10]·[17] 같은 misprediction-penalty 감소 기법들은 "complementary to our proposal and can be implemented on top of the 3D-Branch Overrider for additional gains" (§8).

**Related work에서의 자기 위치 (§2.2, §8):**

- **데이터 값 상관 기반 예측기 [11, 16, 19, 22, 39]** — 선행 연구가 존재하지만 "when data has high entropy, it affects their accuracy" (§8).
- **EXACT [9]** — data-dependent branch instance를 feeder load의 **주소**로 구분하는 branch prediction 기법. 주소가 prediction time에 없으므로 훨씬 전에 retire한 branch instance를 proxy identifier로 쓰는데, "Using this proxy prior instance can be inefficient since contexts can vary across instances, changing the load value which in turn can change the branch direction". 또 prior store에서 consumer load로 값을 forwarding하는데, "the set of distinct load-branch and store-branch pairs that need to get tracked quickly increase the storage requirement over 10KB"이고 "As mentioned in [9], EXACT predictor does not win over an similarly sized TAGE predictor" (§2.2). §8: "Unlike the EXACT predictor, our work uses the load value to compute the branch direction and accomplishes this at much less storage." — 다만 **차용도 명시한다**: 3D-branch 탐지용 ARF 확장은 "a scheme similar to EXACT predictor ID generation [9]"이며, load address 대신 load PC와 연산을 추적한다는 점만 바꿨다 (§4.2).
- **SLB (Store-Load-Branch predictor) [18]** — data-dependent branch에 값을 공급하는 store를 추적하고, 프로파일링과 컴파일러로 ISA 확장을 통해 branch를 표시. load/store hint instruction과 추가 compare instruction을 프로그램에 삽입해야 하며 약 8KB를 요구. "3D-Branch Overrider, on the other hand, is a completely hardware-driven solution with a lower storage requirement and can transparently be integrated in any modern processor's pipeline" (§2.2).
- **Control Flow Decoupling (CFD) [36]** — load를 루프 밖으로 hoist해 load와 branch 사이 slack을 만들고 prediction queue로 dependent branch에 공급. §8은 SLB/CFD와의 두 가지 차이를 든다: (1) "our work proposes a hardware-only technique", (2) "unlike SLB, which used the memory address prediction to forward the data from a prior store (producer) to the data-dependent branch (consumer), our work utilized it to prefetch the data from memory subsystem. Since the producer stores happen much earlier than loads, the storage overhead to track these values is higher as compared to using the load to read the value in timely manner."
- **dependence-tracking 예측기 [16, 22]** — dependence chain을 추적하다가 일부 레지스터가 ready가 되면 predictor table을 조회해 **prediction**을 얻는 방식. 즉 branch predictor를 강화하는 것이지 방향을 계산하는 것이 아니다 (§8).
- **misprediction penalty 감소 계열 — Branch Outcome Anticipation [17]과 unconfident branch slice 우선 issue [10]** — 목표는 같지만 "our approach is distinctive in two ways: (1) they do not examine the data-dependence branches as the focus of design, and (2) if the load execution is delayed in the pipeline, 3D-Branch Overrider can still use the LAP scheme to prefetch the load value to perform the override" (§8). Load Prioritization 자체는 [10, 12]에서 차용했다고 §5.2에서 밝힌다.
- **MDP**는 [40](Tyson & Austin, 1997)의 memory dependence prediction을, **LAP**는 [34](Seznec, EVES)를 가져다 쓴다 (§5.1, §5.3).

### 2.9 Section map과 인용 가능한 문장

**Section map**

| § | 주제 |
|---|---|
| Abstract | 100+ 워크로드 규모의 data-dependent branch characterization 최초 제시; 3D-branch 정의; 12.7% misprediction 감소 / 3.1% IPC / 5.7KB |
| §1 Introduction | OOO 확장과 misprediction 비용; conditional branch가 misprediction의 92%; Fig. 1의 OOO 파이프라인과 이벤트 위치; Fig. 2의 3D-branch 정의 사례 (a)(b) 비-3D, (c)(d) 3D; 52%; timeliness 반영 시 기회 7%로 축소; 세 최적화(feeder load 실행 가속, MDP 기반 store 값 공급, load address 예측 + prefetch); 세 가지 기여; 1.5x/2x 코어 3.7%/4.3%; CBP-unlimited 9.2%/1.3% |
| §2 Background | 최신 예측기와 기존 data-dependent branch 기법 개관 |
| §2.1 | 64KB TAGE-SC-L Branch Predictor — bimodal→PPM→TAGE 계보, CBP-4/CBP-5 우승, SC/loop predictor; 8KB 대신 64KB를 baseline으로 고른 이유 |
| §2.2 | Existing Techniques to Handle Data Dependent Branches — EXACT(>10KB, proxy prior instance, TAGE 동급 대비 우위 없음), SLB(ISA 확장, hint/compare instruction, ~8KB) |
| §3 | Characterizing Direct Data Dependent (3D) Branches — 정의, "feeder load", gobmk Example 1/2 |
| §3.1 | Mispredictions from 3D-branches — Fig. 3(a) dependence chain별 분류, 3D가 52% |
| §3.2 | Data Availability from the Feeder Loads — Fig. 3(b) feeder load 상태(87.3% front-end, 0.5% ROB, 3.5% executed, 7.2% retired ⇒ 10.7%), +1.5% ⇒ 12.2%, 전체 기회 6.7%; Fig. 3(c) slack 원인 |
| §3.3 | Dependence Chains of 3D-branches — Fig. 4(a) 연산 개수(대부분 cmp/test 하나), Fig. 4(b) 연산 종류 |
| §3.4 | Burstiness of 3D-branches — Fig. 4(c) N-cycle 창 내 재발 확률 |
| §4 | 3D-Branch Overrider — predictor 강화가 아닌 override 기법, front-end 배치 |
| §4.1 | 3D-Branch Tracker Table (BT) — 128 entries/8-way/age·usefulness replacement; BPU 단계 override; LT index로 인덱싱되는 direct-mapped 3D Alloc Re-steer Table을 통한 allocation queue override; chain 튜플 표현; register move 제거 최적화 |
| §4.2 | ARF Extensions to Support 3D-branch Detection — 3D compliant 비트, load PC·연산 전파(Fig. 6), reset 조건, x64 flag register 추적, EXACT ID generation 차용 |
| §4.3 | Feeder Load Tracker and the Load Tracker — CAM 대신 FLT 선택, FLT entry당 BT entry 2개, 128-entry set-associative LT, LT→FLT→BT 인덱싱 |
| §4.4 | Branch Computation Unit — 16-entry CB, in-order 1 op/cycle, Fig. 5(b) datapath, full 시 drop |
| §4.5 | Storage Overhead — 구조별 내역, 총 3.2 KB |
| §5 | Optimizing the 3D-Branch Overrider — coverage/timeliness 확장 3종 |
| §5.1 | Memory Dependence Prediction — store PC–load PC pairing 근거(160 값/2800 주소 vs 380 값/260 주소), confidence 보수적 하향 |
| §5.2 | Accelerating the Feeder Load in the Pipeline — Load Prioritization과 LBT |
| §5.3 | Load Address Predictor — EVES를 LAP로 변형, Prefetch Load Tracker, confidence 기반 필터링 |
| §5.4 | Storage overhead — MDP 2 KB, Load Prioritization 1 KB, LAP 2.46 KB |
| §6 | Workloads and Evaluation Setup — Table 1(104 워크로드, 7 범주), Table 2(Icelake-like 파라미터), MPKI reduction 정의 |
| §7 | Results — 개요 |
| §7.1 | Performance Benefits — Fig. 7(a)/(b) 범주별 MPKI/IPC, Fig. 8(a) re-steer 위치 분포, Fig. 8(b) LAP 정확도/트래픽, Fig. 8(c) S-curve, LAP-without-re-steer 대조 실험 |
| §7.2 | Results Summary — Table 3 |
| §7.3 | Sensitivity Studies — Fig. 9 |
| §7.4 | Impact in Future Cores — Fig. 10, 1.5x/2x Icelake |
| §7.5 | Impact with Very Large Baseline Branch Predictor — CBP-5 unlimited baseline |
| §8 | Related Work — 값 상관 예측기, EXACT, SLB, CFD, dependence tracking, Branch Outcome Anticipation, backslice 우선 issue |
| §9 | Conclusion — 100+ 워크로드에서 12.7% / 3.1%; 미래 OOO 코어에서 영향 증가 |

**인용 가능한 문장 (verbatim)**

1. "We find that branches which have only one load instruction feeding them and only simple operations to compute the branch direction from the load value are responsible for a significant fraction of the overall mispredictions. We call such branches Direct Data Dependent (3D) branches." (Abstract)
2. "As newer branch predictor designs like Perceptron and TAGE integrated with statistical corrector emerge [24, 33], mispredictions from data-dependent branches remain the single most important category of mispredictions to tackle." (§1)
3. "Taking timely load value availability into consideration, our overall opportunity shrinks to 7% on average across the 100+ workloads." (§1)
4. "Referring back to Figure 3(a), where we saw that 3D-branch mispredictions constitute 52% of the total mispredictions, our overall opportunity becomes 6.7% of the total mispredictions." (§3.2)
5. "It is to be noted that the 3D-Branch Overrider is designed as an override mechanism to the branch predictor's prediction and is not an enhancement to the branch predictor itself." (§4)
6. "This compact representation is possible because we target 3D-branches which, as discussed earlier, are dependent on only one load and cannot have multiple register operations in their dependence chain. In this manner, the data-flow is implicit in the order of the operations." (§4.1)
7. "Using LT index ensures that correct overriding prediction is used for the 3D-branch." (§4.1)
8. "Once a load with LT index completes execution, we use FLT index stored in the LT to identify the BT entries of interest." (§4.3)
9. "While this simple design increases the latency to compute a branch direction, we find in practice that most branches only require a simple compare/test and hence there is marginal benefit from a more aggressive BCU design." (§4.4)
10. "Overall, 3D-Branch Overrider requires 3.2 KB." (§4.5)
11. "While 3D-Branch Overrider is not a predictor but an accurate branch overriding technique, MPKI reduction here refers to the number of expensive branch-misprediction related pipeline flushes triggered from the execution stage that are saved. These are replaced instead by low-latency front-end re-steer events thus benefiting both performance and efficiency." (§6)
12. "This scheme shows 0.93% IPC gain relative to the baseline which is even worse than 3D-Branch Overrider, and it is due to increased contention for L1 cache bandwidth. This shows that LAP based re-steers are key to getting higher performance." (§7.1)
13. "After careful analysis, we realized that almost all the gains are purely from accelerating the feeder load backslice but only a small portion translated into an override from the 3D-Branch Overrider. While this is an interesting observation, we do not claim novelty here." (§7.1)
14. "We conclude that even in the presence of an unreasonably large branch predictor, that reduces the mispredictions by 40% compared to 64KB CBP-5 winner predictor, data-dependent branch mispredictions cannot be effectively mitigated without unique and targeted solutions such as 3D-Branch Overrider." (§7.5)
15. "Since the producer stores happen much earlier than loads, the storage overhead to track these values is higher as compared to using the load to read the value in timely manner." (§8)

---

## 3. Branch Runahead — "Branch Runahead: An Alternative to Branch Prediction for Impossible to Predict Branches" (MICRO 2021)

> Stephen Pruett, Yale N. Patt · University of Texas at Austin (HPS Research Group)
> MICRO '21, 54th Annual IEEE/ACM International Symposium on Microarchitecture, 2021년 10월 18–22일, Virtual Event, Greece · ACM, pp. 804–815 (12 pages), DOI 10.1145/3466752.3480053
> 원문: [PDF](</home/lee/scarab/reference/[2021, MICRO] Branch Runahead_Pruett,Y.Patt.pdf>)

**Problem.** Branch misprediction은 여전히 성능의 핵심 제약이고, 남아 있는 misprediction은 점점 data-dependent branch에 몰리고 있다. 이런 branch는 "outcome is not correlated to any previous outcomes in the branch history. Rather, the outcome is based on a value recently loaded from memory"(§1)이므로 history-based predictor로는 원리적으로 잡히지 않는다. 실제로 벤치마크당 가장 예측하기 어려운 32개 branch에 대해 64KB TAGE-SC-L에서 unlimited storage MTAGE-SC로 옮겨가도 misprediction rate는 11%에서 9%로만 내려가며, 이는 "an improvement of only 18%"에 불과하다(§1, Fig. 1). 대안으로 제시된 pre-computation은 지금까지 두 갈래로 갈라져 있었다 — compiler/helper-thread 계열은 프로그램의 대부분을 재실행해야 하고 별도 core나 SMT context, 즉 fetch/decode/rename/ROB/LSQ 같은 전체 OoO 하드웨어를 요구하며, profiling data의 대표성에 크게 의존하고 ISA 변경까지 필요하다(§2.1, §2.3). 반대로 light-weight runtime 계열은 dependence chain에 control flow가 없어 continuous 실행이 불가능하고, 그래서 main thread에서 빠르게 이탈해 timeliness를 잃는다(§1, §2.2). 이 논문의 목표는 ISA·compiler 수정 없이 runtime에 하드웨어만으로 light-weight dependence chain을 추출하고 그것을 **연속적으로** 실행하는 메커니즘이다.

### 3.1 Motivation과 characterization

| 관측 | 수치 | 출처 |
|---|---|---|
| 벤치마크당 가장 예측하기 어려운 32개 branch의 misprediction rate | 64KB TAGE-SC-L(CBP-2016 limited storage 우승) 11% → MTAGE-SC(CBP-2016 unlimited storage 우승) 9%, "an improvement of only 18%" | §1, Fig. 1 |
| 같은 branch들을 dependence chain으로 pre-compute했을 때 | 5%로 하락 = TAGE-SC-L 대비 55%, MTAGE-SC 대비 44% misprediction 감소 | §1, Fig. 1 (§7에서 재진술) |
| dependence chain 평균 길이 | 8 micro-op 미만 | §1, Fig. 2 |
| dependence chain의 구조적 상한 | "All dependence chains have fewer than 16 micro-operations, do not contain expensive operations such as integer divide or floating point operations, and do not contain any control flow instructions." | §1 |
| Branch Runahead를 켰을 때 늘어나는 micro-op | 평균 34.3% 증가 (SlipStream은 retired instruction의 15%만 제거 → 85%가 overhead로 남음) | §2.2, Fig. 3 |
| affector 또는 guard branch의 영향을 받는 dependence chain 비율 (SPEC 2017) | Fig. 5. 본문은 "affector and guard branches impact such a large fraction of dependence chains"라고만 쓰고 평균 수치를 인쇄하지 않음 — 대부분 벤치마크의 막대가 100% 근처 | §3, Fig. 5 |
| 오예측 후 wrong path 길이 (merge point가 wrong path에 이미 존재한다는 근거) | "We measured an average of 100 wrong path uops fetched on workloads in SPEC CPU2006" | §4.4 footnote 12 |
| 새 merge point prediction의 정확도 | 92% (선행 연구 [29] 기준 78%) | §1 contributions, §4.4 |
| DCE가 만든 prediction 중 제때 생성되는 비율 | 약 40%; core가 실제로 사용한 prediction은 거의 전부 correct | §5.2, Fig. 12 |

Fig. 3의 세로축은 "Relative Micro-Ops Issued (%)"로, Micro-Ops Issued와 Load Micro-Ops Issued 두 계열을 보여주고 한 벤치마크는 142%까지 튄다. SlipStream 수치는 [35]에서 보고된 값이며 "used a slightly different set of benchmarks"라는 단서가 footnote 2에 달려 있다.

### 3.2 Key insights

- **branch의 backward dataflow slice는 하드웨어로 추출·가속할 만큼 짧고 단순하다.** 모든 chain이 16 uop 미만, 평균 8 uop 미만이고 divide/FP도 control flow도 없다(§1, Fig. 2). Chain에 control flow가 없다는 사실은 chain 자체에 branch prediction이 필요 없다는 뜻이고, 그래서 dependence chain cache에 이미 decode된 micro-op 시퀀스로 저장할 수 있다(§2.3).
- **chain은 trigger instruction 없이 연속 실행될 수 있다.** 완료된 chain이 만들어낸 `<branch address, outcome>`이 그대로 다음 chain을 initiate하는 tag가 되므로, chain은 "as if they were in a loop"로 돌아간다(§2.2, §4.1). DP-SSMT처럼 인스턴스마다 trigger instruction을 요구하지 않는 것이 더 멀리 run ahead할 수 있는 이유다(§2.2).
- **chain의 경계를 올바로 잡으려면 affector와 guard branch를 알아야 한다.** Guard branch는 다른 branch의 실행 여부를 통제하고, affector branch는 다른 branch의 source data에 영향을 준다(§3). Chain extraction은 affector/guard branch에서 종료되고 chain은 그 branch의 PC와 outcome으로 tag되므로, chain은 자신이 가정한 control/data context가 성립할 때만 실행된다(§3, §4.3, §4.4).
- **merge point는 code layout 가정 없이 misprediction 자체를 이용해 동적으로 찾을 수 있다.** 현대 프로세서는 misprediction을 감지하기 전에 수백 개의 wrong path instruction을 fetch하므로 merge point는 이미 wrong path에 들어와 있을 가능성이 높다. 따라서 "The first instruction that appears both in the wrong path and in the correct path is the predicted merge point"(§4.4).
- **chain 내부 통신이 지배적이므로 rename을 둘로 쪼갤 수 있다.** Chain extraction 시점의 one-time local rename과 initiation 시점의 dynamic global rename으로 분리하면, single-ported local register file bank와 작은 local reservation station으로 충분해져 core 구조 대비 cost-per-entry가 내려간다(§2.3, §4.2).
- **timeliness가 병목이고, 그 열쇠는 chain initiation 방식이다.** 논문의 문장은 "Chain Initiation is the most important factor towards improving timeliness"이며, initiation의 aggressiveness가 chain level parallelism에 영향을 주고 그것이 다시 timeliness에 영향을 준다는 인과 순서로 서술된다(§4.1, §5.2). 그래서 predictor의 정확도 자체는 중요하지 않다 — "the prediction is only used to increase chain level parallelism; thus, any level of accuracy will likely improve the timeliness of branch outcomes"(§4.1), 그 결과 per-branch 3-bit counter로 충분하다.
- **misprediction이 DCE와 core 사이의 자연스러운 동기화 지점이다.** "Branch mispredictions present a convenient time to perform this synchronization, as the core backend and frontend are synchronized"(§4.1) — 그 순간에만 core physical register file에서 live-in을 일관되게 복사할 수 있다.
- **move uop과 store–load pair를 모두 제거하면 chain에 store가 남지 않는다.** Store–load pair는 논리적으로 move와 같으므로 move-elimination 대상이며, 이 최적화가 "guarantees that dependence chains will not contain any store instructions"를 보장한다(§4.3). 덕분에 D-Cache를 공유해도 main thread 데이터를 오염시킬 수 없다(§4.2).

**§3의 worked example (Fig. 4a–d, leela).** tag semantics는 이 예제에서 구체화된다. branch A는 GO board의 임의 위치를 load해 그 값이 EMPTY인지 비교하는 data-dependent branch이고, branch B는 A가 not-taken일 때만 실행되므로 **A가 B를 guard한다**. 따라서 B의 chain extraction은 branch A에 도달하는 순간 종료되고, B의 chain은 `<A, NT>`로 tag된다 (Fig. 4d) — 즉 "branch A가 not-taken으로 예측/확정되었을 때 이 chain을 initiate하라"는 뜻이다. 반면 branch A 자신의 chain은 `<A, *>`로 tag되는데, `*`는 wildcard로 **branch A의 outcome이 무엇이든 이 tag에 match**해 chain을 initiate한다는 의미다 (Fig. 4c).

### 3.3 Hardware structures

Fig. 6는 기존 OoO pipeline(Fetch | Decode | Rename | Reservation Stations | Physical Register Read | Execute | Retire) 위에 추가되는 블록을 보여준다: Fetch 위에 **Prediction Queues**, Reservation Stations~Execute 구간 위에 **Chain Live-in Register Access**와 **Dependence Chain Engine**, Retire 위에 **Affector/Guard Detection**과 **Chain Extraction**. Fig. 7은 DCE 내부 블록도다.

| 구조 | 목적 | 구성(entry 수 / associativity / indexing / 파이프라인 위치) | per-entry 필드와 비트폭 |
|---|---|---|---|
| **Dependence Chain Cache** | 추출·local rename까지 끝난 chain을 initiate 대기 상태로 보관 | 최대 32개 dependence chain, LRU replacement(§4.2). Table 2: 32-entry (2KB) — Core-Only/Mini, 1024-entry — Big. DCE 내부, reservation station에 chain을 공급(Fig. 7). core misprediction 시 또는 완료된 chain이 만든 outcome으로 `<branch address, outcome>` tag 인덱싱(§4.1) | Tag = `<branch PC, branch outcome>`, outcome은 wildcard `*` 가능(§3). 본문은 micro-op 시퀀스로 저장되어 decode 불필요(§2.3). Table 2: "1 uop per entry, 4B per uop" |
| **DCE Physical Registers (local register files)** | 각 dynamic chain instance의 chain-내부 값과 inter-chain live-in 보관 | physical register file을 여러 개의 local register file로 분할하고 각각을 독립적인 **single-ported bank**로 취급(§4.2, Fig. 8). Table 2: 0 (0KB) — Core-Only(core의 PRF 공유), 64× 8-entry (4KB) — Mini, 1024× 8-entry — Big. global rename 시 dynamic chain instance마다 하나 할당 | bank별 ready bit; live-in 값은 core physical register file 또는 **producer chain의 local register file**에서 복사(§4.2, footnote 5) |
| **DCE Reservation Stations (local reservation stations)** | chain instance의 uop을 담고 source가 준비되면 ALU로 schedule | 여러 개의 local reservation station으로 분할, §4.2 본문은 각각 "a capacity of 16 uops (1 chain)"이라고 명시. Table 2의 RSV 행은 0 (0KB) — Core-Only, 64× 32-entry (4KB) — Mini, 1024× 32-entry — Big으로 인쇄되어 있어 본문의 16-uop 용량과 표의 32-entry가 문자 그대로는 일치하지 않는다(둘 다 그대로 옮김) | uop entry + source ready 상태. 결과는 physical register file과 reservation station 양쪽으로 broadcast되고, 모든 source가 ready면 schedule. **out-of-order** 스케줄링 — in-order도 실험했으나 "not able to expose enough Memory Level Parallelism (MLP)"(§4.2) |
| **Instruction Window (PRF + RSV)** | 동시에 실행 중인 dynamic chain instance 수 = chain level parallelism 결정 | local register file 수와 local reservation station 수의 곱으로 정의되는 개념적 구조. 전체를 core와 공유하면 비용은 최소, chain level parallelism도 최소(= Core-Only)(§4.2) | — |
| **DCE Functional Units (ALU0…ALU1) + D-Cache/D-TLB 공유** | chain의 산술/논리 uop과 load 실행 | ALU는 DCE 내부(Fig. 7); Core-Only는 core의 functional unit 공유. D-Cache/D-TLB는 core와 공유하되 **main thread가 port 우선권**을 갖고 DCE는 여유가 있을 때만 사용(§4.2). MSHR: 48-entry(Core-Only, Mini) / 64-entry(Big) | 지원 uop(Table 2) — Integer: add/multiply/subtract/mov/load; Logical: and/or/xor/not/shift/sign-extend. divide·FP·control flow·store 없음 |
| **Prediction Queues** | DCE가 만든 outcome을 core fetch stream과 program order로 동기화하고, DCE가 앞서거나 뒤처질 수 있는 거리를 제한 | DCE 내부 **16개 per-branch prediction queue**(§4.2). Table 2: 16× 256-entry (4KB) — Core-Only/Mini, 1024-entry — Big. core Fetch 단계에서 읽히며 출력은 64KB TAGE-SC-L 출력과 MUX(Fig. 6, Fig. 7) | slot별 prediction 값; "consumed but not yet filled" 상태; queue당 3개 포인터 — DCE push / core fetch / core retire; queue당 **2-bit throttle counter**; 각 branch에서 checkpoint된 core fetch pointer |
| **Hard Branch Table (HBT)** | hard-to-predict branch 식별 + 그 branch의 affector/guard와 그들의 bias 추적 | Table 2: 64-entry (1KB), 세 configuration 모두 동일. Retirement에 위치(Fig. 6 "Affector/Guard Detection", Fig. 9). conditional branch가 retire할 때 공간이 있으면 새 entry 할당(§4.3) | Fig. 9 기준 entry 필드: **BR**(branch PC), **Misp. Counter**(5-bit saturating), **AG**(affector/guard 표시), **AGC**(affector/guard changed), **AGL**(affector/guard list — footnote 8: "stored as a bit-vector, 1 bit per entry in the HBT"), **Bias Counter**(7-bit), **BD**(Biased Direction) |
| **Chain Extraction Buffer (CEB) + CEB store buffer** | backward dataflow walk를 수행할 retired uop window 제공 | 최근 retire된 **512개 micro-op**을 담는 circular buffer(§4.3). Table 2: 512-entry (2KB) — Core-Only/Mini, 2048-entry — Big. Retirement에 위치하며 cycle 단위로 스캔(Fig. 9) | uop별 PC, operation, destination/source physical register. 별도의 CEB store buffer가 store 주소를 보관해 load를 producing store와 매칭(§4.3) |
| **Live-in Table / Live-out Table** | chain별 architectural live-in/live-out을 기록해 producer–consumer 간 global register를 식별하고 core에서 live-in을 복사 | chain extraction 종료 시 기록되고 local rename과 core 동기화 때 참조(§4.3). Table 2에 크기 미보고 | live-in vector(LIV)의 architectural register 집합; live-out vector(Fig. 9 예시에서 P0, P2, P3, P7에 대응하는 architectural register). 이후 extraction의 search list 초기화에도 사용 |
| **Wrong Path Buffer (WPB)** | wrong path PC와 correct path PC의 교집합으로 merge point를 예측하고, affector 탐지용 both-path dest set 공급 | **128-entry, 4-way set associative** cache, retirement에 추가(§4.4, Table 1). flush 시 mispredicted branch 직후 instruction부터 forward ROB-walk로 batch 복사 | entry: wrong path PC(tag) + 그 지점까지의 architectural destination register **dest set** bit vector. memory write 주소는 **Bloom filter**로 추적. 별도로 correct-path dest set 하나를 누적하며, merge point에서 둘을 OR하면 **both-path dest set**. WPB 전체에 valid bit |
| **Predictive Initiation predictor** | 아직 방향이 확정되지 않은 non-wildcard chain을 미리 initiate | chain initiation 시점에 triggering branch의 outcome을 예측(§4.1) | targeted branch당 **3-bit counter** |
| **Poison state (Runahead Execution [25]에서 차용)** | affectee branch 식별 | merge point가 발견되면 both-path dest set으로 초기화, 이후 retire하는 correct path instruction을 따라 전파. merge-predicted branch의 두 번째 instance 또는 maximum merge point distance에서 종료(§4.4) | architectural register와 memory 주소(Bloom filter가 추적한 write 주소)의 poison bit. source의 poison이 destination으로 전파되고, poisoned register에 쓰면서 poison을 source하지 않으면 해제 |
| **Fetch-stage prediction MUX** | DCE 결과가 있으면 TAGE-SC-L 예측을 override | 64KB TAGE-SC-L은 baseline으로 그대로 유지. 이 MUX가 Branch Runahead에서 **유일하게 critical path에 있는 구성 요소**(§5.2) | prediction queue의 Hit 신호가 DCE outcome을 선택(Fig. 7) |

### 3.4 Operation — stage by stage

논문은 변경을 세 범주로 나눈다(§4): 1) **dependence chain extraction**(hard-to-predict branch와 그 chain의 uop을 식별해 chain cache에 저장), 2) **Dependence Chain Control**(chain을 core와 동기화하고 연속 실행시켜 TAGE-SC-L 대신 쓸 prediction을 생산), 3) **Dependence Chain Engine (DCE)**(chain을 core보다 효율적으로 실행하는 전용 유닛).

**(1) Hard-to-predict branch 탐지 — Retire.** conditional branch가 retire하면 공간이 있는 한 HBT entry가 할당된다. entry의 5-bit saturating misprediction counter는 mispredicted branch가 retire할 때마다 증가하고, saturate하면 그 branch는 hard-to-predict로 간주된다. counter는 1000개 retired branch마다 15씩 감소하며, counter가 0인 오래된 entry는 덮어쓸 수 있다(§4.3). footnote 7: 감소량과 counter 폭은 "총 misprediction rate의 1.5% 이상을 차지하는 branch를 1% false positive rate로 검출"하도록 binomial distribution으로 계산했다.

**(2) Affector/guard 탐지 — Flush + Retire.** flush가 발생하면 ROB에 wrong path instruction이 남아 있다. mispredicted branch 바로 다음 instruction부터 forward ROB-walk로 이들을 WPB에 복사한다(여러 cycle에 걸쳐 batch로; footnote 14는 복사 속도를 retire rate와 같다고 가정). ROB-walk는 보통 mispredicting branch의 두 번째 dynamic instance를 만나면 종료되고(루프 안이라는 뜻), 못 찾으면 ROB tail에 도달하거나 maximum merge point distance를 초과할 때 끝난다. 복사가 끝나면 WPB가 valid로 표시된다. footnote 13: ROB-walk latency는 문제되지 않는다고 본다 — flush 때 speculative register alias table을 복원하기 위해 이미 흔히 쓰이는 메커니즘이고 "Their latency is typically hidden by the front-end as it refills the pipeline."

recovery 후 retire하는 correct path instruction이 자신의 PC로 WPB를 인덱싱한다. hit하면 그것이 **predicted merge point**다. correct path에서 두 번째 branch instance가 retire하거나 maximum merge point distance를 초과할 때까지 merge point를 못 찾으면 프로세스는 종료되고 WPB는 invalidate된다.

- **Guard 탐지**: 정의상 branch는 자신과 merge point 사이에 관측되는 모든 branch를 guard한다(biased branch 제외). 따라서 merge point prediction 동안 wrong path나 correct path에서 관측된 branch는 merge-predicted branch에 의해 guard되는 것으로 표시된다. merge point를 못 찾으면 아무 branch도 표시하지 않는다.
- **Affector 탐지**: hit한 wrong-path dest set과 누적된 correct-path dest set의 logical OR = both-path dest set. 여기 표시된 register/memory 주소를 poison으로 초기화하고, merge point 이후 retire하는 correct path instruction을 따라 poison을 전파한다. poison을 source하는 모든 branch(merge-predicted branch 자신 포함, biased branch 제외)가 affectee이며 곧 merge-predicted branch가 그 branch의 affector다.

발견된 affector/guard는 HBT에 할당되고 AG 필드가 set된다. AG가 set된 entry는 스스로 hard-to-predict가 아니어도 표에 남고, **연관된 hard-to-predict branch가 제거될 때만 replace될 수 있다**(§4.3). 또한 hard-to-predict branch의 AGL bit-vector에 추가되며, 이전에 없던 branch라면 AGC가 set된다. 각 affector/guard의 7-bit bias counter는 retire한 방향이 BD와 일치하면 증가하고 주기적으로 9씩 감소한다(footnote 9: 1% false positive rate로 90% 이상 bias를 검출하도록 arithmetic model로 산정). biased로 판정되면 AGL에서 제거되고 필요하면 AGC가 set된다.

**(3) Chain extraction — Retire, off critical path.** hard-to-predict branch가 retire하면 시작한다(footnote 10: HBT에 있고 misprediction counter가 saturate했거나, **1% 확률로 무작위 선택**된 branch여야 한다). 알고리즘은 **Hashemi et al. [16]에서 차용**했으며(원 논문은 같은 walk로 load의 prefetch를 만들었다), 512-entry CEB 위에서 backward dataflow walk를 수행한다:

```
cycle 0 : 가장 최근 retire한 hard-to-predict branch(= 두 번째·더 젊은 dynamic instance)를 chain에 추가
          search list(LIV) := 그 branch의 live-in table entry
          search list += branch의 source register (condition code register)
cycle k : CEB를 스캔해 destination register가 search list와 일치하는 uop을 찾음
            1) 그 uop을 dependence chain에 추가
            2) 일치한 register를 search list에서 제거
            3) 그 uop의 source register를 search list에 추가
          load uop이면 그 주소를 CEB store buffer의 주소들과 비교하고,
          대응하는 store가 있으면 그 store도 chain에 추가
종료    : (a) 같은 branch의 두 번째 instance에 도달  또는  (b) affector/guard branch에 도달
          → 종료시킨 branch의 <PC, outcome>으로 chain을 tag하고 chain cache에 install
```

Fig. 9의 예시는 cycle 0에서 LIV={cc}로 출발해 cycle 1에서 CMP(→LIV={P0}), cycle 2에서 LD P0<=[P2](→LIV={P2}), cycle 3에서 MOV P2<=P7(→LIV={P7}), cycle N에서 최초 branch instance에 도달하며 LIV={P5,P3}로 끝나는 흐름을 보여준다. Latency는 `uops in CEB / retire width`로 모델링했고(footnote 11), "we experimented with much longer latency (1000s of cycles) and found no sensitivity" — extraction이 chain cache에 없는 chain을 만들어내는 일이 매우 드물기 때문이다(§4.3).

**(4) Local rename과 chain 최적화 — extraction 시점.** chain 내부에서만 쓰이는 local register는 physical register footprint를 최소화하도록 rename된다. Global register(chain 사이의 통신)는 이 chain의 live-in/live-out register를 각각 producer/consumer branch의 live-out/live-in register와 비교해 식별하고, **"Global registers are renamed in-order to guarantee the same name is used between different chain extractions"** — 즉 서로 다른 extraction으로 만들어진 producer chain과 consumer chain이 같은 이름을 쓰도록 보장하는 것이다(§4.3). (같은 chain의 서로 다른 dynamic instance 사이의 구분은 global rename 때 instance마다 새 local register file을 할당하는 것으로 처리된다 — §4.2, Fig. 8.) 이어서 모든 move uop이 move-eliminate되고, store–load pair도 논리적으로 move와 동등하므로 함께 제거되어 chain에 store가 남지 않는다.

**(5) Runahead 진입과 global rename — Misprediction.** chain은 live-in이 core와 동기화되기 전에는 initiate될 수 없다. core가 branch misprediction을 감지하면 그 branch의 address와 outcome이 tag가 되어 chain cache를 조회한다. hit하면 chain이 *initiated*된다 — uop이 reservation station에 기록되고, 새 local register file이 할당되며, live-in이 core physical register file에서 복사되고, 대응하는 prediction queue가 instruction fetch와 동기화된다(§4.1). Fig. 8의 예시:

```
core misprediction → tag <A,*> 발생, 동시에 DCE-core 동기화 트리거
  live-in을 core PRF에서 읽어 새 local register file(red)로 복사
  복사가 진행되는 동안 매칭 chain을 reservation station으로 issue하고
      새 local register file(blue) 할당; chain의 source register file := red
  branch가 issue되면 그 주소가 chain cache로 broadcast되어 매칭되는 wildcard tag를 initiate
      → 같은 chain의 또 다른 dynamic instance(orange) issue; source register file := blue
  red로의 live-in 복사가 끝나면 PRF의 ready bit가 set되고 reservation station에 통지
      → chain 실행 시작; uop 결과는 해당 local register file에 write back되고 RS에 통지
```

**(6) Continuous execution.** 완료된 chain의 outcome과 branch address가 다시 chain cache를 인덱싱해 다음 chain들을 initiate한다. 세 가지 방식이 평가된다(§4.1):

| Initiation 방식 | 언제 initiate하는가 | Chain level parallelism |
|---|---|---|
| **Non-speculative** | 선행 chain이 실행을 완전히 끝낸 뒤, 그 pre-computed outcome + branch address로 chain cache를 인덱싱해 매칭 chain 전부 initiate | 최소 — chain이 predecessor의 완료를 기다려야 함 |
| **Independent-early** | wildcard tag chain은 triggering branch가 reservation station으로 issue되는 시점에 initiate(방향이 무관하므로 predecessor 완료를 기다릴 이유가 없음). non-wildcard chain은 여전히 predecessor 완료를 기다림 | 증가 — wildcard chain들이 병렬 실행 가능 |
| **Predictive** | initiation 시점에 각 branch의 outcome을 3-bit counter로 예측해 non-wildcard chain도 조기 initiate. 예측이 틀리면 "the speculatively initiated chains are simply flushed and the correct chains are initiated in their place". wildcard chain은 Independent-early와 동일하게 처리 | 최대 — 예측이 맞으면 최대, 틀려도 앞의 두 모드보다 늦어지지는 않음 |

footnote 4는 이 세 방식의 상한을 명시한다: "initiation simply affects when the chain is added to the reservation stations. The individual uops within the dependence chain are still required to wait until their data dependencies have been satisfied before they are scheduled to the functional unit." 세 모드 모두 produced tag가 chain cache에 계속 hit하는 동안만 실행이 이어지고, tag가 miss하면 initiate할 chain이 없으므로 runahead mode를 빠져나온다.

**(7) Fetch에서의 사용.** chain이 initiate될 때 대응 prediction queue에 slot이 하나 할당된다(footnote 6: "Slots must be allocated at initiation to ensure they appear in the prediction queue in program order"). chain이 실행을 마치면 branch outcome을 그 slot에 push한다. branch가 fetch될 때 queue에 prediction이 있으면 TAGE-SC-L 대신 그것을 쓴다. 반대로 DCE가 아직 계산을 못 끝냈는데 core가 먼저 fetch하면 slot은 아직 채워지지 않은 채 **consumed**로 표시되고, 나중에 DCE가 계산을 끝내면 recovery에 대비해 그 slot을 채운다. queue 상태는 DCE push / core fetch / core retire 세 포인터로 유지된다.

**(8) Divergence 감지.** Branch Runahead는 dependence chain이 만든 모든 prediction을 감시하고, misprediction이 감지되면 core에서 live-in 값을 다시 복사해 chain을 재동기화한다(§4.1).

### 3.5 Correctness와 speculation 제어

Branch Runahead는 **prediction 메커니즘이지 execution 메커니즘이 아니다**. chain의 결과는 architectural state를 갱신하지 않으므로 자체 rollback이 필요 없고, 출력은 fetch에서 MUX를 통해 TAGE-SC-L 예측을 override할 뿐이라 틀린 경우는 core의 통상적인 branch recovery가 처리한다(§4.2, Fig. 7). "Dependence chains are not guaranteed to be correct, and occasionally diverge from the main thread"(§1)가 설계의 전제다.

- **Synchronization**: chain은 live-in이 core physical register file에서 복사되기 전에는 initiate될 수 없고, 그 시점으로 branch misprediction을 쓴다 — "the core backend and frontend are synchronized"이기 때문(§4.1).
- **Divergence 대응**: 모든 chain-produced prediction을 감시하다가 misprediction이 감지되면 live-in을 다시 복사해 재동기화한다. produced tag가 chain cache에서 miss하면 runahead mode 자체를 종료한다(§4.1). §3은 이를 "Dependence chains will be deactivated when a misprediction is detected"로 표현한다.
- **Prediction queue ordering과 recovery**: slot은 initiation 시점에 할당되어 program order를 보장하고(footnote 6), DCE보다 먼저 fetch된 slot은 consumed 표시 후 나중에 채워진다("in case there is a recovery"). branch recovery 시 core fetch pointer는 misprediction 이전 상태로 복원되어 "effectively reinserting previously consumed predictions into their original positions in the queue" — 각 branch에서 core fetch pointer를 checkpoint해 두는 방식이다(§4.2).
- **Throttling**: prediction queue마다 2-bit throttle counter가 있어, DCE가 맞고 TAGE가 틀리면 증가, DCE가 틀리고 TAGE가 맞으면 감소한다. counter가 negative면 DCE의 prediction은 무시된다(§4.2).
- **Memory ordering / memory safety**: chain extraction의 move elimination과 store–load pair elimination이 "guarantees that dependence chains will not contain any store instructions"를 보장하고, 따라서 "the main thread does not have to worry about data corruption by the dependence chains"(§4.2, §4.3). D-Cache/D-TLB는 공유하지만 main thread가 port 우선권을 갖고 DCE는 가용할 때만 사용한다. 다만 live-in을 producer chain의 single-ported local register file bank에서 읽는 경로가 있어 "creating the possibility of a bank conflict"(§4.2, footnote 5).
- **Initiation 단계의 speculation**: Predictive Initiation은 non-wildcard chain을 투기적으로 initiate하며, 틀리면 flush 후 올바른 chain을 대신 initiate한다. 최악의 경우에도 앞의 두 모드보다 늦게 initiate되지는 않으므로 correctness가 아니라 에너지만 잃는다(§4.1, §5.2).
- **명시된 가정과 그 실패 모드(§3)**: Branch Runahead는 (a) highly biased branch가 계속 biased일 것이라 가정하고 affector/guard여도 chain extraction 중 무시하며, (b) store–load pair 사이의 memory address aliasing이 지속될 것이라 가정한다. "These assumptions are, of course, not always true and can cause the dependence chains to diverge from the main thread." 논문 자신의 예시: Fig. 4a의 `for` 루프가 종료되면 branch A의 dependence chain은 더 이상 유효하지 않고 그 뒤의 prediction은 대부분 틀리게 된다.

### 3.6 Parameters와 storage budget

**Table 2 — Branch Runahead Configuration** (인쇄된 그대로)

| 항목 | Core-Only (9KB) | Mini (17KB) | Big (Unlimited) |
|---|---|---|---|
| uOps | Integer: add/multiply/subtract/mov/load. Logical: and/or/xor/not/shift/sign-extend. | ← 동일 | ← 동일 |
| Chain Cache | 32-entry (2KB) | 32-entry (2KB) | 1024-entry |
| (chain cache 공통) | 1 uop per entry, 4B per uop. | ← | ← |
| PRF | 0 (0KB) | 64× 8-entry (4KB) | 1024× 8-entry |
| RSV | 0 (0KB) | 64× 32-entry (4KB) | 1024× 32-entry |
| MSHRs | 48-entry | 48-entry | 64-entry |
| Prediction Queue | 16× 256-entry (4KB) | 16× 256-entry (4KB) | 1024-entry |
| HBT | 64-entry (1KB) | 64-entry (1KB) | 64-entry (1KB) |
| CEB | 512-entry (2KB) | 512-entry (2KB) | 2048-entry |

Core-Only는 physical register, reservation station, functional unit을 core와 공유하므로 PRF/RSV가 0KB다(§4.2, §5.1). 합계는 Core-Only = 2+4+1+2 = 9KB, Mini = 9+4+4 = 17KB로 표의 헤더와 일치한다.

**본문에만 있는 값들**

| 파라미터 | 값 | 출처 |
|---|---|---|
| dependence chain 길이 | 모두 16 uop 미만, 평균 8 uop 미만 | §1, Fig. 2 |
| local reservation station 용량 | "a capacity of 16 uops (1 chain)" | §4.2 |
| prediction queue 개수 | per-branch 16개 | §4.2 |
| prediction throttle counter | queue당 2-bit, negative면 DCE 무시 | §4.2 |
| Predictive Initiation predictor | per-branch 3-bit counter | §4.1 |
| HBT misprediction counter | 5-bit saturating, 1000 retired branch마다 15 감소 | §4.3, footnote 7 |
| HBT bias counter | 7-bit, 주기적으로 9 감소 | §4.3, footnote 9 |
| AGL 표현 | bit-vector, HBT entry당 1 bit | §4.3, footnote 8 |
| chain extraction 무작위 샘플링 | 1% 확률 | §4.3, footnote 10 |
| chain extraction latency 모델 | `uops in CEB / retire width`; 1000s of cycles까지 늘려도 민감도 없음 | §4.3, footnote 11 |
| WPB | 128-entry, 4-way set associative | §4.4, Table 1 |
| **maximum merge point distance** | **Table 1은 "256 uops", §4.4 본문은 "(100 uops in our experiments)" — 논문이 두 값을 모두 인쇄하며 서로 맞지 않는다. 양쪽 다 기록해 둔다.** | Table 1 / §4.4 |
| sweep이 시사하는 최적값 | window size 128-entry, chain cache 64-entry | §5.2, Fig. 13 |

**Table 1 — Baseline Configuration**: 4-Wide Issue, 256-Entry ROB, 92-Entry Reservation Station, 3.2 GHz, 64KB TAGE-SC-L [32], Scarab [2]로 모델링 / WPB 128-entry, 4-way, max merge point distance 256 uops / L1 32KB I-Cache + 32KB D-Cache, 64B line, 2 port, 3-cycle hit, 8-way, write-back / L2 2MB 12-way, 18-cycle, write-back / Memory Controller 64-Entry Memory Queue / Prefetcher Stream 64 streams, distance 16, prefetch into LLC / DRAM DDR4 8Gb x8 2400R, Ramulator [20].

**Storage / area 총량**
- 세 configuration: Core-Only 9KB, Mini 17KB, Big unlimited(Table 2). **내부 불일치**: §5.2는 80KB TAGE-SC-L과 비교하면서 Mini를 "(16KB)"로 적는 반면 Table 2 헤더는 "Mini (17KB)"다. 양쪽 다 그대로 옮긴다.
- sweep이 시사하는 최적값을 쓰면 "Big Branch Runahead could be implemented using 27KB of total storage"(§5.2).
- Area(§5.2, McPAT): DCE 엔진 **0.38 mm²**, 22nm baseline out-of-order core(16.96 mm²)의 약 **2.2%**. 내역 — dependence chain cache 0.09 mm², functional unit + reservation station + physical register 0.15 mm², chain extraction + HBT 0.14 mm². §1은 Core-Only 모델이면 **1.4%**라고 밝힌다. footnote 17: 참고로 McPAT이 추정한 64KB TAGE-SC-L은 0.73 mm²이며, 이는 McPAT이 TAGE-SC-L 내부의 interconnect·mux·adder를 충실히 모델링하지 않으므로 **하한**이다.

### 3.7 Evaluation setup과 results

**Setup (§5.1).** 시뮬레이터는 **Scarab [2]** — Intel/NSF FoMR initiative [1]로 Intel이 발주한 open source, execution-driven, cycle-accurate x86 시뮬레이터이며 front-end는 PIN [23] 기반이다. core microarchitecture 상세, cache hierarchy, **wrong-path execution**, 그리고 Ramulator [20]로 모델링한 non-uniform access latency DDR4 메모리 시스템을 충실히 모델링한다. Branch predictor는 CBP-2016 제출 configuration의 64KB TAGE-SC-L로, 논문은 이를 "the best known realistic branch predictor"라 부른다. Baseline은 Table 1, Branch Runahead 세 configuration은 Table 2(위 §3.6).

**Workloads.** SPEC CPU2017 Integer Speed, SPEC CPU2006 Integer [3], GAP Benchmark Suite [6]. 그중 **평균 MPKI가 2보다 큰** branch-misprediction-intensive 벤치마크를 선별한다. **SimPoints [27]** 방법론으로 벤치마크당 1~5개 대표 구간을 뽑아 각 구간을 **2억 instruction** 실행한 뒤 가중 평균한다. SPEC은 ref input set, GAP은 `-g 19 -n 300` 입력을 쓴다. ref input이 여러 개인 벤치마크는 각 input을 모두 실행하고 total dynamic instruction count로 가중해 하나의 값으로 합친다.

**Energy/Area.** McPAT [22]. DCE는 decode, register rename, floating point pipeline, prefetcher, 그리고 ROB처럼 precise state 유지를 위한 구조를 **제거한 stripped-down core**로 모델링했다(§5.1) — 위 area 수치의 전제다.

**Metrics.** 성능은 IPC, 예측 정확도는 branch MPKI. **MPKI Improvement = (Branch Runahead MPKI − TAGE-SC-L MPKI) / TAGE-SC-L MPKI**. Fig. 13의 parameter sweep은 시뮬레이션 개수 때문에 2억이 아니라 **1000만 instruction**만 실행했다(footnote 16).

**Headline results (모두 64KB TAGE-SC-L baseline 대비)**

| 결과 | Core-Only (9KB) | Mini (17KB) | Big (Unlimited) | 출처 |
|---|---|---|---|---|
| MPKI 감소 | **37.5%** | **43.6%** | **47.5%** | §5.2, Fig. 10 |
| IPC 향상 | **8.2%** | **13.7%** | **16.9%** | §5.2, Fig. 10 |

Abstract와 §1이 내세우는 headline pair는 "a reduction in branch MPKI of 47.5% and an average improvement in IPC of 16.9%" — 즉 Big configuration 값이다. 논문의 해석은 "The trade-off comes down to cost vs chain level parallelism": Big은 physical register와 reservation station entry를 가장 많이 줘 chain level parallelism을 최대화하고, Core-Only는 그 둘을 최소화한다(§5.2).

**나머지 결과**

- **Iso-storage 비교(핵심 논거)**: Mini와 대략 같은 storage overhead를 갖는 **80KB TAGE-SC-L**은 64KB baseline 대비 MPKI를 **0.8%**, IPC를 **0.3%**만 개선한다. 반면 Mini Branch Runahead는 **자신이 target하는 branch의 misprediction rate를 평균 55%** 개선하고, 같은 branch들에 대해 80KB TAGE-SC-L은 무시할 만한 효과만 낸다. 논문은 이를 history-based predictor가 이 부류의 data-dependent branch를 원리적으로 예측할 수 없다는 주장의 근거로 든다(§5.2, Fig. 10).
- **Limits of Branch Runahead**: Big은 microarchitecture 파라미터를 "far beyond their reasonable limits"까지 키운 모델인데도 Mini 대비 MPKI를 **3.8%**만 개선하며(§5.2, Fig. 10), 이는 "Mini Branch Runahead is very close to its peak potential"이라는 뜻이다. sweep 논의에서는 같은 격차를 **3.89%**로 적는다(§5.2, Fig. 13) — 논문이 두 수치를 인쇄한다.
- **Limits of history-based predictors**: Big Branch Runahead는 unlimited storage MTAGE-SC [33]를 평균 MPKI improvement에서 **앞선다**. MTAGE-SC는 SPEC에서는 크게 개선하지만 data-dependent branch가 지배하는 **GAP workload에서 부진**하기 때문이다. MTAGE-SC + Big Branch Runahead를 결합하면 **모든 벤치마크에서** MPKI가 추가로 개선된다(§5.2, Fig. 11 top).
- **Chain initiation 비교**: "Chain Initiation is the most important factor towards improving timeliness." chain level parallelism을 최대화하는 **Predictive Initiation이 MPKI에 가장 큰 영향**을 준다. 다만 "it comes at the cost of flushing the DCE on a misprediction, which wastes energy"(§5.2, Fig. 11 bottom).
- **Timeliness breakdown(Fig. 12)**: DCE가 공급한 prediction을 inactive / late / throttled / incorrect / correct로 분해한다. *inactive*는 core가 prediction을 필요로 한 시점에 그 prediction을 만들 chain이 아직 activate되지 않은 경우로, branch가 fetch된 뒤 첫 동기화 misprediction이 오기 전에 주로 발생한다. *late*는 chain은 활성이지만 결과가 너무 늦게 나온 경우로, chain에 long latency operation이 너무 많을 때 주로 발생한다. *throttle*은 §4.2의 throttling에 걸린 경우. 그림이 보여주는 두 가지: 첫째 **core가 실제로 사용한 prediction은 거의 전부 correct**, 둘째 **약 40%의 prediction이 제때 생성**된다.
- **Sweeps(Fig. 13)**: chain cache size, prediction queue entries, CEB entries, window size, HBT entries, max chain length를 각각 Big 값까지 개별적으로 sweep한다. Big이 Mini 대비 얻는 3.89%는 "primarily due to the increased window size and chain cache size"이고, 최적값은 window size 128-entry, chain cache 64-entry로 시사된다 — 이 값을 쓰면 Big을 27KB로 구현할 수 있다.
- **Energy(Fig. 14)**: Branch Runahead는 **평균적으로 에너지를 감소**시키며 "primarily due to faster run times"다. 다만 두 방향으로 에너지를 늘린다 — 첫째 새 구조가 만드는 static/dynamic power, 둘째 실행 instruction 수와 memory access 수의 증가(Fig. 3이 그 증가분을 보여준다). 본문에 평균 감소율의 수치는 인쇄되어 있지 않다.
- **Area**: §3.6 참조 — DCE 0.38 mm² = baseline OoO core(16.96 mm², 22nm)의 2.2%, Core-Only는 1.4%(§1).
- **Clock frequency**: "Branch Runahead minimally affects clock frequency, as almost all units are off the critical-path and are not sensitive to latency." critical path 위에 있는 유일한 구성 요소는 TAGE-SC-L과 DCE prediction queue 중 하나를 고르는 **MUX**뿐이다. DCE는 여러 cycle에 걸쳐 off critical path로 chain을 실행하고 결과를 prediction queue에 넣으므로 processor throughput 영향은 최소다(§5.2).

### 3.8 저자가 밝힌 limitation과 self-positioning

**Limitations (저자 진술)**

1. **Timeliness가 가장 어려운 문제**: "Timeliness is the most difficult issue Branch Runahead faces, with late predictions making up the largest category outside of correct predictions." 제때 생성되는 prediction은 약 40%뿐이다. *inactive* 카테고리가 존재하는 이유도 구조적이다 — "Branch Runahead requires a mispredicted branch to synchronize, which unfortunately has the effect of activating chains late"(§5.2, Fig. 12). late는 chain이 long latency operation을 너무 많이 포함할 때 발생한다.
2. **Chain은 결국 divergence한다**: "Unfortunately, dependence chains will eventually diverge from the main thread"(§4.1). 그때마다 core에서 live-in을 다시 복사하는 재동기화가 필요하고, 잦은 동기화는 다시 run ahead 능력을 갉아먹는다(§3).
3. **두 가지 명시적 가정이 틀릴 수 있다**(§3): highly biased branch가 계속 biased일 것이라는 가정(그래서 affector/guard여도 무시한다)과 store–load pair의 memory address aliasing이 지속될 것이라는 가정. "These assumptions are, of course, not always true and can cause the dependence chains to diverge from the main thread." 논문 자신의 예: Fig. 4a의 `for` 루프가 끝나면 branch A의 chain은 무효가 된다.
4. **Predictive Initiation의 에너지 비용**: 가장 성능이 좋은 initiation 방식이지만 "it comes at the cost of flushing the DCE on a misprediction, which wastes energy"(§5.2).
5. **에너지 증가 요인**: 새 구조의 static/dynamic power와, 실행 instruction·memory access 증가. 순 에너지는 실행 시간 단축 덕에 감소하지만 두 증가분 자체는 인정한다(§5.2).
6. **Bank conflict 가능성**: local register file은 single-ported bank이고 live-in은 producer chain의 bank에서 읽으므로 "creating the possibility of a bank conflict"(§4.2).
7. **Big configuration은 현실적 한계를 넘어선 모델**이며 Mini 대비 MPKI를 3.8%만 벌어준다(§5.2).
8. **McPAT 추정의 한계**: TAGE-SC-L의 0.73 mm²는 하한이다 — McPAT이 interconnect·mux·adder를 충실히 모델링하지 않기 때문(footnote 17).
9. **방법론적 단서**: Fig. 13의 sweep은 시뮬레이션 수 때문에 2억이 아닌 1000만 instruction으로 돌렸다(footnote 16). SlipStream 비교 수치는 약간 다른 벤치마크 집합에서 보고된 값이다(footnote 2).

**Self-positioning (§2, §6, 그리고 §4의 attribution)**

- §2의 총괄 주장: "Branch Runahead is not the first to propose pre-computation as a substitute for branch prediction. In fact, many works have paved the way for Branch Runahead [8, 9, 21, 31, 34, 38, 39]. However, Branch Runahead is the first runtime only solution to execute light-weight dependence chains continuously. This allows Branch Runahead to execute further ahead with fewer hardware resources."
- **§2.1 Compiler-based**: Zilles et al. [38, 39]가 100% 정확한 helper thread는 연산량이 너무 많아 수지가 맞지 않음을 처음 관찰했고, 이후 연구 [8, 21, 31]는 profiling으로 helper thread를 더 공격적으로 줄여왔다. 그 결과 profiling data의 대표성에 크게 의존하게 되고, 비대표적 데이터는 부정확한 축약과 잦고 비싼 동기화를 낳는다. 또한 compiler 수준에서 dependence chain을 도입하려면 ISA 변경이 필요한데 칩 제조사가 꺼린다. "In contrast, Branch Runahead requires no modifications to the ISA or compiler to achieve its full potential."
- **§2.2 Runtime**: SlipStream과 변종 [35, 36]은 runtime-only지만 A-stream이 retired instruction의 평균 15%만 제거해 85%가 overhead로 남는다 — Branch Runahead의 34.3% 추가 micro-op과 대비된다(Fig. 3). DP-SSMT [9]는 runtime에 chain을 추출하지만 인스턴스마다 trigger instruction이 필요하고 "generates dependence chains that only work if control goes down a predefined path"인 반면, Branch Runahead의 chain은 루프처럼 연속 실행되고 affector/guard branch를 고려한다. Hashemi et al. [15, 16]의 prefetching용 dependence chain은 chain의 예측력을 보여주지만, Branch Runahead는 affector/guard branch와 잦은 동기화를 더해 정확도를 높인다. Carlson et al. [7]은 다른 방식으로 chain을 추출하고, Naithani et al. [26]은 별도 pipeline 대신 core가 idle한 cycle에 chain을 issue한다 — 두 연구 모두 올바른 chain을 만들기 위해 branch prediction에 의존하므로 "making the techniques less useful as a branch prediction alternative."
- **§2.3 Heavy-weight helper thread**: 별도 core [21, 36]는 단일 스레드 하드웨어 비용을 두 배로 만들고 core-to-core latency를 더하며, SMT context [8, 9, 31, 38]는 helper thread instruction 전부를 fetch/decode/rename하고 ROB·Load-Store Queue 등 여러 구조를 건드리게 한다. Branch Runahead는 대신 chain의 단순성을 보장하고, chain을 micro-op 시퀀스로 저장해 decode를 없애고, rename을 one-time local + dynamic global 두 단계로 쪼갠다.
- **차용 명시**: chain extraction 알고리즘은 **"adapted from Hashemi et al. [16]"** — 원 논문에서는 같은 walk로 load의 prefetch를 만들었다(§4.3). affector 탐지의 poison 알고리즘은 **"adapted from Runahead Execution [25]"**(§4.4). merge point prediction에 대해서는 "Prior work in merge point prediction makes assumptions about code layout [10, 11], which results in a lower accuracy and coverage [29]"라고 적고, 자신의 방법이 92% vs 78%로 더 정확하다고 주장한다(§1, §4.4).
- **§6 Related Work**: Gupta et al. [14]는 예측 가능한 주소의 load 하나를 포함하는 dependence chain을 target하는데, 이는 branch의 부분집합에만 유효하며 "Branch Runahead is a more general technique that is able to capture more benefit"(대신 그들의 접근은 affector/guard가 없고 chain scheduling이 단순해지지만, chain 실행에 필요한 하드웨어는 상당 부분 동일하다). Farooq et al. [12]의 Store-Load-Branch (SLB) predictor는 compiler로 store-load-branch chain을 식별하는 compiler-assisted 기법. Gao et al. [13]은 load의 memory address를 다가올 branch 결과와 correlate한다. EXACT [4]는 feeder load의 주소로 branch instance를 구분한다. Premillieu et al. [28]은 wrong path에서 계산된 branch 결과를 저장해 correct path에서 replay하지만 "limited to control-independent/data-independent branches that are executed in the shadow of a branch misprediction". Ayers et al. [5]는 메모리 접근 패턴을 분류하는 방법론이다. BranchNet [37]은 CNN을 offline training하지만 "requires correlation between branch outcomes and history, making the technique less effective for data-dependent branches".
- **§7 Conclusion의 자기 요약**: "Branch Runahead, however, uses the application's own code to pre-compute the result of these branches, leading to an accuracy improvement of 55% (TAGE-SC-L) and 44% (MTAGE-SC) for branches which Branch Runahead targets."

### 3.9 Section map과 인용 가능한 문장

| § | 주제 |
|---|---|
| §1 Introduction | data-dependent branch가 history-based predictor를 무력화(Fig. 1); pre-computation으로서의 dependence chain; 네 가지 개선 축 — Light-weight Dependence Chain / Continuous Execution / Timeliness / Dependence Chain Engine; chain 길이 데이터(Fig. 2); contribution 목록 |
| §2 Limitations of Prior Work | "first runtime only solution to execute light-weight dependence chains continuously"라는 프레이밍 |
| §2.1 | Compiler-based 기법의 한계 — helper thread 추출, profiling 의존, ISA 변경 |
| §2.2 | 기존 runtime 기법의 한계 — SlipStream(85% overhead vs 34.3%, Fig. 3), DP-SSMT의 trigger instruction, prefetching용 dependence chain |
| §2.3 | Heavy-weight helper thread의 한계 — 별도 core / SMT context 비용; chain을 uop으로 저장하고 rename을 2단계로 쪼개는 대안 |
| §3 Motivational Example | leela 코드 예제(Fig. 4a–d), branch A와 B, chain 추출과 tagging; affector와 guard branch(Fig. 5); biased branch와 memory address aliasing 가정; "Putting it all together" |
| §4 Branch Runahead | pipeline 변경 개요(Fig. 6) — chain extraction / Dependence Chain Control / Dependence Chain Engine |
| §4.1 Dependence Chain Control | runahead mode 진입과 misprediction 시 live-in 동기화; continuous execution; Non-speculative / Independent-early / Predictive initiation; chain divergence 감지 |
| §4.2 DCE Microarchitecture | Fig. 7, Fig. 8 — dependence chain cache; 2단계 rename과 instruction scheduling; local physical register file; local reservation station; instruction window; global rename 예제; memory access와 D-Cache 공유; prediction queue와 3개 포인터; recovery; prediction throttling |
| §4.3 Chain Extraction Hardware | Fig. 9 — Hard Branch Table; affector/guard 추적(AG/AGC/AGL/bias counter/BD); CEB backward dataflow walk; live-in/live-out table; local rename; chain 최적화(move elimination, store–load pair elimination) |
| §4.4 Detecting Affector and Guard Branches | merge point prediction; Wrong Path Buffer; dest set과 both-path dest set; guard branch 탐지; poison 알고리즘을 통한 affector branch 탐지 |
| §5 Results | — |
| §5.1 Evaluation Methodology | Scarab, Table 1 baseline, Table 2 configuration, 벤치마크와 SimPoints, McPAT 에너지/면적 모델링, metric 정의 |
| §5.2 Branch Runahead Results | Fig. 10–14 — Core-Only/Mini/Big의 MPKI·IPC; 80KB TAGE-SC-L 비교; Limits of Branch Runahead; Limits of History-based Predictors(MTAGE-SC); chain initiation 방식 비교; prediction timeliness; parameter sweep; energy; area; clock frequency 영향 |
| §6 Related Work | Gupta et al.; Farooq et al.(SLB); Gao et al.; EXACT; Premillieu et al.; Ayers et al.; BranchNet |
| §7 Conclusion | — |

**Verbatim 인용문**

1. "Data-dependent branches present serious challenges for history-based predictors as their outcome is not correlated to any previous outcomes in the branch history. Rather, the outcome is based on a value recently loaded from memory." (§1)
2. "On average, MTAGE-SC is only able to reduce misprediction from 11% (TAGE-SC-L) to 9%, an improvement of only 18%." (§1)
3. "All dependence chains have fewer than 16 micro-operations, do not contain expensive operations such as integer divide or floating point operations, and do not contain any control flow instructions." (§1)
4. "However, Branch Runahead is the first runtime only solution to execute light-weight dependence chains continuously. This allows Branch Runahead to execute further ahead with fewer hardware resources." (§2)
5. "Branch mispredictions present a convenient time to perform this synchronization, as the core backend and frontend are synchronized." (§4.1)
6. "It is important to note that the prediction is only used to increase chain level parallelism; thus, any level of accuracy will likely improve the timeliness of branch outcomes. We use a simple per-branch 3-bit counter as the prediction mechanism." (§4.1)
7. "Dependence chains do not contain any store instructions (see section 4.3), so the main thread does not have to worry about data corruption by the dependence chains." (§4.2)
8. "Global registers are renamed in-order to guarantee the same name is used between different chain extractions." (§4.3)
9. "The chain extraction algorithm is adapted from Hashemi et al. [16] where the authors use dependence chains to create prefetches for load instructions." (§4.3)
10. "The first instruction that appears both in the wrong path and in the correct path is the predicted merge point." (§4.4)
11. "The results show that Branch Runahead reduces MPKI by an average of 37.5%, 43.6%, and 47.5% and increases IPC by an average of 8.2%, 13.7%, and 16.9%, respectively." (§5.2)
12. "Chain Initiation is the most important factor towards improving timeliness." (§5.2)
13. "Timeliness is the most difficult issue Branch Runahead faces, with late predictions making up the largest category outside of correct predictions." (§5.2)
14. "Branch Runahead, however, uses the application's own code to pre-compute the result of these branches, leading to an accuracy improvement of 55% (TAGE-SC-L) and 44% (MTAGE-SC) for branches which Branch Runahead targets." (§7)

---

## 4. CRISP — "CRISP: Critical Slice Prefetching" (ASPLOS 2022)

> Heiner Litz (hlitz@ucsc.edu) · Grant Ayers (granta@google.com) · Parthasarathy Ranganathan (parthas@google.com) — University of California Santa Cruz / Google, USA. ASPLOS '22, February 28 – March 4, 2022, Lausanne, Switzerland. ACM ISBN 978-1-4503-9205-1/22/02, DOI 10.1145/3503222.3507745, pp. 300–313. Acknowledgements: "This work was supported by Google and the Intel Corporation."
> 원문: [PDF](</home/lee/scarab/reference/[2022, ASPLOS] CRISP; Critical Slice Prefetching.pdf>)

**Problem.** DRAM 접근 지연은 여전히 현대 마이크로프로세서의 가장 큰 성능 제약이고, cache hierarchy를 놓친 load는 데이터가 돌아올 때까지 ROB head에 머무르며 뒤따르는 모든 instruction의 retire를 막는다 (Abstract, §1, §2). 기존에 실제로 구현된 prefetcher들은 regular pattern은 처리하지만 "existing implemented designs fail to provide any performance benefits in the presence of irregular memory access patterns" (Abstract). irregular access를 예측할 수 있는 선행 제안 — runahead prefetcher, continuous runahead engine, helper thread — 은 prefetch 후보와 그 instruction slice를 학습·저장하는 오버헤드, redundant instruction 실행에 따른 power 오버헤드, runahead interval이 너무 짧을 때의 무효화, continuous runahead의 경우 추가 compute core 요구 같은 hardware complexity 때문에 "has proven untenable for implementation in real hardware" (Abstract, §1). latency-tolerating 쪽인 OOO 실행은 delinquent load 뒤에 독립적인 instruction이 충분하지 않으면 실패한다 (§1, §7, Figure 1). 게다가 instruction scheduling은 non-trivial pipeline에 대해 NP-Hard이고(논문 인쇄본은 "for all but non-trivial pipelines"로 적혀 있는데 문맥상 "all but trivial"의 오식이다) 최소 지연으로 수행되어야 하므로 hardware에 복잡한 heuristic을 넣는 것이 불가능하고, 그 결과 "most CPU schedulers utilize an oldest-instruction-first policy at best, ignoring criticality" (§1, §2). 마지막으로 criticality를 활용한 선행 hardware 연구(Long Term Parking, Delay-and-Bypass, Load Slice Core 등)는 in-order pipeline의 energy efficiency를 겨냥했고 OOO 대비 성능을 오히려 떨어뜨렸으며, 이들이 의존하는 IBDA 기반 hardware slice 추출은 register dependency만 관측하고(메모리를 통한 dependency는 못 봄), 작은 slice만 제한된 개수로 지원하며, 알고리즘이 hardware에 hard-code되어 application별로 기준을 바꿀 수 없다 (§2, §3.5, §7).

### 4.1 Motivation과 characterization

| 관측 | 수치 | 출처 |
|---|---|---|
| Pointer-chasing microbenchmark (linked list traversal + vector-scalar multiply 인터리브)의 UPC 거동 | OOO 프로세서는 최대 UPC 6으로 실행하다가 linked list의 다음 원소를 읽는 LLC miss를 만나면 이후 모든 instruction이 그 load에 의존하므로 stall. CRISP는 delinquent load와 그 load slice를 critical로 태깅해 이전 loop iteration의 vector multiplication보다 앞서 실행되도록 승격하고, 이 워크로드의 average UPC를 **over 30%** 개선 | §1/§2, Figure 1 |
| 같은 kernel의 실측 (Intel Xeon Gold 5117, GCC 9.3, VEC_SIZE 32) | baseline **IPC 1.89** → 다음 iteration의 pointer-chasing memory operation을 vector multiplication 앞으로 수동 이동(Figure 2 line 12의 `__builtin_prefetch` 삽입) 시 **IPC 2.71** | §3.1, Figure 2 |
| 선행 criticality scheduler의 성능 손실 | Long Term Parking과 Delay-and-Bypass는 "sacrifice 10% and 5% performance, respectively, over an OOO core by simplifying the scheduling of non-critical instructions to improve energy efficiency in the frontend by 67% and 47%, respectively" | §2 |
| Load slice 크기 | Figure 4가 SPEC/datacenter application별 average load slice size를 log 축으로 제시. 본문 주장은 "For complex applications, load slices can contain thousands of instructions exceeding the ROB and reservation station size of modern processors." 그림에서 1000 instruction을 넘는 것은 deepsjeng(~2,000), img_dnn(~1,300), moses(~3,500) 세 개이고 Average는 ~500이다 (log 축에서 읽은 근사값 — 논문은 수치 표를 인쇄하지 않는다; namd ~145, memcached ~440으로 1000에 못 미친다). 대비되는 hardware 기법의 한계는 "load slices of a limited size such as 32 instructions" | §3.5, Figure 4; §1 |
| Critical instruction의 절대 개수 | perlbench, gcc(sgcc), moses에서 CRISP가 **over 10,000 unique instructions**를 critical로 분류(단 Figure 11의 막대는 perlbench ~6,000, sgcc·moses ~7,500로 10,000 gridline에 닿지 않아 본문 수치와 그림이 정확히 일치하지는 않는다 — log 축 근사 판독). 이를 hardware로 완벽히 담으려면 IBDA류는 "100's of KB of meta-data storage"가 필요하지만, CRISP는 critical instruction당 1 byte prefix만 추가 | §5.6, Figure 11 |
| Hard-to-predict branch의 지배력 | misprediction rate > 15%인 branch를 포함하는 loop에서는 iteration 실행 시간이 branch outcome 해소 시간에 크게 좌우되어, 그 loop 안의 load slice를 우선화해도 자동으로 큰 이득이 나지 않음. 초기 실험에서 perfect branch predictor를 켜면 irregular load와 그 slice를 우선화하는 이득이 "significantly higher"였음 (특히 SPEC lbm) | §3.4, §5.3 |
| Criticality 비율의 sweet spot | "the prioritization of critical instructions performs best if the ratio of critical instructions among all instructions is **5%-40%**" — memory-bound application에서는 프로그램 instruction의 큰 비중이 최소 하나의 load slice에 속하고, 대부분이 critical로 표시되면 scheduler는 아무것도 우선화할 수 없게 됨 | §3.2 (p.302) |

### 4.2 Key insights

- **oldest-ready-first scheduler는 구조적으로 pointer chase의 latency를 겹칠 수 없다.** reservation station에 더 오래된 ready instruction이 있는 한 기존 OOO scheduler는 더 어린 load를 고르지 않으므로("Existing OOO schedulers generally do not pick younger loads for scheduling as long as older ready instructions are available in the reservation station", §1), window를 키우는 것이 아니라 scheduler에 명시적 criticality 신호를 주는 것이 해법이다. 논문은 이것이 compiler로도 쉽게 해결되지 않는다고 못박는다: "This issue also cannot be easily addressed with the compiler as it requires re-ordering of instructions across loop-iterations." (§1/§2)
- **criticality 판정에 필요한 정보는 hardware가 가질 수 없다.** load의 execution frequency, cache-level miss rate, 그 load에 대한 다른 instruction의 의존 관계 같은 정보는 "generally unavailable in hardware"이며, 그래서 선행 hardware 기법은 "treating all loads as critical" 같은 단순·부정확한 대안으로 후퇴했다 (§3.5, §7). CRISP는 이를 offline software 분석으로 옮기고 hardware에는 태그 하나만 전달한다 (§3.2, §4.1).
- **software slice 추출은 memory를 통한 dependency까지 따라갈 수 있다.** "Hardware methods that utilize IBDA, capture incomplete instruction slices as they can only observe dependencies through registers, but not memory which is crucial for x86 due to register spilling." 논문이 드는 예는 Figure 3의 line 31로, `%rax`의 값이 stack(`%rbp`)을 통해 전달된다 (§3.5).
- **slice 전체를 승격하면 안 된다.** load slice의 instruction이 reservation station 슬롯을 모두 채우면 "there exist no opportunities for the scheduler to prioritize critical over non-critical instructions." 그래서 CRISP는 slice를 DAG로 보고 각 leaf에서 root(delinquent load)까지의 aggregated path latency를 계산해 critical path 위의 instruction만 승격한다 (§3.5).
- **branch resolution latency가 iteration 시간을 결정하는 loop가 있다.** 잦은 branch misprediction은 decoupled frontend가 run ahead하여 reservation station에 충분한 instruction을 채우는 것을 막고, 그러면 scheduler가 criticality를 활용할 여지 자체가 사라진다 (§5.3). 그래서 branch slice도 우선화 대상이며, 논문의 주장은 "the benefit of combining load and branch slicing can be greater than their individual contributions" (§3.4) — 즉 조합이 각각의 단독 적용보다 낫다는 비교이지, 두 이득의 *합*을 초과한다는 주장은 아니다 (§5.3의 표현도 "the combined performance is both higher compared to only prioritizing branch- or load slices").
- **age-matrix RAND scheduler에 priority를 얹는 데는 게이트 한 단계면 된다.** 기존 BID(ready) vector 옆에 PRIO vector를 추가하고, 비싼 NOR reduction은 기존 설계와 병렬로 수행되므로 critical path는 "one additional logic level (AND to generate PRIO) and one multiplexer"만 늘어난다 (§4.2, §4.3).
- **static compiler hoisting으로는 부족하다.** LLVM `Hoist()`는 loop-invariant load를 preheader로 옮기지만, "Most performance-critical loads, however, are loop-dependent and as such, they cannot be reordered across basic blocks." 또 static hoisting은 load를 cold basic block에서 hot basic block으로 옮겨 성능을 떨어뜨릴 수 있어 LLVM은 recipient block이 cold일 때만 옮기는 PGO-based hoisting을 도입해야 했고, 이는 "the need for a more dynamic mechanism"을 보여준다. 더 일반적으로 VLIW식 static code scheduling은 dynamic scheduling보다 열등하다고 밝혀졌다 (§3.6).

### 4.3 Hardware structures

CRISP는 **전용 metadata table을 하나도 추가하지 않는다**. hardware 변경은 아래 네 가지가 전부다.

| 구조 | 목적 | 구성 (entry 수 / associativity / indexing / 파이프라인 위치) | per-entry 필드와 비트폭 |
|---|---|---|---|
| 새 x86 "critical" instruction prefix (ISA 확장) | software가 계산한 criticality 결정을 hardware metadata table 없이 전달 | 별도 hardware 구조 없음. post-link-time compilation이 FDO pass에서 binary에 삽입 (§3.3, §4.1, Figure 5 step 3). 우선화된 load slice/branch slice에 속한 모든 instruction 앞에 붙는다. §4.2는 criticality를 "either directly in their encodings or through a sideband method"로 표시할 수 있고 x86 prefix는 "without loss of generality" 채택한 하나의 구현임을 명시 | critical instruction당 **1 byte** (§4.2, §5.7). static code size 증가는 minimal, dynamic instruction footprint는 평균 5.2% 증가 (Figure 12) |
| Instruction decoder 확장 | prefix를 해석해 instruction을 critical로 태깅하고, 그 태그가 pipeline을 따라 scheduler까지 전달되게 함 | frontend decode 단계. 논문 표현: "we extend the instruction decoder to interpret the new latency-critical instruction prefix and tag all critical instructions as they progress through the CPU pipeline" (§4.2) | 논문 미보고 (태그 폭을 명시하지 않음) |
| IQ slot당 priority bit | 그 slot을 점유한 instruction이 우선화 대상인지 식별 | instruction queue / reservation station의 각 슬롯에 1 bit 추가. baseline은 Table 1의 96-entry unified RS이지만, §4.2는 "Our approach can be implemented in both unified reservation station (RS) architectures as well as in systems that leverage a separate scheduler for each functional unit"라고 명시 | **1 bit/slot** → n-entry IQ에서 공간 오버헤드 1/n. AND 게이트를 앞 pipeline stage로 옮기는 timing 완화 변형에서는 2/n (§4.3) |
| PRIO vector와 그 AND/NOR select logic (age-matrix RAND scheduler 내부) | ready이면서 prioritized인 instruction 중 가장 오래된 것을 선택 | 기존 BID(ready) vector 옆에 추가. baseline age matrix(AMD Bulldozer, IBM POWER8 채택)의 동작: N-entry IQ의 각 entry가 all-ones로 초기화된 N-bit age vector를 가지고, enqueue 시 자기 슬롯 비트를 자기 age vector에서 clear, 이후 들어오는 instruction이 선행 instruction들의 age vector에서 자기 비트를 clear. source operand가 준비되면 BID vector에서 자기 슬롯 비트를 set하고, 각 ready instruction이 자기 age mask와 BID를 bitwise AND — 가장 오래된 것만 결과가 all-zero이고 이를 n-bit NOR로 1비트 신호로 축약. Figure 6에서 CRISP 추가분은 파란색 | PRIO vector: n bit (IQ slot당 1). 각 instruction의 age mask(n bit)와 AND 후 n-bit NOR로 select 신호 생성 |
| Grant multiplexer | 가장 오래된 prioritized instruction을, 그런 것이 없으면 가장 오래된 instruction을 선택 | picker logic 출력단. 두 NOR reduction 결과를 select 입력으로 받음 | transmission gate로 구현하면 logic level을 추가하지 않음 (§4.3) |

Offline 쪽은 hardware가 아니라 software 도구다: DynamoRIO Memtrace instruction trace(대안으로 Intel PT; memory를 통한 dependency 관측에는 Kaby Lake의 PTWrite instruction이 필요, footnote 2) 위에서 backward walk를 수행하며 `frontier`라는 queue를 유지한다 (§3.3).

### 4.4 Operation — stage by stage

**Offline / profiling 경로 (Figure 5, §4.1).**

```
(1) Release binary (CRISP enabled)  ──▶ Data center runtime profiling
        · system-wide profiling: GWP / ODS, instruction tracing: AsmDB
        · PMU counters, PEBS, LBR, Intel PT / DynamoRIO Memtrace
        · 산출물 예: "Load PC / misses" 테이블 (0x4273→42, 0x1234→7 …)
(2) Offline "Dataflow & PMU analysis"   ← trace + unmodified binary
        · delinquent load 판정 → load slice 추출 → branch slice 추출
        · uncommon code path 필터링, 같은 delinquent load를 가리키는 slice 병합
        · slice를 DAG로 보고 critical path만 남김
(3) Criticality prefix injection (post-link-time compilation)
        · 살아남은 critical instruction마다 1-byte prefix를 prepend
   ──▶ Updated binary
```

- **(2)의 판정 기준.** 1차 정의(§3.2): "We define a load to be critical if its last level cache (LLC) miss rate is higher than a particular threshold, for instance, 20% (Section 5.5 explores this threshold), its memory address cannot be easily predicted by the hardware prefetcher (not a constant or stride), and if the number of independent instructions behind the load in the sequential instruction stream is small." — 즉 miss rate만이 아니라 *prefetcher가 예측 못 하는 irregular 주소*와 *load 뒤에 독립 instruction이 적은 상황*까지가 정의에 포함된다.
- **criticality를 결정하는 factor 목록 (§3.2):** ① 프로그램 내 다른 load 대비 그 load의 execution ratio, ② 그 load의 LLC miss rate, ③ 그 load가 유발하는 pipeline stall, ④ 프로그램의 baseline IPC와 instruction mix, ⑤ 그 load가 발생하는 시점의 프로그램 MLP, ⑥ "the time a load becomes ready to be scheduled, determined by its dependency on other high latency instructions" (다른 고지연 instruction에 대한 의존으로 결정되는 ready 시점).
- **관측 수단 (§3.2):** modern CPU는 위 지표를 직접 측정할 observability가 없으므로 대리 지표를 쓴다. IPC는 직접 측정, instruction mix는 VTune 또는 PMU counter, 특정 load의 상대 execution frequency는 PEBS 또는 PT, load의 AMAT는 PEBS로 cache miss를 재어 근사, load가 유발하는 pipeline stall과 MLP는 precise back-end stall과 load queue occupancy로 근사.
- **파생 heuristic (§3.2, p.303):** "we only flag loads as critical if they represent more than 5% of all executed loads of a program, if their LLC miss ratio is above 20%, and if the average MLP is below 5 for phases that include said load." 이 퍼센트들은 instruction mix(다른 instruction 대비 load 수)와 baseline IPC에 따라 선형으로 스케일된다.
- **반복 profiling pass (§3.2):** "By performing multiple profiling passes CRISP can empirically test different injection thresholds to determine the application-specific optimized ratio of critical instructions." — 즉 5%–40% 목표 비율에 맞추기 위한 application별 튜닝 루프가 흐름의 일부다.
- **Branch slice (§3.4):** misprediction rate > 15%인 hard-to-predict branch도 critical로 분류하고, branch outcome을 계산하는 데 필요한 instruction 집합을 branch slice로 정의해 함께 우선화한다.
- **Backward slice walk (§3.3):** trace를 순회하다 delinquent load를 만나면 그 load를 `frontier`에 넣고 source register를 분석한다. 현재 instruction을 frontier에서 빼고 ancestor를 넣기를 반복하되, 다음 네 경우에는 확장하지 않는다 — (1) ancestor가 이미 load slice에 포함됨, (2) source operand가 상수여서 ancestor가 없음, (3) ancestor가 system call return instruction, (4) trace의 끝에 도달. frontier가 비면 그 delinquent load에 대해 알고리즘이 종료한다.
- **Figure 3 walk 예 (§3.3):** PC `0x15e9` (line 30, `mov 0x8(%rax),%rax`)가 critical load로 표시된다. 역방향으로 line 25의 PC `0x15da`까지 따라가고, `0x15da`가 이전 loop 실행의 `0x15e1`에 의존하는데 `0x15e1`이 이미 load slice에 있으므로 recursive dependency로 종료한다. line 18의 PC `0x15cc`는 slice에 포함되지 않는다 — 그 instruction과 load slice 사이에는 forward dependency만 존재하며, 다음 loop iteration의 `0x15cc` instance가 `0x15e9`가 만든 값에 의존할 뿐이기 때문이다. 결과적으로 프로세서는 critical load slice의 실행을 line 2보다 앞으로 재배치해 line 30의 memory latency를 숨길 수 있다.
- **DAG 가지치기 (§3.5):** slice를 DAG로 보고 각 leaf에서 root(delinquent load)까지의 aggregated path latency를 계산한다. 대부분의 instruction에는 프로세서 구현 기준의 고정 latency를 부여하고(uops.info 등 published table), load에는 §3.2에서 구한 AMAT를 cycle 단위로 사용한다. 이 DAG는 instruction trace로부터는 계산 가능하지만 hardware에서는 일반적으로 얻을 수 없다.
- **분석 비용 (§4.1):** 분석 대상 trace는 100M instruction, 5GB (압축 1.6GB)이고, analysis + slice extraction은 "on the order of 100 seconds"이다.

**Runtime 경로 (§4.2, §3.6, Figure 6).**

```
Fetch/Decode : decoupled frontend(TAGE + FDIP)가 미래 loop iteration의
               instruction까지 IQ로 가져옴. decoder가 critical prefix를 해석해
               해당 instruction을 latency-critical로 태깅.
Enqueue      : RAND scheduler가 instruction을 IQ의 임의 슬롯에 삽입.
               age vector에서 자기 슬롯 비트 clear + 선행 entry들의 age vector 갱신.
               instruction의 critical 태그가 그 슬롯의 priority bit를 set.
Wakeup       : source operand가 준비되면 BID vector의 자기 슬롯 비트를 set.
               ready AND prioritized인 instruction은 PRIO vector의 비트도 set.
Select       : 각 instruction이 (age mask AND BID) → n-bit NOR  … oldest-ready
                             (age mask AND PRIO) → n-bit NOR  … oldest-ready-prioritized
Grant        : mux가 oldest prioritized를 고르고, 그런 instruction이 없으면
               oldest ready로 fallback. → delinquent load와 그 slice가
               앞선 loop iteration의 오래된 non-critical instruction보다 먼저 issue.
```

RAND scheduler를 고른 이유도 §4.2에 있다: RAND는 새로 fetch된 instruction을 IQ의 무작위 슬롯에 넣어 공간 효율이 좋고 전통적인 self-compacting queue-based scheduler(SHIFT)보다 회로 복잡도를 크게 줄인다. SHIFT는 fetch cycle 순으로 완벽히 정렬하는 장점이 있지만 compaction이 고클럭에서 감당 불가라 더 이상 쓰이지 않으며, RAND의 IPC는 age matrix로 fetch order를 관측함으로써 개선된다.

### 4.5 Correctness와 speculation 제어

CRISP 자체는 **비투기적**이다. redundant/extra instruction을 실행하지 않고, 정상 실행 경로 밖의 prefetch request를 만들지도 않는다 — 이미 fetch되어 아키텍처적으로 실행되어야 할 instruction들의 **선택 순서만** 바꾼다 (§1 contribution list: "CRISP avoids the execution of redundant instructions inherent to runahead prefetching"). 따라서 정확성은 기존 OOO speculation 하드웨어에 그대로 얹힌다. §3.6이 이를 명시한다: frontend가 branch prediction으로 미래 instruction을 공급하고, CRISP scheduler는 이전 loop iteration의 오래된 non-critical instruction을 건너뛰며 critical instruction을 먼저 실행시키는데, "These early-executed, critical instructions do not affect correctness if misspeculated, as they are squashed by the hardware after resolving the misprediction."

- **Memory ordering:** 변경 없음. §6.1은 memory consistency model에 따라 load queue / on-chip interconnect / DRAM scheduler에서의 재배치로 delinquent load latency를 더 줄일 수 있다고 *향후 방향*으로만 언급한다.
- **오예측 처리:** 기존 squash/recovery 하드웨어를 그대로 사용하며, CRISP 전용 validation이나 rollback 구조는 없다.
- **Over-tagging은 정확성이 아니라 성능 위험:** 대부분의 instruction이 critical로 표시되면 scheduler가 아무것도 우선화할 수 없다 (§3.2). 이 때문에 slice의 critical path만 승격한다 (§3.5).
- **Forward progress / fairness (§6.2):** 한 hyperthread의 instruction을 다른 hyperthread보다 우선화하는 것은, 프로그램이 자기 instruction을 전부 critical로 태깅하는 denial-of-service 공격을 가능하게 한다. 완화책으로 (a) 모든 SMT thread가 일정 시간당 일정 수의 instruction을 실행하도록 execution resource를 예약하는 방식, (b) ready critical instruction이 넘칠 때도 일부 non-critical instruction의 scheduling을 보장하는 정책, (c) thread 간 단순 round-robin arbitration을 제시한다.
- **Side channel (§6.2):** 한 hardware thread에서 모두 critical 또는 모두 non-critical인 application을 실행해 같은 SMT core의 다른 thread의 critical instruction 수를 유출하고, 특정 phase의 criticality 관측으로 상대 application을 식별하는 signature 추출이 가능할 수 있다. 논문은 이것이 PORTSMASH, TLBleed, CacheBleed, MemJam이 보인 SMT 데이터 유출보다 덜 심각하며, SMT 비활성화와 port-independent code 같은 기존 완화책이 그대로 적용된다고 주장한다.

### 4.6 Parameters와 storage budget

| 항목 | 논문이 고른 값 | 출처 |
|---|---|---|
| Critical load 1차 정의 | LLC miss rate > threshold (예: 20%) **AND** 주소가 hardware prefetcher로 쉽게 예측되지 않음(상수/stride 아님) **AND** 순차 instruction stream에서 그 load 뒤의 독립 instruction 수가 적음 | §3.2 (p.302) |
| 파생 heuristic | 프로그램 전체 실행 load의 5% 초과 + LLC miss ratio > 20% + 해당 load를 포함한 phase의 average MLP < 5. 퍼센트는 instruction mix와 baseline IPC에 따라 선형 스케일 | §3.2 (p.303) |
| Critical instruction 목표 비율 | 전체 instruction 대비 **5%–40%** | §3.2 (p.302) |
| Miss-contribution threshold T (sweep) | 5%, 1%, 0.2% — 전체 평균은 1%가 최적, moses는 2%가 최적 | §5.5, Figure 10 |
| Hard-to-predict branch 기준 | misprediction rate > 15% | §3.4 |
| Instruction prefix 크기 | critical instruction당 1 byte | §4.2, §5.7 |
| IQ priority 저장 | slot당 1 bit → 1/n 오버헤드; PRIO AND 게이트를 앞 stage로 옮기는 변형에서는 2/n | §4.3 |
| Scheduler critical path | +1 logic level (PRIO를 만드는 AND) + multiplexer 1개 (transmission gate로 구현 시 logic level 추가 없음). 평가에서는 baseline scheduler와 동일한 scheduling latency 가정 | §4.3 |
| Trace / 분석 비용 | 100M instruction, 5GB (1.6GB compressed), analysis+slice extraction ≈ 100초 | §4.1 |
| Baseline core (Table 1) | Intel Xeon Skylake, 20 cores/socket, all-core turbo 3.0 GHz, frontend/retirement 6-way, 4 ALU / 2 Load / 1 Store, TAGE branch predictor, 8K-entry BTB, 224-entry ROB, 96-entry unified reservation station, baseline scheduler = 6-oldest-ready-instructions-first, data prefetcher BOP + Stream, instruction prefetcher FDIP (128 FTQ entries), 64-entry load buffer, 128-entry store buffer | Table 1 |
| Baseline memory hierarchy (Table 1) | L1 I-cache 32 KiB 8-way (3 cycle), L1 D-cache 32 KiB 8-way (4 cycle), LLC shared 1 MiB/core 20-way (L3 36 cycle), DDR4-2400 1 channel | Table 1 |
| RS/ROB 민감도 구성 | 64RS/180ROB, 128RS/352ROB, 150RS/400ROB (RS와 ROB를 각각 50%/100% 증가; Sunny Cove가 Skylake 대비 RS를 96→128로 늘린 것이 동기) | §5.4, Figure 9 |
| IBDA 비교 구성 | 4-way set associative instruction slice table(IST) 1024 entries (load-slice architecture 제안값), 8K entry (8-way), 64K entry (16-way), infinite. 추가로 32-entry delinquent load table | §5.2 |

**Storage budget.** 전용 metadata table은 없다. on-chip 저장은 IQ slot당 priority bit 1개 — "To support CRISP, each slot in the IQ is extended with a single bit to identify its priority, resulting in a space overhead of 1/n" (§4.3), timing 완화 변형에서는 2/n. criticality metadata는 대신 instruction footprint 안에 critical instruction당 1 byte prefix로 저장된다: static code size 증가는 minimal, dynamic(실행 빈도 가중) instruction footprint는 평균 5.2% 증가, instruction cache MPKI의 worst-case 증가는 2.6% (§5.7, Figure 12). 대비 근거로 §5.6은 IBDA류가 perlbench/gcc/moses의 10,000개 초과 unique critical instruction을 완벽히 담으려면 "100's of KB of meta-data storage"가 필요하다고 말한다. 논문은 단일 합산 비트 총량은 보고하지 않는다.

### 4.7 Evaluation setup과 results

**Setup (§5.1).** Scarab cycle-accurate simulator로 평가하며, Scarab은 detailed decoupled frontend, (타입별) functional unit contention, branch prediction, BTB, RAS, multi-level cache hierarchy, 그리고 Ramulator 기반 detailed memory system을 모델링한다. 주요 파라미터는 Skylake-like Intel 프로세서를 본뜬 Table 1 (위 §4.6). CRISP 구현과 모든 baseline은 CRISP가 도입한 scheduler 변경을 제외하면 동일한 파라미터를 공유한다. 워크로드는 SPEC2017의 memory-intensive application, Xhpcg, 그리고 Tailbench의 datacenter application Moses / Memcached / Img-dnn이며, application당 200M representative instruction을 실행한다. 모든 실험에서 best-offset data prefetcher(BOP)를 켜두었고 — 이는 periodic access pattern과 Figure 2 listing의 vector load 같은 regular stride를 prefetch한다 — regular stride 및 GHB prefetcher도 실험했으나 CRISP의 상대적 개선이 BOP 대비와 유사해 지면상 생략했다. 방법론은 2-pass이고 **두 pass에 서로 다른 입력**을 쓴다: 먼저 application을 한 번 실행해 PMU counter 측정치와 instruction trace를 얻고(§3.3), critical instruction을 결정해 코드에 주석(prefix)을 넣은 뒤, Scarab에서 scheduler가 criticality를 관측하도록 재실행한다. profiling/slice extraction에는 SPEC의 **train** 입력을, 평가에는 **ref** 입력을 쓰며 다른 application에서도 입력을 달리했다(예: xhpcg의 input dimension 파라미터). 비교 대상 IBDA는 동일한 OOO core 위에서 iterative backwards dependency analysis로 load slice를 추출하는 hardware-only 설계다. (그림 x축 라벨은 cam, deepsjeng, exchange, fotonik, imagick, lbm, leela, mcf, nab, perlbench, sgcc, bwaves, pop, roms, namd, xhpcg, img_dnn, memcached, moses + Average이며, Figure 4/11/12는 mcf 대신 cactus를 포함한다 — 논문 그림 사이의 라벨 불일치.)

**Headline 결과.**

| 결과 | 수치 | 기준 | 출처 |
|---|---|---|---|
| CRISP IPC 개선 | **average 8.4%, maximum 38%** | OOO baseline | Abstract, §1, §5.2 (Figure 7), §8 |
| Microbenchmark UPC | **over 30%** average UPC 개선 (OOO의 peak UPC는 6) | OOO 프로세서 | §1/§2, Figure 1 |
| CRISP vs IBDA | §1: "CRISP improves performance over IBDA by up to **4.3×** while avoiding most of its hardware overheads." §5.2는 같은 숫자를 **"CRISP also significantly outperforms IBDA which achieves an average improvement of only 4.3× % over the baseline."** 로 인쇄한다 — 곱셈 기호와 퍼센트 기호가 함께 찍힌 오식이며, 논문 자체가 이 숫자의 의미(CRISP/IBDA 배율 vs IBDA의 baseline 대비 평균 %)에 대해 내부적으로 일관되지 않다. Figure 7의 Average 막대는 두 해석 모두와 양립한다 | OOO baseline / IBDA | §1, §5.2, Figure 7 |
| Branch slice 단독 이득 | **deepsjeng, lbm, nab, namd**가 branch slice 우선화만으로 **over 3%** IPC 이득 | OOO baseline | §5.3, Figure 8 |
| Branch+load slice 시너지 | **cactus, lbm, perlbench, memcached**에서 combined 성능이 branch-only, load-only 각각보다 모두 높음 | 각각 단독 적용 | §5.3 본문 (Figure 8의 x축에는 cactus 막대가 없다 — Figure 4/11/12에만 등장) |
| RS/ROB 민감도 | **Xhpcg**의 IPC 이득이 Skylake 구성의 **12.5%**에서 Sunny-Cove-like core에서 **over 25%**로 상승. **moses**는 오히려 더 작은 **64RS/180ROB** 구성에서 최대 이득 — 큰 ROB가 이미 성능을 상당히 회복시켜 CRISP의 상대 이득이 줄기 때문. 세 구성 모두에서 CRISP는 유의한 개선 제공 | 각 구성의 OOO baseline | §5.4, Figure 9 |
| Criticality threshold sweep | T = 5% / 1% / 0.2% 중 **1%가 전체 평균 최적**, moses는 **2%**에서 최적이고 다른 대부분의 application은 그 threshold에서 이득이 줄어듦 | OOO baseline | §5.5, Figure 10 |
| Critical instruction 수 | perlbench, gcc, moses에서 **over 10,000 unique instructions** | — | §5.6, Figure 11 |
| Code footprint 오버헤드 | static code size 증가는 minimal, **dynamic footprint +5.2% 평균**, **instruction cache MPKI worst-case +2.6%** | prefix 없는 binary | §5.7, Figure 12 |
| 분석 비용 | 100M instruction trace 5GB (1.6GB compressed), 분석+slice 추출 ≈ 100초 | — | §4.1 |

**IBDA가 CRISP를 못 따라오는 이유 — application별 진단 (§5.2, Figure 7).**

- **moses:** load slice가 너무 길고 커서 IST에 담기지 않음.
- **lbm:** IBDA에 branch slice 추출 능력이 없어, **infinite IST**에서도 CRISP에 못 미침.
- **namd, Xhpcg:** memory를 통한 dependency를 따라가지 못해 중요한 load slice를 놓침.
- **bwaves:** 높은 LLC MPKI를 보이지만 high-MLP phase에서 실행되어 실제로는 performance-critical이 아닌 load를 잘못 잡음.
- **fotonik, perlbench, moses:** critical path analysis가 없어 load slice에서 non-critical instruction을 너무 많이 고르고, 그 결과 **baseline보다 성능이 떨어짐**.

**측정 지표에 관한 저자의 단서 (§5.2).** "We do not show MPKI numbers as CRISP only reorders memory accesses without reducing cache misses. A helpful metric to confirm the IPC gains is to count the cycles that instructions reside at the head of the ROB without retiring. We can observe that CRISP indeed reduces these stall cycles as reflected by the IPC gains." — 즉 8.4%를 뒷받침하는 메커니즘 증거는 MPKI가 아니라 ROB head stall cycle의 감소이며, 논문은 이 stall cycle 수치를 별도 그림/표로 인쇄하지는 않는다.

### 4.8 저자가 밝힌 limitation과 self-positioning

> 구현 계보에 관한 저자 자신의 언급: §3.3은 load slice 추출 구현이 "similar to prior work that leverages dataflow analysis for classifying memory access patterns [6]"라고 밝힌다.

**저자가 인정한 한계·미해결 문제**

1. **Threshold가 application-specific이다.** §5.5는 어떤 단일 miss threshold도 모든 application에 최적이 아님을 보이고(1%가 전체 최적, moses는 2%), "For future work, we envision an iterative mechanism that profiles applications with different miss ratio thresholds to enable additional application-specific optimizations."라고 남긴다. §3.2도 같은 목적의 multiple profiling pass를 이미 흐름에 포함시켜 놓았다.
2. **criticality 비율이 5%–40% 밴드를 벗어나면 작동하지 않는다.** memory-bound application처럼 프로그램 instruction의 큰 비중이 최소 하나의 load slice에 속하는 경우, 대부분이 critical로 표시되면 "the scheduler will have no opportunity to prioritize any instruction" (§3.2).
3. **관측 가능성의 제약.** "modern CPUs generally lack the observability for directly measuring all of the above metrics"라서 PMU/PEBS/LBR/PT 대리 지표로 유도된 heuristic에 의존한다. Intel PT로 memory를 통한 dependency를 관측하려면 Kaby Lake에서 제공되는 PTWrite instruction이 필요하다 (§3.2, footnote 1·2 — AMD/ARM도 유사 기능을 제공한다고 각주에 명시).
4. **cache miss 자체는 줄지 않는다.** CRISP는 memory access를 재배치할 뿐이므로 MPKI를 증거 지표로 쓸 수 없다 (§5.2).
5. **instruction cache 압력 증가.** prefix가 dynamic code footprint를 평균 5.2% 늘리고 icache MPKI를 worst case 2.6% 늘린다 (§5.7).
6. **Timing risk.** picker logic이 여전히 가장 timing-critical한 stage라면 AND 게이트를 앞 pipeline stage로 옮겨 PRIO와 RDY를 같은 cycle에 생성해야 하고, 이 구현은 저장 오버헤드를 2/n으로 두 배로 만든다. 그럼에도 §5의 평가는 baseline과 동일한 scheduling latency를 가정한다 (§4.3).
7. **criticality 확장의 미해결 지점 (§6.1).** division 같은 다른 고지연 instruction도 CRISP로 가속할 수 있지만 "the latency can depend on the input operands"라 특정 instruction의 정확한 성능 영향을 정하기 어렵고, 이를 위해 "adding new events to the PMU for determining the PC of arbitrary instructions that induce significant stall cycles"를 구상한다. 그 밖에 (a) AVX-512처럼 비활성 phase 이후 첫 instruction에서 큰 transition latency가 발생하는 vector instruction을 우선화해 additional AVX-512 instruction이 pipeline을 막기 전에 vector unit을 켜는 방안, (b) criticality 정보를 load queue·on-chip interconnection network·DRAM scheduler로 전파하는 방안, (c) SMT 환경에서 latency-sensitive thread의 instruction을 우선화해 tail latency 같은 SLO를 강제하면서 높은 CPU 이용률을 함께 얻는 방안을 향후 기회로 제시한다.
8. **Security 노출 (§6.2).** 위 §4.5에 정리한 SMT criticality side channel과 all-critical tagging DoS.

**Related work에서의 self-positioning (§7).** 논문은 선행 연구를 latency-avoiding / latency-tolerating으로 나눈다.

- **Stream / pattern(delta)-based prefetcher:** 연속 cache miss의 effective address 간 delta를 학습하며 "moderate hardware complexity"로 simple stride와 periodic pattern을 prefetch한다.
- **Spatial prefetcher:** delta prefetcher보다 metadata 저장이 크고 page 내 임의 line을 기억해 coverage를 높이지만 "still limited to prefetching recurrent patterns"이라 linked list traversal 등 irregular access는 prefetch하지 못한다. 여기서 CRISP는 대체재가 아니라 **보완재**로 위치한다: "CRISP can be combined with these prior approaches to increase coverage by reducing the miss penalty of irregular memory accesses."
- **Temporal prefetcher:** Markov prefetching 기반으로 cache line 접근의 시간 순서를 추적하며 "significant storage overheads in the order of megabytes in contrast to CRISP"를 도입한다.
- **Runahead prefetcher / helper thread:** linked-list traversal 같은 irregular access를 prefetch하지만 "significant hardware complexity"를 도입하거나 별도 SMT thread를 소모하는 반면 CRISP는 최소한의 hardware 수정만 요구한다. Branch runahead는 hard-to-predict branch의 실행을 우선화해 misprediction penalty를 줄이지만 다른 runahead 기법처럼 큰 hardware complexity와 pipeline 수정을 수반한다.
- **OOO 실행(latency toleration):** delinquent load 뒤에 충분한 독립 instruction이 없으면 성능을 개선하지 못한다 (Figure 1).
- **Criticality 기반 scheduling:** Fiforder, Long-term parking, Delay-and-Bypass는 IQ를 ready/non-ready·critical/non-critical 하위 큐로 분할해 scheduling energy efficiency를 높인다. Load-Slice Core(LC), Forward Slice Core, Freeway, Front-end Execution Architecture는 non-critical instruction을 in-order pipeline에서 실행하고 load는 bypass시키는 구조다. 논문의 대비 문장: "In contrast to CRISP, all of these prior works do not improve delinquent load latency and, in fact, often reduce performance by 5% [102] to 9% [3] over an OOO baseline." — **주의: 이 수치는 §2와 상충한다.** §2는 Long Term Parking과 Delay-and-Bypass가 각각 "10% and 5%"를 희생한다고 적는데, §7의 인용 번호는 [102]=Long-term parking을 5%, [3]=Delay-and-Bypass를 9%에 대응시켜 어느 쪽이 더 많이 잃는지가 뒤바뀐다. 두 값을 모두 그대로 옮겨 적는다.
- **Criticality Driven Fetch:** critical instruction chain을 판정해 fetch/allocation/execution을 우선화하지만 "requires considerable modifications of the entire processor pipeline".
- **NOREBA:** non-speculative instruction을 early-retire해 ROB 슬롯을 비우는 hardware-software co-design으로, load 이후 instruction의 추가 issue는 가능하게 하지만 "it does not enable issuing instructions before the load".
- **Criticality 기반 cache 할당 (Balasubramonian, Subramaniam, Nori):** cache의 latency/power를 줄이려 data allocation을 최적화하나 "requiring significant cache modifications in contrast to CRISP". 논문은 위 hardware 기법 전체를 두고 "All these works determine criticality solely in hardware introducing complexity and overheads while limiting accuracy"라 요약하고, 구체적 사례로 LC가 per-load cache miss rate 측정에 상당한 metadata 저장이 필요해 **모든 load를 critical로 취급**한다는 점, 그리고 IBDA 기반 slice 추출이 memory를 통한 dependency를 못 봐 incomplete slice를 낳는다는 점을 든다.
- **Recovery Buffer / Waiting Instruction Buffer:** cache miss의 dependent instruction을 별도 큐로 옮겨 main IQ 크기를 줄이지만, CRISP와 달리 delinquent load *이후*의 instruction을 대상으로 하며 목표가 energy efficiency다.
- **Slack 기반 접근 (Srinivasan, Fisk, Fields, Muthler):** slack이 큰 load를 느린 hardware resource로 처리해 latency-critical load에 자원을 몰아주지만, processor frontend 재설계로 인한 큰 hardware 오버헤드와 복잡도를 도입하고, slack 추정에 microarchitectural simulation이 필요해 "inconsistencies between the simulator and the real hardware"를 겪는다.
- **Focused value prediction:** 장지연 load로 흘러드는 결과를 예측해 dependent instruction의 critical path 길이를 줄이며, 논문은 이를 **보완재**로 규정한다: "Value prediction is complementary to CRISP as breaking dependency chains exposes more ILP, generating additional opportunities for criticality-based scheduling mechanisms."

**§1의 contribution 목록 (저자 자신의 self-positioning 요약).** ① runahead prefetching 고유의 redundant instruction 실행 회피, ② hardware에서 load slice를 추출하는 복잡도·저장 오버헤드 회피, ③ criticality 추출을 software에서 수행해 flexibility 증가, ④ memory dependency 관측으로 더 높은 miss coverage와 accuracy, ⑤ load slice 중 critical instruction만 추출, ⑥ branch misprediction 영향을 줄이는 branch slice 도입, ⑦ low-complexity criticality-observing matrix-based instruction scheduler 도입.

### 4.9 Section map과 인용 가능한 문장

| § | 주제 |
|---|---|
| Abstract | 문제 정의, criticality-based scheduling 제안, up to 38% / 8.4% average |
| §1 Introduction | latency-avoiding vs latency-tolerating 분류, 선행 연구의 두 결함(high hardware complexity, low flexibility), scheduling의 NP-Hard성, Figure 1 pointer-chasing UPC microbenchmark, contribution 7개, "up to 4.3× over IBDA" |
| §2 Background | decoupled frontend, TAGE, ROB와 oldest-first policy, criticality가 scheduler에 없다는 문제, LTP/Delay-and-Bypass의 10%/5% 성능 희생과 67%/47% frontend energy efficiency |
| §3 Instruction Criticality | delinquent load와 load slice 개요 ("as simple as a single constant or might involve hundreds of prior instructions") |
| §3.1 Motivating Example | Figure 2 linked-list pseudocode, Xeon Gold 5117 + GCC 9.3 실측 IPC 1.89 → 2.71 |
| §3.2 Determining Delinquent Loads | critical load 3-part 정의, 20% LLC miss threshold, 5%–40% critical 비율, multiple profiling pass, criticality factor 6개, PMU/PEBS/LBR/PT 관측 수단, 파생 heuristic(>5% of loads, >20% miss ratio, MLP < 5) |
| §3.3 Load Slice Extraction | DynamoRIO Memtrace / Intel PT, backward dependency walk, `frontier` queue, 종료 조건 4개, Figure 3 pointer-chase assembly walkthrough, criticality prefix 삽입 |
| §3.4 Branch Slice Extraction | perfect branch predictor 실험, misprediction rate > 15% loop, branch slice 정의, lbm, load slicing과의 시너지 |
| §3.5 Why Hardware-Only Techniques Are Insufficient | IBDA/LTP/LSC의 4가지 결함(profiling 부재, memory dependency 불가, critical path analysis 불가, on-chip 저장 한계), Figure 4 average load slice size, DAG critical-path pruning과 AMAT |
| §3.6 Why Software-Only Techniques Are Insufficient | LLVM `Hoist()`, loop-dependent load, cold→hot block 위험과 PGO-based hoisting, VLIW static scheduling의 열등함, squash-on-misprediction 정확성 논거 |
| §4 Implementation | 개요 |
| §4.1 Software Support | FDO/post-link-time optimization 흐름, Figure 5, GWP/ODS/AsmDB, uncommon path 필터링과 slice 병합, trace 크기와 분석 시간, AutoFDO/BOLT/AsmDB/I-SPY/Ripple/Twig 선례 |
| §4.2 Hardware Support | decoder prefix 해석, encoding vs sideband, unified RS와 per-FU scheduler 양쪽 지원, RAND vs SHIFT scheduler, age matrix 동작, BID/PRIO vector, Figure 6 |
| §4.3 Storage and Timing Overhead | 1/n IQ bit, AND 1단 + mux, transmission gate, 2/n 대안, baseline과 동일 scheduling latency 가정, icache 오버헤드는 §5.7로 연결 |
| §5 Evaluation | 평가 계획 개요 |
| §5.1 Methodology | Scarab + Ramulator, Table 1, SPEC2017/Xhpcg/Tailbench, 200M instruction, BOP prefetcher, train vs ref 2-pass |
| §5.2 IPC Improvement of CRISP | Figure 7 (CRISP vs OOO, IBDA-1K/8K/64Kx/INF), 8.4% average / 38% max, IBDA 실패 원인 application별 분석, MPKI 대신 ROB-head stall cycle 지표 |
| §5.3 Branch Slicing | Figure 8, branch-only >3% (deepsjeng, lbm, nab, namd), 시너지 (cactus, lbm, perlbench, memcached) |
| §5.4 Reservation Station Size Sensitivity Study | Figure 9, 64RS/180ROB·128RS/352ROB·150RS/400ROB, Xhpcg 12.5%→>25%, moses는 작은 구성에서 최대 |
| §5.5 Load Slice Criticality Threshold Study | Figure 10, T = 5%/1%/0.2%, 1% 최적, moses 2% |
| §5.6 Total Number of Critical Instructions | Figure 11, perlbench/gcc/moses에서 >10,000 unique critical instruction, IBDA면 100's of KB |
| §5.7 Instruction Prefix Overhead | Figure 12 static vs dynamic footprint, +5.2% dynamic, worst-case icache MPKI +2.6% |
| §6 Discussion | 개요 |
| §6.1 Further Exploiting Criticality | division과 operand-dependent latency, 새 PMU event, AVX-512 transition latency, load queue/on-chip interconnect/DRAM scheduler로의 criticality 전파, SMT SLO/tail latency |
| §6.2 Security Implications | Spectre/Meltdown 맥락, SMT criticality side channel, PORTSMASH/TLBleed/CacheBleed/MemJam 대비, all-critical tagging DoS와 완화책 |
| §7 Related Work | stream/delta/spatial/temporal prefetcher, runahead와 helper thread, branch runahead, OOO latency toleration, criticality scheduling (Fiforder, LTP, Delay-and-Bypass, LC, Forward Slice Core, Freeway, FXA, Criticality Driven Fetch, NOREBA), criticality 기반 cache 할당, Recovery Buffer/WIB, slack 기반 접근, focused value prediction |
| §8 Conclusion | 메커니즘 재진술과 up-to-38% / 8.4%-average 결과 |

**인용 가능한 문장 (verbatim)**

1. "We propose a lightweight mechanism to hide the high latency of irregular memory access patterns by leveraging criticality-based scheduling. In particular, our technique executes delinquent loads and their load slices as early as possible, hiding a significant fraction of their latency." (Abstract)
2. "To avoid stalls due to a full ROB, schedulers generally prefer to issue the oldest instructions as early as possible to increase the likelihood that the instruction at the head of the ROB can retire. However, in many cases, executing latency-critical instructions early can improve performance far more than selecting the oldest instructions, but information about criticality is generally unavailable to the scheduler." (§2)
3. "This issue also cannot be easily addressed with the compiler as it requires re-ordering of instructions across loop-iterations." (§1)
4. "The kernel executes at an IPC of 1.89, whereas if we manually move the pointer-chasing memory operation of the next iteration to before the vector multiplication by inserting the prefetch in line 12, IPC increases to 2.71." (§3.1)
5. "We define a load to be critical if its last level cache (LLC) miss rate is higher than a particular threshold, for instance, 20% (Section 5.5 explores this threshold), its memory address cannot be easily predicted by the hardware prefetcher (not a constant or stride), and if the number of independent instructions behind the load in the sequential instruction stream is small." (§3.2)
6. "We empirically determined that the prioritization of critical instructions performs best if the ratio of critical instructions among all instructions is 5%-40%. In other words, there must be a sufficient mix of non-critical instructions for the scheduler to deprioritize, in order to hide the latency of the critical loads." (§3.2)
7. "Hardware methods that utilize IBDA, capture incomplete instruction slices as they can only observe dependencies through registers, but not memory which is crucial for x86 due to register spilling." (§3.5)
8. "These early-executed, critical instructions do not affect correctness if misspeculated, as they are squashed by the hardware after resolving the misprediction." (§3.6)
9. "To support CRISP, each slot in the IQ is extended with a single bit to identify its priority, resulting in a space overhead of 1/𝑛." (§4.3)
10. "We do not show MPKI numbers as CRISP only reorders memory accesses without reducing cache misses. A helpful metric to confirm the IPC gains is to count the cycles that instructions reside at the head of the ROB without retiring." (§5.2)
11. "While the static code size increases only minimally, the dynamic code footprint increases more significantly, by 5.2% in average, as critical instructions are common in hot loops." (§5.7)
12. "In contrast to CRISP, all of these prior works do not improve delinquent load latency and, in fact, often reduce performance by 5% [102] to 9% [3] over an OOO baseline." (§7)

---

## 5. RFP — "Register File Prefetching" (ISCA 2022)

> Sudhanshu Shukla, Sumeet Bandishte, Jayesh Gaur, Sreenivas Subramoney · Processor Architecture Research Lab, Intel Labs
> 원문: [PDF](</home/lee/scarab/reference/[2022, ISCA] Reg File prefetching.pdf>)

*ISCA '22, June 18–22, 2022, New York, NY, USA. ACM, 14 pages, pp. 410–423. DOI 10.1145/3470496.3527398. p.410 각주: "Concepts, techniques and implementations presented in this paper are subject matter of pending patent applications, which have been filed by Intel Corporation."*

**Problem.** 논문은 memory wall이 하나의 벽이 아니라 계층마다 존재하는 여러 개의 latency wall이라고 주장한다 (§1). 기존 prefetching 연구는 거의 전부 DRAM latency를 겨냥하지만, oracle prefetching 실험(Fig. 1a)은 L1 data cache → Register File 구간에도 9.0%의 성능 헤드룸이 있음을 보인다. L1 latency가 main memory보다 40배 낮은 5 cycles임에도 이런 헤드룸이 생기는 이유는 volume이다 — demand load의 92.8%가 L1에서 hit하기 때문에(Fig. 2) 작은 L1 latency가 크게 증폭된다. 게다가 critical path는 LLC miss load 하나가 만들지만, 그 miss load의 주소를 계산해 주는 dependence chain 위의 L1 hit load들도 critical path 길이에 그대로 기여한다(Fig. 3). L1 latency를 숨길 수 있는 기존 기법은 논문의 프레이밍에서 모두 불충분하다. Value Prediction(§2.1)은 mis-speculation flush가 비싸서 매우 높은 confidence를 요구하고 그 결과 coverage가 제한된다. Load Address Prediction(§2.2, DLVP·Composite·EPP)은 예측 데이터용과 validation용으로 cache access를 두 번 써서 희소한 L1 bandwidth를 압박하고, 주소가 틀리면 full pipeline flush가 필요하며, in-flight store 때문에 memory aliasing predictor로 심하게 throttle되어야 하고, uop-cache와 loop-stream-detector가 front-end latency를 없애 버려 "fetch 단계에서 미리 던진다"는 전제 자체의 run-ahead window가 사라졌다. L0 cache(§2.4)는 L1 latency가 cache lookup만이 아니라 address calculation·address translation·rotator 회로까지 포함하기 때문에 절약 가능한 비율이 작고, address translation을 우회하고 store-forwarding check를 예측/flush로 대체해야 해서 "very complex to build"라고 기각된다.

### 5.1 Motivation과 characterization

| 항목 | 수치 | 출처 |
|---|---|---|
| Oracle prefetching 헤드룸 (level N → level N−1) | **L1→RF 9.0%**, L2→L1 2.3%, LLC→L2 2.9%, Memory→LLC 13.3% (baseline 대비 성능) | Fig. 1(a) |
| 모델링된 계층 latency | L1 D-cache 48KB 12-way **5 cycles**, L2 1280KB 20-way **14 cycles**, LLC/core 3MB 12-way **39–45 cycles**, DRAM **200–400 cycles** | Fig. 1(b) |
| L1 latency 대 memory latency 비 | "40X lower latency (5 cycles in Tiger Lake)" / main memory 200 cycles | Abstract, §1 |
| Demand load 요청 분포 | **L1 hits 92.8%**, MSHR hits 3.4%, L2 hits 1.6%, LLC hits 0.2%, Memory 0.3% | Fig. 2 |
| Allocation 시점 operand 준비 상태 | **63%의 load는 allocate될 때 operand가 준비되어 있지 않다** → Rename 이후 launch해도 RFP를 완료할 시간이 충분. 나머지 37%도 OOO scheduling pipeline이 최소 3 cycles이므로 modest run-ahead window가 남는다 | §3 (Timeliness) |
| DLVP coverage 깔때기 | address-predictable 비율은 RFP와 비슷하나 → high confidence gating 후 **49%**(APHC) → no-FWD predictor 후 **45%** → port 부족으로 probe launched **22%** → 제때 완료된 probe(ProbeSuccess) **11%** | Fig. 16, §5.4 |

**Fig. 3의 critical-path 예제 (p. 411).** 표는 "Time" 헤더 아래 A / D / WB 세 컬럼을 가진다(논문은 약어를 풀어 쓰지 않는다). 실제 표 내용은 다음과 같다.

| # | Instruction | A | D | WB | label |
|---|---|---|---|---|---|
| 1 | `ld %r1, ($0xabcd)` | 0 | 0 | 30 | **LLC Hit** |
| 2 | `ld %ecx, (%r1)` | 1 | 30 | 35 | **L1 Hit** |
| 3 | `xor %edx, %r2` | 1 | 1 | 2 | — |
| 4 | `ld %ebx, (%ecx, %edx)` | 2 | 35 | 40 | **L1 Hit** |
| 5 | `ld %r3, ($0x1234)` | 2 | 2 | 7 | — |
| 6 | `shrq %r3, $0x2` | 3 | 7 | 8 | — |
| 7 | `addl %r2, %r3` | 3 | 8 | 9 | — |
| 8 | `ld %eax, (%ebx, $0x3)` | 4 | 40 | **240** | **LLC Miss** |
| 9 | `inc %eax` | 4 | 240 | 241 | — |

- Critical path(실선)는 I1 → I2 → I4 → I8 → I9이다. I3와 I5·I6·I7은 data-flow(점선)로만 연결되고 critical path에는 포함되지 않는다.
- Critical path를 만드는 것은 instruction 8의 LLC miss이며, **240은 latency가 아니라 write-back 시각**이다 (D=40에 dispatch되어 WB=240, 즉 §1이 말하는 200-cycle main memory latency와 일치).
- 그 miss load의 주소 dependence chain 위에서 "L1 Hit"으로 표시된 load는 **instruction 2와 4** 둘뿐이다. Instruction 1은 LLC Hit(0→30)이고, instruction 5는 `%r3`를 만들지만 `%r3`는 instruction 8의 주소 `(%ebx, $0x3)`의 입력이 아니므로 이 chain 위에 있지 않다.
- Figure 3 캡션: "The critical path is created by an LLC Miss, but all L1 hits on the dependence chain of the LLC Miss contribute to critical path length."

### 5.2 Key insights

- **Memory wall은 여러 개의 latency wall이다.** L1 hit이 전체 load의 92.8%를 차지하므로, 5 cycles라는 작은 L1 latency도 "highly magnified impact"를 갖고 9% oracle 헤드룸으로 나타난다 (§1, Fig. 1–2).
- **Register File은 tag가 없다는 것이 해결해야 할 과제다.** cache와 달리 RF는 tagged가 아니므로, prefetch한 데이터를 어떤 load에 귀속시킬지 알려 줄 identifier — 즉 physical register file 내의 위치 — 가 *필요하다*. 이 필요 때문에 prefetch는 Rename 이후에 수행되어야 하며, 그래야 load에 이미 할당된 register file entry를 재사용할 수 있다 (§2.3, §3 Timeliness).
- **Rename 직후가 유일하게 옳은 launch 지점이다.** Fetch에서 던지면 (a) uop-cache 때문에 run-ahead가 거의 없고, (b) 모든 in-flight store를 추적해야 correctness를 보장할 수 있어 OOO window가 커질수록 "extremely complex"해지며, (c) prfid가 아직 없다. Rename 이후면 기존 memory disambiguation logic을 그대로 재사용할 수 있고, 63%의 load는 아직 operand가 준비되지 않은 상태다 (§3 Timeliness, §3.2).
- **주소만 맞으면 데이터는 자동으로 맞다 → validation access가 필요 없다.** RFP는 실제 load와 똑같이 older store를 스캔하고 memory disambiguation을 거치므로, 예측 주소가 맞았다면 RF의 데이터는 cache/in-flight store와 up-to-date이다. 따라서 두 번째 L1 access가 불필요하고, 올바른 prefetch에서는 L1 lookup 횟수가 baseline과 동일하다 (§3 Bandwidth, §3.2.1).
- **틀린 prefetch가 싸다 → accuracy 요구를 낮추고 coverage를 산다.** 주소가 틀려도 pipeline flush가 아니라 speculatively scheduled dependents의 cancel/re-dispatch(이미 OOO pipeline에 존재하는 경로)와 cache re-lookup만 든다. 그래서 1-bit confidence counter처럼 공격적인 설정이 가능하다 (§3 Accuracy, §3.3, §5.5.1).
- **"3 cycles = 3 cycles"는 우연이 아니다.** RFP-inflight bit는 (L1 hit 가정 시) RFP 완료 3 cycles 전에 세팅되고, wakeup→select→PRF read/scoreboard의 scheduling pipeline도 3 cycles이다. 이 등식 덕분에 bit가 load wake-up 시점에 이미 서 있으면 dependents가 RFP 완료 직후에 정확히 execution에 도달해 load latency 전체가 절약된다 (§3.3).
- **RFP와 VP는 경쟁이 아니라 상보적이다.** VP는 dependency를 끊지만 높은 accuracy 요구 때문에 coverage가 좁고, RFP는 coverage가 넓지만 L1 bandwidth와 pipeline latency에 묶인다. 서로의 제약이 다르므로 합치면 각각보다 높다 (§5.3).

### 5.3 Hardware structures

| 구조 | 목적 | 구성 (entry 수 / associativity / indexing / 파이프라인 위치) | per-entry 필드와 비트폭 |
|---|---|---|---|
| **Prefetch Table (PT)** | static load PC로 색인되는 stride predictor. RFP-eligible load를 표시하고 예측 주소를 생성 | 8-way set-associative, 1024–2048 entries(기본 1K), load PC indexed. Lookup은 Rename **이전** front-end에서 수행 **가능**하며 timing critical path는 아니라고 논문은 밝힌다(§3.2는 "can happen earlier"로 서술 — 필수 요건이 아니라 선택지). 수행하며 "we do not expect it to be on the timing critical path". Training은 **load retirement** 시점 (Fig. 5) | 논리적 필드(§3.1): Tag 16b, Utility 2b, Confidence 1b, Stride 8b, Inflight counter 7b, Virtual Address 64b. 실제 area-optimized 엔트리(Table 1): Tag 16b, Confidence 3b, Utility 2b, Stride 5b, Inflight 7b, PAT Pointer 6b, Page Offset 12b |
| **Page Address Table (PAT)** | area 최적화. 자주 나타나는 Page Frame number(주소 bits 63:12)를 한 번만 저장해 PT가 full 64-bit VA 대신 pointer+offset만 갖게 함. 유사 아이디어는 area-efficient BTB 연구 [75]에서 차용 | 64 entries, 4-way set-associative. PT entry가 6-bit set-way pointer + 12-bit page offset을 들고 있다가 full virtual address를 재구성 (§3.5) | Page Address 44b |
| **RFP-inflight bit** | prefetch의 진행 상태를 해당 load의 scheduler entry에 알려 dependents를 이른 시점에 깨우게 함 | load의 **RS(Reservation Station / Instruction Queue) entry**에 1비트 추가. Table 1은 128 entries × 1b로 계산. Addr calculation stage 다음의 **첫 L1 lookup cycle**에 세팅 — packet이 더 이상 drop되지 않음이 보장되는 가장 이른 시점 (§3.3) | 1b × 128 entries = 128b |
| **RFP FIFO queue** | 빈 L1 load port를 기다리는 prefetch packet 보관 | 64-entry FIFO. FIFO로 조직되어 오래된 RFP request가 젊은 것보다 우선. 이 queue의 요청은 regular demand load 대비 **최저 우선순위**로 L1 port를 놓고 arbitrate (§3.2, §3.5) | 논문 미보고 (packet 내용 외 별도 필드 명세 없음) |
| **Prefetch packet** | RFP 요청 그 자체. LSQ와 L1 data cache로 전달 | Rename 직후 생성 (prfid가 rename 이후에만 알려지므로). 다만 prfid는 writeback에서만 필요하므로 "strictly, the prfid can be supplied later in the RFP pipeline" (§3.2) | prefetch할 virtual address + load의 physical destination register id (**prfid**) |
| **재사용되는 기존 구조 (신규 storage 아님)** | RFP는 새 로직 추가 대신 기존 OOO 기계를 재사용 | LSQ의 older-store scan과 Memory Disambiguation(Store Sets [14]) → correctness (§3.2.1). Stark et al. [76]의 wire-OR wakeup logic과 Dependence Vector, 그리고 이미 존재하는 cancel/re-dispatch 경로 → early wakeup과 취소 (§3.3). Yoaz et al. [80]의 hit-miss predictor는 baseline에 이미 존재한다고 가정 (§2.5) | Dependence Vector: RS entry **당 2개의 bit-vector**(x86 load의 operand 하나당 하나), 각 vector의 폭 = 전체 RS entry 수. RS entry당 RFP 추적용 1비트 추가 |

### 5.4 Operation — stage by stage

**(1) Training — load retirement (§3.1).**
PT는 load retirement에서 학습된다("which simplifies the logic for determining strides across addresses"). 매 load retire마다 load PC로 PT를 lookup한다.
- stride가 반복되면 → confidence를 **확률 1/16**으로 증가시키고 utility를 증가.
- confidence가 saturate하면 그 load PC는 RFP-eligible이 된다.
- stride가 바뀌면 → confidence와 utility를 모두 reset. stride가 계속 흔들리면 utility가 낮게 유지되어 결국 evict된다.
- **Inflight counter**(7b)는 그 PT entry에 해당하는 outstanding dynamic load 인스턴스 수를 센다(선행 연구 [31, 65]와 유사). load allocation에서 +1, load commit에서 −1, branch misprediction 시 squash된 load마다 −1.
- 예측 주소 = **base address + stride × inflight counter** (선행 연구 [31, 65, 68] 방식).
- 논문은 DLVP의 Path-based Address Predictor [67]와 유사한 context-based prefetcher도 실험했다 (§3.1, 결과는 §5.5.3).

**(2) Front-end lookup → Rename → packet 생성 (§3.2).**
load PC로 PT를 lookup해 RFP-eligible load를 표시한다(논문은 Rename보다 앞선 front-end에서 수행 "가능"하다고만 밝히며 timing critical path는 아니라고 본다 — §3.2, Fig. 5). Rename에서 destination physical register가 할당되어 **prfid**가 확정되므로, prefetch request는 register renaming 직후 즉시 trigger된다. packet = {예측 virtual address, prfid}이며 64-entry RFP FIFO에 들어간다.

**(3) Port arbitration (§3.2, §3.4).**
packet은 다른 load / 다른 RFP request와 L1 port 접근을 놓고 경쟁하되 **항상 최저 우선순위**를 받는다("This prevents latency escalation for regular loads"). FIFO 내부에서는 older RFP가 younger RFP보다 우선. **load port는 추가하지 않고** 비어 있는 port를 기회주의적으로 사용한다.

**(4) L1/LSQ 실행 (§3.2.1).**
dispatch된 RFP는 conventional load처럼 LSQ와 L1으로 간다.
- 모든 older store를 **youngest-first order**로 스캔하며 자신의 주소와 store 주소를 비교.
- match하면 그 store가 완료되기를 기다렸다가 cached data 대신 **store data**를 사용.
- store의 주소가 아직 없으면(= store가 아직 실행되지 않았으면) 기존 **Memory Disambiguation** 메커니즘 [14]이 wait할지 skip할지 결정하고, 그 예측에 따라 older store 또는 L1에서 가장 최신 데이터를 얻는다.
- 이 시퀀스 전체가 conventional load pipeline과 동일하므로, 주소가 맞았다면 데이터도 맞다 → validation용 두 번째 L1 access 불필요.
- **Pipeline simplifications (§3.2.2):** RFP가 L1을 miss하면 demand load처럼 하위 계층으로 진행한다. **DTLB miss가 나면 RFP를 drop**한다(TLB miss는 latency가 길어 남은 run-ahead가 없으므로).

**(5) Scheduler 상호작용 (§3.3, Fig. 6·9).**
Addr calculation 다음의 첫 L1 lookup cycle에 RFP-inflight bit를 세팅한다. 이 타이밍이 중요한 이유:
- packet 생성 시점에 세팅할 수 없다 — port 제약으로 아직 prefetch가 drop될 수 있고, 그 사이 load가 ready가 되면 prefetch는 취소된다. 즉 dependents를 깨울 책임이 여전히 load에 있다.
- 너무 늦게 세팅해도 안 된다 — load가 in-flight prefetch를 모른 채 자기 요청을 내보내 LSQ/cache에서 contention을 만든다.

load가 wake up할 때 이 bit를 확인한다.
- bit가 **clear**: hit-miss predictor 경로대로 load wake-up 후 **5 cycles**(L1 latency) 뒤에 dependents를 깨운다.
- bit가 **set**: dependents가 RFP의 예상 완료 시각에 execution에 도달하도록 깨운다. bit는 RFP 완료 3 cycles 전에 서고 scheduling pipeline도 3 cycles이므로 stall 없이 맞아떨어지며 **load latency 전체가 절약**된다.
- RFP-inflight가 load wake-up보다 다소 늦게 트리거되면 latency의 **일부만** 절약된다.

```
[Fig. 8] RFP 없는 ADD-on-LOAD schedule
L1(LOAD) : Wakeup | Select | RegRead/Scoreboard | Addr | L1 | L1 | L1 | L1
A1(ADD)  :                                        Wakeup | Select | RegRead/SB | Execute
           └ A1은 L1이 schedule된 뒤 5 cycles에 깨어난다 (hit-miss predictor 기반 speculative wakeup)

[Fig. 9] RFP-assisted schedule
L1(LOAD) : Wakeup | Select | RegRead/Scoreboard | Addr | L1 | L1 | L1 | L1
             \__ "RFP-inflight High"
A1(ADD)  :          Wakeup | Select | RegRead/SB | Execute
           └ 주소가 맞으면 load는 L1 access를 통째로 skip (Skip L1 Access)
           └ 주소가 틀리면 load가 L1 Access를 수행 (Prefetched Address Wrong: L1 Access)
```

**(6) 사용 또는 복구.**
load가 execute될 때 예측 주소와 실제 주소를 비교한다.
- **일치**: dependents가 RF의 prefetched value를 소비하고, load는 cache를 완전히 우회한다.
- **불일치**: prefetched data가 틀렸으므로 speculatively scheduled dependents를 모두 cancel하고, load가 dispatch될 때 다시 issue한다. load는 LSQ와 cache를 re-lookup한다.
- **RFP가 L1 miss**: speculatively scheduled dependents를 cancel하고, 하위 계층에서 데이터가 도착하면 re-issue한다.

**(7) Pipeline variations (§3.3).**
논문의 기본 가정은 load가 ROB에 allocate될 때 physical register file이 할당된다는 것이다. Monreal et al. [51]처럼 원래 load에는 virtual pointer만 주고 최종 register file entry는 writeback에 주는 파이프라인 변형도 있는데, RFP는 여기에도 쉽게 적응된다 — dispatch 이후 RFP는 load처럼 행동하므로 RF entry를 자신이 배정받고, prefetch가 틀렸을 경우 그 동일한 RF entry를 demand load에게 돌려주면 된다.

### 5.5 Correctness와 speculation 제어

RFP는 **주소에 대해서만 투기적**이다. 주소가 맞으면 데이터의 architectural correctness는 자동으로 보장된다.

- **In-flight stores (§3.2.1).** RFP는 launch 시 모든 older store를 youngest-first로 스캔하고 주소를 매칭한다. match면 store 완료를 기다려 store data를 쓴다. store 주소가 아직 없으면 기존 MD 메커니즘 [14]의 예측에 따라 wait/skip을 결정한다. 이 시퀀스가 conventional load pipeline과 "the same"이므로 correct address ⇒ correct data가 성립하고, 두 번째 L1 access가 제거되며 L1 lookup 수·bandwidth 요구가 baseline과 동일하게 유지된다. **추가 L1 bandwidth는 오직 wrong prefetch에서만 필요하다.**
- **MD misprediction.** MD 예측이 틀린 것으로 판명되고 load가 **이미 dispatch되었다면 pipeline을 flush하고 load부터 재실행**한다. MD 예측이 틀렸지만 **load가 아직 dispatch되지 않았다면 flush는 필요 없다** — load가 prefetched data를 쓰지 않고 LSQ와 cache를 다시 lookup하면 된다.
- **Address misprediction은 절대 flush를 유발하지 않는다.** 주소 불일치를 감지하면 speculatively scheduled dependents를 cancel하고 load가 dispatch될 때 re-issue한다. 이 cancel/redispatch 경로는 [76, 80]에 의해 이미 OOO pipeline에 존재하므로 "this is not a new pipeline change"이다 (§3.3). 이것이 traditional AP(full pipeline flush 필요)와의 결정적 차이이며, RFP가 accuracy 요구를 완화할 수 있는 근거다.
- **Coherency와 memory ordering (§3.2.1).** coherency 관련 flush도 in-flight store와 동일하게 처리된다. load가 dispatch되기 전까지 RFP prefetch는 **load의 proxy**로 취급되므로(마치 load 자신이 더 일찍 launch된 것처럼) memory consistency 강제를 위한 변경이 전혀 필요 없다. 예를 들어 memory unit이 memory barrier를 넘는 prefetch 재정렬을 자동으로 금지하며, 실제 load에 가했을 제약과 동일한 제약이 적용된다.
- **Speculation 억제 수단.** (a) PT confidence counter로 RFP-eligibility를 gating (기본 1-bit), (b) RFP packet에 L1 port 최저 우선순위를 부여해 demand load 간섭 방지, (c) load가 prefetch보다 먼저 launch되면 prefetch는 cancel·drop, (d) DTLB miss 시 drop.

### 5.6 Parameters와 storage budget

**Table 1 — Storage requirements for RFP**

| Structure | Field (bits) | Storage |
|---|---|---|
| Prefetch Table (1024–2048 entries) | Tag (16b), Confidence (3b), Utility (2b), Stride (5b), Inflight (7b), PAT Pointer (6b), Page Offset (12b) | **6.5 KB – 12 KB** (1K entries → 6.5 KB, 2K entries → 12 KB) |
| Page Address Table (64 entries) | Page Address 44b | **352 b** |
| RFP-Inflight (128 entries) | 1b | **128 b** |

추가로 §3.5는 RFP request 보관용 **64-entry RFP FIFO queue**를 가정하고, **RS entry에 1비트**를 더한다고 명시한다 (이 둘은 Table 1에 별도 항목으로 계상되지 않음).

기타 파라미터:

- PT associativity: 8-way. 기본 entry 수: 1K (§3.5, §5).
- **Confidence counter width: 기본 1 bit** (§5.5.1). 민감도는 1/2/3/4-bit로 스윕 (Fig. 17). — 단, Table 1은 Confidence를 3b로, §3.1의 논리적 엔트리는 1b로 인쇄한다. 마찬가지로 stride는 §3.1에서 8b, Table 1에서 5b다. **논문 내부가 불일치하며 양쪽 값을 그대로 옮긴다.**
- Confidence 증가 확률 (stride 반복 시): 1/16 (§3.1).
- Utility 2b, Inflight counter 7b, Tag 16b.
- PAT: 64 entries, 4-way, Page Address 44b; PT는 6-bit PAT pointer + 12-bit page offset 보유 (§3.5).
- PAT 최적화 효과: full VA를 PT에 기록하는 것 대비 **"saves almost 50% storage"**, 성능 비용 0.09% (§3.5, §5.5.4).
- OOO scheduling pipeline latency: **3 cycles** (1 wakeup + 1 select + 1 PRF read/scoreboard) 후 execution (§3.3, Stark et al. [76] 기준).
- Baseline L1 D-cache latency 5 cycles; RFP-inflight가 clear면 dependents는 load wake-up 후 5 cycles에 wake (§3.3).
- RFP-inflight 세팅 시점: Addr calculation stage 다음 첫 L1 lookup cycle = L1 hit 가정 시 RFP 완료 3 cycles 전 (§3.3).
- VP 비교 실험의 value misprediction flush penalty: **20 cycles** [7] (§4).
- Context (path-based) prefetcher를 평가할 때 entry 수: 1K (stride prefetcher와 동일, §5).
- §5.6: "RFP has a similar area overhead as traditional value and address predictors."

### 5.7 Evaluation setup과 results

**Simulator / methodology (§4).** 사내(in-house) execution-driven cycle-accurate simulator로 dynamically scheduled x86 core를 모델링. 5-wide OOO, 4 GHz, Intel Tiger Lake [22]와 유사한 파라미터 = **Baseline**. 성능은 IPC로 측정하고, 평균 speedup은 **geometric mean**. 카테고리별·전체 결과를 모두 제시. 선행 연구와의 공정 비교를 위해 "we assume very large storage for all prior works". §5의 기본 결과는 stride prefetcher와 context prefetcher 모두 1K entries이고 **PT에 full 64-bit virtual address를 기록**한다고 가정한다(area 최적화는 §5.5에서 별도로 평가).

**Table 2 — Core parameters**

| | |
|---|---|
| **Front End** | 5 wide fetch/decode, TAGE/ITTAGE branch predictors [66], 32KB 8-way L1 instruction cache, 8-way uop-cache with 2.3K micro-op entries, loop-stream-detector [79], 140-entry instruction decode queue, 5 wide rename into OOO with macro/micro fusion |
| **Execution** | 352 ROB entries, 128 Load Queue entries, 72 Store Queue entries, 125 Issue Queue entries. "10 Execution units (ports) including 2 load ports, 2 store address ports, **2 store-data ports**, 4 ALU ports, 3 FP/AVX ports, 2 branch ports." (열거된 port 수의 합은 15로 "10"과 맞지 않으나 표에 인쇄된 그대로다.) 8 wide retire, full bypass support, aggressive memory disambiguation predictor, out-of-order load scheduling to L1 |
| **Caches** | 48 KB 12-way L1 data cache, latency **5 cycles**; 1.25 MB 20-way private L2, round-trip **15 cycles**; 3 MB 12-way shared LLC, data round-trip **40 cycles**. L2/LLC로 aggressive multi-stream prefetching, L1에 PC-based stride prefetcher |
| **Memory** | DDR4-3200 2 channels, 2 ranks/channel, 8 banks/rank, channel당 64-bit data bus, bank당 2 KB row buffer, 22-22-22-56 (tCAS-tRCD-tRP-tRAS) |

**Workloads (Table 3).** "65 diverse, single-threaded applications" — SPEC17 전체 + SPEC06 + 잘 알려진 Cloud/Client 벤치마크. 모두 x86(AVX2 최적화)으로 컴파일되고 SimPoint [69] 기반으로 대표 구간 선정.

| Category | Benchmarks (표에 인쇄된 표기 그대로) |
|---|---|
| ISPEC06 | perlbench, bzip2, gcc, mcf, h264ref, gobmk, hmmer, sjeng, libquantum, omnetpp, astar |
| FSPEC06 | bwaves, gamess, milc, zeusmp, soplex, calculix, tonto, wrf, sphinx3, gromacs, cactusADM, leslie3D, namd, deall |
| ISPEC17 | leela, xz, gcc, mcf, xalanc, exchange2, omnetpp, perlbench, deepsjeng, x264 |
| FSPEC17 | nab, cam4, roms, cactubssn, bwaves, lbm, fotonik3d, namd, parest, povray, wrf, blender, imagick |
| Cloud | lammps [44], tpce [21], spark [81], bigbench [28], specjbb [18], specjenterprise [17], hadoop [3], tpcc [20] |
| Client | sysmark [8], geekbench [43] |

(Table 3에 실제로 나열된 이름은 58개로, 본문의 "65 diverse applications"와 개수가 맞지 않는다. 논문은 두 값을 그대로 인쇄한다.)

**Baseline-2x (§5.1).** Tiger Lake를 상향한 futuristic core — **10-wide, 모든 execution resource 2배, L1 bandwidth 증가**.

---

#### Headline 결과

| 결과 | 수치 | 기준(baseline) | 출처 |
|---|---|---|---|
| **RFP 성능/coverage** | **3.1% speedup (geomean), 43.4% coverage** | Tiger Lake-like Baseline | Fig. 10, §5.1, Abstract |
| oracle 대비 달성률 | 3.1%는 Fig. 1의 L1→RF 9% 헤드룸의 **약 34%** | oracle(100% coverage) 9% | §5.1 |
| **Baseline-2x** | **5.7% speedup, 53.7% coverage** | 각자의 baseline(Baseline-2x) | Fig. 12, §5.1 |
| **L1 cache port 수를 2배로 늘리고 그 절반을 RFP 전용으로 배정한 구성 (§5.2.1)** | **4.0% speedup, 57.4% coverage** (executed 64.4%). shared port 대비 **16.1% 더 많은 prefetch 실행** | shared port RFP (3.1% / 43.4% / executed 48.3%) | Fig. 14, §5.2.1 |
| **VP+RFP 결합** | **4.15% speedup, 54.6% coverage** | Baseline | Fig. 15, §5.3 |

**Coverage 정의 (§5.1):** "the fraction of all loads for which prefetches were useful and the load correctly obtained data through a prefetch."

**카테고리별 speedup / coverage (Fig. 10)**

| | Client | Cloud | FSPEC06 | FSPEC17 | ISPEC06 | ISPEC17 | Mean |
|---|---|---|---|---|---|---|---|
| Speedup | 4.7% | 3.5% | 3.0% | 2.3% | 2.8% | 3.2% | **3.1%** |
| Coverage | 68.0% | 41.4% | 32.3% | 57.3% | 36.0% | 38.7% | **43.4%** |

**Prefetch 깔때기 — injected / executed / useful (Fig. 13, §5.2)**

| | Client | Cloud | FSPEC06 | FSPEC17 | ISPEC06 | ISPEC17 | Mean |
|---|---|---|---|---|---|---|---|
| Prefetches Injected | 85% | 62% | 71% | 84% | 71% | 67% | **72%** |
| Prefetches Executed | 71% | 45% | 35% | 64% | 39% | 48% | **48%** |
| Prefetches Useful (Coverage) | 68% | 41% | 32% | 57% | 36% | 39% | **43%** |

- 전체 load의 **24%**는 packet이 injected되었으나 prefetch가 실행되지 않고 drop된다. 대부분은 **L1 bandwidth 제약**으로 prefetch가 지연되어 load가 prefetch보다 먼저 launch되었기 때문 (§5.2, §3.2).
- 전체 load의 약 **5%**가 incorrect prefetch를 겪어 re-issue가 필요했다. 이 잘못된 주소의 prefetch는 **해당 load의 latency에는 영향을 주지 않지만 추가 L1 bandwidth를 소비**한다 (§5.2).

**Timeliness (§5.2.2).** 전체 load의 **34.2%**에서 prefetch가 대응 load가 dispatch되기도 전에 완료되어 latency를 **완전히** 숨겼다(dependents 입장에서 load가 1 cycle에 실행된 것처럼 보임). 나머지 **9.2% (= 43.4% − 34.2%)**는 prefetch가 지연되어 latency를 **부분적으로만** 절약했다.

**Value Prediction 비교 (Fig. 15, §5.3).** Composite VP는 Sheikh et al. [68]의 state-of-the-art predictor로, EVES [65]와 DLVP [67]를 지능적으로 fusion한 것이다.

| 구성 | Speedup | Coverage |
|---|---|---|
| Composite VP | 2.20% | 34.2% |
| EPP [2] + Composite VP | 2.05% | 34.2% |
| **RFP** | **3.08%** | **43.4%** |
| **VP + RFP** | **4.15%** | **54.6%** |

- EPP는 L1 access와 bandwidth를 줄여 주지만 Composite VP 대비 성능 이득을 주지 못하며, 오히려 SSBF [61] false-positive가 유발하는 retirement 시점 추가 load re-execution 때문에 **약간 더 낮다**(2.05% vs 2.20%). Alves et al. [2] 자신도 EPP가 DLVP [67]와 비슷한 성능을 얻었다고 보고했다.
- **VP+RFP의 결합 규칙(중요):** 이 구성은 EVES [65]와 RFP를 합친 것으로, **"an RFP is performed for a given load only if the load is not value predictable."** 즉 value-predictable하지 않은 load에 대해서만 RFP를 수행한다.

**Address Prediction 비교 (Fig. 16, §5.4).** DLVP [67]는 Fetch 단계에서 load 주소를 예측하고 L1을 probe해 그 데이터로 value prediction을 한다. address-predictable한 load 비율 자체는 RFP와 비슷하지만, 실제로 value predict되는 비율은 **11%**에 불과하다.

| 단계 | 남는 load 비율 |
|---|---|
| APHC (AP High Confidence) — 높은 confidence threshold 적용 후 | 49% |
| APHC + no-FWD — store-forwarding 가능성 차단 predictor 적용 후 | 45% |
| DLVP Probes launched — port 가용성 제약 후 | 22% |
| DLVP Probes Success — 제때 완료된 probe (L1이 1 cycle이 아니라 5 cycles이고, uop-cache가 window를 줄이므로) | **11%** |

반면 RFP는 "converts almost 43% of loads (**3.8X of DLVP coverage**) into prefetches" — (1) misprediction 시 costly pipeline flush를 포기해 accuracy를 완화했고, (2) 추가 L1 bandwidth가 필요 없으며, (3) in-flight store가 있어도 계속 공격적으로 prefetch하기 때문이다.

**Confidence counter width 민감도 (Fig. 17, §5.5.1)**

| Width | Speedup | Injected | Executed | Coverage |
|---|---|---|---|---|
| 4-bit | 2.4% | 57.5% | 38.4% | 37.7% |
| 3-bit | 2.7% | 61.4% | 41.3% | 39.9% |
| 2-bit | 2.9% | 66.2% | 44.3% | 41.6% |
| **1-bit (default)** | **3.1%** | **72.4%** | **48.3%** | **43.4%** |

widening은 accuracy를 높이지만 coverage를 떨어뜨린다: **1-bit는 5% incorrect prefetch, 4-bit는 0.7%**. 그럼에도 RFP misprediction이 비싸지 않으므로 1-bit로 충분하며, "marginally reduced accuracy"의 대가로 훨씬 높은 coverage를 얻는다.

**Prefetch Table entry 수 민감도 (Fig. 18, §5.5.1)**

| Entries | Speedup | Injected | Executed | Coverage |
|---|---|---|---|---|
| **1K (default)** | **3.1%** | 72.4% | 48.3% | 43.4% |
| 2K | 3.2% | 74.7% | 50.1% | 45.0% |
| 4K | 3.3% | 76.3% | 51.3% | 46.0% |
| 8K | 3.4% | 77.4% | 52.2% | 46.8% |
| 16K | 3.5% | 78.1% | 52.7% | 47.3% |

"increasing entries in PT from 1K to 16K leads to minor improvements in performance and further scaling of entries shows no visible improvement." — 1K를 기본값으로 고른 근거.

**L1 latency 민감도 (§5.5.2).** "We observe that RFP's performance increases by **0.5% to 3.6%** when the L1 data cache latency is increased from **5-cycles to 6-cycles**." (baseline RFP 이득이 3.1%이므로, 자연스러운 독해는 "0.5%p 증가하여 3.6%가 된다"이다. 논문은 이 한 문장 외에 부연을 두지 않는다.) → **L1 latency가 커질수록 RFP의 이득이 커진다**는 것이 Baseline-2x와 함께 제시되는 scaling 논거다.

**Context prefetcher (§5.5.3).** DLVP [67]의 path-based address predictor(PAP)에 기반한 context prefetcher는 stride prefetcher 대비 **0.3%의 추가 speedup**만을 준다. 따라서 논문은 "to reduce storage, the stride prefetcher may be enough"이라고 결론짓는다.

**Page Address Table (§5.5.4).** load가 다른 page로 이동하면 기록된 Page Address가 잘못된 virtual address를 계산해 RFP misprediction을 유발하고, PAT pointer가 stale해져 RFP가 새 PAT entry를 다시 학습해야 한다. 논문의 분석으로는 이런 경우가 드물어, PAT는 storage를 크게 줄이면서 **0.09%의 negligible한 성능 하락**만 낳는다.

**Pipeline simplifications (§5.5.5).** TLB miss 시 RFP request를 drop하는 것은 **negligible한 성능 영향**을 갖는다. L1 miss에 대해서도 RFP를 수행하는 것은 **0.02%의 매우 작은 성능 upside**만을 준다.

**Per-workload (Fig. 11, §5.1).**
- 좌측(최저 coverage·최소 IPC 이득): **spec06_tonto, spec06_gamess, spec06_milc**.
- 높은 coverage에도 IPC 이득이 negligible한 경우: **spec17_wrf**. 일반적으로 FSPEC17 전체가 RFP에 덜 민감한데, "these applications are bottlenecked by AVX ports/FMA latency and not just by the L1/memory latency"이기 때문이다.
- 40% 미만 coverage에도 **4% 이상 IPC 이득**을 얻는 워크로드: **lammps, spec06_namd, spec17_xalancbmk, hadoop**. → "some prefetches are more critical for performance and that not all prefetches have a high impact on performance."
- RFP prefetch가 demand load보다 낮은 우선순위를 갖기 때문에 **baseline 성능이 RFP 존재로 저해되지 않는다**는 점도 Fig. 11이 보여 준다.

**Power analysis (§5.6).**
- RFP의 area overhead는 traditional value/address predictor와 유사하다.
- 별도 PAT를 쓰면 storage 요구가 크게 줄어 전체 power가 절약된다.
- **RFP가 맞으면 demand load가 L1 access를 완전히 skip하므로, validation을 위한 추가 L1 bandwidth와 power가 전혀 소비되지 않는다** (AP 계열과의 차이).
- incorrect prefetch가 5%뿐이며, 이는 L1 re-lookup으로 L1 cache power를 조금 늘리지만 "still drastically cheaper than the costly pipeline flushes in value and address predictors."

### 5.8 저자가 밝힌 limitation과 self-positioning

**저자가 인정한 한계**

1. **L1 bandwidth가 timeliness의 주 제약 (§3.4).** RFP는 load port를 추가하지 않고 빈 port를 기회주의적으로 쓴다("Load ports are known to be costly for timing and area [55]"). 그 결과 전체 load의 24%에서 packet이 injected되고도 prefetch가 drop되며(§5.2), port를 전용으로 주면 3.1% → 4.0%로 오른다(§5.2.1).
2. **Allocation 시점에 이미 ready인 load에는 약하다 (§3.4).** "RFP is only issued after the original load has been allocated. If the original load request is ready to dispatch at allocation, RFP has a shorter window to launch in time. For such loads, RFP will have lower effectiveness."
3. **Incorrect prefetch 5% (§5.2, §5.6).** load latency에는 영향이 없지만 추가 L1 bandwidth와 약간의 L1 cache power를 쓴다.
4. **의도적 단순화 (§3.2.2, §5.5.5).** DTLB miss 시 RFP drop (TLB miss는 latency가 길어 run-ahead가 남지 않으므로), L1 miss에 대한 RFP 미수행. 각각 negligible / 0.02% 상실.
5. **PAT의 실패 케이스 (§3.5, §5.5.4).** load가 다른 page로 넘어가면 잘못된 VA를 계산하고, PAT entry가 evict되면 pointer가 stale해져 misprediction 후 재학습이 필요하다 — 드물지만 0.09%의 비용.
6. **Coverage가 IPC로 번역되지 않는 경우 (§5.1).** spec17_wrf 및 FSPEC17 전반은 AVX ports/FMA latency에 병목이 있어 높은 coverage에도 이득이 없다.
7. **Future work (§5.1).** "We leave the research on targeted prefetching for specific load instructions for future work." — Focused Value Prediction [7]과 CATCH [53]이 criticality 학습으로 후보를 좁힌 전례를 들며, 희소한 L1 bandwidth를 감안하면 criticality 기반 타게팅이 다음 단계라고 시사한다.
8. **Future work (§6).** "Intelligently combining RFP with criticality-based solutions can further reduce end-to-end pipeline latency for critical loads and their load slices, and result in even higher performance."

**Related work에서의 self-positioning (§2, §6, 모두 논문 자신의 주장)**

- **vs. Value Prediction (§2.1, §5.3, §6).** VP [7, 46, 58, 59, 63, 65]는 dependency를 투기적으로 끊지만 "offers limited coverage because of the high accuracy needed to avoid costly flushes on mis-speculations". RFP는 flush를 요구하지 않아 low-confidence prediction을 허용하고, VP가 겨냥하지 않는 load의 상당 부분을 커버한다. 다만 VP와 달리 L1 bandwidth와 pipeline latency에 제약된다. §6은 VP 구현 비용 계열 연구를 열거한다 — EOLE [57]은 validation용 register file read port 추가 없이 VP를 구현했고, FVP [7]는 critical load만 겨냥해 value predictor 크기를 관리했으며, Perais [56]는 register renaming 최적화를 활용한 targeted VP를 제안했다. 논문은 "Unlike VP, RFP seamlessly blends into the traditional OOO pipeline and does not add significant hardware or pipeline changes"라고 자리매김한다. 그리고 §5.3에서 둘이 synergistic함을 보인다.
- **vs. Load Address Prediction (§2.2, §5.4, §6).** DLVP [67], Composite [68], EPP [2]는 Fetch에서 주소를 예측해 데이터를 미리 읽고 그것으로 load를 value predict한다. 논문이 드는 문제점: (a) "these prior works require two cache accesses - one for reading data for VP and a subsequent access for validation", (b) 잘못된 speculation에서 costly flush가 필요하고 그래서 coverage가 제한되며, (c) 큰 OOO window에서 in-flight store와 충돌할 확률이 커져 memory aliasing predictor로 "severely throttled"되어야 하고, (d) uop-cache [40]와 loop stream detector [79] 때문에 fetch 조기 발사의 이점이 사라졌으며, (e) DLVP는 L1 latency를 1 cycle로 가정했으나 Tiger Lake [22]·AMD Ryzen [78]은 5 cycles이고 L1 read port도 매우 적다. EPP는 "introduces large hardware complexity while offering little performance benefit over previous load address predictors"라고 평가되며, multi-ported RF tag와 대형 multi-ported Store Sequence Bloom Filter [61]를 쓰는데 SSBF의 false positive가 일부 load를 retirement에서 재실행시킨다. RFP는 반대로 "does not take additional L1 bandwidth"라고 대비된다.
- **vs. Memory Prefetching (§2.3).** 메모리 prefetcher와 helper thread/runahead [15, 24, 38, 48, 82] 계열은 DRAM latency를 겨냥하며 L1→RF 기회를 "traditionally overlooked"했다. 또한 RF는 tagged가 아니어서 prefetched data를 load에 묶을 identifier가 필요하고, uop-cache·loop-stream-detector 때문에 slack이 매우 작아 "strict timeliness guarantees"가 요구된다는 점에서 문제 자체가 구별된다.
- **vs. Caching / L0 (§2.4).** Austin & Sohi [4], Eickemeyer & Vassiliadis [25]의 early address calculation은 단순 5-stage pipeline용이라 현대의 깊은 파이프라인에 적용되지 않는다. L0 cache는 §2.4가 열거한 이유들(L1 latency에 address calculation·translation·rotator가 포함되어 절약 폭이 작음, translation 우회 시 consistency/coherency 관리가 복잡, rotator를 피하려면 정확한 load 크기여야 하고 vector/non-vector load를 함께 다룰 수 없음, older store check를 대체할 predictor와 flush 메커니즘이 필요)로 "very complex to build".
- **vs. Criticality 기반 연구 (§6).** CRISP [47]는 criticality 기반 scheduling으로 delinquent load와 그 load slice를 가능한 한 일찍 실행하고, criticality 기반 instruction scheduling policy들 [1, 39, 47, 64]은 critical instruction의 scheduling pipeline latency를 줄인다. 논문은 "RFP is complementary to these works and hides the L1 cache access latency"라고 위치시키며, 둘을 지능적으로 결합하는 것을 future work로 남긴다.

### 5.9 Section map과 인용 가능한 문장

**§ 번호 → 주제**

| § | 주제 |
|---|---|
| Abstract | memory wall은 monolithic이 아님; L1 latency가 memory보다 40X 낮음; 43.4% load prefetch, 65 workload에서 3.1%, up-scaled core에서 5.7%, VP와 합쳐 4.1% |
| §1 Introduction | 계층별 oracle 헤드룸(Fig. 1), load 분포(Fig. 2), critical-path 예제(Fig. 3), 네 가지 contribution |
| §2 Background | 선행 연구가 L1 latency를 완화하지 못하는 이유 + 전형적 OOO scheduling pipeline |
| §2.1 | Value Prediction |
| §2.2 | Load Address Prediction — DLVP, Composite predictor, EPP, SSBF |
| §2.3 | Memory Prefetching — helper thread/runahead; RF prefetching이 다른 이유(무tag, prfid 필요, strict timeliness) |
| §2.4 | Caching — L0 cache가 복잡한 이유(translation 우회, rotator, store-forwarding check) |
| §2.5 | OOO Pipeline — 파이프라인 단계(Fig. 4), Yoaz et al. hit-miss predictor와 speculative dependent scheduling |
| §3 | Register File Prefetch — 세 설계 축(Timeliness / Bandwidth / Accuracy), 전체 구조도(Fig. 5) |
| §3.1 | RFP Prefetcher — PT 구성, retirement 기반 stride training, confidence/utility/inflight counter, context predictor 변형 |
| §3.2 | Launching RFP — eligible load 표시, {VA, prfid} packet, Rename 직후 발사, L1 port arbitration과 FIFO 우선순위 |
| §3.2.1 | Handling in-flight stores and Coherency — youngest-first older-store scan, Memory Disambiguation, flush 규칙, memory barrier |
| §3.2.2 | Pipeline Simplifications — L1 miss는 하위 계층 진행, DTLB miss는 drop |
| §3.3 | RFP Pipeline — 전형적 wakeup/select/register-read(Fig. 6), Dependence Vector, ADD-on-ADD / ADD-on-LOAD 스케줄(Fig. 7, 8), RFP-inflight 타이밍과 RFP-assisted schedule(Fig. 9), late RF allocation 파이프라인 변형 |
| §3.4 | RFP Timeliness — load port 미추가, 빈 port 기회주의적 사용 |
| §3.5 | Storage Calculations — 8-way PT, 64-entry 4-way PAT, 64-entry RFP FIFO, Table 1 |
| §4 | Evaluation Methodology — simulator, core 파라미터(Table 2), 65 applications(Table 3), IPC / geometric mean |
| §5 | Simulation Results — 로드맵 |
| §5.1 | Performance and Coverage — 카테고리별(Fig. 10), 워크로드별 IPC vs coverage(Fig. 11), Baseline-2x(Fig. 12) |
| §5.2 | Timeliness and Effectiveness — injected / executed / useful(Fig. 13) |
| §5.2.1 | L1 cache bandwidth의 영향 — dedicated vs shared port(Fig. 14) |
| §5.2.2 | Effectiveness — latency 완전 은닉 vs 부분 은닉 |
| §5.3 | Comparison with Value Prediction — Composite VP, EPP+VP, RFP, VP+RFP(Fig. 15) |
| §5.4 | Comparison with Address Prediction — DLVP coverage 깔때기(Fig. 16) |
| §5.5 | RFP Sensitivity |
| §5.5.1 | Confidence counter width(Fig. 17) 및 PT entry 수(Fig. 18) 민감도 |
| §5.5.2 | L1 cache latency의 영향 — 5 → 6 cycles |
| §5.5.3 | Context Prefetcher의 영향 — path-based address predictor |
| §5.5.4 | Page Address Table의 영향 — 0.09% 하락 |
| §5.5.5 | Pipeline simplification의 영향 — TLB-miss drop, L1 miss RFP |
| §5.6 | Power Analysis — storage 절약, L1 access 생략, 5% incorrect prefetch의 비용 |
| §6 | Other Related Work — VP 구현(EOLE, FVP, Perais), load address predictor, CRISP와 criticality 기반 scheduling |
| §7 | Summary |
| References | [1]–[83] |

**Verbatim 인용문**

1. §1 — "Evidently, the figurative memory wall is not just a monolithic wall between the LLC and DRAM but comprises multiple latency walls between the Register File, the on-die caches and the DRAM."
2. §1 — "Figure 1 also shows that mitigating level-1 (L1) data cache latency has a 9% performance headroom despite having 40X lower latency (5 cycles in Tiger Lake [22]) than the main memory."
3. §2.3 — "unlike caches, Register Files (RF) are not tagged and therefore will need an identifier in the RF to associate the prefetched data with the incoming load request."
4. §3 (Timeliness) — "Register Files are not tagged (in contrast to caches) which means we need a location in the physical register file to store the prefetched data. We hence perform RFP prefetch after Rename stage where we can reuse the register file entry assigned to the load."
5. §3 (Bandwidth) — "To remedy this, we design RFP to guarantee that if the prefetched address was correct, the data in the Register File is up-to-date with the caches and inflight stores and needs no validation which means RFP is very frugal with L1 bandwidth."
6. §3 (Accuracy) — "We should note that RFP's incorrect prefetches are less costly as compared to traditional AP which need a full pipeline flush when the prediction is incorrect. Hence, RFP can prefetch more aggressively, but to reduce interference with demand loads (non-prefetched), RFP prefetch should be given lower priority for L1 cache access."
7. §3.2.1 — "Effectively, till the load is dispatched, RFP prefetch is treated as a proxy for the load (as if the load itself was launched earlier), and hence no changes are needed to enforce memory consistency."
8. §3.3 — "Note that RFP-inflight is set 3 cycles before the RFP completes (assuming the RFP hits in L1) and the scheduling pipeline latency is also 3 cycles. This is not a coincidence."
9. §3.3 — "We should recall that cancellation and redispatch of speculatively scheduled dependents already happen in the OOO pipeline [76, 80], so this is not a new pipeline change."
10. §5.4 — "whereas RFP converts almost 43% of loads (3.8X of DLVP coverage) into prefetches because of its unique design"
11. §7 — "We hence proposed RFP (Register File Prefetch) that successfully prefetches 43.4% of loads to the Register File, reducing effective L1 latency and providing a significant performance gain of 3.1%."

---

## 6. TEA — "Timely, Efficient, and Accurate Branch Precomputation" (MICRO 2024)

> Aniket Deshmukh · Lingzhe(Chester) Cai · Yale N. Patt · University of Texas at Austin (HPS Research Group) · MICRO 2024, pp. 480–492, DOI 10.1109/MICRO61859.2024.00043
> 원문: [PDF](</home/lee/scarab/reference/[2024, MICRO] Timely_Efficient_and_Accurate_Branch_Precomputation.pdf>)

**Problem.** Out-of-order 코어는 backend에 유효한 instruction을 공급하기 위해 고정확도 branch predictor에 의존하지만, TAGE-SC-L [23]이나 Perceptron [15] 같은 최신 predictor가 상당한 하드웨어 투자를 하고도 개선하지 못하는 hard-to-predict(H2P) branch가 misprediction의 큰 몫을 차지한다 (Abstract, §I). H2P branch는 data-dependent branch [12], [21]과 런타임에 식별하기 어려운 복잡한 control flow 패턴을 가진 branch [29]로 구성된다 (§I). Precomputation은 H2P branch로 이어지는 dependence chain을 별도의 speculative thread로 미리 실행해 Fetch 시점에 predictor를 override하려는 접근인데, 논문은 기존 precomputation 연구가 동시에 **late**, **inaccurate**, **inefficient** 하다고 프레이밍한다 (§I). *Late*: "결과가 branch가 fetch되기 전에 도착해야 한다"는 timeliness 제약을 만족시키기 어려워, 선행 연구는 timeliness를 높이려고 fixed control flow 속성을 가진 특정 branch만 대상으로 삼고 그 결과 많은 H2P branch가 커버되지 않는다. *Inaccurate*: precomputation이 맞은 prediction까지 override해 오히려 misprediction을 추가로 만들며, 정확도를 지키려는 연구는 단순한 dependence chain만 대상으로 해 다시 coverage가 줄고, compiler 기반의 heavy-weight helper thread는 정확하지만 너무 느려 timeliness를 희생한다. *Inefficient*: timely하려면, 그리고 late하거나 틀린 precomputation에 on-core 자원을 낭비하지 않으려면 별도의 core나 execution engine이 필요하다. TEA의 프레이밍은, branch가 fetch된 **후** 실행되기 **전**에 도착하는 "late" 결과라도 predictor override 대신 **early misprediction flush**를 발행하는 데 쓰면 여전히 instruction stream을 교정할 수 있고, 그러면 제약이 충분히 완화되어 복잡한 control flow를 가로지르는 긴 dependence chain을 추적하고 timeliness 대신 coverage와 accuracy를 최적화할 수 있다는 것이다 (§I, §III).

### 6.1 Motivation과 characterization

| 관찰 | 수치 | 출처 |
|---|---|---|
| 최종 성능 | aggressive 8-wide baseline OoO core 대비 **geomean 10.1%** 성능 향상 | Abstract, §I, §V-B, Fig.5 |
| Precomputation accuracy | 전체 application 평균 **99.3%**; 나머지 모든 feature-ablation 구성은 TEA보다 성능이 3~5% 나쁨 | §I, §V-E, Fig.10-(a) |
| Misprediction coverage | **76%**; 제안된 thread 구성 feature를 전부 제거하면 **39%**로 하락 (어느 단일 feature 제거보다 훨씬 큰 낙폭) | §I, §V-E, Fig.10-(b) |
| TEA thread branch의 오계산 비율 | **0.7% 미만** | §IV-E |
| Misprediction penalty 절감 | TEA thread branch의 **~76%** 가 최소 1 cycle 이상의 misprediction penalty를 절약 | §IV-E |
| 중간 non-H2P branch 처리 근거 | TAGE는 많은 H2P branch조차 **~80%** 의 상당히 높은 정확도로 예측하므로, TEA thread가 그 branch를 지나 계속 fetch하며 다음 H2P branch의 precomputation을 더 일찍 시작할 수 있음 | §III-B |
| Fetch 처리량 격차 (Fig.1 예제) | basic block C가 나올 때마다 TEA thread는 main thread 대비 **2 cycle** 씩 앞섬 — main thread는 C의 모든 instruction을 I-cache에서 읽는 데 cycle 2, 3, 4가 필요한 반면 TEA thread는 Block Cache에서 C의 basic block segment를 1 cycle에 읽음. 그 결과 cycle 4에 TEA thread가 아무것도 fetch하지 않아도 branch A2의 세 번째 instance를 cycle 6에, 즉 main thread보다 **4 cycle** 앞서 시작할 수 있음 | §III-B, Fig.1-(c) |
| Chain 길이 확장 (Fig.2, *leela* 패턴) | Fill Buffer 한 번의 pass로 추적 가능한 chain은 D3 D4 D5로 짧지만, 이미 표시된 dependence chain instruction에서 walk를 재개시하면 B3 C4 D3 D4 D5 → A0 A1 B3 C4 D3 D4 D5로 자라며, Fill Buffer가 512 instruction만 담아도 chain은 수천 instruction에 걸칠 수 있음 | §III-C, Fig.2 |
| 이득의 출처가 prefetch가 아님 | TEA thread의 early resolution을 끄고 dependence chain load의 prefetching 부수효과만 남기면 성능 이득은 **1.2%** 에 불과 (*xalanbmk* 만 예외로, 이득 대부분이 data prefetching에서 옴) | §V-B |
| Block Cache 용량이 병목인 사례 | Block Cache가 추적하는 basic block segment 수를 늘리면 *deepsjeng*, *omnettpp* 성능이 **5%** 개선 | §V-B |
| 단순 폭 확장의 무의미함 | 진짜 16-wide OoO core는 면적을 **~10%** 더 쓰면서 성능은 **2.8%** 만 제공 — frontend 폭을 predictor bandwidth 확장 없이 키우면 cycle당 taken branch 하나를 넘겨 꾸준히 fetch할 방법이 없기 때문 | §IV-H |
| Dynamic instruction footprint | baseline OoO core 대비 **+31.9%**; Slipstream [26]은 85%, Branch Runahead [21]는 34% | §IV-H, Table III |

Fig.7은 TEA thread가 커버한 misprediction의 비율을 **Early / Late / Incorrect / No dependence chain** 네 범주로 분해한다. *deepsjeng*, *omnettpp* 의 낮은 coverage는 "No dependence chain"(Block Cache 용량 부족으로 chain이 존재하지 않음) 때문이며, 이것이 위의 5% 개선치가 나오는 이유다 (§V-B, Fig.7). "Late"는 *gcc*, *mcf*, *omnettpp*, *xz* 에서 나타나는데, 대부분 TEA thread와 main thread가 **같은 cycle에** 실행을 끝내는 경우이고, TEA thread가 main thread보다 **늦게** H2P branch를 끝내는 경우는 0.1% 미만이다 (§V-B).

Table III의 per-benchmark 추가 dynamic instruction 비율 (baseline 대비):

| perlbench | gcc | mcf | omnettpp | xalanbmk | x264 | deepsjeng | leela | exchange2 |
|---|---|---|---|---|---|---|---|---|
| 17.63% | 25.51% | 40.01% | 23.07% | 22.21% | 14.39% | 32.15% | 44.62% | 27.67% |

| xz | pop2 | nab | bfs | sssp | pr | cc | bc | tc |
|---|---|---|---|---|---|---|---|---|
| 51.03% | 16.59% | 10.25% | 43.49% | 56.79% | -1.27% | 45.96% | 48.76% | 40.72% |

### 6.2 Key insights

1. **Timeliness 제약의 완화가 모든 것을 가능케 한다.** precomputation 결과로 predictor를 override하는 대신 early misprediction flush를 발행하면, 결과는 "해당 main thread branch가 fetch되기 전"이 아니라 "그 branch가 **실행을 끝내기** 전"에만 도착하면 된다 (Abstract, §I, §III). TEA thread가 기존 flush 메커니즘을 재사용하므로 out-of-order 및 nested branch resolution까지 지원된다 (§III).
2. **제약이 완화되었으니 coverage와 accuracy를 최적화하는 것이 옳다.** 더 길고 정확한, 여러 control flow에 걸쳐 유효한 chain을 추적하면 precomputation을 instruction stream에서 훨씬 일찍 시작할 수 있어 timeliness도 함께 좋아지고, 높은 accuracy는 coverage 개선으로 이어진다 (§III, §III-C·D·E). 결과적으로 timeliness에 집중한 state of the art보다 성능이 좋다 (§I, §V-C, §VI).
3. **TEA thread의 control flow는 main branch predictor(TAGE)가 몰아준다.** 그래서 임의의 control flow를 따라갈 수 있을 뿐 아니라, 중간 non-H2P branch를 정확히 예측하므로 그 branch들의 dependence chain을 thread에 포함시킬 필요가 없어 thread가 가벼워진다 (§III-B). Fig.3의 예에서 basic block A 끝의 branch(line 4)는 TAGE가 정확히 예측하므로 H2P branch의 chain에 영향을 주더라도 TEA thread에 포함하지 않는다 (§III-B).
4. **basic block segment 표현(Block Cache)은 어떤 control flow도 재구성할 수 있다.** decoupled BP가 만든 fetch address로 서로 다른 basic block segment를 조합하기 때문이며, 특정 control flow 패턴에서만 동작하는 방식 [7], [13], [21]보다 훨씬 나은 misprediction coverage를 준다 (§III-B).
5. **이미 표시된 chain instruction에서 Backward Dataflow Walk를 재개시하면 chain 길이가 Fill Buffer 용량에 묶이지 않는다.** TEA thread로 실행된 instruction도 chain-bit이 켜진 채 Fill Buffer에 들어오므로 다음 walk의 initiation point가 되고, 반복될수록 chain이 위로 자란다 (§III-C, Fig.2).
6. **precomputation thread에 instruction을 더 넣는 것이 오히려 timeliness를 개선할 수 있다** — 추가된 instruction이 chain의 길이를 늘려 precomputation을 더 일찍 시작하게 만든다면 그렇다 (§III-C).
7. **서로 다른 control flow에서 모은 per-basic-block bit-mask를 bit-wise OR 하면 그 모든 경로에서 precomputation이 정확해진다.** 어느 한 경로에서는 불필요한 instruction이 몇 개 추가되는 비용이 있지만, accuracy와 coverage를 극대화하는 쪽이 chain 길이를 최소화하는 쪽보다 성능에 거의 항상 유리하다 (§III-E, §V-E).
8. **TEA thread가 정확하고 timely하기 때문에 on-core 실행이 낭비가 거의 없다.** Issue 우선권과 backend의 static partition만 주면 dedicated execution engine에 근접하는 성능을 훨씬 낮은 비용으로 얻는다 (§III, §IV-E, §V-D). H2P branch misprediction은 MPKI가 상대적으로 낮은 application에서도 거의 항상 critical path 위에 있으므로, main thread instruction 일부의 실행을 늦추더라도 TEA를 우선하는 것이 이득이다 (§IV-E).
9. **branch predictor가 부여하는 synchronized timestamp가 early flush 하드웨어 문제를 없앤다.** TEA branch는 자기 main thread counterpart와 같은 timestamp를 상속받으므로, 기존 flush 하드웨어만으로 그 counterpart보다 younger한 모든 instruction을 flush할 수 있고, 별도 queue 없이 direction과 target 양쪽 precomputation을 지원한다 (§II-C, §III-B, §IV-F).

### 6.3 Hardware structures

Fig.4가 전체 배치를 보여준다: **H2P Branch Table**과 **Fill Buffer**는 backend에 위치해 Retire에서 채워지며 H2P branch 식별과 dependence chain 추적을 담당하고, **Block Cache + TEA Fetch**는 decoupled BP가 shadow Fetch Queue에 넣은 fetch address로 TEA thread를 구성하며, **Shadow RAT**을 쓰는 TEA Rename을 거쳐 두 thread가 **Issue에서 합류**한다 (§IV-A, Fig.4).

| 구조 | 목적 | 구성 (entry 수 / associativity / indexing / 파이프라인 위치) | per-entry 필드와 비트폭 |
|---|---|---|---|
| **H2P Branch Table** | 자주 mispredict하는 branch(direct·indirect, direction·target misprediction 모두)를 식별해 dependence chain 추적 대상으로 삼음 | 256 entry, 8-way set associative, branch PC로 index. backend에 위치하고 Retire에서 학습 (Fig.4). Table II: 256-entry cache, 0.2KB, **1-cycle access** | entry당 3-bit saturating counter 1개. misprediction 시 entry 생성하며 counter를 1로 초기화, 같은 branch가 또 mispredict하면 증가. counter > 1 이면 H2P. 모든 counter는 50k instruction마다 1씩 감소, counter 0인 entry가 replacement 우선순위 (§IV-B) |
| **Fill Buffer** | retire된 instruction을 program order로 보관해 Backward Dataflow Walk가 H2P branch의 chain instruction을 표시할 수 있게 함 | 512-entry queue, 8KB, **single access port** (Table II). backend, Retire에서 채워짐 (Fig.4). x86 ISA를 쓰므로 micro-op(uop) 단위로 동작 (§IV-C 각주 2: fixed-length ISA라면 instruction 단위도 무방). walk 도중 retire되는 instruction은 버려지므로 Fill Buffer는 retire stream의 일부만 sampling함 | entry당 16B: valid bit, uop의 decoded byte(그 uop이 읽고 쓰는 register와 memory location 포함), PC, 그리고 그 uop이 H2P branch이거나 H2P branch dependence chain의 일부인지를 나타내는 **chain-bit**. chain-bit은 H2P branch에 대해서도, TEA thread의 일부로 실행된 instruction에 대해서도 초기에 set 되어 새 walk의 initiation point가 됨 (§III-C, §IV-C) |
| **Source List** (walk 상태) | walk 중 표시된 H2P branch를 계산하는 데 필요한 live-in register와 memory location을 추적 | Backward Dataflow Walk state machine 내부 상태. walk 1회에 **~500 cycle**. 성능은 walk 소요 시간에 그리 민감하지 않으므로 관련 구조에 multiple access port를 둘 필요 없음 (§IV-C) | register 의존성: architectural register 개수만큼의 길이를 가진 bit vector. memory 의존성: memory address를 담는 작은 **16-entry buffer**. walk된 instruction이 Source List 안의 register/memory location에 쓰면 dependence chain instruction으로 표시하고, 그 destination을 Source List에서 제거하며 source를 추가 → 항상 최소 live-in 집합 유지 (§III-A, §IV-C) |
| **Block Cache** | 식별된 모든 dependence chain instruction을 basic-block-sized segment로 저장하고 TEA Fetch에 공급 | **512 entry, 8-way, 19KB**, tag store와 data store로 분리. basic block의 첫 instruction PC로 tag. Table II는 Block Cache에 access latency를 명시하지 않음. 8 sequential uop보다 긴 segment는 여러 entry로 분할. 같은 cache line에 속한 basic block segment들은 같은 cache-line index의 서로 다른 way에 저장. 연속된 cache line의 entry는 (I-cache와 유사하게) 두 bank에 분산되고, decoupled BP가 만든 fetch address가 두 연속 cache line의 모든 Block Cache entry를 읽어내므로 여러 sequential segment에 걸쳐 cycle당 최대 8 uop 공급 가능 (§IV-C) | tag store: **40-bit tag** (basic block 첫 instruction의 PC). data store: decoded dependence chain uop (평균 **4B/uop**, entry당 최대 8 uop) + 그 basic block의 어느 instruction이 chain에 속하는지 표시하는 **32-bit bit-mask**. 추가로 **256-entry zero-tag store**: chain uop이 없는(bit-mask가 전부 0인) basic block 전용 tag만 두어 data store entry를 아끼며, 그 빈 segment 너머에 chain uop이 더 있을 수 있으니 TEA thread를 종료하지 말라는 표시 역할. bit-mask는 **500K instruction마다 주기적으로 reset** (§IV-C) |
| **TEA Fetch stage / shadow Fetch Queue** | Block Cache에서 TEA thread instruction을 main thread의 I-cache fetch와 병렬로 fetch | 전용 **8-wide Fetch stage**. decoupled BP가 만든 fetch address가 Block Cache와 I-cache **양쪽**으로 전달되고 TEA용 shadow Fetch Queue에 삽입됨 (§IV-D, Fig.4). Block Cache **hit**에서 TEA thread가 initiate됨. 읽어낸 uop은 이미 decoded 상태이므로 rotate 후 shadow Rename으로 직행. main thread의 소비 속도가 느린 rate mismatch를 흡수하기 위해 main thread Fetch Queue를 최대 **128 fetch address**까지 확장 — BP가 peak throughput으로 동작할 수 있게 함. TEA thread의 run-ahead 거리는 이 Fetch Queue 크기로 제한됨 (§III-B, §IV-D) | fetch address. Block Cache에서 읽은 bit-mask는 main thread를 먹이는 작은 queue로도 전달되어, main thread instruction 중 Fill Buffer에서 walk initiation point로 쓸 것을 표시하는 데 사용 (§IV-D) |
| **Shadow RAT** | TEA thread instruction을 main RAT과 독립적으로 rename | 전용 **8-wide TEA Rename stage** (Fig.4, Table II). 첫 TEA instruction이 rename되기 전에 main RAT 내용을 shadow RAT으로 복사해 두 thread 상태를 동기화 (§IV-D). TEA thread는 **speculative RAT만** 유지 (§IV-E). flush 시 checkpoint/복구된 RAT 상태를 main RAT과 shadow RAT 양쪽에 복사. TEA thread가 main보다 훨씬 앞서 달릴 때는 main RAT 대신 **shadow RAT을 checkpoint** (§IV-F) | architectural register → physical register 매핑 (논문에 세부 분해 없음) |
| **PR map (Valid bit + Reference Counter)** | TEA instruction은 ROB에 들어가지 않아 in-order retirement에 의존할 수 없으므로 physical register를 조기에 free | physical register당 1 entry. Table II "Other structures" 행에 **"PR map queue, 2400 bits"** 로 기재. TEA thread initiate 시 초기화 (§IV-E) | PR당 **Valid bit 1개 + 5-bit Reference Counter**, TEA thread 시작 시 각각 1과 0으로 초기화. TEA instruction이 rename되면 자기 destination AR에 매핑돼 있던 이전 PR의 Valid bit을 0으로 만들고, 그 PR이 사용 중이 아니면(Valid=0, Reference Counter=0) free. PR을 읽으려는 모든 instruction은 Rename에서 Reference Counter를 증가시키고, Execution Unit 진입 직전 데이터 값을 읽을 때 감소시키며, 감소 후 invalid하기까지 하면(Valid=0, Reference Count=0) free. counter overflow는 잘못된 precomputation을 낳을 수 있으나 main thread에는 영향 없고, TEA thread가 자주 flush되므로 매우 드묾 (§IV-E) |
| **TEA thread store data cache** | TEA thread store가 쓴 값을 buffering — architectural state인 D-cache를 갱신해선 안 되므로 | **16 entry, entry당 32B** (Table II). TEA store가 쓴 마지막 16개 half-line을 buffering. Table II는 access latency를 명시하지 않음 (§IV-E) | buffering된 half-line의 store data (32B/entry) |
| **Reservation Station / Physical Register partition** | TEA thread가 main thread instruction 때문에 back-pressure 받지 않도록 backend 자원을 보장 | TEA thread가 active인 동안 backend 실행 자원을 **static partition**: **192 Reservation Station**과 **192 Physical Register**를 TEA에 예약 (§IV-A, §IV-E, Table II). Execution unit, cache port, MSHR은 두 thread가 공유. Issue logic은 8-wide이며 두 개의 8-wide Rename 출력 중에서 고르되 TEA instruction을 우선하고 남는 Issue slot을 main thread에 줌 (§IV-D, §IV-E) | Reservation Station entry당 추가 1 bit으로 TEA instruction을 식별; TEA entry는 실행을 마치면 폐기 (§IV-E) |
| **In-flight branch queue (수정됨)** | TEA branch가 계산한 direction·target을 main thread counterpart에 전달하고, 잘못된 precomputation에 대한 fail-safe 검사 역할 | 파이프라인 안의 모든 main thread in-flight branch 정보를 추적하는 **기존** 구조. TEA branch가 resolve되면 대응하는 main thread branch의 entry가 precomputed direction·target을 반영하도록 수정됨. main thread branch가 실행을 끝내면 이 queue를 읽어 misprediction이 올바로 해소되었는지 확인하고, 아니면 또 한 번 misprediction flush 발행 (§IV-F, §IV-G) | entry에 precomputed branch direction과 target 추가 (§IV-F) |
| **Main RAT의 poison bit** | 잘못된 precomputation을 조기에 검출해 TEA thread를 선제 종료 — 두 번째 misprediction flush를 회피 | main RAT의 architectural register당 poison bit 1개. TEA thread initiate 시 모든 AR에 대해 0으로 초기화 (§IV-G) | AR당 1 bit. H2P chain에 속하지 **않는** main thread instruction(Block Cache bit-mask가 0)은 자기가 쓰는 AR의 poison bit을 set; chain에 **속하는** instruction(bit-mask 1)은 자기 destination AR의 poison bit을 clear. main thread의 chain instruction이 poisoned register를 **읽으면** TEA thread의 chain이 틀린 것이므로 선제 종료하고, 아직 실행되지 않았고 문제의 instruction보다 younger한 TEA branch들은 misprediction flush 발생이 차단되며, 남은 TEA instruction은 점진적으로 drain (§IV-G) |
| **Per-stage frontend timestamp comparator** | partial frontend flush 지원 — TEA thread가 너무 앞서 달려 frontend의 일부 instruction이 flush 대상 branch보다 **older**인 상황에 필요 | frontend 파이프라인 stage마다 flush signal 앞에 comparator 하나 추가. 해당 stage instruction의 timestamp를 mispredicting branch의 것과 비교. comparator latency는 branch predictor history 수정 같은 다른 flush 작업과 overlap되므로 전체 misprediction flush latency에 영향을 줄 가능성이 낮음. Fetch Queue도 partial flush됨 (§IV-F) | branch predictor가 생성한 timestamp. TEA branch는 main thread counterpart와 **동일한** timestamp를 상속 (§III-B, §IV-F) |

### 6.4 Operation — stage by stage

```
[Retire]  retired uops ──▶ Fill Buffer (512 entry, program order)
              │              ├─ 진입 시 H2P Table 조회 → branch이면 H2P 표시
              │              └─ Block Cache bit-mask로 표시된 main thread instruction도 chain-bit set
              ▼  (Fill Buffer full)
[BW Walk] youngest entry부터 older 방향으로 순회 (~500 cycle)
              ├─ H2P branch 만남 → 그 branch 계산에 필요한 register/memory를 Source List에 추가
              ├─ Source List의 항목에 쓰는 instruction → chain instruction 표시,
              │     destination 제거 + source 추가 (live-in 최소 집합 유지)
              └─ 여러 H2P branch, 같은 branch의 여러 dynamic instance를 동시 추적
              ▼
[Store]   표시된 instruction들을 basic block segment로 묶어 Block Cache에 기록
              └─ 같은 block의 서로 다른 control flow mask들은 bit-wise OR (500K instr마다 reset)
              ▼
[Fetch]   decoupled BP fetch address ──┬──▶ I-cache  ──▶ Main Fetch (Fetch Queue 최대 128 addr)
                                       └──▶ Block Cache ──▶ TEA Fetch (8-wide, 최대 8 uop/cycle)
                                                              └─ hit이면 TEA thread initiate
              ▼
[Rename]  uop은 이미 decoded → rotate 후 shadow Rename(8-wide) 직행, Shadow RAT 사용
              (첫 TEA instruction rename 전에 main RAT → shadow RAT 복사)
              ▼
[Issue]   8-wide Issue가 두 Rename 출력 중 선택, TEA 우선 / 남는 slot을 main에
              192 RS + 192 PR 예약. TEA instruction은 ROB에 들어가지 않음
              ▼
[Execute] TEA branch 실행 완료 → timestamp 기반 early misprediction flush
              + in-flight branch queue entry에 precomputed direction·target 기록
```

**Training (§III-A, §IV-B, §IV-C).** retire된 instruction이 program order로 512-entry Fill Buffer에 들어간다. 진입 시 각 branch를 256-entry 8-way H2P Branch Table에서 조회하고 3-bit counter가 1보다 크면 H2P로 표시한다. Fill Buffer가 가득 차면 state machine이 **가장 young한 entry**에서 시작해 Backward Dataflow Walk를 수행한다 (Fig.1-(b) 예에서는 C0에서 시작). older 방향으로 instruction 단위로 순회하다 H2P branch를 만나면 (예: A2, condition code register RFLAGS) 그 branch를 계산하는 데 필요한 register와 memory address를 Source List에 넣는다. Source List 안의 것에 쓰는 instruction은 chain instruction으로 표시되고 (A1이 표시되며 destination RFLAGS 제거, source R1 추가), 계속 위로 올라가며 A0가 표시되고 Source List는 R4, R5, memory location [R4+R5]가 된다. walk는 Fill Buffer의 가장 old한 instruction(B0)까지 계속된다. H2P로 표시된 모든 branch의 chain을, 같은 H2P branch의 여러 dynamic instance를 포함해, **동시에** 추적한다 (§III-A).

**Chain 확장 (§III-C, Fig.2).** TEA thread로 실행된 instruction도 chain-bit이 켜진 채 Fill Buffer에 들어오므로, 다음 walk는 H2P branch가 아니라 그 chain instruction에서 시작한다. Fig.2에서 첫 pass는 D3 D4 D5, 둘째 pass는 B3 C4 D3 D4 D5, 셋째 pass는 A0 A1 B3 C4 D3 D4 D5로 chain이 자란다.

**Memory dependency (§III-D).** Source List는 walk 중 memory 의존성도 추적한다. correlated load-store pair가 precomputation accuracy에 큰 영향을 주기 때문이며, H2P branch가 function body 안에 있을 때 특히 그렇다 — 이 경우 branch direction을 미리 계산하려면 function call을 가로질러 chain을 추적해야 하고, 입력 변수를 전달하는 push/pop 연산이 precomputation thread에 포함되어야 한다.

**Storing (§III-E, §IV-C).** 표시된 instruction들(H2P branch 포함)을 basic-block-sized segment로 묶어 Block Cache에 쓴다. Fig.1-(b) 예에서 basic block A의 entry는 A0, A1, A2를 담고 A0의 PC로 tag되며, basic block B의 entry는 비어 있고, basic block C의 entry는 Cx만 담는다. 같은 basic block에 대해 서로 다른 control flow에서 생성된 mask는 bit-wise OR 된다. Fig.3의 예(*mcf* 가 이런 control flow를 가짐): control flow **A-B-D**에서는 basic block A의 mask가 **1000**, **A-C-D**에서는 **0100**이고, OR 하면 **1100** 이 되어 A의 첫 번째와 두 번째 instruction이 모두 Block Cache에 저장된다. 그러면 두 control flow 모두에서 TEA thread가 정확하지만, 어느 한 경로에서는 그 경로의 H2P branch 계산에 필요 없는 instruction이 하나 추가된다.

**Fetch (§III-B, §IV-D, Fig.1-(c)).** decoupled BP는 cycle당 taken branch 하나 또는 128B에 걸친 sequential instruction까지 fetch address를 만들어 Fetch Queue에 push하고, 이 address가 main thread용 I-cache와 TEA용 Block Cache **양쪽으로** 간다. Block Cache hit에서 TEA thread가 시작된다. TEA Fetch는 두 bank에서 두 연속 cache line의 entry를 읽어 cycle당 최대 8 uop을 공급한다. main thread가 basic block C를 다 fetch하는 데 3 cycle이 필요할 때 TEA thread는 C의 chain segment를 1 cycle에 읽으므로, C가 나올 때마다 2 cycle씩 앞선다. main thread가 non-dependence-chain instruction을 fetch하느라 막히거나 backend pressure로 stall할 때마다 TEA thread는 더 앞서 나가고, 이 격차는 첫 misprediction이 검출될 때까지 누적된다. run-ahead 거리는 128-entry Fetch Queue 크기로 제한된다. Block Cache에서 읽은 bit-mask는 main thread 쪽 작은 queue로도 보내져, main thread instruction 중 Fill Buffer에서 walk initiation point가 될 것을 표시한다.

**Rename과 Issue (§IV-D, §IV-E).** uop은 이미 decoded 상태이므로 rotate 후 shadow Rename으로 직행한다. shadow RAT은 첫 TEA instruction이 rename되기 전 main RAT 복사본으로 seed된다. §IV-A는 TEA thread의 **전체 frontend latency를 9 cycle**이라고 기술하고, Table II는 8-wide Fetch/Rename stage에 대해 **8-cycle latency**를 적는다 — 논문 내부에서 이 두 값이 일치하지 않으므로 양쪽을 모두 적어 둔다. rename된 TEA instruction은 TEA를 우선하는 8-wide Issue logic으로 가고, 예약된 192 RS와 192 PR을 쓴다. TEA instruction은 ROB에 들어가지 않고 Valid-bit / 5-bit Reference Counter 방식으로 PR을 free한다.

**Use (§IV-F).** mispredict된 TEA branch가 실행을 끝내면 flush가 trigger된다. TEA branch는 branch predictor timestamp를 상속받아 main thread counterpart와 같은 timestamp를 가지므로, 그 branch보다 younger한 모든 instruction이 — main thread든 TEA thread든 — 일반적인 OoO flush와 똑같이 폐기된다. backend는 통상적인 OoO core와 동일하게 부분 flush되고, checkpoint되었거나 복구된 RAT 상태가 main RAT과 shadow RAT 양쪽에 복사되며, 이것이 branch predictor history와 PC 수정과 함께 두 thread 상태를 동기화한다. 동시에 in-flight branch queue의 대응 entry가 precomputed direction·target으로 갱신되고, 나중에 main branch가 실행을 끝낼 때 그 entry를 읽어 precomputation이 틀렸을 때만 또 한 번 flush를 발행한다.

**Termination (§IV-G, §V-B).** TEA thread는 (1) Block Cache **miss**, (2) TEA branch가 **incorrectly computed**로 판정될 때 종료된다. miss의 경우 남아 있는 TEA instruction은 계속해서 임의의 branch direction을 precompute하고, incorrect precomputation이 검출되면 남은 TEA instruction 전부를 프로세서 밖으로 drain한다. 추가로 (3) TEA thread가 H2P branch 실행을 main thread보다 늦게 끝내는 일이 **4번을 넘으면** TEA thread를 종료한다 (§V-B).

### 6.5 Correctness와 speculation 제어

TEA thread는 완전히 speculative하며 architectural state를 전혀 갱신하지 않는다. main thread의 정확성은 기존 flush 하드웨어로 보장된다.

- **Early flush (§IV-F).** synchronized timestamp 덕분에 TEA branch는 자기 main thread counterpart보다 younger한 모든 instruction을 flush할 수 있다. §II-C가 지적하듯 이것이 별도의 forwarding queue나 parallel scanning 없이 direction과 target 양쪽 precomputation을 가능하게 하는 지점이다.
- **Partial frontend flush (§IV-F).** TEA thread가 너무 앞서 달리면 frontend 파이프라인 stage에 flush 대상 branch보다 **older**인 instruction이 존재한다. 이를 위해 stage마다 flush signal 앞에 timestamp comparator를 두어 older instruction은 남긴다. Fetch Queue도 partial flush되며, 이 경우 그 branch의 full misprediction penalty는 사라진다. frontend가 partial flush될 때 **main RAT 상태는 복구할 필요가 없고 shadow RAT은 복구해야 하므로**, TEA thread가 main보다 훨씬 앞서 달리는 동안에는 main RAT 대신 shadow RAT을 checkpoint한다.
- **Fail-safe (§IV-F, §IV-G).** incorrect precomputation 검출 방법은 두 가지다. 첫째, 수정된 in-flight branch queue entry에 저장된 precomputed direction·target을 main thread branch가 실행을 끝낼 때 검사한다 — 틀렸으면 control flow를 바로잡기 위해 또 한 번 misprediction flush를 발행하며, 논문은 이것이 드물고 성능 영향은 negligible하다고 말한다. 둘째가 아래의 poison bit이다.
- **Poison bit 기반 조기 검출 (§IV-G).** main RAT의 AR당 poison bit이 TEA initiate 시 0으로 초기화된다. dependence chain에 속하지 않는 main thread instruction(bit-mask 0)이 쓰는 AR은 poison되고, chain에 속하는 instruction(bit-mask 1)이 쓰는 AR은 poison이 해제된다. chain instruction이 poisoned register를 읽으면 그 chain은 TEA thread 시작 이후 non-chain instruction이 만든 값을 필요로 했다는 뜻이므로 정의상 틀렸고, TEA thread를 선제 종료한다. 이 필터링이 **두 번째 misprediction flush 없이** 대부분의 incorrect precomputation을 걸러내어 **추가 flush를 0.001 PKI 미만**으로 낮춘다. §V-B는 SPEC17 일부 benchmark에 non-negligible한 incorrect precomputation이 있지만 (Fig.7) RAT-poisoning이 거의 항상 검출해 추가 misprediction flush 발행을 막는다고 보고한다 (**< 0.05%**).
- **Memory speculation (§IV-E).** TEA load는 prefetch와 유사하게 취급된다 — **memory ordering을 강제하지 않고, Load Queue entry를 할당받지 않으며**, D-cache port를 확보했을 때에만 Reservation Station을 떠난다. D-cache에서 miss한 TEA load는 destination PR을 MSHR entry에 담고 다닌다. 반면 TEA store는 machine의 architectural state를 갱신하므로 cache에 쓸 수 **없고**, 대신 마지막 16개 half-line을 담는 작은 store data cache에 쓴다.
- **Resource correctness (§IV-E).** TEA instruction은 ROB에 들어가지 않으므로 in-order retirement bottleneck을 피하려 backend 자원을 가능한 한 빨리 free한다. Reference Counter가 overflow하면 잘못된 precomputation이 나올 수 있으나 main thread에는 영향이 없고, TEA thread가 자주 flush되므로 매우 드물어 성능에 유의미한 영향이 없다.

### 6.6 Parameters와 storage budget

**Baseline OoO core (Table I).**

| 범주 | 값 |
|---|---|
| Core | 3.2GHz, 8-wide issue, 12-cycle FE latency; 512-entry ROB, 352-entry Reservation Station, 16-wide retire; **12 Execution Ports (6-ALU, 2-LD, 2-LD/ST, 2-FP)**; 400 Physical Reg, 256-entry load queue, 192-entry store queue |
| Predictors | 64KB TAGE-SC-L [23], 128-entry Fetch Queue; history-based indirect branch predictor, RAS; 1 taken branch per cycle, 4k-entry BTB |
| Caches | 32KB 8-way L1 I-cache (4-cycle access), 2R·1W ports (4 banks); 48KB 12-way D-cache (4-cycle access); 1MB 16-way LLC (18-cycle access), 64B line |
| Memory | DDR4_2400R: 1 rank, 2 channel; channel당 4 bank group, 4 bank; **tRP-tCL-tRCD: 16-16-16** |
| Frontend 세부 (§IV-A) | decoupled BP가 cycle당 taken branch 1개 또는 128B에 걸친 sequential instruction까지 예측; fetch address당 cycle당 최대 2개의 sequential cache line 읽기; Decode/Rename/Issue는 cycle당 최대 8 uop; frontend 깊이 12 cycle; branch의 최소 fetch-to-resolution(실행 종료) latency 15 cycle |

**TEA thread structures (Table II).**

| 범주 | 값 |
|---|---|
| Core | 8-wide Fetch and Rename Stages, **8-cycle latency**; Issue-ports는 main thread와 공유; TEA thread active 시 **192 PR, 192 RS** 예약 |
| Caches | H2P Branch Table: 256-entry cache, **0.2KB, 1-cycle access**; Block Cache: 512-entry cache, **256 zero-tags**, 8-way, **19KB** (access latency 미기재); Store data cache for TEA thread: 16-entry cache (32B per entry) (access latency 미기재) |
| Other structures | Fill Buffer: 512-entry queue, **8KB, single access port**; **PR map queue, 2400 bits**; **Shadow RAT** (크기 미기재) |

**본문에만 있는 파라미터.**

- H2P Table: 3-bit saturating counter, 최초 misprediction 시 1로 초기화, counter > 1이면 H2P, 모든 counter를 **50k instruction마다 1씩 감소** — 즉 50k instruction당 1회 미만으로 mispredict하는 branch(**0.02 MPKI 미만**)의 counter는 0으로 수렴; counter 0 entry가 replacement 우선 (§IV-B).
- Backward Dataflow Walk: state machine, **~500 cycle**; register bit vector 길이 = architectural register 수; memory 의존성용 **16-entry** address buffer (§IV-C).
- Fill Buffer: entry당 **16B**; Fill Buffer 크기는 성능에 유의미한 영향이 없음(**~1% 변화**) — bit-mask 덕분에 여러 walk에 걸쳐 긴 chain을 추적할 수 있기 때문 (§IV-C).
- Block Cache: 40-bit tag, entry당 최대 8 uop, uop당 평균 4B, entry당 32-bit bit-mask; 8 sequential uop보다 긴 segment는 분할; 연속 cache line은 두 bank에 분산 (§IV-C).
- Block Cache bit-mask **reset 주기 500K instruction** (성능이 가장 좋은 값) (§IV-C).
- TEA frontend 총 latency: **§IV-A는 9 cycle**, **Table II는 8-cycle** — 논문 내부 불일치.
- main thread Fetch Queue를 최대 **128 fetch address**로 확장; 이것이 TEA thread run-ahead 거리의 상한 (§III-B, §IV-D).
- late precomputation에 의한 종료 임계값: TEA thread가 H2P branch를 main thread보다 늦게 끝내는 일이 **4회 초과**면 종료 (§V-B).
- §V-D의 dedicated execution engine 구성: **192 Reservation Station과 Physical Register + 16개 전용 execution unit** (Fig.9).

**Storage 총량.** 논문은 합산된 단일 storage 수치를 제시하지 않는다 (Table II 항목 나열이 전부: H2P Branch Table 0.2KB + Block Cache 19KB + store data cache 16×32B + Fill Buffer 8KB + PR map queue 2400 bits + Shadow RAT(크기 미보고)). 대신 §IV-H가 면적 결과를 보고한다: 주된 면적 overhead는 **Fill Buffer와 Block Cache**에서 오고 duplicated pipeline stage와 shadow RAT이 더해져 **총 core 면적의 약 3.5%**, 그중 **2%가 넘는 부분이 cache들과 Fill Buffer**에서 온다.

### 6.7 Evaluation setup과 results

**Setup (§V-A, §IV-H).**

- **시뮬레이터**: Scarab [4], execution-driven cycle-accurate x86-64 시뮬레이터. TEA thread를 구성·실행하는 데 필요한 구조와 로직을 추가해 aggressive OoO core의 마이크로아키텍처를 모델링. main memory는 **Ramulator [16]** 로 모델링.
- **Baseline**: Table I의 8-wide OoO core로, 여러 industry product([1] AMD Zen4, [2] Apple M1, [3] Intel Goldencove)와 유사하게 모델링. **모든 결과는 이 baseline core 구성 대비**.
- **워크로드**: **SPEC CPU2017** [5] (ref input set) + **GAP benchmark suite** [6] (입력 g=19, n=300). **MPKI 0.5 미만인 benchmark는 제외** (floating point benchmark 5개).
- **방법론**: **SimPoint** [25] — benchmark당 최대 **5개 SimPoint**, SimPoint당 **200M instruction**, 각 실행 앞에 **200M instruction warmup**.
- **Power/area/energy**: **McPAT** [18] 로 추정 (§IV-H).
- **비교 대상**: 같은 baseline OoO core 위에 dedicated execution engine을 쓰는 **scaled-up Branch Runahead** 구현 (§V-C, Fig.8). application을 simple control flow / complex control flow 두 범주로 나누며, **GAP benchmark 전부와 xz가 simple control flow**로 분류된다 (xz는 그 범주의 유일한 SPEC benchmark). 논문 각주 3은 "branch로 이어지는 control flow가 단순하다고 해서 그 branch를 예측하기 쉬운 것은 아니다"라고 덧붙인다.
- **Ablation (§V-E, Fig.10)**: TEA 전체 / **only loops** (H2P branch의 연속된 두 instance 사이에서만 chain instruction을 기록 → chain을 loop로 한정) / **no masks** (Block Cache entry를 결합하지 않고 walk를 H2P branch에서만 시작) / **no mem** (walk에서 memory dependency 미포함) / **Branch Runahead chains**.

**Headline 결과.**

| 결과 | 수치 | 비교 대상 | 출처 |
|---|---|---|---|
| 성능 | **+10.1% geomean**, 대부분의 benchmark에 비교적 고르게 분포 | aggressive 8-wide baseline OoO core | Abstract, §V-B, Fig.5 |
| vs Branch Runahead | **10.1% vs 7.3% geomean** — TEA는 on-core 자원만 쓰고 BR은 dedicated execution engine을 씀에도 | 같은 baseline | §V-C, Fig.8 |
| Dedicated execution engine | on-core 10.1% → **12.3%** (192 RS/PR + 16 전용 execution unit, baseline core 크기는 그대로) | 같은 baseline | §V-D, Fig.9 |
| 훨씬 큰 execution engine | **12.8% geomean** — main OoO core와 동일한 backend를 줘도 추가 이득은 매우 작음 | 같은 baseline | §V-D |
| Precomputation accuracy | 평균 **99.3%**; 다른 모든 구성은 3~5% 나쁨, mask가 가장 큰 영향 | Fig.10-(a) 구성 간 | §V-E |
| Misprediction coverage | **76%**; 전 feature 제거 시 **39%** | Fig.10-(b) | §I, §V-E |
| Timeliness | Fig.10-(c)는 branch당 절약된 평균 misprediction cycle 수(main thread의 branch resolution cycle 기준)로 측정. 전 feature 구성이 **가장 무거운** precomputation thread(총 dynamic instruction의 **31.19%** vs no-features 21%)임에도 거의 모든 application에서 branch당 절감이 가장 큼. 예외는 **xalanbmk와 xz** 둘뿐 — 이 둘에서는 Block Cache mask로 여러 control flow의 chain을 결합하는 것이 timeliness를 해침 | Fig.10-(c) | §V-E |
| 오계산 | TEA thread branch의 **0.7% 미만**이 incorrectly computed; **~76%** 의 TEA branch가 최소 1 cycle의 misprediction penalty를 절약 | — | §IV-E |
| 추가 flush | incorrect precomputation에 의한 추가 misprediction flush **< 0.001 PKI** (§IV-G); §V-B는 RAT-poisoning이 막아 **< 0.05%** 로 보고 | — | §IV-G, §V-B |
| Prefetch 기여 분리 | early resolution을 끄면 이득이 **1.2%** 뿐 → 이득의 출처는 branch resolution | baseline | §V-B |
| Dynamic instruction | **+31.9%** vs Slipstream [26] **85%**, Branch Runahead [21] **34%** | baseline OoO core | §IV-H, Table III |
| Area | **~3.5%** of total core area (그중 2%↑가 cache들과 Fill Buffer). 참고로 진짜 16-wide OoO core는 **~10%** 면적에 **2.8%** 성능 | baseline core 면적 | §IV-H |
| Power | McPAT 기준 peak power **+8.5%**; 추가 frontend(Block Cache, Rename stage, shadow RAT)가 추가 power의 **6% 이상**을 차지 | baseline | §IV-H |
| Energy | 평가 benchmark 집합 전체에서 총 energy **-2%** — 실행 시간 단축과 main thread wrong-path instruction fetch 감소가 추가 dynamic instruction을 상쇄. *nab* 과 *pr* 은 wrong path fetch 감소가 매우 커서 두드러짐 | baseline | §IV-H, Table III |

**Per-benchmark 분석 (§V-B).** 가장 성능이 좋은 benchmark는 ***mcf, bfs, cc, tc*** — misprediction이 많고 그 대부분이 프로그램의 critical path 위에 있으며, H2P branch에 가려진(guarded) load가 많아 LLC hit이거나 main memory까지 가기 때문이다. 이 branch를 일찍 resolve하면 correct path load가 더 일찍 실행을 시작해 memory level parallelism이 늘어난다. ***perlbench*** 와 ***nab*** 은 branch MPKI가 높지 않은데도 상당한 개선을 보이는데, 소수의 H2P branch 그늘에 long latency load가 많기 때문이다. ***xalanbmk*** 는 유일하게 이득의 대부분이 data prefetching에서 온다. ***deepsjeng*** 과 ***omnettpp*** 는 Block Cache 크기에 주로 제약되며 (dependence chain이 없어서 생기는 낮은 misprediction coverage), 추적 basic block segment 수를 늘리면 5% 개선된다. chain uop이 없는 basic block용 추가 tag store entry(zero-tag)는 ***perlbench, gcc, omnettpp, deepsjeng, leela*** 의 Block Cache 용량 활용을 개선한다. ***gcc, mcf, omnettpp, xz*** 에는 "late" precomputation이 있으나 대부분 두 thread가 같은 cycle에 끝나는 경우이고, TEA가 실제로 더 늦게 끝내는 경우는 0.1% 미만이다.

**vs Branch Runahead 세부 (§V-C, Fig.8).** simple control flow benchmark에서는 BR이 비등하게 잘 한다 — BR은 **merge point prediction**으로 loop 안의 independent branch를 명시적으로 식별해, 이전 branch가 mispredict되더라도 그 branch의 여러 invocation을 병렬로 issue·실행할 수 있기 때문이다. TEA thread는 control flow를 main branch predictor에 의존하므로 이를 활용할 수 없고, 대신 TAGE의 높은 중간 branch 예측 정확도로 그 이득의 일부를 흡수한다 (§III-B). 그래서 TEA는 simple control flow application에서 timeliness가 덜해 branch당 절감이 약간 작지만, 더 높은 misprediction coverage로 이를 만회한다. BR이 TEA보다 훨씬 잘 하는 것은 ***sssp*** 와 ***bc*** 뿐이고, 주된 이유는 BR이 훨씬 많은 backend 실행 자원을 쓸 수 있기 때문이다. complex control flow benchmark는 independent branch가 적고 chain이 복잡해 misprediction level parallelism이나 control independence를 뽑아내기 어렵고, BR은 loop 경계 안에 갇힌 특정 control flow 유형에서만 최적으로 동작하므로 이런 benchmark에서 잘 하지 못한다.

**Dedicated engine 세부 (§V-D, Fig.9).** 별도 execution engine은 on-core 자원 사용에서 오는 간섭을 없앤다. 개선분의 대부분은 ***mcf, xalanbmk, nab, pr, bc*** 에서 나오며 주로 execution unit 수 증가 덕분이고, ***sssp*** 는 특히 두 thread 간 간섭 최소화에서 이득을 본다. 다만 이 구성은 on-core 대비 훨씬 큰 area·power overhead를 동반한다.

**Ablation 세부 (§V-E).** 어떤 단일 feature도 이득의 대부분을 담당하지 않는다. **memory dependency 포함은 정확한 precomputation에 가장 덜 중요한 feature**다. graph benchmark 중 ***sssp, pr, bc*** 는 어떤 feature에도 상대적으로 영향받지 않는데, 이들의 H2P branch 대부분이 Fig.1이 모델링한 control flow와 일치해 어떤 dynamic chain 구성 방식으로도 쉽게 잡히기 때문이다 — Branch Runahead조차 이 benchmark들에서는 100%에 가까운 precomputation accuracy를 낸다. Branch Runahead는 자기가 커버하는 H2P branch에 대해서는 높은 accuracy를 갖는데, 이는 incorrect precomputation을 내는 H2P branch의 chain을 적극적으로 **제거**하기 때문이고, 그 대가가 Fig.10-(b)의 낮은 misprediction coverage다.

### 6.8 저자가 밝힌 limitation과 self-positioning

**저자가 인정한 한계.**

- **Block Cache 용량**이 misprediction coverage를 제한한다 (*deepsjeng*, *omnettpp*). 추적 basic block segment 수를 늘리면 5% 개선되지만, **Block Cache storage는 I-cache만큼 dense하지 않아 확장이 쉽지 않다** — entry는 최대 8 uop을 담지만 basic block당 dependence chain uop 수의 편차가 크다. 이 storage 비효율을 줄이려고 chain이 없는 basic block용 추가 tag store entry를 넣었다 (§V-B, §IV-C).
- **여러 control flow의 chain 결합은 어느 한 경로에서 불필요한 instruction을 추가해 timeliness를 해친다**. 이는 chain accuracy와 길이 증가로 상쇄되지만, ***xalanbmk*** 와 ***xz*** 에서는 실제로 timeliness를 악화시킨다 (§III-E, §V-E).
- **memory dependency 포함은 때때로 accuracy 개선 없이 thread 크기만 키운다** — correlated load-store pair의 memory address가 시간에 따라 변할 수 있기 때문. 다만 대부분의 benchmark에서는 chain 길이·정확도 개선이 이를 상쇄한다. ablation에서 가장 덜 중요한 feature이기도 하다 (§III-D, §V-E).
- **misprediction-level parallelism을 활용할 수 없다.** control flow를 main branch predictor에 의존하므로 Branch Runahead의 merge point prediction 같은 방식으로 독립 branch의 여러 invocation을 병렬화하지 못하고, 그래서 simple control flow application에서 branch당 절감이 다소 작으며 *sssp*, *bc* 에서는 BR에 진다 (§V-C).
- **main thread instruction 실행 지연.** TEA의 Issue를 우선하고 RS entry·PR의 상당 비율을 할당하는 것은 일부 main thread instruction의 실행을 늦춘다 — 저자는 H2P branch misprediction이 거의 항상 critical path에 있으므로 그래도 이득이라고 논증한다 (§IV-E).
- **PR Reference Counter overflow**는 잘못된 precomputation을 낳을 수 있다. main thread에는 영향이 없고 TEA thread가 자주 flush되므로 매우 드물다 (§IV-E).
- **Late 및 incorrect precomputation이 남아 있다** — *gcc, mcf, omnettpp, xz* 의 late 사례, 일부 SPEC17 benchmark의 non-negligible한 incorrect precomputation (§V-B, Fig.7).
- **명시된 비용**: dynamic instruction +31.9%, core 면적 ~3.5%, peak power +8.5% (§IV-H). on-core 구현은 dedicated execution engine 대비 2.2 percentage point를 남겨 둔다 (10.1% vs 12.3%) (§V-D).
- **H2P 표시 임계값의 한계**: 더 많은 branch를 H2P로 표시할수록 coverage와 성능이 개선되지만, **정확도가 아주 높은 branch까지 H2P로 표시하기 시작하면 timeliness를 크게 해쳐 이득이 꺾인다** (§IV-B).
- **Future work**: 결론은 timeliness보다 accuracy와 coverage에 더 큰 비중을 두는 precomputation 기반 문제들로 폭넓은 해법을 열어 준다고 밝힌다 (§VI).

**Attribution (저자 자신이 밝힌 계보).** Backward Dataflow Walk는 TEA의 발명이 아니다 — §III-A는 "We use a variant of the Backward Dataflow Walk proposed in **Criticality Driven Fetch [10]**"라고 명시한다. §II-B는 **DP-SSMT [9]** 가 post-retire buffer에서 live-in/live-out을 추적하는 dataflow walk와 trigger instruction을 썼고, 유사한 접근인 Backward Dataflow Walk가 **Filtered Runahead [14]** 에서 revisit되었으며 **Branch Runahead [21]** 에서 쓰인다고 기술한다. H2P branch 식별용 per-branch counter table 역시 §IV-B가 "Similar to previous work"라고 밝힌다.

**Related work에서의 자기 위치 (§II).**

- **§II-A Compiler-based**: static analysis 기반 helper thread는 항상 correct하지만, 정확성을 위해 compile time의 모든 가능한 control flow를 감안하다 보니 과팽창하여 너무 느려 도움이 안 된다 [8], [22], [27], [30]. 이후 연구는 profiling으로 helper thread 크기를 줄였지만 [8], [17], [22], [24], [28], [30], [31], profiling이 항상 대표적이지 않고 phase behavior를 반영하지 못해 정확도를 해친다. **CRISP [19]** 는 data center 환경에서 여러 입력 데이터셋을 지속적으로 profiling해 critical path 위의 H2P branch와 long latency load dependence chain을 찾아 backend에서 우선 실행시키는 lightweight compiler 해법이지만, branch dependence chain을 몇 cycle 일찍 스케줄하는 정도만 허용해 이득이 제한적이다. 하이브리드 접근인 **Control-Flow Decoupling [24]** 은 loop 안의 control-flow 계산을 hoist해 계산된 branch direction을 queue에 미리 넣지만, loop가 없거나 iteration이 적으면 branch로 이어지는 모든 control flow를 감당하기 위한 상당한 code duplication이 필요해 어렵다.
- **§II-B Runtime**: **IBDA [7]** 는 RAT entry에 그 register에 마지막으로 쓴 instruction의 PC를 태깅해 H2P가 필요로 하는 register에 쓰는 instruction을 식별하고, 이를 반복해 chain을 찾아 fetch stream을 필터링한다. 그러나 (i) call/return 같은 control flow 구조를 가로지르는 길고 정확한 chain에 필요한 **memory dependency를 추적할 수 없고**, (ii) H2P branch를 볼 때마다 chain의 **한 단계씩만** 포착하며, (iii) normal fetch stream을 필터링하는 방식이라 **baseline main thread보다 빠르게 chain instruction을 fetch할 수 없다**. **[13]의 Branch Tracker Table** 도 유사하게 동작하지만 load 하나에 몇 개의 arithmetic operation이 이어지는 chain에만 통한다. 가장 최근 제안인 **Slipstream [26]** 은 H2P branch에 대해 control-dependent instruction을 전부 제거하고, 그 H2P에 대해 control-independent이지만 data-dependent인 후속 branch도 제거해 misprediction-level parallelism [20]을 활용하지만, 결과 thread가 여전히 heavy-weight이고 **별도 core가 필요**하며 branch direction을 core 간에 forwarding하는 통신 latency가 timeliness를 해친다. **Branch Runahead [21]** 는 논문이 지목한 현재 state of the art로, H2P branch의 연속된 두 instance 사이의 모든 chain instruction을 post-retire buffer로 추적하고, 이전 branch의 예측 적중 여부와 무관한 chain을 가진 branch를 식별해 일찍 시작함으로써 timeliness를 높인다 — 그러나 **복잡한 control flow를 가진 프로그램에서 성능이 나쁘고, 병렬 실행을 위한 dedicated dependence chain engine이 필요하다**.
- **§II-C Early misprediction flush**: 선행 연구는 precomputed direction을 predictor로 forwarding하기 위해 unified queue 또는 per-branch queue를 쓰는데, branch당 수백 개의 prediction을 buffering해 비싸고, precomputation thread의 여러 branch가 함께 끝날 수 있어 multiple write port가 필요하다. 그래서 **선행 연구는 direction만 precompute한다 — target을 추가하는 것이 비싸기 때문**. early resolution까지 지원하려면 여러 파이프라인 stage에서의 simultaneous read가 필요하거나, in-flight branch queue와 precomputed branch queue를 병렬 scanning해 main thread counterpart를 매칭해야 하는데 둘 다 하드웨어 비용과 복잡도를 높인다. **[11]** 은 scanning 방식을 쓰지만 구현 overhead를 논의하지 않고, **[13]** 은 frontend에 몇 개의 고정 flush point만 둔다. TEA는 대신 synchronized timestamp를 써서 기존 flush 하드웨어만으로 direction과 target 양쪽 misprediction을 **추가 overhead 없이** precompute하게 한다.
- **§III / §VI 자기 배치**: TEA thread는 highly accurate하며 복잡한 control flow를 가로질러 긴 dependence chain을 추적하면서도 lightweight를 유지한다. dedicated frontend와 partitioned backend를 써서 on-core에서 효과적으로 실행되며 precomputation의 하드웨어 overhead를 줄인다 (§II-B 말미, §III, §IV). 결론은 TEA thread가 precomputation의 이전 state of the art를 이겼고, 이는 **오직 timeliness에만 집중하는 것이 더 나은 성능을 주지 않음**을 보인다고 주장한다 (§VI).

### 6.9 Section map과 인용 가능한 문장

| § | 주제 |
|---|---|
| §I | Introduction — H2P branch, 선행 precomputation의 세 한계(late / inaccurate / inefficient), timeliness 제약 완화, 4개 contribution (99.3% accuracy와 76% coverage의 lightweight thread, synchronized timestamp로 기존 flush 메커니즘 재사용, on-core 자원으로 dedicated core/engine에 필적, Branch Runahead 상회) |
| §II | Prior Work |
| §II-A | Compiler-based approaches — static helper thread [8],[22],[27],[30], profiling 기반 축소, CRISP [19], hybrid Control-Flow Decoupling [24] |
| §II-B | Runtime approaches — IBDA [7], Branch Tracker Table [13], Slipstream [26], DP-SSMT [9], Filtered Runahead [14], Branch Runahead [21] |
| §II-C | Issuing Early Misprediction Flushes — forwarding queue와 scanning의 비용, TEA의 synchronized timestamp |
| §III | The TEA Thread — dedicated frontend, backend 공유, main branch predictor 기반 control flow, synchronized timestamp 개요 |
| §III-A | Identifying Dependence Chain Instructions — Fill Buffer, Backward Dataflow Walk(Criticality Driven Fetch [10]의 변형), Source List, Block Cache 저장 (Fig.1-(a),(b)) |
| §III-B | Constructing the TEA Thread at Fetch — decoupled BP fetch address, Block Cache 읽기, run-ahead 메커니즘, 128-entry Fetch Queue, TAGE로 중간 branch 처리 (Fig.1-(c)) |
| §III-C | Tracing Longer Dependence Chains — 표시된 chain instruction에서 walk 재개시 (Fig.2, *leela* 패턴) |
| §III-D | Incorporating Memory Dependencies — function call을 가로지르는 live-in, push/pop, 주소 변화의 비용 |
| §III-E | Combining chains across multiple control flows — per-basic-block bit-mask의 bit-wise OR (Fig.3, *mcf* 패턴) |
| §IV | Implementation Details |
| §IV-A | Overview — baseline OoO core와 TEA 구조 개관 (Fig.4, Table I, Table II) |
| §IV-B | Finding H2P Branches — H2P Table 구성, counter, 주기적 감소, H2P 표시 개수에 대한 민감도 |
| §IV-C | Constructing and storing H2P branch dependence chains — Fill Buffer entry 포맷, walk state machine, Block Cache tag/data store, zero-tag, banking, 주기적 bit-mask reset |
| §IV-D | TEA thread frontend — shadow Fetch Queue, Block Cache hit으로 initiate, rotation 후 shadow Rename, shadow RAT, TEA 우선 8-wide Issue |
| §IV-E | TEA thread backend — on-core vs dedicated engine 논거, 192 RS/PR 예약, Valid bit + Reference Counter로 PR free, speculative load, store data cache |
| §IV-F | Branch misprediction flushes — timestamp 기반 flush, RAT 복사, per-stage comparator로 partial frontend flush, in-flight branch queue 갱신·검사 |
| §IV-G | TEA thread termination — Block Cache miss, incorrect precomputation, in-flight branch queue fail-safe, RAT poison bit, 추가 flush < 0.001 PKI |
| §IV-H | Discussion on hardware overhead — dynamic instruction footprint, area, power, energy (Table III) |
| §V | Evaluation |
| §V-A | Methodology — Scarab [4], Ramulator [16], SPEC CPU2017 + GAP, MPKI < 0.5 제외, SimPoint [25] |
| §V-B | Performance — 10.1% geomean, per-benchmark 분석, prefetching 부수효과, Block Cache 한계, late·incorrect precomputation (Fig.5, Fig.6, Fig.7) |
| §V-C | Comparison against Branch Runahead — simple vs complex control flow 분리, 10.1% vs 7.3% (Fig.8) |
| §V-D | On-core vs dedicated execution engine — 12.3%, 12.8% 구성 (Fig.9) |
| §V-E | TEA thread features — chain accuracy, misprediction coverage, timeliness ablation (Fig.10-(a),(b),(c)) |
| §VI | Conclusion |
| — | Acknowledgment (Intel, Arm, NSF grant #2011145); References [1]–[31] |

**인용 가능한 문장 (verbatim).**

1. "Our work relaxes this timeliness constraint by using precomputation results to issue early misprediction flushes instead of overriding the branch predictor." (Abstract)
2. "We argue that these limitations can be overcome if precomputation results that arrive "late", i.e. after the branch is fetched but before it is executed, can still be used to correct the instruction stream." (§I)
3. "We use a variant of the Backward Dataflow Walk proposed in Criticality Driven Fetch [10] for identifying dependence chain instructions." (§III-A)
4. "Since the TEA thread uses existing flush mechanisms (Section IV-F), it supports out-of-order and nested branch resolutions. This significantly reduces the timeliness constraint as the precomputation result only needs to arrive before the corresponding main thread branch finishes execution to provide some benefit." (§III)
5. "The main advantage of this model is the ability to trace any possible control flow by composing different basic block segments using the fetch addresses generated by the branch predictor." (§III-B)
6. "Adding more instructions to a precomputation thread often improves timeliness rather than hurting it if the added instructions increase the length of the dependence chains in the precomputation thread." (§III-C)
7. "We found that prioritizing the Issue of TEA thread instructions and allocating a significant proportion of Reservation Station entries and Physical Registers to it is better for performance, even though it delays the execution of some main thread instructions." (§IV-E)
8. "This is because reading from a poisoned register means that the dependence chain instruction needed a result produced by a non-dependence chain instruction (after the TEA thread started), which is incorrect by definition." (§IV-G)
9. "Precomputation using the TEA thread overall does much better than Branch Runahead (10.1% vs 7.3% geomean improvement) even though it uses on-core execution resources compared to Branch Runahead's dedicated execution engine (which has a large backend optimized to execute branch dependence chains)." (§V-C)
10. "The TEA thread beats the prior state-of-the-art in precomputation, showing that focusing purely on timeliness does not provide better performance." (§VI)

