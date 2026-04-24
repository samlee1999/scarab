# Multi-H2P Implementation Review

> 검토 대상: `tea_thread.h/c`, `tea_fetch_stage.c`, `tea_rename.c`, `tea_store_buffer.h/c`,
>             `node_stage.c`, `exec_stage.c`, `cmp_model.c`, `tea.stat.def`

---

## ⚠️ 주의 사항 — 동작은 하지만 개선 필요

### ⚠️-1 `terminate_tea_thread()`: INACTIVE 먼저 → flush 순서
**위치**: `tea_thread.c:274-281`

`flush_tea_ops_from_node_stage()`는 `thread_id==1` 조건이므로 chain state에 무관하게 동작. 그러나 `terminate_tea_chain()`은 flush → INACTIVE 순서로 진행해 **두 함수 간 순서 철학이 역전**되어 있음.

> **권고**: `terminate_tea_thread()`도 flush 후 INACTIVE 표시하는 방식으로 통일.

---

### ⚠️-2 `recover_tea_on_flush()`: `node` 전역 포인터에 직접 의존
**위치**: `cmp_model.c:425`

```c
Node_Stage* node_local = node;  /* global set by cmp_set_all_stages() */
```

`flush_tea_ops_by_chain_id()`는 `&cmp_model.node_stage[proc_id]`를 사용하는데 여기만 전역 포인터를 사용해 일관성이 없음.

> **권고**: `&cmp_model.node_stage[proc_id]`로 통일.

---

### ⚠️-3 `TEA_TRIGGER_SKIP_ACTIVE` — Orphan stat
**위치**: `tea.stat.def:28`

주석에 "single-H2P legacy"라고 명시되어 있으며 `trigger_tea_thread()` 내부에 increment 사이트가 없음. 다중 H2P 전환 후 해당 조건 자체가 사라짐.

> **권고**: 제거.

---

### ⚠️-4 `Tea_Store_Buffer_Entry.write_cycle` — 죽은 필드
**위치**: `tea_store_buffer.h:52`

`tea_store_buffer.c:117,131`에서 write-only, 읽기 사이트 없음. 이전 검증 보고서에서도 지적했으나 아직 잔존.

> **권고**: 제거하여 구조체 크기 최소화.

---

### ⚠️-5 `setup_fetch_for_chain()` 종료 시 `current_chain_id = -1` 중복 설정
**위치**: `tea_fetch_stage.c:148`

`terminate_tea_chain()` 내부에서 이미 `tea->current_fetch_chain = -1`을 설정(`tea_thread.c:213`)하는데, 호출 직후 `tf->current_chain_id = -1`을 한번 더 설정. 무해하지만 중복.

> **권고**: 주석으로 명확히 설명하거나 제거.

---

## ❌ 버그 / 결함 — 수정 필요

### ❌-1 `recover_tea_rename_stage_by_chain()`: Shadow RAT 매핑 미복구 [심각]
**위치**: `tea_rename.c:607-628`

```c
void recover_tea_rename_stage_by_chain(uns proc_id, uns8 chain_id) {
  // sd.ops[]에서 해당 chain_id op만 free_op() ← OK
  // Shadow RAT의 gp_mappings[], producer_ops[] 미복구 ← ❌
}
```

**문제**: Chain A 종료 시 Chain A의 ops가 작성한 Shadow RAT 매핑(`gp_mappings[R5] = Preg_200`)이 그대로 남음. Chain B가 R5를 읽으면 이미 반환된 Preg_200에서 데이터를 읽어 **잘못된 precomputation** 발생.

**영향 범위**: `num_active_chains >= 2`인 경우만 해당. 단일 chain은 종료 시 전체 초기화되므로 문제 없음.

---

### ❌-2 `find_next_fetching_chain()`: 음수 modulo — implementation-defined
**위치**: `tea_fetch_stage.c:121-132`

```c
int start = tea->current_fetch_chain;  // -1이 될 수 있음
int next = (start + i) % (int)TEA_MAX_CHAINS;
// start=-1, i=1 → 0 % 4 = 0 ← 현재는 우연히 올바름
// C 표준에서 음수 modulo 결과는 implementation-defined
```

> **수정 권고**:
> ```c
> int begin = (tea->current_fetch_chain >= 0)
>               ? (tea->current_fetch_chain + 1) % (int)TEA_MAX_CHAINS
>               : 0;
> for (int i = 0; i < (int)TEA_MAX_CHAINS; i++) {
>   int next = (begin + i) % (int)TEA_MAX_CHAINS;
>   ...
> }
> ```

---

## 요약

| | 건수 | 핵심 |
|--|------|------|
| ⚠️ 주의 | 5건 | 동작은 하지만 개선 권고 |
| ❌ 버그 | 2건 | ❌-1 Shadow RAT 롤백 미구현 (심각), ❌-2 modulo 불명확 (낮음) |
