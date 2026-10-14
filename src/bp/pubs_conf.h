#ifndef __PUBS_CONF_H__
#define __PUBS_CONF_H__

#include "globals/global_types.h"
#include "op.h"
#include "bp/hbt.h" /* Hbt_Lookup */

/* ==========================================================
 * PUBS confidence table (conf_tab) -- Ando, MICRO 2018, Section III-A1 and Table II.
 *
 *   - one saturated resetting counter per branch (Jacobsen et al., MICRO'96): +1 when the
 *     branch was predicted correctly, back to 0 when it was not
 *   - every branch that executes gets an entry -- at the maximum if its first prediction
 *     was correct, at 0 if it was not
 *   - a branch is confident when its counter is at the maximum and unconfident (H2P, in
 *     this code's words) otherwise; a branch the table does not hold is confident
 *   - 128 sets x 8 ways, 6-bit counter, 4-bit hashed tag
 *
 * Here it is trained at retire, on committed branches only (the HBT's place, user decision
 * 2026-10-14; the paper says "executed"), and replaced LRU (the paper does not say).  The
 * geometry, counter width and hashed tag width are parameters (bp.param.def).  The table
 * exists only while it decides H2P (h2p_source 1, bp/h2p_source.h).
 * ========================================================== */

void pubs_conf_init(void);

/* A committed control-flow instruction: allocate or update its counter. */
void pubs_conf_update(Op* op);

/* Held and below the maximum. */
Flag pubs_conf_is_unconfident(Addr pc);

/* MISS (not held), COLD (confident) or HARD (unconfident) -- the HBT's three answers. */
Hbt_Lookup pubs_conf_lookup_state(Addr pc);

#endif /* #ifndef __PUBS_CONF_H__ */
