# TEA Op 관리 계획

**최종 갱신**: 2026-04-27
**상태**: Work F 1차 구현 반영

---

## 1. 목적

TEA op 관리는 main op과 TEA op을 같은 backend resource 위에서 안전하게 공존시키는 것을 목표로 한다. Work F 이후에는 TEA op 전체가 아니라 특정 H2P chain의 op만 선택적으로 종료할 수 있어야 한다.

---

## 2. 현재 구현

### Op 식별

- Main op: `thread_id == 0`, `h2p_chain_id == 0`이어야 한다.
- TEA op: `thread_id == 1`, `h2p_chain_id in 1..MAX_TEA_CHAINS`.
- TEA op은 `tea_create_op_from_cache()`에서 chain id와 별도 op_num을 받는다.

### Dispatch

- TEA op은 `tea_dispatch_to_rs()`에서 Node Table 진입과 동시에 RS dispatch를 시도한다.
- RS full이면 `OS_IN_ROB`로 남고 `tea_dispatch_retry()`가 재시도한다.
- Main dispatch path는 TEA op을 skip한다.

### Completion / retire

- `tea_op_completed()`는 `op->state = OS_DONE`만 수행한다.
- `tea_op_count` 감소는 `node_retire_tea_ops()`에서 `free_op()` 직전에 수행한다.
- 이 방식은 0-latency op에서 free 이후 접근이 발생하는 문제와 double decrement를 피한다.

### Selective flush

`terminate_tea_chain()`은 다음 chain-local cleanup을 한 곳에서 수행하는 canonical API다.

- `recover_tea_fetch_stage_by_chain()`
- `recover_tea_rename_stage_by_chain()`
- `flush_tea_ops_by_chain_id()`
- `tea_store_buffer_clear_by_chain_id()`

마지막 active chain이 끝날 때만 shared preg pool, store buffer, Shadow RAT, TEA state를 reset한다.

---

## 3. 구현 원칙

- Chain별 종료는 `terminate_tea_chain()`을 통해서만 수행한다.
- Full TEA 종료가 필요한 경우에는 `terminate_tea_thread()`를 사용한다.
- Per-chain preg 반환은 하지 않는다.
- Per-chain op count는 fetch 시 증가, node retire 시 감소, forced terminate 시 chain reset으로 정리한다.
- Selective flush는 main op을 건드리지 않아야 한다.

---

## 4. 현재 디버깅 계획

| 우선순위 | 작업 | 기대 결과 |
|----------|------|-----------|
| P0 | `op_pool_setup_op()`에서 `h2p_chain_id=0` reset 검토 | stale chain id 제거 |
| P0 | `flush_tea_ops_by_chain_id()`의 모든 filter에 `thread_id == 1` guard 검토 | main op 오염 방지 |
| P1 | fetch/rename/node selective flush 후 `sd.op_count` 일관성 확인 | sparse buffer와 count mismatch 방지 |
| P1 | `TEA_OPS_FETCHED/DISPATCHED/RETIRED/FLUSHED` consistency 확인 | op leak 또는 double free 탐지 |

---

## 5. 시뮬레이션 체크리스트

- `TEA_OPS_FETCHED >= TEA_OPS_DISPATCHED >= TEA_OPS_RETIRED` 관계가 workload별로 합리적인지 확인.
- Forced terminate가 많은 경우 `TEA_OPS_FLUSHED`와 `TEA_CHAIN_TERMINATED`가 같이 증가하는지 확인.
- RS counter ASSERT 또는 op pool exhaustion이 재발하지 않는지 확인.
- TEA store buffer full이 전체 TEA 종료를 과도하게 유발하지 않는지 확인.
