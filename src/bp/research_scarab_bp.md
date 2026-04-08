# Scarab Branch Prediction 서브시스템 심층 분석 보고서

작성일: 2026-03-08
분석 대상: `/home/lee/scarab/src/bp/` 디렉토리 전체

---

## 목차

1. [디렉토리 구조 개요](#1-디렉토리-구조-개요)
2. [아키텍처 설계 철학](#2-아키텍처-설계-철학)
3. [핵심 자료구조 분석](#3-핵심-자료구조-분석)
4. [Branch Predictor 인터페이스 (Bp)](#4-branch-predictor-인터페이스-bp)
5. [브랜치 예측 메인 흐름 (bp.c)](#5-브랜치-예측-메인-흐름-bpc)
6. [방향 예측기 구현체 상세 분석](#6-방향-예측기-구현체-상세-분석)
7. [타겟 예측 메커니즘 (BTB / CRS / IBTB)](#7-타겟-예측-메커니즘-btb--crs--ibtb)
8. [신뢰도 추정기 (Branch Confidence)](#8-신뢰도-추정기-branch-confidence)
9. [HBT (Hard Branch Table) — TEA 연동 핵심](#9-hbt-hard-branch-table--tea-연동-핵심)
10. [Late Branch Predictor (듀얼 BP)](#10-late-branch-predictor-듀얼-bp)
11. [복구 메커니즘 (Recovery)](#11-복구-메커니즘-recovery)
12. [통계 시스템 (bp.stat.def)](#12-통계-시스템-bpstatdef)
13. [파라미터 시스템 (bp.param.def)](#13-파라미터-시스템-bpparamdef)
14. [CBP 통합 어댑터 (cbp_to_scarab)](#14-cbp-통합-어댑터-cbp_to_scarab)
15. [TEA와의 통합 지점](#15-tea와의-통합-지점)
16. [CF 타입별 예측 처리 흐름 요약](#16-cf-타입별-예측-처리-흐름-요약)
17. [설계상 주목할 만한 패턴 및 트레이드오프](#17-설계상-주목할-만한-패턴-및-트레이드오프)

---

## 1. 디렉토리 구조 개요

```
src/bp/
├── bp.h / bp.c               # BP 서브시스템 핵심: 인터페이스 정의 + 메인 디스패처
├── bp.param.def / bp.param.h # 파라미터 정의 (~40개)
├── bp.stat.def               # 통계 이벤트 정의 (~200개)
├── bp_table.def              # 예측기 dispatch table (함수 포인터 배열 초기화)
├── cbp_table.def             # CBP 예측기 목록 (mtage, tage64k)
│
├── gshare.h / gshare.cc      # GShare 방향 예측기
├── hybridgp.h / hybridgp.cc  # HybridGP (Local+Global+Perceptron 하이브리드)
├── tagescl.h / tagescl.cc    # TAGE-SC-L 64KB (default, 기본 예측기)
│
├── cbp_tagescl_64k.h/.cc     # CBP 호환 TAGE-SC-L 64KB 구현
├── mtage_unlimited.h/.cc     # CBP 호환 mTage (무제한 버전)
│
├── bp_targ_mech.h / bp_targ_mech.c  # BTB / CRS / IBTB 구현
├── bp_conf.h / bp_conf.c     # 신뢰도 추정기 (Counter / OnPath / Perceptron)
├── hbt.h / hbt.c             # HBT (Hard Branch Table) — TEA 전용
│
├── cbp_to_scarab.h / cbp_to_scarab.cc  # CBP → Scarab 인터페이스 변환 어댑터
└── template_lib/             # HybridGP용 템플릿 유틸리티
```

**파일 수**: 26개 (헤더 포함)
**총 코드량**: 약 5,000+ 라인

---

## 2. 아키텍처 설계 철학

### 2.1 플러그인 방식의 다형성 설계

Scarab의 BP 서브시스템은 **함수 포인터 기반의 vtable 패턴**으로 설계되어 있다. C로 구현된 OOP 패턴으로, 런타임에 예측기 종류를 파라미터로 선택할 수 있다.

```c
// bp.h: 브랜치 예측기 "인터페이스" 정의
typedef struct Bp_struct {
  Bp_Id   id;
  const char* name;
  void  (*init_func)(void);
  void  (*timestamp_func)(Op*);
  uns8  (*pred_func)(Op*);          // 방향 예측
  void  (*spec_update_func)(Op*);   // 투기적 업데이트 (fetch 시)
  void  (*update_func)(Op*);        // 결과 확정 업데이트 (exec/retire 시)
  void  (*retire_func)(Op*);        // Retire 시 최종 업데이트
  void  (*recover_func)(Recovery_Info*); // 복구
  uns8  (*full_func)(uns);
} Bp;
```

동일한 인터페이스가 `Bp_Btb` (BTB), `Bp_Ibtb` (간접 분기 BTB), `Br_Conf` (신뢰도 추정기) 에도 적용된다.

### 2.2 예측과 평가의 분리 (FDIP 지원)

`bp_predict_op()` 는 두 단계로 분리된다:
1. `bp_predict_op()`: 예측 수행 (pred, pred_npc 설정)
2. `bp_predict_op_evaluate()`: 예측 결과 평가 (mispred, misfetch 계산)

이는 FDIP (Fetch-Directed Instruction Prefetching)가 예측 결과를 평가하기 전에 프리패치를 트리거할 수 있도록 하기 위한 설계다.

### 2.3 On-path / Off-path 이중 트래킹

모든 예측 이벤트는 `op->off_path` 플래그로 구분되며, 통계는 on/off path 각각 별도로 집계된다. 오프패스 업데이트는 `UPDATE_BP_OFF_PATH` 파라미터로 제어한다.

---

## 3. 핵심 자료구조 분석

### 3.1 Bp_Data — 코어당 BP 전체 상태

```c
typedef struct Bp_Data_struct {
  uns proc_id;

  struct Bp_struct*     bp;       // 메인 방향 예측기 (e.g., TAGE-SC-L)
  struct Bp_struct*     late_bp;  // Late BP (멀티사이클 지연 예측기, 옵션)
  struct Bp_Btb_struct* bp_btb;   // BTB
  struct Bp_Ibtb_struct*bp_ibtb;  // 간접 분기 타겟 예측기
  struct Br_Conf_struct*br_conf;  // 신뢰도 추정기 (선택적)

  uns32  global_hist;             // 전역 분기 이력 (32비트 shift register)
  Cache  btb;                     // BTB 캐시 구조체

  struct { /* CRS (Call-Return Stack) */
    Crs_Entry* entries;   // 실제 엔트리 배열 (2 * CRS_ENTRIES, on/off path 분리)
    Flag*  off_path;      // 각 엔트리의 off-path 여부
    uns    depth;         // 현재 스택 깊이
    uns    head, tail;    // 원형 버퍼 포인터
    uns    tail_save, depth_save; // on-path 체크포인트
    uns    tos, next;     // realistic CRS용 (next/tos 방식)
  } crs;

  Cache   tc_tagged;    // Tagged indirect target predictor
  Addr*   tc_tagless;   // Tagless indirect target predictor (직접 배열)
  uns8*   tc_selector;  // Hybrid IBTB 선택기
  uns32   targ_hist;    // 간접 분기 타겟 이력
  uns32   targ_index;
  uns8    target_bit_length;

  Flag    on_path_pred;
  List    cbrs_in_machine; // 비행 중인 조건 분기 목록
} Bp_Data;
```

**핵심 포인트**: BTB는 Scarab의 `Cache` 라이브러리를 재사용 (LRU 교체 정책, 파라미터로 entries/assoc 설정 가능).

### 3.2 Bp_Recovery_Info — 복구 체크포인트

```c
typedef struct Bp_Recovery_Info_struct {
  uns    proc_id;
  Counter recovery_cycle;        // 복구가 시작될 사이클
  Addr   recovery_fetch_addr;    // 복구 후 fetch할 주소
  Counter recovery_op_num;       // 복구를 유발한 op 번호
  Counter recovery_cf_type;
  Recovery_Info recovery_info;   // op별 체크포인트 (global_hist, CRS 상태 등)

  Counter redirect_cycle;        // BTB miss로 인한 리다이렉트 사이클
  Counter redirect_op_num;

  Flag   late_bp_recovery;       // Late BP 복구 여부
  Flag   late_bp_recovery_wrong; // Late BP가 틀렸는지 여부
} Bp_Recovery_Info;
```

`recovery_cycle == MAX_CTR`일 때 "복구 없음" 상태를 나타낸다 (sentinel 값).

---

## 4. Branch Predictor 인터페이스 (Bp)

### 4.1 예측기 등록 테이블 (bp_table.def)

```
ID            이름        구현체
─────────────────────────────────────────
GSHARE_BP     gshare      gshare.cc
HYBRIDGP_BP   hybridgp    hybridgp.cc
TAGESCL_BP    tagescl     tagescl.cc (TAGE-SC-L 64KB)
TAGESCL80_BP  tagescl80   tagescl.cc (동일 코드, 80KB 설정)
MTAGE_BP      mtage       mtage_unlimited.cc (CBP 어댑터)
TAGE64K_BP    tage64k     cbp_tagescl_64k.cc (CBP 어댑터) ← 기본값
```

BTB는 `GENERIC_BTB` 단일 구현 (LRU 캐시 기반).
IBTB (간접 분기)는 3가지: `TC_TAGLESS`, `TC_TAGGED`, `TC_HYBRID`.

### 4.2 7-함수 생명주기

각 예측기는 7개 함수를 구현해야 한다:

| 함수 | 호출 시점 | 목적 |
|------|----------|------|
| `init_func` | 시뮬레이션 시작 | 내부 상태 초기화 |
| `timestamp_func` | Fetch 시 | 예측기의 in-flight 체크포인트 기록 |
| `pred_func` | Fetch 시 | 방향 예측 반환 (0=NT, 1=T) |
| `spec_update_func` | Fetch 시 (예측 직후) | 투기적 전역 이력 업데이트 |
| `update_func` | Execute/Retire 시 | 결과 확정 후 예측기 테이블 업데이트 |
| `retire_func` | Retire 시 | Retire 단계에서만 해야 하는 업데이트 |
| `recover_func` | 오예측 감지 시 | 내부 상태를 체크포인트로 롤백 |

---

## 5. 브랜치 예측 메인 흐름 (bp.c)

### 5.1 bp_predict_op() — 예측 단계

```
bp_predict_op(bp_data, op, br_num, fetch_addr)
  │
  ├─ HBT 조회: op->oracle_info.hbt_pred_is_hard = hbt_is_hard_branch(pc)
  │
  ├─ Recovery_Info 초기화 (체크포인트 저장)
  │   └─ global_hist, targ_hist, CRS 상태, predict_cycle
  │
  ├─ timestamp_func(op) 호출 (예측기 체크포인트)
  │
  ├─ BTB 조회: bp_btb->pred_func(bp_data, op)
  │   ├─ Hit: pred_target = btb_entry, btb_miss = FALSE
  │   └─ Miss: pred_target = pc+size, btb_miss = TRUE
  │
  ├─ IBTB 조회 (CF_IBR / CF_ICALL만):
  │   └─ bp_ibtb->pred_func(bp_data, op)
  │
  ├─ CF 타입별 switch 처리 (CF_BR/CBR/CALL/IBR/ICALL/ICO/RET/SYS)
  │   └─ 방향 예측, pred_npc 설정, recover_at_exec/decode 설정
  │
  ├─ spec_update_func(op) — 전역 이력 투기적 업데이트
  │
  ├─ [PERFECT_TARGET / PERFECT_BP 옵션 처리]
  │
  ├─ bp_predict_op_evaluate(bp_data, op, pred_npc)
  │   └─ mispred = (pred != dir) && (pred_npc != npc)
  │   └─ misfetch = !mispred && (pred_npc != npc)
  │
  └─ TEA trigger: if (TEA_ENABLE && hbt_pred_is_hard && !off_path)
                    trigger_tea_thread(proc_id, pc, op_num, op)
```

### 5.2 복구 흐름

```
bp_recover_op(bp_data, cf_type, info)
  │
  ├─ global_hist 복구 (CBR: roll-in actual direction, others: restore)
  ├─ targ_hist 복구
  ├─ IBTB recover_func (CF_IBR/ICALL)
  ├─ bp->recover_func (예측기 내부 상태 롤백)
  ├─ late_bp->recover_func (있는 경우)
  └─ CRS 복구 (realistic: info 기반, 비realistic: tail_save 기반)
```

### 5.3 bp_retire_op()

```c
void bp_retire_op(Bp_Data* bp_data, Op* op) {
  bp_data->bp->retire_func(op);
  if (USE_LATE_BP)
    bp_data->late_bp->retire_func(op);
  hbt_update(op);   // ← HBT는 반드시 Retire 시에만 업데이트
}
```

---

## 6. 방향 예측기 구현체 상세 분석

### 6.1 GShare (gshare.cc)

**알고리즘**: 전역 이력 XOR PC → PHT 인덱스

```cpp
uns32 get_pht_index(Addr addr, uns32 hist) {
  const uns32 cooked_hist = hist >> (32 - HIST_LENGTH);
  const uns32 cooked_addr = (addr >> 2) & N_BIT_MASK(HIST_LENGTH);
  return cooked_hist ^ cooked_addr;
}
```

- **PHT**: `2^HIST_LENGTH` 엔트리, 각 엔트리는 `PHT_CTR_BITS`-bit saturating counter
- **기본값**: HIST_LENGTH=16 → 65536 엔트리 PHT
- **초기값**: weakly taken (`0x1 << (PHT_CTR_BITS - 1)`)
- **투기적 상태**: 전역 이력은 `bp_data->global_hist`에서 관리 (bp.c). GShare 자체는 `timestamp_func`, `recover_func`, `spec_update_func` 가 모두 no-op — 전역 이력 관리가 bp.c에 위임되어 있기 때문

**업데이트 조건**: `CF_CBR`만 (조건부 분기만 방향 예측 업데이트)

### 6.2 HybridGP (hybridgp.cc)

**알고리즘**: Local + Global + Perceptron 하이브리드 예측기

구조:
- `bht` (Branch History Table): per-PC local history (Cache 또는 Hash)
- `hybspht`: selector PHT (meta-predictor)
- `hybgpht`: global history PHT
- `hybppht`: perceptron-based PHT
- `filter`: 필터 테이블

**특징**:
- `INF_HYBRIDGP` 파라미터: true면 무한 BHT (Hash_Table 사용), false면 실제 캐시 크기 BHT
- `in_flight` 원형 버퍼를 통한 in-flight 브랜치 상태 체크포인트 관리
- `USE_FILTER`: 필터 테이블을 통한 예측 필터링 옵션
- `template_lib/utils.h`의 `Circular_Buffer` 활용

### 6.3 TAGE-SC-L (tagescl.cc) — 기본 예측기

**TAGE-SC-L (TAgged GEometric with Statistical Corrector and Loop predictor)**은 현대 고성능 CPU 예측기의 학술적 참조 구현체다.

구성 요소 (bp.param.def의 toggle 파라미터):
- **TAGE 베이스 예측기**: 2-bit counter 테이블 (base predictor)
- **TAGE 태그 테이블**: 여러 이력 길이(기하급수적으로 증가)를 가진 tagged 예측기들
- **SC (Statistical Corrector)**: TAGE 예측에 대한 통계적 보정기 (`TAGESCL64KB_SC` 파라미터)
- **Loop Predictor**: 루프 반복 패턴 감지 (`TAGESCL64KB_LOOP` 파라미터)

기본값: `BP_MECH = TAGE64K_BP` (cbp_tagescl_64k.cc의 CBP 어댑터 버전)

**spec_update**: TAGE는 복잡한 내부 상태(folded history 등)를 가지므로 `timestamp_func`에서 예측 시점의 상태를 기록하고, `recover_func`에서 롤백한다. 이는 GShare와 달리 TAGE 자체가 투기적 상태를 관리함을 의미한다.

**SPEC_LEVEL 파라미터** (`bp.param.def:138`):
```
// 0: baseline
// 1: take checkpoint
// 2: off-path spec_update
// 3: off-path prediction ← 기본값
// 4: update N at exec stage
```
`SPEC_LEVEL > 0`이면 반드시 `BP_MECH == TAGE64K_BP`이어야 한다 (assert).

### 6.4 CBP 어댑터 (cbp_tagescl_64k.cc, mtage_unlimited.cc)

CBP (Championship Branch Prediction) 2016 대회 코드를 Scarab 인터페이스에 연결하는 어댑터. `cbp_to_scarab.cc`가 CBP API → Scarab API 변환을 수행한다.

---

## 7. 타겟 예측 메커니즘 (BTB / CRS / IBTB)

### 7.1 BTB (Branch Target Buffer) — bp_targ_mech.c

**구조**: Scarab `Cache` 라이브러리 기반, LRU 교체 정책
- 기본: 4K entries, 4-way associative, line size = 1 (각 엔트리 = `Addr` 하나)
- BTB miss 시: fall-through (pc+size)를 타겟으로 사용

**업데이트 정책**:
```c
void bp_btb_gen_update(Bp_Data* bp_data, Op* op) {
  if (BTB_OFF_PATH_WRITES || !op->off_path) {
    // 타겟이 변경된 경우(JIT/indirect) BTB hit이어도 업데이트
    btb_line = cache_access() or cache_insert();
    *btb_line = op->oracle_info.target;
  }
}
```

**BTB miss 처리 흐름** (bp_predict_op에서):
- CBR: BTB miss인 경우 NOT_TAKEN으로 예측 (4가지 경우 분기 처리)
- BR/CALL: BTB miss → recover_at_decode = TRUE
- IBR/ICALL/RET: BTB miss → recover_at_exec = TRUE

### 7.2 CRS (Call-Return Stack)

**두 가지 모드** (`CRS_REALISTIC` 파라미터):
- **모드 0 (비현실적)**: on/off path 분리 저장 (flag 비트로 선택)
  - on-path 상태는 `tail_save`, `depth_save`로 체크포인트
  - 복구: `tail/depth`를 saved 값으로 복원
- **모드 1 (현실적 — next/tos 방식)**: 추천 모드
  - `tos` (top-of-stack)과 `next` (쓸 위치) 포인터 기반
  - 각 push/pop이 `recovery_info`에 CRS 상태 저장
  - 복구: `Recovery_Info`의 `crs_next, crs_tos, crs_depth` 복원
- **모드 2 (순수 스택)**: 잘 작동하지 않는다고 주석에 명시

**CRS Clobber**: 스택이 꽉 차면 head를 밀어내고 덮어씀 (`CRS_CLOBBER` 통계 집계).

**BP_HASH_TOS 파라미터**: TOS 주소를 방향 예측 인덱스 계산에 XOR로 혼합.

### 7.3 IBTB (Indirect Branch Target Buffer)

간접 분기(CF_IBR, CF_ICALL)의 타겟 주소 예측.

**세 가지 구현**:

#### TC_TAGLESS (IBTB_MECH=0)
- **직접 배열**: `tc_tagless[2^IBTB_HIST_LENGTH]`
- **인덱스 계산**: `cooked_hist XOR cooked_addr` (전역 이력 + PC 하위 비트)
- **USE_PAT_HIST**: true면 global_hist 재사용, false면 별도 targ_hist 관리

#### TC_TAGGED (기본값: IBTB_MECH=1)
- **태그된 캐시**: `Cache tc_tagged` (TC_ENTRIES, TC_ASSOC)
- 태그 미스 시 `target = 0` 반환 (예측 불가)
- LRU 캐시 기반으로 태그 충돌 처리

#### TC_HYBRID (IBTB_MECH=2)
- **메타 예측기**: `tc_selector[2^IBTB_HIST_LENGTH]` (4-state: tagless_strong, tagless_weak, tagged_weak, tagged_strong)
- 예측: selector 값에 따라 tagged 또는 tagless 선택
- 업데이트: 맞으면 현재 예측기 방향으로 selector 업데이트, 틀리면 반대 방향

---

## 8. 신뢰도 추정기 (Branch Confidence)

`ENABLE_BP_CONF` 파라미터로 활성화. 세 가지 메커니즘:

### 8.1 Counter 방식 (CONF_MECH=2, COUNTER_CONF)

```c
// 인덱스: cooked_hist XOR cooked_addr
// 예측: counter가 최대값이면 LOW confidence, 아니면 HIGH
pred_conf = (entry == N_BIT_MASK(BPC_CTR_BITS)) ? FALSE : TRUE;
// 업데이트: mispred → SAT_INC, correct → SAT_DEC
```

### 8.2 OnPath 방식 (CONF_MECH=0, ONPATH_CONF)

```c
// In-flight 브랜치의 예측 신뢰도 비트 벡터를 유지
// 비트 벡터에 '0'이 있으면 off-path 가능성 → pred_onpath = FALSE
// 모두 '1'이면 on-path로 판단
// count > 128이면 무조건 LOW confidence (LONG_OVWT)
```

`opc_table[OPC_SIZE]`에 비행 중인 브랜치들의 상태 저장.

### 8.3 Perceptron 방식 (CONF_MECH=1, PERCEPTRON_CONF)

Akkary et al. (HPCA 2004) 논문 기반.
```
index = pc % CONF_PERCEPTRON_ENTRIES
output = w[0] + Σ(w[i] * x_i)  // x_i = ±1 (history bits)
pred_conf = (output < threshold) ? HIGH : LOW
```

학습: 출력이 threshold 범위 내이거나 confidence-prediction 불일치 시 가중치 업데이트.
다양한 변형 파라미터:
- `PERCEPTRON_CONF_TRAIN_HIS`: 이력 방향으로 학습
- `PERCEPTPON_CONF_TRAIN_CONF`: 신뢰도로 학습
- `PERCEPTRON_CONF_HIS_BOTH`: mispred 이력도 혼합

---

## 9. HBT (Hard Branch Table) — TEA 연동 핵심

### 9.1 설계 목적

TEA (Timely, Efficient, Accurate) 기능이 어떤 브랜치를 H2P (Hard-to-Predict)로 지정할지 결정하기 위한 테이블. 논문 Section IV-B 기준.

### 9.2 자료구조

```c
#define HBT_SIZE       1024    // 테이블 크기
#define HBT_CTR_BITS   3       // 3-bit saturating counter (최대 7)
#define HBT_CTR_MAX    7
#define HBT_H2P_THRESHOLD 1   // counter > 1이면 H2P (즉, 2이상이면 H2P)

typedef struct {
  uns64  tag;       // pc / HBT_SIZE
  uns32  counter;   // 3-bit saturation counter
} HbtEntry;

HbtEntry hbt_table[HBT_SIZE];
uns64    retired_branch_count;
```

**인덱싱**: Direct-mapped (1024-entry), `index = pc % 1024`, `tag = pc / 1024`

### 9.3 업데이트 로직 (hbt_update)

```
hbt_update(op) → Retire 시 호출
  │
  ├─ pc = op->inst_info->addr
  ├─ mispred = op->oracle_info.mispred | op->oracle_info.misfetch
  ├─ index = pc % HBT_SIZE,  tag = pc / HBT_SIZE
  │
  ├─ 태그 불일치인 경우:
  │   ├─ counter == 0: 새 브랜치로 overwrite (counter = 0)
  │   └─ counter > 0: 다른 H2P 브랜치가 점유 중 → skip (업데이트 안 함)
  │
  ├─ 예측 실패 시: counter = SAT_INC(counter, 7)
  ├─ 예측 성공 시: 아무것도 안 함 (핵심 로직!)
  │
  └─ 50K 브랜치마다 전체 테이블 -1 감소 (hbt_periodic_decrement)
```

**중요한 비대칭성**: 예측 실패만 카운터를 증가시키고, 성공 시에는 감소시키지 않는다. 대신 50K 단위 주기적 감소로 aging을 구현한다.

### 9.4 H2P 판정 (hbt_is_hard_branch)

```c
Flag hbt_is_hard_branch(Addr pc) {
  uns32 index = pc % HBT_SIZE;
  uns64 tag   = pc / HBT_SIZE;
  if (entry->tag == tag && entry->counter > HBT_H2P_THRESHOLD)
    return TRUE;  // counter >= 2이면 H2P
  return FALSE;
}
```

### 9.5 bp.c와의 통합

```c
// bp_predict_op() 앞부분:
op->oracle_info.hbt_pred_is_hard = hbt_is_hard_branch(pc);
op->oracle_info.hbt_misp_counter = hbt_get_counter(pc);

// bp_predict_op_evaluate() 이후:
if (TEA_ENABLE && op->oracle_info.hbt_pred_is_hard && !op->off_path) {
    trigger_tea_thread(proc_id, pc, op_num, op);
}

// bp_retire_op() 마지막:
hbt_update(op);  // HBT는 반드시 Retire에서만 업데이트
```

**TEA 트리거 위치 선택 이유** (bp.c 주석):
> "Must be called AFTER bp_predict_op_evaluate() so that oracle_info.mispred is computed. Off-path branches are excluded: they have no BP checkpoint (spec_update skips TakeCheckpoint for off_path ops), and they will be flushed anyway."

---

## 10. Late Branch Predictor (듀얼 BP)

### 10.1 개념

`LATE_BP_MECH` 파라미터로 두 번째 예측기 활성화. 멀티사이클 지연으로 더 정확한 예측 제공.

```c
USE_LATE_BP = (LATE_BP_MECH != NUM_BP);
// 기본값: LATE_BP_MECH = NUM_BP → 비활성
```

**지연**: `LATE_BP_LATENCY` 파라미터 (기본 5 사이클)

### 10.2 Late BP 흐름

```
bp_predict_op():
  op->oracle_info.late_pred = bp_data->late_bp->pred_func(op)
  // late_pred_npc 계산

bp_predict_op_evaluate():
  op->oracle_info.late_mispred = (late_pred != dir) && (late_pred_npc != npc)
  op->oracle_info.late_misfetch = !late_mispred && late_pred_npc != npc
```

`bp_sched_recovery`에서 `late_bp_recovery=TRUE`이면 `recovery_cycle += LATE_BP_LATENCY`.

`force_offpath` 플래그: Late BP가 틀린 경우, `late_pred_npc`로 복구하되 `recovery_force_offpath = TRUE` 설정.

---

## 11. 복구 메커니즘 (Recovery)

### 11.1 두 종류의 복구

| 종류 | 트리거 | 함수 |
|------|--------|------|
| **Recovery** | 오예측 (방향/타겟 틀림) | `bp_sched_recovery()` |
| **Redirect** | BTB miss (타겟 미확인) | `bp_sched_redirect()` |

### 11.2 bp_sched_recovery 로직

```c
// 이미 더 이른 복구가 예약된 경우 무시 (younger op의 복구는 older op의 복구 때 자동 해소)
if (recovery_cycle == MAX_CTR || op->op_num <= recovery_op_num) {
  recovery_cycle = cycle + latency + penalty;
  // latency: late_bp_recovery이면 LATE_BP_LATENCY, 아니면 1
  recovery_fetch_addr = op->oracle_info.npc;  // 정답 주소
  // ... 체크포인트 정보 저장
}
```

### 11.3 op별 체크포인트 (Recovery_Info)

`bp_predict_op`에서 매 CF 명령마다 저장:
```c
op->recovery_info.pred_global_hist = bp_data->global_hist;
op->recovery_info.targ_hist = bp_data->targ_hist;
op->recovery_info.crs_next = ...;
op->recovery_info.crs_tos = ...;
op->recovery_info.crs_depth = ...;
op->recovery_info.predict_cycle = cycle_count;
```

`bp_recover_op`에서 이 체크포인트로 모든 상태를 복원.

---

## 12. 통계 시스템 (bp.stat.def)

약 200개의 통계 이벤트가 정의되어 있다. 주요 카테고리:

| 카테고리 | 대표 통계 | 설명 |
|---------|---------|------|
| BTB | `BTB_ON_PATH_HIT/MISS` | On/off path BTB 적중률 |
| BP 방향 | `BP_ON_PATH_CORRECT/MISPREDICT/MISFETCH` | 방향 예측 정확도 |
| Late BP | `LATE_BP_ON_PATH_*` | Late BP 성능 |
| CRS | `CRS_MISS/HIT_ON/OFF_PATH` | RAS 적중률, CRS_CLOBBER |
| IBTB | `TARG_ON_PATH_MISS/HIT` | 간접 분기 타겟 적중률 |
| CBR | `CBR_RECOVER_MISPREDICT/MISFETCH/BTB_MISS_*` | 조건 분기 복구 원인 |
| Confidence | `BP_ON_PATH_CONF_PVP/PVN` | 신뢰도 예측기 정확도 |
| TAGE-SC-L | `TAGESCL_COMP_*` | TAGE의 각 컴포넌트 기여도 |
| 복구 지연 | `PERFORMED_RECOVERY_LAT` | 복구 지연 사이클 합계 |
| 개당 통계 | `CBR_ON_PATH_MISPREDICT_PER1000INST` | 1000 명령당 오예측 수 |

`BP_MISP_PENALTY`: `exec_cycle - issue_cycle` 누적으로 실제 성능 영향 측정 가능.

---

## 13. 파라미터 시스템 (bp.param.def)

약 40개 파라미터, 주요 분류:

### 완벽 예측기 오라클 (디버깅용)
| 파라미터 | 기본값 | 효과 |
|---------|-------|------|
| `PERFECT_BP` | FALSE | 방향+타겟 완벽 예측 |
| `PERFECT_BTB` | FALSE | BTB 완벽 |
| `PERFECT_IBP` | FALSE | 간접 분기 완벽 |
| `PERFECT_CRS` | FALSE | CRS 완벽 |
| `PERFECT_TARGET` | FALSE | 타겟만 완벽 (방향은 실제 예측기) |

### 방향 예측기 파라미터
| 파라미터 | 기본값 | 의미 |
|---------|-------|-----|
| `BP_MECH` | TAGE64K_BP | 메인 예측기 선택 |
| `LATE_BP_MECH` | NUM_BP (없음) | Late BP 선택 |
| `LATE_BP_LATENCY` | 5 | Late BP 지연 사이클 |
| `HIST_LENGTH` | 16 | GShare/HybridGP 이력 길이 |

### BTB/CRS/IBTB 파라미터
| 파라미터 | 기본값 |
|---------|-------|
| `BTB_ENTRIES` | 4096 |
| `BTB_ASSOC` | 4 |
| `CRS_ENTRIES` | 32 |
| `CRS_REALISTIC` | 0 |
| `IBTB_MECH` | 1 (TC_TAGGED) |
| `TC_ENTRIES` | 4096 |

### 신뢰도 파라미터
| 파라미터 | 기본값 |
|---------|-------|
| `ENABLE_BP_CONF` | FALSE |
| `CONF_MECH` | 0 (ONPATH_CONF) |

---

## 14. CBP 통합 어댑터 (cbp_to_scarab)

### 14.1 설계 목적

CBP (Championship Branch Prediction) 2016 대회 코드를 Scarab에 통합할 때 인터페이스 변환을 최소화하기 위한 어댑터 레이어.

### 14.2 OpType 변환

```c
OpType scarab_to_cbp_optype(Cf_Type cf_type) {
  CF_BR    → OPTYPE_JMP_DIRECT_UNCOND
  CF_CALL  → OPTYPE_CALL_DIRECT_UNCOND
  CF_CBR   → OPTYPE_JMP_DIRECT_COND
  CF_IBR   → OPTYPE_JMP_INDIRECT_UNCOND
  CF_ICALL → OPTYPE_CALL_INDIRECT_UNCOND
  CF_RET   → OPTYPE_RET_UNCOND
}
```

### 14.3 BpOffPredType 열거형

```c
typedef enum {
  BP_PRED_ON,                              // on-path만 BP
  BP_PRED_ON_SPEC_UPDATE_S_ON_N_ON,       // + 체크포인트
  BP_PRED_ON_SPEC_UPDATE_S_ONOFF_N_ON,    // + off-path spec_update
  BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_N_ON, // + on/off-path 모두 BP
  BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON, // + exec에서 비투기적 업데이트
} BpOffPredType;
```

이 열거형은 SPEC_LEVEL 파라미터와 연동된다.

### 14.4 매크로 자동화

```c
// cbp_table.def에 추가하면 자동으로 함수 선언/dispatch table 등록
DEF_CBP("mtage",   MTAGE)
DEF_CBP("tage64k", TAGE64K)

// → bp_MTAGE_init, bp_MTAGE_pred, ... 함수들 자동 생성/등록
```

---

## 15. TEA와의 통합 지점

BP 서브시스템과 TEA의 연동은 세 지점에서 발생한다:

### 15.1 HBT — H2P 브랜치 식별

```
[Fetch] bp_predict_op():
  hbt_pred_is_hard = hbt_is_hard_branch(pc)  ← H2P 여부 저장

[Predict Evaluate] bp_predict_op_evaluate():
  mispred 계산 완료

[After Evaluate] bp_predict_op():
  if (TEA_ENABLE && hbt_pred_is_hard && !off_path)
    trigger_tea_thread(proc_id, pc, op_num, op)  ← TEA 스레드 트리거

[Retire] bp_retire_op():
  hbt_update(op)  ← H2P 카운터 업데이트
```

### 15.2 오프패스 브랜치 제외 이유

TEA 트리거는 반드시 on-path 브랜치에서만 실행된다:
- off-path 브랜치는 BP 체크포인트(`TakeCheckpoint`)를 가지지 않는다 (spec_update가 off-path를 skip)
- 어차피 flush 될 것이므로 TEA를 트리거하는 것은 낭비

### 15.3 bp.c → tea_thread.h 의존성

```c
#include "tea/tea_thread.h"  // trigger_tea_thread 선언
```

BP 모듈이 TEA를 직접 호출하는 단방향 의존성. TEA가 BP를 알 필요는 없다.

---

## 16. CF 타입별 예측 처리 흐름 요약

| CF 타입 | 방향 예측 | 타겟 소스 | BTB Miss 처리 | 복구 지점 |
|---------|---------|---------|-------------|---------|
| CF_BR | 항상 TAKEN | BTB | BTB miss → decode 복구 | Decode |
| CF_CBR | bp->pred_func | BTB | 4가지 경우 분기 처리 | Exec (방향 오예측) / Decode (타겟 오예측) |
| CF_CALL | 항상 TAKEN | BTB | BTB miss → decode 복구 | Decode |
| CF_IBR | 항상 TAKEN | IBTB > BTB | BTB+IBTB miss → exec 복구 | Exec |
| CF_ICALL | 항상 TAKEN | IBTB > BTB | BTB+IBTB miss → exec 복구 | Exec |
| CF_ICO | 항상 TAKEN | CRS pop | 항상 예측 | Exec (타겟 불일치) |
| CF_RET | 항상 TAKEN | CRS pop | CRS underflow → exec 복구 | Exec |
| CF_SYS | 항상 TAKEN | Oracle NPC | 복구 없음 | Decode (항상 flush) |

**mispred vs misfetch**:
- **mispred**: 방향 예측 오류 (`pred != dir`) AND `pred_npc != npc`
- **misfetch**: 방향은 맞지만 타겟 주소가 틀린 경우 (`pred_npc != npc`)

특수 케이스: `pc_plus_offset == oracle_info.target`이면 BTB miss이더라도 타겟이 맞으므로 복구 불필요.

---

## 17. 설계상 주목할 만한 패턴 및 트레이드오프

### 17.1 전역 이력 관리의 이중성

- **공유 이력** (`bp_data->global_hist`): BP 서브시스템이 관리, IBTB도 USE_PAT_HIST 시 이를 재사용
- **TAGE 내부 이력**: TAGE는 folded history 등 자체 내부 이력을 별도 관리 (checkpointing 필요)

이 두 이력의 동기화는 `spec_update_func` → `timestamp_func` → `recover_func` 체인으로 보장된다.

### 17.2 BTB miss와 방향 예측의 분리

BTB miss인 상태에서도 방향 예측기는 작동한다. 이는:
- NOT_TAKEN 예측이면: fall-through를 타겟으로 실행 (자연스럽게 correct 가능)
- TAKEN 예측이면: BTB에서 타겟을 모르므로 decode에서 flush 필요

이 설계는 "BTB miss = 항상 flush"라는 단순화를 피해 성능을 개선한다.

### 17.3 HBT의 단방향 카운터 설계

```
mispred → counter++
correct → 아무것도 안 함
50K branches → 전체 -1 (aging)
```

이 비대칭 설계의 의미:
- 한 번이라도 오예측하면 카운터가 올라간다
- 아무리 많이 맞춰도 카운터가 내려가지 않는다 (aging으로만 감소)
- 결과적으로 "최근 50K 사이클 내에 2번 이상 오예측한 브랜치" = H2P

이는 TEA의 목적 (H2P 브랜치를 빨리 식별하여 프리컴퓨테이션)에 적합한 aggressive한 탐지 방식이다.

### 17.4 Direct-mapped HBT의 충돌 처리

1024-entry direct-mapped 구조에서 태그 충돌 시:
- `counter == 0`: 기존 브랜치가 더 이상 H2P가 아닌 것으로 보고 overwrite
- `counter > 0`: 기존 H2P 브랜치가 테이블을 점유 중이므로 skip

이는 "H2P 브랜치가 H2P가 아닌 브랜치에 의해 밀려나지 않는다"는 보수적 설계다.

### 17.5 온패스/오프패스 이중 CRS

CRS 모드 0의 독특한 설계:
```c
Crs_Entry* ent = &bp_data->crs.entries[bp_data->crs.tail << 1 | flag];
// flag = op->off_path
```
각 tail 위치에 on-path 버전(flag=0)과 off-path 버전(flag=1) 두 슬롯 유지. 복구 시 tail_save로 되돌리면 off-path 엔트리는 자연히 무효화.

---

## 결론

Scarab의 `src/bp/` 서브시스템은 다음과 같은 특성을 가진다:

1. **플러그인 가능한 예측기 아키텍처**: 함수 포인터 vtable 패턴으로 런타임 예측기 교체 지원
2. **TEA 연구 기능 통합**: HBT가 H2P 브랜치를 식별하여 TEA 스레드 트리거
3. **현대 최신 예측기 지원**: TAGE-SC-L이 기본값으로, CBP 어댑터를 통한 확장 가능
4. **세밀한 복구 모델**: 방향 오예측(exec)과 타겟 오예측(decode/exec)을 별도 처리
5. **멀티 레벨 타겟 예측**: BTB + IBTB (태그드/태그리스/하이브리드) + CRS (현실적/비현실적 모드)
6. **선택적 신뢰도 추정**: Counter/OnPath/Perceptron 중 선택
7. **Late BP 지원**: 멀티사이클 지연 2차 예측기 아키텍처
8. **완벽 예측기 옵션**: 디버깅/연구용 oracle predictor 플래그들

총 파일: 26개 | 총 코드: ~5,000+ 라인 | 예측기 구현: 6종 | 파라미터: ~40개 | 통계: ~200개
