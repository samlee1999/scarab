# TEA Shadow FTQ / Fetch 현재 구현 상태

**최종 갱신**: 2026-04-27
**관련 계획**: `../TEA_implementation_plan/TEA_shadow_ftq_plan.md`

---

## 1. 요약

논문의 128-entry Shadow FTQ는 현재 구현하지 않는다. Scarab TEA는 Dependency Chain Cache를 H2P PC로 직접 조회하는 방안 A를 채택한다. Work F 이후 이 방안 A 위에 multi-H2P sequential fetch가 구현되어 있다.

| 항목 | 현재 상태 |
|------|-----------|
| Shadow FTQ FIFO | 미구현, 현재 계획 없음 |
| Dep Chain Cache 직접 조회 | 구현됨 |
| single active chain fetch | 과거 모델 |
| multi-H2P sequential fetch | 구현됨 |
| `current_chain_id` | 구현됨 |
| `recover_tea_fetch_stage_by_chain()` | 구현됨 |
| Hybrid Chain / Block Cache 재조합 | 아직 미구현 |

---

## 2. 현재 fetch 모델

`trigger_tea_thread()`는 dependency chain cache hit 여부를 확인하고 chain slot을 `CHAIN_FETCHING`으로 만든다. `Tea_Thread.current_fetch_chain`이 현재 fetch 대상 slot을 가리킨다.

`update_tea_fetch_stage()`는 다음 순서로 동작한다.

1. active chain이 없으면 `current_fetch_chain`의 H2P PC로 dependency chain cache를 조회한다.
2. `TEA_FETCH_WIDTH`만큼 chain op을 fetch stage output buffer에 넣는다.
3. 생성된 TEA op에는 `h2p_chain_id = current_chain_id + 1`을 부여한다.
4. 현재 chain fetch가 끝나면 chain state를 `CHAIN_EXECUTING`으로 전환한다.
5. 다음 `CHAIN_FETCHING` chain을 찾아 fetch stage를 setup한다.

Fetch는 한 번에 하나의 chain만 처리하지만, 이미 fetch 완료된 chain의 op은 backend에서 계속 실행되므로 execution은 overlap된다.

---

## 3. 논문과의 차이

| 항목 | 논문 | 현재 구현 |
|------|------|-----------|
| Fetch source | Shadow FTQ + Block Cache | Dependency Chain Cache |
| Chain 조합 | fetch 시점 basic-block segment 조합 | BW Walk 시점에 전체 chain 저장 |
| Fetch width | 8-wide | `TEA_FETCH_WIDTH`, 현재 기본 2 |
| Multi-H2P | 여러 H2P chain 처리 | 최대 4 slot, sequential fetch |
| Block Cache 활용 | TEA Fetch 직접 사용 | 현재 fetch에는 직접 사용 안 함 |

현재 방식은 논문 구조를 완전히 재현하지는 않지만, H2P dependency chain을 미리 구성해서 TEA thread가 빠르게 실행한다는 기능적 목표를 우선 구현한다.

---

## 4. 제한과 다음 작업

- Dependency Chain Cache는 현재 BW Walk 결과를 덮어쓰는 경향이 있어 control-flow multi-path 정보를 충분히 보존하지 못할 수 있다.
- Hybrid Chain 작업에서 Block Cache의 OR 누적 bitmask를 사용해 Dep Chain Cache entry를 재구성할 계획이다.
- Shadow FTQ 자체는 현재 우선순위가 낮다. 필요성이 시뮬레이션 결과로 확인되기 전에는 방안 A를 유지한다.
