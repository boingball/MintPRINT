#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/jpeg_writer.h"

static const double mp_dct_ref[8][8] = {
    { 362, 362, 362, 362, 362, 362, 362, 362 },
    { 502, 426, 284, 100,-100,-284,-426,-502 },
    { 473, 196,-196,-473,-473,-196, 196, 473 },
    { 426,-100,-502,-284, 284, 502, 100,-426 },
    { 362,-362,-362, 362, 362,-362,-362, 362 },
    { 284,-502, 100, 426,-426,-100, 502,-284 },
    { 196,-473, 473,-196,-196, 473,-473, 196 },
    { 100,-284, 426,-502, 502,-426, 284,-100 }
};

static void mp_ref_idct_block(const long *coeff, double *pixel)
{
    double tmp[64];
    int y, x, u, v;

    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            double sum = 0.0;
            for (v = 0; v < 8; ++v)
                sum += (mp_dct_ref[v][x] / 1024.0) *
                       (double)coeff[y * 8 + v];
            tmp[y * 8 + x] = sum;
        }
    }
    for (x = 0; x < 8; ++x) {
        for (y = 0; y < 8; ++y) {
            double sum = 0.0;
            for (u = 0; u < 8; ++u)
                sum += (mp_dct_ref[u][y] / 1024.0) * tmp[u * 8 + x];
            pixel[y * 8 + x] = sum;
        }
    }
}

static const unsigned char test_qtable[64] = {
     8, 9,10,11,12,13,14,15,
     9,10,11,12,13,14,15,16,
    10,11,12,13,14,15,16,17,
    11,12,13,14,15,16,17,18,
    12,13,14,15,16,17,18,19,
    13,14,15,16,17,18,19,20,
    14,15,16,17,18,19,20,21,
    15,16,17,18,19,20,21,22
};

static double mp_round_trip_max_error(const short *block)
{
    long coeff[64];
    long dequant[64];
    double pixel[64];
    double max_err = 0.0;
    int k;

    mp_fdct(block, coeff);
    for (k = 0; k < 64; ++k) {
        int q = mp_quantize_aan(coeff[k], (int)test_qtable[k],
                                mp_fdct_aan_scale[k]);
        dequant[k] = (long)q * (long)test_qtable[k];
    }
    mp_ref_idct_block(dequant, pixel);

    for (k = 0; k < 64; ++k) {
        double err = pixel[k] - (double)block[k];
        if (err < 0) err = -err;
        if (err > max_err) max_err = err;
    }
    return max_err;
}

static void fill_flat(short *block, int value)
{
    int i;
    for (i = 0; i < 64; ++i) block[i] = (short)value;
}

static void fill_gradient(short *block)
{
    int y, x;
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x)
            block[y * 8 + x] = (short)((y * 16 + x * 16) - 128);
}

static void fill_checkerboard(short *block)
{
    int y, x;
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x)
            block[y * 8 + x] =
                (short)(((y + x) & 1) ? 127 : -128);
}

static void fill_random(short *block, unsigned int *seed)
{
    int i;
    for (i = 0; i < 64; ++i) {
        *seed = *seed * 1103515245u + 12345u;
        block[i] = (short)((int)((*seed >> 16) % 256) - 128);
    }
}

struct TestSink {
    unsigned long bytes;
    unsigned long calls;
    unsigned char header[32];
    unsigned long header_used;
};

static long test_sink_write(void *ctx, const unsigned char *data,
                            unsigned long len)
{
    struct TestSink *sink = (struct TestSink *)ctx;
    if (sink->header_used < sizeof(sink->header)) {
        unsigned long room = sizeof(sink->header) - sink->header_used;
        unsigned long copy = len < room ? len : room;
        memcpy(sink->header + sink->header_used, data, copy);
        sink->header_used += copy;
    }
    sink->bytes += len;
    ++sink->calls;
    return (long)len;
}


static void test_jfif_density(void)
{
    MPJpegEncoder enc;
    struct TestSink sink;
    unsigned char scratch[16 * 16 * 3];

    memset(&sink, 0, sizeof(sink));
    assert(mp_jpeg_begin_dpi(&enc, 16, 16, 600UL,
                             scratch, sizeof(scratch),
                             test_sink_write, &sink));
    /* SOI(2) + APP0 marker(2) + length(2) + JFIF data. Units byte is file
     * offset 13; X/Y density are offsets 14..17. */
    assert(sink.header_used >= 18UL);
    assert(sink.header[13] == 1); /* pixels per inch */
    assert(sink.header[14] == 0x02 && sink.header[15] == 0x58);
    assert(sink.header[16] == 0x02 && sink.header[17] == 0x58);
}

static void test_flat_encoder_fast_path(void)
{
    MPJpegEncoder enc;
    struct TestSink sink;
    unsigned char scratch[16 * 16 * 3];
    unsigned char row[16 * 3];
    unsigned long scratch_size;
    int y, i;

    sink.bytes = 0;
    sink.calls = 0;
    sink.header_used = 0;
    for (i = 0; i < (int)sizeof(row); ++i) row[i] = 255;

    scratch_size = mp_jpeg_scratch_size(16);
    assert(scratch_size == sizeof(scratch));
    assert(mp_jpeg_begin(&enc, 16, 16, scratch, sizeof(scratch),
                         test_sink_write, &sink));
    for (y = 0; y < 16; ++y)
        assert(mp_jpeg_write_scanline(&enc, row));
    assert(mp_jpeg_finish(&enc));

    /* One 16x16 4:2:0 MCU = four Y blocks + one Cb + one Cr. Pure white
     * makes all six blocks constant, so every one must take the shortcut. */
    assert(enc.blocks_total == 6UL);
    assert(enc.blocks_constant == 6UL);
    assert(sink.bytes > 0UL);
    printf("flat encoder: %lu/%lu blocks used constant fast path\n",
           enc.blocks_constant, enc.blocks_total);
}

static void test_nonflat_encoder_still_uses_dct(void)
{
    MPJpegEncoder enc;
    struct TestSink sink;
    unsigned char scratch[16 * 16 * 3];
    unsigned char row[16 * 3];
    int y, x;

    sink.bytes = 0;
    sink.calls = 0;
    sink.header_used = 0;
    assert(mp_jpeg_begin(&enc, 16, 16, scratch, sizeof(scratch),
                         test_sink_write, &sink));
    for (y = 0; y < 16; ++y) {
        for (x = 0; x < 16; ++x) {
            row[x * 3 + 0] = (unsigned char)(x * 16);
            row[x * 3 + 1] = (unsigned char)(y * 16);
            row[x * 3 + 2] = (unsigned char)((x + y) * 8);
        }
        assert(mp_jpeg_write_scanline(&enc, row));
    }
    assert(mp_jpeg_finish(&enc));
    assert(enc.blocks_total == 6UL);
    assert(enc.blocks_constant < enc.blocks_total);
    assert(sink.bytes > 0UL);
    printf("gradient encoder: %lu/%lu constant blocks\n",
           enc.blocks_constant, enc.blocks_total);
}

int main(void)
{
    short block[64];
    double err;
    unsigned int seed = 12345u;
    int trial;
    double max_seen = 0.0;

    fill_flat(block, 0);
    err = mp_round_trip_max_error(block);
    printf("flat 0: max error %.3f\n", err);
    assert(err < 1.0);

    fill_flat(block, 127);
    err = mp_round_trip_max_error(block);
    printf("flat 127: max error %.3f\n", err);
    assert(err < 1.0);

    fill_flat(block, -128);
    err = mp_round_trip_max_error(block);
    printf("flat -128: max error %.3f\n", err);
    assert(err < 1.0);

    fill_gradient(block);
    err = mp_round_trip_max_error(block);
    printf("gradient: max error %.3f\n", err);
    assert(err < 10.0);

    fill_checkerboard(block);
    err = mp_round_trip_max_error(block);
    printf("checkerboard: max error %.3f\n", err);
    assert(err < 20.0);

    for (trial = 0; trial < 500; ++trial) {
        fill_random(block, &seed);
        err = mp_round_trip_max_error(block);
        if (err > max_seen) max_seen = err;
        assert(err < 25.0);
    }
    printf("random sweep (500 blocks): worst-case max error %.3f\n",
           max_seen);

    test_jfif_density();
    test_flat_encoder_fast_path();
    test_nonflat_encoder_still_uses_dct();

    puts("jpeg AAN/JPEG Turbo tests passed");
    return 0;
}
