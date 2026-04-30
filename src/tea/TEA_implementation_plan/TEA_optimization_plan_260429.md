# TEA Optimization Implementation Plan (2026-04-29)

## 0. Scope

This plan covers the next correctness/fidelity pass for the current Scarab TEA implementation.
It intentionally does not change source code yet.  The implementation work should modify only the
items listed here after this plan is reviewed.

The item numbers below use the current discussion's numbering:

| Item | Target |
|---|---|
| 1 | TEA ready-list / RS counter cleanup for completed TEA ops |
| 2 | Stale dependency cleanup for surviving TEA chains after main recovery |
| 3 | Dynamic oracle copy for non-H2P TEA memory ops |
| 6 | Backward walk should process all H2P branches in one snapshot |
| 9 | HBT counter decay should use 50K retired instructions, not 50K retired branches |

Non-goals for this pass:

- Keep item 10 unchanged: current experiments already set `TEA_FETCH_WIDTH=8` through PARAMS, and
  we will not widen the main decode path in this pass.
- Do not convert TEA fetch to the full paper model using BP fetch-address streams plus Block Cache
  stitching.  The current simulator still triggers TEA at predicted H2P branches and fetches from
  `dependency_chain_cache`.
- Do not rework physical register management into the paper's valid-bit/reference-counter design.
  The current per-chain PREG partitioning and per-chain reset are retained.
- Do not replace current early flush with the paper's full timestamp / in-flight branch queue model.

## 1. Paper Intent and Current Simulator Shape

The TEA paper's key constraints for this pass are:

- TEA execution must be timely and accurate enough to reduce main-thread misprediction penalty.
- H2P identification decays every 50K retired instructions so stale H2P branches disappear after
  phase changes.
- Fill Buffer backward walk marks dependence chains for all H2P branches in the sampled window,
  including multiple dynamic instances.
- Memory dependencies are part of the chain construction and execution model; TEA loads access the
  data cache like speculative prefetch-like loads, and TEA stores use a private store buffer.
- TEA instructions do not retire in-order through the ROB, but backend bookkeeping still has to be
  exact: ready-list membership, RS counters, dependencies, and chain lifetime must stay consistent.

The current Scarab implementation differs in several pragmatic ways:

- `cmp_cores()` updates backend stages in this order: DCache, Exec, Node, Map, frontend stages, then
  TEA rename/fetch/update, then the backward-walk engine.
- TEA has a dedicated fetch/rename path, but fetch uses `get_dependency_chain(proc_id, h2p_pc)`
  from a direct dependency-chain cache rather than stitching Block Cache segments from BP fetch
  addresses.
- Multi-H2P is already present: `Tea_H2P_Chain`, `h2p_chain_id`, per-chain Shadow RATs, per-chain
  PREG pools, selective chain termination, pending Case 1 flush entries, and selective
  `recover_tea_on_flush(proc_id, recovery_op_num)`.
- `node_issue_queue_clear()` removes ready-list entries only when state is `OS_SCHEDULED` or
  `OS_MISS`.  TEA ops can become `OS_DONE` before this clear step sees them.
- `tea_create_op_from_cache()` currently copies full oracle/recovery state only for the H2P branch.
  Non-H2P TEA memory ops still use dynamic memory fields in `dcache_stage.c`.
- `add_dependency_chain()` currently chooses the youngest H2P in the snapshot and builds one chain.
- `hbt_update()` currently triggers decay every 50K retired branches even though the comment and
  the paper specify retired instructions.

## 2. Implementation Summary

| Phase | Item | Primary files | Why first/now |
|---|---|---|---|
| A | 1 | `node_issue_queue.cc`, `node_stage.c` | Prevent dangling ready-list entries and RS counter leaks before changing chain coverage |
| B | 2 | `cmp_model.c`, possibly `node_stage.c` helper | Keep surviving chains from waiting forever on flushed main producers |
| C | 3 | `tea/tea_fetch_stage.c` | Make TEA memory timing/store-buffer behavior use valid dynamic addresses and sizes |
| D | 9 | `bp/hbt.c`, `bp/hbt.h`, `node_stage.c` | Make H2P phase behavior match the paper with a small isolated change |
| E | 6 | `dependency_chain_cache.c`, `dependency_chain_cache.h`, logs/stats | Increase H2P coverage after correctness foundations are stable |

The preferred order is A -> B -> C -> D -> E.  Phase E can noticeably change TEA trigger pressure,
so it should come after the backend accounting and stale dependency fixes.

## 3. Phase A: TEA Ready-List / RS Counter Cleanup

### 3.1 Current Failure Path

Current stage order makes this path possible:

1. A TEA op is dispatched to an RS and inserted into `node->rdy_head`.
2. It is scheduled and later completed in `exec_stage` or `dcache_stage`.
3. Completion calls `tea_op_completed()`, setting `op->state = OS_DONE`.
4. In the next `update_node_stage()`, `node_issue_queue_clear()` scans the ready list but only
   removes `OS_SCHEDULED` and `OS_MISS`.
5. The completed TEA op remains in `node->rdy_head`, but `node_retire_tea_ops()` can free it from
   the node table.
6. Result: dangling ready-list pointer and leaked `rs_op_count` / `tea_op_count`.

This is especially likely for:

- 0-latency non-memory TEA ops, because `exec_stage_process_op()` marks them `OS_DONE` immediately.
- TEA memory ops, because `dcache_stage.c` calls `tea_op_completed()` before node clear sees them.

There is a second instance of the same accounting bug in TEA flush paths:

- `flush_tea_ops_from_node_stage()` step 1 removes every TEA op from `node_local->rdy_head`, but
  its ready-list counter fixup currently covers only `OS_SCHEDULED` / `OS_MISS`.
- `flush_tea_ops_by_chain_id()` has the same pattern for one chain's ready-list entries.
- If a TEA op has already become `OS_DONE` but is still in `rdy_head`, removing it during either
  full-thread flush or per-chain flush prevents `node_issue_queue_clear()` from ever seeing it.
  Without treating TEA `OS_DONE` as an already-issued ready-list entry, the RS counters leak even
  if the dangling ready-list pointer itself is removed by the flush.
- The node-table cleanup pass cannot safely compensate for this, because its whitelist intentionally
  handles pre-scheduling states such as `OS_IN_RS`, `OS_READY`, and `OS_WAIT_FWD`; adding `OS_DONE`
  there would risk double-decrementing ops that were already removed from the RS by clear/flush
  ready-list handling.

### 3.2 Planned Fix

Add one shared policy for "this ready-list entry has left the RS and must decrement RS counters":

- existing main/TEA states: `OS_SCHEDULED`, `OS_MISS`
- additional TEA-only state: `OS_DONE`

Planned code shape:

- Add a small helper near `node_issue_queue_clear()`:
  - `node_ready_op_should_clear_rs(Op* op)`
  - true for `OS_SCHEDULED || OS_MISS`
  - also true for `TEA_ENABLE && op->thread_id == 1 && op->state == OS_DONE`
- Add a helper for counter decrement to avoid duplicating underflow-sensitive code:
  - decrements `rs_op_count`
  - decrements `tea_op_count` for TEA ops, `main_op_count` for main ops
  - uses assertions in normal paths, guarded checks only in recovery/flush paths where the op may
    have already been partially cleaned up
- Update `node_issue_queue_clear()` to use this helper.
- Update `flush_tea_ops_from_node_stage()` and `flush_tea_ops_by_chain_id()` ready-list cleanup:
  - if a removed TEA op is `OS_SCHEDULED`, `OS_MISS`, or `OS_DONE`, decrement RS counters there
  - this must cover both full-thread termination and selective per-chain termination, because both
    functions can remove an `OS_DONE` TEA op from `rdy_head` before `node_issue_queue_clear()` gets
    another chance to process it
  - keep the existing node-table whitelist for pre-scheduling states
  - do not decrement for `OS_READY`/`OS_WAIT_FWD` in ready-list step, because those are still
    accounted for by the node-table pre-scheduling-state cleanup
  - do not add `OS_DONE` to the node-table whitelist; TEA `OS_DONE` belongs to the issued/done
    ready-list cleanup policy, not the pre-scheduling cleanup policy
- Add a defensive check in `node_retire_tea_ops()`:
  - normal expectation: `op->in_rdy_list == FALSE` before `free_op(op)`
  - if we choose a non-fatal recovery path, remove it from the ready list and count a diagnostic
    stat before freeing
  - if we choose a strict debug path, assert so the bug is caught immediately during testing

Recommended stats:

- `TEA_READY_LIST_DONE_CLEARED`: number of TEA `OS_DONE` entries cleared by
  `node_issue_queue_clear()`
- `TEA_FLUSH_READY_LIST_DONE_CLEARED`: number of TEA `OS_DONE` entries removed from `rdy_head` by
  `flush_tea_ops_from_node_stage()` or `flush_tea_ops_by_chain_id()`
- `TEA_RETIRE_READY_LIST_ESCAPE`: number of completed TEA ops that reached retire while still in
  ready list.  This should be zero after the fix.
- `TEA_RS_COUNTER_FIXUPS`: optional, for guarded flush-path counter corrections.

### 3.3 Validation

Minimum checks:

- No dangling ready-list assertion or RS counter divergence assertion.
- `sum(rs[i].tea_op_count)` should not monotonically grow when TEA chains naturally complete.
- `TEA_RETIRE_READY_LIST_ESCAPE == 0` after warmup in normal runs.
- `TEA_READY_LIST_DONE_CLEARED > 0` is expected for workloads with 0-latency TEA ops; this confirms
  the previously missed path is being handled.

## 4. Phase B: Stale Dependency Cleanup After Main Recovery

### 4.1 Current Failure Path

TEA rename snapshots producer `Op*` pointers from the main RAT:

- `shadow_rat_snapshot()` copies `map_data->reg_map[]` producer op pointers and unique numbers.
- `tea_rename_op()` turns those producers into `oracle_info.src_info[]` dependencies and calls
  `add_to_wake_up_lists()`.

During main recovery:

1. `cmp_recover()` runs normal main-thread recovery first.
2. `recover_node_stage()` frees flushed main ops.
3. `recover_exec_stage()` / `recover_dcache_stage()` clean backend stage entries.
4. Only after that, `recover_tea_on_flush(proc_id, recovery_op_num)` terminates TEA chains whose
   `target_h2p_op_num >= recovery_op_num`.
5. Chains older than the recovery point survive.

If a surviving TEA op is waiting on a main producer that was just flushed, the producer's wake-up
list is freed, but the TEA consumer's `srcs_not_rdy_vector` bit is not automatically cleared.  That
consumer can then wait forever.

### 4.2 Planned Fix

After the chain-termination loop in `recover_tea_on_flush()`, scan surviving TEA ops and clear only
dependencies that are proven stale because of this main recovery.

Add a helper:

```text
tea_clear_stale_main_producer_deps_after_recovery(proc_id, recovery_op_num)
```

The helper should scan two places:

- `tea_rename_stages[proc_id]->sd.ops[]`
  - TEA ops already renamed but not yet dispatched to node/RS can also carry stale src bits.
  - Clear bits only; no ready-list insertion is needed here.
- `cmp_model.node_stage[proc_id].node_head`
  - TEA ops in node table, RS, ready list, exec, or dcache still have node-table entries.
  - If a node-table TEA op becomes ready and is in `OS_IN_RS`, add it to `node->rdy_head`.

Stale dependency predicate:

- source bit is currently set in `op->srcs_not_rdy_vector`
- source is a main-thread op, identified by saved `src_info.op_num` being in the main op-number
  namespace, not the TEA high-bit namespace
- `src_info.op_num >= recovery_op_num`
- producer pointer is invalid, null, or has a mismatched `unique_num`

The `src_info.op_num >= recovery_op_num` guard is important.  It limits the cleanup to producers
that this recovery could have flushed and avoids accidentally hiding a real latency dependency on an
older still-valid main producer.

Ready-list insertion rule for node-table ops:

- after clearing bits, if `srcs_not_rdy_vector == 0`
- and `state == OS_IN_RS`
- and `!in_rdy_list`
- insert at `node_local->rdy_head` and set `in_rdy_list = TRUE`

Recommended stats:

- `TEA_STALE_MAIN_DEP_CLEARED`
- `TEA_STALE_MAIN_DEP_OPS_READIED`
- `TEA_STALE_MAIN_DEP_RENAME_CLEARED`
- `TEA_STALE_MAIN_DEP_NODE_CLEARED`

### 4.3 Safety Notes

- This should run after `recover_tea_on_flush()` terminates younger chains, so terminated-chain ops
  have already been removed.
- This does not change functional values.  TEA is already speculative and uses the cached dynamic
  op stream; the goal here is to prevent a surviving TEA op from waiting on a producer that no
  longer exists in the simulated machine.
- Do not clear dependencies on valid older main producers; those still model real timing.

## 5. Phase C: Dynamic Oracle Copy for TEA Memory Ops

### 5.1 Current Problem

`tea_create_op_from_cache()` currently does this:

- copy `inst_info`
- copy `table_info`
- set TEA bookkeeping fields
- if this op is the target H2P branch, copy `h2p_oracle_info` and `h2p_recovery_info`

For non-H2P memory ops, the dynamic memory fields are not copied.  But `dcache_stage.c` uses:

- `op->oracle_info.va`
- `op->oracle_info.mem_size`
- `op->oracle_info.new_mem_value`
- cache/miss-related dynamic fields during DCache access

Therefore TEA memory ops can access address 0 or stale/default memory metadata.  That makes DCache
timing, TEA store-buffer forwarding, and any future TEA-load prefetch experiment unreliable.

### 5.2 Planned Fix

Add a helper in `tea_fetch_stage.c`:

```text
tea_copy_cached_dynamic_oracle_fields(Op* tea_op, Op* cached_op)
```

Policy:

- For the H2P branch:
  - keep current behavior: copy `c->h2p_oracle_info` and `c->h2p_recovery_info`
  - this preserves the live main H2P prediction/recovery state captured at trigger time
- For non-H2P memory ops:
  - copy memory dynamic fields from `cached_op->oracle_info`
  - minimum fields:
    - `va`
    - `mem_size`
    - `old_mem_value`
    - `new_mem_value`
    - `inst_sim_cycle`
    - `l1_miss`, `mlc_miss`, `l1_miss_satisfied`, `mlc_miss_satisfied`
    - `dep_on_l1_miss`, `was_dep_on_l1_miss`
  - `dcmiss` can be copied as diagnostic history, but DCache access will recompute the current
    access result and may overwrite it.
- Do not rely on copied `num_srcs` / `src_info`.
  - `tea_rename_op()` must remain the authority for dependency tracking and should keep resetting
    `op->oracle_info.num_srcs = 0`.
- For non-H2P non-memory ops:
  - no broad full-oracle copy is required in this pass.
  - copy only fields that are proven consumed by TEA execution.

Add debug checks:

- If a TEA memory op reaches `tea_create_op_from_cache()` with `mem_size == 0`, count a stat.
- If a TEA load/store reaches DCache with `va == 0` and the original cached op had a nonzero VA,
  count a stat or assert in debug mode.

Recommended stats:

- `TEA_MEM_ORACLE_COPIED`
- `TEA_MEM_ORACLE_ZERO_VA`
- `TEA_MEM_ORACLE_ZERO_SIZE`

### 5.3 Validation

- TEA loads should show nonzero addresses in dependency-chain / fill-buffer logs.
- TEA store-buffer writes/reads should use the same VA and size fields used by chain construction.
- If TEA-load prefetching is added later, this phase is a prerequisite.

## 6. Phase D: HBT Decay by Retired Instructions

### 6.1 Current Problem

`hbt.c` comments match the paper: all counters decrement every 50K instructions.  The implementation
increments `retired_branch_count` in `hbt_update()` and decays every 50K retired branches.

Since branches are much less frequent than instructions, this makes decay far slower than intended.
Stale H2P branches can remain marked across phase changes, increasing TEA trigger pressure and
hurting timeliness.

### 6.2 Planned Fix

Move the periodic decay trigger out of `hbt_update()` and into the main instruction retirement path.

Planned API:

```text
void hbt_retire_instruction_tick(uns proc_id);
```

Implementation details:

- Add `HBT_DECAY_INTERVAL 50000` in `hbt.h` or `hbt.c`.
- Add `uns64 retired_instruction_count` or `hbt_retired_instruction_count`.
- Reset it in `hbt_init()`.
- In `node_retire()`, call `hbt_retire_instruction_tick()` exactly when Scarab increments
  `inst_count[proc_id]`, i.e. inside the `if (op->eom)` block after a main instruction retires.
- Keep `hbt_update(Op* op)` responsible only for branch-specific counter allocation/increment.
- Keep `retired_branch_count` if useful for diagnostics, but it should no longer trigger decay.

Recommended stats:

- `HBT_DECAY_EVENTS`
- `HBT_RETIRED_INST_TICKS`
- optional: `HBT_RETIRED_BRANCH_UPDATES`

### 6.3 Validation

- In a 100K retired-instruction run, HBT decay should fire twice regardless of branch count.
- HBT H2P population should generally decrease versus the old implementation in long phases.
- Watch TEA trigger stats after this change:
  - `TEA_TRIGGER_ATTEMPTS`
  - `TEA_TRIGGERS`
  - `TEA_TRIGGER_SKIP_FULL`
  - `TEA_TRIGGER_SKIP_NO_CHAIN`

## 7. Phase E: Backward Walk for All H2P Branches in a Snapshot

### 7.1 Current Problem

`add_dependency_chain()` currently scans from the youngest op backward, picks the first op with
`oracle_info.hbt_pred_is_hard`, and builds only that one dependency chain.

This loses coverage when a Fill Buffer snapshot contains multiple H2P branches.  It is especially
important for workloads like `leela`, where multiple H2P branches can coexist in the sampled window.

The paper traces chains for all marked H2P branches, including multiple dynamic instances of the
same H2P branch.  The current one-H2P policy under-populates the dependency-chain cache and can make
`TEA_TRIGGER_SKIP_NO_CHAIN` artificially high for non-youngest H2Ps.

### 7.2 Planned Refactor

Refactor `add_dependency_chain()` into helpers:

```text
build_block_start_pc_map(...)
collect_h2p_indices(...)
build_dependency_mask_for_target(...)
commit_dependency_chain_entry(...)
commit_block_cache_masks(...)
```

Algorithm:

1. Build `block_start_pc_map[]` exactly as today.
2. Collect all indices where `ordered_ops[i].oracle_info.hbt_pred_is_hard`.
3. If no H2P exists, return.
4. For each H2P target index:
   - initialize a fresh `SourceList`
   - initialize a fresh `is_data_dependent[]`
   - mark the target H2P itself
   - walk backward to the oldest snapshot entry
   - track register dependencies through the bit vector
   - track memory dependencies through the 16-entry address buffer
   - produce one direct `dependency_chain_cache` entry keyed by target H2P PC
5. Maintain a separate `is_data_dependent_union[]` across all target walks.
6. Build Block Cache masks from the union and keep the existing OR behavior.

Direct dependency-chain cache overwrite policy:

- Process H2P targets in program order from oldest to youngest.
- The cache remains direct-mapped by `h2p_pc % DEPENDENCY_CHAIN_CACHE_SIZE`.
- If the same H2P PC appears multiple times in one snapshot, the younger dynamic instance overwrites
  the older one, preserving the current "most recent observed chain" behavior.
- If later experiments show this hurts chain length, add a stat and test a longest-chain policy, but
  do not combine that with the first implementation.

Block Cache policy:

- Use the union mask across all H2P targets.
- Existing `dependency_mask = old_mask | new_mask` behavior remains.
- Empty block tag-store insertion remains unchanged for blocks with no dependent ops.

Recommended stats:

- `DCC_SNAPSHOT_H2P_BRANCHES_TOTAL`
- `DCC_SNAPSHOT_H2P_BRANCHES_0`, `DCC_SNAPSHOT_H2P_BRANCHES_1`,
  `DCC_SNAPSHOT_H2P_BRANCHES_2`, `DCC_SNAPSHOT_H2P_BRANCHES_3`,
  `DCC_SNAPSHOT_H2P_BRANCHES_4_PLUS`
- `DCC_CHAINS_INSERTED`
- `DCC_CHAIN_OVERWRITE_SAME_PC`
- `DCC_BLOCK_MASK_OR_UPDATES`

### 7.3 Performance and Correctness Notes

- This change is expected to increase dependency-chain cache coverage and therefore increase TEA
  trigger eligibility.
- It can also increase TEA chain-slot pressure.  Interpret later results together with:
  - `TEA_TRIGGER_SKIP_FULL`
  - active/fetching/executing chain distribution stats
  - PREG and RS stalls
- The current TEA fetch path still does not consume Block Cache masks directly.  The direct
  dependency-chain entries are what affect execution immediately.
- The Block Cache union update is still worth maintaining because it keeps the code closer to the
  paper and preserves a future path toward BP-address-stream TEA fetch.

## 8. Source File Change List for Later Implementation

Expected source changes after plan approval:

| File | Planned changes |
|---|---|
| `src/node_issue_queue.cc` | Clear TEA `OS_DONE` ready-list entries and decrement RS counters exactly once |
| `src/node_stage.c` | Defensive TEA retire check; flush-path RS counter handling for `OS_DONE`; HBT retired-instruction tick call |
| `src/tea/tea_fetch_stage.c` | Copy dynamic memory oracle fields from cached ops |
| `src/cmp_model.c` | Add stale main-producer dependency cleanup after selective TEA chain recovery |
| `src/dependency_chain_cache.c` | Process all H2P branches per snapshot; union block-cache masks |
| `src/dependency_chain_cache.h` | Add helper declarations only if helpers need external visibility |
| `src/bp/hbt.c` | Move periodic decrement trigger to retired-instruction counter |
| `src/bp/hbt.h` | Add retired-instruction tick API and decay interval macro |
| `src/tea/tea.stat.def` or relevant stat defs | Add diagnostics listed above |

## 9. Regression Plan

Build-only checks:

- Compile with `TEA_ENABLE=0` default configs.
- Compile with TEA configs that set:
  - `TEA_MAX_CHAINS=4`
  - `TEA_MAX_CHAINS=8`
  - larger chain counts used in recent experiments

Short simulation checks:

- Run a short `leela` TEA-on case and confirm no ready-list/RS counter assertions.
- Run a short `mcf` TEA-on case because it stresses memory dependence behavior differently.
- Run one TEA-off case to confirm HBT/node-stage changes do not require TEA structures to be active.

Stat checks after Phase A/B:

- `TEA_RETIRE_READY_LIST_ESCAPE == 0`
- stale dependency cleanup stats are nonzero only when main recovery actually flushes producers
- no monotonically increasing active-chain count after chains naturally complete

Stat checks after Phase C:

- `TEA_MEM_ORACLE_COPIED > 0` when TEA chain contains memory ops
- `TEA_MEM_ORACLE_ZERO_VA` and `TEA_MEM_ORACLE_ZERO_SIZE` should be investigated if nonzero
- TEA store-buffer forward stats remain plausible

Stat checks after Phase D:

- `HBT_DECAY_EVENTS ~= retired_instructions / 50000`
- H2P population and TEA triggers may change; compare to previous branch-based decay as an expected
  behavioral change, not necessarily a regression.

Stat checks after Phase E:

- multiple dependency-chain entries can be logged from one snapshot
- `TEA_TRIGGER_SKIP_NO_CHAIN` should decrease for benchmarks with multiple H2Ps per Fill Buffer
- if `TEA_TRIGGER_SKIP_FULL` rises, interpret it as newly exposed TEA slot pressure rather than a
  direct Phase E bug

## 10. Main Risks

- Phase A can double-decrement RS counters if state ownership is not centralized.  Use one helper
  and keep ready-list states and node-table states disjoint.
- Phase B can hide real latency if it clears dependencies too broadly.  Restrict cleanup to
  invalid flushed main producers with `src_info.op_num >= recovery_op_num`.
- Phase C improves current memory metadata consistency, but it does not make cached dynamic memory
  values future-perfect.  It only ensures TEA uses the dynamic fields that were captured in the
  dependency-chain entry.
- Phase D changes H2P phase behavior and may reduce TEA trigger count.  That is expected if stale
  H2Ps were previously surviving too long.
- Phase E may expose resource bottlenecks by increasing chain coverage.  Analyze it with chain-slot,
  PREG, RS, and early-flush timeliness stats together.

## 11. Recommended Commit Split

Use separate commits or checkpoints:

1. `tea: fix completed-op ready-list cleanup`
2. `tea: clear stale main producer deps after recovery`
3. `tea: copy dynamic oracle fields for memory ops`
4. `bp: decay HBT by retired instructions`
5. `tea: build dependency chains for all H2Ps in snapshot`

This split makes it possible to bisect correctness regressions separately from the larger coverage
change in Phase E.
