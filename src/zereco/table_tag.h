/***************************************************************************************
 * File         : zereco/table_tag.h
 * Description  : Tags of the PC-indexed tables -- brslice_tab (zereco/critpath.c), the
 *                H2P Branch Table (bp/hbt.c) and the RFP Prefetch Table (zereco/rfp.c).
 *
 * A table indexes its set with the low PC bits (pc % sets) and compares a tag made of
 * the rest of the PC.  One switch picks the tag form for all of them:
 *
 *   table_tag_hashed 0 (default) : the full tag -- every PC bit takes part, so a lookup
 *                 can only match its own entry (no aliasing).  Every run before
 *                 2026-10-14 used it.
 *   table_tag_hashed 1 : an N-bit hashed tag, built as PUBS does it (MICRO'18, Fig. 7):
 *                 the tag part of the PC (pc / sets) cut into N-bit pieces that are XORed
 *                 together.  Two PCs of one set that fold to the same N bits share an
 *                 entry -- the aliasing a real table with short tags has.  N is the
 *                 table's own width parameter (zereco_critpath_tag_bits 8, hbt_tag_bits 8,
 *                 rfp_pt_tag_bits 16 by default -- PUBS's brslice_tab, the same for the
 *                 HBT, the RFP paper's Prefetch Table); a width of 0 keeps that one table
 *                 on the full tag.
 *
 * The tables also record the PC that allocated each entry, for measurement only, so a
 * lookup that matched another PC's entry can be counted (the *_ALIAS_HITS statistics).
 ***************************************************************************************/

#ifndef __ZERECO_TABLE_TAG_H__
#define __ZERECO_TABLE_TAG_H__

#include "globals/global_types.h"
#include "core.param.h"

/* The width a table actually uses: its parameter under table_tag_hashed, 0 (the full
   tag) otherwise. */
static inline uns table_tag_bits(uns bits) {
  return TABLE_TAG_HASHED ? bits : 0;
}

/* XOR of the `bits`-bit pieces of `tag_part` (PUBS Fig. 7).  bits must be 1..63. */
static inline uns64 table_tag_fold(uns64 tag_part, uns bits) {
  const uns64 mask = (1ULL << bits) - 1;
  uns64       hash = 0;
  while (tag_part) {
    hash ^= tag_part & mask;
    tag_part >>= bits;
  }
  return hash;
}

/* The tag a table compares for `pc`, `bits` being the width from table_tag_bits().
   `full_tag` is what the table stores with no hashing (its value before this option
   existed), so bits == 0 changes nothing. */
static inline uns64 table_tag(Addr pc, uns sets, uns bits, uns64 full_tag) {
  return bits ? table_tag_fold((uns64)pc / sets, bits) : full_tag;
}

#endif /* __ZERECO_TABLE_TAG_H__ */
