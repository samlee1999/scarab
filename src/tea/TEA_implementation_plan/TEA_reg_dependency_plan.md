# TEA Register Dependency 구현

> **상태**: ✅ §1-§3 구현 완료 | ✅ §4.1 구현 완료 (node_stage.c flush_tea_ops_by_chain_id) | ⬜ §4.2 미구현 (불필요: shadow_rat_snapshot memset으로 inter-thread dep 제거) | ⬜ §5 Per-chain preg 반환 및 Shadow RAT rollback 미구현

---

## §1-§3 단일 H2P 구현 (완료)

기존 `tea_rename_op()`은 Shadow RAT에서 arch→phys 매핑만 읽고 producer Op를 추적하지 않아 모든 TEA ops가 즉시 ready로 처리되었다. 이를 `Shadow_RAT` 구조체에 `gp_producer_ops/unums`, `vec_producer_ops/unums` 4개 필드를 추가하여 해결.

- `shadow_rat_snapshot()`: Main thread `map_data->reg_map[]` producer를 복사 (첫 chain trigger 시)
- `tea_rename_op()`: src 처리 시 `tea_add_src_dependency()`로 not-rdy bit 설정 + dst 처리 시 `shadow_rat_write_mapping()` + `gp_producer_ops[]` 갱신
- `tea_add_src_dependency()`: `add_src_from_map_entry()`의 TEA 버전 (`op_num < consumer_op_num` ASSERT 제거)

**수정 파일**: `tea_rename.h`, `tea_rename.c`

---

## §4 다중 H2P Dependency 관리 (완료)

### §4.1 `flush_tea_ops_by_chain_id()` — dependent ops not-rdy bit 강제 clear

Chain X flush 시 `free_op()` 전에 `op->wake_up_head`를 순회하여 dependent ops의 `srcs_not_rdy_vector` bit 강제 clear. bit가 0이 되면 ready list에 추가.
구현 위치: `node_stage.c: flush_tea_ops_by_chain_id()` step 5 (`free_op()` 직전).

### §4.2 `recover_tea_on_flush()` — 생존 chain의 flushed Main op 의존성 정리

Main misprediction 복구 시 flushed Main ops (`op_pool_valid=FALSE`)를 producer로 가진 생존 TEA op의 not-rdy bit 강제 clear. `recover_node_stage()` 이후 실행되므로 `op_pool_valid` 체크만으로 stale producer 식별 가능.
구현 위치: `cmp_model.c: recover_tea_on_flush()` chain 종료 루프 이후 (`tea->num_active_chains > 0` guard).

---

## §5 Per-Chain Preg 반환 및 Shadow RAT Producer Rollback

> **상태**: ⬜ 미구현 — 다중 H2P 빌드 안정화 후 구현

### §5.1 문제

단일 H2P에서는 chain 종료 = TEA 전체 종료이므로 `reset_tea_preg_pool()`이 즉시 호출되어 문제 없음.
다중 H2P에서:

- **Preg 고갈**: chain A early flush 후에도 chain B/C가 살아있으면 `num_active_chains > 0` →
  `reset_tea_preg_pool()` 미호출 → chain A가 할당한 preg 미반환 → pool 점진 고갈
- **Shadow RAT 오염**: `gp_producer_ops[R5] = Op_A` (chain A의 op)가 잔존 → chain A 종료 후
  Op_A가 freed → `op_pool_valid=FALSE` → 이후 chain이 R5를 읽으면 "즉시 ready"로 처리
  → 실제로 기다려야 했던 chain 간 의존성 타이밍 손실

### §5.2 현재 dep 추적 구조 (왜 로직은 맞는가)

`gp_producer_ops[]`는 메인 스레드 `map_data->reg_map[]`과 동일한 oracle 기반 구조다:

| | 메인 스레드 | TEA |
|--|---------|---------|
| 추적 대상 | arch_reg → 현재 writer Op* | arch_reg → 현재 writer TEA Op* |
| 초기화 | flush 시 `recover_map()` 자동 복구 | chain 전체 종료 시만 `reset_tea_rename_stage()` |
| dep 설정 | `add_src_from_map_entry()` | `tea_add_src_dependency()` |

dep 추적 로직 자체(arch-reg 기반)는 변경하지 않는다.
**수명 관리만 수정**: chain 단위로 preg 반환 + Shadow RAT rollback.

### §5.3 `Tea_H2P_Chain` 필드 추가

```c
// tea_thread.h — Tea_H2P_Chain 구조체 확장
typedef struct Tea_H2P_Chain_struct {
  /* ... 기존 필드 ... */

  /* Per-chain preg allocation list — for individual return on termination.
   * Populated by tea_rename_op() for each dst reg allocation.
   * Size upper bound: MAX_CHAIN_LENGTH * max_dst_regs_per_op (실제 2 이하) */
  uns  gp_pregs_allocated[MAX_CHAIN_LENGTH * 2];
  uns  num_gp_pregs;
  uns  vec_pregs_allocated[MAX_CHAIN_LENGTH * 2];
  uns  num_vec_pregs;

  /* Per-chain Shadow RAT producer snapshot — saved at trigger, restored at termination.
   * Enables rollback of gp_producer_ops[] entries that THIS chain wrote.
   * Dynamically allocated after init_shadow_rat() so gp_size/vec_size are known. */
  Op**     saved_gp_producer_ops;    /* calloc(srat->gp_size, sizeof(Op*)) */
  Counter* saved_gp_producer_unums;  /* calloc(srat->gp_size, sizeof(Counter)) */
  Op**     saved_vec_producer_ops;   /* calloc(srat->vec_size, sizeof(Op*)) */
  Counter* saved_vec_producer_unums; /* calloc(srat->vec_size, sizeof(Counter)) */
} Tea_H2P_Chain;
```

**할당 시점**: `init_tea_preg_pools(proc_id)` 이후 별도 `init_tea_chain_lifecycle_buffers(proc_id)` 호출.
`srat->gp_size` / `srat->vec_size`가 확정된 이후여야 올바른 크기로 calloc 가능.

```c
// cmp_model.c: init 순서에 추가
init_tea_preg_pools(proc_id);
init_tea_chain_lifecycle_buffers(proc_id);  /* 신규 함수 */
```

```c
// tea_rename.c (또는 tea_thread.c): init_tea_chain_lifecycle_buffers()
void init_tea_chain_lifecycle_buffers(uns proc_id) {
  Shadow_RAT* srat = tea_rename_stages[proc_id]->shadow_rat;
  Tea_Thread*  tea = tea_threads[proc_id];
  for (int i = 0; i < MAX_TEA_CHAINS; i++) {
    Tea_H2P_Chain* c = &tea->chains[i];
    c->saved_gp_producer_ops    = calloc(srat->gp_size, sizeof(Op*));
    c->saved_gp_producer_unums  = calloc(srat->gp_size, sizeof(Counter));
    c->saved_vec_producer_ops   = calloc(srat->vec_size, sizeof(Op*));
    c->saved_vec_producer_unums = calloc(srat->vec_size, sizeof(Counter));
    c->num_gp_pregs  = 0;
    c->num_vec_pregs = 0;
  }
}
```

### §5.4 `trigger_tea_thread()` — chain 할당 직후 Shadow RAT snapshot

```c
// tea_thread.c: trigger_tea_thread() — c->state = CHAIN_FETCHING 설정 직후
Shadow_RAT* srat = tea_rename_stages[proc_id]->shadow_rat;

memcpy(c->saved_gp_producer_ops,    srat->gp_producer_ops,
       srat->gp_size * sizeof(Op*));
memcpy(c->saved_gp_producer_unums,  srat->gp_producer_unums,
       srat->gp_size * sizeof(Counter));
memcpy(c->saved_vec_producer_ops,   srat->vec_producer_ops,
       srat->vec_size * sizeof(Op*));
memcpy(c->saved_vec_producer_unums, srat->vec_producer_unums,
       srat->vec_size * sizeof(Counter));

c->num_gp_pregs  = 0;
c->num_vec_pregs = 0;
```

**snapshot 의미**: "이 chain이 rename을 시작하기 직전의 Shadow RAT producer 상태".
이전 chain들이 여기까지 쓴 producer 정보를 포함. 이 chain 종료 시 이 snapshot으로 복원.

### §5.5 `tea_rename_op()` — preg 할당 시 chain 목록에 기록

```c
// tea_rename.c: tea_rename_op() — tea_preg_pool_alloc() 호출 직후, shadow_rat_write_mapping() 전

Tea_Fetch_Stage* tf = tea_fetch_stages[proc_id];
Tea_H2P_Chain*   c  = &tea_threads[proc_id]->chains[tf->current_chain_id];

if (reg_type == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
  ASSERT(proc_id, c->num_gp_pregs < MAX_CHAIN_LENGTH * 2);
  c->gp_pregs_allocated[c->num_gp_pregs++] = (uns)new_phys_reg_id;
} else {
  ASSERT(proc_id, c->num_vec_pregs < MAX_CHAIN_LENGTH * 2);
  c->vec_pregs_allocated[c->num_vec_pregs++] = (uns)new_phys_reg_id;
}
```

### §5.6 `terminate_tea_chain()` — rollback 및 preg 반환 삽입 위치와 순서

**핵심**: rollback은 step 2 (rename stage flush / `free_op()`) **이전**에 수행해야 한다.
`op_pool_setup_op()`는 `h2p_chain_id`를 reset하지 않으므로 freed op 재사용 시 stale `h2p_chain_id`가
남아 rollback 조건을 오염시킬 수 있기 때문 → `op_pool_valid=TRUE` 상태일 때만 rollback 조건이 안전.

**수정된 terminate_tea_chain() 실행 순서**:

```
기존 Step 1: fetch stage flush (fetch 중인 ops free — 아직 rename 안 됨, Shadow RAT 미기록)
★ 신규 Step 2.5: Shadow RAT rollback (rename + node stage ops 아직 op_pool_valid=TRUE)
★ 신규 Step 2.6: Per-chain preg 반환 (chain allocation list 기반, op validity 불필요)
기존 Step 2: rename stage flush (rollback 이후 free_op() 호출)
기존 Step 3: node/RS/exec/dcache flush + not-rdy bit clear (§4.1)
기존 Step 4: store buffer clear
기존 Step 5: chain slot INACTIVE 마킹 + num_active_chains--
기존 Step 6: 마지막 chain이면 shared resource 전체 정리
```

```c
/* Step 2.5 (신규): Shadow RAT rollback */
Shadow_RAT* srat = tea_rename_stages[proc_id]->shadow_rat;
for (uns i = 0; i < srat->gp_size; i++) {
  Op* cur = srat->gp_producer_ops[i];
  if (cur && cur->op_pool_valid && cur->h2p_chain_id == h2p_chain_id) {
    srat->gp_producer_ops[i]   = c->saved_gp_producer_ops[i];
    srat->gp_producer_unums[i] = c->saved_gp_producer_unums[i];
  }
}
for (uns i = 0; i < srat->vec_size; i++) {
  Op* cur = srat->vec_producer_ops[i];
  if (cur && cur->op_pool_valid && cur->h2p_chain_id == h2p_chain_id) {
    srat->vec_producer_ops[i]   = c->saved_vec_producer_ops[i];
    srat->vec_producer_unums[i] = c->saved_vec_producer_unums[i];
  }
}

/* Step 2.6 (신규): Per-chain preg 반환 */
for (uns i = 0; i < c->num_gp_pregs; i++)
  tea_preg_pool_free(srat->tea_gp_preg_pool, c->gp_pregs_allocated[i]);
for (uns i = 0; i < c->num_vec_pregs; i++)
  tea_preg_pool_free(srat->tea_vec_preg_pool, c->vec_pregs_allocated[i]);
c->num_gp_pregs  = 0;
c->num_vec_pregs = 0;

/* Step 2 (기존): rename stage flush (rollback 이후 free_op() 호출) */
recover_tea_rename_stage_by_chain(proc_id, h2p_chain_id);
```

### §5.7 `reset_tea_thread()` — per-chain preg 카운터 초기화

```c
for (int i = 0; i < MAX_TEA_CHAINS; i++) {
  tea->chains[i].num_gp_pregs  = 0;
  tea->chains[i].num_vec_pregs = 0;
  /* saved_*_producer_ops/unums: 내용은 trigger 시 덮어쓰므로 초기화 불필요 */
}
```

### §5.8 기타

- `terminate_tea_thread()`: 전체 flush + `reset_tea_preg_pool()` (전체 반환) → per-chain rollback 불필요. 변경 없음.
- `tea_preg_pool_free()`: `__attribute__((unused))` 제거.

### §5.9 rollback 정확성

| 시나리오 | 동작 |
|---------|------|
| Chain A만 R5 write, Chain A 종료 | `gp_producer_ops[R5]` → snapshot 복원 ✅ |
| Chain A가 R5 write, Chain B도 R5 write, Chain A 종료 | 현재 producer = chain B의 op → h2p_chain_id 불일치 → 복원하지 않음 ✅ |
| Chain B가 Chain A의 R5 write에 의존, Chain A 종료 | `flush_tea_ops_by_chain_id()` (§4.1)에서 not-rdy bit clear → chain B ops 즉시 ready ✅ |
| Chain A 종료 후 Chain C trigger | `gp_producer_ops[R5]` = snapshot 복원된 상태 → Chain C snapshot에 올바른 상태 캡처 ✅ |

### §5.10 수정 파일 요약

| 파일 | 함수 | 변경 내용 |
|------|------|----------|
| `src/tea/tea_thread.h` | `Tea_H2P_Chain` | per-chain preg 목록 + producer snapshot 포인터 필드 추가 |
| `src/tea/tea_thread.c` | `trigger_tea_thread()` | chain 할당 직후 Shadow RAT snapshot + preg 카운터 초기화 |
| `src/tea/tea_thread.c` | `terminate_tea_chain()` | Step 2.5~2.6 삽입 (step 2 이전) |
| `src/tea/tea_thread.c` | `reset_tea_thread()` | per-chain preg 카운터 초기화 추가 |
| `src/tea/tea_rename.c` | `tea_rename_op()` | dst 할당 시 `c->gp_pregs_allocated[]` 기록 추가 |
| `src/tea/tea_rename.c` | `init_tea_chain_lifecycle_buffers()` | 신규 함수 — snapshot 버퍼 calloc |
| `src/tea/tea_rename.c` | `tea_preg_pool_free()` | `__attribute__((unused))` 제거 |
| `src/cmp_model.c` | init 순서 | `init_tea_preg_pools()` 이후 `init_tea_chain_lifecycle_buffers()` 호출 추가 |
