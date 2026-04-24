# TEA Early Flush 현재 구현 상태

**최종 갱신**: 2026-04-15
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
| `reg_file_checkpoint_is_valid() == FALSE` 분기 | `exec_stage.c` | ✅ |
| `main_h2p->tea_pending_mispred = TRUE` 세팅 | `exec_stage.c` | ✅ |
| TEA chain 즉시 종료 (`terminate_tea_chain`) | `exec_stage.c` | ✅ |
| `TEA_EARLY_FLUSH_CASE1` stat 기록 | `exec_stage.c` | ✅ |
| deferred flush hook in rename stage | `map_stage.c:stage_process_op()` | ✅ |
| `bp_sched_recovery()` at rename (if checkpoint valid) | `map_stage.c` | ✅ |
| `recover_at_exec = FALSE` + `recovery_scheduled = TRUE` | `map_stage.c` | ✅ |
| `map_rename.c` SRT rollback guard (`!checkpoint_is_valid`) | `map_rename.c` | ✅ |

**현재 구현 (2026-04-15, deferred-to-rename)**:

TEA가 Main H2P보다 먼저 H2P branch를 execute하여 mispred 탐지 (SRT checkpoint 없음):
1. `exec_stage.c`: `main_h2p->tea_pending_mispred = TRUE` 세팅 후 chain 종료
2. Main H2P가 rename 통과 → `reg_file_rename()` 내부에서 SRT checkpoint 생성
3. `map_stage.c:stage_process_op()`: checkpoint 확인 후 즉시 `bp_sched_recovery()` 호출
4. 다음 cycle `cmp_recover()`: SRT rollback + flush

**Fallback**: `bp_sched_recovery()`가 older recovery 우선으로 no-op 되면 `recover_at_exec = TRUE` 유지 → exec_stage 정상 처리.

**이전 구현 히스토리 (아카이브)**:
- 2026-04-12: Case 1a (`decode_cycle > 0`) / Case 1b (`decode_cycle == 0`) 분기. flush timing 이득 없음 — no-TEA와 동일.
  - Case 1a: `TEA_EARLY_FLUSH_CASE1_NO_CHKPT` stat, `recover_at_exec=TRUE` fallback
  - Case 1b: `TEA_EARLY_FLUSH_CASE1_DECODE` stat, `recover_at_exec=TRUE` fallback
- 2026-03-26 Phase 9 실패: exec_stage에서 즉시 `bp_sched_recovery()` 시도 → checkpoint 없어서 ASSERT

**동작**: Main H2P의 rename→exec 구간만큼 early flush benefit 획득 (Case 2와 동등).

### Case 2: Post-Rename (Main H2P가 Rename 통과, SRT Checkpoint 존재)

| 항목 | 위치 | 상태 |
|------|------|------|
| `reg_file_checkpoint_is_valid() == TRUE` 분기 | `exec_stage.c:594` | ✅ |
| `bp_sched_recovery(main_h2p, ...)` 호출 | `exec_stage.c:603-604` | ✅ |
| `recovery_scheduled = TRUE` (retirement 차단) | `exec_stage.c:609-611` | ✅ |
| `recover_at_exec = FALSE` (double-recovery 방지) | `exec_stage.c:612` | ✅ |

**동작**: 다음 cycle `cmp_recover()`에서 SRT rollback + BP 복원 + 전체 flush + TEA 종료.
Main H2P는 Node Table에 남아 execute되지만 `recover_at_exec == FALSE`이므로 recovery 재발 없음.

### 공통 Guard

| Guard | 위치 | 상태 |
|-------|------|------|
| Off-path Main H2P skip | `exec_stage.c:588-590` | ✅ |
| 이미 recovery 스케줄됨 skip (`recovery_sch`) | `exec_stage.c:593` | ✅ |
| On-path TEA H2P만 Early Flush (oracle 활용) | `exec_stage.c:578` | ✅ |

---

## 3. 핵심 코드

### 3.1 exec_stage_bp_resolve() — TEA H2P 분기 (`exec_stage.c:569-644`)

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
                // Case 1a: decode 통과, rename 미통과
                // recover_at_exec는 bp_predict_op()에서 이미 설정됨 → 자연 recovery
                STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_NO_CHKPT);
              } else {
                // Case 1b: decode 미도달
                // recover_at_decode 설정 안 함 — Main H2P가 rename→exec 경로에서 자연 recovery
                STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_DECODE);
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

**2026-04-12 최종 수정 — `recover_at_decode` 접근법 포기**:

`recover_at_decode=TRUE` 접근법은 Case 1b에서 MAP stall 시 deadlock을 유발하는 구조적 문제가 있어
완전히 포기함. 현재 코드는 Case 1b에서 Main H2P의 flag를 일절 건드리지 않으며, TEA만 즉시 종료.

Main H2P의 기존 `recover_at_exec=TRUE` flag가 자연스럽게 유지되어:
- Main H2P가 rename 단계에 도달하면 SRT checkpoint 생성
- exec 단계에 도달하면 기존 recovery 경로(`recover_at_exec`)로 정상 처리

이 방식은 blender 기준으로 `TEA_EARLY_FLUSH_CASE1_DECODE = 43` 정상 동작 확인됨 (2026-04-12).

