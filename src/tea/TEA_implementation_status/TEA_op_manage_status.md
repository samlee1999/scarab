# TEA Op 관리 현재 구현 상태

**최종 갱신**: 2026-05-04
**관련 계획**: `../TEA_implementation_plan/TEA_op_manage_plan.md`

---

## 1. 요약

TEA op 관리는 현재 multi-H2P chain 모델 기준으로 동작한다. TEA op은 Main ROB retirement를 사용하지 않고 Node Table/RS/Exec/Dcache를 통과한 뒤 `node_retire_tea_ops()`에서 free된다.

| 항목 | 현재 상태 |
|------|-----------|
| TEA op 별도 op_num 공간 | 구현됨, `TEA_OP_NUM_BASE` 영역 사용 |
| `thread_id == 1` 식별 | 구현됨 |
| `h2p_chain_id` 태깅 | 구현됨 |
| op pool TEA field reset | 구현됨 |
| Direct RS dispatch | 구현됨 |
| RS full retry | 구현됨 |
| per-chain `tea_op_count` | 구현됨 |
| TEA op retire/free | 구현됨, Main commit path 우회 |
| ready-list/RS counter leak 방지 | 구현됨 |
| per-chain fetch/rename/node/store flush | 구현됨 |
| full TEA flush | 유지됨 |

---

## 2. 생성과 식별

`tea_create_op_from_cache()`는 dependency chain cache에 저장된 cached op을 기반으로 TEA op을 생성한다.

설정되는 주요 필드:

- `proc_id = proc_id`
- `thread_id = 1`
- `h2p_chain_id = current_chain_id + 1`
- `fetch_cycle = cycle_count`
- `state = OS_FETCHED`
- `op_num = tea->tea_op_counter++`
- `unique_num`, `unique_num_per_proc`는 새로 할당

H2P branch op은 chain trigger 시점에 저장해 둔 `h2p_oracle_info`, `h2p_recovery_info`를 사용한다. Non-H2P memory op은 cached op의 dynamic oracle memory field를 복사한다. 따라서 TEA load/store의 `va`, `mem_size`, cache miss flag, value 관련 field가 default 값에 의존하지 않도록 보정되어 있다.

`op_pool_setup_op()`는 op 재사용 시 다음 TEA 관련 field를 초기화한다.

- `thread_id = 0`
- `h2p_chain_id = 0`
- `tea_h2p_exec_cycle = MAX_CTR`
- `tea_case1_detect_cycle = MAX_CTR`
- `tea_early_flush_detected = FALSE`
- `tea_early_flush_delta_recorded = FALSE`
- `tea_case1_pending_recovery = FALSE`

---

## 3. Dispatch / execution / retire

### Dispatch

TEA op은 fetch/rename 이후 Node Table에 들어가고, `tea_dispatch_to_rs()`에서 RS dispatch를 시도한다. RS가 full이면 `OS_IN_ROB` 상태로 Node Table에 남고, `tea_dispatch_retry()`가 이후 cycle에 재시도한다.

RS 선택은 Main/TEA partition counter를 같이 본다.

- Main op: `main_op_count < main_rs_limit`
- TEA op: `tea_op_count < tea_rs_limit`

따라서 TEA op은 shared RS 안에서 Main과 물리 구조를 공유하지만, partition limit을 통해 TEA가 Main RS를 무제한 점유하지 못하도록 제한한다.

### Completion

`tea_op_completed()`는 TEA op의 completion을 `OS_DONE`으로 표시하고 `TEA_OPS_EXECUTED`를 기록한다. 이 함수는 chain op count를 줄이지 않는다.

0-latency non-memory TEA op은 exec stage에서 같은 cycle에 `tea_op_completed()`가 호출될 수 있다. Memory TEA op은 dcache path가 완료되어 `tea_op_completed()`를 호출해야 retire 가능하다.

### Retire / free

`node_retire_tea_ops()`는 Node Table을 순회하면서 완료된 TEA op을 제거한다.

- memory op: `state == OS_DONE`일 때만 retire
- non-memory op: `OP_DONE(op) || state == OS_DONE`이면 retire
- ready list에 아직 남아 있으면 제거하고 RS counter를 보정
- 해당 chain의 `tea_op_count--`
- TEA op의 previous mapping preg를 chain pool로 반환
- `TEA_OPS_RETIRED` 기록 후 `free_op()`

`tea_op_count` 감소의 authoritative point는 `node_retire_tea_ops()`다. Exec/dcache completion 시점에서 바로 감소시키지 않는 이유는 same-cycle ready-list/RS counter race와 use-after-free를 피하기 위해서다.

---

## 4. Flush 경로

### Full TEA flush

`flush_tea_ops_from_node_stage()`는 전체 TEA op을 제거한다.

1. exec stage SD에서 TEA op pointer 제거.
2. dcache stage SD에서 TEA op pointer 제거.
3. ready list에서 TEA op 제거 및 `OS_SCHEDULED`/`OS_MISS`/`OS_DONE` 상태의 RS counter 보정.
4. node scheduling buffer에서 TEA op 제거.
5. `next_op_into_rs`가 TEA op을 가리키면 다음 non-TEA op으로 이동.
6. Node Table의 TEA op을 제거/free.

`OS_DONE` TEA op이 ready list에 남아 있어도 `node_ready_op_should_clear_rs()`가 true를 반환하므로 RS counter leak을 막는다.

### Per-chain selective flush

`flush_tea_ops_by_chain_id(proc_id, chain_id)`는 특정 chain의 TEA op만 제거한다.

- exec/dcache SD에서 해당 `thread_id == 1 && h2p_chain_id == chain_id` op 제거
- ready list에서 해당 chain op 제거 및 RS counter 보정
- scheduling buffer에서 해당 chain op 제거
- `next_op_into_rs`가 해당 chain op이면 같은 chain op을 건너뜀
- Node Table에서 해당 chain op 제거/free
- flushed TEA producer를 기다리던 surviving op의 not-ready bit를 clear

같은 chain dependent는 곧 같이 free되므로 wakeup propagation에서 제외한다. 다른 chain이나 main op이 flushed TEA producer를 기다리고 있었다면 ready 상태로 전환될 수 있다.

### Stage buffer 주의점

Fetch/rename selective recovery는 matching op을 `NULL`로 만들고 `sd.op_count--`를 수행한다. 현재 update path는 `max_op_count` 범위를 scan하는 형태라 sparse entry를 견딜 수 있게 작성되어 있다. 다만 향후 stage buffer를 compacted array로 가정하는 코드가 추가되면 이 부분은 다시 점검해야 한다.

---

## 5. Store buffer

TEA store buffer entry는 `h2p_chain_id`로 태깅된다. Per-chain 종료 시 `tea_store_buffer_clear_by_chain_id()`가 해당 chain entry만 제거한다. Full TEA 종료 시에는 `reset_tea_store_buffer()`로 전체 TEA store buffer를 reset한다.

---

## 6. 현재 관찰해야 할 stat

| 목적 | Stat |
|------|------|
| TEA op flow | `TEA_OPS_FETCHED`, `TEA_OPS_DISPATCHED`, `TEA_OPS_ISSUED`, `TEA_OPS_EXECUTED`, `TEA_OPS_RETIRED`, `TEA_OPS_FLUSHED` |
| RS pressure | `TEA_RS_STALLS`, `TEA_RS_COUNTER_FIXUPS` |
| rename/PREG stall | `TEA_RENAME_STALL_PREG`, `TEA_RENAME_STALL_DISPATCH`, `TEA_RENAME_BATCH_*` |
| ready-list race | `TEA_READY_LIST_DONE_CLEARED`, `TEA_FLUSH_READY_LIST_DONE_CLEARED`, `TEA_RETIRE_READY_LIST_ESCAPE` |
| TEA op residence | `TEA_OP_NODE_CYCLES_*`, `TEA_OP_NODE_CYCLES_TOTAL` |
| store buffer | `TEA_STORES_BUFFERED`, `TEA_STORE_FORWARDS`, `TEA_STORE_BUFFER_FULL` |

---

## 7. 남은 제한

현재 TEA op free는 op 단위로 수행되지만, chain context 자체는 `tea_op_count == 0 && tea_ops_fetched > 0`이 될 때 종료된다. 즉 chain slot, pending Case 1 entry, per-chain fetch/rename state는 chain 단위 lifecycle을 유지한다.

Main/TEA backend 공유 자체는 유지된다. Node/RS/Exec/Dcache pressure를 줄이려면 op retire/free 최적화만으로는 부족하고, TEA trigger policy, chain length, RS/PREG reservation, fetch width, backend partition을 함께 봐야 한다.
