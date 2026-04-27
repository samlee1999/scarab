# TEA Register Dependency 계획

**최종 갱신**: 2026-04-27
**상태**: Work G 구현 완료, Work F dependency cleanup 일부 점검 필요

---

## 1. 목적

TEA thread는 Main thread의 architectural register state를 기반으로 dependency chain을 빠르게 실행해야 한다. 이를 위해 Shadow RAT, TEA preg pool, producer tracking, wakeup list를 사용한다.

Poison bit는 구현하지 않는다. 현재 시뮬레이터 모델은 oracle 정보를 사용하므로 dependency chain validity를 별도 poison bit로 검증하지 않는다.

---

## 2. 현재 구현

### Shadow RAT

- 첫 active TEA chain trigger 시 Main RAT/producer state를 snapshot한다.
- 모든 active chain은 하나의 Shadow RAT을 공유한다.
- Chain A rename 결과는 Chain B rename에서 볼 수 있다.

### Source dependency

- `tea_rename_op()`은 Shadow RAT에서 source phys reg와 producer op/unique_num을 읽는다.
- 유효한 producer가 있으면 `srcs_not_rdy_vector`를 세팅한다.
- `add_to_wake_up_lists()`로 기존 Scarab wakeup infrastructure에 연결한다.

### Destination dependency

- Destination은 TEA preg pool에서 새 phys reg를 받는다.
- Shadow RAT mapping과 producer op/unique_num을 현재 TEA op으로 갱신한다.

### Completion wakeup

- Main producer 또는 TEA producer가 execute되면 `wake_up_ops()`와 `cmp_wake()`가 dependent TEA op을 깨운다.
- `free_op()`은 기존처럼 wakeup list를 반환하므로 별도 memory cleanup은 필요 없다.

---

## 3. Multi-H2P flush 정책

`flush_tea_ops_by_chain_id()`는 특정 chain의 TEA op을 free하기 전, 그 TEA op을 기다리는 생존 dependent op의 not-ready bit를 clear한다. 같은 chain의 dependent는 같이 flush되므로 건드리지 않는다.

Per-chain preg 반환은 하지 않는다. Shadow RAT과 preg pool은 마지막 active chain 종료 시 reset한다.

---

## 4. 남은 구현 후보

Main recovery에서 younger main producer가 flush되고 older TEA chain이 살아남는 경우가 있다. 이때 surviving TEA op이 flushed main producer를 기다리고 있었다면 not-ready bit가 영구히 남을 수 있다.

후보 구현:

1. `recover_tea_on_flush()`가 종료할 chain을 처리한 뒤 surviving TEA op을 순회한다.
2. 각 source dependency의 producer가 invalid이면 not-ready bit를 clear한다.
3. 모든 dependency가 ready가 된 op이 RS에 있으면 ready list에 넣는다.

이 작업은 Work F 안정화 중 P1로 둔다.
