# TEA Multi-H2P 현재 구현 상태

**최종 갱신**: 2026-05-04
**관련 계획**: `../TEA_implementation_plan/TEA_multi_h2p_plan.md`

---

## 1. 요약

Work F multi-H2P는 현재 코드의 기본 TEA 실행 모델이다. 이전 단일 H2P 모델의 "`state != TEA_IDLE`이면 trigger skip" 구조는 legacy path로 남아 있을 뿐이고, 실제 trigger는 빈 chain slot을 찾아 여러 H2P chain을 동시에 살릴 수 있다.

| 항목 | 현재 상태 |
|------|-----------|
| `Op.h2p_chain_id` | 구현됨, `0`은 main op, `1..TEA_MAX_CHAINS`는 TEA chain |
| `Tea_H2P_Chain` | 구현됨, H2P 정보와 per-chain op count/lifetime 보관 |
| `Tea_Thread.chains[MAX_TEA_CHAINS]` | 구현됨, compile-time capacity는 16 |
| `TEA_MAX_CHAINS` runtime limit | 구현됨, `TEA_MAX_CHAINS <= MAX_TEA_CHAINS` ASSERT |
| 빈 slot 기반 trigger | 구현됨 |
| sequential TEA fetch | 구현됨, 한 cycle에는 한 chain fetch stream만 진행 |
| overlapped backend execution | 구현됨, fetch 완료 chain들은 shared backend에서 같이 실행 |
| per-chain Shadow RAT/PREG pool | 구현됨 |
| per-chain early flush/terminate | 구현됨 |
| Main recovery 시 selective chain flush | 구현됨 |
| full slot replacement policy | 미구현, full이면 trigger skip |

---

## 2. 실제 코드 구조

### Chain state

`tea_thread.h`는 `CHAIN_INACTIVE`, `CHAIN_FETCHING`, `CHAIN_EXECUTING`을 갖는 `Tea_H2P_Chain`을 정의한다. 각 chain은 다음 정보를 가진다.

- `target_h2p_pc`, `target_h2p_op_num`
- `main_h2p_op`, `saved_unique_num`
- saved `h2p_oracle_info`, saved `h2p_recovery_info`
- `tea_op_count`, `tea_ops_fetched`
- `trigger_cycle`, `fetch_done_cycle`

`main_h2p_op` 포인터는 TEA H2P가 execute될 때 `op_pool_valid`와 `saved_unique_num`으로 다시 검증한다. 따라서 op pool 재사용으로 인한 stale pointer를 직접 early flush에 쓰지 않는다.

### Runtime chain 수

`MAX_TEA_CHAINS`는 16으로 잡힌 compile-time storage capacity다. 실제 실험에서 사용하는 chain 수는 `TEA_MAX_CHAINS` parameter가 정하며, `tea_max_chains()`는 다음 조건을 ASSERT한다.

- `TEA_MAX_CHAINS > 0`
- `TEA_MAX_CHAINS <= MAX_TEA_CHAINS`

즉 4/8/12/16 chain 실험은 같은 바이너리 안에서 parameter로 조절할 수 있다. 단, 16을 넘기는 구조는 현재 header capacity 확장이 필요하다.

### Trigger

`bp.c`는 fetch/BP 경로에서 `hbt_is_hard_branch(pc)`로 현재 branch가 H2P인지 판정하고, prediction evaluation 이후 `trigger_tea_thread(proc_id, pc, op_num, op)`를 호출한다. Trigger는 다음 순서로 동작한다.

1. `TEA_TRIGGER_ATTEMPTS` 기록.
2. 현재 active/fetching/executing chain pressure stat 기록.
3. `TEA_MAX_CHAINS` 범위에서 `CHAIN_INACTIVE` slot 탐색.
4. slot이 없으면 `TEA_TRIGGER_SKIP_FULL`.
5. Dependency Chain Cache를 H2P PC로 조회.
6. miss 또는 length 0이면 `TEA_TRIGGER_SKIP_NO_CHAIN`.
7. chain slot을 `CHAIN_FETCHING`으로 채우고 `num_active_chains++`.
8. 해당 slot의 Shadow RAT을 Main RAT에서 snapshot.
9. fetch 중인 chain이 없으면 이 slot을 `current_fetch_chain`으로 지정.

첫 active chain에서만 TEA op counter와 coarse TEA state를 초기화한다. 이후 추가 chain은 독립 slot으로 들어오며 기존 active chain을 강제로 종료하지 않는다.

### Fetch / execution

`update_tea_fetch_stage()`는 `Tea_Thread.current_fetch_chain`이 가리키는 chain 하나를 fetch한다. 한 chain의 dependency chain fetch가 끝나면 해당 chain은 `CHAIN_EXECUTING`으로 전환되고, 다음 `CHAIN_FETCHING` slot을 찾아 fetch stream을 넘긴다.

중요한 의미:

- `TEA_MAX_CHAINS`는 N개의 완전한 in-order frontend queue가 아니라, 동시에 살아 있을 수 있는 H2P chain context 수다.
- TEA frontend는 sequential fetch 구조다.
- fetch/rename이 끝난 TEA ops는 Node/RS/Exec/Dcache를 공유하므로 backend에서는 여러 chain의 op이 out-of-order로 섞여 실행될 수 있다.

---

## 3. 자원 모델

| 자원 | 현재 모델 |
|------|-----------|
| Shadow RAT | chain slot별 독립 snapshot |
| TEA preg | `TEA_PREG_RESERVATION / TEA_MAX_CHAINS`로 per-chain sub-pool 생성 |
| RS | Main/TEA partition counter 사용, `TEA_RS_RESERVATION` 기반 |
| Store buffer | TEA 전용 buffer, entry에 `h2p_chain_id` 태깅 |
| Node/Exec/Dcache | Main/TEA가 공유 |
| Recovery checkpoint | Main SRT checkpoint는 기존 Scarab mechanism 사용 |

Per-chain preg pool은 chain 종료 시 `reset_tea_preg_pool(proc_id, slot)`으로 독립 reset된다. 또한 TEA op retire 시 이전 mapping preg를 해당 chain pool로 반환하는 경로가 있다.

---

## 4. 종료 경로

### Per-chain 종료

`terminate_tea_chain_with_reason(proc_id, chain_slot, reason)`은 다음을 수행한다.

1. fetch stage SD에 남은 해당 chain op free.
2. fetch 중이던 chain이면 다음 `CHAIN_FETCHING` chain으로 전환.
3. rename stage SD에서 해당 chain op 제거.
4. exec/dcache SD, ready list, scheduling buffer, node table에서 해당 chain op 제거.
5. 해당 chain의 TEA store buffer entry 제거.
6. chain termination reason/lifetime/fetch wait/exec wait stat 기록.
7. chain state/counter reset.
8. 해당 chain preg pool reset.
9. 마지막 active chain이면 TEA store buffer와 rename stage 전체 reset.

### Full 종료

`terminate_tea_thread()`는 모든 runtime chain slot을 inactive로 만들고, TEA fetch/rename/node/store/preg/pending Case 1 state를 모두 flush한다. 일반적인 H2P 결과 처리는 per-chain 종료가 담당하고, full 종료는 전체 TEA cleanup이 필요한 경우에 남아 있다.

---

## 5. 현재 관찰해야 할 stat

| 목적 | Stat |
|------|------|
| trigger volume | `TEA_TRIGGER_ATTEMPTS`, `TEA_TRIGGERS` |
| chain slot 병목 | `TEA_TRIGGER_SKIP_FULL`, `TEA_TRIGGER_ACTIVE_CHAINS_*`, `TEA_TRIGGER_FREE_SLOTS_TOTAL` |
| dep chain coverage | `TEA_TRIGGER_SKIP_NO_CHAIN`, `TEA_CHAIN_LENGTH_*` |
| active chain occupancy | `TEA_ACTIVE_CYCLES`, `TEA_ACTIVE_CHAIN_SLOTS_TOTAL`, `TEA_FETCHING_CHAIN_SLOTS_TOTAL`, `TEA_EXECUTING_CHAIN_SLOTS_TOTAL` |
| max concurrent chains | `TEA_CHAINS_CONCURRENT_MAX` |
| 종료 이유 | `TEA_CHAIN_TERM_*`, `TEA_CHAIN_TERMINATED` |
| chain lifetime | `TEA_CHAIN_LIFETIME_*`, `TEA_CHAIN_FETCH_WAIT_TOTAL`, `TEA_CHAIN_EXEC_WAIT_TOTAL` |

`TEA_TRIGGER_SKIP_ACTIVE`는 single-H2P legacy stat이다. 현재 multi-H2P trigger path에서는 증가하지 않는 것이 기대값이다.

---

## 6. 현재 제한

| 제한 | 설명 |
|------|------|
| full slot 처리 | 모든 slot이 active이면 replacement 없이 `TEA_TRIGGER_SKIP_FULL`로 버린다. |
| frontend 병렬성 | chain context는 여러 개지만 TEA fetch stream은 하나다. |
| slot 수 상한 | runtime 최대는 compile-time `MAX_TEA_CHAINS=16`에 묶여 있다. |
| dep chain miss | H2P라고 판정되어도 Dependency Chain Cache miss이면 TEA 실행은 시작하지 않는다. |
| backend 공유 | Node/RS/Exec/Dcache는 Main과 TEA가 공유하므로 TEA benefit이 backend pressure로 상쇄될 수 있다. |

현재 구조는 논문의 multi-H2P 개념을 Scarab에 맞게 구현한 모델이다. 완전한 "unlimited chain stream"이 아니라, 제한된 chain context와 shared backend 위에서 trigger/full/rename/RS/PREG 병목을 관찰하는 구조다.
