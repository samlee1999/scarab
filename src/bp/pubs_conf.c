#include "bp/pubs_conf.h"

#include <stdlib.h>
#include <string.h>

#include "bp/bp.param.h"
#include "globals/assert.h"
#include "globals/utils.h"
#include "statistics.h"
#include "zereco/table_tag.h"

/* See bp/pubs_conf.h.  Global like the HBT: the simulations are single-core, so the
   lookup statistics of this shared table go to core 0. */
typedef struct {
  Flag    valid;
  uns64   tag;     /* full tag, or under table_tag_hashed a pubs_conf_tag_bits-bit hash */
  Addr    pc;      /* measurement only: the branch that allocated the entry */
  uns32   counter; /* correct predictions in a row since the last misprediction, saturating */
  Counter lru;
} Pubs_Conf_Entry;

static Pubs_Conf_Entry* conf_table = NULL;
static uns              conf_sets  = 0;
static uns              conf_ways  = 0;
static uns32            conf_max   = 0;
static Counter          conf_clock = 0;

static inline uns64 conf_tag_of(Addr pc) {
  return table_tag(pc, conf_sets, table_tag_bits(PUBS_CONF_TAG_BITS), (uns64)(pc / conf_sets));
}

static inline Pubs_Conf_Entry* conf_set_base(Addr pc) {
  return &conf_table[(size_t)(pc % conf_sets) * conf_ways];
}

/* The way holding this PC, or NULL.  Read-only, like hbt_find(): only training ages an
   entry, so the frontend's lookups do not decide what is replaced. */
static Pubs_Conf_Entry* conf_find(Addr pc) {
  if (!conf_table)
    return NULL;
  Pubs_Conf_Entry* set = conf_set_base(pc);
  uns64            tag = conf_tag_of(pc);
  STAT_EVENT(0, PUBS_CONF_LOOKUPS);
  for (uns w = 0; w < conf_ways; ++w) {
    if (set[w].valid && set[w].tag == tag) {
      /* With a hashed tag the way may belong to another branch of the set. */
      if (set[w].pc != pc)
        STAT_EVENT(0, PUBS_CONF_ALIAS_HITS);
      return &set[w];
    }
  }
  return NULL;
}

void pubs_conf_init(void) {
  free(conf_table);
  conf_table = NULL;
  conf_clock = 0;
  if (H2P_SOURCE != 1)
    return;
  ASSERTM(0, PUBS_CONF_ASSOC > 0 && PUBS_CONF_ENTRIES >= PUBS_CONF_ASSOC &&
               PUBS_CONF_ENTRIES % PUBS_CONF_ASSOC == 0,
          "pubs_conf_entries (%u) must be a non-zero multiple of pubs_conf_assoc (%u)\n",
          PUBS_CONF_ENTRIES, PUBS_CONF_ASSOC);
  ASSERTM(0, PUBS_CONF_COUNTER_BITS >= 1 && PUBS_CONF_COUNTER_BITS <= 16,
          "pubs_conf_counter_bits must be between 1 and 16\n");
  ASSERTM(0, PUBS_CONF_TAG_BITS < 64, "pubs_conf_tag_bits must be 0 (full tag) or 1..63\n");
  conf_ways  = PUBS_CONF_ASSOC;
  conf_sets  = PUBS_CONF_ENTRIES / conf_ways;
  conf_max   = (1u << PUBS_CONF_COUNTER_BITS) - 1;
  conf_table = (Pubs_Conf_Entry*)calloc(PUBS_CONF_ENTRIES, sizeof(Pubs_Conf_Entry));
  ASSERTM(0, conf_table, "Failed to allocate the PUBS confidence table\n");
}

void pubs_conf_update(Op* op) {
  if (!conf_table)
    return;
  Addr pc      = op->inst_info->addr;
  Flag correct = !(op->oracle_info.mispred | op->oracle_info.misfetch);
  conf_clock++;
  STAT_EVENT(op->proc_id, PUBS_CONF_UPDATES);

  Pubs_Conf_Entry* entry = conf_find(pc);
  if (entry) {
    if (entry->pc != pc)
      STAT_EVENT(op->proc_id, PUBS_CONF_UPDATE_ALIAS_HITS);
    entry->lru = conf_clock;
    if (!correct) {
      entry->counter = 0;
      STAT_EVENT(op->proc_id, PUBS_CONF_RESETS);
    } else if (entry->counter < conf_max) {
      entry->counter++;
    }
  } else {
    /* Every branch gets an entry, confident from the start if this prediction was right. */
    Pubs_Conf_Entry* set    = conf_set_base(pc);
    Pubs_Conf_Entry* victim = NULL;
    for (uns w = 0; w < conf_ways; ++w) {
      if (!set[w].valid) {
        victim = &set[w];
        break;
      }
      if (!victim || set[w].lru < victim->lru)
        victim = &set[w];
    }
    if (victim->valid)
      STAT_EVENT(op->proc_id, PUBS_CONF_EVICTIONS);
    victim->valid   = TRUE;
    victim->tag     = conf_tag_of(pc);
    victim->pc      = pc;
    victim->lru     = conf_clock;
    victim->counter = correct ? conf_max : 0;
    STAT_EVENT(op->proc_id, PUBS_CONF_ALLOCATIONS);
    if (!correct)
      STAT_EVENT(op->proc_id, PUBS_CONF_RESETS);
    entry = victim;
  }
  /* The committed instances that leave the branch unconfident -- the PUBS counterpart of
     the H2P share of committed branches. */
  if (entry->counter < conf_max)
    STAT_EVENT(op->proc_id, PUBS_CONF_UNCONFIDENT_COMMITS);
}

Flag pubs_conf_is_unconfident(Addr pc) {
  Pubs_Conf_Entry* entry = conf_find(pc);
  return entry && entry->counter < conf_max;
}

Hbt_Lookup pubs_conf_lookup_state(Addr pc) {
  Pubs_Conf_Entry* entry = conf_find(pc);
  if (!entry)
    return HBT_LOOKUP_MISS;
  return entry->counter < conf_max ? HBT_LOOKUP_HARD : HBT_LOOKUP_COLD;
}
