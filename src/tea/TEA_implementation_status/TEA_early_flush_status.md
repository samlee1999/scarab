# TEA Early Flush 현재 구현 상태

**최종 갱신**: 2026-03-26
**관련 계획 문서**: `TEA_early_flush_plan.md`

---

## 1. 개요

TEA 스레드의 목표는 H2P 분기 명령을 메인 스레드보다 빠르게 execute하여 branch misprediction을 빠르게 탐지하는 것이다.
TEA H2P가 execute에서 misprediction을 감지하면 동일한 Identity를 가진 메인 H2P의 상태에 따라 early flush 및 recovery 전략을 다르게 가져간다. 
case 1: Pre-Rename (Main H2P가 아직 Rename 안 됨)
case 2: Post-Rename (Main H2P가 Rename 통과, SRT Checkpoint 존재) 
아래에 자세한 구현 상태가 설명되어있다.

**단일 H2P 기준으로 Early Flush 로직은 완전히 구현되어 있음.** ✅

---

## 2. 구현된 Case 요약

### Case 1: Pre-Rename (Main H2P가 아직 Rename 안 됨)

| 항목 | 위치 | 상태 |
|------|------|------|
| `reg_file_checkpoint_is_valid() == FALSE` 분기 | `exec_stage.c:598` | ✅ |
| `decode_cycle` 기반 분기 (Bug Fix) | `exec_stage.c:604` | ✅ |
| Case 1a: decode 통과 → no-op (기존 `recover_at_exec` 활용) | `exec_stage.c:600-604` | ✅ |
| Case 1b: decode 미도달 → `recover_at_decode = TRUE` | `exec_stage.c:610-611` | ✅ |
| 즉시 `recover_tea_on_flush()` 호출 | `exec_stage.c:613` | ✅ |

**Bug Fix History**:

**(2026-03-16)** 원래 코드는 Case 1에서 항상 `recover_at_decode = TRUE`를 설정했으나,
Main H2P가 이미 decode를 통과한 경우 이 플래그는 다시 확인되지 않아 recovery가 영원히 발생하지 않았음 (→ FTQ deadlock).
수정: `main_h2p->decode_cycle`을 확인하여 Case 1a/1b로 분기.

**(2026-03-26) Phase 9 수정**:
- **Case 1a** (decode 통과, rename 미통과): `recover_at_exec = TRUE` 중복 설정 제거.
  Main H2P가 mispredicted이면 `bp_predict_op()`에서 이미 `recover_at_exec=TRUE`가 설정됨.
  TEA가 추가로 설정할 필요 없이 Main H2P가 exec에 도달하면 자연 recovery.
- **Case 1b** (decode 미도달): `recover_at_decode = TRUE` + `recover_at_exec = FALSE` 설정.
  Main H2P가 decode에 도달하면 `decode_stage_process_op()`에서 `bp_sched_recovery()` 호출.
  즉시 `bp_sched_recovery()`를 호출하는 시도는 `recovery_sch` 이중 설정 ASSERT (`bp.c:153`)를
  유발하여 revert함 — decode stage에서의 호출과 충돌.
- **`map_rename.c`**: `reg_renaming_scheme_realistic_recover()`에 `!reg_file_checkpoint_is_valid()` guard 추가.
  Case 1에서 `cmp_recover()` 시 SRT checkpoint이 없으므로 rollback + preg flush를 안전하게 스킵.
  In-order rename이므로 Main H2P 이후 ops가 rename되지 않아 preg 해제 불필요.

**동작**: SRT checkpoint이 없으므로 즉시 SRT rollback 불가. Main H2P의 파이프라인 위치에 따라
적절한 recovery flag를 설정하여 해당 스테이지에서 recovery 수행. TEA는 즉시 종료.

### Case 2: Post-Rename (Main H2P가 Rename 통과, SRT Checkpoint 존재)

| 항목 | 위치 | 상태 |
|------|------|------|
| `reg_file_checkpoint_is_valid() == TRUE` 분기 | `exec_stage.c:579` | ✅ |
| `bp_sched_recovery(main_h2p, ...)` 호출 | `exec_stage.c:588-589` | ✅ |
| `recovery_scheduled = TRUE` (retirement 차단) | `exec_stage.c:594-596` | ✅ |
| `recover_at_exec = FALSE` (double-recovery 방지) | `exec_stage.c:597` | ✅ |

**동작**: 다음 cycle `cmp_recover()`에서 SRT rollback + BP 복원 + 전체 flush + TEA 종료.
Main H2P는 Node Table에 남아 execute되지만 `recover_at_exec == FALSE`이므로 recovery 재발 없음.

### 공통 Guard

| Guard | 위치 | 상태 |
|-------|------|------|
| Off-path Main H2P skip | `exec_stage.c:573-575` | ✅ |
| 이미 recovery 스케줄됨 skip (`recovery_sch`) | `exec_stage.c:578` | ✅ |
| On-path TEA H2P만 Early Flush (oracle 활용) | `exec_stage.c:563` | ✅ |

---

## 3. 핵심 코드

### 3.1 exec_stage_bp_resolve() — TEA H2P 분기 (`exec_stage.c:554-627`)

```c
static inline void exec_stage_bp_resolve(Op* op) {
  if (TEA_ENABLE && op->thread_id == 1) {
    if (tea_is_active(op->proc_id)) {
      Tea_Thread* tea = tea_threads[op->proc_id];

      if (op->inst_info->addr == tea->target_h2p_pc) {
        if (op->oracle_info.mispred || op->oracle_info.misfetch) {
          Op* main_h2p = tea->main_h2p_op;
          ASSERT(op->proc_id, main_h2p != NULL);
          ASSERT(op->proc_id, main_h2p->op_pool_valid);

          if (main_h2p->off_path) return;             // Guard 1
          if (!main_h2p->oracle_info.recovery_sch) {   // Guard 2
            if (reg_file_checkpoint_is_valid()) {
              // Case 2: Post-rename
              bp_sched_recovery(bp_recovery_info, main_h2p,
                                op->exec_cycle, FALSE, FALSE,
                                EXTRA_LATE_RECOVERY_CYCLES);
              if (main_h2p->oracle_info.recovery_sch)
                main_h2p->recovery_scheduled = TRUE;
              main_h2p->oracle_info.recover_at_exec = FALSE;
            } else {
              // Case 1: Pre-rename (no SRT checkpoint)
              if (main_h2p->decode_cycle) {
                // Case 1a: decode 통과 → no-op
                // bp_predict_op()에서 이미 recover_at_exec 설정됨
              } else {
                // Case 1b: decode 미도달
                main_h2p->oracle_info.recover_at_decode = TRUE;
                main_h2p->oracle_info.recover_at_exec = FALSE;
              }
              recover_tea_on_flush(op->proc_id);
            }
            STAT_EVENT(op->proc_id, TEA_EARLY_FLUSHES);
          }
        } else {
          STAT_EVENT(op->proc_id, TEA_H2P_CORRECT);
        }
      }
      STAT_EVENT(op->proc_id, TEA_OPS_EXECUTED);
    }
    return;
  }
  // ... Main thread branch resolution ...
}
```

### 3.2 recover_tea_on_flush() (`cmp_model.c:389-399`)

```c
void recover_tea_on_flush(uns proc_id) {
  if (!TEA_ENABLE || !tea_is_active(proc_id)) return;
  terminate_tea_thread(proc_id);
  reset_tea_fetch_stage(proc_id);
  reset_tea_rename_stage(proc_id);
  reset_tea_preg_pool(proc_id);
}
```

### 3.3 cmp_recover() 내 TEA 종료 (`cmp_model.c:443-446`)

```c
/* TEA Thread recovery: Terminate TEA on any flush */
if (TEA_ENABLE) {
  recover_tea_on_flush(bp_recovery_info->proc_id);
}
```

### 3.4 SRT Checkpoint 생성 (`map_rename.c:958-959`)

```c
if (!op->off_path && op->table_info->cf_type && op->oracle_info.recover_at_exec)
    reg_file_snapshot_srt();
```

### 3.5 SRT Rollback (`map_rename.c:984-999`)

```c
void reg_renaming_scheme_realistic_recover(Op *op) {
  ASSERT(op->proc_id, op->table_info->cf_type);
  if (op->oracle_info.recover_at_decode) return;  // decode recovery는 SRT rollback 불필요
  if (!reg_file_checkpoint_is_valid()) return;     // TEA Case 1: checkpoint 없으면 스킵
  reg_file_rollback_srt();
  // youngest → flush point까지 off-path pregs 해제
  for (Op **op_p = list_start_tail_traversal(&td->seq_op_list);
       op_p && (*op_p)->op_num > op->op_num;
       op_p = list_prev_element(&td->seq_op_list)) {
    reg_file_flush_mispredict(*op_p, ...);
  }
}
```

### 3.6 Retirement 차단 (`node_stage.c:850-851`)

```c
Flag op_not_ready_for_retire(Op* op) {
  return !(op->state == OS_DONE || OP_DONE(op)) || op->off_path
         || op->recovery_scheduled || op->redirect_scheduled;
}
```

### 3.7 flush_window()에서 recovery_scheduled 해제 (`node_stage.c:277-279`)

```c
if (IS_FLUSHING_OP(op)) {
  op->recovery_scheduled = FALSE;  // Recovery 완료 → retirement 허용
}
```

---

## 4. Recovery 경로 상세 추적

### Cycle X: TEA H2P 실행

1. `exec_stage_bp_resolve()` 진입 (TEA op, `thread_id == 1`)
2. `reg_file_checkpoint_is_valid()` → TRUE (Main H2P가 이전 cycle에 Rename 통과)
3. `bp_sched_recovery(bp_recovery_info, main_h2p, X, FALSE, FALSE, EXTRA_LATE_RECOVERY_CYCLES)`:
   - `bp_recovery_info->recovery_cycle = X + 1 + EXTRA_LATE_RECOVERY_CYCLES`
   - `bp_recovery_info->recovery_op_num = main_h2p->op_num`
   - `bp_recovery_info->recovery_op = main_h2p`
   - `bp_recovery_info->recovery_info = main_h2p->recovery_info`
   - `main_h2p->oracle_info.recovery_sch = TRUE`
4. `main_h2p->recovery_scheduled = TRUE` → retirement 차단
5. `main_h2p->oracle_info.recover_at_exec = FALSE` → double-recovery 방지

### Cycle X+1 (EXTRA_LATE_RECOVERY_CYCLES=0 가정)

6. `cmp_istreams()` → `cycle_count >= recovery_cycle` → `cmp_recover()` 실행
7. `bp_recover_op()`: BP 상태를 Main H2P의 `recovery_info`로 복원
8. `reg_file_recover(main_h2p)`: SRT rollback + off-path pregs 해제
9. `recover_thread()`: seq_op_list에서 younger ops 제거
10. 각 stage recover 함수 호출 (frontend → backend 전체 flush)
11. `recover_tea_on_flush(proc_id)` → TEA 종료

### 이후

Main H2P는 Node Table에 남아 execute되지만, `recover_at_exec == FALSE`이므로 recovery 미발생.

---

## 5. 정확성 검증 결과

| 검증 항목 | 결과 | 근거 |
|-----------|------|------|
| SRT Checkpoint 존재 보장 | ✅ | `checkpoint_is_valid()` TRUE 조건에서만 Case 2 진입 |
| SRT Rollback 정상 동작 | ✅ | `recover_at_decode == FALSE` + `checkpoint_is_valid()` → rollback 진행 |
| BP 상태 복원 | ✅ | `bp_recovery_info->recovery_info = main_h2p->recovery_info` |
| 파이프라인 전체 flush | ✅ | `FLUSH_OP(op)` = `op_num > main_h2p->op_num` |
| Double-recovery 방지 | ✅ | `recover_at_exec = FALSE` + `recovery_sch` guard |
| Recovery timing | ✅ | `cmp_istreams()`가 `cmp_cores()` 보다 먼저 실행 |
| Older pending recovery 우선 | ✅ | `bp_sched_recovery()` 내 op_num 비교 |
| Main H2P retirement 차단 | ✅ | `recovery_scheduled = TRUE` → `op_not_ready_for_retire()` |
| Main H2P off-path 처리 | ✅ | off-path guard로 skip |

---

## 6. 분석된 잠재적 문제점 (모두 현재 코드에서 안전)

### 6.1 기존 pending recovery 충돌 → ✅ 안전

`bp_sched_recovery()` (`bp.c:146`):
```c
if (bp_recovery_info->recovery_cycle == MAX_CTR ||
    op->op_num <= bp_recovery_info->recovery_op_num)
```
- **Older recovery 존재**: `main_h2p->op_num > recovery_op_num` → TEA recovery 무시 → 올바름 (older recovery가 flush)
- **Younger recovery 존재**: `main_h2p->op_num <= recovery_op_num` → TEA recovery가 대체 → 올바름 (older 우선)

### 6.2 Main H2P가 이미 Execute 통과 → ✅ 안전

- Main H2P 실행 시 `recovery_sch = TRUE` → TEA 실행 시 guard에서 skip

### 6.3 `main_h2p->op_pool_valid` → ✅ 안전

- Mispredicted branch는 execute 전에 retire 불가 (`recovery_scheduled` 또는 아직 `OS_DONE` 아님)
- 따라서 TEA H2P 실행 시 Main H2P는 항상 op pool에 유효

### 6.4 `EXTRA_LATE_RECOVERY_CYCLES > 0` → ✅ 안전

- TEA가 먼저 `recover_at_exec = FALSE` 설정 → Main H2P execute 시 recovery 미발생

### 6.5 SRT checkpoint 단일성 → ✅ 현재 안전 (다중 H2P 시 변경 필요)

- 단일 H2P만 추적하므로 checkpoint 하나로 충분
- **다중 H2P 구현 시**: `TEA_early_flush_plan.md` 참조

---

## 7. 버그 수정 이력

### 7.1 Case 1 `recover_at_decode` deadlock (2026-03-16 ~ 2026-03-26)

**증상**: FTQ deadlock — `decoupled_frontend.cc:280` ASSERT 발생 (100만 cycle 무진행)
- leela/128383 simpoint에서 재현 (다른 5개 simpoint은 정상 종료)

**근본 원인**: TEA Early Flush Case 1에서 `recover_at_decode = TRUE` + `recover_at_exec = FALSE`를 무조건 설정.
Main H2P가 이미 decode를 통과한 상태이면:
1. `recover_at_decode`는 decode stage에서 다시 확인되지 않음 (이미 지남)
2. `recover_at_exec = FALSE`로 덮어쓰여 exec stage에서도 recovery 불가
3. off-path ops가 retirement을 차단 → FTQ drain 불가 → deadlock

**수정 이력**:

1. **(2026-03-16)** `decode_cycle` 기반 분기 추가: Case 1a (`recover_at_exec=TRUE`) / Case 1b (`recover_at_decode=TRUE`)
   - 결과: mcf deadlock 해소, leela/128383은 여전히 deadlock

2. **(2026-03-24)** Phase 9 시도: Case 1b에서 즉시 `bp_sched_recovery()` 호출
   - 결과: `map_rename.c:553 checkpoint->is_valid` ASSERT 발생 → revert

3. **(2026-03-26)** Phase 9 재시도:
   - Case 1a: `recover_at_exec=TRUE` 중복 설정 제거 (이미 `bp_predict_op()`에서 설정됨)
   - Case 1b: 즉시 `bp_sched_recovery()` 호출 추가 → `bp.c:153 !recovery_sch` ASSERT 발생
     (decode stage에서 `recover_at_decode` 처리 시 `bp_sched_recovery()` 이중 호출)
   - 최종: `bp_sched_recovery()` 즉시 호출 revert → `recover_at_decode=TRUE`만 설정
   - `map_rename.c`에 `!reg_file_checkpoint_is_valid()` guard 추가 (Case 1에서 SRT rollback 안전 스킵)
   - 결과: 5/6 simpoint 정상 종료, leela/128383만 여전히 deadlock

**미해결 deadlock 분석** (leela/128383):
```
Case 1b: recover_at_decode=TRUE 설정 (bp_sched_recovery 미호출)
  → Main H2P가 decode에 도달해야 recovery 시작
  → 하지만 GP free_num < 32 → MAP stall → IDQ full → decode stall
  → Main H2P가 decode에 영원히 도달 못함 → FTQ full → ASSERT
```
- 다른 simpoint에서는 이 deadlock 조건에 걸리지 않음 (GP preg 충분 또는 Case 1b 미발생)
- 해결하려면 Case 1b에서 즉시 recovery를 트리거하되, decode stage와의 이중 호출 문제를 해결해야 함

**⚠️ 부분 해결**: 마스터 문서 `TEA_implementation_plan.md` §13.1에 등록됨.

