#!/usr/bin/env python3
"""Generate include/test_bk7258_image.h: a 320x160 baseline 4:2:2 JPEG plus
the host-computed ground truth the on-target cases assert against.

The hardware JPEG decoder in chip/bk7258_jpegdec.c only accepts a narrow
profile: three-component baseline, Y sampling 0x21 (4:2:2), chroma 0x11,
quantisation ids Y=0 / C=1, Huffman selectors 00/11, no progressive coding,
no restart interval, and every header inside the first 1 KB.  This script
builds such a file, re-decodes it with the reference decoder, and writes the
resulting luma/chroma and RGB values out as constants -- so the target test
compares hardware output against an independently computed expectation
rather than against itself.

Run from anywhere:  python3 tools/gen_test_image.py
"""

import io
import os
import sys

from PIL import Image

W, H = 320, 160

# Four quadrants, deliberately far apart in both luma and chroma so a
# transposed, half-shifted or chroma-swapped decode cannot pass.  The
# bottom-right value is the vendor's cream background, the colour the
# DMA2D input format was originally calibrated against (face CAL).
QUADRANTS = [
    ((0, 0, 160, 80), (200, 40, 40)),        # top-left     crimson
    ((160, 0, 320, 80), (40, 180, 60)),      # top-right    green
    ((0, 80, 160, 160), (50, 60, 200)),      # bottom-left  blue
    ((160, 80, 320, 160), (222, 210, 181)),  # bottom-right cream
]

# Sample points, frame coordinates.  All x are even so each point starts a
# whole 4-byte 4:2:2 group, and all sit at least 24 px away from every
# quadrant boundary so JPEG ringing at the edges cannot reach them.
POINTS = [
    (40, 24), (120, 56),      # top-left
    (200, 24), (280, 56),     # top-right
    (40, 104), (120, 136),    # bottom-left
    (200, 104), (280, 136),   # bottom-right
]

QUALITY = 92


def build_jpeg():
    img = Image.new("RGB", (W, H))
    for box, colour in QUADRANTS:
        img.paste(colour, box)

    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=QUALITY, subsampling=1,
             optimize=False, progressive=False)
    return buf.getvalue()


def check_profile(jpg):
    """Walk the markers exactly the way bk7258_jpegdec_decode() does and
    refuse anything it would refuse, so a Pillow upgrade cannot silently
    produce a file the hardware rejects."""
    assert jpg[0] == 0xFF and jpg[1] == 0xD8, "no SOI"

    i = 2
    seen = {"dqt": set(), "dht": set(), "sof": False, "sos": 0}
    while i + 4 <= len(jpg) and i < 1024:
        assert jpg[i] == 0xFF, "marker desync at %d" % i
        marker = jpg[i + 1]
        seglen = (jpg[i + 2] << 8) | jpg[i + 3]

        if marker == 0xDB:
            o = i + 4
            while o + 65 <= i + 2 + seglen:
                assert (jpg[o] & 0xF0) == 0, "16-bit quant table"
                seen["dqt"].add(jpg[o] & 1)
                o += 65
        elif marker == 0xC4:
            o = i + 4
            while o + 17 <= i + 2 + seglen:
                total = sum(jpg[o + 1:o + 17])
                seen["dht"].add((jpg[o] & 1, (jpg[o] >> 4) & 1))
                o += 17 + total
        elif marker == 0xC0:
            w = (jpg[i + 7] << 8) | jpg[i + 8]
            h = (jpg[i + 5] << 8) | jpg[i + 6]
            assert (w, h) == (W, H), "SOF0 geometry %dx%d" % (w, h)
            assert jpg[i + 9] == 3, "not three components"
            assert jpg[i + 11] == 0x21, "Y sampling 0x%02x" % jpg[i + 11]
            assert jpg[i + 14] == 0x11 and jpg[i + 17] == 0x11, "chroma"
            assert jpg[i + 12] == 0 and jpg[i + 15] == 1, "quant ids"
            seen["sof"] = True
        elif marker == 0xDD:
            raise AssertionError("restart interval present")
        elif marker in (0xC2, 0xC1, 0xC3):
            raise AssertionError("not baseline sequential")
        elif marker == 0xDA:
            assert jpg[i + 6] == 0x00, "Y huffman selector"
            assert jpg[i + 8] == 0x11 and jpg[i + 10] == 0x11, "C selectors"
            seen["sos"] = i
            break
        elif marker == 0xD9 or 0xD0 <= marker <= 0xD7:
            raise AssertionError("marker 0x%02x inside header walk" % marker)

        i += 2 + seglen

    assert seen["sof"], "no SOF0"
    assert seen["sos"], "no SOS inside the first 1 KB"
    assert seen["dqt"] == {0, 1}, "quant tables %s" % seen["dqt"]
    assert seen["dht"] == {(0, 0), (0, 1), (1, 0), (1, 1)}, "huffman tables"
    return seen["sos"]


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, os.pardir, "include", "test_bk7258_image.h")

    jpg = build_jpeg()
    sos = check_profile(jpg)

    # Ground truth: what a reference decoder makes of the very same bytes.
    # Feeding the target the encoder's *input* colours instead would hide
    # the encoder's own quantisation inside the comparison tolerance.
    #
    # draft() makes libjpeg hand back its native YCbCr, so the luma and
    # chroma truth is the decoder's own output rather than a YCbCr -> RGB
    # -> YCbCr round trip, whose clipping would show up as error the
    # hardware is not responsible for.
    ycc = Image.open(io.BytesIO(jpg))
    ycc.draft("YCbCr", ycc.size)
    ycc.load()
    assert ycc.mode == "YCbCr", "libjpeg would not hand back YCbCr"

    rgb = Image.open(io.BytesIO(jpg))
    rgb.load()
    rgb = rgb.convert("RGB")

    rows = []
    for (x, y) in POINTS:
        yy, cb, cr = ycc.getpixel((x, y))
        r, g, b = rgb.getpixel((x, y))
        rows.append((x, y, yy, cb, cr, r, g, b, rgb565(r, g, b)))

    body = []
    for n, byte in enumerate(jpg):
        if n % 12 == 0:
            body.append("\n ")
        body.append(" 0x%02x," % byte)

    with open(out, "w") as fp:
        fp.write(HEADER_TOP % dict(
            len=len(jpg), w=W, h=H, sos=sos, quality=QUALITY,
            npoints=len(POINTS)))
        fp.write("static const unsigned char g_test_jpeg[%d] =\n{" % len(jpg))
        fp.write("".join(body).rstrip(","))
        fp.write("\n};\n\n")
        fp.write(TRUTH_TOP)
        for (x, y, yy, cb, cr, r, g, b, p565) in rows:
            fp.write("  {\n")
            fp.write("    %3d, %3d, %3d, %3d, %3d, %3d, %3d, %3d, 0x%04x\n"
                     % (x, y, yy, cb, cr, r, g, b, p565))
            fp.write("  },\n")
        fp.write("};\n\n#endif /* __TESTS_BK7258_INCLUDE_TEST_BK7258_IMAGE_H */\n")

    sys.stderr.write("wrote %s: %d JPEG bytes, SOS at %d, %d truth points\n"
                     % (out, len(jpg), sos, len(rows)))


HEADER_TOP = """\
/****************************************************************************
 * board/contest_board/tests/bk7258/include/test_bk7258_image.h
 *
 * GENERATED FILE -- regenerate with tools/gen_test_image.py, do not edit.
 *
 * A %(w)dx%(h)d baseline JPEG (4:2:2, Y sampling 0x21, quality %(quality)d,
 * headers end at byte %(sos)d) drawn as four solid quadrants, together with
 * the pixel values an independent host-side decoder produces from these
 * exact bytes.  The target cases compare the hardware decoder and the
 * DMA2D converter against those numbers, which is why the ground truth is
 * taken from the decoded image rather than from the colours that were
 * encoded: the encoder's own loss must sit outside the comparison, not
 * inside its tolerance.
 *
 ****************************************************************************/

#ifndef __TESTS_BK7258_INCLUDE_TEST_BK7258_IMAGE_H
#define __TESTS_BK7258_INCLUDE_TEST_BK7258_IMAGE_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TEST_JPEG_LEN      %(len)d
#define TEST_JPEG_WIDTH    %(w)d
#define TEST_JPEG_HEIGHT   %(h)d
#define TEST_JPEG_NPOINTS  %(npoints)d

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct test_jpeg_point_s
{
  int x;              /* Frame coordinate, always even so the point starts */
  int y;              /* a whole 4:2:2 group                              */
  int luma;           /* Reference Y, Cb, Cr at that pixel                 */
  int cb;
  int cr;
  int r;              /* Reference R, G, B and the RGB565 word it packs to */
  int g;
  int b;
  unsigned short rgb565;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

"""

TRUTH_TOP = """\
static const struct test_jpeg_point_s g_test_jpeg_points[TEST_JPEG_NPOINTS] =
{
"""


if __name__ == "__main__":
    main()
