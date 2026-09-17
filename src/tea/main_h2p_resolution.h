#ifndef __MAIN_H2P_RESOLUTION_H__
#define __MAIN_H2P_RESOLUTION_H__

#include "globals/global_types.h"
#include "op.h"

void reset_main_h2p_resolution_profiler(void);
void main_h2p_resolution_begin_recovery(Op* op, Flag late_bp_recovery,
                                        Counter recovery_cycle);
void main_h2p_resolution_record_fetch(Op* op);

#endif /* __MAIN_H2P_RESOLUTION_H__ */
