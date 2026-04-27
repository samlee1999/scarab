/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : tea/tea_store_buffer.c
 * Author       : TEA Implementation
 * Date         : 2025
 * Description  : TEA Store Buffer implementation — multi-H2P chain support
 ***************************************************************************************/

#include "tea/tea_store_buffer.h"

#include <stdlib.h>
#include <string.h>

#include "core.param.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_vars.h"

/**************************************************************************************/
/* Global Variables */

Tea_Store_Buffer** tea_store_buffers = NULL;

/**************************************************************************************/
/* init_tea_store_buffer */

void init_tea_store_buffer(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_store_buffers) {
    tea_store_buffers = (Tea_Store_Buffer**)calloc(NUM_CORES, sizeof(Tea_Store_Buffer*));
  }

  Tea_Store_Buffer* buf = (Tea_Store_Buffer*)calloc(1, sizeof(Tea_Store_Buffer));
  tea_store_buffers[proc_id] = buf;

  buf->proc_id = proc_id;
  buf->capacity = TEA_STORE_BUFFER_SIZE;
  buf->count = 0;

  buf->entries = (Tea_Store_Buffer_Entry*)calloc(TEA_STORE_BUFFER_SIZE,
                                                  sizeof(Tea_Store_Buffer_Entry));
  for (uns i = 0; i < TEA_STORE_BUFFER_SIZE; i++)
    buf->entries[i].valid = FALSE;
}

/**************************************************************************************/
/* reset_tea_store_buffer */

void reset_tea_store_buffer(uns proc_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_store_buffers || !tea_store_buffers[proc_id])
    return;

  Tea_Store_Buffer* buf = tea_store_buffers[proc_id];
  for (uns i = 0; i < buf->capacity; i++)
    buf->entries[i].valid = FALSE;
  buf->count = 0;
}

/**************************************************************************************/
/* tea_store_buffer_write */

Flag tea_store_buffer_write(uns proc_id, Addr addr, Quad data, uns size, uns8 chain_id) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, tea_store_buffers && tea_store_buffers[proc_id]);
  ASSERT(proc_id, size <= TEA_STORE_BUFFER_ENTRY_SIZE);

  Tea_Store_Buffer* buf = tea_store_buffers[proc_id];
  uns copy_size = (size <= sizeof(Quad)) ? size : sizeof(Quad);

  /* Update existing entry for same address */
  for (uns i = 0; i < buf->capacity; i++) {
    Tea_Store_Buffer_Entry* entry = &buf->entries[i];
    if (entry->valid && entry->addr == addr) {
      memcpy(entry->data, &data, copy_size);
      entry->size = copy_size;
      entry->write_cycle = cycle_count;
      entry->h2p_chain_id = chain_id;
      return TRUE;
    }
  }

  /* Find an empty slot */
  for (uns i = 0; i < buf->capacity; i++) {
    Tea_Store_Buffer_Entry* entry = &buf->entries[i];
    if (!entry->valid) {
      entry->addr = addr;
      memcpy(entry->data, &data, copy_size);
      entry->size = copy_size;
      entry->valid = TRUE;
      entry->write_cycle = cycle_count;
      entry->h2p_chain_id = chain_id;
      buf->count++;
      return TRUE;
    }
  }

  return FALSE;  /* Buffer full */
}

/**************************************************************************************/
/* tea_store_buffer_read */

Flag tea_store_buffer_read(uns proc_id, Addr addr, uns size, Quad* data_out) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  ASSERT(proc_id, data_out);

  if (!tea_store_buffers || !tea_store_buffers[proc_id])
    return FALSE;

  Tea_Store_Buffer* buf = tea_store_buffers[proc_id];

  for (uns i = 0; i < buf->capacity; i++) {
    Tea_Store_Buffer_Entry* entry = &buf->entries[i];
    if (entry->valid) {
      if (addr >= entry->addr && (addr + size) <= (entry->addr + entry->size)) {
        uns offset = addr - entry->addr;
        *data_out = 0;
        memcpy(data_out, &entry->data[offset], size);
        return TRUE;
      }
    }
  }

  return FALSE;
}

/**************************************************************************************/
/* tea_store_buffer_scan */

Flag tea_store_buffer_scan(uns proc_id, Addr addr, uns size) {
  ASSERT(proc_id, proc_id < NUM_CORES);

  if (!tea_store_buffers || !tea_store_buffers[proc_id])
    return FALSE;

  Tea_Store_Buffer* buf = tea_store_buffers[proc_id];

  for (uns i = 0; i < buf->capacity; i++) {
    Tea_Store_Buffer_Entry* entry = &buf->entries[i];
    if (entry->valid) {
      if (addr >= entry->addr && (addr + size) <= (entry->addr + entry->size))
        return TRUE;
    }
  }

  return FALSE;
}

/**************************************************************************************/
/* tea_store_buffer_clear_by_chain_id */

void tea_store_buffer_clear_by_chain_id(uns proc_id, uns8 chain_id) {
  if (!tea_store_buffers || !tea_store_buffers[proc_id])
    return;

  Tea_Store_Buffer* buf = tea_store_buffers[proc_id];
  for (uns i = 0; i < buf->capacity; i++) {
    Tea_Store_Buffer_Entry* entry = &buf->entries[i];
    if (entry->valid && entry->h2p_chain_id == chain_id) {
      entry->valid = FALSE;
      if (buf->count > 0)
        buf->count--;
    }
  }
}
