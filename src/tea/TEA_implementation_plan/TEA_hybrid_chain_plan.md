# TEA Hybrid Chain 구현 계획: Dependency Chain Cache + Block Cache OR 누적

**최종 갱신**: 2026-03-21
**관련 문서**:
- [`TEA_implementation_plan.md`](../TEA_implementation_plan.md) — 마스터 문서 §6 (작업 C)
- [`TEA_shadow_ftq_status.md`](../TEA_implementation_status/TEA_shadow_ftq_status.md) — 현재 fetch 방식
- [`TEA_multi_h2p_plan.md`](TEA_multi_h2p_plan.md) — 다중 H2P 계획 (dependency_chain_cache 변경 불필요 → 수정 필요)

---

## 1. 문제점: 현재 Dependency Chain Cache의 단일 경로 덮어쓰기

### 1.1 현재 동작

`add_dependency_chain()` 파트 2 (`dependency_chain_cache.c:152-168`):

```c
dep_entry->is_valid = TRUE;
dep_entry->h2p_branch_pc = trigger_op->inst_info->addr;
dep_entry->chain_length = 0;
for (int i = first_dep_op_idx; i <= trigger_op_idx; ++i) {
    if (is_data_dependent[i]) {
        dep_entry->chain[dep_entry->chain_length++] = ordered_ops[i];  // 덮어쓰기
    }
}
```

매 BW Walk 시 이전 chain을 **완전히 덮어쓴다**.

### 1.2 문제

동일 H2P 분기에 대해 서로 다른 경로(path)로 도달하는 경우:

```
Walk 1 (Path A): [block_X] → [block_Y] → [block_Z] → [H2P]
Walk 2 (Path B): [block_X] → [block_W] → [block_Z] → [H2P]
```

Walk 2 완료 후 dep cache에는 Path B의 chain만 남음. Path A에서만 나타나는
block_Y의 의존 ops가 유실됨 → TEA precomputation이 Path A로 실행될 때 의존성 누락.

### 1.3 Block Cache는 이미 해결

파트 3 (`dependency_chain_cache.c:194-214`)의 Block Cache는 bitmask OR 누적으로
다중 경로를 정확히 처리:

```c
uint64_t old_mask = block_entry->dependency_mask;
block_entry->dependency_mask = old_mask | new_mask;  // OR 누적 → 합집합
```

**하지만** TEA Fetch는 Block Cache를 사용하지 않고 dep cache만 조회.

---

## 2. 하이브리드 설계 개요

### 2.1 핵심 아이디어

1. **TEA Fetch 인터페이스 유지**: `get_dependency_chain(proc_id, h2p_pc)` 변경 없음
2. **Block Cache OR 누적 활용**: 기존 파트 3 로직 그대로 사용 (이미 구현됨)
3. **Dep cache chain을 Block Cache에서 재구축**: BW Walk 완료 시, dep cache의 chain[]을
   Block Cache 엔트리들의 누적 bitmask로부터 재구축
4. **block_pcs 추적**: dep cache 엔트리에 "이 H2P chain에 기여하는 block PC 목록" 저장,
   walk마다 합집합(union) 갱신

### 2.2 데이터 흐름

```
BW Walk 완료
    │
    ├── 파트 1: Backward Dataflow Walk (변경 없음)
    │     is_data_dependent[] 마킹
    │
    ├── 파트 2 (수정): dep cache에 block_pcs 갱신
    │     기존: chain[] 직접 저장 (덮어쓰기)
    │     변경: block_pcs[] 합집합 갱신 (union merge)
    │
    ├── 파트 3: Block Cache OR 누적 (변경 없음)
    │     block_entry->dependency_mask |= new_mask
    │
    └── 파트 4 (신규): Block Cache → dep cache chain 재구축
          dep cache의 block_pcs[]를 순회하며
          각 Block Cache 엔트리의 누적 bitmask로 chain[] 재구성
```

### 2.3 효과

| 항목 | 현재 (덮어쓰기) | 하이브리드 (OR 누적) |
|------|-----------------|---------------------|
| 블록 내 다중 경로 | ❌ 마지막 walk만 | ✅ bitmask OR 합집합 |
| 블록 간 다중 경로 | ❌ 마지막 walk만 | ✅ block_pcs union |
| Chain 길이 | 짧음 (단일 path) | 김 (합집합, 최대 MAX_CHAIN_LENGTH) |
| 정확성 (시뮬레이터) | ✅ oracle_info 사용 | ✅ 동일 — oracle_info 사용 |
| 타이밍 영향 | — | chain 길이 증가 → precomputation latency 증가 (trade-off) |

### 2.4 정확성 근거

시뮬레이터 환경에서 TEA는 **타이밍만 모델링**:
- TEA H2P branch의 실행 결과는 `oracle_info`로 결정 (`tea_fetch_stage.c:220-222`)
- 다른 경로의 ops가 chain에 포함되더라도 "wrong precomputation" 문제 없음
- 추가 ops는 TEA 파이프라인 latency만 증가시킴 — 논문 §III-E의 의도된 trade-off

---

## 3. 데이터 구조 변경

### 3.1 `Dependency_Chain_Cache_Entry` — block_pcs 필드 추가

**파일**: `src/dependency_chain_cache.h`

```c
#define MAX_BLOCKS_PER_CHAIN 16  // 신규 상수

typedef struct Dependency_Chain_Cache_Entry_struct {
  Flag          is_valid;
  Addr          h2p_branch_pc;
  Counter       h2p_branch_op_num;
  uns           chain_length;
  Op            chain[MAX_CHAIN_LENGTH];
  uint64_t      dependency_mask;
  uns           total_ops_in_block;

  /* === Hybrid chain: Block Cache 연동 필드 (신규) === */
  Addr          block_pcs[MAX_BLOCKS_PER_CHAIN];  // 이 H2P chain에 기여하는 block PC 목록 (프로그램 순서)
  uns           num_blocks;                         // block_pcs의 유효 엔트리 수
} Dependency_Chain_Cache_Entry;
```

**설계 근거**:
- `MAX_BLOCKS_PER_CHAIN = 16`: 일반적인 dependency chain은 2~8개 basic block 범위 내.
  MAX_CHAIN_LENGTH=64 ops / 평균 block 크기 ~8 ops = ~8 blocks. 16은 충분한 여유.
- `block_pcs[]`는 프로그램 순서로 유지 — chain 재구축 시 ops가 올바른 순서로 나열됨

### 3.2 기존 필드 유지

`dependency_mask`, `total_ops_in_block`은 dep cache 엔트리에서 사용하지 않게 되지만
(Block Cache 엔트리에서만 의미), 구조체를 공유하므로 유지. 삭제하면 Block Cache
엔트리에서도 사라지므로 건드리지 않음.

---

## 4. 알고리즘 변경: `add_dependency_chain()`

### 4.1 파트 1: Backward Dataflow Walk — 변경 없음

`dependency_chain_cache.c:97-150` 그대로 유지. `is_data_dependent[]` 마킹,
`first_dep_op_idx`, `trigger_op_idx` 결정.

### 4.2 파트 2 (수정): block_pcs 합집합 갱신

기존 파트 2 (`dependency_chain_cache.c:152-168`)를 교체:

```c
// --- 파트 2 (수정): block_pcs 갱신 + H2P 메타 정보 저장 ---
Dependency_Chain_Cache_Entry* dep_cache = dependency_chain_caches[proc_id];
int dep_entry_index = trigger_op->inst_info->addr % DEPENDENCY_CHAIN_CACHE_SIZE;
Dependency_Chain_Cache_Entry* dep_entry = &dep_cache[dep_entry_index];

// (a) 이번 walk에서 의존 ops가 존재하는 block들의 PC 수집 (프로그램 순서)
Addr walk_blocks[MAX_BLOCKS_PER_CHAIN];
int walk_block_count = 0;
Addr prev_block_pc = 0;
for (int i = first_dep_op_idx; i <= trigger_op_idx; ++i) {
    if (!is_data_dependent[i]) continue;
    Addr bpc = block_start_pc_map[i];
    if (bpc != prev_block_pc && walk_block_count < MAX_BLOCKS_PER_CHAIN) {
        walk_blocks[walk_block_count++] = bpc;
        prev_block_pc = bpc;
    }
}

// (b) dep cache 엔트리 갱신
if (!dep_entry->is_valid || dep_entry->h2p_branch_pc != trigger_op->inst_info->addr) {
    // 첫 번째 walk (또는 다른 H2P에 의해 evict된 후): 초기화
    dep_entry->is_valid = TRUE;
    dep_entry->h2p_branch_pc = trigger_op->inst_info->addr;
    dep_entry->h2p_branch_op_num = trigger_op->op_num;
    memcpy(dep_entry->block_pcs, walk_blocks, sizeof(Addr) * walk_block_count);
    dep_entry->num_blocks = walk_block_count;
} else {
    // 후속 walk: block_pcs 합집합 갱신 (union merge)
    merge_block_pcs(dep_entry, walk_blocks, walk_block_count);
}
```

### 4.3 파트 3: Block Cache OR 누적 — 변경 없음

`dependency_chain_cache.c:170-226` 그대로 유지. 이미 올바르게 구현됨:
- `block_entry->dependency_mask = old_mask | new_mask;`
- `block_entry->chain[]`도 누적 mask 기준으로 재구축

### 4.4 파트 4 (신규): Block Cache → dep cache chain 재구축

파트 3 이후에 추가:

```c
// --- 파트 4 (신규): Block Cache에서 dep cache chain 재구축 ---
dep_entry->chain_length = 0;
for (int b = 0; b < dep_entry->num_blocks; b++) {
    Addr bpc = dep_entry->block_pcs[b];
    int block_idx = bpc % BLOCK_CACHE_SIZE;
    Dependency_Chain_Cache_Entry* block = &block_caches[proc_id][block_idx];

    // Block Cache 히트 확인 (aliasing으로 evict되었을 수 있음)
    if (!block->is_valid || block->h2p_branch_pc != bpc) continue;

    // Block Cache의 chain[]은 이미 누적 mask 기준으로 구축되어 있음 (파트 3)
    for (int j = 0; j < block->chain_length; j++) {
        if (dep_entry->chain_length < MAX_CHAIN_LENGTH) {
            dep_entry->chain[dep_entry->chain_length++] = block->chain[j];
        }
    }
}

log_dependency_chain_entry(proc_id, dep_entry, cycle_count);
```

**핵심 포인트**:
- Block Cache의 `chain[]`은 파트 3에서 이미 누적 bitmask 기준으로 구축됨
  (`dependency_chain_cache.c:207-214`)
- 동일 basic block의 같은 위치는 항상 같은 instruction이므로, 어떤 walk의 ops 복사본이든 무관
- Block Cache aliasing으로 evict된 block은 건너뜀 → chain이 부분적으로 짧아질 수 있지만 정확성에 영향 없음

### 4.5 `merge_block_pcs()` 헬퍼 함수 (신규)

```c
/* 프로그램 순서를 유지하면서 walk_blocks를 dep_entry->block_pcs에 합집합 병합.
 * walk_blocks는 현재 walk의 프로그램 순서.
 * 이전 walk의 block이 현재 walk에 없으면 기존 위치 유지.
 * 현재 walk의 block이 기존 목록에 없으면 walk 내 선행 block 뒤에 삽입.
 */
static void merge_block_pcs(Dependency_Chain_Cache_Entry* dep_entry,
                            Addr* walk_blocks, int walk_count) {
    for (int w = 0; w < walk_count; w++) {
        Addr bpc = walk_blocks[w];

        // 이미 block_pcs에 있는지 확인
        Flag found = FALSE;
        for (uns e = 0; e < dep_entry->num_blocks; e++) {
            if (dep_entry->block_pcs[e] == bpc) {
                found = TRUE;
                break;
            }
        }
        if (found) continue;

        // 용량 초과 시 건너뜀
        if (dep_entry->num_blocks >= MAX_BLOCKS_PER_CHAIN) continue;

        // 삽입 위치 결정: walk_blocks에서 이미 block_pcs에 존재하는
        // 가장 가까운 선행 block 바로 뒤
        int insert_pos = (int)dep_entry->num_blocks;  // 기본: 끝에 추가
        for (int p = w - 1; p >= 0; p--) {
            for (uns e = 0; e < dep_entry->num_blocks; e++) {
                if (dep_entry->block_pcs[e] == walk_blocks[p]) {
                    insert_pos = e + 1;
                    goto found_insert_pos;
                }
            }
        }
        found_insert_pos:

        // 삽입 위치 이후 요소들을 한 칸 뒤로 이동
        for (int e = (int)dep_entry->num_blocks; e > insert_pos; e--) {
            dep_entry->block_pcs[e] = dep_entry->block_pcs[e - 1];
        }
        dep_entry->block_pcs[insert_pos] = bpc;
        dep_entry->num_blocks++;
    }
}
```

**복잡도**: O(W × N × N) where W=walk_block_count, N=num_blocks. 최대 16×16×16 = 4096 연산. 무시 가능.

---

## 5. `periodically_reset_caches()` 변경

### 5.1 현재 코드 (`dependency_chain_cache.c:246-258`)

```c
void periodically_reset_caches(uns proc_id) {
    // Block Cache bitmask 리셋
    for (int i = 0; i < BLOCK_CACHE_SIZE; ++i) {
        block_caches[proc_id][i].dependency_mask = 0;
        block_caches[proc_id][i].chain_length = 0;
    }
    // Empty block tag store 리셋
    memset(empty_block_tag_store[proc_id], 0, ...);
}
```

### 5.2 변경: dep cache의 chain/block_pcs도 초기화

```c
void periodically_reset_caches(uns proc_id) {
    // Block Cache bitmask 리셋 (기존)
    if (block_caches && block_caches[proc_id]) {
        for (int i = 0; i < BLOCK_CACHE_SIZE; ++i) {
            if (block_caches[proc_id][i].is_valid) {
                block_caches[proc_id][i].dependency_mask = 0;
                block_caches[proc_id][i].chain_length = 0;
            }
        }
    }
    // Empty block tag store 리셋 (기존)
    if (empty_block_tag_store && empty_block_tag_store[proc_id]) {
        memset(empty_block_tag_store[proc_id], 0,
               sizeof(Block_Cache_Tag_Entry) * EMPTY_BLOCK_TAG_STORE_SIZE);
    }

    // === 신규: dep cache의 chain/block_pcs 초기화 ===
    // Block Cache가 리셋되면 dep cache chain은 stale → 초기화
    // is_valid와 h2p_branch_pc는 유지 (다음 walk에서 재구축 가능하도록)
    if (dependency_chain_caches && dependency_chain_caches[proc_id]) {
        for (int i = 0; i < DEPENDENCY_CHAIN_CACHE_SIZE; ++i) {
            Dependency_Chain_Cache_Entry* e = &dependency_chain_caches[proc_id][i];
            if (e->is_valid) {
                e->chain_length = 0;
                e->num_blocks = 0;
                // is_valid, h2p_branch_pc 유지
            }
        }
    }
}
```

**효과**:
- Block Cache 리셋 후 dep cache의 `chain_length = 0` → TEA trigger 시 `chain->chain_length == 0` 체크에 걸려 `TEA_TRIGGER_SKIP_NO_CHAIN` 발생
- 다음 BW Walk 완료 시 파트 2~4에서 자동 재구축
- 이전의 stale한 multi-path 누적 데이터가 주기적으로 정리됨

---

## 6. 작업 C 연동: `periodically_reset_caches()` 호출 연결

**파일**: `src/node_stage.c` (retire 루프 내)

마스터 플랜 §6에 기술된 대로, `node_retire()`에서 Main thread ops의 retire 카운터가
`BLOCK_CACHE_RESET_PERIOD` (기본 500K)에 도달할 때마다 호출.

**주의사항**:
- `node_retire()`의 `ret_count`는 로컬 변수 (line 622)이며 Main ops만 카운트함
  (TEA ops는 line 645에서 `continue`로 skip). 따라서 Main ops 기준 카운트가 자연스럽게 보장됨.
- Multi-core 환경에서 `static` 변수는 core별로 분리되어야 하므로,
  `Node_Stage` 구조체에 필드를 추가하는 것이 안전함 (`node->retired_since_last_reset`).

```c
// node_stage.h — Node_Stage 구조체에 추가
Counter retired_since_last_reset;  /* periodically_reset_caches 호출용 retire 카운터 */

// node_stage.c — node_retire() 내부, ret_count++ (line 665) 직후
node->retired_since_last_reset++;
if (node->retired_since_last_reset >= BLOCK_CACHE_RESET_PERIOD) {
    periodically_reset_caches(node->proc_id);
    node->retired_since_last_reset = 0;
}
```

**파라미터** (`core.param.def`):
```c
DEF_PARAM(block_cache_reset_period, BLOCK_CACHE_RESET_PERIOD, uns, uns, 500000, )
```

---

## 7. 기존 구현 계획과의 충돌 분석

### 7.1 작업 F (다중 H2P+DC): 충돌 없음 ✅

| 항목 | 영향 |
|------|------|
| `get_dependency_chain()` 인터페이스 | 변경 없음 — 작업 F의 모든 호출부 호환 |
| `Tea_Fetch_Stage` | 변경 없음 — `active_chain` 포인터가 가리키는 dep cache 엔트리의 chain[]이 더 길어질 뿐 |
| `tea_create_op_from_cache()` | 변경 없음 — chain[]의 Op 복사 방식 동일 |
| `Tea_Rename_Stage` | 변경 없음 — chain 내 ops가 더 많을 뿐, rename 로직 동일 |
| `Op.h2p_chain_id` | 무관 — 하이브리드는 chain 저장 방식 변경, h2p_chain_id는 다중 H2P 식별용 |

**수정 필요**: 작업 F의 `TEA_multi_h2p_plan.md` §9 "변경 불필요" 목록에서
`dependency_chain_cache.c/h`를 제거하고 하이브리드 변경 사항 추가.

### 7.2 작업 I (독립 Dispatch): 충돌 없음 ✅

독립 dispatch는 node_stage/node_issue_queue 레벨. chain 저장 방식과 무관.

### 7.3 작업 C (periodically_reset_caches): 시너지 ✅

하이브리드 도입으로 `periodically_reset_caches()`가 실질적 의미를 갖게 됨.
기존에는 Block Cache만 리셋하고 TEA Fetch가 Block Cache를 사용하지 않아 무의미했음.
하이브리드에서는 Block Cache 리셋이 dep cache chain 재구축에 직접 영향 → 연결 필수.

**결론**: 작업 C와 하이브리드를 함께 구현.

### 7.4 작업 B (Iterative Walk): 충돌 없음 ✅

Iterative Walk는 BW Walk의 도달 범위를 확장 (chain_bit 활용). 하이브리드와 직교:
- Iterative Walk → 더 긴 스냅샷 → 파트 1에서 더 많은 deps 발견 → 파트 2~4에서 더 큰 chain 구축
- 하이브리드의 OR 누적과 자연스럽게 결합됨

### 7.5 ASSERT 1&2 (§13.1): 충돌 없음 ✅

ASSERT들은 dispatch/RS 카운터 문제. chain 저장 방식과 무관.

### 7.6 작업 G (TEA 의존성 Wakeup): 충돌 없음 ✅

Shadow RAT producer 추적은 rename 단계. chain 내 ops가 많아져도 각 op의
src/dst rename 로직은 동일.

---

## 8. 다른 경로의 ops 포함 시 영향 분석

### 8.1 TEA Rename 영향

Shadow RAT에서 rename 시, 다른 경로의 ops가 chain에 포함되면:
- **src**: 해당 arch reg의 현재 Shadow RAT 매핑에서 producer를 찾음. 다른 경로의 op이라도
  같은 arch reg을 사용하면 올바른 데이터 의존성이 설정됨.
- **dst**: 새 TEA preg 할당. Shadow RAT 갱신. 이후 ops가 이 preg을 src로 사용.

**결과**: 다른 경로의 ops는 "실제로는 실행될 필요 없는 ops"이지만, 시뮬레이터에서는
타이밍만 모델링하므로 문제 없음. TEA preg pool 소비량이 증가할 뿐.

### 8.2 TEA Execute 영향

- 추가 ops가 RS/FU를 점유 → Main thread와의 자원 경합 미미하게 증가
- TEA H2P branch의 실행 시점이 약간 지연될 수 있음 (chain이 길어지므로)
- 논문 §III-E: "the overhead of additional computations in the TEA thread is small because
  most of the operations are shared across different paths"

### 8.3 TEA preg pool 영향

`TEA_PREG_POOL_SIZE = 192`에서, 합집합 chain이 최대 64 ops (MAX_CHAIN_LENGTH) 일 때:
- 각 op의 dst reg ≈ 1~2개 → 최대 ~128 pregs 소비
- 192 - 128 = 64 pregs 여유 → 단일 chain에서는 충분
- 다중 H2P (4 chains × 64 ops)에서는 부족 가능 → preg 부족 시 chain 종료로 처리
  (기존 `TEA_multi_h2p_plan.md` §12.4 정책 그대로 적용)

### 8.4 다른 H2P의 Block Cache 공유 영향 (cross-H2P 의존성 혼합)

Block Cache는 `block_start_pc`로 인덱싱되며, H2P branch PC와는 무관하다
(`dependency_chain_cache.c:195-198`). 따라서 **서로 다른 H2P branch의 BW Walk**가
같은 basic block을 포함하면, 해당 block cache entry의 `dependency_mask`에 두 H2P의
의존성이 OR로 합쳐진다:

```
H2P-A Walk: block X → dep bit 3 (reg R5 producer)
H2P-B Walk: block X → dep bit 7 (reg R9 producer)
→ block X의 mask = bit 3 | bit 7
```

Part 4에서 H2P-A의 dep cache chain을 재구축할 때, block X에서 bit 7 (H2P-B 전용)의
op까지 포함하게 된다.

**영향**: §8.1~8.2에서 분석한 "같은 H2P의 다른 경로 ops 포함"과 동일한 범주:
- TEA는 oracle_info를 사용하므로 정확성 문제 없음
- 추가 ops → TEA pipeline latency 미미 증가, TEA preg pool 소비 미미 증가
- `periodically_reset_caches()`가 Block Cache bitmask를 주기적으로 초기화하므로,
  cross-H2P 오염이 무한히 누적되지 않음

**§8.1~8.3과의 차이점**: 같은 H2P의 다른 경로 ops는 "precomputation에 기여할 가능성이 있는"
ops이지만, 다른 H2P의 deps는 "precomputation과 완전히 무관한" ops이다. 다만 시뮬레이터에서
timing-only 모델링이므로 실질적 차이는 없다.

### 8.5 Block Cache chain[] 재구축 시 Op 복사본 출처

Part 3 (`dependency_chain_cache.c:207-214`)에서 Block Cache의 `chain[]`을 OR 누적
mask 기준으로 재구축하지만, 복사 원본은 최신 walk의 `ordered_ops[]` 스냅샷이다.
이전 walk에서 set된 bit에 해당하는 op을 최신 walk의 ops에서 복사한다.

**정확성**: 같은 basic block + 같은 offset = 같은 instruction이므로, 어떤 walk의
Op 복사본이든 `inst_info`와 `table_info`는 동일. TEA Fetch는 이 정적 정보만
사용하므로 문제없다.

**Aliasing edge case**: 이전 walk에서 set된 bit의 offset이 최신 walk의 block 범위를
초과하면 (block cache aliasing으로 `total_ops_in_block`이 달라진 경우), 해당 bit은
무시될 수 있다. 그러나 line 198의 tag 체크 (`h2p_branch_pc == real_block_start_pc`)가
aliasing을 감지하여 miss 시 `old_mask = 0`으로 초기화하므로, 이 edge case는 안전하다.

### 8.6 merge_block_pcs() 프로그램 순서 근사

다른 경로의 blocks (예: Path A의 block Y, Path B의 block W)의 상대적 순서는
본질적으로 정의 불가능하다. `merge_block_pcs()`의 삽입 위치 휴리스틱은 근사값이며,
chain reconstruction 시 ops 순서에 미미한 차이가 생길 수 있다.

**영향**: TEA는 OoO backend에서 실행하고 oracle 값을 사용하므로 timing-only 영향.
프로그램 순서 정확성이 필요 없는 환경에서 수용 가능한 trade-off.

---

## 9. 수정 파일 요약

| 파일 | 변경 내용 | 규모 |
|------|----------|------|
| `src/dependency_chain_cache.h` | `MAX_BLOCKS_PER_CHAIN` 상수, `block_pcs[]`, `num_blocks` 필드 | ~5줄 추가 |
| `src/dependency_chain_cache.c` | 파트 2 교체 (~20줄), 파트 4 추가 (~15줄), `merge_block_pcs()` (~30줄), `periodically_reset_caches()` 확장 (~10줄) | ~75줄 수정/추가 |
| `src/node_stage.h` | `Node_Stage` 구조체에 `retired_since_last_reset` 필드 추가 (작업 C, multi-core safe) | 1줄 추가 |
| `src/node_stage.c` | `node_retire()`에서 `periodically_reset_caches()` 호출 연결 (작업 C) | ~5줄 추가 |
| `src/core.param.def` | `BLOCK_CACHE_RESET_PERIOD` 파라미터 (작업 C) | 1줄 |

**변경 불필요 (확인 완료)**:

| 파일 | 이유 |
|------|------|
| `tea/tea_fetch_stage.h/c` | `get_dependency_chain()` 인터페이스 동일, `active_chain->chain[]` 참조 방식 동일 |
| `tea/tea_rename.h/c` | chain 내 ops의 rename 로직 동일, Shadow RAT 순차 갱신 동일 |
| `tea/tea_thread.h/c` | trigger/terminate 로직 동일 — dep cache 조회 인터페이스 변경 없음 |
| `op.h` | Op 구조체 변경 없음 |
| `node_issue_queue.cc` | RS 스케줄링 무관 |
| `exec_stage.c` | Early Flush 로직 무관 |

---

## 10. 구현 순서

```
단계 1: 데이터 구조 변경
  └─ dependency_chain_cache.h: MAX_BLOCKS_PER_CHAIN, block_pcs[], num_blocks 추가

단계 2: merge_block_pcs() 헬퍼 구현
  └─ dependency_chain_cache.c: static 함수 추가

단계 3: 파트 2 교체
  └─ dependency_chain_cache.c: chain[] 직접 저장 → block_pcs 갱신으로 교체

단계 4: 파트 4 추가
  └─ dependency_chain_cache.c: Block Cache → dep cache chain 재구축

단계 5: periodically_reset_caches() 확장
  └─ dependency_chain_cache.c: dep cache chain/block_pcs 초기화 추가

단계 6: periodically_reset_caches() 호출 연결 (작업 C)
  └─ node_stage.c: retire 루프에서 주기적 호출
  └─ core.param.def: BLOCK_CACHE_RESET_PERIOD 파라미터 추가
```

---

## 11. 우선순위 배치

마스터 플랜 §10의 기존 우선순위에 하이브리드 작업(작업 HC)을 삽입:

```
작업 A (BW Walk Trigger) ✅ 완료
    │
    ▼
작업 G (TEA 의존성 Wakeup) ✅ 완료
    │
    ├──→ 작업 I (독립 Dispatch) ← ASSERT 1&2 근본 해결
    │
    ▼
작업 C + HC (periodically_reset + Hybrid Chain) ← 함께 구현
    │   Block Cache OR 누적을 dep cache에 반영
    │   periodically_reset에 실질적 의미 부여
    │
    ▼
시뮬레이션 검증: chain 합집합 확인, TEA_TRIGGERS 유지, 정확성 회귀 테스트
    │
    ▼
작업 F (다중 H2P+DC) ← 핵심 기능
    │
    ▼
작업 B (Iterative Walk) ← 체인 품질 향상
```

**근거**: 하이브리드는 작업 C와 함께 구현하는 것이 자연스러움 (둘 다 Block Cache 활용).
작업 I와는 독립적이므로 병렬 진행 가능. 작업 F 이전에 완료하는 것이 바람직 —
다중 H2P에서 chain 합집합의 효과가 극대화됨.

---

## 12. 검증

### 12.1 빌드 및 시뮬레이션

```bash
cd ~/scarab-infra
./sci --build-scarab tea_dbg
./sci --sim tea_dbg
/analyze_tea_sim
```

### 12.2 확인 지표

| 지표 | 기대값 | 의미 |
|------|--------|------|
| `TEA_TRIGGERS` | ≈ 기존 (미감소) | chain 가용성 유지 |
| `TEA_OPS_FETCHED` | ≥ 기존 (증가 가능) | 합집합으로 chain 길이 증가 |
| `TEA_FETCH_CHAIN_HIT` | ≈ 기존 | dep cache 적중률 유지 |
| `TEA_EARLY_FLUSHES` | ≥ 기존 (증가 가능) | 더 많은 의존 ops 포함 → 타이밍 개선 |
| ASSERT 없음 | TRUE | 정확성 회귀 없음 |

### 12.3 디버그 확인 방법

`log_dependency_chain_entry()` 출력에서:
- `chain_length`가 이전보다 증가하는 H2P PC 확인
- 동일 H2P PC에 대해 연속 BW Walk 후 `num_blocks`가 증가하는지 확인
- `periodically_reset_caches()` 후 `chain_length = 0` → 다음 walk 후 재구축 확인

---

## 13. 기존 문서 갱신 필요 항목

| 문서 | 갱신 내용 |
|------|----------|
| `TEA_implementation_plan.md` §2 | "Block Cache bitmask OR" 옆에 하이브리드 반영 추가 |
| `TEA_implementation_plan.md` §6 | 작업 C에 하이브리드 연동 내용 추가 |
| `TEA_implementation_plan.md` §10 | 우선순위에 작업 HC 삽입 |
| `TEA_multi_h2p_plan.md` §9 | "변경 불필요" 목록에서 `dependency_chain_cache.c/h` 제거 |
| `TEA_shadow_ftq_plan.md` §9 | 동일 |
| `TEA_shadow_ftq_status.md` §3.2 | "Block Cache 미활용" → "Hybrid Chain으로 간접 활용" |
