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
 * File         : tea/tea_store_buffer.h
 * Author       : TEA Implementation
 * Date         : 2025
 * Description  : TEA Store Buffer - Isolates TEA stores from D-cache
 *
 * Per paper Section IV-E:
 * - TEA stores write to this buffer instead of D-cache (architectural state protection)
 * - TEA loads check this buffer first for store forwarding
 * - Buffer is cleared on TEA termination
 ***************************************************************************************/

#ifndef __TEA_STORE_BUFFER_H__
#define __TEA_STORE_BUFFER_H__

#include "globals/global_types.h"

/**************************************************************************************/
/* Constants */

#define TEA_STORE_BUFFER_ENTRY_SIZE 64  /* Full cache line size for max coverage */

/**************************************************************************************/
/* TEA Store Buffer Entry */

typedef struct Tea_Store_Buffer_Entry_struct {
  Addr   addr;           /* Store address */
  uns8   data[TEA_STORE_BUFFER_ENTRY_SIZE];  /* Store data */
  uns    size;           /* Actual data size (1, 2, 4, 8, etc.) */
  Flag   valid;          /* Entry valid flag */
  Counter write_cycle;   /* Cycle when store was written */
  uns8   h2p_chain_id;   /* Chain that wrote this entry (1-based, 0=unset) */
} Tea_Store_Buffer_Entry;

/**************************************************************************************/
/* TEA Store Buffer Structure */

typedef struct Tea_Store_Buffer_struct {
  uns8  proc_id;
  Tea_Store_Buffer_Entry* entries;  /* Dynamically allocated array */
  uns   capacity;        /* Buffer capacity (TEA_STORE_BUFFER_SIZE) */
  uns   count;           /* Current valid entries count */
} Tea_Store_Buffer;

/**************************************************************************************/
/* Global Variables */

extern Tea_Store_Buffer** tea_store_buffers;  /* Per-core TEA store buffers */

/**************************************************************************************/
/* Function Prototypes */

/* Initialization and cleanup */
void init_tea_store_buffer(uns proc_id);
void reset_tea_store_buffer(uns proc_id);

/* Store buffer operations */
Flag tea_store_buffer_write(uns proc_id, Addr addr, Quad data, uns size, uns8 chain_id);
Flag tea_store_buffer_read(uns proc_id, Addr addr, uns size, Quad* data_out);
Flag tea_store_buffer_scan(uns proc_id, Addr addr, uns size);
void tea_store_buffer_clear_by_chain_id(uns proc_id, uns8 chain_id);

/**************************************************************************************/

#endif /* __TEA_STORE_BUFFER_H__ */
