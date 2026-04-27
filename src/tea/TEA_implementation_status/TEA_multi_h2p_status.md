# TEA Multi-H2P 현재 구현 상태

**최종 갱신**: 2026-04-27
**관련 계획**: `../TEA_implementation_plan/TEA_multi_h2p_plan.md`

---

## 1. 요약

Work F multi-H2P는 1차 구현이 들어간 상태다. 이전 단일 H2P 문서에 있던 "`state != TEA_IDLE`이면 trigger skip" 모델은 현재 코드의 주 동작이 아니다.

| 항목 | 현재 상태 |
|------|-----------|
| `Op.h2p_chain_id` | 구현됨 |
| `Tea_H2P_Chain` | 구현됨 |
| `Tea_Thread.chains[MAX_TEA_CHAINS]` | 구현됨 |
| 빈 slot 기반 trigger | 구현됨 |
| sequential fetch + overlapped execute | 구현됨 |
| per-chain early flush | 구현됨 |
| per-chain terminate | 구현됨 |
| selective main recovery handling | 구현됨 |
| Work F 이후 시뮬레이션 검증 | 아직 문서화 안 됨 |

---

## 2. 실제 코드 구조

### Chain state

`tea_thread.h`는 `CHAIN_INACTIVE`, `CHAIN_FETCHING`, `CHAIN_EXECUTING`을 갖는 `Tea_H2P_Chain`을 정의한다. 각 chain은 Main H2P PC/op_num, Main H2P 포인터, `saved_unique_num`, oracle/recovery info, per-chain `tea_op_count`, `tea_ops_fetched`를 가진다.

### Trigger

`trigger_tea_thread()`는 `MAX_TEA_CHAINS` 안에서 빈 slot을 찾는다. 빈 slot이 없으면 `TEA_TRIGGER_SKIP_FULL`, dependency chain miss이면 `TEA_TRIGGER_SKIP_NO_CHAIN`을 기록한다. 첫 active chain에서만 Shadow RAT snapshot과 TEA op counter reset을 수행한다.

### Fetch

`tea_fetch_stage.c`는 `current_chain_id`를 갖고, `Tea_Thread.current_fetch_chain`이 가리키는 chain을 fetch한다. 한 chain fetch가 끝나면 해당 chain을 `CHAIN_EXECUTING`으로 전환하고 다음 `CHAIN_FETCHING` chain을 찾는다.

### Execute / recovery

TEA H2P branch가 execute되면 `op->h2p_chain_id`로 chain slot을 직접 찾는다. Main H2P 포인터는 `op_pool_valid`와 `saved_unique_num`으로 검증한다.

- Case 1: SRT checkpoint 없음. 해당 chain만 `terminate_tea_chain()`으로 종료한다.
- Case 2: SRT checkpoint 있음. Main H2P 기준 recovery를 schedule하고, 실제 `cmp_recover()`에서 recovery point 이상의 chain을 종료한다.
- Correct H2P: 해당 chain만 정상 종료한다.

---

## 3. 아직 검증/수정이 필요한 항목

| 항목 | 상태 | 이유 |
|------|------|------|
| `h2p_chain_id` 재사용 안전성 | 점검 필요 | `op_pool_setup_op()` reset이 보이지 않음 |
| selective flush의 `thread_id` guard | 점검 필요 | 현재 일부 필터가 `h2p_chain_id`만 비교 |
| surviving chain dependency cleanup | 미구현 가능성 | Main recovery로 producer가 flush된 경우 not-ready bit 정리가 필요 |
| `TEA_CHAINS_CONCURRENT_MAX` | instrumentation 미완성 | stat은 정의되어 있으나 emission이 placeholder |
| `TEA_MAX_CHAINS` param | runtime 반영 미완성 | loops는 compile-time `MAX_TEA_CHAINS` 사용 |

---

## 4. 다음 시뮬레이션에서 볼 지표

- `TEA_TRIGGER_ATTEMPTS`
- `TEA_TRIGGER_SKIP_NO_CHAIN`
- `TEA_TRIGGER_SKIP_FULL`
- `TEA_TRIGGERS`
- `TEA_CHAIN_TERMINATED`
- `TEA_EARLY_FLUSHES`
- `TEA_H2P_CORRECT`
- `TEA_OPS_FETCHED / DISPATCHED / RETIRED`
- `TEA_RENAME_STALL_DISPATCH`
- `TEA_RS_STALLS`
- IPC: TEA off/on 비교

`TEA_TRIGGER_SKIP_ACTIVE`는 legacy stat으로 남아 있으나 현재 Work F trigger path에서는 증가하지 않는 것이 기대값이다.
