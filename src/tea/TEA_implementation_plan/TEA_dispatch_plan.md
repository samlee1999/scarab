# TEA 독립 Dispatch 구현 — 완료

> **상태**: ✅ 구현 완료 (`e0d65de` 기준)

---

## 1. 해결한 문제

기존 구현은 Node Table의 linked list를 `next_op_into_rs` 단일 포인터로 순차 순회하여 TEA/Main ops를 함께 dispatch했다. TEA op이 RS partition full로 skip되면 `first_undispatched`가 해당 TEA op을 기억하고, 다음 사이클에 이미 dispatch된 Main ops를 재방문 → `ASSERT(state == OS_IN_ROB)` 실패.

근본 원인: 단일 in-order dispatch 포인터가 TEA/Main 간 인위적 순서 의존성을 만들어냄 (논문 Fig.4의 2개 독립 Rename 출력 구조와 불일치).

---

## 2. 해결 방식 (구현 완료)

TEA ops를 `node_issue_queue_dispatch()` 순회에서 완전히 분리하여 독립 dispatch path 구성:

| 변경 | 내용 |
|------|------|
| `tea_dispatch_to_rs()` | TEA ops가 Node Table 진입 시 `node_dispatch_find_emptiest_rs()` 직접 호출로 RS dispatch. `next_op_into_rs`에 TEA ops 등록 안 함 |
| `tea_dispatch_retry()` | RS full로 OS_IN_ROB 상태로 남은 TEA ops를 `node_issue_queue_update()` 이후 매 사이클 재시도 |
| `node_issue_queue_dispatch()` | `first_undispatched` 완전 제거. `thread_id == 1 → continue`로 TEA ops skip. `main_op_count++`만 유지 |
| `node_dispatch_find_emptiest_rs()` | `node_issue_queue.h`에 프로토타입 추가로 외부 노출 |
| flush step 1 RS 카운터 동기화 | `flush_tea_ops_from_node_stage()` step 1에서 ready list의 OS_SCHEDULED/OS_MISS TEA ops 제거 시 RS 카운터 직접 감소. step 4는 whitelist 방식 (`OS_IN_RS`, `OS_READY`, `OS_WAIT_FWD` 등) |

---

## 3. 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `src/node_stage.c` | `tea_dispatch_to_rs()` RS 직접 dispatch. `tea_dispatch_retry()` 신규. `flush_tea_ops_from_node_stage()` step 1/4 RS 카운터 동기화 |
| `src/node_issue_queue.cc` | `first_undispatched` 제거. TEA ops skip. `main_op_count++` 단독 |
| `src/node_issue_queue.h` | `node_dispatch_find_emptiest_rs()` 프로토타입 추가 |

---

## 4. 미구현 항목 (Work F)

`flush_tea_ops_by_chain_id()` step 3 RS 카운터 동기화 — 동일한 OS_SCHEDULED/OS_MISS 처리 패턴을 chain 선택적 flush 함수에도 적용 필요. `TEA_multi_h2p_plan.md` §6.1에 포함됨.
