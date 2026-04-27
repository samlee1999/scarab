# TEA 독립 Dispatch 계획

**최종 갱신**: 2026-04-27
**상태**: 구현 완료, Work F와 호환

---

## 1. 목적

TEA op dispatch는 Main thread의 in-order `next_op_into_rs` 진행과 분리되어야 한다. TEA op이 RS partition full로 막혀도 Main op dispatch를 되돌리거나 재방문하면 안 된다.

---

## 2. 현재 구현

- `tea_dispatch_to_rs()`가 TEA op을 Node Table에 삽입하면서 즉시 RS dispatch를 시도한다.
- RS가 full이면 TEA op은 `OS_IN_ROB` 상태로 남는다.
- `tea_dispatch_retry()`가 매 cycle RS full로 남은 TEA op을 다시 dispatch한다.
- `node_issue_queue_dispatch()`는 `thread_id == 1` op을 skip하고 Main op만 처리한다.
- RS는 `main_op_count`와 `tea_op_count`를 별도로 추적한다.

---

## 3. Work F 관련 주의점

Per-chain selective flush는 기존 full TEA flush와 같은 RS counter 원칙을 따라야 한다.

- Ready list에서 `OS_SCHEDULED` 또는 `OS_MISS`인 TEA op을 제거하면 RS counter를 감소한다.
- Node Table에서 아직 RS를 점유하는 상태인 TEA op을 free하면 RS counter를 감소한다.
- `OS_IN_ROB`, 이미 scheduled/miss/done 처리된 op은 double decrement하지 않는다.

현재 구현은 동작 형태를 갖췄지만, `h2p_chain_id`만 보는 필터는 `thread_id == 1` guard를 함께 두는 쪽이 안전하다.

---

## 4. 검증 포인트

- RS counter ASSERT 미발생.
- TEA RS full 상황에서 Main dispatch가 멈추지 않음.
- `TEA_RS_STALLS`가 증가해도 `TEA_OPS_DISPATCHED`가 장기적으로 따라옴.
- Per-chain flush 후 `rs_op_count == main_op_count + tea_op_count` invariant 유지.
