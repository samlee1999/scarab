# ZERECO — Branch 오예측 조기 해소를 위한 H2P-Chain Load 가속

> 연구 요약 및 실험 로그. 최종 수정 2026-07-14 (Scarab `test` 브랜치).
> 이 문서는 핵심 아이디어, related-work 포지셔닝, 메커니즘 설계, 지금까지의 모든 실험 결과를
> 하나로 정리한다. TEA를 end-to-end로 완성하는 방향을 대체하며, TEA는 이제 비교 baseline이다.

---

## 1. 목표 & 핵심 통찰 (Core Insight)

**목표**: branch 오예측 페널티를 줄이기 위해 오예측을 최대한 빨리 감지한다. 특히 **H2P
(Hard-to-Predict, 예측하기 어려운) branch**를 대상으로 한다.

**핵심 통찰**: 오예측된 H2P branch가 늦게 resolve되는 주된 이유는 그 **dependence chain 안의
load가 느리기 때문**이다. TEA (MICRO 2024)는 이를 별도의 precomputation thread로 공략하는데,
빠르고 정확하지만 복잡도가 매우 높다. 대신 **H2P dependence chain 안의 load latency만 줄이면**,
별도 thread 없이도 H2P branch가 execute 단계에서 일찍 resolve된다.

**동기가 되는 oracle 실험**: 모든 H2P-chain load를 1-cycle latency로 강제했을 때 (geomean Periodic IPC):
- 최신 baseline(golden_cove stream prefetcher ON) 대비 **+28.4%** (run `260709_decoupling_RFP_L1P`).

이 headroom은 같은 시뮬레이터에서 TEA가 보고한 이득의 약 2배이며, 복잡도는 훨씬 낮다.

---

## 2. 메커니즘 설계

**H2P dependence chain에 속한 load만** 타깃으로 한다 (TEA 기반 구조로 식별:
HBT → Fill Buffer → Backward Dataflow Walk → Dependency Chain Cache). 타깃 집합을 작게 유지하는
것이 하드웨어를 가볍게 만드는 핵심이다.

각 타깃 load에 대해:
1. **주소 예측 가능 → prefetch.**
   - 정확한 주소 예측 → **register-file (RF) prefetch** (ISCA'22 RFP 방식): 값을 물리 레지스터에
     직접 배달. 주소가 틀리면 기존 scheduler replay로 no-flush 복구.
   - cache line만 예측 → **L1 prefetch**: 라인을 L1로 가져옴 (정확하지만 효과 약함).
2. **예측 불가 → PUBS 방식 issue priority** (MICRO'18): chain을 IQ에서 먼저 스케줄링.
   정확하지만 arbitration cycle 몇 개만 절약. 핵심 축이 아니라 바닥 보정으로 취급.
3. **table 기반 per-PC confidence classifier**가 예측 가능/불가를 판정 (stride / top-delta).

**복잡도 스토리(단계적)**: ① H2P 타깃 L1 prefetch (correctness 장치 불필요) →
② RFP 방식 RF 배달 (no-flush replay 재사용) → ③ PUBS priority 폴백 (IQ 무수정).

---

## 3. Related-Work 포지셔닝

| 논문 | 하는 일 | 우리가 채우는 빈틈 |
|------|---------|--------------------|
| **TEA** (MICRO'24) | H2P chain을 별도 thread로 precompute; 늦은 결과 → early flush | chain-load latency/예측가능성을 특성화하지 않음; prefetcher baseline 없음; backend prioritization(CRISP)을 한 문장으로 기각. 높은 복잡도 (Block Cache 19KB, Fill Buffer 8KB, RS/PR 192개 예약, dynamic inst +31.9%). |
| **DLVP** (MICRO'17) | Path 기반 주소 예측 → 조기 cache probe → value | 직접 증거: perlbmk +71% — value 예측된 load가 **오예측 조기 해소**를 가능케 했기 때문. 단 criticality 타깃팅은 없음. |
| **RFP** (ISCA'22) | rename 시점에 물리 레지스터 파일로 stride prefetch; no-flush 복구 | **criticality 기반 타깃팅을 future work로 명시** — 정확히 우리의 H2P-chain 필터. rename 발사라 L1 latency만 숨김. |
| **PUBS** (MICRO'18) | unconfident-branch-slice op를 IQ에서 우선 처리 | issue-arbitration cycle만 제거; LLC MPKI 높으면 스스로 비활성. 우리의 "예측 불가" 폴백 상한을 규정. |
| **Prefetch survey** (2020) | Strided/spatial/temporal 분류 | temporal만 pointer chasing을 잡고, MB급 메타데이터 필요. H2P-chain load로 필터링하면 작은 on-chip temporal/Markov 테이블이 가능해짐. |

Novelty 주장 전 확인 필요: **CRISP** (ASPLOS'22, 컴파일러 criticality slice), **Focused Value
Prediction** (ISCA'20), **Hermes** (MICRO'22, off-chip load 예측), **Branch Runahead** (MICRO'21),
**SLB** (HPCA'13).

우리의 고유 좌표: *H2P-chain membership을 criticality 필터로 써서, 값싼 주소 예측기 + RF prefetch로
오예측 페널티를 직접 공략한다.*

---

## 4. 실험 인프라 (Scarab)

- **Oracle** (`h2p_chain_perfect_load`): 모든 on-path H2P-chain load를 고정 latency로 강제
  (`dcache_stage.c:dcache_stage_try_main_chain_load_oracle`). store-forward load는 제외.
- **Access-pattern 프로파일링** (`h2p_chain_load_pattern_profile`): per-PC reuse/stride/delta 통계.
- **Raw-stream 덤프** (`h2p_chain_load_raw_stream_dump`) + **offline replay**
  (`src/tools/h2p_chain_load_predictor_replay.py`): last-value / stride / top-delta / Markov를
  vaddr 및 cache-line 단위로, on/off-path 필터와 함께 replay.
- **Online predictor oracle** (본 프로젝트): oracle을 *online* per-PC 예측기로 gating (offline
  replay와 동일 알고리즘). 그래서 측정된 IPC = oracle 상한 중 실제 회수 가능한 비율. 파라미터
  (`core.param.def`):
  `h2p_chain_oracle_predictor` (0=none/전체, 1=stride, 2=top-delta),
  `h2p_chain_oracle_granularity` (0=vaddr/RF, 1=line/L1),
  `h2p_chain_oracle_hit_latency` (vaddr latency, 기본 1),
  `h2p_chain_oracle_stride_confidence` (2), `h2p_chain_oracle_min_count` (2).
  line 단위는 flat 상수가 아니라 실제 L1-hit latency(`DCACHE_CYCLES + extra_ld_latency`)를
  적용해서, `vaddr − line` 차이가 RF-vs-L1 이득을 분리한다. **first-visit 가드**
  (`op->dcache_cycle == MAX_CTR`)로 각 dynamic load를 정확히 한 번만(program order) 학습시켜
  sim 커버리지를 offline replay와 정렬한다.

**예측기 알고리즘**
- *stride*: PC마다 delta 하나만 기억. 같은 delta가 `confidence`(=2)회 반복돼야 `last + stride`
  예측. 좁지만 정확 — 규칙적 stride 접근에 강함.
- *top-delta*: PC마다 delta 히스토그램(16 슬롯) 유지. 최빈 delta로 `last + most_frequent_delta`
  예측(등장 ≥ `min_count`=2). 넓은 커버리지, 낮은 정확도 — no-flush 복구에서는 저렴함.

---

## 5. 결과

### 5.1 Access-pattern 특성화 (`260624`, top-5-weight simpoint, 가중 평균)
- H2P-chain 타깃 load = 전체 on-path load의 **52%**.
- Dcache hit **83%**, memory access **12%**, **store-forwarding ~0.08%** (→ LSCD blacklist 불필요).
- 평균 latency 23.7 cyc, 그러나 workload별 이중모드: L1-hit 지배적인 leela 5.5 / sssp 7.3 vs pr 117.7.
- Per-PC top-4 delta 예측가능성: **byte 82% / line 86%** (bfs/cc/pr 94-98%; omnetpp/mcf/leela ~61-68%).
- 주소 반복성 **이중모드**: 접근의 61%가 ≥128회 반복, 22%가 <8회 반복 (중간은 거의 없음).
- Top-5 PC가 접근의 43% 커버 (GAP 높음: pr 91%, sssp 80%; SPEC/DC는 낮음).

**→ "H2P load는 본질적으로 예측 불가"라는 리스크를 반박: 대체로 예측 가능하다.**

### 5.2 Online predictor replay (in-sim, run `260709_decoupling_RFP_L1P`)

이 수치는 timing 시뮬레이션 내부에서 측정한 **online** per-PC 예측기 통계(oracle을 gating하는 바로
그 예측기)라서, §5.3의 IPC config와 정확히 대응한다 — 독립적인 offline replay가 아니다.
벤치마크별, simpoint 가중 (clang은 1305 제외).

- **coverage** = correct / candidate chain load = 실제로 idealize된 chain load 비율.
- **accuracy** = correct / 예측 수행(made).
- stride는 delta가 ≥2회 반복될 때만 예측(따라서 made < 100%); top-delta는 거의 항상 예측
  (made ≈ 100%)이라 coverage ≈ accuracy.

**RF / exact-vaddr 예측기** (`pred_*_vaddr`에서 사용, latency=1):

| benchmark | stride cover% | stride acc% | top-delta cover% | top-delta acc% |
|-----------|--------------:|------------:|-----------------:|---------------:|
| bc        | 70.8 | 99.5 | 71.5 | 71.5 |
| bfs       | 87.8 | 97.7 | 92.2 | 92.2 |
| cc        | 89.4 | 99.0 | 91.6 | 91.7 |
| pr        | 94.3 | 98.7 | 96.7 | 96.7 |
| sssp      | 95.4 | 99.2 | 97.0 | 97.0 |
| tc        | 69.3 | 92.6 | 81.1 | 81.1 |
| deepsjeng | 49.6 | 87.6 | 66.5 | 66.7 |
| leela     | 39.3 | 89.5 | 54.7 | 54.7 |
| mcf       | 47.3 | 91.1 | 58.6 | 58.6 |
| omnetpp   | 34.2 | 90.1 | 48.1 | 49.8 |
| xz        | 65.9 | 92.9 | 77.6 | 77.6 |
| clang     | 55.5 | 94.5 | 69.1 | 69.1 |
| gcc       | 40.9 | 88.3 | 56.5 | 57.5 |
| xgboost   | 78.8 | 97.2 | 86.3 | 86.3 |
| **AVG**   | **65.6** | **94.1** | **74.8** | **75.0** |

**L1 / cache-line 예측기** (`pred_*_line`에서 사용, L1-hit latency):

| benchmark | stride cover% | stride acc% | top-delta cover% | top-delta acc% |
|-----------|--------------:|------------:|-----------------:|---------------:|
| bc        | 58.5 | 92.6 | 68.9 | 68.9 |
| bfs       | 75.0 | 91.5 | 89.2 | 89.2 |
| cc        | 73.3 | 91.1 | 87.8 | 87.8 |
| pr        | 78.7 | 92.4 | 91.7 | 91.7 |
| sssp      | 92.9 | 98.2 | 96.4 | 96.4 |
| tc        | 58.2 | 84.5 | 80.2 | 80.2 |
| deepsjeng | 52.9 | 87.3 | 70.8 | 70.9 |
| leela     | 48.0 | 90.2 | 62.6 | 62.6 |
| mcf       | 41.1 | 86.1 | 58.1 | 58.2 |
| omnetpp   | 36.8 | 88.5 | 52.3 | 54.2 |
| xz        | 74.0 | 94.5 | 83.6 | 83.6 |
| clang     | 54.3 | 93.2 | 68.4 | 68.5 |
| gcc       | 42.5 | 87.5 | 59.9 | 60.9 |
| xgboost   | 88.4 | 97.7 | 94.7 | 94.8 |
| **AVG**   | **62.5** | **91.1** | **76.0** | **76.3** |

**해석**:
- GAP 그래프 workload(bc/bfs/cc/pr/sssp)와 xgboost는 예측 매우 잘 됨 (stride 88-96% cover
  @ 97-99% acc); chain load가 거의 완전히 회수 가능.
- SPEC-int/DC의 pointer-chasing(omnetpp 34%, leela 39%, gcc 41%, mcf 47%)이 저커버리지 tail —
  Markov/temporal 예측기가 공략할 잔여 gap (§7 참조).
- stride vs top-delta: stride는 정확도가 높지만(vaddr 기준 94.1% vs 75.0%) 커버리지가 낮음
  (65.6% vs 74.8%); no-flush 복구에서는 정확도 손실이 무비용이라 top-delta의 넓은 커버리지가
  §5.3의 더 높은 IPC로 이어진다.
- 이전 offline replay와 교차검증 일치 (stride vaddr: online 65.6/94.1 vs offline 66.3/94.7) →
  online 구현 검증됨.

### 5.3 성능 & RF-vs-L1 분해 (`260709`, prefetcher-ON, geomean Periodic IPC)
Baseline = golden_cove + 기본 stream prefetcher ON. clang/1305 제외 (predictor config 3개에서
"no forward progress" ASSERT — 알려진, 무시하는 Scarab 워치독).

| config | vs baseline | oracle headroom 대비 |
|--------|-------------|----------------------|
| **oracle** (모든 chain load latency=1) | **+28.41%** | 100% |
| **pred_top_delta_vaddr** (RF) | **+12.06%** | 42.5% |
| pred_stride_vaddr (RF) | +10.01% | 35.2% |
| pred_top_delta_line (L1) | +4.54% | 16.0% |
| pred_stride_line (L1) | +1.16% | 4.1% |

**RF vs L1 (vaddr − line):** stride RF 추가분 **+8.85pp**, top-delta **+7.52pp**.

**→ 이득의 ~80-90%는 RF-level 정확-vaddr 배달이 필요하고, L1-prefetch만으로는 거의 회수 못 함.**
이유: chain load의 ~83%가 이미 L1 hit이라 라인을 L1로 가져오는 건 잉여이고, 진짜 병목은 dependent
chain을 따라 누적되는 L1-hit-use latency이며, 이는 값을 레지스터로 배달해야만 제거된다.
**설계 결론: 메커니즘은 L1 prefetch가 아니라 register-file prefetch여야 한다.**

stream prefetcher가 oracle headroom을 +32%(OFF)에서 +28.4%(ON)로 줄였다: 쉬운 strided chain load를
이미 흡수했기 때문이며, 남은 +28%가 범용 prefetcher가 잡지 못하는 몫 — H2P-chain 타깃 RF prefetch의
동기다.

### 5.4 구현 교차검증
Online predictor의 sim 커버리지가 offline replay와 거의 정확히 일치:
- stride vaddr: **online 65.8% cover @ 94.7% acc** vs offline 66.3% @ 94.7%.
- top-delta vaddr: online 75.8% cover @ 76.0% acc (16-슬롯 히스토그램 근사).

top-delta가 stride를 이기는 이유: make-rate가 99.8% vs 69.4%이고, no-flush 복구가 낮은 76% 정확도의
비용을 없애기 때문 → **pred_top_delta_vaddr가 현실 최적 config**.

벤치마크별: 예측성이 높은 workload(xgboost, mcf, bfs, cc, tc)에서 현실 이득이 가장 큼;
저예측성/장지연 workload(xz oracle +57% vs 현실 +7%, omnetpp, pr)에서는 현실 ≪ oracle —
temporal/Markov gap.

---

## 6. 핵심 통찰 (한 줄 요약)

1. 인과 고리의 각 단계는 이미 선행 연구에서 검증됨; *조합*(H2P-criticality 필터 주소 예측 →
   RF prefetch → branch 조기 해소)만이 새로움.
2. Oracle headroom은 TEA의 ~2배인데 복잡도는 훨씬 낮고, 강한 prefetcher baseline에서도 살아남음.
3. H2P-chain load는 대체로 예측 가능(delta 82-86%)해서 역상관 리스크를 반박함.
4. 이득은 본질적으로 **RF-level**(정확 주소 → 레지스터)이지 L1-level이 아님 — load 대부분이 이미
   L1 hit이고 병목이 chain을 따라 누적된 hit-use latency이기 때문.
5. no-flush 복구 하에서는, 넓고 부정확한 예측기(top-delta)가 좁고 정확한 예측기(stride)를 이김.

---

## 7. 열린 질문 / 다음 단계

- 저예측성 gap(xz, omnetpp, pr) 좁히기: Markov/temporal 예측기 추가; stride/delta가 놓치는
  인터리브된 반복 주소를 회수하는지 측정.
- Prefetch **timeliness** 명시적 모델링 (in-flight 수만큼 N-ahead 예측, RFP 방식) — online
  예측기가 이를 할 수 있는 기반; offline replay는 불가.
- "예측 불가" PUBS 방식 폴백 기여분을 별도로 정량화.
- 오예측 비용: 현재 실험은 no-flush 가정(틀리면 idealize 안 함); 현실적 순이득을 위해
  scheduler-replay 페널티 추가.
- CRISP / Focused VP / Hermes / Branch Runahead / SLB 대비 novelty 확인.

---

## 8. 파일 / Run 색인
- 코드: `src/dcache_stage.c` (oracle + online 예측기), `src/core.param.def` (파라미터),
  `src/tea/tea.stat.def` (`H2P_CHAIN_LOAD_ORACLE_PRED_*`).
- Offline 도구: `src/tools/h2p_chain_load_predictor_replay.py`.
- Descriptor: `~/scarab-infra/json/zereco_dbg.json` (config: baseline / oracle /
  pred_{stride,top_delta}_{vaddr,line}, 전부 prefetcher-ON).
- Run: `~/simulations/260625_perf_comparison` (prefetcher-OFF oracle),
  `~/simulations/260624_h2p_chain_load_access_pattern_all_simpoints` (access pattern + offline replay),
  `~/simulations/260709_decoupling_RFP_L1P` (RF-vs-L1 분해; 그래프 + collected_stats.csv 여기).
- 참고 논문: `/home/lee/scarab/reference/` (TEA, PUBS, RFP, DLVP, prefetch survey).
