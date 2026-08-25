# ZERECO Timed RFP — 구현·실험 현황

> Last updated: 2026-08-19 (경량판 — Phase 0~4 구현 완료 시점에 v5 계획서 1,067줄을 축약.
> 상세 설계·의사코드·유도 과정의 원문은 git 이력(e962cfa 이전)과 구현 코드
> [rfp.h](rfp.h)/[rfp.c](rfp.c)의 주석이 대체한다)
> 근거 논문: `[2022, ISCA] Reg File prefetching.pdf` / 관련: `zereco_ARCHITECTURE.md`,
> `zereco_REFERENCE_NOTES.md` §1·§5

---

## 1. 아키텍처 ↔ 구현 지도

ZERECO datapath 전 컴포넌트의 구현 상태. **전부 timed 모델**이며 oracle 경로
(`h2p_chain_perfect_load`)와 상호 배제된다.

| 컴포넌트 (hardware 의미) | 상태 | 코드 |
|---|---|---|
| **H2P Branch Table** — retire-time misprediction 추적, 1024-entry 3-bit saturating counter, 50K-retire decay | 완료 (TEA 재활용) | `bp/hbt.c` |
| **Retired Instruction Window + Backward-Walk Engine** — 512-uop Fill Buffer에서 committed backward slice 복원 (walk 중 retire drop = sampled training) | 완료 | `fill_buffer.c`, `dependency_chain_cache.c` |
| **Block Metadata Cache + frontend tagging** — 다음 dynamic occurrence의 op에 chain/priority bit 부착 | 완료 | `dependency_chain_cache.c`, `decoupled_frontend.cc` |
| **Target-Load Address Predictor (Prefetch Table)** — load-PC indexed 1K×8-way; base+stride×inflight; 1-bit confidence(1/16 확률 증가); walk가 할당 권한(=criticality 필터), retire가 주소 학습 | 완료 | `rfp.c` PT 절 |
| **Prefetch packet 생성** — rename 직후(prfid 확정 시점) launch; in-flight counter alloc++/commit−−/squash−−; 진짜 store dependence 보유 load는 abstain | 완료 | `rfp_rename_launch` (map_stage 훅) |
| **RFP Queue + L1 port 중재** — 64-entry FIFO; 기본 최저 우선순위(demand가 남긴 port만), 전용 port/선점 모드 별도 | 완료 | `rfp_queue_drain` (dcache 훅 2곳) |
| **Validation & RF delivery** — load의 AGU(=첫 dcache 시도)에서 주소 비교; covered load는 cache 재접근 없이 load-to-use 1 cycle로 완료, dependent wakeup | 완료 | `rfp_try_validate` |
| **L1-miss → lower-level fill** — miss한 probe를 demand miss처럼 하위 계층으로 (MSHR 점유·coalescing·pollution 모델링 포함) — **기본값** | 완료 | `rfp_send_to_lower_levels`, `rfp_fill_done` |
| **RF-coverage feedback → slice filtering** — covered slice의 priority 억제 (streak 2) | 완료 — **단 실험 결과 기각 후보 (§5)** | `dependency_chain_cache.c` |
| **Distributed P-IQ** — slice-wide priority marking + select 우선 + 유한 partition(비율 예약) + non-stall fallback | 완료 | `node_issue_queue.cc`, `exec_ports.c` |
| **계측** — H2P resolution 4-stage 분해 profiler / RFP 깔때기·대역폭·간섭 / stale-value corner-case 검출 | 완료 | `h2p_mispred_latency.c`, `zereco.stat.def` (~200 stat) |

**모델링 대체 (하드웨어 ≠ 시뮬레이터, 의도적 — 근거는 §7 논문 서술 지침):**

| 하드웨어 요소 | 시뮬레이터 |
|---|---|
| RFP-inflight bit + speculative wakeup 정렬 (RS entry×1b) | `done_cycle` 산술로 등가 — Scarab wakeup이 "결과 확정 후 미래 ready cycle 통보"라 정렬이 불필요. 논문 mechanism·storage에는 원형 기술 |
| store→prefetch forwarding + MD predictor + flush | **abstain** (launch 시 `MEM_DATA_DEP` 검사 + probe/validate `scan_stores`). Scarab은 rename에서 oracle mem-dep을 걸어 MD 오판이 구조적으로 불가 |
| PAT storage 압축, DTLB-miss drop, context predictor | 생략 (timing 무관 / 모델 없음 / +0.3%) — storage 계산에만 인용 |
| off-path covered fast path | 미적용 (oracle과 동일 기준; funnel 통계는 on-path만, 대역폭 통계는 전체) |
| `rfp_walk_bootstrap` | 미구현 — `rfp_init` assert가 명시 거부 |

**최종 파라미터 기본값** (`core.param.def`): PT 1024/8-way, conf 1-bit(p=1/16), queue 64,
drain 2, hit latency 1, `scope 0`(Target Load), **`l1_miss_policy 1`(하위 진행)**,
port `priority 0`(잔여)·`fail 0`(wait), `covered_min_saved_cycles 1`, `launch_offpath 1`,
`probe_updates_repl 1`, `stale_value_check 1`.

---

## 2. 확정된 설계 결정 대장

| 결정 | 근거 (실측) |
|---|---|
| 훈련 2단 분리: **walk = Target-Load PC 선별(PT 할당 권한), retire = 주소 학습** | walk 단독 훈련은 sampled stream이라 `base+stride×inflight` 점화식이 깨짐 (snapshot 경계 delta ≈ k×stride). PC 멤버십은 집합 판정이라 sampling에 강인 |
| scope 필터 = **PT 멤버십** (chain_bit 아님) | Block-Cache miss로 인한 훈련 누락 제거. chain_bit은 계측·P-IQ 전용 |
| store 처리 = **abstain** | validation-시점 store conflict 실측 1.4K~2.1K / 347M — launch-시점 dep 검사가 창을 닫음. 포기 모집단은 Target Load의 10.7% (즉시 forwarding 가능분은 그중 12%) |
| covered 판정: **latency 이득은 항상 취득**, `covered_min_saved_cycles`는 `zereco_rf_covered` 주장(=P-IQ filtering 입력)만 가름 | threshold가 이득까지 버리면 이미 RF에 있는 데이터를 두고 재접근하는 모순 (Phase 3 재실행으로 확인) |
| **L1-miss는 하위 계층 진행이 기본** | drop +1.75% vs 진행 +5.90% (L1D read miss 126.0M→95.8M). RFP 논문 §3.2.2의 원 설계이기도 함 |
| off-path launch 유지, confidence gating **폐기** | probe 66% 억제 오라클(onpath)이 IPC 완전 동일 — wrong-path prefetch의 측정 가능한 비용 없음 |
| dedicated L1 port **불채택** (negative result로 인용) | coverage +1.9%p에 IPC +0.03%p — L1 대역폭은 제약이 아님 (read port 유휴 57~67%) |
| probe가 replacement 갱신 (`repl 1`) | covered load가 자기 access를 생략하므로 probe가 그 demand touch를 대체 — L1 접근 수 보존(논문의 bandwidth-neutral 주장) |
| queue 64·수명 무제한 유지 | 평균 점유 1.35/64, full 0.06% cycle — queue는 제약이 아님 |
| descriptor는 baseline 문자열 파싱→덮어쓰기→재직렬화 | append는 중복 인자를 만들어 파서 순서에 결과가 좌우됨 |

---

## 3. 검증 대장

| 검증 | 결과 |
|---|---|
| Phase 1 (profile-only): 3 config × 68 SimPoint cycle-identical + 기존 baseline과 일치 | ✓ (reg_vector 확장도 timing 중립 입증) |
| Phase 2 (queue+probe, 유휴 port·상태 불변): baseline과 cycle-identical | ✓ |
| Phase 2 상한-실측 gap 분해: 손실 채널 합으로 설명 | ✓ 미설명 0.08% |
| Phase 3 재실행: 무변경 4 config 비트 단위 재현 | ✓ |
| 총체 점검(2026-08-17) 수정 4건의 timing 중립성: 260818의 baseline·rfp_only가 Phase 3와 비트 단위 일치 | ✓ |
| 무결성 카운터 6종 (shadow-selection / physical-entry / partition / non-stall / demand-delayed / stale-deref) | ✓ 전 실험·전 config 0 |
| 스모크 3건 (piq_partition, filtered_randq, partition_randq — 미실행 조합 사전 검증) | ✓ |
| **미완**: offline replay 교차검증(원 §5.7) / stale 검출 byte-granular 정제 | 보류 (§6) |

총체 점검(2026-08-17)에서 잡은 결함 4건: stale 검출기 미구현(구현), policy-0에서 chain 계측
사망(태깅 게이트에 `RFP_ENABLE` 추가), walk_bootstrap 조용한 무시(assert), 죽은 stat(삭제).
Phase 3 첫 실행 결함 2건: `_l2` line-size assert(수정), threshold가 이득 폐기(수정).

---

## 4. 실험 기록

| 실험 | 내용 | 핵심 산출 |
|---|---|---|
| `260815_RFP_baseline_oldest_first` | Phase 1 profile-only | 예측기 특성 |
| `260816_RFP_phase2_oldest_first` | Phase 2 대역폭·timeliness | 깔때기 실측 |
| `260817_RFP_phase3_oldest_first` | Phase 3 fast path + 축별 오라클 | RFP 단독 성능 |
| `260818_RFP_piq_integration` | RFP+P-IQ 첫 결합 (OF/randq) | 사다리 + filtering |
| `260819_RFP_piq_axes` | randq 분해 + partition 축 완성 | 제안 설계 확정 |

### 4.1 예측기·깔때기 (Phase 1~2, all-loads 분모)

| | scope 0 (Target Load) | scope 1 (전체) |
|---|---:|---:|
| Target Load 비율 (PT-hit) | 59.2% | 94.0% |
| 주소 정확도 (correct/launched) | 92.6% | 95.1% |
| injected → executed → useful | 28.9 → 18.3 → 16.9% | 46.6 → 26.6 → 25.1% |
| coverage 상한 (∞ 대역폭·timeliness) | 26.8% | 44.4% |
| 상한 도달률 (Phase 2) | 63.1% | — |
| **PT 할당 / 축출** | **155K / 134K** | **18.9M / 18.9M (스래싱)** |

- FULL:PARTIAL = 1:2 — **예측 가능성 ↔ run-ahead 역상관**: strided load는 operand가 일찍
  ready라 rename→AGU가 ~4.6cy뿐(FULL은 ~23.8cy). launch→probe는 2.5cy
- L1 read port 사용률 33% (demand) — 유휴 2/3. off-path probe 68.8%지만 baseline 기계 자체가
  off-path 58.9%라 초과분은 ~10%p
- lower-level 진행분: 요청의 96%는 load가 fill보다 먼저 도착(슬롯 회수) — miss 경로는
  사실상 **순수 cache prefetch**로 동작

### 4.2 성능 사다리 (Phase 3~4 통합)

```
[OLDEST-FIRST]                 IPC      f→res      dep     br-select
  baseline                      —      42.02cy   29.94cy    0.10cy
  + RFP                      +5.90%    37.52     25.50      0.10
  + P-IQ 상한(무제한 mark)   +6.43%    37.28     25.20      0.07
  + P-IQ 20% partition       +6.15%    37.38     25.30      0.09

[RANDOM QUEUE(PUBS 대표 조직)] (vs baseline_randq; OF-baseline 대비는 괄호)
  baseline_randq                —      45.76     31.07      2.42     (−4.02%)
  + RFP                      +6.75%    40.05     26.17      1.66     (+2.46%)
  + P-IQ 상한                +9.40%    37.96     25.12      0.53     (+5.01%)
  + P-IQ 20% partition       +8.95%    38.24     25.35      0.63     (+4.57%)
```

- P-IQ 순수 몫: OF +0.46%(상한)/+0.24%(partition), randq **+2.48%/+2.06%** —
  partition이 상한의 **83%**(randq) / 52%(OF) 보존
- L1-miss 진행이 RFP 이득의 지배 성분: +1.75%(drop) vs +5.90%(진행), L1D miss −24%
- claimed saved cycles의 13%만 총 cycle 감소로 실현 (OoO 흡수 + 이득은 해당 branch가
  실제로 틀린 경우에만 실현) ⇒ **1차 지표는 H2P resolution latency**
- covered load의 93.1%가 H2P slice 소속 (선별이 의도 모집단 적중)
- stale-value 창: useful의 8.6% — 단 **line(64B)-granular 상한** (byte 겹침 아님, §6)

### 4.3 Filtering 대조 (기각 근거)

| OF, 20% partition | 후보 | fallback률 | part-full cyc | IPC |
|---|---:|---:|---:|---:|
| unfiltered (chain 전부) | 892.7M | 11.1% | 78.4M | **+6.22%** |
| filtered (covered slice 제외) | 811.4M (−9.1%) | 11.4% | 76.2M | +6.15% |

후보를 9% 덜어내도 fallback률·partition-full이 불변 — partition 압력은 국소적(bursty)이라
전역 인구 감소가 병목을 안 풀고, **non-stall bounded partition 자체가 인구 자기조절**을
수행한다(넘치면 priority 회수). filtering은 metadata 비용만 추가.

### 4.4 간섭 (limitation 재료)

- pr (DRAM BW-bound): OF에서 P-IQ가 −1.3%p — chain 우대가 MLP 생성 독립 miss load를
  select에서 밀어냄 (normal displaced ~80M op-cycle). randq에서는 미발생.
  partition/filtering으로도 안 풀림 — PUBS 자인("scheduling은 memory latency를 못 없앰")의 실측판

---

## 5. 발견 정리 — 사전 예상 대비

**예상과 일치 (설계 검증):**
1. H2P resolution의 지배 성분은 dependency (29.94/42.02 = 71%) — thesis 그대로
2. "branch 자체에만 priority는 너무 늦다"(ARCHITECTURE §2.3): branch의 select 대기 실측
   0.10cy — slice-wide marking의 이득은 전부 dependency 단계(producer 조기 issue)로 발현
3. P-IQ headroom은 age 중재가 없는 조직(random queue)에서 발생 — PUBS 이해와 일치
4. Target-Load 선별의 predictor 효율: 동등 성능에 PT 압력 **122×** 차이
5. validate-then-use라 오예측 비용 = 낭비 probe뿐, demand delay 구조적 0

**예상과 다름 (방향 수정 유발):**
1. **cache-fill 경로가 RF 경로보다 3.4× 큼** — "cache prefetch로는 불충분" motivation과
   긴장 ⇒ "criticality 예측기 하나, 전달 경로 둘(L1-hit→RF, L1-miss→fill)"로 재구성 필요
2. **대역폭·wrong-path 모두 비제약** — gating/전용 port 계획 폐기, negative result화
3. 예측 가능성 ↔ run-ahead **역상관** (strided load일수록 operand가 일찍 ready) —
   RF 경로 이득이 load당 평균 2.7cy(최대 4)에 구조적으로 묶임
4. **RFP의 2차 효과**: RS 점유 완화가 select 대기까지 단축 (randq에서 2.42→1.66cy) —
   RFP 몫이 randq에서 더 큼
5. **RF filtering 존재 이유 상실** (§4.3) — bounded partition이 filtering을 포섭
6. partition 보존율의 스케줄러 의존성: randq 83% vs OF 52%
7. IPC는 약한 대리 지표 (실현률 13%) — H2P resolution latency가 1차 지표

---

## 6. 미결정 사항 · sweep 목록

**결정 필요 (논문 구조 직결):**

| # | 사항 | 선택지 / 현재 기본안 |
|---|---|---|
| D-1 | **RF filtering 거취** | (a) ablation으로 기각 보고 — bounded partition이 포섭 (권고) / (b) per-load 등 개선 시도 (상한 ~0.2%p) / (c) oracle-coverage에서 재확인. 기각 시 `zereco_rf_covered` 피드백 루프와 `covered_min_saved_cycles`의 소비자가 사라짐에 유의 |
| D-2 | 논문 서사 확정 | 단일 randq 사다리 채택 여부 / motivation "경로 둘" 재구성 / 1차 지표 = H2P resolution |
| D-3 | store-forwarding case 2·3 보류건 | 모집단 10.7%·즉시 가능 12% — 이득 상한 작음. 채택/기각 |
| D-4 | stale 검출 byte-granular 정제 | 현재 8.6%는 line-granular 상한 — 논문 게재 시 정제 필요 |
| D-5 | offline replay 교차검증 (원 §5.7) | 검증 편의 항목 — 수행/생략 |

**Sweep 필요 (optimal 지점):**

| # | 축 | 값 | 목적 |
|---|---|---|---|
| S-1 | **PT 크기 × scope** | 128/256/512/1K/2K/4K × {0,1} | complexity 주장의 본 그림 — "vanilla가 몇 K를 써야 ZERECO 1K(이하)와 같아지나". gcc(scope 0에서도 97% 축출) 주목 |
| S-2 | confidence 폭 | 1~4 bit | 정확도↔coverage (논문 Fig 17 대응) |
| S-3 | **partition 예약률** | 10/15/20/25/50%, randq 축 | 제안 설계의 예약 크기 — 20%에서 fallback 12%·상한의 83% |
| S-4 | `covered_min_saved_cycles` | 1~4 | D-1에서 filtering 생존 시에만 의미 |
| S-5 | SimPoint 확대 | 전체 SimPoint, **weight 가중 집계** | 효과 기준 선별은 금지 — weight 가중이 정직한 확장 |

---

## 7. 논문 서술 지침 (보존)

1. **Mechanism 절은 하드웨어 원형으로**: load consumer의 speculative wakeup이 있는 현대적
   baseline을 가정하고 RFP-inflight bit(RS entry×1b, storage 표 포함)와 wakeup 정렬을 원형
   기술. Methodology에 "시뮬레이터는 비투기적 wakeup이므로 steady-state 타이밍을 완료 시점
   등가로 모델링, baseline과 동일 규칙이라 공정"을 한 문단 명시
2. **Random-physical IQ = PUBS가 현대 대표 조직으로 채택·정당화한 baseline** — 인위적
   sensitivity 도구가 아님 (REFERENCE_NOTES §5.2). partition의 하드웨어 서술은 PUBS
   **free-list 분할**(low-ID 구간=priority, select 무수정), 시뮬레이터는 그 선택 결과를
   counter-partition+비교기로 모델링
3. **인용 포인트**: RFP 논문 §6이 CRISP를 두고 *"RFP is complementary to these works...
   combining RFP with criticality-based solutions can further reduce end-to-end pipeline
   latency for critical loads and their load slices"* — 저자들이 직접 ZERECO 방향을 열어둠.
   §5.1도 targeted prefetching을 future work로 명시
4. **RFP 논문이 보고하지 않아 우리가 처음 재는 수치**: store-dependent prefetch 모집단
   (10.7%), stale-value 창, wrong-path prefetch 대역폭의 실비용(=0), PT 압력의
   criticality-선별 효과(122×)
5. RFP 논문 대조 수치 (sanity 앵커): coverage 43.4%/오예측 ~5%/injected 72→executed
   48→useful 43.4/full-hide 34.2+partial 9.2/PT 1K 충분/L1-miss 진행 +0.02%(우리는
   +4.15%p — 워크로드 차이)/dedicated port +0.9%p(우리는 +0.03%p)

---

## 8. 방향 전환 (2026-08-21): critical-path 선별 oracle

**동기.** Backward walk는 reachability 폐포를 계산하므로 on-path op의 56.7%가 chain bit를
달았고(2026-08-20 실측, `260819_RFP_piq_axes`), 그 원인의 하나로 **TEA §IV-C의 Block Cache
bit-mask 주기 초기화(500K instruction)가 구현에서 누락**되어 있었다
(`periodically_reset_caches()`가 정의만 있고 호출처 없음 → mask가 OR로만 영구 누적).
새 방향: **H2P resolution을 실제로 지연시킨 last-arriving chain만** 선별해 RFP/P-IQ에
적용했을 때의 headroom을 hardware 설계 이전에 oracle로 측정한다.

**Oracle 방법.** RIW/snapshot entry는 retire 시점 Op 전체 사본이므로 timing이 이미 있다.
비교 키는 `op->wake_cycle` — map.c가 consumer의 `rdy_cycle`을 `MAX(producer wake_cycle)`로
만드는 바로 그 값(load는 dcache가 실제 데이터 반환 cycle을 기록; miss/fill 포함).
walk가 trigger H2P에서 시작해 각 op의 producer(레지스터: youngest older writer, load:
committed VA 일치 store) 중 **wake_cycle이 가장 큰 쪽만** 따라간다. x86 flag hop을 거쳐
cmp의 두 src 비교로 내려간다. critical set ⊆ reachability slice 보장(slice 밖 producer는
따라가지 않음). 순수 시뮬레이터 oracle이며 hardware 실현성은 의도적으로 범위 밖.

**추가된 param (전부 기본값이 기존 동작 보존, reset만 기본 ON):**

| param | 기본 | 의미 |
|---|---|---|
| `zereco_block_mask_reset_interval` | **500000** | TEA §IV-C mask 초기화 주기. 0 = 이전(누적) 동작 |
| `zereco_iq_priority_critical_only` | 0 | IQ priority 후보 = critical chain만 |
| `rfp_target_critical_only` | 0 | Target Load 판정(PT allocation·slice coverage 판단) = critical load만 |
| `zereco_critical_slack_cycles` | 0 | last-arrival에서 이 cycle 이내 producer도 추적(동률은 항상) |

**추가된 stat:** `ZERECO_WALK_SLICE_DEP_OPS` / `CRITICAL_OPS`(+PORTION),
`WALK_TARGET_LOADS_SEEN` / `CRITICAL_TARGET_LOADS`(+PORTION), `CRITICAL_STEPS` /
`CRITICAL_MULTI_FOLLOW`, `ZERECO_BLOCK_MASK_RESETS`. Critical 계산·계측은 knob과 무관하게
매 walk 수행(동작 변화는 knob 켠 경우만).

**실험 사다리 (`zereco_dbg.json`, randq 축):**

실험명 `260821_critical_path_oracle`, 인접 config 간 diff = 변인 하나 (json diff로 검증).

| config | 변인 |
|---|---|
| `rfp_piq_unfiltered_randq_noreset` | 없음 — 이전 동작 재현 (same-binary 기준점; randq+policy 1 조합은 과거 기록에 없음) |
| `rfp_piq_unfiltered_randq_reset` | + BMC 500K reset (chain 56.7% → ? 측정; critical portion stat 동시 산출) |
| `rfp_piq_critical_randq` | + critical-path 선별 (RFP+P-IQ 동시 — 사용자 실험 1) |
| `rfp_piq_critical_filtered_randq` | + RF-covered slice priority 제외, policy 2 (사용자 실험 2 = D-1 재평가) | 유의: critical 선별은 fill path의 volume
이득(L1D miss −24%)을 깎을 수 있음 — RFP·P-IQ knob이 분리된 이유.

### 8.1 결과 (`260821_critical_path_oracle`, 68 SimPoint, randq 축)

무결성 6종 0. reset tick 500,180 inst/회(HBT decay 50,003과 동일 기준)로 검증.

| | noreset | reset | critical | crit+filt |
|---|---:|---:|---:|---:|
| chain-bit % (on-path op) | 56.83 | 47.46 | 47.43 | 47.46 |
| priority % | 56.83 | 47.46 | 44.25 | 39.36 |
| **f→resolution (cy)** | 37.93 | 37.98 | **37.90** | 38.45 |
| dependency | 25.46 | 25.51 | 25.44 | 25.52 |
| br-select | 0.18 | 0.18 | 0.18 | **0.65** |
| IPC vs noreset | — | −0.06% | **+0.02%** | −0.64% |
| PT alloc | 55,604 | 55,447 | **33,252** | 33,891 |
| PT refresh | 91.7M | 91.7M | **45.0M** | 45.1M |
| RFP injected / useful | 49.6M/33.7M | 49.6M/33.7M | 47.9M/32.2M | 47.8M/32.2M |

**walk 형상 (4개 config 동일):** slice 29.97 op/walk, critical **22.53 (75.2%)**, Target Load
3.52 → critical **1.73 (49.1%)**, multi-follow 0.59%.

**해석 4건:**

1. **slice는 이미 거의 선형 chain** (분기 계수 1.33). "두 src 중 한쪽만 critical" 가정은 op
   기준으로 성립하지 않는다. 다만 **load 기준으로는 49%만 critical**이라 선별의 실질 내용은
   load 쪽에 있다.
2. **iso-performance에서 학습량 절반.** full-slice → critical-only에서 f→r가 오히려 0.08 cy
   낫고 PT allocation은 40% 준다. 사다리가 평평한 것은 "이득 없음"이 아니라 **"전체 slice를
   찾을 필요가 없다"**는 결과다.
3. **상한은 표현에 묶여 있다.** walk에서 Target Load 51%를 걷었는데 실제 발사는 −3.5%뿐 —
   BMC mask OR 누적 + per-PC PT membership이 per-occurrence 판정을 합집합으로 접는다(20:1).
   다음 단계는 walk 알고리즘이 아니라 criticality를 dynamic instance에 결합하는 것.
4. **D-1 기각 확정.** critical 모집단에서 RF filtering은 br-select 0.18 → 0.65 cy, IPC −0.66%.
   이전 사이클의 "중립"에서 "명백한 손해"로 바뀌었다.

**scheduling 축 종료 근거:** br-select 실측 0.18 cy — 완벽한 선별로도 회수할 예산이 없다.
P-IQ는 secondary로 고정한다.

### 8.2 선행연구 — CRISP (ASPLOS'22)

`reference/[2022, ASPLOS] CRISP; Critical Slice Prefetching.pdf`가 최근접이다. §3.5에
"slice가 RS를 채우면 우선순위 여지가 없다 ⇒ critical path 위 명령어만 승격"이 명시되어 있고,
DAG leaf→root aggregated path latency(load는 AMAT)로 criticality를 산출하며, **branch slice도
이미 다룬다**(단독 3%+ IPC). 평가는 Scarab + RAND/age-matrix scheduler. 8.4% avg / 38% max.

⇒ "critical path 선별"만으로는 novelty가 없다. 주장은 두 축으로만 세운다.

1. **Runtime 추출.** CRISP는 profiling → trace(100M inst/5GB) → offline 분석 **~100초** →
   post-link binary rewrite(**새 instruction prefix = ISA 변경**) → 재배포. ZERECO는 committed
   retire-side 상태만으로 walk 후 곧바로 다음 dynamic instance에 적용. CRISP §3.5의
   "hardware는 critical path analysis를 할 수 없다"는 명제의 반례가 우리 자리다.
2. **전달 경로.** CRISP는 scheduling 전용. RF prefetch(L1-hit→RF, L1-miss→fill)는 없다.
   논거는 정량("CRISP 이득이 작다" — 성립하지 않음)이 아니라 기계적("scheduling은 아직 없는
   데이터를 만들 수 없다", PUBS 자인)으로 쓴다.

### 8.3 실험 계획 (2026-08-21 확정 — 이전 full-slice 계열 sweep S-1~S-5는 우선순위에서 제외)

방향의 성패는 세 주장 중 무엇이 서느냐로 갈린다.
**(가) efficiency** 동일 성능·적은 하드웨어 / **(나) novelty** runtime 추출이 offline 고정보다
낫다(CRISP §3.5 반박) / **(다) performance** 절감분 재투자로 실제 이득.
각 실험에 **중단 기준**을 명시한다.

#### Phase 0 — 하드웨어 실현 경로 (**oracle을 대체하는 필수 관문**)

현재 `build_critical_path_mask_for_target()`은 retire 후 `wake_cycle`을 비교하는 **순수 oracle**이며
실제 RIW가 가질 수 없는 상태를 읽는다. 아래 기구로 대체되지 않으면 이후 결과는 논문에 못 쓴다.

**기구: last-arriving edge 기록.** "어느 src가 마지막이었나"는 wakeup에서 **공짜로 알 수 있다** —
consumer를 ready로 만든 broadcast가 정확히 그 producer이기 때문이다. Timestamp도 비교기도 필요없다.

| 단계 | 동작 | 비용 |
|---|---|---|
| wakeup | consumer의 ready 시각을 **올린** producer의 tag를 latch (`simple_wake`의 `MAX2`가 이미 그 비교를 한다) | ROB/IQ entry당 tag 1개(512 ROB → 9~10 bit). 새 비교기 0 |
| retire | `delta = my_op_num − producer_op_num`을 RIW entry에 적재. retire 순서 = window 순서이므로 delta가 곧 window offset | RIW 16 B → ~18 B (+12%) |
| walk | timestamp 비교가 사라지고 **포인터 체이싱** `idx −= delta`. producer가 window 밖이면 delta > 512 → chain 종료 | O(chain length) ≈ 22 step |

Store→load forwarding은 LSU가 forwarding store를 알고 있으므로 같은 필드로 기록된다. Cache에서
온 load는 in-window producer가 없어 chain이 거기서 끝나는데, 이는 **그 load가 곧 Target Load**라는
뜻이라 의미론이 정확하다. x86 flag도 별도 처리 없이 같은 경로를 탄다.

**부수 이득 (complexity 서사에 직접 기여).** 이 방식은 live-in bit vector도, 512-entry backward
스캔도 필요 없다. BWE의 500-cycle 예산이 크게 줄어 **identification의 sampling 손실(§5.3의 gating)도
완화**된다. 즉 하드웨어 버전이 현재 oracle보다 *싸다*.

| | E0: oracle → 실현 기구 교체 후 재현 |
|---|---|
| 방법 | `Op`에 last-arriving producer tag 추가(`map.c:simple_wake`에서 `MAX2` 갱신 시 기록), retire에서 delta 적재, walk를 포인터 체이싱으로 교체. oracle 버전과 선별 결과를 **직접 대조** |
| 판단 | critical op/TL 비율과 성능이 oracle과 유의하게 다르면 → oracle 기반 결론(§8.1) 전부 재검토. 일치하면 이후 모든 실험을 실현 기구 위에서 수행 |
| 순서 | E2/E3(저비용 관문)를 먼저 돌려 oracle **상한**이 값어치 있는지 본 뒤, 통과하면 **Phase 3 이전에 반드시** 수행 |

> ⚠ **선행연구 경고.** last-arriving edge 기반 하드웨어 criticality 예측은 **Fields, Rubin, Bodík,
> "Focusing Processor Policies via Critical-Path Prediction," ISCA 2001**(token-passing predictor)이
> 이미 제안했다. 즉 *기구 자체는 novelty가 아니다*. CRISP §3.5의 "hardware는 critical path analysis를
> 할 수 없다"는 주장도 이 논문 앞에서는 성립하지 않으므로, **§8.2의 주장 1을 그대로 쓰면 안 된다.**
> 논문 반입 전 원문 확보 후 정확히 대조할 것(현재 `reference/`에 없음).

#### Phase 1 — 무엇을 골라낸 것인지 규명 (전제)

| | E1: critical/non-critical 분해 계측 |
|---|---|
| 목적 | "critical load란 어떤 load인가"를 확정. criticality ⟂ predictability 여부 |
| 방법 | `is_critical`을 load에 1 bit로 전달(priority bit와 같은 경로), RFP funnel 전체(PT-hit/발사율/정확도/useful/saved cycle)와 L1 miss율·rename→AGU를 **class별로 분해**. 1 run |
| 판단 | 발사율·정확도가 두 class에서 비슷하면 → 선별은 "같은 품질의 더 작은 집합"이므로 (가)·(나)로 간다. critical 쪽이 현저히 낮으면 → **RF 전달 축이 구조적으로 약함**, (다) 포기하고 efficiency 주장만 남긴다 |
| 비용 | 코드 소(bit 전파 + stat 12종 분해), 1 run |

#### Phase 2 — 두 주장 축을 병렬 검증 (둘 다 저비용, 결정적)

| | E2: PT 축소 sweep → (가) |
|---|---|
| 목적 | iso-performance를 **storage 절감 주장**으로 승격 |
| 방법 | `rfp_pt_entries` 128/256/512/1024 × {full-slice, critical-only} = 8 config |
| 판단 | 두 곡선이 벌어지면 "critical-only 256 = full-slice 1024" 형태의 4× 절감 주장 성립. **겹치면 (가) 폐기** — 남은 근거는 (나)뿐 |
| 비용 | **코드 변경 0**, config 8개 |

| | E3: static-freeze 대조 → (나) |
|---|---|
| 목적 | CRISP의 offline·고정 criticality 대비 runtime 재산출의 이득을 **우리 harness에서 직접 정량화**. §8.2 주장 1의 근거 figure |
| 방법 | 새 param `zereco_criticality_freeze_inst`. N instruction까지 정상 동작 후 mask 갱신과 reset을 모두 정지(= 한 번 뽑아 baked-in). N = 1M/2M/5M로 sweep, dynamic(정지 없음)과 대조 |
| 판단 | frozen이 유의하게 나빠지면 **(나) 성립 — 이게 논문의 핵심 figure**. 차이가 없으면 CRISP가 우리를 포섭하므로 **방향 재검토** |
| 비용 | param 1 + freeze 로직, config 4개 |

#### Phase 3 — 상한 해제 (Phase 1~2 결과에 따라)

| | E4: per-PC union 제거 → (다)의 전제 |
|---|---|
| 목적 | walk가 51% 걷어낸 것이 발사 −3.5%로 씻기는 문제 해소 |
| 방법 | OR 누적을 **빈도 게이팅**으로 교체. 1안(권고): PT entry에 criticality confidence 필드 추가 — walk가 critical로 지목하면 ++, slice 멤버지만 비critical이면 −−, 임계 미만이면 발사 금지. BMC를 건드리지 않아 저렴하고 하드웨어 타당 |
| 판단 | prefetch 발사가 실질적으로 줄면서 성능이 유지되면 **energy/bandwidth 주장** 확보(PACT'20이 지적한 L1 대역폭 문제와 직결) |

| | E5: 절감분 재투자 → (다) |
|---|---|
| 목적 | 선별은 그 자체로 성능을 못 올린다(줄이기만 함). 확보한 예산을 써야 이득이 난다 |
| 방법 | 깔때기 최대 손실 지점은 **PT-hit 59.2% → injected 29.0%**(confidence 미포화·stride 미확립). 대상이 절반이면 load당 예산이 2배이므로: confidence bit 확대, multi-stride/2-level, 또는 critical load 한정 재시도 |
| 판단 | critical load 발사율이 47.9%에서 유의하게 오르고 f→resolution이 줄면 성능 주장 성립 |

#### 게이트

- Phase 1~2까지 (가)·(나) **모두 실패하면 이 방향은 접는다.**
- 논문 최종 수치는 SimPoint 전체 + weight 가중으로 재산출한다(효과 기준 선별 금지).
- Baseline·RFP-only 기준점은 `260819_RFP_piq_axes`를 재사용한다(재측정 불필요).

### 8.4 실험 1 준비 (`260824_pt_size_sweep`) — 사전 점검 기록

`rfp_pt_entries`는 **총 entry 수**다 (`pt_sets = RFP_PT_ENTRIES / RFP_PT_ASSOC`, [rfp.c:173]).
기본값 1024 = 128 set × 8 way. 이전 서술의 "1024 set × 8 way = 8192 entry"는 오류였다.
1024에서도 gcc 외 대부분 축출 0이므로 sweep은 **아래로**(64~1024) 잡는다.

**사다리**: `{full,crit} × pt{64,128,256,512,1024}` = 10 config × 68 SimPoint = 680 run.
`full_pt1024`/`crit_pt1024`는 260821의 reset/critical config와 동일 설정이라 **앵커 겸 재현성 확인**.
코드 변경 불필요(파라미터만).

**점검 결과 — 이상 없음:**

| 항목 | 확인 |
|---|---|
| `PARAMS.golden_cove` | rfp/zereco 파라미터 미포함 → `DEF_PARAM` 기본값 적용 |
| PT 크기 assert | `entries ≥ assoc`, `entries % assoc == 0` — 64~1024 모두 통과 |
| PT 하드코딩 | 없음. `RFP_STALE_TABLE_ENTRIES(64K)`는 stale 검출용 별도 구조로 PT와 무관 |
| in-flight underflow | 축출 후 같은 PC 재할당 시 stale 인스턴스가 감소시킬 수 있으나 `inflight > 0` 가드로 underflow 없음. 260821 실측 398/50M (0.0008%) |
| chain 로깅 | `DEBUG_HBT \|\| DEBUG_TEA` 게이트, 두 config 모두 off → 디스크 위험 없음 |
| 디스크 | 260821이 272 run에 798 MB → 680 run ≈ 2 GB, 여유 604 GB |
| 무결성 카운터 | 260821 전 config 0 |

**보강한 것 (2026-08-24):** critical walk의 `dcc_op_value_ready_cycle()` fallback이
`done_cycle == MAX_CTR`을 그대로 반환할 수 있었다. 그 값은 모든 last-arrival 비교에서 이기고
slack 덧셈에서 overflow하므로, **모든 fallback을 `retire_cycle`로 상한**하고
`ZERECO_WALK_READY_CYCLE_FALLBACK` / `_UNKNOWN` 두 카운터를 추가했다. **결과 해석 전에 이 둘이
0 근처인지 먼저 확인할 것** — 크면 선별이 `wake_cycle`이 아닌 약한 신호로 이루어진 것이다.

**해석 시 주의 2건:**

1. **PT=64는 8 set뿐**이라 `load_pc % 8`의 낮은 엔트로피로 conflict가 지배할 수 있다. 압력이
   capacity인지 conflict인지는 `RFP_PT_EVICTIONS`와 함께 봐야 하고, 64 지점이 이상하면 같은
   크기에 assoc을 올려 재확인한다.
2. **criticality는 현재 기계의 timing 위에서 측정된다.** PT가 작아 coverage가 줄면 covered
   load의 `wake_cycle`이 늦어지고 critical 판정이 바뀐다. 두 arm이 완전히 독립 변인은 아니며,
   이건 설계에 내재한 피드백이다(설계 의도이기도 하다).


### 8.5 실험 1 결과 (`260824_pt_size_sweep`) — **(가) efficiency 주장 기각**

**신뢰성:** 무결성 6종 0. `ZERECO_WALK_READY_CYCLE_FALLBACK`/`_UNKNOWN` **전 config 0**
(선별이 100% `wake_cycle` 기반). reset 500,180 inst/회. `full_pt1024`/`crit_pt1024`가
260821의 대응 config와 **bit-exact 일치**.

**압력은 확실히 걸렸다:** PT=64에서 축출률 100%, walk allocation 55K → 4.94M (89×).

| PT (저장) | full | crit |
|---|---:|---:|
| 64 (0.75 KB) | −0.094% | −0.072% |
| 128 (1.5 KB) | −0.068% | −0.015% |
| 256 (3 KB) | +0.040% | +0.038% |
| 512 (6 KB) | +0.010% | +0.058% |
| 1024 (12 KB) | 0 (기준) | +0.073% |

**전 구간 IPC 폭 0.17%p. PT를 16× 줄여도 0.094%.** f→resolution도 37.90~38.01로 불변.

**기계적 원인.** PT 1024→64에서 useful prefetch가 6.25M 줄지만 그중 **99%가 L1D hit이던
load**다 (demand hit +6.21M, demand miss +0.03M). claimed saved cycles −17.54M에 대해 실제
cycle 증가는 +1.17M — **실현율 6.6%**. RF path의 load당 이득이 ≤4 cycle이고 OoO가 대부분
흡수하므로 구조적으로 그렇다.

**핵심: utility 기반 교체가 이미 criticality가 하려던 선별을 한다.** PT가 좁아질수록 살아남은
prefetch 중 lower-level로 가는 비율이 **17.31% → 20.21%**로 올라간다. 압력이 걸리면 PT는
자동으로 miss-prone load를 남기고 L1D-hit load를 버린다. 그래서 fill path 요청량은 −1.88%만
줄고(RF path probe는 −10.5%), 이득의 지배 성분이 보존된다.

**crit vs full은 노이즈.** 부호 일관성 8/14, 9/14, 5/14, 7/14, 8/14. xgboost·gcc 제외 시
+0.040 / +0.023 / +0.002 / −0.003 / +0.004%. 260821의 "crit +1.28%(xgboost)"는 **artifact**다:
xgboost는 injected가 1.852M→1.849M로 거의 불변인데 IPC가 0.00/+1.33/+0.27/+1.30%로 튀고,
`full_pt256`도 +1.33%를 낸다 — criticality와 무관한 bimodal 현상.

**판정**
- **(가) efficiency 기각.** criticality가 PT를 줄여주는 것이 아니다. PT는 원래 줄일 수 있다.
- **(나) static-freeze는 전제 소멸.** criticality 선별이 어느 방향으로도 성능을 바꾸지 않으므로
  "runtime이 offline 고정보다 낫다"를 비교할 대상 자체가 없다. E3 보류.

**건질 것 — RFP 자체에 대한 발견.** RFP 이득 전체가 극소수 load PC에 실려 있다.
**PT를 12 KB → 0.75 KB로 줄이는 데 0.09%**밖에 안 든다. mechanism 고유 비용 주장이
"PT 12 KB + queue 0.6 KB"에서 **"PT 0.75 KB + queue 0.6 KB"**로 개선된다. fill path는
lower-level request 8.59M으로 작동하며 PT 크기에 둔감(−1.88%)하다.

**반복되는 패턴 (세 번째).** ① RF filtering → bounded non-stall partition이 인구를 자기조절
→ 기각. ② criticality for P-IQ → br-select 0.18 cy, 예산 없음 → 무효. ③ criticality for PT →
utility 교체가 이미 선별 → 무효. **ZERECO의 자원 제약 구조들이 이미 자기선별을 하므로 명시적
선별 layer가 더할 것이 없다.** 이것이 이 연구 사이클의 negative result이며 서술 가치가 있다.

### 8.6 Phase 1 결과 (`260824_walk_reach_profile`) — chain은 멀리 뻗지만 **Target Load는 가깝다**

계측 전용 변경이므로 `crit_pt1024`/`crit_pt64`가 260824의 대응 config와 **bit-exact 일치**(cycle,
injected, useful, walk 카운터 전부). 계측이 타이밍에 영향 없음을 확인.

**window 통계 (두 config 동일, PT 크기의 되먹임 없음):**

| | 값 |
|---|---:|
| 측정 slice | 26.05 M (그중 room ≥ 256인 WIDE 49.9%) |
| 평균 room (볼 수 있었던 거리) | **255.2 uop** |
| 평균 critical span | **197.9 uop** (WIDE만 297.7) |
| critical TL 없는 slice | 19.4% |
| `SPAN_HIT_WINDOW_EDGE` | 7.8% of slices |
| `SRC_CUT_BY_WINDOW` | 8.1% of walk steps |

⇒ **512는 이미 8% 정도 chain을 자르고 있다.** 크지 않지만 0은 아니므로, 축소 sweep 결과는
"이미 잘린 상태에서 더 자른 것"으로 읽어야 한다.

**핵심: chain의 뻗음과 Target Load의 위치가 다르다.**

| 누적분포 (WIDE) | ≤8 | ≤16 | ≤32 | ≤64 | ≤128 | ≤256 |
|---|---:|---:|---:|---:|---:|---:|
| critical **chain op** 전체 | 17.6 | 22.7 | 29.1 | 39.8 | 57.2 | 82.0 |
| critical **Target Load** | **43.7** | 53.1 | 60.5 | 67.1 | 76.1 | 89.7 |

critical chain은 22.5 op이 ~200~300 uop에 걸쳐 **성기게** 뻗는다. 그런데 그 위의 **load는 branch
근처에 뭉쳐 있다.** 멀리 뻗은 부분은 대부분 ALU/주소계산 op이다.

> ⚠ **정정 (2026-08-25).** 이 문서의 이전 판은 "먼 부분은 P-IQ도 쓸 수 없다(br-select 0.18 cy =
> 예산 없음)"라고 썼는데 **틀렸다.** 0.18 cy는 **P-IQ가 이미 켜진 뒤 남은 잔여값**이다. 실측
> 사다리(randq, baseline 대비): baseline br-select **2.41** → +RFP 1.66 → +P-IQ **0.63**,
> **P-IQ 순수 몫 +2.24% IPC**이며 그 효과는 `dependency` 26.31 → 25.54(**−0.77 cy**, producer들의
> issue가 당겨진 몫)와 `br-select` 1.66 → 0.63(−1.03 cy)로 나뉘어 나타난다. ARCHITECTURE §2.3의
> "branch 자체에만 priority는 무의미"는 **slice 전체를 표시해야 하는 근거**이지 scheduling 축에
> 예산이 없다는 뜻이 아니다.
>
> 따라서 **"먼 chain op이 무용하다"는 아직 미검증 가설이다.** 260821이 보인 것은 *critical path
> 밖의 곁가지*가 무의미하다는 것이지, *critical path 위의 먼 op*에 대해서는 측정이 없다.
> Phase 2가 정확히 이것을 판정한다 — 거리를 줄일 때 `dependency`와 `br-select`를 따로 볼 것.

**가치로 가중하면 더 짧아진다.** fill path(이득의 지배 성분, 89%가 그래프 워크로드)로 가중한
critical Target Load 누적분포:

| 가중 기준 | ≤8 | ≤16 | ≤32 | ≤64 | ≤128 | ≤256 |
|---|---:|---:|---:|---:|---:|---:|
| 균등(slice) | 43.7 | 53.1 | 60.5 | 67.1 | 76.1 | 89.7 |
| RFP useful | 42.0 | 52.3 | 60.5 | 67.9 | 77.5 | 90.8 |
| **fill path (WIDE)** | **55.4** | 64.9 | **72.1** | **76.8** | 82.8 | 92.3 |
| **fill path (전체 slice)** | **59.8** | 70.2 | **78.0** | **82.9** | 88.7 | 95.7 |

벤치마크별로 보면 이유가 분명하다 — **fill path를 지배하는 워크로드가 곧 reach가 짧은 워크로드**다:

| bench | fill path 비중 | TL ≤8 | TL ≤64 |
|---|---:|---:|---:|
| tc | 31.2% | 63.8% | 76.1% |
| bfs | 20.1% | 63.8% | 89.1% |
| bc | 17.5% | 72.9% | 87.4% |
| mcf | 12.1% | 30.4% | 54.6% |
| sssp | 8.5% | 45.8% | 74.7% |
| clang/gcc/xz/deepsjeng/omnetpp | **합계 ~3%** | 20~32% | 40~56% |

reach가 긴 워크로드(clang/gcc/xz)는 fill path 기여가 3%뿐이고, 260824에서 이들의 RFP 이득 자체가
작았다(PT 1024→64에 −0.25~−0.50%).

**판정: 사용자 가설이 "중요한 부분에 대해" 확인되었다.** 단 정확한 형태는 "chain이 짧다"가 아니라
**"chain은 길지만 Target Load는 가깝고, 먼 부분은 수익화할 수 없는 ALU op이다"**이다.

**Phase 2 예측.** 260824의 탄력성(useful prefetch −18.5%에 IPC −0.09%)을 이 분포에 적용하면
**D = 32~64에서 fill 가중 TL을 ~20% 잃고, 그 손실은 0.1% 수준**일 가능성이 높다. sweep 값은
{8, 16, 32, 64, 128, 256, 무제한}으로 잡고 baseline_randq를 함께 넣는다.

**RIW(= fill buffer) 축소로 넘어갈 때 주의.** 도달거리 제한과 RIW 축소는 다른 변인이다. RIW를 줄이면 buffer가
더 빨리 차서 **walk 빈도가 올라가고 gating으로 인한 sampling 손실이 커진다.** 평균 room이 RIW의
절반이므로, D=64를 지탱하려면 RIW는 256 정도(8 KB → 4 KB)가 안전하고, 128까지 줄이면 room이
평균 64로 빠듯해진다. Phase 2(거리 제한)를 먼저 확정한 뒤 Phase 3에서 별도로 측정할 것.

### 8.7 Phase 2 결과 (`260825_walk_distance_sweep`) — **D=64에서 99.6% 회수**

`crit_dfull`이 260824 `crit_pt1024`와 bit-exact 일치(앵커 확인). baseline_randq 포함 8 config.

| | d8 | d16 | d32 | **d64** | d128 | d256 | dfull |
|---|---:|---:|---:|---:|---:|---:|---:|
| **IPC vs baseline** | +8.70 | +9.36 | +9.89 | **+10.09** | +10.03 | +10.11 | +10.13 |
| 무제한 대비 회수 | 86.0% | 92.5% | 97.7% | **99.6%** | 99.1% | 99.9% | 100% |
| f→resolution | 38.91 | 38.41 | 38.09 | 37.91 | 38.00 | 37.90 | 37.90 |
| dependency 회수 | 81.0% | 90.9% | 96.2% | **99.9%** | 98.3% | 100% | 100% |
| br-select 회수 | **102.5%** | 100.5% | 101.0% | 100.2% | 100.1% | 99.9% | 100% |
| fill path 보존 | 85.2% | 95.9% | 96.1% | **99.9%** | 99.9% | 100.1% | 100% |
| walk가 훑는 op/slice | 4.33 | 5.99 | 7.91 | **11.12** | 16.48 | 24.35 | **29.97** |
| mem live-in 초과 | 0 | 0 | 0 | **123** | 29,185 | 843,683 | **1,873,230** |

*(baseline_randq: f→res 45.86, dependency 31.13, br-select 2.41)*

**판정 4건:**

1. **D=64가 무제한의 99.6%.** walk가 훑는 op은 29.97 → **11.12 (−63%)**, PT allocation
   33,252 → 23,693(−29%), priority 인구 44.25% → 41.44%. **fill path는 완전 보존**(8.58 vs 8.59M).
2. **br-select는 모든 D에서 100% 회수된다** — D=8에서도 102.5%다. 즉 **branch 자신의 select 대기는
   거리를 전혀 요구하지 않는다.** D=8에서 오히려 100%를 넘는 이유는 priority 인구가 28.47%로 줄어
   Priority 구획이 덜 붐비고 branch가 더 빨리 select되기 때문 — **priority 희석 효과의 직접 관측**이다.
3. **거리를 요구하는 것은 `dependency` 구간**이다 (81.0% → 99.9%). producer들의 issue 당겨짐과
   RFP coverage가 여기 함께 실린다. 두 성분의 분리는 아직 안 됐다(§8.8 실험 필요).
4. **짧은 walk가 오히려 더 충실하다.** 16-entry 메모리 live-in 목록 초과가 무제한에서 **1,873,230건**
   (조용히 버려진 store→load 간선)인데 D=64에서 **123건**으로 사라진다. 긴 walk는 그 자체로 절단을
   만들고 있었다.

**벤치마크별:** fill path를 지배하는 워크로드가 가장 먼저 포화한다 — tc(fill 31.2%)는 **D=8에서 이미
99.1%**, mcf(12.1%) D=8에서 95.9%, bfs(20.1%) D=16에서 97.4%, bc(17.5%) D=32에서 99.5%.
거리를 요구하는 clang/gcc/xz/deepsjeng/omnetpp은 fill 기여 합계 3%이고 절대 이득도 1.3~4.2%로 작다.

### 8.8 다음 실험 — **criticality가 여전히 필요한가** (최우선)

지금까지 확인된 것: criticality 선별은 무제한 거리에서 **성능을 바꾸지 않았다**(260821, 260824의
full vs crit이 +10.04 vs +10.13%). 그런데 **거리 제한이 걸린 상태에서도 그런지는 측정된 적이 없다.**
작은 D에서는 예산이 빠듯하므로 "제한된 예산 안에서 옳은 op을 고르는" criticality가 그때 비로소
값어치를 낼 수도 있고, 아니면 여전히 무의미할 수도 있다.

**이 질문의 무게가 크다.** 만약 full-slice가 D=64에서 critical-only와 같다면:
- last-arriving edge 기록 하드웨어(§Phase 0의 PCT 설계)가 **통째로 불필요**해진다
- `wake_cycle` oracle 의존이 사라져 **실현 가능성 논란 자체가 소멸**한다
- 기구가 "TEA식 walk를 64 uop 깊이로만" 으로 단순해진다

⇒ config: `full_d{8,16,32,64}` × `crit_d{8,16,32,64}` + baseline. 코드 변경 불필요.
