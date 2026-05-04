# TEA Shadow FTQ / Fetch 현재 구현 상태

**최종 갱신**: 2026-05-04
**관련 계획**: `../TEA_implementation_plan/TEA_shadow_ftq_plan.md`

---

## 1. 요약

논문의 Shadow FTQ + Block Cache fetch stream은 현재 그대로 구현하지 않았다. 현재 Scarab TEA는 HBT가 H2P로 판정한 branch PC를 사용해 Dependency Chain Cache를 직접 조회하고, 저장된 전체 dependency chain을 TEA fetch stage가 sequential하게 가져오는 모델이다.

| 항목 | 현재 상태 |
|------|-----------|
| Shadow FTQ FIFO | 미구현 |
| BP fetch address stream 기반 stitch | 미구현 |
| Dependency Chain Cache 직접 조회 | 구현됨 |
| Dependency Chain Cache entry 수 | 1024 direct-mapped entry |
| Block Cache | 구현됨, fetch path에는 직접 미사용 |
| snapshot 내 모든 H2P backward walk | 구현됨 |
| multi-H2P sequential fetch | 구현됨 |
| `TEA_FETCH_WIDTH` parameter | 구현됨 |
| `BACKWARD_WALK_CYCLES` parameter | 구현됨 |

---

## 2. H2P 판정과 Fill Buffer

Main branch는 fetch/BP 시점에 `hbt_is_hard_branch(pc)` 결과를 `op->oracle_info.hbt_pred_is_hard`에 저장한다. Retire 시점에도 HBT counter를 다시 읽어 fill buffer에 기록되는 op snapshot에 반영한다.

HBT update/decay는 현재 다음 기준을 따른다.

- mispred/misfetch branch retire 시 `hbt_update(op)`로 counter 증가
- main instruction retire마다 `hbt_retire_instruction_tick()` 호출
- `HBT_DECAY_INTERVAL == 50000` retired instructions마다 모든 counter를 1 감소

즉 HBT decay는 논문 설명과 맞게 retired branch 수가 아니라 retired instruction tick 기준이다.

Fill Buffer는 retired main op snapshot을 저장한다. Buffer가 full이고 evict되는 op이 H2P이면 backward walk engine을 시작한다. 이때 evict되는 oldest H2P 자체는 snapshot에서 제외하고, 남아 있는 fill buffer window를 `BACKWARD_WALK_CYCLES` 이후 분석한다.

---

## 3. Backward Walk / Dependency Chain Cache

`add_dependency_chain()`은 이미 정렬된 snapshot을 입력으로 받는다. 현재 구현은 snapshot 안의 H2P를 하나만 고르는 모델이 아니다.

동작 순서:

1. snapshot op들에 대해 block start PC map 생성.
2. `oracle_info.hbt_pred_is_hard`가 true인 모든 H2P index 수집.
3. 각 H2P마다 독립 backward walk 수행.
4. 각 H2P PC에 대해 Dependency Chain Cache entry 저장.
5. 모든 H2P walk 결과의 union mask를 Block Cache에 OR 누적.

Dependency Chain Cache는 direct-mapped 구조다.

- size: `DEPENDENCY_CHAIN_CACHE_SIZE == 1024`
- index: `h2p_pc % DEPENDENCY_CHAIN_CACHE_SIZE`
- tag: `h2p_branch_pc`
- chain max length: `MAX_CHAIN_LENGTH == 64`

같은 index의 다른 PC가 들어오면 기존 entry는 replacement metadata 없이 덮어써진다. 같은 PC entry가 다시 들어오면 `DCC_CHAIN_OVERWRITE_SAME_PC`가 기록된다.

---

## 4. Block Cache의 현재 의미

Block Cache도 1024 entry direct-mapped 구조로 존재한다. `commit_block_cache_masks()`는 block 단위 dependency mask를 OR 누적하고, `DCC_BLOCK_MASK_OR_UPDATES`를 기록한다.

다만 현재 TEA fetch stage는 Block Cache를 직접 사용하지 않는다.

현재 fetch source:

```
setup_fetch_for_chain()
  -> get_dependency_chain(proc_id, target_h2p_pc)
  -> Dependency Chain Cache entry의 chain[]을 fetch
```

따라서 Block Cache의 OR 누적은 현재 코드에서 logging/향후 Hybrid Chain 작업을 위한 상태에 가깝다. 논문의 "fetch address stream을 따라 block segment를 stitch"하는 효과는 아직 TEA fetch timing에 직접 반영되지 않는다.

---

## 5. TEA Fetch Stage

`update_tea_fetch_stage()`는 `Tea_Thread.current_fetch_chain` 하나를 대상으로 동작한다.

1. active chain이 없으면 skip.
2. `current_fetch_chain < 0`이면 skip.
3. current chain이 `CHAIN_FETCHING`이 아니면 skip.
4. fetch stage에 active dep chain이 없으면 `setup_fetch_for_chain()` 수행.
5. 이전 chain fetch가 complete이면 chain을 `CHAIN_EXECUTING`으로 바꾸고 다음 fetching chain을 찾음.
6. rename stage가 이전 batch를 소비하지 않았으면 `TEA_FETCH_BACKPRESSURE_RENAME`.
7. 최대 `TEA_FETCH_WIDTH`개 TEA op 생성.

Fetch completion 시 `fetch_done_cycle`이 기록되어 chain lifetime breakdown에서 trigger-to-fetch-done, fetch-done-to-terminate 구간을 볼 수 있다.

---

## 6. 논문과의 차이

| 항목 | 논문 | 현재 구현 |
|------|------|-----------|
| TEA frontend source | Shadow FTQ + Block Cache | H2P PC -> Dependency Chain Cache 직접 조회 |
| Fetch stream | BP가 생성한 dynamic fetch address stream 활용 | H2P branch trigger 시 전체 chain fetch |
| Chain 조합 | fetch 시점에 block segment stitch | backward walk 시점에 full chain materialize |
| Block Cache OR 효과 | TEA fetch coverage에 직접 반영 | 현재 fetch path에는 직접 미반영 |
| Multi-H2P BW walk | fill buffer 내 여러 H2P 처리 | 현재 snapshot 내 모든 H2P 처리 |
| Fetch parallelism | TEA fetch width 중심 | 한 번에 한 chain stream, width는 `TEA_FETCH_WIDTH` |

현재 방식은 논문의 frontend를 완전히 재현하기보다는, H2P dependency chain을 미리 만들어 TEA thread가 backend에서 H2P를 먼저 execute하도록 하는 기능적 모델이다.

---

## 7. 현재 관찰해야 할 stat

| 목적 | Stat |
|------|------|
| snapshot H2P coverage | `DCC_SNAPSHOT_H2P_BRANCHES_TOTAL`, `DCC_SNAPSHOT_H2P_BRANCHES_*` |
| chain insert | `DCC_CHAINS_INSERTED`, `DCC_CHAIN_OVERWRITE_SAME_PC` |
| Block Cache OR | `DCC_BLOCK_MASK_OR_UPDATES` |
| fetch chain hit/miss | `TEA_FETCH_CHAIN_HIT`, `TEA_FETCH_CHAIN_MISS` |
| fetch progress | `TEA_FETCH_LOOP_ENTERED`, `TEA_FETCH_CHAIN_COMPLETED`, `TEA_FETCH_SWITCH_NEXT_CHAIN`, `TEA_FETCH_NO_NEXT_CHAIN` |
| frontend backpressure | `TEA_FETCH_BACKPRESSURE_RENAME` |

---

## 8. 남은 제한

| 제한 | 설명 |
|------|------|
| No Shadow FTQ | Main BP fetch address stream을 TEA frontend가 직접 소비하지 않는다. |
| Direct-mapped DCC | associativity/LRU가 없고 index collision은 overwrite로 처리된다. |
| Evicted H2P 제외 | fill buffer overflow를 유발한 oldest H2P 자체는 backward-walk snapshot에서 제외된다. |
| Block Cache 미사용 | OR 누적 mask는 현재 TEA fetch source가 아니다. |
| Sequential chain fetch | 여러 chain slot이 있어도 TEA fetch stream은 하나다. |

Hybrid Chain / Block Cache 기반 fetch를 구현하기 전까지는 `TEA_TRIGGER_SKIP_NO_CHAIN`, `DCC_*`, `TEA_FETCH_CHAIN_MISS`가 TEA frontend coverage 한계를 판단하는 핵심 지표다.
