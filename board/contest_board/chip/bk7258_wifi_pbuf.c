/****************************************************************************
 * board/contest_board/chip/bk7258_wifi_pbuf.c
 *
 * lwIP's pbuf allocator, reimplemented on the NuttX heap for Beken's WiFi
 * stack.
 *
 * Four facts decided the shape of this file.  None of them is visible from
 * the source that calls into it, so they are written down here.
 *
 * 1. This file is ours, but it is compiled inside the vendor OBJECT library
 *    (see bk7258_wifi_vendor.cmake), with the vendor's flags and the
 *    vendor's 113 include directories -- not with the NuttX flags the rest
 *    of chip/ uses.  That is deliberate.  rwnx_rx.c:16 includes "pbuf.h"
 *    *unguarded*, and the vendor include list puts lwIP 2.1.2's
 *    src/include/lwip on the path, so every vendor object already sees
 *    lwIP's struct pbuf.  Compiling this file anywhere else would give it a
 *    different definition of the same struct -- and since struct pbuf grows
 *    extra trailing fields under PBUF_LIFETIME_DBG, the mismatch would be a
 *    layout mismatch, not a compile error.  Same header, same flags, same
 *    layout, by construction.
 *
 * 2. The vendor's own components/bk_wifi/src/pbuf.c cannot be used.  Its
 *    entire body sits behind #if (CONFIG_FULLY_HOSTED || CONFIG_SEMI_HOSTED),
 *    and turning that on does not help: that file assigns p->total_len while
 *    every other file in the tree reads p->tot_len.  It targets an older
 *    struct and no longer compiles against the header its own siblings use.
 *
 * 3. The closed MAC library does not participate.  arm-none-eabi-nm over all
 *    32 archives in components/bk_libs/bk7258/libs shows libwifi.a
 *    referencing no pbuf_* symbol at all, so the buffer layout is not part
 *    of the closed-source ABI -- only the open sources we compile care.
 *
 * 4. Only six entry points are actually referenced by those sources: alloc,
 *    free, ref, header, coalesce and cat.  pbuf_concat and pbuf_free_all are
 *    used only from within the vendor's own disabled pbuf.c, so they are not
 *    provided here.
 *
 * One assumption worth stating: allocation happens in task context.  The
 * vendor RX path reaches pbuf_alloc() from rw_task, which our OSI layer
 * backs with a real NuttX kernel thread, so the heap is safe there.  If a
 * future path calls in from an ISR this file has to change -- NuttX's
 * allocator is not interrupt-safe.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "pbuf.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Armv8-M wants 4-byte alignment for the header and for the payload that
 * follows it.  lwIP's own MEM_ALIGNMENT is not used here on purpose: this
 * allocator hands out plain heap blocks, so the only alignment that has to
 * be honoured is the one the CPU needs.
 */

#define BK_PBUF_ALIGN(x)  (((x) + 3u) & ~3u)
#define BK_PBUF_HDRSZ     BK_PBUF_ALIGN(sizeof(struct pbuf))

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* malloc()/free() rather than the kmm_* the rest of chip/ uses.  Not a
 * style choice: with CONFIG_MM_KERNEL_HEAP off, kmm_malloc is a macro
 * aliasing malloc, so there is no such symbol to link against -- and this
 * translation unit is compiled with the vendor's include path, which has no
 * NuttX headers to expand the macro.  Declaring it extern here produced two
 * undefined references that looked like a missing library.
 *
 * The heap underneath is the same one either way.
 */

/* The NuttX-side landing point for received frames.  Lives in
 * bk7258_wifi_shim.c, which is compiled with NuttX flags and therefore can
 * talk to the netdev lower half.  The boundary is deliberately a flat
 * (pointer, length) pair so that neither side needs the other's headers.
 */

/* Cross-domain contract: rx_frame, tx_*, vif. */

#include "bk7258_wifi_scan.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pbuf_alloc
 *
 * Description:
 *   Allocate a single unchained pbuf holding 'length' bytes of payload.
 *
 *   'l' is not an enumerator to switch on -- lwIP's pbuf_layer values *are*
 *   byte counts (PBUF_RAW = 0, PBUF_RAW_TX = PBUF_LINK_ENCAPSULATION_HLEN,
 *   PBUF_LINK = that plus the ethernet header, and so on).  Using it
 *   directly as the headroom is what lwIP itself does, and it is what makes
 *   a later pbuf_header() able to walk backwards to prepend a header.
 *
 *   'type' is recorded but not acted on: every buffer here comes from the
 *   one heap, so the PBUF_RAM behaviour (struct and data contiguous, freed
 *   together) is the only one implemented.  Callers asking for PBUF_POOL
 *   get the same thing, which is a superset of what they may do with it.
 *
 ****************************************************************************/

struct pbuf *pbuf_alloc(pbuf_layer l, u16_t length, pbuf_type type)
{
  struct pbuf *p;
  unsigned int offset = (unsigned int)l;
  unsigned int total  = BK_PBUF_HDRSZ + offset + BK_PBUF_ALIGN(length);

  p = (struct pbuf *)malloc(total);
  if (p == NULL)
    {
      return NULL;
    }

  p->next          = NULL;
  p->payload       = (u8_t *)p + BK_PBUF_HDRSZ + offset;
  p->tot_len       = length;
  p->len           = length;
  p->type_internal = (u8_t)type;
  p->flags         = 0;
  p->ref           = 1;
  p->if_idx        = 0;             /* lwIP's NETIF_NO_INDEX, without
                                     * pulling in netif.h for one constant */

  return p;
}

/****************************************************************************
 * Name: pbuf_free
 *
 * Description:
 *   Drop one reference.  Returns the number of pbufs actually released,
 *   which is what lwIP's contract promises and what the vendor RX path
 *   checks in a couple of places.
 *
 *   Freeing walks the chain: a chained pbuf's tail is owned by its head, so
 *   releasing the head releases every link whose own refcount reaches zero.
 *
 ****************************************************************************/

u8_t pbuf_free(struct pbuf *p)
{
  u8_t count = 0;

  while (p != NULL)
    {
      struct pbuf *next;

      if (p->ref > 0)
        {
          p->ref--;
        }

      if (p->ref != 0)
        {
          break;
        }

      next = p->next;
      free(p);
      count++;
      p = next;
    }

  return count;
}

/****************************************************************************
 * Name: pbuf_ref
 ****************************************************************************/

void pbuf_ref(struct pbuf *p)
{
  if (p != NULL)
    {
      p->ref++;
    }
}

/****************************************************************************
 * Name: pbuf_header
 *
 * Description:
 *   Move the payload pointer by 'header_size' bytes: positive grows the
 *   buffer at the front (prepending a header), negative shrinks it from the
 *   front (skipping one).  Returns 0 on success, non-zero on refusal --
 *   note the inverted sense, which is lwIP's, not ours.
 *
 *   The backwards limit is the first byte after the struct, i.e. the
 *   headroom that pbuf_alloc() reserved from the pbuf_layer argument.  A
 *   caller that asks to prepend more than it reserved is refused rather
 *   than allowed to walk into the heap block's own header, which would
 *   corrupt the allocator silently.
 *
 ****************************************************************************/

u8_t pbuf_header(struct pbuf *p, s16_t header_size)
{
  u8_t *floor;
  u8_t *payload;

  if (p == NULL || header_size == 0)
    {
      return 0;
    }

  floor   = (u8_t *)p + BK_PBUF_HDRSZ;
  payload = (u8_t *)p->payload - header_size;

  if (payload < floor)
    {
      return 1;
    }

  if (header_size < 0 && (u16_t)(-header_size) > p->len)
    {
      return 1;
    }

  p->payload  = payload;
  p->len      = (u16_t)(p->len + header_size);
  p->tot_len  = (u16_t)(p->tot_len + header_size);

  return 0;
}

/****************************************************************************
 * Name: pbuf_coalesce
 *
 * Description:
 *   Flatten a chain into one buffer.  An unchained pbuf is returned
 *   unchanged with its reference intact -- that is lwIP's contract, and the
 *   RX path at rwnx_rx.c:569 relies on it (it assigns the result over its
 *   own pointer and would double-free otherwise).
 *
 ****************************************************************************/

struct pbuf *pbuf_coalesce(struct pbuf *p, pbuf_layer layer)
{
  struct pbuf *q;
  struct pbuf *iter;
  u8_t *dst;

  if (p == NULL || p->next == NULL)
    {
      return p;
    }

  q = pbuf_alloc(layer, p->tot_len, PBUF_RAM);
  if (q == NULL)
    {
      return p;
    }

  dst = (u8_t *)q->payload;
  for (iter = p; iter != NULL; iter = iter->next)
    {
      memcpy(dst, iter->payload, iter->len);
      dst += iter->len;
    }

  pbuf_free(p);
  return q;
}

/****************************************************************************
 * Name: ethernetif_input
 *
 * Description:
 *   Where the vendor stack hands a received frame to the IP stack.  In a
 *   Beken build that is lwIP; here it is NuttX, so the frame is flattened
 *   and passed across the flag boundary to bk7258_wifi_rx_frame().
 *
 *   Ownership of 'p' transfers to us, as it does in lwIP.
 *
 ****************************************************************************/

#ifdef BK7258_WIFI_WPA
/* bk_patch/sk_intf.c.  Queues the frame for the supplicant's l2_packet
 * reader and wakes the wpas thread itself.
 */

extern int ke_l2_packet_tx(unsigned char *buf, int len, int flag);
#endif

void ethernetif_input(int iface, struct pbuf *p)
{
  if (p == NULL)
    {
      return;
    }

  if (p->next != NULL)
    {
      p = pbuf_coalesce(p, PBUF_RAW);
    }

#ifdef BK7258_WIFI_WPA
  /* EAPOL belongs to the supplicant, not the IP stack.  This is the same
   * ethertype fork the vendor's own lwIP port makes at exactly this spot
   * (wlanif.c:273): the 4-way handshake frames go to the fake l2 socket
   * (full frame, ethernet header included; 'iface' is the vif index), and
   * without this fork every PSK join would associate and then time out
   * waiting for message 1/4 that went to NuttX instead.
   */

  if (p->len >= 14 &&
      ((u8_t *)p->payload)[12] == 0x88 && ((u8_t *)p->payload)[13] == 0x8e)
    {
      ke_l2_packet_tx(p->payload, p->len, iface);
      pbuf_free(p);
      return;
    }
#endif

  bk7258_wifi_rx_frame(iface, p->payload, p->len);
  pbuf_free(p);
}

/****************************************************************************
 * Name: bk7258_wifi_tx_alloc / bk7258_wifi_tx_send / bk7258_wifi_tx_abort
 *
 * Description:
 *   The TX half of the same seam.  The netdev driver cannot build the
 *   vendor's buffer itself -- struct pbuf only has its real layout under
 *   these flags -- so it asks for one here, fills it through *payload, and
 *   sends it.
 *
 *   PBUF_RAW_TX matters.  bmsg_tx_handler() hands the pbuf to
 *   rwnx_start_xmit(), which wraps it in an sk_buff IN PLACE
 *   (alloc_skb_with_pbuf) -- the CONFIG_MSDU_RESV_HEAD_LENGTH (96) bytes
 *   of headroom in front of the payload are where the descriptor and
 *   802.11 header go.  A PBUF_RAW allocation would have the vendor writing
 *   in front of the buffer.  PBUF_RAW_TX is what the vendor's own lwIP
 *   output path (wlanif.c low_level_output) hands it, so this is the same
 *   shape by construction.
 *
 *   Reference discipline copied from that same caller: bmsg_tx_sender()
 *   takes its own pbuf_ref() and the queue drains it; the caller drops its
 *   own reference when the call returns, success or not (rw_task.c:493-498).
 *
 ****************************************************************************/

extern int bmsg_tx_sender(struct pbuf *p, uint32_t vif_idx);

void *bk7258_wifi_tx_alloc(unsigned int len, uint8_t **payload)
{
  struct pbuf *p;

  if (payload == NULL || len == 0 || len > 0xffff)
    {
      return NULL;
    }

  p = pbuf_alloc(PBUF_RAW_TX, (u16_t)len, PBUF_RAM);
  if (p == NULL)
    {
      return NULL;
    }

  *payload = (uint8_t *)p->payload;
  return p;
}

int bk7258_wifi_tx_send(void *frame)
{
  struct pbuf *p = (struct pbuf *)frame;
  uint8_t vif = bk7258_wifi_vif();
  int ret = -1;

  if (vif != 0xff)
    {
      ret = bmsg_tx_sender(p, vif);
    }

  pbuf_free(p);
  return ret == 0 ? 0 : -1;
}

void bk7258_wifi_tx_abort(void *frame)
{
  pbuf_free((struct pbuf *)frame);
}

/****************************************************************************
 * Name: pbuf_cat
 *
 * Description:
 *   Append 't' to the tail of 'h' and hand its reference over -- lwIP's
 *   pbuf_cat, as distinct from pbuf_chain, which takes a reference of its
 *   own.  The caller must not touch 't' afterwards.
 *
 *   tot_len is a running total over the rest of the chain, so every pbuf
 *   ahead of the join has to be updated, not just the head.
 *
 ****************************************************************************/

void pbuf_cat(struct pbuf *h, struct pbuf *t)
{
  struct pbuf *p;

  if (h == NULL || t == NULL)
    {
      return;
    }

  for (p = h; p->next != NULL; p = p->next)
    {
      p->tot_len = (u16_t)(p->tot_len + t->tot_len);
    }

  p->tot_len = (u16_t)(p->tot_len + t->tot_len);
  p->next    = t;
}
