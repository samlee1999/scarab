# TEA Early Flush 현재 구현 상태

**최종 갱신**: 2026-04-27
**관련 계획**: `../TEA_implementation_plan/TEA_early_flush_plan.md`

---

## 1. 요약

Early Flush는 현재 multi-H2P chain 기준으로 동작한다. TEA H2P branch가 execute되면 `h2p_chain_id`로 chain slot을 찾고, 해당 chain의 Main H2P 정보를 사용해 Case 1/2를 처리한다.

| 항목 | 현재 상태 |
|------|-----------|
| TEA H2P chain 식별 | `op->h2p_chain_id` 기반 |
| Main H2P pointer 검증 | `op_pool_valid` + `saved_unique_num` |
| Case 1 no SRT checkpoint | 해당 chain만 즉시 종료 |
| Case 2 SRT checkpoint 있음 | Main H2P recovery schedule |
| Correct TEA H2P | 해당 chain 정상 종료 |
| Main recovery 시 TEA 처리 | recovery point 이상 chain만 종료 |

---

## 2. Case 동작

### Case 1: Main H2P가 아직 SRT checkpoint를 만들지 못한 경우

`reg_file_checkpoint_is_valid() == FALSE`이면 즉시 SRT rollback을 할 수 없다. 현재 구현은 Main H2P의 flag를 새로 조작하지 않고, TEA가 계산한 해당 chain만 종료한다.

- Main H2P가 decode를 통과했으면 `TEA_EARLY_FLUSH_CASE1_NO_CHKPT`.
- 아직 decode 전이면 `TEA_EARLY_FLUSH_CASE1_DECODE`.
- 두 경우 모두 `terminate_tea_chain(proc_id, chain_slot)`만 호출한다.
- Main H2P는 기존 main pipeline의 branch recovery 경로에서 자연스럽게 처리된다.

### Case 2: Main H2P가 SRT checkpoint를 가진 경우

`reg_file_checkpoint_is_valid() == TRUE`이면 TEA가 Main H2P 기준 recovery를 schedule한다.

- `bp_sched_recovery()` 호출.
- Main H2P의 `recovery_scheduled`를 설정해 retire를 막는다.
- `recover_at_exec = FALSE`로 double recovery를 막는다.
- 다음 `cmp_recover()`에서 `recover_tea_on_flush(proc_id, recovery_op_num)`가 호출되어 recovery point 이상 chain을 종료한다.

### Correct prediction

TEA H2P가 mispred/misfetch가 아니면 `TEA_H2P_CORRECT`를 기록하고 해당 chain만 정상 종료한다.

---

## 3. Guard

- `tea_is_active(proc_id)`가 false이면 TEA branch resolution은 종료한다.
- `h2p_chain_id`가 유효 range 밖이면 무시한다.
- chain state가 `CHAIN_INACTIVE`이면 residual op으로 보고 무시한다.
- TEA op PC가 chain의 `target_h2p_pc`와 다르면 early flush 대상이 아니다.
- Main H2P pointer는 `op_pool_valid`와 `saved_unique_num`으로 검증한다.
- Main H2P가 off-path이면 이전 recovery가 처리한다고 보고 early flush를 skip한다.
- 이미 `recovery_sch`가 있으면 중복 recovery를 schedule하지 않는다.

---

## 4. Recovery 연동

`cmp_recover()`의 main recovery path가 frontend/backend stage를 recovery한 뒤 `recover_tea_on_flush(proc_id, recovery_op_num)`을 호출한다. 이 함수는 active chain을 순회하면서 `target_h2p_op_num >= recovery_op_num`인 chain만 종료한다. Recovery point보다 older한 chain은 생존할 수 있다.

생존 chain이 있는 구조이므로, Main recovery로 사라진 producer에 대한 TEA consumer dependency cleanup은 별도 점검 대상이다. 현재 문서 기준 이 부분은 아직 명시적으로 구현된 것으로 보지 않는다.

---

## 5. 검증 포인트

- Case 1에서 `recover_tea_on_flush()`를 직접 부르지 않고 해당 chain만 종료하는지.
- Case 2에서 Main H2P recovery가 한 번만 schedule되는지.
- `saved_unique_num` mismatch 때 chain이 안전하게 종료되는지.
- Main recovery 후 older chain이 생존하는 경우 RS에서 영구 not-ready가 발생하지 않는지.
- `TEA_EARLY_FLUSHES`, Case breakdown stat, `TEA_CHAIN_TERMINATED`가 서로 일관되는지.
