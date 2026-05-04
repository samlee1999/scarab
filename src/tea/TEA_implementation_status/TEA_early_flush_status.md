# TEA Early Flush 현재 구현 상태

**최종 갱신**: 2026-05-04
**관련 계획**: `../TEA_implementation_plan/TEA_early_flush_plan.md`

---

## 1. 요약

Early Flush는 현재 multi-H2P chain 기준으로 동작한다. TEA H2P branch가 execute되면 `h2p_chain_id`로 chain slot을 찾고, chain이 저장한 Main H2P 정보를 검증한 뒤 Case 1/2를 처리한다.

| 항목 | 현재 상태 |
|------|-----------|
| TEA H2P chain 식별 | `op->h2p_chain_id` 기반 |
| H2P branch 확인 | TEA op PC와 `target_h2p_pc` 비교 |
| Main H2P pointer 검증 | `op_pool_valid` + `saved_unique_num` |
| Case 1 no SRT checkpoint | pending flush 기록 후 main H2P rename에서 recovery schedule |
| Case 2 SRT checkpoint 있음 | 즉시 Main H2P 기준 recovery schedule |
| Too late 판정 | Main H2P가 이미 recovery schedule한 경우 stat 기록 |
| Correct TEA H2P | 해당 chain 정상 종료 |
| Main recovery 시 TEA 처리 | recovery point 이상 chain 종료 + stale dependency cleanup |

---

## 2. TEA H2P resolve 경로

`exec_stage_bp_resolve()`에서 `TEA_ENABLE && op->thread_id == 1`이면 TEA branch resolve path로 들어간다.

Guard 순서:

1. `tea_is_active(proc_id)` 확인.
2. `op->h2p_chain_id - 1`로 chain slot 계산.
3. `tea_chain_slot_is_valid()` 확인.
4. chain이 `CHAIN_INACTIVE`이면 residual op으로 보고 return.
5. TEA op PC가 `target_h2p_pc`와 다르면 early flush 대상이 아니므로 return.
6. TEA H2P trigger/fetch-to-exec timing stat 기록.
7. TEA H2P가 mispred/misfetch이면 early flush attempt 처리.
8. correct이면 `TEA_H2P_CORRECT` 기록 후 해당 chain 종료.

Main H2P pointer는 `main_h2p_op`, `saved_unique_num`으로 검증한다. 검증 실패 시 해당 chain은 `TEA_CHAIN_TERM_INVALID_MAIN_H2P` reason으로 종료된다.

---

## 3. Case 동작

### Case 1: Main H2P가 아직 SRT checkpoint를 만들지 못한 경우

`reg_file_checkpoint_is_valid() == FALSE`이면 즉시 SRT rollback을 할 수 없다. 현재 구현은 TEA detect를 버리지 않고 pending table에 기록한다.

동작:

1. `tea_classify_case1_main_stage(main_h2p)`로 detect 시점의 Main H2P stage 분류.
2. `tea_record_pending_case1_flush()`로 Main H2P pointer/unique/op_num/detect_cycle/stage 저장.
3. Main H2P에 `tea_case1_detect_cycle`과 early-detect flag 기록.
4. `TEA_EARLY_FLUSHES` 및 Case 1 timing stat 기록.
5. 해당 TEA chain은 `TEA_CHAIN_TERM_EARLY_FLUSH_CASE1` reason으로 종료.

이후 Main H2P가 rename stage에서 `reg_file_snapshot_srt()`로 SRT checkpoint를 만들면, 같은 branch에 대해 `exec_stage_tea_pending_flush_at_rename()`이 호출된다. unique number가 pending entry와 일치하고 아직 recovery가 schedule되지 않았으면 그 시점에 `bp_sched_recovery()`를 호출한다.

Case 1에서 중요한 점:

- recovery schedule cycle은 TEA detect cycle이 아니라 Main H2P rename cycle이다.
- detect-to-schedule delay는 `TEA_EARLY_FLUSH_CASE1_TO_SCHEDULE_*`로 기록된다.
- 실제 `cmp_recover()`까지의 delay는 `TEA_EARLY_FLUSH_CASE1_TO_RECOVERY_*`로 기록된다.

### Case 2: Main H2P가 SRT checkpoint를 가진 경우

`reg_file_checkpoint_is_valid() == TRUE`이면 TEA가 즉시 Main H2P 기준 recovery를 schedule한다.

동작:

1. `bp_sched_recovery(bp_recovery_info, main_h2p, op->exec_cycle, ..., EXTRA_LATE_RECOVERY_CYCLES)` 호출.
2. schedule 성공 시 Main H2P의 `recovery_scheduled = TRUE`.
3. Main H2P에 early-detect cycle 기록.
4. `recover_at_exec = FALSE`로 Main H2P execute 시 double recovery를 막음.
5. `TEA_EARLY_FLUSH_CASE2_WITH_CHKPT` 기록.

실제 recovery는 기존 Scarab recovery machinery가 수행한다. `cmp_recover()`가 호출되면 `recover_tea_on_flush(proc_id, recovery_op_num)`가 TEA chain들을 selective하게 정리한다.

### Too late

TEA H2P가 misprediction을 detect했지만 Main H2P의 `oracle_info.recovery_sch`가 이미 set되어 있으면 early flush schedule에는 실패한 것으로 본다. 이때 `TEA_EARLY_FLUSH_TOO_LATE_MAIN_RECOVERY`와 timing stat을 기록한다.

### Correct prediction

TEA H2P가 mispred/misfetch가 아니면 `TEA_H2P_CORRECT`를 기록하고 해당 chain만 정상 종료한다.

---

## 4. Main recovery 연동

`cmp_recover()`는 main pipeline recovery를 수행하면서 `recover_tea_on_flush(proc_id, recovery_op_num)`를 호출한다.

`recover_tea_on_flush()`의 현재 동작:

1. recovery point 이상 pending Case 1 flush entry 제거.
2. TEA가 inactive이면 return.
3. active chain 중 `target_h2p_op_num >= recovery_op_num`인 chain 종료.
4. surviving TEA op에서 flushed main producer dependency cleanup.

이 정책은 recovery point보다 younger/equal한 H2P chain은 wrong-path로 보고 종료하고, older H2P chain은 생존시킨다. 생존 chain이 flushed main producer를 기다리던 경우에는 stale dependency cleanup이 not-ready bit를 clear한다.

---

## 5. Timing / stage stat

현재 early flush timing 분석을 위해 다음 stat이 구현되어 있다.

| 목적 | Stat |
|------|------|
| TEA H2P 전체 execute latency | `TEA_H2P_TRIGGER_TO_EXEC_*`, `TEA_H2P_FETCH_TO_EXEC_*` |
| early flush detect latency | `TEA_EARLY_FLUSH_TRIGGER_TO_DETECT_*`, `TEA_EARLY_FLUSH_FETCH_TO_DETECT_*` |
| Case 1 detect latency | `TEA_EARLY_FLUSH_CASE1_TRIGGER_TO_DETECT_*`, `TEA_EARLY_FLUSH_CASE1_FETCH_TO_DETECT_*` |
| Case 2 detect latency | `TEA_EARLY_FLUSH_CASE2_TRIGGER_TO_DETECT_*`, `TEA_EARLY_FLUSH_CASE2_FETCH_TO_DETECT_*` |
| too-late detect latency | `TEA_EARLY_FLUSH_TOO_LATE_TRIGGER_TO_DETECT_*`, `TEA_EARLY_FLUSH_TOO_LATE_FETCH_TO_DETECT_*` |
| Case 1 detect-to-schedule | `TEA_EARLY_FLUSH_CASE1_TO_SCHEDULE_*` |
| Case 1 detect-to-recovery | `TEA_EARLY_FLUSH_CASE1_TO_RECOVERY_*` |
| Main H2P fetch-to-exec | `TEA_MAIN_H2P_FETCH_TO_EXEC_*`, `TEA_MAIN_H2P_MISPRED_FETCH_TO_EXEC_*` |
| Case 1 Main H2P timing | `TEA_EARLY_FLUSH_CASE1_MAIN_FETCH_TO_EXEC_*`, `TEA_EARLY_FLUSH_CASE1_DETECT_TO_MAIN_EXEC_*` |
| TEA vs Main H2P exec delta | `TEA_H2P_MAIN_EXEC_DELTA_SAMPLES`, `TEA_H2P_EXEC_BEFORE_MAIN`, `TEA_H2P_EXEC_SAVED_CYCLES_AVG`, `TEA_H2P_EXEC_AFTER_MAIN` |
| Case 1 detect stage | `TEA_EARLY_FLUSH_CASE1_MAIN_STAGE_*` |
| Case 1 stage별 delay | `TEA_EARLY_FLUSH_CASE1_*_TO_SCHEDULE_*` |

`TEA_H2P_EXEC_SAVED_CYCLES_AVG`는 TEA H2P execute cycle이 Main H2P execute cycle보다 빠른 sample에 대해서만 평균을 낸다. Main H2P가 early flush 이후 실제 execute 전에 free되면 `TEA_H2P_MAIN_EXEC_UNKNOWN`이 기록될 수 있다.

---

## 6. Case 1 stage 분류

Case 1 detect 시 Main H2P stage는 `tea_classify_case1_main_stage()`가 다음 기준으로 분류한다.

| Stage | 기준 |
|-------|------|
| `PRE_DECODE` | `decode_cycle == 0` |
| `DECODED_PRE_RENAME` | decode는 끝났지만 `map_cycle == MAX_CTR` |
| `IN_RENAME` | map은 되었지만 `issue_cycle == MAX_CTR` |
| `IN_NODE_OR_RS` | issue는 되었지만 `exec_cycle == MAX_CTR` |
| `SCHEDULED_OR_EXECUTING` | exec cycle이 정해졌고 현재 cycle이 exec cycle 전 |
| `DONE_OR_LATER` | 현재 cycle이 Main H2P exec cycle 이상 |

이 stage breakdown은 Case 1 pending delay가 frontend/rename 지연인지, backend scheduling 지연인지 구분하기 위한 diagnostic이다.

---

## 7. 남은 제한

| 제한 | 설명 |
|------|------|
| Case 1 immediate recovery 불가 | SRT checkpoint가 없으면 TEA detect 즉시 rollback할 수 없어 rename까지 기다린다. |
| synchronized timestamp model 아님 | 논문의 branch timestamp/queue 수정 모델이 아니라 Main H2P pointer와 Scarab recovery path를 사용한다. |
| Main H2P pointer 의존 | op pool validity와 unique guard가 있지만, Main H2P가 이미 사라진 경우 saved-cycle 분석은 unknown으로 빠질 수 있다. |
| Case 1 pending table | runtime chain slot 수만큼 저장하며 full이면 pending dropped stat을 기록한다. |

현재 구현은 "TEA가 Main보다 먼저 misprediction을 detect하면 기존 Scarab recovery를 더 빨리 schedule한다"는 기능 목표를 구현한다. 다만 Case 1은 checkpoint 생성 전이라는 구조적 제약 때문에 detect-to-schedule delay가 남는다.
