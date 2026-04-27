# TEA Register Dependency 현재 구현 상태

**최종 갱신**: 2026-04-27
**관련 계획**: `../TEA_implementation_plan/TEA_reg_dependency_plan.md`

---

## 1. 요약

TEA register dependency는 Shadow RAT와 기존 wakeup list 인프라를 사용한다. Poison bit는 구현하지 않으며, 시뮬레이터의 oracle 정보를 사용한다는 전제에서 dependency chain은 올바른 것으로 취급한다.

| 항목 | 현재 상태 |
|------|-----------|
| Shadow RAT arch->phys mapping | 구현됨 |
| Shadow RAT producer tracking | 구현됨 |
| TEA preg pool | 구현됨 |
| TEA src dependency 등록 | 구현됨 |
| Main/TEA producer wakeup | 구현됨 |
| per-chain flush 시 TEA producer wakeup propagation | 구현됨 |
| Main recovery 후 surviving TEA dependency cleanup | 추가 점검 필요 |
| Poison bit | 구현하지 않음 |

---

## 2. 현재 dependency 경로

1. 첫 active TEA chain trigger 시 `shadow_rat_snapshot()`이 Main map state와 producer op/unique_num을 복사한다.
2. `tea_rename_op()`은 Shadow RAT에서 source mapping과 producer를 읽는다.
3. 유효한 producer가 있으면 `tea_add_src_dependency()`로 `srcs_not_rdy_vector`를 세팅한다.
4. `add_to_wake_up_lists()`가 기존 Main wakeup infrastructure에 TEA consumer를 연결한다.
5. Producer가 execute되면 `wake_up_ops()`와 `cmp_wake()`가 TEA consumer를 ready list로 보낸다.
6. Destination register는 TEA preg pool에서 할당하고 Shadow RAT producer를 현재 TEA op으로 갱신한다.

이 구조는 cross-thread dependency(Main producer -> TEA consumer)와 intra-TEA dependency(TEA producer -> TEA consumer)를 모두 처리한다.

---

## 3. Multi-H2P에서의 의미

Work F 이후 모든 active chain이 하나의 Shadow RAT을 공유한다. Fetch는 sequential이므로 Chain A rename이 Shadow RAT을 갱신한 뒤 Chain B rename이 그 결과를 볼 수 있다. 이는 논문 모델의 "sequential fetch, overlapped execution"과 맞춘 설계다.

Per-chain preg 반환은 하지 않는다. 특정 chain만 종료할 때 preg mapping을 부분적으로 되돌리면 다른 생존 chain의 Shadow RAT mapping이 dangling될 수 있기 때문이다. 마지막 active chain이 종료될 때 shared preg pool과 Shadow RAT을 reset한다.

---

## 4. Flush와 dependency

`flush_tea_ops_by_chain_id()`는 특정 chain의 TEA producer를 free하기 전에 wakeup list를 보고 dependent op의 not-ready bit를 clear한다. 같은 chain의 dependent는 어차피 같이 free되므로 skip하고, 다른 chain이나 main op이 기다리고 있을 때만 unblock한다.

`free_op()`은 기존처럼 `free_wake_up_list()`를 호출하므로 Wake_Up_Entry 메모리 반환은 별도 코드가 필요 없다.

---

## 5. 남은 위험

Main recovery가 younger main producer를 flush하고, recovery point보다 older한 TEA chain이 살아남는 경우가 있다. 이때 생존 TEA op이 flushed main producer를 기다리고 있었다면 해당 not-ready bit를 명시적으로 clear해야 한다. 현재 `recover_tea_on_flush()`는 younger/equal chain 종료만 수행하므로 이 cleanup은 구현 여부를 추가 확인해야 한다.

다음 코드 수정 후보:

- `recover_tea_on_flush()` chain 종료 루프 이후 surviving TEA op을 순회한다.
- 각 src의 producer가 `op_pool_valid == FALSE`이거나 `unique_num` mismatch이면 해당 ready bit를 clear한다.
- `srcs_not_rdy_vector == 0`이고 op이 RS 안에 있으면 ready list에 등록한다.

이 항목은 Work F 안정화에서 P1 디버깅 후보로 둔다.
