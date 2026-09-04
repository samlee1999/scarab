# TODO — deferred work

> Last updated: 2026-09-04 · branch `test`
> Design: [zereco_CRITPATH_DESIGN.md](zereco_CRITPATH_DESIGN.md) · Papers: [zereco_REFERENCE_NOTES.md](zereco_REFERENCE_NOTES.md)

---

## 1. Address-generation slice statistics (요청: 2026-09-04, 급하지 않음)

**왜** — store→load memory dependence를 추적하지 못하므로, Target Load에 대해 남는 가속 경로는
**주소 계산 slice**뿐이다. 그 경로가 실제로 얼마나 기여하는지는 지금 측정되지 않는다.
현재 아는 것은 critical edge의 종류뿐이다: register 97.7% / store→load 2.3%.

**넣어야 할 계측** (`critpath_note_retire`에서 load를 처리할 때)

| 카운터 | 무엇을 답하나 |
|---|---|
| Target Load의 LPR이 **주소 operand**의 producer인지 **데이터 operand**의 producer인지 | 가속되는 경로가 주소 쪽인지 확인 |
| Target Load의 주소 chain 길이 (depth 분포) | 주소 계산이 몇 단계인가 |
| priority를 받은 주소 chain이 load의 **AGU 도달 시점**을 얼마나 앞당기는가 | 효과의 크기 |
| **RFP가 실패한(주소 예측 불가) Target Load 중 주소 chain 가속만으로 회수되는 비율** | ← 가장 중요 |

마지막 항목이 *"RFP가 못 잡는 load를 P-IQ가 주소 경로로 잡는다"*는 두 메커니즘 상보성의
직접 증거가 된다. 지금은 두 메커니즘이 거의 가산적(+5.17%, +4.89% → +9.09%)이라는 간접
증거만 있다.

구현 힌트: load의 source 중 어느 것이 주소 계산에 쓰이는지는 `op->oracle_info.src_info[]`와
`op->table_info`로 판정한다. LPR 인덱스(`op->critpath_last_src`)를 그 집합과 대조하면 된다.

---

## 2. 알려진 결함 · 계측 공백

| 항목 | 상태 | 비고 |
|---|---|---|
| `RFP_INFLIGHT_UNDERFLOW` (~3.6K, injected의 0.017%) | 무해, 미수정 | PT entry가 축출된 뒤 **같은 PC로 재할당**되면, 축출 전에 rename된 in-flight 인스턴스가 retire할 때 tag가 맞아 0인 카운터를 감소시킨다. validate-then-use가 걸러내므로 correctness 문제는 아니고 예측 정확도의 미세 손실. PT를 키우면 자연히 줄어든다 |
| ~~`ZERECO_IQ_PRIORITY_OVERTAKES_NORMAL` 등 경합 카운터가 0~~ | **해결 (0de65a7)** | 세 게이트에 `ZERECO_CRITPATH_PRIORITY`를 추가했다. B-2부터 기록된다 |
| 관찰 테이블이 Phase B에서도 계측용 크기 | 의도적 | 4096 sets × 8-way. Phase B 확정 후 실제 하드웨어 예산으로 축소하고 민감도를 재야 한다 |

---

## 3. 워크로드 · 방법론

- **clang 4개 simpoint(1270/1305/2249/62), xgboost 1개(3311)가 watchdog assert로 미완료.**
  `decoupled_frontend.cc:518` "No forward progress" — **`baseline_randq`(순정 Scarab)에서도
  실패**하므로 우리 코드와 무관하다. 현재는 제외하고 나머지에서 top-8을 뽑아 쓴다.
  → clang은 원래 weight의 45%, **xgboost는 23%**만 커버한다. xgboost/3311(weight 0.759)이
  지배 phase였으므로, **xgboost 결과는 "지배 phase를 뺀 나머지"**임에 유의.
- **tc는 255개 중 8개(weight 7%)만 사용.** weight가 균등(최대 0.011, 중앙값 0.0039)해서
  표본으로는 타당하나 표본 수가 적다. 결과 분산이 크면 tc만 늘린다. 최종 논문 전에 전체
  255개와 대조해 저weight simpoint의 편향 없음을 확인할 것.
- **gcc는 4개 유지** (오래 걸리는 simpoint가 많음). 필요해지면 8개로.

---

## 4. 남은 실험

| # | 내용 | 상태 |
|---|---|---|
| **B-1** | PT 크기 sweep | **완료 — PT는 제약이 아니다.** 1K와 무제한이 모두 +9.1%. §6 참조 |
| **B-2** | partition 비율 sweep (무한/30/25/20/15/10%), PT 무제한·depth 무제한 | **1차 전량 실패** — §7 참조. 가드 수정 후 재실행 필요 |
| **B-3** | depth 제한 sweep (무제한/8/4/2/1), PT·partition 비병목 | 대기 |
| **B-4** | **PT 축소 sweep (128/256/512/1K)** | 대기. B-1이 1K↔무제한 무차별을 보였으므로 아래쪽 무릎이 어디인지 미측정. "값싼 예측기" 논거를 정량화한다. **주의: PT entry는 PC만이 아니라 base_va·stride·confidence를 담으므로 축출은 학습 상태의 소실이다** |
| — | 축 간 상호작용이 의심되는 지점만 2차원으로 좁혀 확인 | 필요시 |

---

## 5. Phase A2가 남긴 미해결 질문

**retention counter와 decay는 인구를 줄이지 못했다.** threshold 0→8+에 68.1%→67.2%,
decay 20K에도 59.3%. 원인은 refresh:new = 1792:1 — 재확인이 노화를 압도해 "드물게 확인되는
멤버"라는 모집단이 존재하지 않는다. 삽입 게이트는 이미 H2P-only(가장 엄격)라 조일 여지가 없다.

**그런데 Phase B에서 성능은 잘 나왔다** (+9.09%). 그리고 RS 실제 점유율은 25.3%로, 커밋 기준
멤버십 59.9%의 절반 이하였다 — 멤버 op가 우선 issue되어 큐에서 빨리 빠지기 때문이다.
즉 **"인구가 많다"는 것이 곧 "priority가 희석된다"를 뜻하지 않았다.**

⇒ 인구 축소가 그 자체로 목표인지는 재검토 대상이다. depth 제한(B-3)이 성능에 어떤 영향인지
본 뒤에 판단한다. 인구를 줄여도 성능이 유지되면 하드웨어 비용 절감 논거가 되고, 떨어지면
"critical path는 넓지만 그래서 유효하다"는 서술로 간다.

---

## 6. B-1 결과 — PT 크기는 제약이 아니다 (2026-09-04)

| PT | IPC | alloc | evict/alloc | 포화/alloc | eligible | useful |
|---|---:|---:|---:|---:|---:|---:|
| 1K | +9.09% | 1,120,402 | 1.00 | 0.33 | 31.6% | 20.1% |
| 4K | +9.12% | 138,950 | 0.93 | 2.75 | 31.9% | 20.4% |
| 16K | +9.10% | 25,231 | 0.31 | 15.30 | 31.9% | 20.4% |
| 무제한 | +9.11% | 20,524 | 0.00 | 18.83 | 31.9% | 20.4% |

**스래싱은 실재했지만 성능과 무관했다.** 축출을 완전히 없애도 IPC는 +0.02%p 움직인다.
confidence 포화 **건수**가 크기와 무관하게 거의 같다(365K → 386K, +5.7%)는 것이 이유다.
1K에서의 112만 할당은 **한 번도 포화하지 못할 cold PC들의 회전**이고, 실제로 포화하는 hot PC
집합은 1K 안에 이미 상주한다. RFP 논문이 1K를 기본값으로 고른 것과 같은 결론.

**RFP의 상한은 5.19%** (PT 무제한, priority 없음). 1K에서의 5.17%와 사실상 같다.
따라서 RFP를 묶는 것은 테이블 용량이 아니라 **주소 예측 가능성**이다 — PT-hit 69.5% 중
eligible이 31.9%뿐이고, 그 격차는 confidence가 서지 않는 load들이다.

부수 확인: `RFP_INFLIGHT_UNDERFLOW`가 PT 크기와 함께 3,443 → 296 → 7 → 0으로 사라졌다.
§2에서 추정한 원인(축출 후 같은 PC로 재할당)이 맞았다.

---

## 7. B-2 1차 실패와 그 과정에서 드러난 오염 (2026-09-04)

**증상.** partition config 5개가 전부 0/108. `b2_unbounded`(piq_enable=0)만 완주.

**원인.** `dependency_chain_cache.c`의 init assert가 `ZERECO_PIQ_ENABLE`일 때
`ZERECO_IQ_PRIORITY_POLICY == 1 || == 2`를 요구했다. 옛 Block-Cache 시절의 가드이고,
critical-path 경로로 priority를 넣는 구성을 알지 못한다. **수정**: 특정 policy 값이 아니라
"유효한 priority source가 정확히 하나"를 요구하도록 바꾸고, 두 source 동시 활성을 잡는
assert를 추가했다. `ZERECO_PIQ_ENTRY_PERCENT`의 {10,15,20,25,50} 화이트리스트도 1~99로 풀었다.

**그 과정에서 발견한 더 중요한 문제 — 옛 backward walk가 계속 돌고 있었다.**

```
node_stage.c:1042   fill_buffer_add()              ← 매 retire, 무조건
cmp_model.c:302     cycle_backward_walk_engine()   ← 매 cycle, 무조건
  └─ commit_dependency_chain_entry() → rfp_note_target_load()
```

B-1 `b1_ptinf` 기준 Target Load 지명 82.37M 중 **42.9%(35.32M)가 옛 walk에서** 나왔다
(critical path는 57.1%). 즉 `rfp_target_critpath 1`이 순수한 critical-path scoping이
아니었고, full-slice walk가 절반 가까이 섞여 있었다.

**수정**: walk의 `rfp_note_target_load()` 호출을 `!RFP_TARGET_CRITPATH`로 게이트했다.

**영향 범위**: RFP 계열 수치(Phase B `cp_rfp` +5.17%, B-1 전체)가 이 오염을 포함한다.
P-IQ 쪽은 priority bit를 critpath에서만 받으므로 무관하다. 방향은 "선별이 덜 엄격했다"이므로
순수화하면 PT 압력이 줄어들 것이고, B-1이 PT 크기 무관을 보였으므로 IPC 변화는 작을 것으로
예상하나 **재측정 전에는 확정할 수 없다.**

~~**남은 확인 사항**: walk 자체는 여전히 매 cycle 돈다~~ → **해결.** `LEGACY_WALK_NEEDED()`
(dependency_chain_cache.h)가 소비자 유무를 판정하고, `fill_buffer_add`·node_stage의 buffer-full
트리거·`cycle_backward_walk_engine` 세 곳을 게이트한다. critical-path 구성에서는 walk가 아예
돌지 않는다.

---

## 8. TEA 잔여 코드 전수 감사 (2026-09-04)

critical-path 구성(TEA off, policy 0, critpath priority/RFP target on)에서 옛 코드가
**타이밍**이나 **우리 로직이 읽는 상태**에 영향을 주는지 경로별로 확인했다.

| 경로 | 판정 | 근거 |
|---|---|---|
| Fill Buffer + backward walk | **오염 → 수정** | PT 지명의 42.9%가 walk에서 옴 (§7). 지명 게이트 + walk 전체 게이트 |
| Frontend Block-Cache 태깅 (`cp_rfp`처럼 priority off·RFP on) | **stat 오염 → 수정** | `chain_bit`이 옛 slice 기준이라 `RFP_COVERED_FEEDS_H2P_BRANCH` 등이 다른 slice 정의로 측정됨. `RFP_TARGET_CRITPATH`면 chain_bit도 chain 멤버십에서 받도록 변경 |
| **`TEA_RS_RESERVATION=192`** | **의도된 것, 유지** — 단 논문 수치 주의 | TEA off에서도 `main_rs_limit = size − 192`. rs_sizes 285/204/55(544)는 TEA용으로 키운 값이고 main은 **352**를 쓴다. 모든 config 공통이라 비교는 공정하나, **논문의 RS 크기와 P-IQ 예약 %의 분모는 352** |
| `TEA_PREG_RESERVATION=192` | 영향 없음 | TEA 풀은 `init_tea_preg_pools`(TEA_ENABLE 게이트)에서만 분리. main PRF는 592 전부 사용 |
| SRT checkpoint (`reg_file_snapshot_srt`) | baseline 기능 | 모든 on-path branch에서 찍는 Scarab 기본 recovery 체크포인트. `exec_stage_tea_pending_flush_at_rename`은 `!TEA_ENABLE` 즉시 반환 |
| `reg_renaming_scheme_realistic_recover` | baseline 기능 | stat 이름(`TEA_RECOVER_CALLS_*`)만 TEA 흔적. 로직은 표준 rollback |
| HBT (`hbt_update` at retire, `hbt_pred_is_hard` at predict/retire) | **우리 설계 A1** | TEA 의존 없음 |
| `dcache_stage_try_main_chain_load_oracle` | 무효 | `H2P_CHAIN_PERFECT_LOAD=0`이면 첫 줄에서 FALSE |
| `tea_record_load_cache_access_order` | 무효 | 내부 첫 검사 `!TEA_ENABLE` 반환. stat 전용 |
| `cmp_wake` TEA stale-dep 정리 | 무효 | `!TEA_ENABLE \|\| !tea_is_active` 게이트 |
| exec/dcache/node/issue-queue의 `thread_id==1` 분기 | 무효 | 전부 `TEA_ENABLE &&` 게이트 |
| `record_on_off_path` | 무해 | 읽는 곳이 log 전용. walk 게이트로 이제 호출도 안 됨 |
| `zereco_rf_covered` | stat 전용 | walk의 policy-2 통계에서만 읽음 |
| `periodically_reset_caches` | 미호출 | `tea-random-queue` 브랜치의 미커밋 변경. `test`에는 없음 |
| dependency-chain / fill-buffer 로그 | 무효 | 로그 파일 핸들이 없으면 즉시 반환 |

**결론**: 타이밍에 개입하는 잔여 경로는 없다. 오염은 (1) walk의 PT 지명, (2) chain_bit의 slice
정의 두 건이었고 둘 다 수정했다. `TEA_RS_RESERVATION`은 기계 정의의 일부로 유지하되 논문에
352로 적어야 한다.

**재실행 필요**: Phase B(`260903`), B-1(`260904_B1`), B-2(`260904_B2`)는 오염된 코드로 돌았다.
P-IQ 단독 수치는 무관하지만 RFP 계열과 결합 수치가 영향을 받으므로 전량 재실행한다.
