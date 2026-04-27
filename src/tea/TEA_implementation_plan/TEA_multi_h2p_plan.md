# TEA Multi-H2P Work F 계획 및 현재 구현

**최종 갱신**: 2026-04-27
**상태**: Work F 1차 구현 완료, 시뮬레이션 기반 디버깅 전

---

## 1. 목표

Work F의 목표는 단일 active H2P 제한을 제거하고, 최대 4개의 H2P dependency chain을 동시에 유지하는 것이다. Fetch는 한 번에 하나의 chain만 처리하지만, 이미 fetch된 chain은 backend에서 overlapped execution을 수행한다.

기대 효과는 Work F 이전 baseline에서 관찰된 `TEA_TRIGGER_SKIP_ACTIVE` 96.2% 병목을 줄이고, 더 많은 H2P에 대해 TEA early flush 기회를 만드는 것이다.

---

## 2. 현재 구현된 구조

### Data model

- `Op.h2p_chain_id`: TEA op이 어느 H2P chain에 속하는지 표시한다. `0`은 main op, `1..MAX_TEA_CHAINS`는 chain slot+1.
- `Tea_H2P_Chain`: per-chain state와 H2P metadata를 보관한다.
- `Tea_Thread.chains[MAX_TEA_CHAINS]`: active/inactive chain slot 배열이다.
- `Tea_Thread.num_active_chains`: active chain 수다.
- `Tea_Thread.current_fetch_chain`: 현재 fetch 중인 chain slot이다.

### Trigger

`trigger_tea_thread()`는 빈 chain slot을 찾고, dependency chain cache hit이면 해당 slot을 `CHAIN_FETCHING`으로 만든다. 첫 active chain에서만 Shadow RAT snapshot과 TEA op counter reset을 수행한다. 모든 slot이 사용 중이면 `TEA_TRIGGER_SKIP_FULL`을 기록한다.

### Fetch

`update_tea_fetch_stage()`는 `current_fetch_chain`을 기준으로 dependency chain cache를 직접 조회한다. Fetch 완료된 chain은 `CHAIN_EXECUTING`으로 전환하고, 다음 `CHAIN_FETCHING` chain을 찾아 이어서 fetch한다.

### Execution and termination

TEA H2P branch execute 시 `h2p_chain_id`로 chain을 직접 찾는다.

- Mispred + SRT checkpoint 있음: Main H2P 기준 recovery를 schedule.
- Mispred + SRT checkpoint 없음: 해당 chain만 즉시 terminate.
- Correct: 해당 chain 정상 terminate.
- Main recovery: `recover_tea_on_flush(proc_id, recovery_op_num)`가 recovery point 이상의 chain만 terminate.

### Shared resources

- Shadow RAT: 모든 chain이 공유.
- TEA preg pool: 모든 chain이 공유.
- Store buffer: entry에 `h2p_chain_id`를 저장하고 chain별 clear 가능.
- TEA op counter: 모든 chain이 공유하는 전역 counter.

---

## 3. 구현된 phase 매핑

| Phase | 현재 상태 | 핵심 구현 |
|-------|-----------|-----------|
| F.1 data structure | 구현됨 | `h2p_chain_id`, `Tea_H2P_Chain`, `chains[]` |
| F.2 trigger/terminate | 구현됨 | 빈 slot trigger, `terminate_tea_chain()` |
| F.3 fetch switching | 구현됨 | `current_fetch_chain`, `current_chain_id` |
| F.4 per-chain early flush | 구현됨 | `exec_stage_bp_resolve()` chain lookup |
| F.5 selective flush | 구현됨 | fetch/rename/node/store chain clear |
| F.6 selective main recovery | 구현됨 | `recover_tea_on_flush(proc_id, recovery_op_num)` |

---

## 4. 현재 알려진 디버깅 후보

이 항목들은 소스 수정 전에 시뮬레이션과 ASSERT 로그로 우선순위를 확인한다.

| 우선순위 | 후보 | 설명 |
|----------|------|------|
| P0 | `h2p_chain_id` reset | op pool 재사용 시 stale chain id가 main op에 남을 수 있음 |
| P0 | selective flush filter | 모든 `h2p_chain_id` 비교에 `thread_id == 1` guard가 필요할 수 있음 |
| P1 | surviving chain dependency cleanup | Main recovery 후 older TEA chain이 flushed producer를 기다릴 수 있음 |
| P1 | concurrent-chain stat | `TEA_CHAINS_CONCURRENT_MAX` emission이 아직 placeholder |
| P1 | runtime max-chain param | `TEA_MAX_CHAINS` param과 `MAX_TEA_CHAINS` 사용이 분리됨 |

---

## 5. 검증 계획

### Build/run sanity

- 빌드가 통과해야 한다.
- 짧은 simpoint에서 ASSERT 없이 종료해야 한다.
- `TEA_OPS_FETCHED`, `TEA_OPS_DISPATCHED`, `TEA_OPS_RETIRED`가 비정상적으로 벌어지지 않아야 한다.

### Work F 효과 확인

- `TEA_TRIGGER_SKIP_ACTIVE`는 새 path에서 증가하지 않아야 한다.
- `TEA_TRIGGER_SKIP_FULL`은 slot 부족을 나타내야 한다.
- `TEA_CHAIN_TERMINATED > 0`이어야 한다.
- concurrent chain을 확인할 수 있도록 `TEA_CHAINS_CONCURRENT_MAX` 또는 대체 로그가 필요하다.
- `TEA_TRIGGERS`와 `TEA_EARLY_FLUSHES`가 Work F 이전보다 증가하는지 본다.

### 정확성 확인

- Case 1은 해당 chain만 종료해야 한다.
- Case 2는 Main H2P 기준 recovery를 schedule하고, `cmp_recover()`에서 recovery point 이상의 chain만 종료해야 한다.
- Correct TEA H2P는 해당 chain만 정상 종료해야 한다.
- Surviving chain이 RS에서 영구 not-ready로 남지 않아야 한다.

---

## 6. 다음 구현 후보

Work F 안정화 이후 진행할 작업:

1. P0 안정성 수정.
2. stat instrumentation 정리.
3. Hybrid Chain / periodic reset.
4. Iterative Walk.

Poison bit는 현재 구현하지 않는다.
