# TEA Case 1 Early Flush 즉시 Recovery 최적화

## 배경 및 문제

현재 Case 1 동작 (SRT checkpoint 없을 때):

```
Cycle T:     TEA exec → mispred 감지 → tea_record_pending_case1_flush()
Cycle T+k:   Main H2P가 rename에 도달 → SRT snapshot → exec_stage_tea_pending_flush_at_rename() → bp_sched_recovery()
Cycle T+k+1: cmp_recover() fires
```

`k` = Main H2P가 rename에 도달할 때까지의 지연 (decode/IDQ에 있으면 수 cycle).  
이 지연 동안 파이프라인은 잘못된 경로의 instruction을 계속 fetch/decode/rename한다.

---

## 핵심 통찰: 즉시 bp_sched_recovery() 호출 가능

`reg_renaming_scheme_realistic_recover()` (map_rename.c:1006-1012):

```c
// TEA Case 1: No SRT checkpoint means recovery_op has not yet passed rename.
// In-order rename guarantees no ops after it have been renamed either,
// so there are no off-path pregs to free and no SRT rollback needed.
if (!reg_file_checkpoint_is_valid()) {
    STAT_EVENT(op->proc_id, TEA_RECOVER_CALLS_SKIPPED);
    return;
}
```

SRT checkpoint 없이 `bp_sched_recovery()`를 호출하더라도:
- Recovery가 rename 전에 fire: `!checkpoint_is_valid` → SRT rollback skip, 물리 레지스터 누수 없음 (in-order rename 보장 — main H2P 이후 rename된 op 없음)
- Recovery가 rename 후에 fire: main H2P가 rename 시 SRT snapshot 생성 → 정상 SRT rollback

따라서 TEA exec 시점에 즉시 `bp_sched_recovery()`를 호출해도 안전하다.

---

## 제안: Case 1 즉시 Recovery 스케줄링

```
Cycle T:     TEA exec → mispred 감지 → bp_sched_recovery() 즉시 호출
Cycle T+1:   cmp_recover() fires (recovery_cycle = T + EXTRA_LATE_RECOVERY_CYCLES + 1)
```

---

## Frontend Partial Flush 분석

### 논문 §IV-F 요구사항

논문은 TEA early flush를 위해 frontend에서 **partial flush** 지원을 명시한다:
> "we support partial flushes in the processor frontend, i.e. instructions older than the mispredicting branch in the frontend pipeline stages are also flushed"
> "partial flushes in the frontend are supported by adding a **comparator before the flush signal for each pipeline stage**"
> "The Fetch Queue is also partially flushed"

### 현재 Scarab 구현 상태

| 스테이지 | 함수 | Partial flush | 비고 |
|--------|------|:---:|------|
| ROB/RS | `recover_node_stage()` | ✓ | `FLUSH_OP` macro 사용 |
| Map/Rename | `recover_map_stage()` | ✓ | `FLUSH_OP` 사용 |
| Decode | `recover_decode_stage()` | ✓ | `FLUSH_OP` 사용 |
| IDQ | `IDQ_Stage::recover()` | ✓ | `FLUSH_OP` 사용 |
| **Icache** | `recover_icache_stage()` | **✗** | **ASSERT — 모든 op이 off-path라고 전제** |
| FTQ | `Decoupled_FE::recover()` | 사실상 ✓ | Full flush이지만 Case 1에서 동등 (아래 참조) |
| SRT | `reg_renaming_scheme_realistic_recover()` | ✓ | `!checkpoint_is_valid` → skip |

### Icache Stage 문제: 즉시 Recovery 충돌 (`icache_stage.c:222-228`)

```c
void recover_icache_stage() {
  Stage_Data* cur_data = get_current_stage_data();
  for (ii = 0; ii < cur_data->max_op_count; ii++) {
    if (cur_data->ops[ii]) {
      ASSERT(ic->proc_id, FLUSH_OP(cur_data->ops[ii]));  // ← 모든 op이 flush 대상이어야 한다고 전제
      ASSERT(ic->proc_id, cur_data->ops[ii]->off_path);  // ← 모든 op이 off_path여야 한다고 전제
      free_op(cur_data->ops[ii]);
      cur_data->ops[ii] = NULL;
    }
  }
  cur_data->op_count = 0;
  ...
}
```

**기존 deferred recovery에서는 이 전제가 성립**:
- Rename 시점에 recovery fire → main H2P는 이미 icache 통과 → icache buffer에는 H2P 이후 wrong-path op만 존재 → `FLUSH_OP` = TRUE, `off_path` = TRUE ✓

**즉시 recovery에서는 충돌 가능**:

```
Cycle T:
  update_exec_stage()   → TEA Case1 감지 → bp_sched_recovery() → recovery_cycle = T+1
  update_icache_stage() → main H2P(PRE_DECODE 상태) fetch → icache output buffer 배치

Cycle T+1:
  cmp_istreams() → cmp_recover() → recover_icache_stage()
    → main H2P가 icache output buffer에서 발견
    → ASSERT(FLUSH_OP(main_h2p)) = ASSERT(FALSE) → CRASH !!
    → ASSERT(main_h2p->off_path)  = ASSERT(FALSE) → CRASH !!
```

`decode_cycle == 0`(`PRE_DECODE`)인 main H2P가 TEA exec과 같은 cycle에 fetch될 때 발생.  
이 경우 `EXTRA_LATE_RECOVERY_CYCLES = 0`(기본값)이면 T+1에서 crash.

**발생 조건**: TEA가 main H2P의 icache fetch와 거의 동시에 TEA H2P를 실행하는 경우 (TEA가 frontier 근처에 있을 때). 드물지만 correctness 문제이므로 반드시 수정 필요.

### FTQ Partial Flush — ⚠️ 즉시 Recovery에서 심각한 문제

**Op 생성 위치**: Scarab에서 Op는 icache가 아닌 **DFE(Decoupled Frontend)** 단계에서 생성됩니다
(`decoupled_frontend.cc:343-356`):

```c
Op* op = alloc_op(proc_id);        // Op 생성 — FTQ 배치 전
frontend_fetch_op(proc_id, op);    // oracle 정보 채움
bp_predict_op(..., op, ...);       // TEA trigger 발생 ← 이 시점에 trigger_tea_thread() 호출
current_ft_to_push.add_op(op, ...);
ftq.emplace_back(current_ft_to_push); // FTQ에 FT(Op 묶음) 배치
```

Icache는 FTQ에서 FT를 가져와 `ft_fetch_op(ft)`로 **기존 Op를 그대로** 꺼냅니다 (새 Op 생성 X).
즉, **`c->main_h2p_op`는 아직 FTQ 안에 있는 Op를 가리킬 수 있습니다**.

`decode_cycle == 0`(`PRE_DECODE`) 상태에는 두 가지가 섞여있습니다:
- **FTQ 내 Op**: `fetch_cycle == 0` (icache가 아직 소비하지 않음)
- **Icache output buffer 내 Op**: `fetch_cycle > 0`, `decode_cycle == 0`

**Main H2P가 FTQ 안에 있을 때 즉시 Recovery가 fire되는 시나리오**:

```
FTQ 상태 (main H2P = position N, icache = position M, M < N):
  pos M: ic->current_ft (일부 fetch 중)
  pos M+1 ~ N-1: correct-path FTs (아직 icache 미소비)
  pos N: main H2P FT (아직 icache 미소비)
  pos N+1~: wrong-path FTs

cmp_recover() 실행:
  recover_decoupled_fe():
    → 전체 FTQ flush (free_ops_and_clear for all FTs)
    → M+1~N-1 (correct-path) ops: free_op() ← 올바른 경로 instructions 해제!
    → N (main H2P): free_op(main_h2p) ← main H2P 자체도 해제!
  dfe_op_count = main_h2p->op_num + 1
  recovery_addr = main_h2p->oracle_info.npc  ← main H2P 이후부터 재fetch

  recover_icache_stage():
    → icache output buffer에 pos M의 on-path ops 존재 (op_num < main_h2p->op_num)
    → ASSERT(FLUSH_OP(op)) = ASSERT(FALSE) → CRASH!!
```

**두 가지 문제**:
1. `recover_icache_stage()` ASSERT crash (icache buffer에 on-path op 존재)
2. **(더 심각)** FTQ 안의 M+1 ~ N 범위의 correct-path instructions 해제 → re-fetch는 main H2P NPC부터 → M+1 ~ N 구간의 instructions 영원히 실행되지 않음 → **파이프라인 correctness 위반**

**논문의 Partial FTQ Flush가 요구하는 것**:
- pos N+1 이후의 wrong-path FTs만 free
- pos M ~ N(main H2P 포함)은 보존 → icache가 계속 소비
- DFE BP generation을 main_h2p->oracle_info.npc부터 재개

이것이 "full misprediction penalty saved"의 실제 의미입니다.

**결론**: FTQ case에서의 즉시 Recovery는 **Partial FTQ Flush 구현 없이는 불가능**합니다.

---

## 해결 방향 비교

FTQ Case 문제("FTQ Partial Flush — ⚠️" 섹션 참조)를 해결하기 위한 두 가지 방향:

### Option A: Partial FTQ Flush (논문 완전 구현)

FTQ flush 시 `recovery_op_num` 이하의 FT는 보존, 초과분만 해제.
Case 1 즉시 recovery를 **모든 경우**에 적용.

**추가 변경 파일**:
- `src/ft.h / ft.cc` — `FT::free_ops_after_opnum(Counter)` 신규 메서드
- `src/decoupled_frontend.cc` — `Decoupled_FE::recover()` partial FTQ flush 로직

**장점**: 논문 §IV-F 완전 구현. FTQ 내 main H2P도 즉시 recovery → 최대 성능.

**단점**: 구현 복잡도 높음.
- FT 클래스 신규 메서드 필요
- `Decoupled_FE::recover()` 내 FTQ partial erase 로직 — `ftq_iterators`(`ft_pos` / `op_pos` / `flattened_op_pos`) 무효화 처리 필요
- 새 코드 경로로 인한 버그 위험

---

### Option B: Hybrid — fetch_cycle 분기 (단순 구현)

`main_h2p->fetch_cycle > 0`(icache 통과 완료) 여부로 즉시/지연 분기.
- `fetch_cycle > 0`: FTQ 밖에 있음 → 즉시 `bp_sched_recovery()`
- `fetch_cycle == 0`: FTQ 안에 있음 → 기존 `tea_record_pending_case1_flush()` 유지

**추가 변경 파일**: 없음 (기존 변경 범위 동일).

**장점**: 구현 단순, 버그 위험 낮음. 대부분의 Case 1 즉시 recovery 이득 획득.

**단점**: FTQ 내 main H2P Case 1은 여전히 deferred → 해당 케이스 성능 개선 없음.
논문 §IV-F와 완전 일치하지 않음.

---

### 비교 요약

| 항목 | Option A (Partial FTQ Flush) | Option B (Hybrid) |
|------|-----|-----|
| 즉시 recovery 적용 대상 | 모든 Case 1 | `fetch_cycle > 0` Case 1만 |
| FTQ 케이스 처리 | Partial FTQ flush → 즉시 recovery | 기존 deferred 유지 |
| 구현 복잡도 | 높음 (FT + DFE 수정) | 낮음 (분기 추가만) |
| 버그 위험 | 높음 | 낮음 |
| 논문 충실도 | ✓ 완전 구현 | △ 부분 구현 |
| 성능 이득 | 최대 | 부분 (FTQ 케이스 제외) |
| 필수 공통 변경 | `icache_stage.c` ASSERT 수정 | `icache_stage.c` ASSERT 수정 |

**FTQ 케이스 빈도 추정**:
- `TEA_EARLY_FLUSH_CASE1_NO_CHKPT`(=2,720): `decode_cycle > 0` → 반드시 `fetch_cycle > 0` → Option B로도 즉시 recovery 적용 ✓
- `TEA_EARLY_FLUSH_CASE1_DECODE`(=1,944): `decode_cycle == 0` → FTQ 또는 icache output buffer — 이 중 일부만 FTQ 케이스

따라서 Option B의 성능 손실 = 1,944 중 `fetch_cycle == 0`인 케이스(FTQ 안)의 비율에 비례.
이 비율이 낮다면 Option B로도 대부분의 성능 이득 달성 가능.

---

## Option A 구현 계획: Partial FTQ Flush

### 변경 파일 요약

| 파일 | 변경 내용 |
|------|-----------|
| `src/ft.h / ft.cc` | `FT::free_ops_after_opnum()` 신규 메서드 추가 |
| `src/decoupled_frontend.cc` | `Decoupled_FE::recover()` — partial FTQ flush 로직 |
| `src/exec_stage.c` | Case 1 handler: pending flush 대신 즉시 `bp_sched_recovery()` 호출 (모든 경우) |
| **`src/icache_stage.c`** | **`recover_icache_stage()` ASSERT → 조건부 FLUSH_OP partial flush로 수정** |
| `src/tea/tea.stat.def` | `TEA_EARLY_FLUSH_CASE1_IMMEDIATE` 신규 stat 추가 |
| `src/map_rename.c` | 변경 없음 (rename hook은 `recovery_sch=TRUE` guard로 no-op됨) |
| `src/tea/tea_thread.h/c` | 변경 없음 (pending table 코드 유지, 단순 no-op) |

---

### Step 0: `src/ft.h / ft.cc` — `FT::free_ops_after_opnum()` 메서드 추가

**`src/ft.h`** — public 메서드 선언 추가:
```cpp
void free_ops_after_opnum(Counter recovery_op_num);
```

**`src/ft.cc`** — 구현 추가:
```cpp
/* Partial FTQ flush support: free unfetched ops with op_num > recovery_op_num
 * and truncate ops vector so fetch_op() stops at recovery_op. */
void FT::free_ops_after_opnum(Counter recovery_op_num) {
  size_t new_size = op_pos;  // start: preserve already-fetched region
  for (size_t i = op_pos; i < ops.size(); i++) {
    if (ops[i] && ops[i]->op_num <= recovery_op_num) {
      new_size = i + 1;  // keep this op
    } else if (ops[i]) {
      free_op(ops[i]);
    }
  }
  ops.resize(new_size);
}
```

`op_num`은 FT 내에서 단조 증가(multi-uop 고려 시 non-decreasing)하므로 단순 구간 탐색으로 경계를 찾을 수 있다.

---

### Step 1: `src/decoupled_frontend.cc` — `Decoupled_FE::recover()` Partial FTQ Flush

**위치**: `Decoupled_FE::recover()` (line 217) — FTQ flush 루프 수정.

**현재 코드**:
```cpp
for (auto it = ftq.begin(); it != ftq.end(); it++) {
    it->free_ops_and_clear();
}
ftq.clear();
```

**변경 후 코드**:
```cpp
Counter recovery_op_num = bp_recovery_info->recovery_op_num;
bool recovery_ft_found = false;
size_t num_kept_fts = 0;

for (auto it = ftq.begin(); it != ftq.end(); ) {
    if (recovery_ft_found) {
        /* This FT comes after the one containing recovery_op — free entirely */
        it->free_ops_and_clear();
        it = ftq.erase(it);
    } else {
        /* Check if recovery_op is in this FT */
        bool contains_recovery = false;
        bool entirely_after = true;
        for (size_t i = it->op_pos; i < it->ops.size(); i++) {
            Op* op = it->ops[i];
            if (op && op->op_num <= recovery_op_num) {
                entirely_after = false;
                if (op->op_num == recovery_op_num)
                    contains_recovery = true;
            }
        }

        if (entirely_after) {
            /* All unfetched ops in this FT are wrong-path */
            it->free_ops_and_clear();
            it = ftq.erase(it);
        } else if (contains_recovery) {
            /* Free ops after recovery_op in this FT, keep the rest */
            it->free_ops_after_opnum(recovery_op_num);
            recovery_ft_found = true;
            num_kept_fts++;
            ++it;
        } else {
            /* All unfetched ops <= recovery_op — keep entirely */
            num_kept_fts++;
            ++it;
        }
    }
}
```

**`ftq_iterators` 처리** (`ft_pos` / `op_pos` / `flattened_op_pos` 무효화):

`ftq.erase(it)`로 back-end FTs를 제거하므로 front-end에 있는 iterator들의 `ft_pos`는 유효하게 유지된다.
`ft_pos >= num_kept_fts`인 iterator는 제거된 FT를 가리키므로 reset 필요:

```cpp
for (auto& iter : ftq_iterators) {
    if (iter.ft_pos >= (int)num_kept_fts) {
        /* Iterator was pointing into the erased portion — reset to end of kept region */
        iter.ft_pos = (int)num_kept_fts;
        iter.op_pos = 0;
        iter.flattened_op_pos = /* recalculate from kept FTs */;
    }
    /* ft_pos < num_kept_fts: iterator remains valid, no change */
}
```

`flattened_op_pos` 재계산:
```cpp
uint64_t kept_op_count = 0;
for (size_t fi = 0; fi < num_kept_fts; fi++)
    kept_op_count += ftq[fi].ops.size() - ftq[fi].op_pos;
```

`Decoupled_FE`는 `FT`의 `friend`이므로 `it->op_pos`, `it->ops` 등에 직접 접근 가능.

**`dfe_op_count` 설정**: 기존과 동일 — `dfe_op_count = recovery_op_num + 1`.

**`recovery_addr` 설정**: `recover()` 앞부분의 `recovery_addr = bp_recovery_info->recovery_fetch_addr`는 DFE가 main H2P NPC부터 새 ops를 생성하도록 설정 — 변경 없음. `frontend_recover()`도 동일 주소로 재설정.

---

### Step 2 (구 Step 1): `src/exec_stage.c` — Case 1 Handler 수정

**위치**: 현재 Case 1 handler (line ~909, `} else {` branch):

**현재 코드**:
```c
} else {
  /* Case 1: No SRT checkpoint yet — record pending flush so recovery
   * fires when main H2P reaches rename and the checkpoint is created. */
  Tea_Case1_Main_Stage main_stage_at_detect =
    tea_classify_case1_main_stage(main_h2p);
  Flag pending_recorded =
    tea_record_pending_case1_flush(op->proc_id, main_h2p,
                                   op->exec_cycle,
                                   main_stage_at_detect);
  if (pending_recorded) {
    tea_mark_early_flush_detection(main_h2p, op->exec_cycle);
    main_h2p->tea_case1_detect_cycle = op->exec_cycle;
    STAT_EVENT(op->proc_id, TEA_EARLY_FLUSHES);
    tea_record_case1_main_stage(op->proc_id, main_stage_at_detect);
    tea_record_early_flush_time_to_detect(
      op->proc_id, c, op,
      TEA_EARLY_FLUSH_TRIGGER_TO_DETECT_SAMPLES, ...);
    tea_record_early_flush_time_to_detect(
      op->proc_id, c, op,
      TEA_EARLY_FLUSH_CASE1_TRIGGER_TO_DETECT_SAMPLES, ...);
  }
  if (main_h2p->decode_cycle)
    STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_NO_CHKPT);
  else
    STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_DECODE);
  terminate_tea_chain_with_reason(op->proc_id, chain_slot,
                                  TEA_CHAIN_TERM_REASON_EARLY_FLUSH_CASE1);
}
```

**변경 후 코드**:
```c
} else {
  /* Case 1: No SRT checkpoint yet.
   * Schedule recovery immediately — reg_renaming_scheme_realistic_recover()
   * already handles !checkpoint_is_valid by skipping SRT rollback (safe:
   * in-order rename guarantees no off-path pregs before main H2P). */
  Tea_Case1_Main_Stage main_stage_at_detect =
    tea_classify_case1_main_stage(main_h2p);
  bp_sched_recovery(bp_recovery_info, main_h2p, op->exec_cycle,
                    FALSE, FALSE, EXTRA_LATE_RECOVERY_CYCLES);
  if (main_h2p->oracle_info.recovery_sch) {
    main_h2p->recovery_scheduled = TRUE;
    main_h2p->tea_case1_pending_recovery = TRUE;
    main_h2p->tea_case1_detect_cycle = op->exec_cycle;
    tea_mark_early_flush_detection(main_h2p, op->exec_cycle);
    STAT_EVENT(op->proc_id, TEA_EARLY_FLUSHES);
    STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_IMMEDIATE);
    tea_record_case1_main_stage(op->proc_id, main_stage_at_detect);
    tea_record_early_flush_time_to_detect(
      op->proc_id, c, op,
      TEA_EARLY_FLUSH_TRIGGER_TO_DETECT_SAMPLES, ...);
    tea_record_early_flush_time_to_detect(
      op->proc_id, c, op,
      TEA_EARLY_FLUSH_CASE1_TRIGGER_TO_DETECT_SAMPLES, ...);
  }
  if (main_h2p->decode_cycle)
    STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_NO_CHKPT);
  else
    STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_DECODE);
  terminate_tea_chain_with_reason(op->proc_id, chain_slot,
                                  TEA_CHAIN_TERM_REASON_EARLY_FLUSH_CASE1);
}
```

**주요 변경점**:
- `tea_record_pending_case1_flush()` 제거 → `bp_sched_recovery()` 즉시 호출
- `pending_recorded` flag 대신 `main_h2p->oracle_info.recovery_sch` check
- `tea_case1_pending_recovery = TRUE` 및 `tea_case1_detect_cycle` 설정 (cmp_recover stat용)
- `TEA_EARLY_FLUSH_CASE1_IMMEDIATE` 신규 stat 추가
- **`recover_at_exec = FALSE`로 설정하지 않음** (중요):
  - Main H2P가 rename에 도달하면 `recover_at_exec = TRUE`이므로 SRT snapshot을 생성
  - Recovery가 rename 후에 fire되면 SRT rollback이 정상 작동
  - `exec_stage_tea_pending_flush_at_rename()` hook은 호출되지만 `recovery_sch = TRUE` → no-op

**주의**: `bp_sched_recovery()`는 내부에서 `ASSERT(!op->oracle_info.recovery_sch)` 호출.  
따라서 이 함수를 호출하기 전 `recovery_sch` 상태를 확인해야 한다.  
현재 코드 구조 (`if (!main_h2p->oracle_info.recovery_sch) {` guard, line 879) 안에서 이미 실행되므로 안전하다.

---

### Step 3 (구 Step 2): `src/icache_stage.c` — `recover_icache_stage()` Partial Flush 지원

**현재 코드** (ASSERT로 모든 op이 off-path라고 전제):
```c
void recover_icache_stage() {
  Stage_Data* cur_data = get_current_stage_data();
  for (ii = 0; ii < cur_data->max_op_count; ii++) {
    if (cur_data->ops[ii]) {
      ASSERT(ic->proc_id, FLUSH_OP(cur_data->ops[ii]));  // ← 제거 필요
      ASSERT(ic->proc_id, cur_data->ops[ii]->off_path);  // ← 제거 필요
      free_op(cur_data->ops[ii]);
      cur_data->ops[ii] = NULL;
    }
  }
  cur_data->op_count = 0;
  ...
}
```

**변경 후 코드** (다른 frontend stage와 동일하게 `FLUSH_OP` 조건부 처리):
```c
void recover_icache_stage() {
  Stage_Data* cur_data = get_current_stage_data();
  cur_data->op_count = 0;
  for (ii = 0; ii < cur_data->max_op_count; ii++) {
    if (cur_data->ops[ii]) {
      if (FLUSH_OP(cur_data->ops[ii])) {
        /* Normal case: wrong-path op in icache output buffer */
        free_op(cur_data->ops[ii]);
        cur_data->ops[ii] = NULL;
      } else {
        /* TEA Case 1: on-path op (main H2P or older) — keep it.
         * Happens when immediate recovery fires before main H2P exits icache. */
        cur_data->op_count++;
      }
    }
  }
  ...  /* 나머지 코드 동일 (ic->back_on_path, op_count[ic->proc_id], etc.) */
}
```

**주의**: 기존 deferred recovery에서는 icache buffer에 on-path op이 없으므로 동작 변화 없음.  
즉시 recovery에서만 차이가 발생한다 (PRE_DECODE main H2P가 icache buffer에 있을 때).

---

### Step 4 (구 Step 3): `src/tea/tea.stat.def` — 신규 Stat 추가

기존 `TEA_EARLY_FLUSH_CASE1_NO_CHKPT` 근처에 추가:

```c
DEF_STAT(TEA_EARLY_FLUSH_CASE1_IMMEDIATE, COUNT, PER_CORE)
```

---

## Option B 구현 계획: Hybrid (fetch_cycle 분기)

### 변경 파일 요약

| 파일 | 변경 내용 |
|------|-----------|
| `src/exec_stage.c` | Case 1 handler: `fetch_cycle > 0` 분기 — fetch 완료 시 즉시, FTQ 시 deferred |
| **`src/icache_stage.c`** | **`recover_icache_stage()` ASSERT → 조건부 FLUSH_OP (Option A Step 3과 동일)** |
| `src/tea/tea.stat.def` | `TEA_EARLY_FLUSH_CASE1_IMMEDIATE` 신규 stat 추가 (Option A Step 4와 동일) |
| `src/decoupled_frontend.cc` | 변경 없음 (FTQ 케이스는 deferred 유지) |
| `src/ft.h / ft.cc` | 변경 없음 |
| `src/map_rename.c` | 변경 없음 |
| `src/tea/tea_thread.h/c` | 변경 없음 (pending table 계속 사용) |

---

### Step 1: `src/exec_stage.c` — Case 1 Handler fetch_cycle 분기

**위치**: 현재 Case 1 handler (line ~909, `} else {` branch):

**변경 후 코드**:
```c
} else {
  /* Case 1: No SRT checkpoint yet.
   * If main H2P is past icache (fetch_cycle > 0), schedule recovery immediately —
   * reg_renaming_scheme_realistic_recover() handles !checkpoint_is_valid safely.
   * If main H2P is still in FTQ (fetch_cycle == 0), defer to rename stage
   * to avoid FTQ correctness violation (full FTQ flush would lose on-path ops). */
  Tea_Case1_Main_Stage main_stage_at_detect =
    tea_classify_case1_main_stage(main_h2p);

  if (main_h2p->fetch_cycle > 0) {
    /* Immediate recovery — Op is past FTQ, no FTQ correctness issue */
    bp_sched_recovery(bp_recovery_info, main_h2p, op->exec_cycle,
                      FALSE, FALSE, EXTRA_LATE_RECOVERY_CYCLES);
    if (main_h2p->oracle_info.recovery_sch) {
      main_h2p->recovery_scheduled = TRUE;
      main_h2p->tea_case1_pending_recovery = TRUE;
      main_h2p->tea_case1_detect_cycle = op->exec_cycle;
      tea_mark_early_flush_detection(main_h2p, op->exec_cycle);
      STAT_EVENT(op->proc_id, TEA_EARLY_FLUSHES);
      STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_IMMEDIATE);
      tea_record_case1_main_stage(op->proc_id, main_stage_at_detect);
      tea_record_early_flush_time_to_detect(
        op->proc_id, c, op,
        TEA_EARLY_FLUSH_TRIGGER_TO_DETECT_SAMPLES, ...);
      tea_record_early_flush_time_to_detect(
        op->proc_id, c, op,
        TEA_EARLY_FLUSH_CASE1_TRIGGER_TO_DETECT_SAMPLES, ...);
    }
  } else {
    /* FTQ case — defer to rename stage (existing deferred path) */
    Flag pending_recorded =
      tea_record_pending_case1_flush(op->proc_id, main_h2p,
                                     op->exec_cycle,
                                     main_stage_at_detect);
    if (pending_recorded) {
      tea_mark_early_flush_detection(main_h2p, op->exec_cycle);
      main_h2p->tea_case1_detect_cycle = op->exec_cycle;
      STAT_EVENT(op->proc_id, TEA_EARLY_FLUSHES);
      tea_record_case1_main_stage(op->proc_id, main_stage_at_detect);
      tea_record_early_flush_time_to_detect(
        op->proc_id, c, op,
        TEA_EARLY_FLUSH_TRIGGER_TO_DETECT_SAMPLES, ...);
      tea_record_early_flush_time_to_detect(
        op->proc_id, c, op,
        TEA_EARLY_FLUSH_CASE1_TRIGGER_TO_DETECT_SAMPLES, ...);
    }
  }
  if (main_h2p->decode_cycle)
    STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_NO_CHKPT);
  else
    STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_DECODE);
  terminate_tea_chain_with_reason(op->proc_id, chain_slot,
                                  TEA_CHAIN_TERM_REASON_EARLY_FLUSH_CASE1);
}
```

**핵심 변경점**:
- `fetch_cycle > 0`: 기존 deferred path 대신 즉시 `bp_sched_recovery()` 호출
- `fetch_cycle == 0`: 기존 `tea_record_pending_case1_flush()` 경로 유지 (변경 없음)
- `fetch_cycle`은 `icache_stage.c:862`에서 `op->fetch_cycle = cycle_count`로 설정됨 (`decoupled_frontend.cc`에서 DFE가 Op 생성 시에는 0, calloc 초기화)

**주의**: `fetch_cycle > 0` 이어도 main H2P가 icache output buffer에 있으면 (decode_cycle == 0)
`recover_icache_stage()` ASSERT가 여전히 필요 → Step 2(icache ASSERT 수정) 필수.

---

### Step 2: `src/icache_stage.c` — `recover_icache_stage()` Partial Flush 지원

Option A Step 3과 **동일한 변경**:

```c
void recover_icache_stage() {
  Stage_Data* cur_data = get_current_stage_data();
  cur_data->op_count = 0;
  for (ii = 0; ii < cur_data->max_op_count; ii++) {
    if (cur_data->ops[ii]) {
      if (FLUSH_OP(cur_data->ops[ii])) {
        free_op(cur_data->ops[ii]);
        cur_data->ops[ii] = NULL;
      } else {
        cur_data->op_count++;
      }
    }
  }
  /* 나머지 코드 동일 */
}
```

---

### Step 3: `src/tea/tea.stat.def` — 신규 Stat 추가

Option A Step 4와 **동일**:

```c
DEF_STAT(TEA_EARLY_FLUSH_CASE1_IMMEDIATE, COUNT, PER_CORE)
```

---

## 유지되는 코드 (변경 없음)

### `exec_stage_tea_pending_flush_at_rename()` (exec_stage.c)
- `bp_sched_recovery()`를 rename cycle에 호출하는 함수
- 변경 후에는 항상 `!h2p->oracle_info.recovery_sch` guard에 걸려 no-op
- 코드 유지 (안전한 fallback)

### `tea_record_pending_case1_flush()` / pending table (tea_thread.h/c)
- Case 1 handler에서 더 이상 호출되지 않으므로 pending table이 채워지지 않음
- 코드 유지 (다른 경로에서 사용 가능성 대비)

### `map_rename.c:971` — `exec_stage_tea_pending_flush_at_rename()` hook
- `exec_stage_tea_pending_flush_at_rename()`이 여전히 호출되지만 no-op
- 변경 없음

---

## Edge Case 분석

| 시나리오 | 동작 |
|----------|------|
| Recovery가 rename 전 fire | `!checkpoint_is_valid` → SRT rollback skip, ROB 비어있음(in-order rename), frontend만 flush ✓ |
| Recovery가 rename 후 fire | Main H2P의 SRT snapshot 존재 → full SRT rollback + off-path preg 해제 ✓ |
| 더 오래된 branch가 먼저 mispred | `op_num` 비교로 오래된 쪽 recovery가 우선, main H2P flush됨 ✓ |
| `off_path = TRUE` | line 872 guard에서 이미 early return ✓ |
| `recovery_sch = TRUE` (이미 다른 recovery) | line 879 guard `if (!main_h2p->oracle_info.recovery_sch)` → skip ✓ |
| Main H2P retire 전 recovery | `bp_recovery_info->recovery_op->op_pool_valid` guard in cmp_recover ✓ |

---

## 기대 효과

### 성능 향상
- Case 1 detect → recovery 지연 감소: rename 대기 없이 다음 cycle에 recovery fire
- `TEA_EARLY_FLUSH_CASE1_TO_RECOVERY_AVG` 감소 → misprediction penalty 단축
- 특히 Main H2P가 decode/IDQ에 있을 때 (TEA가 훨씬 앞서 있을 때) 효과 극대화

### 검증 지표 (Periodic stats 사용)

| Stat | Option A 예상 | Option B 예상 |
|------|--------------|--------------|
| `TEA_EARLY_FLUSH_CASE1_IMMEDIATE` | > 0 (모든 Case 1) | > 0 (`fetch_cycle > 0` Case 1만) |
| `TEA_EARLY_FLUSH_CASE1_PENDING_TRIGGERED` | ≈ 0 (pending table 비활성) | 감소 (FTQ 케이스만 남음) |
| `TEA_EARLY_FLUSH_CASE1_TO_SCHEDULE_AVG` | ≈ 0 (즉시 schedule) | 감소 (fetch된 케이스만) |
| `TEA_EARLY_FLUSH_CASE1_TO_RECOVERY_AVG` | 감소 (최대) | 감소 (부분) |
| Periodic IPC | 개선 (최대) | 개선 (부분) |
| `BP_MISP_PENALTY` | 감소 (최대) | 감소 (부분) |

---

## 빌드 및 검증 절차

```bash
cd ~/scarab-infra
./sci --build-scarab tea_dbg
./sci --sim tea_dbg
# /analyze_tea_sim으로 결과 분석
```
