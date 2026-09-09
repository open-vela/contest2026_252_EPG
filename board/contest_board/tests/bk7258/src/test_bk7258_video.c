/****************************************************************************
 * board/contest_board/tests/bk7258/src/test_bk7258_video.c
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

#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "test_bk7258.h"

#ifdef CONFIG_TESTING_BK7258_VIDEO

#include "test_bk7258_image.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The decoder's AHB master over-reads past the end of the entropy data, so
 * the input buffer has to carry readable slack; the driver programs the
 * read length as len + 2048 and this is that same 2 KB.  Handing it the
 * const array directly would have it read whatever follows in .rodata.
 */

#define TB_JPEG_SLACK      2048

#define TB_YUYV_BYTES      (TEST_JPEG_WIDTH * TEST_JPEG_HEIGHT * 2)
#define TB_PANEL_BYTES     (TB_PANEL_W * TB_PANEL_H * 2)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR uint8_t  *g_jpeg;      /* JPEG plus its over-read slack        */
static FAR uint8_t  *g_yuyv;      /* Decoder output, packed 4:2:2         */
static FAR uint16_t *g_rgbl;      /* DMA2D output, left panel             */
static FAR uint16_t *g_rgbr;      /* DMA2D output, right panel            */
static int           g_decode_ret = -1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tb_expand5 / tb_expand6
 *
 * Description:
 *   RGB565 field back to eight bits by replicating the high bits into the
 *   low ones, which is what maps 0x1f to 0xff rather than to 0xf8.  The
 *   reference values are full eight-bit components, so the comparison has
 *   to happen in that space; the residual quantisation is at most six
 *   counts and sits well inside the tolerance.
 *
 ****************************************************************************/

static int tb_expand5(unsigned int v)
{
  return (int)((v << 3) | (v >> 2));
}

static int tb_expand6(unsigned int v)
{
  return (int)((v << 2) | (v >> 4));
}

/****************************************************************************
 * Name: tb_panel_pixel
 *
 * Description:
 *   The RGB565 word a frame coordinate ends up as after the split: the
 *   left 160 columns go to dstl, the right 160 to dstr, both at the same
 *   row and at the column reduced modulo the panel width.
 *
 ****************************************************************************/

static uint16_t tb_panel_pixel(int x, int y)
{
  if (x < TB_PANEL_W)
    {
      return g_rgbl[y * TB_PANEL_W + x];
    }

  return g_rgbr[y * TB_PANEL_W + (x - TB_PANEL_W)];
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: test_bk7258_video_setup
 *
 * Description:
 *   Fresh buffers, poisoned outputs, the production input-format setting,
 *   and one decode of the embedded frame.  Allocating per case rather than
 *   once for the module is deliberate: it guarantees neither case can read
 *   what the other left behind, and it makes a leak in the decode path
 *   show up as an allocation failure here instead of much later.
 *
 *   The decode itself belongs in the fixture because both cases need a
 *   decoded frame -- the splitter case needs one as its input -- and
 *   running it twice on the same buffers would test the decoder's second
 *   pass rather than the splitter.
 *
 ****************************************************************************/

int test_bk7258_video_setup(FAR void **state)
{
  UNUSED(state);

  g_decode_ret = -1;

  g_jpeg = memalign(32, TEST_JPEG_LEN + TB_JPEG_SLACK);
  g_yuyv = memalign(32, TB_YUYV_BYTES);
  g_rgbl = memalign(4, TB_PANEL_BYTES);
  g_rgbr = memalign(4, TB_PANEL_BYTES);

  if (g_jpeg == NULL || g_yuyv == NULL || g_rgbl == NULL || g_rgbr == NULL)
    {
      /* cmocka skips the teardown when a setup fails, so the partial
       * allocation has to be released here or the next case inherits a
       * smaller heap than this one had.
       */

      test_bk7258_video_teardown(state);
      return -1;
    }

  memcpy(g_jpeg, g_test_jpeg, TEST_JPEG_LEN);
  memset(g_jpeg + TEST_JPEG_LEN, 0, TB_JPEG_SLACK);

  memset(g_yuyv, TB_POISON, TB_YUYV_BYTES);
  memset(g_rgbl, TB_POISON, TB_PANEL_BYTES);
  memset(g_rgbr, TB_POISON, TB_PANEL_BYTES);

  /* Format code 0 on both blocks with no byte reversal: the pairing the
   * face application calibrated against the cream background and ships
   * with.  Restating it here keeps a case from inheriting whatever the
   * calibration sweep last left in the driver's static.
   */

  bk7258_dma2d_set_fgcfg(0, 0);

  g_decode_ret = bk7258_jpegdec_decode(g_jpeg, TEST_JPEG_LEN, g_yuyv,
                                       TEST_JPEG_WIDTH, TEST_JPEG_HEIGHT);
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_video_teardown
 ****************************************************************************/

int test_bk7258_video_teardown(FAR void **state)
{
  UNUSED(state);

  free(g_jpeg);
  free(g_yuyv);
  free(g_rgbl);
  free(g_rgbr);

  g_jpeg = NULL;
  g_yuyv = NULL;
  g_rgbl = NULL;
  g_rgbr = NULL;
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_jpegdec_decode
 *
 * Description:
 *   Decode the embedded frame and compare eight sample points against what
 *   a host-side reference decoder produced from the very same bytes.
 *
 *   The frame is four solid quadrants whose luma values are 73, 88, 124
 *   and 210 and whose chroma pairs are far apart, and the sample points
 *   sit two per quadrant, at least 24 pixels from every boundary.  That
 *   makes the comparison a geometry test as well as an arithmetic one: a
 *   decode that came out transposed, half-line shifted, written with the
 *   wrong row stride, or with the chroma pair swapped lands on a different
 *   quadrant's numbers and misses by far more than the tolerance.  A block
 *   that never wrote anything leaves the poison, which is 32 counts clear
 *   of every reference component, so an untouched buffer fails too.
 *
 *   The tolerance covers the difference between the hardware IDCT and the
 *   reference one on identical coefficients.  It does not cover a
 *   different picture.
 *
 ****************************************************************************/

void test_bk7258_jpegdec_decode(FAR void **state)
{
  int i;

  UNUSED(state);

  assert_non_null(g_yuyv);
  assert_int_equal(g_decode_ret, 0);

  for (i = 0; i < TEST_JPEG_NPOINTS; i++)
    {
      FAR const struct test_jpeg_point_s *p = &g_test_jpeg_points[i];
      FAR const uint8_t *grp;

      /* Every sample point starts on an even column, so it is the first
       * pixel of a whole Y Cb Y Cr group.
       */

      grp = &g_yuyv[(p->y * TEST_JPEG_WIDTH + p->x) * 2];

      assert_in_range(tb_absdiff(grp[0], p->luma), 0, TB_TOL_YUV);
      assert_in_range(tb_absdiff(grp[1], p->cb), 0, TB_TOL_YUV);
      assert_in_range(tb_absdiff(grp[3], p->cr), 0, TB_TOL_YUV);
    }
}

/****************************************************************************
 * Name: test_bk7258_dma2d_split
 *
 * Description:
 *   Feed the decoded frame to the splitter and check the two panel buffers
 *   against the reference colours, converted independently on the host.
 *
 *   Three separate things can fail here.  The conversion itself: a colour
 *   that comes back with Cb and Cr exchanged is over a hundred counts out
 *   on these saturated quadrants, several times the tolerance.  The
 *   windowing: the source line offset is what makes the second pass read
 *   the right-hand columns, and getting it wrong hands both panels the
 *   same half -- which the explicit inequality catches even where both
 *   halves would otherwise be plausible pictures.  And the run itself: an
 *   untouched buffer still holds the poison, which as an RGB565 pair is
 *   over a hundred counts from every reference colour.
 *
 ****************************************************************************/

void test_bk7258_dma2d_split(FAR void **state)
{
  int i;

  UNUSED(state);

  assert_non_null(g_yuyv);
  assert_int_equal(g_decode_ret, 0);

  assert_int_equal(bk7258_dma2d_split(g_yuyv, g_rgbl, g_rgbr), 0);

  for (i = 0; i < TEST_JPEG_NPOINTS; i++)
    {
      FAR const struct test_jpeg_point_s *p = &g_test_jpeg_points[i];
      uint16_t pix = tb_panel_pixel(p->x, p->y);

      assert_in_range(tb_absdiff(tb_expand5((pix >> 11) & 0x1f), p->r),
                      0, TB_TOL_RGB);
      assert_in_range(tb_absdiff(tb_expand6((pix >> 5) & 0x3f), p->g),
                      0, TB_TOL_RGB);
      assert_in_range(tb_absdiff(tb_expand5(pix & 0x1f), p->b),
                      0, TB_TOL_RGB);
    }

  /* The two panels carry different quadrants of the frame, so identical
   * words at the same panel coordinate mean the second pass re-read the
   * first half instead of stepping over it.
   */

  assert_true(g_rgbl[24 * TB_PANEL_W + 40] !=
              g_rgbr[24 * TB_PANEL_W + 40]);
  assert_true(g_rgbl[136 * TB_PANEL_W + 120] !=
              g_rgbr[136 * TB_PANEL_W + 120]);
}

#endif /* CONFIG_TESTING_BK7258_VIDEO */
