/***************************************************************************************
 * File         : zereco/rfp.h
 * Description  : Timed Register File Prefetching (RFP, ISCA'22) for ZERECO.
 *
 * Unlike the h2p_chain_perfect_load oracle -- which predicts and validates in
 * the same cycle, after address generation, and never touches the memory system
 * -- this model reproduces the paper's datapath:
 *
 *   retire  : train a finite, set-associative Prefetch Table (base/stride/conf)
 *   rename  : count the in-flight instance and queue a prefetch packet
 *   dcache  : drain the queue with L1 read ports the demand loads did not use
 *   dcache  : validate the predicted address at the load's first attempt; only
 *             a prediction that was correct *and* arrived in time saves latency
 *
 * Which loads may own a Prefetch Table entry is set by RFP_SCOPE: the H2P
 * backward-slice Target Loads found by the retire-time walk (ZERECO), or every
 * load (vanilla RFP).  Design rationale and the mapping to the paper are in
 * src/zereco/zereco_RFP_IMPLEMENTATION_PLAN.md.
 ***************************************************************************************/

#ifndef __ZERECO_RFP_H__
#define __ZERECO_RFP_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Paper Table 1 fixes these widths; they are not swept, so they are not
   parameters.  The in-flight counter is 7 bits, which comfortably covers the
   ~49 concurrent instances of a single load PC measured in GAP bc. */
#define RFP_UTILITY_MAX 3   /* 2-bit utility */
#define RFP_INFLIGHT_MAX 127 /* 7-bit in-flight counter */
/* Slots in the store-write timestamp table used for stale-prefetch detection.
   Measurement scaffolding, not modeled hardware, so size it generously. */
#define RFP_STALE_TABLE_ENTRIES (64 * 1024)

/* One static load PC.  `base_va` is the address of the most recently *retired*
   instance, so a prediction for an instance that is `inflight` allocations
   younger is `base_va + stride * inflight` (paper §3.1).  That arithmetic is
   why training must see every retired instance rather than a sampled subset. */
typedef struct RFP_PT_Entry_struct {
  Flag valid;
  Addr tag;             /* full load PC; the paper keeps a 16-bit tag */
  Flag has_base;        /* FALSE until the first retire fills base_va */
  Addr base_va;
  Flag has_stride;
  int64 stride;
  uns confidence;       /* saturated => eligible to launch */
  uns utility;          /* replacement victim selection */
  uns inflight;         /* allocated but not yet committed instances */
  Counter lru_touch;    /* tie-break when utilities are equal */
} RFP_PT_Entry;

/* A queued prefetch packet.  The op pointer is only dereferenced after
   op_num/unique_num confirm the op pool has not recycled the entry, matching
   how Scarab's memory requests guard their own op back-pointers. */
typedef struct RFP_Queue_Entry_struct {
  Op* op;
  Counter op_num;
  Counter unique_num;
  Addr pred_va;
  uns mem_size;
  Counter launch_cycle;
} RFP_Queue_Entry;

/* A probe that missed the L1 and was sent on to the lower levels
   (RFP_L1_MISS_POLICY 1).  The memory system carries no back-pointer to the op,
   so the fill callback finds its owner here by line address and identity. */
typedef struct RFP_Pending_Fill_struct {
  Flag valid;
  Addr line_addr;
  Op* op;
  Counter op_num;
  Counter unique_num;
  Counter issue_cycle;
} RFP_Pending_Fill;

typedef struct RFP_Core_State_struct {
  Flag initialized;
  RFP_PT_Entry* pt;      /* pt_sets * RFP_PT_ASSOC entries */
  uns pt_sets;
  /* Request FIFO.  Entries drop out by going invalid rather than by being
     shifted, so the head skips tombstones at the start of each drain -- which
     is also what lets RFP_PORT_FAIL_POLICY 2 probe past a blocked head. */
  RFP_Queue_Entry* queue;
  uns queue_head;
  uns queue_count;
  /* Extra read ports that serve prefetches only (RFP_PORT_PRIORITY 1), one set
     per dcache bank.  Unused by the other priority policies. */
  Ports* dedicated_ports;
  /* Probes waiting on a lower-level fill (RFP_L1_MISS_POLICY 1). */
  RFP_Pending_Fill* pending;
  uns pending_count;
  /* Direct-mapped record of when each L1 line was last written by a store, used
     only to detect the stale-prefetch window.  Approximate by construction: two
     lines that alias share a slot, which can only over-report. */
  Counter* last_store_cycle;
  /* Per-cycle port bookkeeping, reset on the cycle's first drain. */
  Counter port_cycle;
  uns ports_taken_this_cycle;
  uns64 rand_state;      /* deterministic source for the 1/16 confidence bump */
} RFP_Core_State;

struct Dcache_Stage_struct;

void rfp_init(uns proc_id);
void rfp_reset(uns proc_id);

/* Retire-time backward walk found `load_pc` inside an H2P backward slice, so
   this PC is a Target Load and may own a Prefetch Table entry.  Under
   RFP_SCOPE 0 this is the only path that allocates entries. */
void rfp_note_target_load(uns proc_id, Addr load_pc, Addr va);

/* Retire: train base/stride/confidence and release this instance's in-flight
   count.  Only ever updates an entry that already exists (allocation under
   RFP_SCOPE 0 belongs to the walk). */
void rfp_retire_train(Op* op);

/* Rename: count the instance, then queue a prefetch packet if the PC is
   eligible and no older store overlaps this load. */
void rfp_rename_launch(Op* op);

/* Drain queued prefetches into the L1.  Called twice per dcache cycle; the hook
   whose position matches RFP_PORT_PRIORITY does the work and the other returns.
   `before_demand` is TRUE at the call placed ahead of the demand loop, which is
   where priority 2 (prefetch first) probes.  Priorities 0 and 1 run after the
   demand loop, so they can only take ports the demand loads did not want. */
void rfp_queue_drain(uns proc_id, struct Dcache_Stage_struct* dcache,
                     Flag before_demand);

/* End of the dcache cycle: attribute this cycle's L1 read ports to demand
   traffic, prefetches, or idle. */
void rfp_account_dcache_ports(uns proc_id, struct Dcache_Stage_struct* dcache);

/* A demand load just failed to get a read port.  Only counts as prefetch
   interference when a prefetch actually took a port this cycle, which cannot
   happen under the default priority. */
void rfp_note_demand_port_denied(uns proc_id);

/* Fill callback for a probe that continued past the L1.  Performs the normal
   dcache fill, then hands the data to the load that asked for it. */
Flag rfp_fill_done(Mem_Req* req);

/* A store just wrote a line in the L1.  Recorded so validation can tell whether
   a correct-address prefetch read the line before that write landed -- the one
   window in which the prefetched value could be stale.  Scarab's values are
   oracle, so this only ever counts; see the plan §2.7. */
void rfp_note_store_write(uns proc_id, Addr line_addr);

/* A load reached the dcache stage.  Returns TRUE when a prefetch covered it, in
   which case the load is already complete and must not access the cache. */
Flag rfp_try_validate(Op* op);

/* An op is being returned to the pool.  If it still owns an in-flight count it
   was squashed rather than retired, so release the count here (paper §3.1:
   "this counter is decremented for each squashed load"). */
void rfp_note_op_freed(Op* op);

#ifdef __cplusplus
}
#endif

#endif /* __ZERECO_RFP_H__ */
