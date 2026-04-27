# TEA Op 관리 현재 구현 상태

**최종 갱신**: 2026-04-27
**관련 계획**: `../TEA_implementation_plan/TEA_op_manage_plan.md`

---

## 1. 요약

TEA op 관리는 단일 H2P 모델에서 per-chain 모델로 확장된 상태다. TEA op은 `thread_id == 1`과 `h2p_chain_id > 0`으로 식별되고, 각 chain의 op 수는 `Tea_H2P_Chain.tea_op_count`로 관리된다.

| 항목 | 현재 상태 |
|------|-----------|
| TEA op 별도 op_num 공간 | 구현됨 |
| `h2p_chain_id` 태깅 | 구현됨 |
| Direct RS dispatch | 구현됨 |
| RS full retry | 구현됨 |
| per-chain `tea_op_count` | 구현됨 |
| per-chain fetch/rename/node/store flush | 구현됨 |
| full TEA flush | 유지됨 |
| Work F 이후 runtime 검증 | 필요 |

---

## 2. 생성과 식별

`tea_create_op_from_cache()`는 TEA op을 op pool에서 할당한 뒤 다음을 설정한다.

- `thread_id = 1`
- `h2p_chain_id = current_chain_id + 1`
- `op_num = tea_op_counter++`
- `state = OS_FETCHED`
- H2P branch인 경우 해당 chain의 saved oracle/recovery info 복사

TEA op counter는 전체 TEA thread가 공유하며 `0x8000...` 영역에서 시작한다. Chain별로 별도 op_num 공간을 만들지 않는다.

---

## 3. Dispatch / execution / retire

TEA op은 `tea_dispatch_to_rs()`에서 Node Table에 들어가는 즉시 RS dispatch를 시도한다. RS가 full이면 `OS_IN_ROB`로 남고, `tea_dispatch_retry()`가 매 cycle 재시도한다. Main dispatch path인 `node_issue_queue_dispatch()`는 `thread_id == 1` op을 skip한다.

`tea_op_completed()`는 `OS_DONE`만 설정한다. `tea_op_count` 감소는 `node_retire_tea_ops()`가 op을 free하기 직전에 수행한다. 이 위치가 authoritative decrement point다. 0-latency TEA op의 use-after-free와 double decrement를 피하기 위한 설계다.

---

## 4. 종료 경로

### Per-chain 종료

`terminate_tea_chain(proc_id, chain_slot)`은 다음을 순서대로 수행한다.

1. 현재 fetch 중인 chain이면 `recover_tea_fetch_stage_by_chain()`.
2. rename stage의 해당 chain op을 `recover_tea_rename_stage_by_chain()`으로 제거.
3. exec/dcache/ready/scheduling/node table의 해당 chain op을 `flush_tea_ops_by_chain_id()`로 제거.
4. store buffer의 해당 chain entry를 `tea_store_buffer_clear_by_chain_id()`로 제거.
5. chain state/counters를 reset.
6. 마지막 active chain이면 shared preg pool, store buffer, Shadow RAT, TEA state를 reset.

### Full 종료

`terminate_tea_thread()`는 전체 TEA pipeline을 일괄 flush하는 경로로 남아 있다. 전체 종료가 필요한 fatal/backpressure 상황에서 사용한다.

---

## 5. 디버깅 주의점

현재 소스 기준으로 가장 중요한 점검 항목:

- `op_pool_setup_op()`에서 `h2p_chain_id`를 0으로 reset하지 않으면 op pool 재사용 시 stale chain id가 남을 수 있다.
- `flush_tea_ops_by_chain_id()`와 fetch/rename selective recovery는 가능한 모든 필터에서 `thread_id == 1 && h2p_chain_id == chain_id`를 함께 확인하는 형태가 안전하다.
- selective flush가 `sd.op_count--`만 수행할 때 stage buffer가 sparse해지는지 확인해야 한다.
- `flush_tea_ops_by_chain_id()`는 flushed TEA producer의 dependent not-ready bit는 정리하지만, Main recovery로 사라진 producer에 대한 surviving TEA consumer 정리는 별도 로직이 필요하다.

이 문서는 현 구현을 반영한다. 위 항목은 소스 수정 전 시뮬레이션과 ASSERT 결과로 우선순위를 정한다.
