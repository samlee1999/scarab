# TEA Shadow FTQ / Fetch 계획

**최종 갱신**: 2026-04-27
**상태**: Shadow FTQ 미구현, Dependency Chain Cache 직접 조회 방식 유지

---

## 1. 결정 사항

논문의 Shadow FTQ는 현재 구현하지 않는다. 현재 Scarab TEA는 dependency chain cache를 H2P PC로 직접 조회하는 방안 A를 사용한다. Work F multi-H2P도 이 방안 위에 구현되어 있다.

이 결정은 코드 복잡도를 낮추고, BW Walk가 만든 dependency chain을 바로 TEA Fetch가 사용할 수 있게 하기 위한 것이다.

---

## 2. 현재 fetch 구조

- `Tea_Fetch_Stage.active_chain`: 현재 fetch 중인 dependency chain cache entry.
- `current_chain_idx`: chain 내부 op index.
- `current_chain_id`: `Tea_Thread.chains[]` slot index.
- `fetch_complete`: 현재 chain fetch 완료 여부.
- `Tea_Thread.current_fetch_chain`: 다음에 fetch할 chain slot.

`update_tea_fetch_stage()`는 현재 chain을 `TEA_FETCH_WIDTH`씩 fetch한다. Fetch가 끝나면 chain을 `CHAIN_EXECUTING`으로 만들고, 다음 `CHAIN_FETCHING` chain을 찾아 setup한다.

---

## 3. Multi-H2P와의 관계

Fetch는 sequential이다. 한 cycle에 여러 chain을 동시에 fetch하지 않는다. 그러나 Chain A fetch가 끝난 뒤 Chain A op들은 backend에서 실행 중이고, Fetch stage는 Chain B를 가져올 수 있으므로 execution overlap은 가능하다.

TEA op 생성 시 `h2p_chain_id = current_chain_id + 1`을 설정한다. 이 id가 이후 early flush, selective flush, store buffer clear의 기준이 된다.

---

## 4. Shadow FTQ를 다시 고려할 조건

다음 결과가 시뮬레이션으로 확인되기 전에는 Shadow FTQ 구현을 진행하지 않는다.

- Fetch width 또는 sequential fetch가 주요 병목으로 보인다.
- Dep Chain Cache 직접 조회가 control-flow path coverage를 크게 잃는다.
- Hybrid Chain 적용 후에도 chain miss/staleness가 성능을 제한한다.

그 전까지는 Hybrid Chain과 Work F 안정화가 우선이다.
