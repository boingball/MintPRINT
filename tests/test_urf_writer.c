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

static int test_single_page(void)
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
    if (mp_urf_begin(NULL, width, height, 300, 4, scratch, scratch_size,
                     sink_write, NULL)) return 3;
    {
        MPUrfEncoder bad;
        if (mp_urf_begin(&bad, 0, height, 300, 4, scratch, scratch_size,
                         sink_write, NULL)) return 4;
        if (mp_urf_begin(&bad, width, height, 300, 4, scratch, 4UL,
                         sink_write, NULL)) return 5;
        if (mp_urf_begin(&bad, 70000UL, height, 300, 4, scratch,
                         mp_urf_scratch_size(70000UL), sink_write, NULL))
            return 6;
    }

    sink.buf = out_buf;
    sink.size = 0;
    sink.cap = sizeof(out_buf);

    if (!mp_urf_begin(&enc, width, height, 300, 4, scratch, scratch_size,
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
    if (p[2] != 1) { free(scratch); return 17; }    /* simplex */
    if (p[3] != 4 || p[4] != 0 || p[5] != 0) { free(scratch); return 18; }
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

/* Two-page duplex stream: the 12-byte file header (with its placeholder
 * page count) is written exactly once, each page gets its own 32-byte
 * header with the correct duplex/tumble byte, and no per-page reversal
 * happens - see urf_writer.h and driver_core.c's mp_job_begin() for why
 * URF backsides stream in the same natural row order as any front page. */
static int test_duplex(void)
{
    const unsigned long width = 2;
    const unsigned long height = 1;
    unsigned long scratch_size = mp_urf_scratch_size(width);
    unsigned char *scratch = (unsigned char *)malloc(scratch_size);
    unsigned char out_buf[4096];
    struct Sink sink;
    MPUrfEncoder enc;
    unsigned char row[2 * 3];
    const unsigned char *p;
    unsigned long page1_end;
    unsigned long page2_header_off;

    if (!scratch) return 1;

    sink.buf = out_buf;
    sink.size = 0;
    sink.cap = sizeof(out_buf);

    row[0] = 5; row[1] = 6; row[2] = 7;
    row[3] = 5; row[4] = 6; row[5] = 7;

    /* Page 1 (front): write_file_header=1, duplex=1, tumble=0 -
     * two-sided-long-edge -> duplex byte 3. */
    if (!mp_urf_begin_page(&enc, width, height, 300, 4, 1, 1, 0,
                           scratch, scratch_size, sink_write, &sink)) {
        free(scratch); return 10;
    }
    if (sink.size != 44UL) { free(scratch); return 11; }
    if (memcmp(out_buf, "UNIRAST", 7) != 0) { free(scratch); return 12; }
    /* Placeholder page count: "unknown/streaming", not the real total -
     * see MP_URF_PAGECOUNT_FIELD_OFFSET. */
    if (be32(out_buf + MP_URF_PAGECOUNT_FIELD_OFFSET) != 0xffffffffUL) {
        free(scratch); return 13;
    }
    if (out_buf[12 + 2] != 3) { free(scratch); return 14; } /* duplex, long side */
    if (out_buf[12 + 3] != 4) { free(scratch); return 15; } /* normal quality */
    if (!mp_urf_write_scanline(&enc, row)) { free(scratch); return 16; }
    if (!mp_urf_finish(&enc)) { free(scratch); return 17; }
    page1_end = sink.size;

    /* Page 2 (back): write_file_header=0 - no second file header, and the
     * stream continues appending straight after page 1's rows. */
    if (!mp_urf_begin_page(&enc, width, height, 300, 4, 0, 1, 0,
                           scratch, scratch_size, sink_write, &sink)) {
        free(scratch); return 20;
    }
    page2_header_off = page1_end;
    if (sink.size != page1_end + 32UL) { free(scratch); return 21; }
    p = out_buf + page2_header_off;
    if (p[2] != 3) { free(scratch); return 22; } /* duplex, long side, again */
    if (be32(p + 12) != width || be32(p + 16) != height) {
        free(scratch); return 23;
    }
    if (!mp_urf_write_scanline(&enc, row)) { free(scratch); return 24; }
    if (!mp_urf_finish(&enc)) { free(scratch); return 25; }

    /* Patch the placeholder with the real total (2), exactly as
     * driver_core.c's DriverClose() does before closing the file - the
     * decoder-facing contract this whole mechanism exists for. */
    {
        unsigned char *count_field = out_buf + MP_URF_PAGECOUNT_FIELD_OFFSET;
        count_field[0] = 0; count_field[1] = 0;
        count_field[2] = 0; count_field[3] = 2;
    }
    if (be32(out_buf + MP_URF_PAGECOUNT_FIELD_OFFSET) != 2UL) {
        free(scratch); return 30;
    }

    free(scratch);
    return 0;
}

/* Tumble byte value: two-sided-short-edge -> duplex, short side (2), not
 * duplex, long side (3) - confirms tumble is wired through, not just
 * hardcoded to one value. */
static int test_tumble_byte(void)
{
    const unsigned long width = 2;
    const unsigned long height = 1;
    unsigned long scratch_size = mp_urf_scratch_size(width);
    unsigned char *scratch = (unsigned char *)malloc(scratch_size);
    unsigned char out_buf[256];
    struct Sink sink;
    MPUrfEncoder enc;

    if (!scratch) return 1;
    sink.buf = out_buf;
    sink.size = 0;
    sink.cap = sizeof(out_buf);

    if (!mp_urf_begin_page(&enc, width, height, 300, 4, 1, 1, 1,
                           scratch, scratch_size, sink_write, &sink)) {
        free(scratch); return 10;
    }
    if (out_buf[12 + 2] != 2) { free(scratch); return 11; }

    free(scratch);
    return 0;
}

/* The HP Color LaserJet M255/M256 advertises PQ3-4-5 and rejects the old
 * unspecified value 0. Confirm both config translation and the exact byte
 * written for every supported quality, including the normal fallback. */
static int test_quality_byte(void)
{
    static const unsigned long inputs[] = { 3UL, 4UL, 5UL, 0UL, 99UL };
    static const unsigned char expected[] = { 3, 4, 5, 4, 4 };
    const unsigned long width = 1;
    const unsigned long height = 1;
    unsigned long scratch_size = mp_urf_scratch_size(width);
    unsigned char *scratch = (unsigned char *)malloc(scratch_size);
    unsigned char out_buf[128];
    struct Sink sink;
    MPUrfEncoder enc;
    unsigned int i;

    if (!scratch) return 1;
    if (mp_urf_quality_value(NULL) != 4UL ||
        mp_urf_quality_value("") != 4UL ||
        mp_urf_quality_value("draft") != 3UL ||
        mp_urf_quality_value("normal") != 4UL ||
        mp_urf_quality_value("high") != 5UL ||
        mp_urf_quality_value("3") != 3UL ||
        mp_urf_quality_value("4") != 4UL ||
        mp_urf_quality_value("5") != 5UL ||
        mp_urf_quality_value("unknown") != 4UL) {
        free(scratch); return 2;
    }

    for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        sink.buf = out_buf;
        sink.size = 0;
        sink.cap = sizeof(out_buf);
        if (!mp_urf_begin(&enc, width, height, 600, inputs[i],
                          scratch, scratch_size, sink_write, &sink)) {
            free(scratch); return 10 + (int)i;
        }
        if (sink.size != 44UL || out_buf[12 + 2] != 1 ||
            out_buf[12 + 3] != expected[i]) {
            free(scratch); return 20 + (int)i;
        }
    }

    free(scratch);
    return 0;
}

/* Strip-printing accumulation: a page begins at one band's height, grows
 * as more NOFORMFEED bands arrive (mp_urf_grow(), the same growable-
 * declared-height contract pwg_writer.c's mp_pwg_grow() already offers),
 * accepts rows up to the new total, and the page header's height field
 * gets patched with the final total once known - exactly what
 * driver_core.c's mp_page_finalize() does for a real strip-printed page. */
static int test_grow_and_patch(void)
{
    const unsigned long width = 2;
    unsigned long scratch_size = mp_urf_scratch_size(width);
    unsigned char *scratch = (unsigned char *)malloc(scratch_size);
    unsigned char out_buf[512];
    struct Sink sink;
    MPUrfEncoder enc;
    unsigned char row[2 * 3];
    unsigned char *height_field;

    if (!scratch) return 1;
    sink.buf = out_buf;
    sink.size = 0;
    sink.cap = sizeof(out_buf);
    row[0] = 1; row[1] = 1; row[2] = 1;
    row[3] = 2; row[4] = 2; row[5] = 2;

    /* First band: declares height 1, matching the first NOFORMFEED band's
     * own height - exactly like case 0's initial mp_job_begin() call. */
    if (!mp_urf_begin_page(&enc, width, 1, 300, 4, 1, 0, 0,
                           scratch, scratch_size, sink_write, &sink)) {
        free(scratch); return 10;
    }
    if (be32(out_buf + 12 + 16) != 1UL) { free(scratch); return 11; }
    if (!mp_urf_write_scanline(&enc, row)) { free(scratch); return 12; }

    /* Growing by 0 or past 65535 must fail without touching enc->height. */
    if (mp_urf_grow(&enc, 0)) { free(scratch); return 13; }
    if (mp_urf_grow(&enc, 65535UL)) { free(scratch); return 14; }
    if (enc.height != 1UL) { free(scratch); return 15; }

    /* A second and third band arrive (mp_job_reserve_page's job) - grow to
     * accept them without a new header or losing what's already written. */
    if (!mp_urf_grow(&enc, 2)) { free(scratch); return 16; }
    if (enc.height != 3UL) { free(scratch); return 17; }
    if (!mp_urf_write_scanline(&enc, row)) { free(scratch); return 18; }
    if (!mp_urf_write_scanline(&enc, row)) { free(scratch); return 19; }
    if (!mp_urf_finish(&enc)) { free(scratch); return 20; }

    /* The on-the-wire header still says height=1 - only the encoder's own
     * declared cap changed until this patch happens, the same two-step
     * mp_page_finalize() relies on. */
    if (be32(out_buf + 12 + 16) != 1UL) { free(scratch); return 21; }

    height_field = out_buf + 12 + MP_URF_HEIGHT_FIELD_OFFSET;
    height_field[0] = 0; height_field[1] = 0;
    height_field[2] = 0; height_field[3] = 3;
    if (be32(out_buf + 12 + 16) != 3UL) { free(scratch); return 22; }

    free(scratch);
    return 0;
}

int main(void)
{
    int rc;

    rc = test_single_page();
    if (rc) return rc;

    rc = test_duplex();
    if (rc) return 100 + rc;

    rc = test_tumble_byte();
    if (rc) return 200 + rc;

    rc = test_quality_byte();
    if (rc) return 300 + rc;

    rc = test_grow_and_patch();
    if (rc) return 400 + rc;

    return 0;
}
