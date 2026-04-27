# TEA Early Flush 계획

**최종 갱신**: 2026-04-27
**상태**: multi-H2P Work F 1차 구현 반영

---

## 1. 목적

TEA Early Flush는 TEA thread가 Main H2P branch보다 먼저 branch outcome을 계산했을 때, 기존 main recovery mechanism을 더 빨리 작동시키는 기능이다. 현재 구현은 H2P chain별로 독립적으로 동작한다.

---

## 2. 현재 알고리즘

TEA branch execute 시:

1. `op->thread_id == 1`이면 TEA branch path로 진입한다.
2. `op->h2p_chain_id - 1`로 chain slot을 찾는다.
3. chain이 active이고 op PC가 `target_h2p_pc`와 같을 때만 H2P branch로 처리한다.
4. Main H2P pointer를 `op_pool_valid`와 `saved_unique_num`으로 검증한다.
5. mispred/misfetch 여부와 SRT checkpoint 존재 여부로 Case 1/2를 선택한다.

---

## 3. Case 정책

### Case 1: no SRT checkpoint

Main H2P가 아직 SRT checkpoint를 만들지 못했다. 현재 정책은 Main H2P flag를 새로 바꾸지 않고 해당 TEA chain만 종료하는 것이다.

- decode 통과: `TEA_EARLY_FLUSH_CASE1_NO_CHKPT`
- decode 전: `TEA_EARLY_FLUSH_CASE1_DECODE`
- action: `terminate_tea_chain(proc_id, chain_slot)`

이 정책은 과거 `recover_at_decode` 기반 deadlock 회피를 반영한다.

### Case 2: SRT checkpoint present

Main H2P가 rename을 통과해 SRT checkpoint가 있다.

- `bp_sched_recovery()`를 Main H2P 기준으로 호출한다.
- Main H2P retire를 막기 위해 `recovery_scheduled`를 설정한다.
- double recovery 방지를 위해 `recover_at_exec = FALSE`로 둔다.
- 다음 `cmp_recover()`에서 `recover_tea_on_flush(proc_id, recovery_op_num)`이 recovery point 이상의 chain을 종료한다.

### Correct TEA H2P

원래 BP 예측이 맞았으면 `TEA_H2P_CORRECT`를 기록하고 해당 chain을 종료한다.

---

## 4. Recovery 연동

`recover_tea_on_flush(proc_id, recovery_op_num)`은 active chain을 순회한다. `target_h2p_op_num >= recovery_op_num`인 chain은 Main recovery에 의해 의미가 없어졌거나 off-path가 될 수 있으므로 종료한다. Older chain은 살아남을 수 있다.

Case 1에서는 이 함수를 직접 호출하지 않는다. 해당 chain만 종료하고 Main H2P의 기존 recovery path가 나중에 동작하도록 둔다.

---

## 5. 남은 점검 항목

- Surviving older chain이 flushed main producer를 기다리는 경우 not-ready bit cleanup이 필요하다.
- `main_h2p_op` pointer가 invalid일 때 terminate만 하고 stat을 어떻게 기록할지 정리할 수 있다.
- Case breakdown stat과 `TEA_EARLY_FLUSHES`가 중복/누락 없이 맞는지 확인한다.
- `EXTRA_LATE_RECOVERY_CYCLES`가 0이 아닐 때 Main H2P double recovery가 없는지 확인한다.
