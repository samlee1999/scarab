#ifndef __ZERECO_H2P_MISPRED_LATENCY_H__
#define __ZERECO_H2P_MISPRED_LATENCY_H__

#include "globals/global_types.h"

void reset_zereco_h2p_mispred_latency_profiler(void);
void zereco_h2p_mispred_latency_begin_recovery(Op* op, Addr correct_fetch_addr,
                                                Counter recovery_cycle);
void zereco_h2p_mispred_latency_record_fetch(Op* op);

#endif /* __ZERECO_H2P_MISPRED_LATENCY_H__ */
