#ifndef __HBT_H__
#define __HBT_H__

#include "globals/global_types.h" // Addr, uns, Flag 등 기본 타입
#include "op.h"                   // Op 구조체

// ==========================================================
// HBT (Hard Branch Table)
// TEA paper (MICRO 2024) Table II and Section IV-B:
//   - 256 entries, 8-way set associative, indexed by branch PC (0.2KB, 1-cycle)
//   - 3-bit saturating counter; an entry is created ON A MISPREDICTION with the
//     counter at 1, and a later misprediction of the same branch increments it
//   - H2P if the counter is greater than 1
//   - every counter is decremented by 1 every 50K retired instructions
//   - entries whose counter is 0 take replacement priority
// The geometry is a parameter (hbt_entries / hbt_assoc, defaults 256 / 8) so it
// can be swept; everything else above is fixed by the paper.
//
// The table is global, not per-core -- the simulations this is used in are
// single-core, and the previous implementation was global as well.
// ==========================================================
#define HBT_CTR_BITS 3        // HBT 카운터의 비트 수 (논문: 3-bit)
#define HBT_CTR_MAX ((1 << HBT_CTR_BITS) - 1) // HBT 카운터의 최댓값 (7)
#define HBT_H2P_THRESHOLD 1   // counter > 1이면 H2P (논문 기준)
#define HBT_DECAY_INTERVAL 50000

// ==========================================================
// HBT 자료구조 정의
// ==========================================================
typedef struct {
  uns64   tag;
  uns32   counter;
  Flag    valid;
  Counter lru;          // 마지막 접근 시점 (counter 0인 way들 중 희생자 선택용)
} HbtEntry;

/* What a lookup found.  Separating "not in the table" from "in the table with a
   low counter" matters: the first is a capacity/conflict miss of the HBT itself,
   the second is a branch the HBT has seen and judged easy.  Consumers that gate
   on H2P status inherit the first as a false negative, so it has to be
   measurable. */
typedef enum {
  HBT_LOOKUP_MISS = 0,  // 그 PC의 entry가 없다 (충돌/용량)
  HBT_LOOKUP_COLD,      // entry는 있고 counter <= 1
  HBT_LOOKUP_HARD       // entry가 있고 counter > 1 = H2P
} Hbt_Lookup;

// ==========================================================
// 전역 변수 선언 (extern)
// ==========================================================
extern uns64 retired_branch_count;
extern uns64 hbt_retired_instruction_count;

// ==========================================================
// 외부 공개 함수 원형(Prototype) 선언
// ==========================================================

/**
 * @brief HBT 모듈을 초기화합니다. 시뮬레이션 시작 시 한 번 호출됩니다.
 */
void hbt_init(void);

/**
 * @brief 브랜치가 retire될 때 HBT의 상태를 업데이트합니다.
 * @param op Retire되는 브랜치 명령어 정보
 */
void hbt_update(Op* op);

/**
 * @brief Main-thread instruction retirement tick used for paper-accurate HBT decay.
 */
void hbt_retire_instruction_tick(uns proc_id);

/**
 * @brief 특정 주소(PC)의 브랜치가 'Hard-to-predict' 상태인지 확인합니다.
 * @param pc 확인할 브랜치의 주소
 * @return 'Hard' 상태이면 TRUE, 아니면 FALSE
 */
Flag hbt_is_hard_branch(Addr pc);

uns32 hbt_get_counter(Addr pc);

/**
 * @brief 조회 결과를 miss / cold / hard로 구분해 돌려줍니다 (계측용).
 */
Hbt_Lookup hbt_lookup_state(Addr pc);

#endif /* #ifndef __HBT_H__ */
