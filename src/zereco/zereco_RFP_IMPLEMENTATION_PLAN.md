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
