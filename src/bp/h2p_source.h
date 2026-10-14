#ifndef __H2P_SOURCE_H__
#define __H2P_SOURCE_H__

#include "bp/bp.param.h"
#include "bp/hbt.h"
#include "bp/pubs_conf.h"

/* ==========================================================
 * Which table says a branch is H2P (h2p_source, bp.param.def; 2026-10-14).
 *
 *   0 (default) : the HBT (TEA): H2P = counter > 1, tracked = counter >= 1.
 *   1           : PUBS's conf_tab (bp/pubs_conf.h): H2P = unconfident.  conf_tab has a
 *                 single level, so tracked = unconfident as well.
 *
 * Every H2P judgment of CLEAR goes through here -- the seed gate, the learning gate,
 * the consumption gate (priority bit and RF prefetch launch), owner protection -- and so
 * do the H2P flags an op carries (op->oracle_info.hbt_pred_is_hard / hbt_misp_counter, set
 * at prediction and at retire), which the H2P statistics read.  The HBT keeps training
 * under either source.
 * ========================================================== */

static inline Flag h2p_is_hard(Addr pc) {
  return H2P_SOURCE == 1 ? pubs_conf_is_unconfident(pc) : hbt_is_hard_branch(pc);
}

static inline Flag h2p_is_tracked(Addr pc) {
  return H2P_SOURCE == 1 ? pubs_conf_is_unconfident(pc) : hbt_get_counter(pc) >= 1;
}

static inline Hbt_Lookup h2p_lookup_state(Addr pc) {
  return H2P_SOURCE == 1 ? pubs_conf_lookup_state(pc) : hbt_lookup_state(pc);
}

/* What op->oracle_info.hbt_misp_counter carries: the HBT counter, or under conf_tab 2 for
   unconfident and 0 for confident, so that ">= 1" reads "tracked" and "> 1" reads "H2P"
   under either source. */
static inline uns32 h2p_level(Addr pc) {
  return H2P_SOURCE == 1 ? (pubs_conf_is_unconfident(pc) ? 2 : 0) : hbt_get_counter(pc);
}

#endif /* #ifndef __H2P_SOURCE_H__ */
