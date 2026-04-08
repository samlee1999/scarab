# Scarab 시뮬레이터 `src/` 심층 분석 보고서

작성일: 2026-03-08
분석 대상: `/home/lee/scarab/src/` 전체 디렉토리

---

## 목차

1. [전체 아키텍처 개요](#1-전체-아키텍처-개요)
2. [디렉토리 구조](#2-디렉토리-구조)
3. [시뮬레이터 진입점 및 루프 (main / sim / cmp_model)](#3-시뮬레이터-진입점-및-루프)
4. [핵심 데이터 구조](#4-핵심-데이터-구조)
5. [파이프라인 스테이지 상세 분석](#5-파이프라인-스테이지-상세-분석)
6. [레지스터 리네이밍 (map_rename)](#6-레지스터-리네이밍-map_rename)
7. [메모리 서브시스템](#7-메모리-서브시스템)
8. [Decoupled Frontend](#8-decoupled-frontend)
9. [Uop Cache](#9-uop-cache)
10. [Fill Buffer & Dependency Chain Cache](#10-fill-buffer--dependency-chain-cache)
11. [복구 메커니즘](#11-복구-메커니즘)
12. [파라미터 시스템](#12-파라미터-시스템)
13. [통계 시스템](#13-통계-시스템)
14. [로깅 서브시스템 (src/log/)](#14-로깅-서브시스템)
15. [공통 라이브러리 (globals / libs)](#15-공통-라이브러리-globals--libs)
16. [ISA 레이어](#16-isa-레이어)
17. [Golden Cove 설정 분석](#17-golden-cove-설정-분석)
18. [설계상 주목할 패턴](#18-설계상-주목할-패턴)

---

## 1. 전체 아키텍처 개요

Scarab은 HPS (High Performance Simulation) / SAFARI 연구 그룹이 개발한 **사이클-어큐레이트, 멀티코어 Out-of-Order CPU 시뮬레이터**다. 현재 UCSC Litz Lab이 유지·개발 중이다.

### 시뮬레이터 모델 개요

```
┌──────────────────────────────────────────────────────────┐
│                     Scarab Simulator                      │
│                                                           │
│  ┌────────────┐   ┌──────────────────────────────────┐   │
│  │ Decoupled  │   │      OoO Backend (per core)      │   │
│  │    FE      │   │  Map/Rename → ROB+RS → Execute   │   │
│  │  (FTQ/BP)  │   │           → DCache               │   │
│  └─────┬──────┘   └──────────────────────────────────┘   │
│        ▼                                                  │
│  ┌──────────────┐  ┌──────────────────────────────────┐  │
│  │ ICache /     │  │       Memory Subsystem           │  │
│  │ Uop Cache    │  │  L1 DCache → MLC → LLC → DRAM   │  │
│  └─────┬────────┘  └──────────────────────────────────┘  │
│        ▼                                                  │
│  ┌──────────────┐                                         │
│  │ Decode →     │                                         │
│  │ IDQ → Map    │                                         │
│  └──────────────┘                                         │
└──────────────────────────────────────────────────────────┘
```

**실행 모드**:
- **WARMUP_MODE**: 캐시 워밍업, 통계 미수집
- **SIMULATION_MODE**: 정밀 통계 수집

---

## 2. 디렉토리 구조

```
src/
├── main.c                    # 시뮬레이터 진입점
├── sim.c / sim.h             # 시뮬레이션 루프, 종료 조건
├── cmp_model.c / .h          # 최상위 멀티코어 모델, 사이클 오케스트레이션
├── model.h / model_table.def # 모델 선택 테이블
│
├── [파이프라인 스테이지]
│   ├── icache_stage.c / .h   # Fetch: I-Cache 접근
│   ├── uop_cache.cc / .h     # Fetch: Uop Cache
│   ├── decode_stage.c / .h   # Decode: 명령어 디코딩
│   ├── idq_stage.cc / .h     # IDQ: Instruction Decode Queue
│   ├── uop_queue_stage.cc/.h # Uop 큐 (uop cache ↔ IDQ 중간)
│   ├── map_stage.c / .h      # Map: 레지스터 리네이밍 파이프라인
│   ├── map_rename.c / .h     # 물리 레지스터 할당 로직
│   ├── node_stage.c / .h     # Node/ROB: Reservation Station
│   ├── node_issue_queue.cc/.h# Issue Queue 스케줄링
│   ├── exec_stage.c / .h     # Execute: FU 실행
│   ├── exec_ports.c / .h     # FU 포트 설정 (rs_sizes, rs_connections)
│   ├── dcache_stage.c / .h   # DCache: 데이터 캐시 접근
│   └── lsq.cc / .h           # Load-Store Queue
│
├── [핵심 자료구조]
│   ├── op.h                  # Op struct (마이크로-op 표현)
│   ├── op_info.h             # Oracle / Engine Info 서브구조
│   ├── inst_info.h           # 정적 명령어 정보
│   ├── table_info.h          # 명령어 타입 테이블
│   ├── stage_data.h          # 스테이지 간 통신 버스
│   ├── op_pool.c / .h        # Op 메모리 풀
│   ├── thread.c / .h         # 스레드 상태 (TD)
│   └── ft.cc / .h / ft_info.h # Fetch Target
│
├── fill_buffer.c / .h            # Retire된 Op 원형 버퍼
├── dependency_chain_cache.c / .h # Block Cache (의존성 체인 저장)
├── on_off_path_cache.c / .h      # On/off path 캐시
│
├── decoupled_frontend.cc / .h # Decoupled Branch Prediction Frontend
├── freq.c / .h                # 주파수/DVFS 지원
│
├── [파라미터/통계]
│   ├── core.param.def         # 코어 파라미터 (~350개)
│   ├── core.stat.def          # 코어 통계 이벤트
│   ├── stat_files.def         # 통계 파일 목록
│   ├── general.param.def      # 일반 파라미터
│   ├── param_parser.c / .h    # CLI 파라미터 파서
│   ├── statistics.c / .h      # 통계 수집 엔진
│   └── stat_mon.c / .h        # 통계 모니터링
│
├── [로깅]
│   └── log/                   # 5가지 로그 모듈
│
├── [서브시스템]
│   ├── bp/                    # 브랜치 예측 (별도 문서)
│   ├── memory/                # 메모리 계층 (L1/MLC/LLC/DRAM)
│   ├── prefetcher/            # 프리패처 (stream, FDIP, EIP, D_JOLT, FNL+MMA)
│   ├── frontend/              # PIN 트레이스 프론트엔드
│   ├── isa/                   # ISA 매핑 (x86)
│   ├── globals/               # 전역 타입/유틸
│   ├── libs/                  # 범용 라이브러리
│   ├── debug/                 # 디버그 파라미터/매크로
│   ├── dvfs/                  # Dynamic Voltage/Frequency Scaling
│   └── confidence/            # 신뢰도 관련
│
├── PARAMS.golden_cove         # Intel Golden Cove (Alder Lake-P P-core) 설정
├── PARAMS.sunny_cove          # Intel Sunny Cove (Ice Lake) 설정
├── PARAMS.kaby_lake           # Intel Kaby Lake 설정
└── PARAMS.cortex_a76 / .m55  # ARM 설정들
```

---

## 3. 시뮬레이터 진입점 및 루프

### 3.1 main.c → sim.c → cmp_model.c 호출 체인

```
main()
  └─ sim_run()                           # sim.c
       ├─ sim_init()                     # 파라미터 로드, model 선택
       │   └─ cmp_init(WARMUP_MODE)      # cmp_model.c:97
       │       ├─ freq_init()
       │       ├─ cmp_init_cmp_model()
       │       ├─ for each core:
       │       │   ├─ init_uop_cache_stage()
       │       │   ├─ init_icache_stage()
       │       │   ├─ init_decode_stage()
       │       │   ├─ init_uop_queue_stage()
       │       │   ├─ init_idq_stage()
       │       │   ├─ init_map_stage()
       │       │   ├─ init_node_stage()
       │       │   ├─ init_lsq()
       │       │   ├─ init_exec_stage()
       │       │   ├─ init_exec_ports()
       │       │   ├─ init_dcache_stage()
       │       │   ├─ init_fill_buffer()
       │       │   ├─ init_dependency_chain_cache()
       │       │   ├─ init_on_off_path_cache()
       │       │   ├─ init_bp_recovery_info()
       │       │   ├─ init_bp_data()
       │       │   ├─ init_decoupled_fe()
       │       │   ├─ init_fdip()
       │       │   ├─ init_eip()
       │       │   ├─ init_djolt()
       │       │   └─ init_fnlmma()
       │       └─ init_memory()
       │
       └─ while (sim_active):
           └─ cmp_cycle()               # cmp_model.c:209
               ├─ cmp_istreams()        # 복구/redirect 체크
               ├─ update_memory()       # 메모리 계층 업데이트
               └─ cmp_cores()          # 각 코어 파이프라인 스텝
```

### 3.2 cmp_cycle() — 메인 사이클 루프

`cmp_cycle()` 내에서 **역순 파이프라인** 업데이트가 실행된다 (하류→상류 순서, 스톨 전파를 위해):

```c
// cmp_model.c:247-294
void cmp_cores(void) {
  for (uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    // --- 백엔드 (역순) ---
    update_dcache_stage(&exec->sd);          // 1. DCache
    update_exec_stage(&node->sd);            // 2. Execute
    update_node_stage(map->last_sd);         // 3. Node/ROB
    update_map_stage(idq_stage_get_stage_data());    // 4. Map
    // --- IDQ (브리지) ---
    update_idq_stage(dec->last_sd, &uc->sd, ...); // 5. IDQ
    update_uop_queue_stage(&uc->sd);         // 6. Uop Queue
    // --- 프론트엔드 ---
    update_decode_stage(&ic->sd);            // 7. Decode
    update_icache_stage();                   // 8. ICache (FTQ 기반)
    // --- 디커플드 프론트엔드 ---
    update_decoupled_fe();                   // 9. Branch Pred
    update_fdip();                           // 10. FDIP prefetcher
    update_eip();                            // 11. EIP prefetcher
  }
}
```

**역순 실행의 이유**: 이전 사이클에 실행된 결과를 받기 전에 상류 스테이지가 새 데이터를 보내지 않도록 함. 스톨 신호가 자동으로 상류로 전파된다.

---

## 4. 핵심 데이터 구조

### 4.1 Op 구조체 — 파이프라인의 기본 단위 (op.h)

`Op`는 하나의 마이크로-op을 표현하며, 파이프라인 전 단계를 통과하는 동안 상태가 갱신된다.

```c
struct Op_struct {
  // === 식별자 ===
  uns    proc_id;            // 코어 ID
  Counter op_num;            // 복구 시 리셋되는 순서 번호
  Counter unique_num;        // 복구 후에도 고유한 전체 순서 번호
  uns64  inst_uid;           // PIN이 부여하는 마크로 명령어 고유 ID

  // === 정보 포인터 ===
  Table_Info*  table_info;   // 명령어 타입 (CF 종류, 메모리 여부 등)
  Inst_Info*   inst_info;    // 정적 명령어 (PC, 크기 등)
  Op_Info      oracle_info;  // 오라클이 아는 실행 결과 (실제 방향, NPC 등)
  Op_Info      engine_info;  // 엔진이 예측한 결과

  // === 상태 ===
  Op_State state;            // FETCHED→IN_ROB→IN_RS→READY→SCHEDULED→DONE

  // === 타이밍 ===
  Counter fetch_cycle;       // Fetch된 사이클
  Counter bp_cycle;          // BP 접근 사이클
  Counter map_cycle;         // Map 완료 사이클
  Counter issue_cycle;       // ROB 진입 사이클
  Counter rdy_cycle;         // 소스 준비 완료 사이클
  Counter sched_cycle;       // FU 스케줄 사이클
  Counter exec_cycle;        // 실행 완료 사이클
  Counter dcache_cycle;      // DCache 접근 사이클
  Counter done_cycle;        // Retire 가능 사이클
  Counter retire_cycle;      // 실제 Retire 사이클

  // === 경로 정보 ===
  Flag off_path;             // 오예측 경로의 Op인가?
  Flag conf_off_path;        // 신뢰도 기반 off-path 판단
  Recovery_Info recovery_info; // 오예측 복구 체크포인트

  // === 스케줄러 ===
  uns fu_num;                // 사용할 FU 번호
  Counter node_id;           // ROB 내 위치
  Counter rs_id;             // 어느 RS에 배치됐는가
  Counter rs_entry_id;       // RS 내 엔트리 번호
  Counter chkpt_num;         // 체크포인트 ID
  struct Op_struct* next_rdy; // Ready List 링크드 리스트 포인터
  Flag in_rdy_list;          // Ready List에 있는가
  Flag macro_fused;          // 매크로 fusion 여부
  Flag move_eliminated;      // Move elimination 여부

  // === 의존성 ===
  uns srcs_not_rdy_vector;              // 미준비 소스 비트벡터
  Wake_Up_Entry* wake_up_head;          // 이 Op에 의존하는 Op 목록 헤드
  Wake_Up_Entry* wake_up_tail;
  uns wake_up_count;
  Counter wake_cycle;                   // 결과 브로드캐스트 사이클

  // === 레지스터 리네이밍 ===
  int src_reg_id[MAX_SRCS][REG_TABLE_TYPE_NUM];       // 소스 물리 레지스터
  int dst_reg_id[MAX_DESTS][REG_TABLE_TYPE_NUM];      // 대상 물리 레지스터
  int prev_dst_reg_id[MAX_DESTS][REG_TABLE_TYPE_NUM]; // 이전 매핑 (복구용)

  // === Uop Cache ===
  Flag fetched_from_uop_cache;
};
```

**Op_State 전이**:
```
FETCHED → IN_ROB (dispatch) → IN_RS (RS 배치)
       → SLEEP (pipelined schedule: 다음 사이클 wake-up)
       → WAIT_FWD (forwarding 대기)
       → LOW_PRIORITY (낮은 우선순위 대기)
       → READY (소스 준비됨)
       → TENTATIVE (스케줄됐지만 실패 가능, 재스케줄 대상)
       → SCHEDULED (FU 배정 확정)
       → [MISS/WAIT_DCACHE/WAIT_MEM] → DONE
```

### 4.2 Stage_Data — 스테이지 간 통신 버스 (stage_data.h)

```c
typedef struct Stage_Data_struct {
  char*  name;          // 스테이지 이름 (디버깅용)
  int    op_count;      // 현재 담긴 Op 수
  int    max_op_count;  // 최대 대역폭 (보통 ISSUE_WIDTH)
  Op**   ops;           // Op 포인터 배열
} Stage_Data;
```

각 스테이지는 상류 스테이지의 `Stage_Data`를 입력으로 받고, 자신의 `Stage_Data`를 출력으로 다음 스테이지에 전달한다.

### 4.3 Cmp_Model — 최상위 전역 상태 (cmp_model.h)

```
Cmp_Model cmp_model;  // 전역 단일 인스턴스

구성 요소 (per-core):
  Icache_Stage     icache_stage[NUM_CORES]
  Uop_Cache_Stage  uop_cache_stage[NUM_CORES]
  Decode_Stage     decode_stage[NUM_CORES]
  Idq_Stage        idq_stage[NUM_CORES]
  Map_Stage        map_stage[NUM_CORES]
  Node_Stage       node_stage[NUM_CORES]
  Exec_Stage       exec_stage[NUM_CORES]
  Dcache_Stage     dcache_stage[NUM_CORES]
  Bp_Data          bp_data[NUM_CORES]
  Bp_Recovery_Info bp_recovery_info[NUM_CORES]
  Memory           memory  (공유)
```

---

## 5. 파이프라인 스테이지 상세 분석

### 5.1 Fetch Stage: ICache (icache_stage.c)

**상태 기계** (`Icache_State`):
- `ICACHE_STAGE_RESTEER`: fetch 주소 변경 중
- `MEM_REQ`: 캐시 미스 → 메모리 요청 전송
- `WAIT_FOR_MISS`: 미스 응답 대기
- `SERVING`: 정상 ICache 서빙
- `UOP_CACHE_SERVING`: Uop Cache에서 서빙

**핵심 자료구조**:
```c
typedef struct Icache_Stage_struct {
  Stage_Data  sd;            // 출력 (→ Decode Stage)
  Icache_State state;
  Inst_Info** line;          // 현재 캐시 라인
  Addr        fetch_addr;    // 현재 Fetch 주소
  FT*         current_ft;    // 현재 Fetch Target
  Cache       icache;        // L1 명령어 캐시
  Cache       icache_line_info; // 라인 메타데이터
} Icache_Stage;
```

**Fetch Target Queue (FTQ)** 기반:
- `decoupled_frontend`가 FTQ를 채운다
- ICache는 FTQ에서 FT를 꺼내 fetch 실행
- `ft_arbitration()`: ICache vs Uop Cache 중 하나 선택

**Golden Cove 설정**:
- L1i: 32KB, 8-way, 64B line, 3 사이클 레이턴시

### 5.2 Uop Cache (uop_cache.cc)

```c
typedef struct Uop_Cache_Stage_struct {
  Stage_Data sd;
  FT*        current_ft;
} Uop_Cache_Stage;
```

- `UOP_CACHE_ENABLE` 파라미터로 활성화
- 디코딩된 마이크로-op을 캐싱하여 재디코딩 비용 절감
- ICache와 병렬로 동작, 둘 중 유효한 것 사용
- **Golden Cove**: 512 라인 × 8 uops = ~4K uops, 8-way

### 5.3 Decode Stage (decode_stage.c)

```c
typedef struct Decode_Stage_struct {
  Stage_Data* sds;       // DECODE_CYCLES개의 파이프라인 스테이지
  Stage_Data* last_sd;   // 최종 출력
} Decode_Stage;
```

- **DECODE_CYCLES** 사이클에 걸쳐 파이프라인화
- **DECODE_WIDTH** op/cycle 대역폭
- 명령어 → 마이크로-op 변환 (xed/PIN 인터페이스 통해)
- **Golden Cove**: 6 decode width, 5 cycles

### 5.4 IDQ Stage (idq_stage.cc) — Frontend/Backend 브리지

```c
typedef struct Idq_Stage_struct {
  // IDQ_SIZE = 140 (default), 144 (Golden Cove)
  Op**  entries;          // 원형 버퍼
  int   head, tail;
  int   count, size;
  // 소스 추적
  int   ops_from_decoder;
  int   ops_from_uop_cache;
} Idq_Stage;
```

IDQ는 Decode/Uop Cache → Map Stage 사이의 디커플링 버퍼로:
- 프론트엔드가 빠를 때 버퍼링
- 백엔드가 스톨될 때 프론트엔드 계속 실행
- `idq_stage_get_stage_data()`: 다음 사이클에 Map이 가져갈 데이터 준비

### 5.5 Map Stage — 레지스터 리네이밍 (map_stage.c)

```c
typedef struct Map_Stage_struct {
  Stage_Data* sds;       // MAP_CYCLES개 파이프라인
  Stage_Data* last_sd;   // ROB/RS에 전달할 출력
} Map_Stage;
```

- IDQ → Map: `MAP_CYCLES` 사이클 파이프라인 (Golden Cove: 5 cycles)
- 레지스터 리네이밍 (`map_rename.c`에 위임)
- 소스 의존성 추적 및 `srcs_not_rdy_vector` 설정
- **Map_Data** (thread.h 내): RAT (Register Alias Table) 포함

### 5.6 Node Stage — ROB + Reservation Station (node_stage.c)

**Reservation_Station 구조**:
```c
typedef struct Reservation_Station_struct {
  uns proc_id;
  char name[EXEC_PORTS_MAX_NAME_LEN]; // e.g., "RS0", "RS1"
  uns32 size;                          // 0 = 무한
  Func_Unit** connected_fus;           // 연결된 FU 목록
  uns32 num_fus;
  uns32 rs_op_count;                   // 현재 Op 수
  uint64_t* entry_status;              // 비트마스크 (0=빈, 1=점유)
} Reservation_Station;
```

**Node_Stage 구조**:
```c
typedef struct Node_Stage_struct {
  Stage_Data sd;         // 출력 (→ Exec Stage)
  Op* node_head;         // ROB 헤드 (가장 오래된 Op)
  Op* node_tail;         // ROB 테일 (가장 새 Op)
  Op* node_precommit;    // Pre-commit 포인터
  int32 node_count;      // ROB에 있는 Op 수

  Op*  rdy_head;         // Ready List (발화 가능한 Op 연결 리스트)
  Counter ret_op;        // 다음 Retire할 Op 번호
  Counter last_scheduled_opnum;

  Op* next_op_into_rs;   // RS 아직 못 들어간 Op 중 가장 오래된 것
  Reservation_Station* rs;  // RS 배열

  Flag mem_blocked;      // 메모리 요청 버퍼 고갈
} Node_Stage;
```

**Dispatch 과정** (`node_fill_rob()`):
1. `NODE_TABLE_SIZE`(256/512) 체크
2. `rs_dispatch_scheme`에 따른 RS 선택
   - `FIND_EMPTIEST_RS(0)`: 가장 비어있는 RS에 배치
3. Macro-op fusion 처리 (`macro_fused` 플래그)

**Issue 과정** (Ready List 기반):
- `rdy_head`: 소스가 모두 준비된 Op의 연결 리스트
- `node_issue_queue.cc`가 스케줄링 정책 구현
  - `OLDEST_FIRST(0)` (기본): op_num이 가장 작은 Op 우선
- FU 가용성 확인 후 `exec_stage`로 전달

**Retire 과정**:
- `node_ret_width`(4~8) op/cycle 은퇴
- `node_retire_rate`(10) 제한 내에서 은퇴
- 프로그램 순서대로 (node_head부터)
- `done_cycle <= cycle_count`인 Op만 은퇴 가능
- `bp_retire_op()` 호출: 브랜치 예측기 retire 업데이트

### 5.7 Execute Stage (exec_stage.c)

**Func_Unit 구조**:
```c
typedef struct Func_Unit_struct {
  uns32   fu_id;
  uns64   type;           // OP_XXXXX_BIT 비트마스크
  Counter avail_cycle;    // FU 가용 사이클 (파이프라인 완료 시점)
  Counter idle_cycle;     // FU가 마지막으로 쉰 사이클
  Flag    held_by_mem;    // 메모리 스톨로 점유
} Func_Unit;
```

**실행 과정** (2단계):
```
Phase 1: 체크
  exec_stage_check_fu_available(op)  → FU 가용 여부
  reg_file_consume(op)               → 소스 물리 레지스터 소비 (Late Alloc)
  exec_stage_dep_wakeup(op)          → 결과 브로드캐스트 → 의존 Op wake-up

Phase 2: 래치
  op->sched_cycle = cycle_count
  op->exec_cycle = cycle_count + latency
  avail_cycle 갱신 (비파이프라인 FU: avail_cycle = exec_cycle)
```

**Op 타입별 레이턴시** (core.param.def):
| Op 타입 | 기본 레이턴시 | 비고 |
|---------|-------------|------|
| IADD/MOV/CF | 1 | 단일 사이클 |
| IMUL | 3 | 곱셈 |
| FCVT/FADD | 3 | FP 덧셈/변환 |
| FMUL/FMA | 5 | FP 곱셈/복합 |
| FDIV/IDIV | -20 | 비파이프라인 (20사이클) |
| GATHER | 12 | AVX 수집 |
| SCATTER | 18 | AVX 산포 |

**음수 레이턴시의 의미**: `avail_cycle = current + ABS(delay)` → 비파이프라인 FU

**Branch Resolution**:
- `exec_stage_bp_resolve()`: 오예측 감지 → `bp_sched_recovery()` 호출
- `bp_resolve_op()`: 예측기 업데이트

### 5.8 DCache Stage (dcache_stage.c)

```c
typedef struct Dcache_Stage_struct {
  Cache  dcache;            // L1 데이터 캐시
  Ports* ports;             // 뱅크별 read/write 포트
  Cache  pref_dcache;       // 프리패처 전용 캐시
  Counter idle_cycle;
  Flag   mem_blocked;       // Mem_Req 버퍼 고갈
} Dcache_Stage;
```

**메모리 접근 과정**:
1. Exec Stage에서 메모리 Op 수신
2. 주소 생성 완료 확인 (`exec_cycle <= cycle_count`)
3. 포트 중재 (프로그램 순서대로)
4. `cache_access()`: L1 DCache 접근
   - **Hit**: `done_cycle = dcache_cycle + DCACHE_CYCLES`
   - **Miss**: `Mem_Req` 생성 → 메모리 계층으로 전달

**Dcache_Data 메타데이터**:
```c
typedef struct Dcache_Data_struct {
  Flag   dirty;               // 더티 여부
  Flag   prefetch;            // 프리패치로 적재됐는가
  Flag   HW_prefetch;         // HW 프리패처
  uns    read_count[2];       // on/off-path 읽기 횟수
  uns    write_count[2];      // on/off-path 쓰기 횟수
  Counter fetch_cycle;
  Counter onpath_use_cycle;
} Dcache_Data;
```

**Golden Cove DCache**: 48KB, 12-way, 64B line, 5 사이클, read 2포트 + write 1포트

---

## 6. 레지스터 리네이밍 (map_rename)

2024년 UC Santa Cruz Litz Lab에서 구현한 현대적 레지스터 리네이밍.

### 6.1 레지스터 테이블 엔트리

```c
struct reg_table_entry {
  // Op 연결
  Op*     op;
  Counter op_num;
  Counter unique_num;
  Flag    off_path;

  // 레지스터 ID
  int parent_reg_id;   // 아키텍처 레지스터 번호
  int self_reg_id;     // 물리 레지스터 번호 (자신)
  int child_reg_id;    // 이후 버전의 물리 레지스터 (RAT 체인)

  // 타입
  int reg_type;        // INT or VEC
  int reg_table_type;  // ARCH / PHYSICAL / VIRTUAL

  // 상태
  enum reg_table_entry_state reg_state;  // FREE/ALLOC/PRODUCED/COMMIT
  struct reg_table_entry* next_free;     // 프리 리스트 연결

  // 라이프사이클 추적
  Counter allocated_cycle;
  Counter produced_cycle;
  Counter consumed_cycle;
  int     num_refs;        // 이 레지스터를 참조하는 Op 수
  int     num_consumers;   // 소비자 수
};
```

### 6.2 리네이밍 방식 (REG_RENAMING_SCHEME)

| 값 | 이름 | 설명 |
|----|------|------|
| 0 | INFINITE | 물리 레지스터 무제한 (연구 기준선) |
| 1 | REALISTIC | 고정 물리 레지스터 풀 (Golden Cove: INT 280, VEC 332) |
| 2 | LATE_ALLOCATION | Issue 시 할당 |
| 3 | EARLY_RELEASE_SPEC | 투기적 조기 반환 |
| 4 | EARLY_RELEASE_NONSPEC | 비투기적 조기 반환 |
| 5 | EARLY_RELEASE_LASTUSE | 마지막 사용 시 반환 |

### 6.3 Move Elimination

```c
// XED_ICLASS_MOV 명령어에 대해 별도 물리 레지스터 없이
// 소스와 같은 물리 레지스터를 대상으로 매핑
if (REG_RENAMING_MOVE_ELIMINATE && op->table_info->op_type == OP_MOV) {
  op->dst_reg_id[i] = op->src_reg_id[0]; // 소스와 동일 레지스터 사용
  op->move_eliminated = TRUE;
}
```

### 6.4 복구 (reg_file_recover)

`cmp_recover()` 에서 호출:
1. `op->prev_dst_reg_id`로 RAT 롤백
2. 오예측 이후 할당된 물리 레지스터 모두 해제
3. Free List 복원

---

## 7. 메모리 서브시스템

### 7.1 메모리 계층 구조 (memory/)

```
L1 DCache (per-core)
    ↕
MLC (Mid-Level Cache, per-core or shared)
    ↕
LLC (Last-Level Cache, shared)
    ↕
DRAM (Ramulator)
```

**cache_lib.c**: 범용 N-way LRU/PLRU 캐시 구현
- `init_cache(cache, name, entries, assoc, line_size, data_size, repl_policy)`
- `cache_access(cache, addr, &line_addr, update_repl)`: 접근 + 교체 정책 업데이트
- `cache_insert(cache, proc_id, addr, &line_addr, &repl_addr)`: 삽입

**메모리 요청** (`mem_req.h`):
```
Mem_Req 타입:
  MRT_IFETCH    → 명령어 페치
  MRT_DFETCH    → 데이터 로드
  MRT_DSTORE    → 데이터 스토어
  MRT_IPRF      → 명령어 프리패치
  MRT_DPRF      → 데이터 프리패치
  MRT_WB        → 라이트백
```

**Ramulator** (ramulator.cc): DRAM 타이밍 시뮬레이터 통합
- DDR4 2400R 기본 설정 (tCL=16, tCK=833333 ps)
- FRFCFS_Cap 스케줄링 정책

**Golden Cove 메모리 설정**:
- MLC: 2MB, 8-way, 16 사이클
- LLC: 3MB/core, 16-way, 36 사이클
- DRAM: DDR4 2400R

### 7.2 LSQ (Load-Store Queue, lsq.cc)

2025년 Litz Lab 구현:
```c
// 파라미터
LSQ_ENABLE = TRUE
LOAD_QUEUE_ENTRY_NUM = 128
STORE_QUEUE_ENTRY_NUM = 72
```

**기능**:
- `lsq_available()`: 슬롯 가용성 확인 (ROB 진입 전 체크)
- `lsq_dispatch(op)`: Dispatch 시 엔트리 할당
- `lsq_commit(op)`: Retire 시 엔트리 해제
- 로드-스토어 의존성 추적 및 메모리 오더링 위반 감지

### 7.3 프리패처

**프리패처 목록** (prefetcher/):
| 이름 | 파일 | 설명 |
|------|------|------|
| Stream | pref_stream | 스트라이드/스트림 프리패치 |
| FDIP | fdip.c/h | Fetch-Directed Instruction Prefetching |
| EIP | eip.c/h | Execution-based Instruction Prefetching |
| D_JOLT | D_JOLT.c/h | 동적 점프 오프셋 학습 |
| FNL+MMA | FNL+MMA.c/h | 프리패처 앙상블 |

**Golden Cove 프리패처 설정**:
- stream length: 64
- stream prefetch N: 4
- pref_throttlefb_on: 정확도 기반 스로틀링

---

## 8. Decoupled Frontend

`decoupled_frontend.cc / .h`

### 8.1 설계 목적

메인 파이프라인과 분리된 전용 브랜치 예측 스레드로:
- 파이프라인 백엔드와 독립적으로 브랜치 예측 실행
- FTQ (Fetch Target Queue)를 미리 채워둠
- ICache가 FTQ에서 FT를 꺼내어 fetch

### 8.2 FT (Fetch Target, ft.h)

```c
typedef struct FT_struct {
  Addr  start;        // Fetch 시작 주소
  Addr  end;          // Fetch 종료 주소
  Flag  taken;        // 마지막 분기가 taken인가
  Addr  next_addr;    // 다음 FT 시작 주소
  // 오프패스 추적
  Flag  off_path;
  Off_Path_Reason reason;
  Conf_Off_Path_Reason conf_reason;
} FT;
```

**FTQ 파라미터**:
- `FE_FTQ_BLOCK_NUM`: 32개 FT 버퍼
- `FE_FTQ_TAKEN_CFS_PER_CYCLE`: 사이클당 taken 브랜치 2개 예측
- `FE_FTQ_BYTES_PER_CYCLE`: 128 bytes/cycle

### 8.3 Off-Path 추적 이유 목록

```c
typedef enum OFF_PATH_REASON_enum {
  REASON_NOT_IDENTIFIED,
  REASON_IBTB_MISS,          // 간접 분기 BTB 미스
  REASON_BTB_MISS,           // BTB 미스
  REASON_BTB_MISS_MISPRED,   // BTB 미스 + 방향 예측 오류
  REASON_MISPRED,            // 방향 예측 오류
  REASON_MISFETCH,           // 타겟 예측 오류
} Off_Path_Reason;
```

---

## 9. Uop Cache

`uop_cache.cc / .h`

L1i와 inclusive한 구조로, 디코딩된 uop를 캐싱:

**파라미터** (Golden Cove):
```
--uop_cache_enable     1
--uop_cache_width      8     (8 uops per line)
--uop_cache_lines    512     (512 lines)
--uop_cache_assoc      8     (8-way)
→ 총 4096 uops 저장
--iprf_on_uop_cache_hit 1    (L1i 프리패치 연계)
```

**ICache vs Uop Cache 중재** (`ft_arbitration()`):
- 동일 FT에 대해 둘 다 응답 가능할 때 Uop Cache 우선
- Uop Cache hit: `uop_cache_stage.sd`에 Op 적재
- ICache hit: `icache_stage.sd`에 Op 적재
- IDQ로 합쳐짐

---

## 10. Fill Buffer & Dependency Chain Cache

### 10.1 Fill Buffer (fill_buffer.h)

```c
typedef struct Fill_Buffer_struct {
  Op*  entries;     // 원형 버퍼 (최근 retire된 Op들)
  int  head;        // 쓰기 포인터
  int  tail;        // 읽기 포인터
  int  count;
  int  size;        // FILL_BUFFER_SIZE (파라미터)
  char* name;
} Fill_Buffer;

extern Fill_Buffer** retired_fill_buffers;  // per-core 배열
```

**동작**:
- `fill_buffer_add(proc_id, op)`: Op retire 시 호출 → 원형 버퍼에 Op 복사본 저장
- Backward Walk Engine이 이 버퍼를 역방향으로 읽어 의존성 체인 추적

### 10.2 Dependency Chain Cache (dependency_chain_cache.h)

**Block Cache**: 의존성 체인을 기본 블록 단위로 캐싱

```c
// 상수
DEPENDENCY_CHAIN_CACHE_SIZE = 1024   // 브랜치별 체인 캐시
BLOCK_CACHE_SIZE            = 1024   // 기본 블록별 체인 캐시
MAX_CHAIN_LENGTH            = 64     // 체인 최대 길이
MAX_LIVE_INS                = 32     // 라이브-인 최대 수
MAX_ARCH_REGS               = 64     // 추적 아키텍처 레지스터 수
MAX_MEM_LIVE_INS            = 16     // 메모리 의존성 버퍼 크기

typedef struct Dependency_Chain_Cache_Entry_struct {
  Flag     is_valid;
  Addr     h2p_branch_pc;
  Counter  h2p_branch_op_num;
  uns      chain_length;
  Op       chain[MAX_CHAIN_LENGTH];  // 의존성 체인 Op들
  uint64_t dependency_mask;          // 기본 블록 내 의존성 비트마스크
  uns      total_ops_in_block;
} Dependency_Chain_Cache_Entry;

// Source List (Live-in 추적용)
typedef struct SourceList_struct {
  uint64_t reg_vector;              // 레지스터 비트 벡터
  Addr     addrs[MAX_MEM_LIVE_INS]; // 메모리 주소 목록
  uns      addr_count;
} SourceList;
```

**Backward Walk Engine**:
```c
typedef struct Backward_Walk_Engine_struct {
  Backward_Walk_State state;        // BW_IDLE or BW_WALKING
  Counter walk_cycles_remaining;
  Op*     snapshot_buffer;          // 분석 중인 Op 버퍼 (동적 할당)
  int     snapshot_op_count;
} Backward_Walk_Engine;

extern Backward_Walk_Engine** bw_engines;  // per-core
```

**동작 흐름**:
1. `cycle_backward_walk_engine()` 매 사이클 구동
2. Fill Buffer에서 역방향으로 Op 읽기
3. 의존성 체인 재구성
4. `add_dependency_chain()` → Block Cache에 저장

---

## 11. 복구 메커니즘

### 11.1 복구 트리거 (cmp_istreams)

```c
void cmp_istreams(void) {
  for (uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    if (freq_is_ready(FREQ_DOMAIN_CORES[proc_id])) {
      // Recovery: 오예측 복구
      if (cycle_count >= bp_recovery_info->recovery_cycle) {
        cmp_recover();
      }
      // Redirect: BTB miss로 인한 리다이렉트
      if (cycle_count >= bp_recovery_info->redirect_cycle) {
        cmp_redirect();
      }
    }
  }
}
```

`recovery_cycle == MAX_CTR` = 복구 없음 (sentinel 값)

### 11.2 cmp_recover() 전체 흐름

```c
void cmp_recover() {
  // 1. Branch Predictor 상태 복구
  bp_recover_op(g_bp_data, cf_type, &recovery_info);

  // 2. Late BP 처리 (late_bp_recovery이면 pred를 late_pred로 변경)
  if (USE_LATE_BP && bp_recovery_info->late_bp_recovery) { ... }

  // 3. 레지스터 파일 복구 (RAT 롤백)
  reg_file_recover(bp_recovery_info->recovery_op);

  // 4. 스레드 상태 복구 (fetch 주소 리셋)
  recover_thread(td, recovery_fetch_addr, recovery_op_num, ...);

  // 5. 각 스테이지 복구 (상류→하류 순)
  recover_decoupled_fe();
  recover_fdip();
  recover_icache_stage();
  recover_uop_cache();
  recover_decode_stage();
  recover_uop_queue_stage();
  recover_idq_stage();
  recover_map_stage();
  recover_node_stage();    // ROB flush
  recover_lsq();
  recover_exec_stage();
  recover_dcache_stage();
  recover_memory();

  // 6. 로그 기록
  log_recovery_end(node, cycle_count, bp_recovery_info);

  // 7. 복구 완료 표시
  bp_recovery_info->recovery_cycle = MAX_CTR;
  bp_recovery_info->redirect_cycle = MAX_CTR;
}
```

### 11.3 Wake-Up 메커니즘 (cmp_wake)

```c
void cmp_wake(Op* src_op, Op* dep_op, uns8 rdy_bit) {
  // dep_op가 RS에 없으면 rdy_cycle만 갱신하고 리턴
  if (dep_op->state != OS_IN_RS) {
    dep_op->rdy_cycle = MAX2(dep_op->rdy_cycle, src_op->wake_cycle);
    return;
  }

  simple_wake(src_op, dep_op, rdy_bit);  // srcs_not_rdy_vector 업데이트

  // 모든 소스 준비됐으면 Ready List에 추가
  if (dep_op->srcs_not_rdy_vector == 0 && cycle_count >= dep_op->issue_cycle) {
    dep_op->next_rdy = node->rdy_head;
    node->rdy_head = dep_op;
    dep_op->in_rdy_list = TRUE;
  }
}
```

---

## 12. 파라미터 시스템

### 12.1 파라미터 정의 매크로

```c
DEF_PARAM(param_name, VARIABLE_NAME, type, parser_func, default_value, const)
```

- `param_name`: CLI에서 `--param_name=val` 형태로 사용
- `VARIABLE_NAME`: C 변수 이름 (extern으로 참조)
- `.param.def` 파일들이 `parameters.c`에 포함되어 변수 생성

### 12.2 주요 파라미터 분류

**코어 구성** (core.param.def):
| 파라미터 | 기본값 | Golden Cove |
|---------|-------|-------------|
| num_cores | 1 | 1 |
| issue_width | 4 | 6 |
| node_table_size | 256 | 512 |
| decode_width | 4 | 6 |
| decode_cycles | 1 | 5 |
| map_cycles | 1 | 5 |
| idq_size | 140 | 144 |
| node_ret_width | 4 | 8 |

**레지스터 파일**:
| 파라미터 | 기본값 | Golden Cove |
|---------|-------|-------------|
| reg_renaming_scheme | 0 (INFINITE) | 1 (REALISTIC) |
| reg_table_integer_physical_size | 256 | 280 |
| reg_table_vector_physical_size | 256 | 332 |

**실행 포트** (RS 설정):
```
rs_sizes       = "256"     # 단일 RS 256 entries
rs_connections = "xF"      # 모든 FU 연결 (비트마스크 0xF)
fu_types       = "0,0,0,0" # 모든 op 타입 허용
```

Golden Cove 실제 설정:
```
rs_sizes       = "184 132 36"              # RS1, RS2, RS3
rs_connections = "b0100110011 b1000001100 b0011000000"
                                            # 포트별 RS 연결 비트마스크
```

**프론트엔드**:
```
icache_latency    = 3       # L1i 레이턴시
fe_ftq_block_num  = 32      # FTQ 크기
fe_ftq_bytes_per_cycle = 128 # FTQ throughput
```

### 12.3 파라미터 파일 계층

```
PARAMS.golden_cove   (하드웨어 특성)
    ↓ override
PARAMS.in            (실험별 설정)
    ↓ override
CLI flags            (최우선)
```

---

## 13. 통계 시스템

### 13.1 통계 정의 매크로

```c
DEF_STAT(STAT_NAME, TYPE, RATIO_STAT)
```

타입:
- `COUNT`: 누적 카운트
- `DIST`: 분포 (비율로 표시)
- `PER_CYCLE`: 사이클당 값
- `PER_INST`: 명령어당 값
- `PER_1000_INST`: 1000 명령어당 값
- `RATIO`: 다른 통계 대비 비율

### 13.2 주요 통계 파일

| 파일 | 포함 통계 |
|------|---------|
| `core.stat.def` | IPC, 사이클, 명령어 수, 스톨 |
| `fetch.stat.def` | ICache 적중률, BTB, 브랜치 |
| `bp/bp.stat.def` | BP 오예측, BTB, CRS, IBTB |
| `inst.stat.def` | 명령어 타입 분포 |
| `memory/memory.stat.def` | L1/MLC/LLC hit/miss |

### 13.3 통계 수집 매크로

```c
STAT_EVENT(proc_id, STAT_NAME)        // 카운트 +1
INC_STAT_EVENT(proc_id, NAME, amount) // amount 만큼 증가
```

**통계 주기**: `STAT_INTERVAL` 파라미터로 인터벌 통계 수집, 누적 통계도 별도 추적.

---

## 14. 로깅 서브시스템 (src/log/)

5가지 전문 로거가 시뮬레이션 이벤트를 기록한다:

| 로거 | 파일 | 기록 내용 |
|------|------|---------|
| **op_trace_log** | op_trace_log.c/h | Op별 타이밍/상태 추적 |
| **recovery_log** | recovery_log.c/h | 브랜치 복구 이벤트 |
| **on_off_path_log** | on_off_path_log.c/h | On/off path 전환 이벤트 |
| **dependency_chain_log** | dependency_chain_log.c/h | 의존성 체인 이벤트 |
| **fill_buffer_log** | fill_buffer_log.c/h | Fill Buffer 상태 변화 |

모든 로거는 `cmp_init()`에서 초기화:
```c
init_op_trace_log();
init_recovery_log();
init_on_off_path_log();
init_dependency_chain_log();
init_fill_buffer_log();
```

---

## 15. 공통 라이브러리 (globals / libs)

### 15.1 globals/ — 전역 타입 및 유틸리티

| 파일 | 내용 |
|------|------|
| `global_types.h` | `uns`, `uns8`, `Addr`, `Flag`, `Counter` 등 기본 타입 |
| `global_defs.h` | `TAKEN/NOT_TAKEN`, `MAX_CTR`, `N_BIT_MASK()` 등 매크로 |
| `global_vars.h` | `cycle_count`, `td` 등 전역 변수 extern 선언 |
| `assert.h` | `ASSERT()`, `ASSERTM()`, `FATAL_ERROR()` 매크로 |
| `utils.h/c` | `SAT_INC()`, `SAT_DEC()`, `CIRC_INC()`, `CIRC_DEC()` 등 |
| `enum.h/c` | `DECLARE_ENUM()` — enum + 이름 배열 자동 생성 |

**SAT_INC/DEC**: Saturating arithmetic (언더/오버플로우 방지)
```c
#define SAT_INC(x, max) ((x) < (max) ? (x) + 1 : (max))
#define SAT_DEC(x, min) ((x) > (min) ? (x) - 1 : (min))
```

### 15.2 libs/ — 범용 데이터 구조

| 파일 | 구현 |
|------|------|
| `cache_lib.h/c` | 범용 N-way LRU 캐시 (BTB, ICache, DCache 모두 사용) |
| `hash_lib.h/c` | 해시 테이블 (무한 BHT, per-branch 통계 등) |
| `list_lib.h/c` | 연결 리스트 |
| `port_lib.h/c` | 포트 중재 (DCache 뱅크 포트) |
| `malloc_lib.h/c` | 메모리 할당 래퍼 |
| `bloom_filter.hpp` | Bloom Filter |
| `perceptron.hpp` | Perceptron 구현 |
| `cpp_cache.h/tpp` | C++ 캐시 래퍼 |

**cache_lib.c 핵심 API**:
```c
void init_cache(Cache*, name, entries, assoc, line_size, data_size, repl_policy);
void* cache_access(Cache*, addr, &line_addr, update_repl);
void* cache_insert(Cache*, proc_id, addr, &line_addr, &repl_addr);
void  cache_invalidate(Cache*, addr, &line_addr);
```

---

## 16. ISA 레이어

`src/isa/`: x86-64 ISA 매핑

| 파일 | 내용 |
|------|------|
| `isa.h/c` | Inst_Info 생성, xed 디코딩 인터페이스 |
| `isa_macros.h` | ISA 관련 매크로 (`IS_CALLSYS`, `IS_MEM_LD` 등) |
| `x86_regs.def` | x86-64 레지스터 정의 (RAX, RBX, ..., XMM0, ...) |

**프론트엔드 인터페이스** (`frontend/`):
- `pin_trace_fe.c/h`: Intel PIN 트레이스 기반 명령어 공급
- 트레이스 파일에서 명령어 읽어 `Inst_Info` 생성

---

## 17. Golden Cove 설정 분석

**PARAMS.golden_cove** (Alder Lake P-core, Intel 12세대):

```
클럭:       3.2 GHz (chip_cycle_time = 312500 fs)
BP:         TAGE-SC-L 64K (tage64k)
             CFS per cycle: 6
             BTB: 16384 entries, 4-way
             CRS: 128 entries, realistic mode
             iBTB: TC_Tagged, 4096 entries

프론트엔드:
  ICache:   32KB, 8-way, 64B line
  UopCache: 8-wide, 512 lines, 8-way (~4K uops)
  Decode:   6-wide, 5 cycles
  IDQ:      144 entries
  Map:      5 cycles
  Regs:     INT 280, VEC 332

백엔드:
  Issue:    6-wide
  ROB:      512 entries
  RS:       RS1=184, RS2=132, RS3=36 (352 total)
  Retire:   8-wide
  FU:       10개 포트 (복잡한 연결 비트마스크)

DCache:     48KB, 12-way, 64B, 5 cycles
            R:2포트 + W:1포트

메모리:
  MLC:      2MB, 8-way, 16 cycles
  LLC:      3MB/core, 16-way, 36 cycles
  DRAM:     DDR4-2400R (Ramulator)
```

---

## 18. 설계상 주목할 패턴

### 18.1 역순 파이프라인 업데이트

사이클마다 DCache → Exec → Node → ... → ICache 순으로 역방향 업데이트. 이는:
- 스톨 신호의 자동 상류 전파
- "이번 사이클에 생성된 결과"가 "같은 사이클의 상류 스테이지"로 가지 않음 보장
- 사이클-어큐레이트 모델링의 표준 기법

### 18.2 Global 포인터 패턴 (cmp_set_all_stages)

Scarab은 전역 포인터(`ic`, `dec`, `uc`, `map`, `node`, `exec`, `dcache`, `td` 등)를 사용해서 현재 코어의 스테이지를 가리킨다:

```c
// cmp_set_all_stages(proc_id):
  ic    = &cmp_model.icache_stage[proc_id];
  dec   = &cmp_model.decode_stage[proc_id];
  node  = &cmp_model.node_stage[proc_id];
  ...
```

이 패턴은 스테이지 함수들이 항상 동일한 전역 포인터를 사용할 수 있게 해 파라미터 전달을 단순화한다. 멀티코어 처리 시 루프 시작마다 `cmp_set_all_stages(proc_id)` 호출이 필수적이다.

### 18.3 Oracle + Engine 이중 정보

`Op`는 두 종류의 실행 정보를 갖는다:
- `oracle_info`: 시뮬레이터 오라클이 아는 정답 (실제 방향, NPC, 결과)
- `engine_info`: 파이프라인 엔진이 예측/수행한 결과

이 분리로 오예측 감지, 통계 수집, 디버깅이 자연스럽게 이루어진다.

### 18.4 Warmup Mode와 Simulation Mode 분리

`cmp_init(WARMUP_MODE)` → 실제 초기화
`cmp_init(SIMULATION_MODE)` → 캐시 교체 정책만 변경 (Warm 상태에서 시작)

이로써 cold-start 문제 없이 안정된 측정 가능.

### 18.5 파라미터 기반 완벽 예측기 오라클

`PERFECT_BP`, `PERFECT_BTB`, `PERFECT_IBP`, `PERFECT_CRS`, `PERFECT_TARGET` 등의 파라미터로 각 서브시스템을 선택적으로 완벽하게 만들 수 있다. 이는:
- 각 서브시스템의 영향도 격리 측정
- 이상적 구현 대비 실제 성능 gap 측정
- 연구 가설 검증에 유용

---

---

Scarab은 TAGE-SC-L 기반 최신 브랜치 예측기, 현대적 레지스터 리네이밍 시스템, 그리고 Decoupled Frontend 기반의 고성능 파이프라인 모델을 갖춘 사이클-어큐레이트 OoO CPU 시뮬레이터다.
