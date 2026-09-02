# Critical-Path Slice Acceleration — 설계 이해와 구현 현황

> Last updated: 2026-09-01 · branch `test`
> A = 연구 아이디어의 동작 로직 / B = 현재 코드가 그 아키텍처의 어디에 해당하는가
> 참고 논문 정독 노트: [zereco_REFERENCE_NOTES.md](zereco_REFERENCE_NOTES.md)

---

## A. 아이디어

**한 줄 요약** — H2P branch의 backward slice 중 **critical path에 속한 명령어만** 골라, 그 위의
명령어에는 issue priority를, 그 위의 예측 가능한 load에는 RF prefetch를 준다.

### A0. 왜 critical path만인가

`cmp r1, r2` 처럼 branch의 producer가 둘이면, branch가 언제 resolve되는지는 **늦게 도착하는
쪽**이 정한다. 일찍 도착하는 쪽을 아무리 가속해도 branch resolution 시점은 그대로다.
PUBS·TEA는 slice 전체를 가속 대상으로 삼지만, 그중 실제로 resolution을 앞당기는 것은
critical path 하나다. 나머지는 priority 자원과 predictor 용량만 소모한다.

지난 측정이 이를 뒷받침한다 — 무제한 marking(892.7M 후보)과 20% partition의 성능 차이가
거의 없었다. 표시된 것 대부분이 애초에 branch ready 시점을 못 움직였다는 뜻이다.

### A1~A6. 여섯 개 핵심 로직

| # | 로직 | 하는 일 | 동작 시점 | 구조 | 출처 |
|---|---|---|---|---|---|
| **A1** | H2P 판별 | branch PC별 misprediction 이력으로 "자주 틀리는 branch" 선별 | branch execute (갱신) / 소비는 아래 | **HBT** — 3-bit saturating counter, counter>1이면 H2P, 50K retire마다 decay | TEA HBT (= PUBS conf_tab과 동형) |
| **A2** | **critical producer 찾기** | 어떤 명령어의 여러 source 중 **가장 늦게 ready된 source**와 그 producer를 식별 | wakeup (관찰) → issue (확정) → commit (사용) | **PRF scoreboard** (preg → producer PC) + **RSE의 LPR 필드** + **LPR tracker** (ROB entry별) | **본 연구 고유** |
| **A3** | critical chain 멤버십 | commit 때 "내가 slice 소속이면 내 critical producer도 slice"를 한 단계씩 전파 | commit | **brslice_tab** (PC → owner branch pointer) → HBT | PUBS 방식, physical register 기반으로 변경 |
| **A4** | Target load 판정 | critical chain 위의 load 중 주소가 규칙적인 것 선별 | rename (조회) / retire (학습) | **Prefetch Table** — PC별 stride predictor | RFP 논문 |
| **A5** | priority scheduling | critical chain 명령어를 IQ head 쪽 예약 entry에 배치해 먼저 issue | dispatch (배치) → select (실현) | **P-IQ** — 유한 partition + non-stall fallback | PUBS |
| **A6** | RF prefetch | 예측 주소의 데이터를 load의 destination physical register로 미리 가져옴 | rename (발사) → L1 probe → AGU (검증) | **PT + RFP Queue + validate-then-use** | RFP 논문 |

### A2 상세 — LPR (Last Producer Register) 추적

그림의 흐름 그대로:

```
rename   : destination preg 할당 → PRF scoreboard[preg] = 내 PC
wakeup   : source별로 producer tag가 broadcast되어 M bit set → DELAY shift → R bit
           R이 가장 마지막에 선 source의 preg를 RSE의 LPR 필드에 래치
issue    : LPR을 ROB entry(LPR tracker)로 고정
commit   : brslice_tab[내 PC] 조회 → slice 소속이면
           LPR → PRF scoreboard 역참조 → producer PC 획득
           → brslice_tab[producer PC].owner = 내 owner branch    (한 단계 전파)
```

PUBS와의 차이 셋:

1. **logical → physical register.** rename이 WAW를 이미 해소했으므로 항상 실제 dynamic
   dataflow의 producer를 얻는다. PUBS의 def_tab은 logical 기반이라 다른 control-flow
   경로의 stale producer를 가리킬 수 있다.
2. **decode-time → commit-time 학습.** on-path 명령어로만 학습된다. wrong-path 오염이
   구조적으로 불가능하다 (committed consumer의 source producer는 반드시 committed).
3. **모든 source → last-ready source만.** 이것이 critical path 필터.

전파 cadence는 PUBS와 같다 — dynamic instance 하나당 한 level. 깊이 D인 chain은 그 코드가
D번 재실행되며 연결된다.

### 설계상 알려진 성질

**walk가 저절로 끝난다.** dispatch 시점에 모든 source가 이미 ready였던 명령어는 LPR 이벤트가
없으므로 전파하지 않는다. 즉 전파는 operand wait이 실재하는 경계에서 멈춘다 — 가속해도
효과가 없는 지점에서 정확히 종료된다. Load에도 같은 규칙이 적용된다: 주소는 일찍 나왔고
자기 memory latency가 병목인 load는 더 전파하지 않고 그 자리에서 RF prefetch 대상이 된다.

**store→load memory dependence는 추적 불가.** PRF 기반이라 register edge만 따라간다.
value chain이 load에서 끊기지만, 그 load는 leaf로서 priority/RFP가 맡고 address-generation
chain으로는 계속 전파된다. PUBS도 같은 한계를 갖는다.

### 미결정 — 측정으로 정할 것

| 항목 | 무엇을 정하나 | 근거가 될 측정 |
|---|---|---|
| **Δ-window** | `|t_last − t_second| < Δ`면 양쪽 producer 모두 삽입 (공동 critical). Δ=0이면 순수 last-producer, Δ=∞면 PUBS full slice | slack 분포 |
| **retention counter** | brslice_tab entry가 critical로 재확인된 빈도. threshold 이상만 priority 적용, decay로 옛 path 자연 소멸 | argmax 안정성 |
| **삽입 게이트** | 일단 **전 branch 삽입**으로 시작(비-H2P도 H2P가 될 수 있으므로), 이후 {counter≥1, H2P-only}로 좁혀가며 sweep | entry 점유 구성비, H2P-owned 축출률 |
| **owner 충돌** | 한 PC가 두 H2P branch의 slice에 동시에 속할 때 pointer가 ping-pong. 빈도 측정 후 대처 | owner overwrite 이벤트 |

---

## B. 현재 코드 상태 ↔ 아키텍처 매핑

| 아키텍처 요소 | 상태 | 코드 |
|---|---|---|
| **A1** H2P 판별 (HBT) | ✅ **그대로 재사용** | `bp/hbt.c` — 1024 entry, 3-bit, threshold>1, decay 50K |
| **A5** P-IQ (partition + non-stall fallback + priority select) | ✅ **재사용, 입력만 교체** | `node_issue_queue.cc`, `exec_ports.c` — priority bit의 출처가 Block-Cache mask → brslice_tab lookup |
| **A6** RF prefetch (PT, queue, L1 probe, validate-then-use, fill path) | ✅ **재사용, 입력만 교체** | `zereco/rfp.c` — PT allocation 권한이 backward walk → critical-chain load 판정 |
| H2P resolution 4단계 profiler | ✅ 그대로 재사용 | `zereco/h2p_mispred_latency.c` |
| **A2** LPR 추적 (PRF scoreboard, LPR 래치/tracker) | ❌ **신규 구현 필요** | 관찰 지점은 존재 — 아래 참조 |
| **A3** brslice_tab (chain 멤버십, owner pointer, retention counter) | ❌ **신규 구현 필요** | — |
| 구 identification (Fill Buffer snapshot + 500-cycle batch walk + Block Cache mask + frontend tagging) | 🗑 **폐기** — A2/A3가 대체 | `fill_buffer.c`, `dependency_chain_cache.c`, `decoupled_frontend.cc` 태깅 |

### 신규 구현이 얹힐 지점 (코드 검토 결과)

| 필요한 것 | 시뮬레이터에 이미 있는 것 |
|---|---|
| destination preg id | `op->dst_reg_id[][]` — rename에서 확정 |
| source preg id | `op->src_reg_id[][]` |
| **모든 wakeup 이벤트가 지나는 단일 지점** | `cmp_model.c: cmp_wake_op()` — 여기서 `rdy_cycle = MAX(rdy_cycle, src_op->wake_cycle)` 계산. **argmax를 함께 기록하면 그것이 LPR** |
| source 인덱스 | wake hook의 `rdy_bit` 인자 |
| dependence 종류 (register / memory) | `src_info->type` (`REG_DATA_DEP` / `MEM_DATA_DEP`) |
| operand wait 유무 | `rdy_cycle <= issue_cycle` 이면 대기 없음 = 전파 종료점 |

> ⚠ **주의 — wake 이벤트 경로가 둘이다.** `cmp_wake_op()`는 consumer가 아직 RS에 없으면
> `rdy_cycle`만 갱신하고 **early return** 한다. `simple_wake()`만 계측하면 RS 진입 전에 도착한
> wake를 전부 놓친다. 계측은 반드시 early return **앞**에 둔다.

### 참고 — 이전 프로토타입

`critpath-test` 브랜치 (`24fabb7`)에 구 Fill-Buffer walk 위에 올린 시도가 보존되어 있다.
`zereco_critical_slack_cycles`, `zereco_iq_priority_critical_only`, `rfp_target_critical_only`
파라미터가 정의되어 있어 이름 규약을 맞출 때 참고한다. 구조는 재구현한다.

---

## Phase A — 계측 (진행 중)

타이밍을 바꾸지 않는다. **baseline과 cycle-identical** 해야 하며 그것이 검증 조건이다.

| # | 측정 | 정하는 것 |
|---|---|---|
| 1 | **slack 분포** — `t_last − t_second` 히스토그램 | Δ 값 |
| 2 | **argmax 안정성** — static PC별 LPR source 인덱스가 instance 간 바뀌는 비율 | retention threshold·decay |
| 3 | **frontier 비율** — operand 대기 없이 dispatch된 명령어 비율 | 전파 종료가 걸리는 지점 |
| 4 | **전파 깊이** — H2P branch로부터 몇 level까지 도달하는가 | warm-up 비용 |

1·3은 op 필드와 카운터만으로 된다. 2·4는 관찰용 테이블이 필요하다 (PC→직전 LPR,
preg→producer PC). 두 테이블은 **계측 scaffolding이며 모델링된 하드웨어가 아니다** —
Phase B에서 실제 구조로 승격된다.

부수 소득: `MEM_DATA_DEP`가 LPR인 비율을 함께 세면 **memory dependence를 추적하지 못해
잃는 몫**이 정량화된다.
