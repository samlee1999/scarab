# TEA Register Dependency 현재 구현 상태

**최종 갱신**: 2026-05-04
**관련 계획**: `../TEA_implementation_plan/TEA_reg_dependency_plan.md`

---

## 1. 요약

TEA register dependency는 per-chain Shadow RAT과 기존 Scarab wakeup list를 사용한다. 현재 구현은 모든 active chain이 하나의 Shadow RAT을 공유하는 모델이 아니라, chain slot별 독립 Shadow RAT snapshot을 갖는 모델이다.

| 항목 | 현재 상태 |
|------|-----------|
| per-chain Shadow RAT | 구현됨 |
| Main RAT snapshot at trigger | 구현됨 |
| Main producer pointer/unique tracking | 구현됨 |
| intra-chain TEA producer tracking | 구현됨 |
| cross-chain register dependency | 의도적으로 없음, chain별 독립 snapshot |
| TEA preg reservation | 구현됨 |
| per-chain preg sub-pool | 구현됨 |
| TEA prev-preg retire 반환 | 구현됨 |
| Main recovery 후 surviving TEA stale dep cleanup | 구현됨 |
| Poison bit | 구현하지 않음 |

---

## 2. Shadow RAT 구조

`Tea_Rename_Stage`는 `chain_srats[MAX_TEA_CHAINS]`를 가진다. 각 `Shadow_RAT`은 다음을 보관한다.

- GP/VEC arch-to-phys mapping
- GP/VEC producer op pointer
- producer unique number
- per-chain GP/VEC TEA preg free list
- per-chain PREG range start index
- `is_valid`

`shadow_rat_snapshot(proc_id, chain_slot)`은 TEA trigger 시점에 Main RAT의 architectural mapping과 Main map module의 producer op/unique 정보를 해당 chain slot의 Shadow RAT으로 복사한다.

이 설계의 의미:

- 각 chain은 trigger 시점의 Main speculative state를 독립적으로 본다.
- Chain A의 TEA rename 결과가 Chain B의 source mapping에 영향을 주지 않는다.
- Cross-chain register dependency를 만들지 않으므로 per-chain termination/reset이 안전하다.

---

## 3. Rename dependency 경로

`tea_rename_op()`은 TEA op의 `h2p_chain_id`로 chain slot을 찾고, 해당 slot의 Shadow RAT만 사용한다.

Source register 처리:

1. source arch reg를 chain Shadow RAT에서 phys reg로 변환.
2. Shadow RAT에 저장된 producer op/unique를 확인.
3. producer가 live하고 unique가 맞으면 `tea_add_src_dependency()` 호출.
4. `srcs_not_rdy_vector` bit를 세팅.
5. 기존 `add_to_wake_up_lists()`로 Scarab wakeup list에 연결.

Destination register 처리:

1. 이전 phys mapping을 `prev_dst_reg_id`에 기록.
2. 해당 chain의 TEA preg pool에서 새 phys reg 할당.
3. Shadow RAT mapping을 새 phys reg로 갱신.
4. Shadow RAT producer를 현재 TEA op으로 갱신.

이 구조는 Main producer -> TEA consumer, 같은 chain의 TEA producer -> TEA consumer dependency를 추적한다. 다른 chain의 TEA producer는 같은 Shadow RAT에 기록되지 않으므로 dependency source가 되지 않는다.

---

## 4. PREG 관리

`init_tea_preg_pools()`는 physical register table의 끝부분에서 `TEA_PREG_RESERVATION`개를 TEA 전용으로 예약하고, Main free list에서 제거한다. 이후 runtime `TEA_MAX_CHAINS`로 균등 분할한다.

```
per_chain_pregs = TEA_PREG_RESERVATION / TEA_MAX_CHAINS
```

현재 ASSERT 조건:

- `TEA_PREG_RESERVATION > 0`
- `TEA_PREG_RESERVATION <= REG_TABLE_INTEGER_PHYSICAL_SIZE`
- `TEA_PREG_RESERVATION <= REG_TABLE_VECTOR_PHYSICAL_SIZE`
- `TEA_PREG_RESERVATION % TEA_MAX_CHAINS == 0`
- `per_chain_pregs > 0`

Per-chain termination 시 해당 chain pool은 `reset_tea_preg_pool(proc_id, chain_slot)`으로 reset된다. TEA op retire 시에는 `tea_preg_pool_return_prev()`가 `prev_dst_reg_id`에 기록된 이전 mapping preg를 해당 chain pool로 반환한다. 따라서 긴 chain에서도 retire된 TEA op의 old mapping preg가 일부 재사용될 수 있다.

---

## 5. Main recovery와 stale dependency cleanup

Main recovery는 recovery point 이상의 main op을 flush한다. 이때 recovery point보다 older한 TEA chain은 살아남을 수 있고, 그 chain 안의 TEA consumer가 younger main producer를 기다리고 있을 수 있다.

현재 `recover_tea_on_flush()`는 다음을 수행한다.

1. recovery point 이상의 pending Case 1 flush entry 제거.
2. active chain 중 `target_h2p_op_num >= recovery_op_num`인 chain 종료.
3. surviving TEA op의 source dependency를 scan.
4. source가 main producer이고 `src->op_num >= recovery_op_num`이며 producer pointer가 invalid/unique mismatch이면 not-ready bit clear.
5. Node Table 안의 TEA op이 모든 source ready가 되면 ready list에 재등록.

관련 stat:

- `TEA_STALE_MAIN_DEP_CLEARED`
- `TEA_STALE_MAIN_DEP_RENAME_CLEARED`
- `TEA_STALE_MAIN_DEP_NODE_CLEARED`
- `TEA_STALE_MAIN_DEP_OPS_READIED`

---

## 6. Flush와 wakeup propagation

`flush_tea_ops_by_chain_id()`는 flushed TEA producer의 wakeup list를 순회한다. 같은 chain dependent는 같이 free되므로 skip하고, surviving chain 또는 main op dependent는 not-ready bit를 clear한다. 해당 op이 RS 안에 있고 모든 source가 ready가 되면 ready list에 넣는다.

`free_op()`은 기존 Scarab 경로처럼 wakeup list memory를 반환한다. 따라서 TEA selective flush에서 wakeup list entry를 별도로 free할 필요는 없다.

---

## 7. 남은 제한

| 제한 | 설명 |
|------|------|
| Poison bit | 구현하지 않는다. 현재 시뮬레이터는 oracle 기반 dependency chain을 사용한다. |
| cross-chain dependency | chain별 독립 snapshot 구조이므로 서로 다른 H2P chain 사이의 TEA-produced register value는 공유하지 않는다. |
| PREG 분할 | `TEA_PREG_RESERVATION`은 `TEA_MAX_CHAINS`로 나누어 떨어져야 한다. |
| Preg pool 정책 | 논문처럼 global reference counter 기반 shared preg reclaim은 아니다. |

현재 구현은 correctness와 per-chain cleanup 안전성을 우선한 모델이다. 논문과 같은 shared TEA preg reference counter 모델은 아니지만, multi-H2P chain을 동시에 실행하면서 stale dependency와 preg leak을 피하는 방향으로 정리되어 있다.
