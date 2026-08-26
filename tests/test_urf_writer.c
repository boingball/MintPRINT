#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urf_writer.h"

struct Sink {
    unsigned char *buf;
    unsigned long size;
    unsigned long cap;
};

static long sink_write(void *ctx, const unsigned char *data, unsigned long len)
{
    struct Sink *sink = (struct Sink *)ctx;
    if (sink->size + len > sink->cap) return -1;
    memcpy(sink->buf + sink->size, data, len);
    sink->size += len;
    return (long)len;
}

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

int main(void)
{
    const unsigned long width = 4;
    const unsigned long height = 2;
    unsigned long scratch_size = mp_urf_scratch_size(width);
    unsigned char *scratch = (unsigned char *)malloc(scratch_size);
    unsigned char out_buf[4096];
    struct Sink sink;
    MPUrfEncoder enc;
    unsigned char row0[4 * 3];
    unsigned char row1[4 * 3];
    unsigned long expected_row0_len;
    const unsigned char *p;

    if (!scratch) return 1;
    if (scratch_size != width * 4UL + 16UL) return 2;

    /* Reject bad begin() arguments before touching a real sink. */
    if (mp_urf_begin(NULL, width, height, 300, scratch, scratch_size,
                     sink_write, NULL)) return 3;
    {
        MPUrfEncoder bad;
        if (mp_urf_begin(&bad, 0, height, 300, scratch, scratch_size,
                         sink_write, NULL)) return 4;
        if (mp_urf_begin(&bad, width, height, 300, scratch, 4UL,
                         sink_write, NULL)) return 5;
        if (mp_urf_begin(&bad, 70000UL, height, 300, scratch,
                         mp_urf_scratch_size(70000UL), sink_write, NULL))
            return 6;
    }

    sink.buf = out_buf;
    sink.size = 0;
    sink.cap = sizeof(out_buf);

    if (!mp_urf_begin(&enc, width, height, 300, scratch, scratch_size,
                      sink_write, &sink)) {
        free(scratch);
        return 10;
    }

    /* File header (12 bytes) + page header (32 bytes) = 44 bytes, written
     * synchronously by mp_urf_begin - see urf_writer.c's file comment. */
    if (sink.size != 44UL) { free(scratch); return 11; }

    p = out_buf;
    if (memcmp(p, "UNIRAST", 7) != 0) { free(scratch); return 12; }
    if (p[7] != 0) { free(scratch); return 13; }
    if (be32(p + 8) != 1UL) { free(scratch); return 14; } /* page count */

    p = out_buf + 12; /* page header */
    if (p[0] != 24) { free(scratch); return 15; }  /* cupsBitsPerPixel */
    if (p[1] != 1) { free(scratch); return 16; }    /* colorspace: sRGB */
    if (p[2] != 1) { free(scratch); return 17; }    /* duplex: simplex */
    if (p[3] != 0 || p[4] != 0 || p[5] != 0) { free(scratch); return 18; }
    { /* bytes 6-11: reserved, zero */
        int i;
        for (i = 6; i <= 11; ++i)
            if (p[i] != 0) { free(scratch); return 19; }
    }
    if (be32(p + 12) != width) { free(scratch); return 20; }
    if (be32(p + 16) != height) { free(scratch); return 21; }
    if (be32(p + 20) != 300UL) { free(scratch); return 22; }
    { /* bytes 24-31: reserved, zero */
        int i;
        for (i = 24; i <= 31; ++i)
            if (p[i] != 0) { free(scratch); return 23; }
    }

    /* Row 0: pixels (1,2,3),(1,2,3),(9,9,9),(9,9,9) - two runs of two. */
    row0[0] = 1; row0[1] = 2; row0[2] = 3;
    row0[3] = 1; row0[4] = 2; row0[5] = 3;
    row0[6] = 9; row0[7] = 9; row0[8] = 9;
    row0[9] = 9; row0[10] = 9; row0[11] = 9;
    if (!mp_urf_write_scanline(&enc, row0)) { free(scratch); return 30; }

    /* line-repeat(1) + control+pixel(4) + control+pixel(4) = 9 bytes. */
    expected_row0_len = 9UL;
    if (sink.size != 44UL + expected_row0_len) { free(scratch); return 31; }
    p = out_buf + 44;
    if (p[0] != 0) { free(scratch); return 32; } /* line appears once */
    if (p[1] != 1 || p[2] != 1 || p[3] != 2 || p[4] != 3) {
        free(scratch); return 33; /* run of 2, pixel (1,2,3) */
    }
    if (p[5] != 1 || p[6] != 9 || p[7] != 9 || p[8] != 9) {
        free(scratch); return 34; /* run of 2, pixel (9,9,9) */
    }

    /* Row 1: four distinct, non-repeating pixels - each its own trivial
     * run of one (control byte 0), never the format's literal-run half. */
    row1[0] = 1; row1[1] = 1; row1[2] = 1;
    row1[3] = 2; row1[4] = 2; row1[5] = 2;
    row1[6] = 3; row1[7] = 3; row1[8] = 3;
    row1[9] = 4; row1[10] = 4; row1[11] = 4;
    if (!mp_urf_write_scanline(&enc, row1)) { free(scratch); return 40; }

    if (sink.size != 44UL + expected_row0_len + 1UL + 4UL * 4UL) {
        free(scratch); return 41;
    }
    p = out_buf + 44 + expected_row0_len;
    if (p[0] != 0) { free(scratch); return 42; }
    {
        int i;
        for (i = 0; i < 4; ++i) {
            unsigned char v = (unsigned char)(i + 1);
            if (p[1 + i * 4] != 0 || p[1 + i * 4 + 1] != v ||
                p[1 + i * 4 + 2] != v || p[1 + i * 4 + 3] != v) {
                free(scratch);
                return 43;
            }
        }
    }

    /* Writing beyond the declared height must fail. */
    if (mp_urf_write_scanline(&enc, row1)) { free(scratch); return 50; }

    if (!mp_urf_finish(&enc)) { free(scratch); return 51; }

    free(scratch);
    return 0;
}
