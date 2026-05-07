# H2P Chain Perfect Load 실험 구현 기록

## 목적

TEA H2P dependency chain에 속한 Load 명령어의 cache miss latency가 misprediction 조기 감지의 bottleneck인지 검증한다.
`TEA_PERFECT_LOAD` 파라미터를 추가하여 TEA store buffer를 miss한 TEA load를 항상 L1 hit latency로 처리하는 oracle upper-bound 실험이다.
Main thread cache behavior는 완전히 보존된다.

---

## TEA load 경로 요약 (구현 기반)

```
TEA load (thread_id == 1) in dcache_stage.c
│
├─ TEA Store Buffer forwarding hit      (line ~258)
│     done_cycle = cycle_count + DCACHE_CYCLES
│     → TEA_LOADS_STORE_FORWARD
│
└─ TEA Store Buffer miss
      │
      ├─ TEA_PERFECT_LOAD == TRUE       (line ~275)
      │     done_cycle = cycle_count + latency  (static L1 latency)
      │     → TEA_LOADS_BYPASSED
      │
      └─ TEA_PERFECT_LOAD == FALSE: goto tea_load_dcache_access
            │
            ├─ PERFECT_DCACHE=1         → TEA_LOADS_DCACHE_HIT
            ├─ Real L1 hit              → TEA_LOADS_DCACHE_HIT
            ├─ L1 miss + scan_stores()  → TEA_LOADS_STORE_SCAN_FWD
            └─ L1 miss fill             → TEA_LOADS_DCACHE_MISS
```

---

## 구현 완료된 변경 사항

### 1. `src/tea/tea.stat.def` — 새 stat 11개 추가

파일 맨 끝에 추가 (line 277–289):

```c
/* [EXPERIMENT: TEA_PERFECT_LOAD] TEA load instrumentation stats */
DEF_STAT(TEA_CHAIN_LOAD_OPS_TOTAL, COUNT, NO_RATIO)  /* load ops in triggered dep chains */
DEF_STAT(TEA_LOADS_FETCHED,        COUNT, NO_RATIO)  /* load ops created by TEA fetch stage */
DEF_STAT(TEA_LOADS_EXECUTED,       COUNT, NO_RATIO)  /* TEA loads processed by dcache stage */
DEF_STAT(TEA_LOADS_STORE_FORWARD,  COUNT, NO_RATIO)  /* TEA loads satisfied by TEA store buffer */
DEF_STAT(TEA_LOADS_DCACHE_HIT,     COUNT, NO_RATIO)  /* TEA loads hitting real L1 (non-forwarded) */
DEF_STAT(TEA_LOADS_DCACHE_MISS,    COUNT, NO_RATIO)  /* TEA loads missing L1 (full memory latency) */
DEF_STAT(TEA_LOADS_STORE_SCAN_FWD, COUNT, NO_RATIO)  /* TEA loads forwarded by main store-buffer scan after L1 miss */
DEF_STAT(TEA_LOADS_BYPASSED,       COUNT, NO_RATIO)  /* TEA loads bypassed by TEA_PERFECT_LOAD */
DEF_STAT(TEA_LOAD_LATENCY_SAMPLES, COUNT, NO_RATIO)  /* samples for average latency */
DEF_STAT(TEA_LOAD_LATENCY_TOTAL,   COUNT, NO_RATIO)  /* sum of per-load latency (cycles) */
DEF_STAT(TEA_LOAD_LATENCY_AVG,     RATIO, TEA_LOAD_LATENCY_SAMPLES)
```

**주의**: `TEA_LOADS_STORE_SCAN_FWD`는 `dcache_cacheline_miss()` 안에서 `scan_stores()`가 히트한 경로다.
이는 real L1 hit이 아니므로 `TEA_LOADS_DCACHE_HIT`와 구분해야 한다.

**주의**: `RATIO` stat(`TEA_LOAD_LATENCY_AVG`)은 자동 계산되지 않는다.
`INC_STAT_EVENT(..., TEA_LOAD_LATENCY_AVG, value)`로 직접 증가시켜야 한다.

---

### 2. `src/core.param.def` — 파라미터 2개 추가

`tea_max_chains` 파라미터 아래에 추가 (line 386–388):

```c
/* [EXPERIMENT: TEA_PERFECT_LOAD] Perfect-load sensitivity experiment parameters */
DEF_PARAM(tea_perfect_load, TEA_PERFECT_LOAD, Flag, Flag, FALSE, )
DEF_PARAM(tea_perfect_load_latency, TEA_PERFECT_LOAD_LATENCY, uns, uns, 0, )
```

`TEA_PERFECT_LOAD_LATENCY == 0`이면 코드에서 `DCACHE_CYCLES`를 사용한다 (golden_cove 기준 5 cycles).

---

### 3. `src/tea/tea_thread.c` — trigger 시 chain 내 load op 수 계산

`record_chain_length_bucket()` 호출 바로 뒤에 추가 (line 306–311):

```c
/* [EXPERIMENT: TEA_PERFECT_LOAD] Count load ops in triggered dependency chain */
for (uns ci = 0; ci < chain->chain_length; ci++) {
  if (chain->chain[ci].table_info &&
      chain->chain[ci].table_info->mem_type == MEM_LD)
    STAT_EVENT(proc_id, TEA_CHAIN_LOAD_OPS_TOTAL);
}
```

`chain`은 `Dependency_Chain_Cache_Entry*`이고 `chain->chain[ci]`는 `Op` 배열이다.

---

### 4. `src/tea/tea_fetch_stage.c` — fetch stage에서 TEA load stat

`tea_create_op_from_cache()` 함수 내 `STAT_EVENT(proc_id, TEA_OPS_FETCHED)` 직후에 추가 (line 301–302):

```c
/* [EXPERIMENT: TEA_PERFECT_LOAD] Count fetched TEA load ops */
if (cached_op->table_info && cached_op->table_info->mem_type == MEM_LD)
  STAT_EVENT(proc_id, TEA_LOADS_FETCHED);
```

---

### 5. `src/dcache_stage.c` — TEA load stat 6개 위치 추가 + perfect-load bypass 분기

TEA op 식별은 `TEA_ENABLE && op->thread_id == 1`로 한다.

#### Area 1: TEA Store Buffer forwarding hit (~line 267)

TEA store buffer read 성공 후 `wake_up_ops()` 직후에 추가:

```c
/* [EXPERIMENT: TEA_PERFECT_LOAD] store-forward hit stats */
STAT_EVENT(op->proc_id, TEA_LOADS_EXECUTED);
STAT_EVENT(op->proc_id, TEA_LOADS_STORE_FORWARD);
STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_SAMPLES);
INC_STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_TOTAL, DCACHE_CYCLES);
INC_STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_AVG,   DCACHE_CYCLES);
```

latency = `DCACHE_CYCLES` (forwarding = L1 hit latency로 간주)

#### Area 2: TEA_PERFECT_LOAD bypass (~line 274)

`goto tea_load_dcache_access` 대신 조건부 분기로 교체. 원래 코드는 주석으로 보존:

```c
/* [EXPERIMENT: TEA_PERFECT_LOAD] original: goto tea_load_dcache_access; */
if (TEA_PERFECT_LOAD) {
  /* Ideal upper bound: bypass real dcache/memory, apply static L1 latency */
  Counter latency = TEA_PERFECT_LOAD_LATENCY ? TEA_PERFECT_LOAD_LATENCY : DCACHE_CYCLES;
  op->done_cycle = cycle_count + latency;
  op->wake_cycle = cycle_count + latency;
  op->state = OS_SCHEDULED;
  op->oracle_info.dcmiss = FALSE;
  wake_up_ops(op, REG_DATA_DEP, model->wake_hook);
  STAT_EVENT(op->proc_id, TEA_LOADS_EXECUTED);
  STAT_EVENT(op->proc_id, TEA_LOADS_BYPASSED);
  STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_SAMPLES);
  INC_STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_TOTAL, latency);
  INC_STAT_EVENT(op->proc_id, TEA_LOAD_LATENCY_AVG,   latency);
  /* falls through to tea_op_completed + stage removal below */
} else {
  goto tea_load_dcache_access;
}
```

`tea_op_completed()` 호출은 이 블록 이후 공통 경로에서 처리된다.

#### Area 3: PERFECT_DCACHE 경로 (~line 354)

`PERFECT_DCACHE` 분기에서 TEA load가 완료될 때 추가:

```c
if (TEA_ENABLE && op->thread_id == 1) {
  /* [EXPERIMENT: TEA_PERFECT_LOAD] PERFECT_DCACHE hit stats */
  {
    Counter _lat = op->done_cycle - cycle_count;
    STAT_EVENT(dc->proc_id, TEA_LOADS_EXECUTED);
    STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_HIT);
    STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_SAMPLES);
    INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_TOTAL, _lat);
    INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_AVG,   _lat);
  }
  tea_op_completed(dc->proc_id, op);
}
```

latency = `op->done_cycle - cycle_count` (`DCACHE_CYCLES + extra_ld_latency`)

#### Area 4: Real L1 hit 경로 (~line 372)

`dcache_cacheline_hit()` 호출 후 TEA load 완료 시 추가:

```c
if (TEA_ENABLE && op->thread_id == 1) {
  /* [EXPERIMENT: TEA_PERFECT_LOAD] real L1 hit stats */
  {
    Counter _lat = op->done_cycle - cycle_count;
    STAT_EVENT(dc->proc_id, TEA_LOADS_EXECUTED);
    STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_HIT);
    STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_SAMPLES);
    INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_TOTAL, _lat);
    INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_AVG,   _lat);
  }
  tea_op_completed(dc->proc_id, op);
}
```

latency = `op->done_cycle - cycle_count` (`DCACHE_CYCLES + extra_ld_latency`)

#### Area 5: L1 miss 후 scan_stores() forwarding (~line 704)

`dcache_cacheline_miss()` 내부, `scan_stores()` 히트 경로에 추가.
이 경로는 L1 miss인데 메모리 요청 버퍼 안의 store가 주소를 커버한 경우다.
`TEA_LOADS_DCACHE_HIT`가 아닌 별도 stat `TEA_LOADS_STORE_SCAN_FWD`를 사용한다:

```c
if (TEA_ENABLE && op->thread_id == 1) {
  /* [EXPERIMENT: TEA_PERFECT_LOAD] main store-scan forward-on-miss stats.
   * This is NOT an L1 hit — it's a dcache_cacheline_miss() path where
   * scan_stores() found a matching store in the memory request buffer. */
  Counter _lat = op->done_cycle - cycle_count;
  STAT_EVENT(dc->proc_id, TEA_LOADS_EXECUTED);
  STAT_EVENT(dc->proc_id, TEA_LOADS_STORE_SCAN_FWD);
  STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_SAMPLES);
  INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_TOTAL, _lat);
  INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_AVG,   _lat);
  tea_op_completed(dc->proc_id, op);
}
```

#### Area 6: L1 miss fill 완료 — `dcache_fill_line()` (~line 962)

메모리에서 데이터가 도착하여 fill이 완료될 때 (`dcache_fill_line()` 함수 내) 추가:

```c
if (TEA_ENABLE && op->thread_id == 1) {
  /* [EXPERIMENT: TEA_PERFECT_LOAD] L1 miss fill stats (full memory latency) */
  STAT_EVENT(dc->proc_id, TEA_LOADS_EXECUTED);
  STAT_EVENT(dc->proc_id, TEA_LOADS_DCACHE_MISS);
  if (op->dcache_cycle != MAX_CTR && op->done_cycle >= op->dcache_cycle) {
    Counter _lat = op->done_cycle - op->dcache_cycle;
    STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_SAMPLES);
    INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_TOTAL, _lat);
    INC_STAT_EVENT(dc->proc_id, TEA_LOAD_LATENCY_AVG,   _lat);
  }
  tea_op_completed(dc->proc_id, op);
}
```

**주의**: `op->dcache_cycle`은 `op_pool.c:226`에서 `MAX_CTR`로 초기화된다.
`> 0` guard는 항상 참이므로 `!= MAX_CTR` sentinel check를 사용한다.
`done_cycle >= dcache_cycle` 체크로 underflow 방지한다.
latency = `op->done_cycle - op->dcache_cycle` (dcache access 시작부터 fill 완료까지)

---

## 파일별 변경 요약

| 파일 | 변경 내용 |
|------|----------|
| `src/tea/tea.stat.def` | TEA load stat 11개 추가 |
| `src/core.param.def` | `TEA_PERFECT_LOAD`, `TEA_PERFECT_LOAD_LATENCY` 파라미터 추가 |
| `src/tea/tea_thread.c` | trigger 시 dep chain 내 load op 수 카운트 |
| `src/tea/tea_fetch_stage.c` | TEA fetch에서 `TEA_LOADS_FETCHED` 카운트 |
| `src/dcache_stage.c` | 6개 위치에 TEA load stat + perfect-load bypass 분기 추가 |

---

## 실험 설정 (`~/scarab-infra/json/tea_dbg.json`)

```json
"tea_baseline":     "--tea_perfect_load 0 --tea_perfect_load_latency 0"
"tea_perfect_load": "--tea_perfect_load 1 --tea_perfect_load_latency 0"
```

`TEA_PERFECT_LOAD_LATENCY=0`이면 `DCACHE_CYCLES` 사용 (golden_cove: `--dcache_cycles 5` → 5 cycles).

---

## 결과 분석 지표 (Periodic 기준)

| 지표 | 의미 |
|------|------|
| `TEA_CHAIN_LOAD_OPS_TOTAL / TEA_CHAIN_LENGTH_TOTAL` | triggered chain 내 load 비율 |
| `TEA_LOADS_FETCHED / TEA_OPS_FETCHED` | 실제 TEA fetch op 중 load 비율 |
| `TEA_LOADS_DCACHE_MISS / (TEA_LOADS_DCACHE_HIT + TEA_LOADS_DCACHE_MISS)` | baseline real dcache miss rate |
| `TEA_LOAD_LATENCY_AVG` | baseline vs perfect-load 평균 latency 비교 |
| `TEA_LOADS_EXECUTED ≈ TEA_LOADS_STORE_FORWARD + TEA_LOADS_BYPASSED` | perfect-load run 정합성 확인 |
| `TEA_EARLY_FLUSH_TRIGGER_TO_DETECT_AVG`, `TEA_H2P_TRIGGER_TO_EXEC_AVG` | detect timing 개선 여부 |
| `TEA_EARLY_FLUSHES` | 조기 감지 건수 증가 여부 |
| `TEA_EARLY_FLUSH_TOO_LATE_MAIN_RECOVERY` | too-late 감소 여부 |
| `BP_MISP_PENALTY` | 실질 성능 개선 |
| Periodic IPC | 최종 비교 기준 |

---

## 해석 시 주의사항

| 항목 | 내용 |
|------|------|
| TEA op 식별 | `op->thread_id == 1` — `is_tea_op` 플래그 없음 |
| TEA_PERFECT_LOAD bypass 범위 | TEA store buffer miss인 TEA load만 대상. Store buffer hit path는 기존 코드 그대로 유지 |
| bypass 낙관성 | cache port 미소모, dcache metadata 미갱신, scan_stores() 미실행 → 가장 강한 upper bound |
| scan_stores() | `TEA_LOADS_STORE_SCAN_FWD`는 L1 hit이 아님 — miss rate 계산 시 구분 필요 |
| TEA load ↔ LSQ | TEA op는 `tea_dispatch_to_rs()`를 통해 dispatch되며 `lsq_dispatch()` 경로에 진입하지 않음 |
| `TEA_LOADS_BYPASSED` | baseline `TEA_LOADS_DCACHE_MISS`보다 많을 수 있음 (non-forwarded TEA load 전체를 bypass하므로) |
| 개선 원인 분해 | (1) miss latency 제거, (2) cache port 미점유, (3) H2P 더 빨리 execute, (4) backend pressure 완화 — 이 중 (1)이 prefetch 아이디어와 직결 |
| 코드 보존 원칙 | 기존 코드 삭제 금지. `goto tea_load_dcache_access` 원문은 주석으로 보존됨 |
