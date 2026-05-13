# TEA Case 1 Early Flush 즉시 Recovery 최적화

## 목표

Case 1은 TEA thread가 H2P misprediction을 먼저 발견했지만, Main H2P가 아직 rename을 지나지 않아 SRT checkpoint가 없는 경우다.

현재 구현은 이 경우 `tea_record_pending_case1_flush()`로 pending entry를 남기고, Main H2P가 rename에 도달해 `reg_file_snapshot_srt()`가 생성될 때까지 recovery를 지연한다. 이 구조에서는 TEA가 매우 빨리 H2P를 resolve해도 Main H2P가 rename에 도달하기 전까지 frontend/backend flush가 시작되지 않는다.

목표는 Case 1에서도 TEA detection cycle에 즉시 `bp_sched_recovery()`를 호출하고, processor frontend가 recovery op보다 오래된 instruction은 보존하고 younger wrong-path instruction만 flush하도록 만드는 것이다.

## 최신 실험 근거

실험 디렉토리: `/home/lee/simulations/tea_on_case1_opt_dbg`

아래 값은 periodic `tea.stat.0.csv` 기준으로 52개 simpoint를 합산한 값이다.

| Config | Attempts | Case 1 | Case 2 | Too late | Pre-decode Case 1 | FTQ Case 1 | Fetched pre-decode |
|---|---:|---:|---:|---:|---:|---:|---:|
| `tea_baseline` | 254,889 | 111,035 | 98,666 | 45,180 | 59,022 | 44,247 | 14,775 |
| `tea_perfect_load_5cycle` | 256,023 | 111,626 | 98,668 | 45,713 | 59,955 | 45,123 | 14,832 |
| `tea_perfect_load_1cycle` | 287,796 | 151,892 | 103,822 | 32,064 | 76,996 | 57,503 | 19,493 |

`tea_baseline -> tea_perfect_load_1cycle`에서 Case 1은 40,857개 증가했다. 특히 Main H2P가 아직 FTQ 안에 있는 Case 1도 44,247개에서 57,503개로 증가했다.

따라서 FTQ case를 deferred path로 남기는 방향은 이번 목적에 맞지 않는다. FTQ에 Main H2P가 있을 때도 즉시 recovery할 수 있어야 한다.

## 논문 요구사항 해석

TEA 논문의 frontend partial flush 요구사항은 "frontend 전체를 비우라"는 뜻이 아니다. Recovery branch보다 오래된 frontend instruction은 보존하고, recovery branch보다 younger인 instruction만 flush해야 한다는 뜻이다. Fetch Queue도 같은 기준으로 partial flush되어야 한다.

Scarab 코드에서는 이 기준이 `FLUSH_OP(op)`로 표현된다.

```c
#define FLUSH_OP(op) (op->op_num > bp_recovery_info->recovery_op_num)
```

Option A의 구현 기준은 다음 하나다.

- `op_num <= recovery_op_num`: correct-path prefix로 보존
- `op_num > recovery_op_num`: wrong-path suffix로 flush

## 현재 구현에서 깨지는 지점

### 1. SRT checkpoint가 없어도 reg-file recovery는 안전하게 skip된다

`src/map_rename.c`의 `reg_renaming_scheme_realistic_recover()`는 checkpoint가 없으면 SRT rollback을 건너뛴다.

```c
if (!reg_file_checkpoint_is_valid()) {
  STAT_EVENT(op->proc_id, TEA_RECOVER_CALLS_SKIPPED);
  return;
}
```

Main H2P가 아직 rename 전이면 in-order rename 특성상 그 뒤의 Main op도 rename되지 않았다. 따라서 이 경우에는 회수할 physical register가 없고, frontend flush만 먼저 수행해도 된다.

### 2. `recover_icache_stage()`는 현재 full flush를 가정한다

`src/icache_stage.c`의 `recover_icache_stage()`는 icache output buffer의 모든 op이 `FLUSH_OP`이고 `off_path`라고 assert한다.

즉시 Case 1 recovery에서는 Main H2P 자체 또는 Main H2P보다 오래된 op이 icache output buffer에 남아 있을 수 있다. 이 op들은 `FLUSH_OP == FALSE`이므로 보존되어야 한다. 현재 코드는 이 상황에서 assert로 죽는다.

### 3. `Decoupled_FE::recover()`는 FTQ를 전부 비운다

`src/decoupled_frontend.cc`의 `Decoupled_FE::recover()`는 현재 FTQ 안의 모든 FT를 `free_ops_and_clear()`한 뒤 `ftq.clear()`한다.

Main H2P가 아직 FTQ 안에 있는 Case 1에서는 이것이 correctness bug가 된다. FTQ 안에는 다음과 같은 correct-path prefix가 있을 수 있다.

```text
FTQ:
  ... older correct-path FTs ...
  FT containing Main H2P
  younger wrong-path FTs
```

full flush를 하면 Main H2P와 그 앞의 correct-path FT까지 free된다. 그런데 recovery fetch address는 Main H2P의 true NPC이므로, Main H2P 이전 prefix는 다시 fetch되지 않는다. 따라서 partial FTQ flush가 필수다.

### 4. FDIP가 켜져 있으므로 FTQ iterator도 recovery와 함께 정리해야 한다

현재 `FDIP_ENABLE` default는 1이고, 실험 config에서도 FDIP가 켜져 있다. FDIP는 `src/prefetcher/fdip.cc`에서 `decoupled_fe_new_ftq_iter()`로 만든 FTQ iterator를 사용해 DFE FTQ를 앞서 스캔하며 instruction prefetch를 낸다.

`recover_fdip()` 자체는 `last_line_addr`와 `last_recover_cycle`만 갱신한다. full FTQ flush에서는 `Decoupled_FE::recover()`가 모든 iterator를 0으로 reset했기 때문에 문제가 작게 보였다. Option A에서는 FTQ prefix를 보존하므로, FDIP iterator가 flush된 suffix를 가리키지 않도록 DFE recover에서 clamp해야 한다.

이미 발행된 FDIP memory request는 완전히 취소할 수 없다. 이것은 instruction prefetch pollution 또는 useful prefetch 차이로 성능에 영향을 줄 수 있지만, correctness는 FTQ/icache/decode가 `op_num` 기준으로 보존/flush되는지에 달려 있다.

## FDIP가 켜진 상태에서 FTQ selective flush의 의미

마이크로아키텍처 관점에서 현재 Scarab frontend는 다음 세 흐름이 같은 FTQ를 기준으로 움직인다.

1. Decoupled frontend가 branch prediction 결과를 따라 FT를 만들고 FTQ에 push한다.
2. I-cache/uop-cache fetch path가 FTQ 앞쪽의 FT를 demand fetch한다.
3. FDIP가 별도 FTQ iterator로 FTQ를 미리 훑으면서 instruction prefetch request를 낸다.

여기서 FTQ flush는 cache line을 지우는 동작이 아니라, frontend가 앞으로 유효한 instruction stream으로 인정할 FT/op를 정리하는 동작이다. 따라서 selective flush의 기준은 다음처럼 잡는다.

```text
FTQ before recovery:
  FT0: older correct-path ops
  FT1: ... Main H2P(recovery_op_num) ... younger wrong-path ops
  FT2: younger wrong-path ops

FTQ after selective recovery:
  FT0: older correct-path ops preserved
  FT1: Main H2P까지 preserved, younger suffix removed
  new recovery FT: true NPC에서 새로 생성
```

FDIP prefetch까지 고려해도 이 기준은 바뀌지 않는다.

- 이미 I-cache/MLC/L1/memory로 나간 FDIP prefetch request는 architectural state가 아니므로 rollback하지 않는다.
- 이미 들어온 prefetched instruction cache line도 rollback하지 않는다. wrong-path prefetch pollution은 성능 현상이지 correctness violation이 아니다.
- 대신 recovery 이후 FDIP가 flush된 FT/op를 다시 future stream으로 믿고 prefetch하지 않도록 FTQ iterator만 보정한다.

즉, prefetch를 고려한 selective flush는 "prefetch request를 selective cancel"하는 기능이 아니다. Scarab에서 필요한 동작은 "FTQ와 FTQ를 바라보는 모든 consumer의 cursor를 같은 preserved prefix 기준으로 정렬"하는 것이다.

현재 호출 순서도 이 해석과 맞아야 한다.

```text
cmp_recover()
  recover_decoupled_fe()  // FTQ partial trim + FDIP iterator clamp
  recover_fdip()          // FDIP 내부 recover timestamp/state 갱신
  recover_icache_stage()  // demand fetch path의 output/current_ft 정리
```

`recover_fdip()`는 FTQ 구조를 직접 고치지 않으므로, FDIP iterator clamp는 반드시 `Decoupled_FE::recover()` 쪽에서 처리하는 것이 맞다.

## 구현자가 혼동하면 안 되는 경계

- `recovery_op_num`보다 오래된 op를 보존하는 것은 "그 op를 다시 fetch하라"는 뜻이 아니다. 이미 FTQ, current FT, icache output buffer에 있다면 그대로 흘려보내고, 새 fetch는 true NPC부터 시작한다.
- `dfe_op_count = recovery_op_num + 1`은 recovery 이후 새로 생성되는 op 번호의 시작점이다. preserved FTQ 안에 아직 demand fetch되지 않은 op가 있다면, `icache_stage.c`의 `op_count[]`까지 무조건 `recovery_op_num + 1`로 점프시키면 안 된다.
- `current_ft_to_push`는 FTQ에 commit되지 않은 transient FT다. Main H2P가 PRE_DECODE 서브케이스라면 아직 FTQ가 아니라 `current_ft_to_push` 안에 있을 수 있다 (`decoupled_frontend.cc:433`에서 op 추가 후 `ft_ended_by != FT_NOT_ENDED`일 때만 FTQ에 push됨 — line 436). 이 상태에서 무조건 `free_ops_and_clear()`를 호출하면 Main H2P Op*가 해제되어 pipeline이 해당 dynamic instance를 잃는다. `Decoupled_FE::recover()` 에서는 partial flush 전에 `current_ft_to_push.ops`를 순회해 `op_num <= recovery_op_num`인 op가 하나라도 있으면 trim 후 FTQ에 force-materialize하고, 없으면 기존대로 free 후 `FT_STARTED_BY_RECOVERY`로 새로 시작한다 (Step 4 상세 참조).
- `ic->current_ft`와 `uc->current_ft`는 FTQ 안에 남은 FT를 가리키고 있을 수 있다. 해당 FT가 preserved prefix에 남아 있으면 유지하고, erased suffix에 속하면 NULL 처리한다.
- FTQ prefix가 남은 상태에서 recovery-start FT를 append하면 기존 FT 연속성 assert가 깨질 수 있다. `FT_STARTED_BY_RECOVERY`로 시작하는 FT는 이전 FT의 fall-through/predicted NPC와 연속일 필요가 없다.
- FDIP iterator를 0으로 reset하면 correctness는 대체로 유지되지만, preserved FT를 다시 스캔하면서 FDIP traffic/stat을 왜곡한다. Option A의 목표에는 reset보다 clamp가 더 적합하다.

## 구현 방향: Option A만 적용

fetch_cycle 기반 deferred path는 사용하지 않는다. 모든 Case 1에서 즉시 recovery를 시도하고, FTQ를 포함한 frontend가 partial flush를 지원하도록 수정한다.

## 수정 파일 요약

| 파일 | 수정 이유 |
|---|---|
| `src/exec_stage.c` | Case 1 pending path 대신 즉시 `bp_sched_recovery()` 호출 |
| `src/cmp_model.c` | Case 1 recovery가 실제로 consume된 뒤 Main H2P의 중복 main-exec recovery 방지 |
| `src/decoupled_frontend.cc` | `Decoupled_FE::recover()`를 full FTQ flush에서 partial FTQ flush로 변경, FDIP iterator clamp, recovery-start FT append assert 처리 |
| `src/decoupled_frontend.h` | icache가 preserved FTQ state를 질의할 수 있는 C API 추가 |
| `src/ft.h` | FT partial trim helper 선언 |
| `src/ft.cc` | FT 안에서 `recovery_op_num` 이후 op만 free하고 FT metadata를 갱신하는 helper 구현 |
| `src/icache_stage.c` | icache output buffer partial flush, `op_count[]`, `current_ft` 보존/무효화 처리 |
| `src/tea/tea.stat.def` | immediate Case 1 recovery 및 FTQ immediate case 검증용 stat 추가 |

검토 결과 `src/map_rename.c`, `src/idq_stage.cc`, `src/map_stage.c`, `src/uop_queue_stage.cc`, `src/uop_cache.cc`, `src/prefetcher/fdip.cc`는 1차 구현에서 직접 수정하지 않는 방향이 맞다. 다만 이 파일들의 기존 invariant를 깨지 않는지 validation 대상에 포함한다.

## Step 1. `src/exec_stage.c`: Case 1 즉시 schedule

현재 Case 1 branch는 `tea_record_pending_case1_flush()`를 호출한다. 이를 즉시 schedule로 바꾼다.

핵심 로직:

```c
Tea_Case1_Main_Stage main_stage_at_detect =
  tea_classify_case1_main_stage(main_h2p);

tea_record_case1_main_stage(op->proc_id, main_stage_at_detect, main_h2p);

bp_sched_recovery(bp_recovery_info, main_h2p, op->exec_cycle,
                  FALSE, FALSE, EXTRA_LATE_RECOVERY_CYCLES);

if (main_h2p->oracle_info.recovery_sch) {
  main_h2p->recovery_scheduled = TRUE;
  main_h2p->tea_case1_pending_recovery = TRUE;
  main_h2p->tea_case1_detect_cycle = op->exec_cycle;
  tea_mark_early_flush_detection(main_h2p, op->exec_cycle);
  STAT_EVENT(op->proc_id, TEA_EARLY_FLUSHES);
  STAT_EVENT(op->proc_id, TEA_EARLY_FLUSH_CASE1_IMMEDIATE);
  ...
}
```

주의점:

- `main_h2p->oracle_info.recover_at_exec`는 여기서 지우지 않는다.
- recovery가 rename 이후에 fire될 수 있으므로, 그 경우 SRT snapshot이 생성될 기회를 남겨야 한다.
- recovery가 실제로 fire된 뒤에는 `src/cmp_model.c`에서 `recover_at_exec`를 지워 중복 main-exec recovery를 막는다.
- 기존 `exec_stage_tea_pending_flush_at_rename()`와 pending table은 fallback code로 남긴다. Option A가 정상 동작하면 `TEA_EARLY_FLUSH_CASE1_PENDING_TRIGGERED`는 거의 0이 되어야 한다.

**제거해야 할 코드 (현재 `exec_stage.c:928–966` Case 1 else branch)**:

```c
// 제거: tea_record_pending_case1_flush() 호출 전체
// 제거: terminate_tea_chain_with_reason() 호출
```

Case 2(`exec_stage.c:900–927`)와 달리 `terminate_tea_chain_with_reason()`을 호출하지 않는다 — **이것은 안전하다**. Case 2도 이미 `terminate_tea_chain_with_reason()`을 호출하지 않으며, chain cleanup은 `cmp_recover()` → `recover_tea_on_flush()`가 next cycle에 담당한다. 기존 `TEA_EARLY_FLUSH_CASE1_NO_CHKPT`/`TEA_EARLY_FLUSH_CASE1_DECODE` stat event는 유지한다.

**DECODED_PRE_RENAME/Map-entry 경계의 SRT snapshot 생성 (유효함)**: Main H2P가 `DECODED_PRE_RENAME` 서브케이스 (`map_cycle == MAX_CTR`)일 때, TEA의 `update_exec_stage()`에서 Case 1 즉시 schedule이 먼저 발생한 뒤, 같은 cycle의 `update_map_stage()`가 Main H2P를 rename 처리하면서 `map_rename.c:964-971`의 `reg_file_snapshot_srt()`를 호출한다. `exec_stage_tea_pending_flush_at_rename()` 안의 `!recovery_sch` guard가 이중 recovery를 막는다. 이 snapshot은 Main H2P의 rename 직후 RAT 상태를 저장하며, 다음 cycle의 `cmp_recover()` → `reg_file_recover()`가 이를 사용해 Main H2P의 register 할당은 보존하고 younger wrong-path rename만 rollback한다 — partial flush에 정확히 필요한 동작이다. correctness 문제 없음.

**`IN_RENAME` 서브케이스 (`map_cycle != MAX_CTR`)에서는 이 현상이 발생하지 않는다**: `map_cycle`은 `map_stage.c:264`의 `map_stage_fetch_op()`에서 설정되며, `update_exec_stage()`는 `update_map_stage()`보다 먼저 실행된다. 따라서 exec_stage가 `map_cycle != MAX_CTR`인 op를 보는 시점에 그 `map_cycle`은 반드시 이전 cycle에 설정된 값이다. 해당 op의 rename과 `map_rename.c:964` 호출은 이미 이전 cycle에 완료되었으므로, 현재 cycle의 `update_map_stage()`는 dispatch만 시도하며 SRT snapshot이 새로 생성되지 않는다.

**`recover_at_exec`를 지우지 않으면 생기는 main exec 재트리거 문제 및 필수 추가 수정**:

`exec_stage.c:1004`의 normal branch resolution 경로:

```c
if(op->oracle_info.recover_at_exec) {   // recovery_sch 체크 없음!
    bp_sched_recovery(bp_recovery_info, op, op->exec_cycle, ...);
```

`bp_sched_recovery()` 내부 `bp.c:153`에는 `ASSERT(!op->oracle_info.recovery_sch)`가 있다. 따라서 Case 1 immediate path에서 `recovery_sch = TRUE`로 설정된 Main H2P가 이후 exec stage에 도달하면 이 ASSERT가 발동한다.

**같은 cycle race**: `cmp_recover()`는 recovery schedule 다음 cycle에 실행된다. Case 1 sub-case `IN_NODE_OR_RS`/`SCHEDULED_OR_EXECUTING`에서 TEA H2P와 Main H2P가 같은 cycle에 exec stage를 통과하면, 같은 `update_exec_stage()` 호출 내에서 TEA H2P 처리(→ `recovery_sch = TRUE`) 후 Main H2P 처리(→ ASSERT)가 발생한다. cmp_recover()는 그 다음 cycle에 실행되므로 이 race를 막을 수 없다.

**필수 수정 (`exec_stage.c:1004`)**: TEA early flush guard(line 898)와 동일하게 `recovery_sch` 체크를 추가한다:

```c
// 기존:
if(op->oracle_info.recover_at_exec) {

// 수정:
if(op->oracle_info.recover_at_exec && !op->oracle_info.recovery_sch) {
```

이 수정은 Main H2P의 SRT checkpoint 생성 path(`map_rename.c:964`)와 무관하다. `recovery_sch`가 TRUE이면 `bp_sched_recovery()`를 skip하고 해당 op는 이미 scheduled된 recovery에 의해 flush/retire 처리된다.

Step 2의 `recover_at_exec = FALSE` 클리어는 이후 cycle에 cmp_recover()가 실행될 때 처리된다. 이것으로 Main H2P가 훨씬 나중에 exec에 도달하는 일반적인 경우(FTQ case)도 함께 커버된다.

## Step 2. `src/cmp_model.c`: Case 1 recovery consume 후 중복 recovery 방지

현재 `cmp_recover()`는 Case 1 stat을 기록하고 `tea_case1_pending_recovery`를 false로 지운다. Option A에서는 recovery op인 Main H2P가 frontend에 보존되어 나중에 main exec까지 도달할 수 있다.

따라서 recovery가 실제로 consume된 뒤 `recover_at_exec = FALSE` 처리가 필요하다.

**삽입 위치**: `cmp_model.c:570-574`의 기존 `tea_early_recovery` 블록 안에 추가한다 — **`recover_decoupled_fe()` 이후** 여야 한다.

```c
/* 기존 코드 (cmp_model.c:570-574) */
if (tea_early_recovery && tea_early_recovery_op &&
    tea_early_recovery_op->op_pool_valid) {
  if (tea_early_recovery_op->recovery_scheduled)
    tea_early_recovery_op->recovery_scheduled = FALSE;
  if (tea_case1_recovery)                                     // NEW
    tea_early_recovery_op->oracle_info.recover_at_exec = FALSE;  // NEW
}
```

**왜 `recover_decoupled_fe()` 이후여야 하는가**: `Decoupled_FE::recover()` (line 249-252)은 `op->oracle_info.recover_at_exec`를 읽어 `FTQ_RECOVER_EXEC` stat을 기록한다. `tea_case1_recovery` 블록(line 515-529)은 `recover_decoupled_fe()` 보다 먼저 실행된다. 그 시점에 `recover_at_exec`를 지우면 stat이 기록되지 않는다. `recovery_sch` guard (`exec_stage.c:1004`)가 same-cycle ASSERT를 막으므로, `recover_at_exec`는 `recover_decoupled_fe()` 이후에 지워도 충분히 안전하다.

**왜 exec_stage에서 즉시 지우면 안 되는가**: `recover_at_exec = TRUE`인 상태로 Main H2P가 rename을 통과할 때 `map_rename.c:964`에서 SRT snapshot을 찍는다. 이 snapshot은 Case 2 early flush에 쓰인다. TEA Case 1 검출 시점 즉시 지우면 rename 단계에서 snapshot 기회가 사라져 Case 2 기회를 박탈한다.

`op_pool_valid` 재확인은 방어적으로 추가한다.

## Step 3. `src/ft.h`, `src/ft.cc`: FT partial trim helper

FTQ partial flush는 FT 단위 erase만으로 충분하지 않다. Recovery op이 FT 중간에 있을 수 있고, 특히 branch가 predicted not-taken이면 같은 FT 안에 younger wrong-path op가 남을 수 있다.

필요 helper:

```cpp
bool contains_op_num(Counter op_num) const;
bool has_unfetched_op_after(Counter op_num) const;
Counter next_unfetched_op_num_or(Counter fallback) const;
void free_ops_after_opnum(Counter recovery_op_num);
```

`free_ops_after_opnum()`는 다음을 수행해야 한다.

- `op_num > recovery_op_num`인 op만 `free_op()`
- `ops` vector를 마지막 preserved op까지 truncate
- `ft_info.static_info.n_uops`를 새 `ops.size()`로 갱신
- `ft_info.static_info.length`를 마지막 preserved op의 end address 기준으로 갱신
- preserved op들의 `op->ft_info`를 갱신

FT metadata 갱신이 중요하다. `Decoupled_FE::update()`의 FT consecutivity check와 uop cache lookup은 `FT_Info`를 사용하므로, ops vector만 줄이고 metadata를 그대로 두면 잘못된 FT length를 사용하게 된다.

**`free_ops_after_opnum()` 구현 시 추가 주의사항 (`ft.cc` 기준)**:

1. **`op_pos` truncation**: `ops.resize(new_size)` 이후 `op_pos > new_size`가 될 수 있다 — 이미 icache가 preserved 범위를 넘어 cursor를 진행시킨 경우. `op_pos = MIN(op_pos, new_size)`로 clamp해야 한다. 이것은 fetch 누락을 막는 것이 아니라, `op_pos > ops.size()`가 되는 invariant 위반과 out-of-range 접근을 방지하기 위한 처리다.

2. **모든 op이 wrong-path인 경우**: FT 안에 `op_num <= recovery_op_num`인 op가 하나도 없으면 FT 전체를 free하고 `new_size = 0`으로 설정한다. 이 FT는 호출자 (DFE partial flush 루프)가 FTQ에서 erase해야 한다.

3. **`ft_info.dynamic_info.ended_by` 처리**: `ft.cc:add_op()`에서 `ended_by`와 `length`는 FT의 마지막 op(branch)를 추가할 때만 설정된다. 이미 FTQ에 들어간 FT는 항상 `ended_by != FT_NOT_ENDED`이므로, `free_ops_after_opnum()`이 FTQ 순회 중에 `FT_NOT_ENDED` FT를 만나는 경우는 없다. `FT_NOT_ENDED`는 `current_ft_to_push`에서만 발생한다. `current_ft_to_push`를 trim 후 FTQ에 materialize할 때는 반드시 synthetic `ended_by`를 설정해야 한다 — `FT_NOT_ENDED`인 FT를 그대로 push하면 `decoupled_frontend.cc:437`의 ASSERT와 uop cache lookup이 실패한다. 구체적 처리는 Step 4의 materialize 경로에 기술한다.

4. **`ft_info.static_info.length` 재계산**: `ended_by != FT_NOT_ENDED`인 경우만 재계산한다. `ops[new_size-1]->inst_info->addr + ops[new_size-1]->inst_info->trace_info.inst_size - ft_info.static_info.start`로 계산한다.

5. **`ft_info.static_info.n_uops`**: 항상 `new_size`로 갱신한다.

## Step 4. `src/decoupled_frontend.cc`: Partial FTQ recovery

`Decoupled_FE::recover()`의 full flush를 다음 정책으로 바꾼다.

1. `recovery_op_num = bp_recovery_info->recovery_op_num`
2. FTQ 앞에서부터 순회한다.
3. unfetched/preserved 영역에 `op_num <= recovery_op_num`이 있으면 보존한다.
4. recovery op을 포함한 FT는 `free_ops_after_opnum(recovery_op_num)`로 FT 내부 suffix만 제거한다.
5. recovery op 이후 FT들은 suffix erase로 제거한다.
6. **`current_ft_to_push` 처리** (두 가지 경로):
   - `current_ft_to_push.ops`를 순회해 `op_num <= recovery_op_num`인 op가 있는지 확인한다.
   - **있으면 (Main H2P가 `current_ft_to_push` 안에 있음)**: `free_ops_after_opnum(recovery_op_num)`로 wrong-path suffix를 free하고, 남은 ops로 FT metadata를 재구성한다:
     - `ended_by = FT_ICACHE_LINE_BOUNDARY` (synthetic — `FT_NOT_ENDED`로 push하면 `line 437` ASSERT 실패)
     - `length` = 마지막 preserved op의 end address - `ft_info.static_info.start`
     - `n_uops` = preserved ops 수
     - `set_per_op_ft_info()`로 각 op에 전파
     - **`eom` 전제**: force-materialize 후 `ops.back()` = Main H2P다. Main H2P는 항상 branch(single uop)이므로 `eom == TRUE`가 보장된다. `decoupled_frontend.cc:439`의 `ops.back()->eom` ASSERT는 해당 경로에 없으므로(force-materialize는 `recover()`에서 `ftq.emplace_back()` 직접 호출 — `update()`의 정상 push ASSERT를 우회) 실제 ASSERT 위험 없음.
     - 연속성 ASSERT (`line 441-452`): `current_ft_to_push`는 FTQ.back()과 동일한 prediction stream에서 연속으로 추가된 FT다. `current_ft_to_push.start`는 FTQ.back()의 마지막 op 다음 주소와 일치하므로, force-materialize 시 consecutivity check는 자연적으로 통과한다. 이 경로에서는 ASSERT bypass가 불필요하다. (bypass가 필요한 것은 force-materialize된 FT 다음에 append되는 `FT_STARTED_BY_RECOVERY` 새 FT다 — true NPC가 Main H2P의 mispredicted `pred_npc`와 다르기 때문이며, 이는 아래 invariant 절에서 처리한다.)
     - FTQ 끝에 push (force-materialize).
     - `current_ft_to_push = FT(proc_id)` + `FT_STARTED_BY_RECOVERY`로 초기화.
   - **없으면 (모든 ops이 wrong-path)**: `free_ops_and_clear()` 후 `FT_STARTED_BY_RECOVERY`로 초기화.
7. `dfe_op_count = recovery_op_num + 1`로 설정한다.
8. `frontend_recover(proc_id, recovery_inst_uid)`는 기존처럼 호출해 다음 새 FT generation을 true NPC에서 시작하게 한다.

추가로 반드시 처리해야 하는 invariant:

- `ftq.erase()`는 preserved prefix를 건드리지 않고 suffix만 제거해야 한다.
- `ic->current_ft` 또는 `uc->current_ft`가 preserved FT를 가리킬 수 있으므로, preserved FT reference가 깨지지 않게 suffix erase만 사용한다.
- recovery 이후 새 FT는 `FT_STARTED_BY_RECOVERY`로 append된다. FTQ prefix가 남아 있으므로 기존 consecutivity assert는 그대로 두면 실패할 수 있다. `current_ft_to_push.ft_info.dynamic_info.started_by == FT_STARTED_BY_RECOVERY`인 경우에는 이전 FT의 `pred_npc`/fall-through 연속성 assert를 건너뛰어야 한다.

## Step 5. `src/decoupled_frontend.cc`: FDIP iterator clamp

FDIP는 DFE FTQ iterator를 소유하지 않고, `Decoupled_FE`의 `ftq_iterators` vector 안에 등록된 iterator를 사용한다. 따라서 partial FTQ recovery 후 모든 iterator를 다음 기준으로 보정한다.

- iterator가 preserved FT 안의 preserved op를 가리키면 그대로 둔다.
- iterator가 trimmed된 FT의 제거된 op_pos를 가리키면 FTQ end로 clamp한다. 구체적으로: `ft_pos < ftq.size()` 이면서 `op_pos >= ftq[ft_pos].ops.size()`인 경우다. 이때 `ftq_iter_get()`의 `decoupled_frontend.cc:532` ASSERT(`op_pos < ops.size()`)가 발동하므로 반드시 FTQ end로 clamp해야 한다.
- iterator가 erased suffix를 가리키면 FTQ end로 clamp한다.
- FTQ가 비었으면 `{ft_pos=0, op_pos=0, flattened_op_pos=0}`으로 reset한다.

여기서 kept-end는 현재 보존된 FTQ의 "마지막 op 다음 위치"를 의미한다.

```text
ft_pos = ftq.size()
op_pos = 0
flattened_op_pos = preserved_num_ops
```

`Decoupled_FE::ftq_iter_get()`는 `iter->ft_pos == ftq.size()`일 때 NULL을 반환한다. 따라서 kept-end로 clamp하면 FDIP는 이미 보존된 prefix를 다시 스캔하지 않고, recovery 이후 새 FT가 append될 때부터 다시 진행한다.

full flush처럼 모든 iterator를 0으로 되돌리면 FDIP가 이미 처리한 preserved FT를 다시 스캔한다. correctness bug는 아니지만 prefetch traffic과 FDIP usefulness stat을 왜곡하므로, Option A에서는 kept-end clamp가 더 맞다.

`recover_fdip()` 호출 순서는 현재 `cmp_model.c`처럼 `recover_decoupled_fe()` 다음이면 된다. DFE가 FTQ와 iterator를 먼저 정리한 뒤 FDIP가 `last_recover_cycle`을 갱신하는 순서다.

## Step 6. `src/decoupled_frontend.h`: icache용 query API

`src/icache_stage.c`는 C 파일이므로 DFE 내부 `std::deque<FT>`를 직접 볼 수 없다. Partial recovery에서는 `ic->current_ft`와 `uc->current_ft`를 무조건 NULL로 만들면 안 되므로, DFE 상태를 질의하는 C API가 필요하다.

필요 API 예시:

```c
Flag decoupled_fe_ftq_contains_ft(FT* ft);
Counter decoupled_fe_next_unfetched_op_num_or(Counter fallback);
```

용도:

- `decoupled_fe_ftq_contains_ft(ft)`: current FT pointer가 preserved FTQ 안에 아직 살아 있는지 확인
- `decoupled_fe_next_unfetched_op_num_or(fallback)`: icache `op_count[proc_id]`를 `recovery_op_num + 1`로 무조건 점프시키지 않고, preserved FTQ에 남아 있는 다음 fetch op 번호로 유지

## Step 7. `src/icache_stage.c`: icache output/current FT partial recovery

`recover_icache_stage()`는 다음처럼 바뀌어야 한다.

1. icache output buffer에서 `FLUSH_OP(op)`인 op만 free한다.
2. `FLUSH_OP(op) == FALSE`인 op은 보존하고 stage data를 compact한다.
3. `cur_data->op_count`는 보존한 op 수로 재계산한다.
4. `op_count[proc_id]`는 DFE helper가 알려주는 다음 unfetched preserved op num으로 설정한다. 그런 op가 없으면 `recovery_op_num + 1`로 설정한다.
5. `ic->current_ft`와 `uc->current_ft`는 DFE FTQ에 아직 존재하는 preserved FT이면 유지한다. erased suffix에 속하면 NULL 처리한다.
6. `uop_cache_clear_lookup_buffer()`는 유지한다. Recovery 이후 lookup buffer는 이전 FTQ state와 맞지 않을 수 있다.

기존 코드의 다음 처리는 partial recovery에서는 위험하다.

```c
op_count[ic->proc_id] = bp_recovery_info->recovery_op_num + 1;
uc->current_ft = NULL;
ic->current_ft = NULL;
```

Main H2P가 FTQ 또는 current FT 안에 아직 있으면, 위 코드는 stat/debug 카운터(`op_count`)를 잘못된 값으로 설정하고, `current_ft` 포인터를 무효화해 preserved FT를 orphan으로 만든다.

**`op_count[proc_id]` 역할 명확화**:

`icache_stage.c`의 `op_count[proc_id]`는 debug 로그용 카운터다(`op_count[ic->proc_id]++` at line 864). 실제 fetch 순서는 FT의 `op_pos` 커서(`ft_fetch_op()`)가 제어하므로, `op_count`를 잘못 설정해도 fetch 순서 자체는 바뀌지 않는다. Partial recovery 이후 이 값을 보정하는 이유는 fetch 누락 방지가 아니라 debug/stat 일관성 유지다.

Full recovery에서 `op_count = recovery_op_num + 1`로 리셋하는 이유는 FTQ가 비워지므로 다음 fetch가 `recovery_op_num + 1` op_num부터 시작하기 때문이다. Partial recovery에서는 FTQ에 preserved ops가 남아 있으므로, `op_count`를 `recovery_op_num + 1`로 설정하면 preserved ops와 debug counter가 어긋난다. DFE helper `decoupled_fe_next_unfetched_op_num_or(recovery_op_num + 1)`로 실제 preserved FTQ의 다음 unfetched op_num을 얻어 설정한다.

**`ic->current_ft`와 FTQ 멤버십**:

`ic->current_ft`가 non-null인 동안 해당 FT는 FTQ deque에 여전히 존재한다. Icache는 FT를 다 소비한 후 `ft_set_consumed()` + `ic->current_ft = NULL`을 호출하고, FTQ popping은 다음 `Decoupled_FE::update()` cycle에서 일어난다. 따라서 `decoupled_fe_ftq_contains_ft(ic->current_ft)`는 partial recovery 시점에서 정확히 동작한다 — 이 FT가 preserved prefix에 속하면 TRUE를 반환하고, icache는 기존 포인터를 그대로 유지하여 계속 fetch할 수 있다.

## Step 8. 기존 stage들의 상태

아래 stage들은 이미 `FLUSH_OP` 기준 partial flush 구조를 갖고 있으므로 1차 구현에서 직접 수정하지 않는다.

| 파일 | 현재 상태 |
|---|---|
| `src/decode_stage.c` | `FLUSH_OP`이면 free, 아니면 compact |
| `src/uop_queue_stage.cc` | queue 내부 `FLUSH_OP`만 free |
| `src/idq_stage.cc` | queue와 output sd에서 `FLUSH_OP`만 free, `next_op_num`은 `recovery_op_num`보다 클 때만 clamp |
| `src/map_stage.c` | `FLUSH_OP`만 free, `map_stage_next_op_num`은 `recovery_op_num`보다 클 때만 clamp |
| `src/uop_cache.cc` | accumulation buffer가 recovery point 이후일 때만 clear |
| `src/map_rename.c` | checkpoint 없음 case를 이미 skip하고, rename hook은 fallback으로 유지 가능 |

다만 validation에서 `next_op_num` 또는 `map_stage_next_op_num` assertion이 발생하면, preserved frontend prefix와 stage counter 사이에 아직 맞지 않는 경계가 있다는 뜻이므로 이 두 파일을 2차 수정 대상으로 본다.

## Step 9. `src/tea/tea.stat.def`: 검증 stat 추가

최소 추가 stat:

```c
DEF_STAT(TEA_EARLY_FLUSH_CASE1_IMMEDIATE, COUNT, NO_RATIO)
DEF_STAT(TEA_EARLY_FLUSH_CASE1_IMMEDIATE_FTQ, COUNT, NO_RATIO)
DEF_STAT(TEA_EARLY_FLUSH_CASE1_IMMEDIATE_FETCHED, COUNT, NO_RATIO)
```

기존 stage classification stat과 함께 확인한다.

- `TEA_EARLY_FLUSH_CASE1_PENDING_TRIGGERED`: 거의 0이어야 함
- `TEA_EARLY_FLUSH_CASE1_IMMEDIATE`: Case 1 count와 거의 같아야 함
- `TEA_EARLY_FLUSH_CASE1_IMMEDIATE_FTQ`: 기존 `TEA_EARLY_FLUSH_CASE1_MAIN_STAGE_PRE_DECODE_FTQ`와 대응
- `TEA_EARLY_FLUSH_CASE1_TO_SCHEDULE_AVG`: pending rename 대기가 사라져 0에 가까워져야 함
- `TEA_EARLY_FLUSH_CASE1_TO_RECOVERY_AVG`: schedule latency 중심으로 크게 감소해야 함

FDIP 영향은 기존 memory/fetch stat으로 같이 본다.

- `ICACHE_HIT_BY_FDIP_ONPATH`
- `ICACHE_HIT_BY_FDIP_OFFPATH`
- `ICACHE_MISS_MSHR_HIT_BY_FDIP_ONPATH`
- `ICACHE_MISS_MSHR_HIT_BY_FDIP_OFFPATH`
- `ICACHE_FILL_CORRECT_REQ_BY_FDIP`
- `ICACHE_FILL_INCORRECT_REQ_BY_FDIP`
- `FDIP_AVG_FTQ_OCCUPANCY_OPS`
- `FDIP_AVG_FTQ_OCCUPANCY`

## 논문 §IV-F 정합성 확인

논문의 Partial Frontend Flush 요구사항 4가지와 Scarab 구현의 대응 관계를 정리한다.

| 논문 §IV-F 요구사항 | Scarab 구현 대응 | 이 계획의 처리 |
|---|---|---|
| 각 pipeline stage마다 timestamp comparator로 flush 여부 결정 | `FLUSH_OP(op)` = `op->op_num > recovery_op_num` — 이미 decode/uop_queue/idq/map stage에서 사용 중 | Step 8에서 확인: 기존 stage들은 이미 `FLUSH_OP` 기반 partial flush 구조를 갖고 있음 |
| Fetch Queue(FTQ) partial flush | 현재는 full flush (`Decoupled_FE::recover()`) | Step 3+4: `free_ops_after_opnum()` + partial FTQ sweep으로 대체 |
| Partial flush 시 main RAT recovery 불필요 | `reg_renaming_scheme_realistic_recover()`가 `!reg_file_checkpoint_is_valid()`이면 early return | 기존 구현이 이미 처리함. 추가 수정 불필요 |
| Shadow RAT는 checkpoint로 fix | 논문과 Scarab 구현이 다름 — 아래 설명 참조 | chain termination(`recover_tea_rename_stage_by_chain()`)으로 per-chain shadow RAT invalidate |

**논문과 Scarab 구현의 차이점 (중요)**: 논문(§IV-F)은 TEA thread가 main thread보다 멀리 앞서가면 shadow RAT를 checkpoint해뒀다가 recovery 시 복원한다고 설명한다. Scarab의 SRT checkpoint(`reg_file_snapshot_srt()`, `map_rename.c:532`)는 main RAT(Speculative Register Table)를 저장하며, TEA Shadow RAT와는 별개다. TEA Shadow RAT는 chain termination 시 `tea_thread.c:1090`의 `recover_tea_rename_stage_by_chain()`으로 정리되고, `tea_thread.c:1131` 주석("Shadow RAT was already invalidated by recover_tea_rename_stage_by_chain")이 이를 확인한다. 즉, Scarab에서 Shadow RAT "fix"는 논문처럼 checkpoint rollback이 아니라 chain terminate 시 per-chain invalidation으로 구현된다. 이 차이는 기존 Scarab 설계이며, Case 1 early flush 계획은 이 구조를 그대로 따른다.

**Partial FTQ flush가 "full misprediction penalty 절약"을 달성하는 이유**: 논문은 "FTQ가 partial flush되면 full penalty가 절약된다"고 말한다. Full FTQ flush를 하면 Main H2P보다 오래된 correct-path FT들도 버려지고, recovery address(Main H2P true NPC)부터 다시 fetch해도 Main H2P 자신은 그 NPC 이전에 위치하므로 건너뛰게 된다. Partial flush는 Main H2P를 포함한 prefix를 보존해, Main H2P의 동일 dynamic instance가 pipeline에서 계속 진행된다 — 이때 misprediction은 이미 감지되어 backend flush가 진행 중이므로, frontend는 correct-path로 즉시 이어갈 수 있다. 이것이 "full misprediction penalty 절약"의 실체다.

## 최종 correctness checklist

- Main H2P가 FTQ에 있을 때 Main H2P와 그 이전 FTQ prefix가 free되지 않는다.
- Main H2P가 icache output buffer에 있을 때 `recover_icache_stage()`가 assert하지 않고 해당 op를 보존한다.
- Recovery 이후 DFE는 Main H2P true NPC에서 새 op를 생성하고, 새 op_num은 `recovery_op_num + 1`부터 시작한다.
- `op_count[]`는 preserved FTQ의 다음 unfetched op_num으로 보정되어 debug/stat이 일관된다 (fetch 순서 자체는 FT::op_pos가 제어).
- FDIP iterator는 erased suffix를 가리키지 않는다.
- Case 1 recovery가 consume된 뒤 Main H2P가 main exec에서 다시 `bp_sched_recovery()`를 호출하지 않는다.
- 기존 pending rename hook은 fallback으로 남지만, Option A 정상 경로에서는 pending table이 채워지지 않는다.

## 검증 절차

1. `tea_baseline` 단일 config로 먼저 smoke run
2. assert 없이 완료되는지 확인
3. `TEA_EARLY_FLUSH_CASE1_IMMEDIATE`, `TEA_EARLY_FLUSH_CASE1_PENDING_TRIGGERED`, `TEA_EARLY_FLUSH_CASE1_TO_RECOVERY_AVG` 확인
4. FDIP 관련 `ICACHE_*_FDIP*`, `FDIP_AVG_FTQ_OCCUPANCY*`가 비정상적으로 폭증하지 않는지 확인
5. 이후 `tea_baseline`, `tea_perfect_load_5cycle`, `tea_perfect_load_1cycle` 세 config를 모두 돌려 Case 1 증가분이 실제 IPC 개선으로 이어지는지 비교
