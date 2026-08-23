#include "jpeg_writer.h"

static const unsigned char mp_q_luma[64] = {
     8, 9,10,11,12,13,14,15,
     9,10,11,12,13,14,15,16,
    10,11,12,13,14,15,16,17,
    11,12,13,14,15,16,17,18,
    12,13,14,15,16,17,18,19,
    13,14,15,16,17,18,19,20,
    14,15,16,17,18,19,20,21,
    15,16,17,18,19,20,21,22
};

static const unsigned char mp_q_chroma[64] = {
    10,11,12,13,14,15,16,17,
    11,12,13,14,15,16,17,18,
    12,13,14,15,16,17,18,19,
    13,14,15,16,17,18,19,20,
    14,15,16,17,18,19,20,21,
    15,16,17,18,19,20,21,22,
    16,17,18,19,20,21,22,23,
    17,18,19,20,21,22,23,24
};

/* JPEG zig-zag order as direct row-major coefficient indices.  The old
 * encoder rebuilt this sequence with nested loops for every AC coefficient
 * and every DQT byte.  A 64-byte table is vastly cheaper on 68k and is
 * format-identical. */
static const unsigned char mp_zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

static long mp_round_shift(long v, int bits)
{
    long add = 1L << (bits - 1);
    if (v < 0) return -(((-v) + add) >> bits);
    return (v + add) >> bits;
}

/* AAN (Arai, Agui, Nakajima 1988) fast integer forward DCT - the same
 * algorithm family as IJG libjpeg's jfdctfst.c. A direct 8x8 matrix DCT
 * needs roughly 1024 multiplies per block; AAN restructures it into two
 * butterfly passes with about 80 multiplies and defers the non-uniform
 * coefficient scale until quantisation. */
#define MP_FDCT_CONST_BITS 8
#define MP_FDCT_FIX_0_382683433 98L
#define MP_FDCT_FIX_0_541196100 139L
#define MP_FDCT_FIX_0_707106781 181L
#define MP_FDCT_FIX_1_306562965 334L

const long mp_fdct_aan_scale[64] = {
    16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
    22725, 31521, 29692, 26722, 22725, 17855, 12299,  6270,
    21407, 29692, 27969, 25172, 21407, 16819, 11585,  5906,
    19266, 26722, 25172, 22654, 19266, 15137, 10426,  5315,
    16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
    12873, 17855, 16819, 15137, 12873, 10114,  6967,  3552,
     8867, 12299, 11585, 10426,  8867,  6967,  4799,  2446,
     4520,  6270,  5906,  5315,  4520,  3552,  2446,  1247
};

static int mp_abs(int v) { return v < 0 ? -v : v; }

static int mp_category(int v)
{
    unsigned int a = (unsigned int)mp_abs(v);
    int n = 0;
    while (a) { ++n; a >>= 1; }
    return n;
}

static unsigned int mp_amplitude_bits(int v, int size)
{
    if (size == 0) return 0;
    if (v >= 0) return (unsigned int)v;
    return (unsigned int)(v + ((1 << size) - 1));
}

static int mp_write_raw(MPJpegEncoder *e, const unsigned char *p,
                        unsigned long n)
{
    if (e->failed) return 0;
    if (!e->write_fn || e->write_fn(e->write_ctx, p, n) != (long)n) {
        e->failed = 1;
        return 0;
    }
    return 1;
}

static int mp_flush_out(MPJpegEncoder *e)
{
    if (!e->out_used) return !e->failed;
    if (!mp_write_raw(e, e->outbuf, e->out_used)) return 0;
    e->out_used = 0;
    return 1;
}

static int mp_put_byte(MPJpegEncoder *e, unsigned int b)
{
    if (e->failed) return 0;
    e->outbuf[e->out_used++] = (unsigned char)b;
    if (e->out_used == sizeof(e->outbuf)) return mp_flush_out(e);
    return 1;
}

static int mp_put_marker(MPJpegEncoder *e, unsigned int m)
{
    return mp_put_byte(e, 0xff) && mp_put_byte(e, m);
}

static int mp_put_u16(MPJpegEncoder *e, unsigned int v)
{
    return mp_put_byte(e, (v >> 8) & 255) && mp_put_byte(e, v & 255);
}

static int mp_emit_entropy_byte(MPJpegEncoder *e, unsigned int b)
{
    if (!mp_put_byte(e, b)) return 0;
    if (b == 0xff) return mp_put_byte(e, 0x00);
    return 1;
}

static int mp_put_bits(MPJpegEncoder *e, unsigned int bits, int count)
{
    if (count <= 0) return !e->failed;
    bits &= (count == 32) ? 0xffffffffUL : ((1UL << count) - 1UL);
    e->bit_acc = (e->bit_acc << count) | bits;
    e->bit_count += count;
    while (e->bit_count >= 8) {
        unsigned int b = (unsigned int)((e->bit_acc >>
                                         (e->bit_count - 8)) & 255UL);
        e->bit_count -= 8;
        if (!mp_emit_entropy_byte(e, b)) return 0;
    }
    return 1;
}

static int mp_flush_bits(MPJpegEncoder *e)
{
    if (e->bit_count > 0) {
        int pad = 8 - e->bit_count;
        unsigned int ones = (1U << pad) - 1U;
        if (!mp_put_bits(e, ones, pad)) return 0;
    }
    e->bit_acc = 0;
    e->bit_count = 0;
    return 1;
}

static long mp_fdct_mul(long var, long c)
{
    return mp_round_shift(var * c, MP_FDCT_CONST_BITS);
}

static void mp_fdct_pass(long *data, int base, int stride)
{
    long t0, t1, t2, t3, t4, t5, t6, t7;
    long t10, t11, t12, t13;
    long z1, z2, z3, z4, z5, z11, z13;
    long v0 = data[base + 0 * stride];
    long v1 = data[base + 1 * stride];
    long v2 = data[base + 2 * stride];
    long v3 = data[base + 3 * stride];
    long v4 = data[base + 4 * stride];
    long v5 = data[base + 5 * stride];
    long v6 = data[base + 6 * stride];
    long v7 = data[base + 7 * stride];

    t0 = v0 + v7; t7 = v0 - v7;
    t1 = v1 + v6; t6 = v1 - v6;
    t2 = v2 + v5; t5 = v2 - v5;
    t3 = v3 + v4; t4 = v3 - v4;

    t10 = t0 + t3; t13 = t0 - t3;
    t11 = t1 + t2; t12 = t1 - t2;

    data[base + 0 * stride] = t10 + t11;
    data[base + 4 * stride] = t10 - t11;

    z1 = mp_fdct_mul(t12 + t13, MP_FDCT_FIX_0_707106781);
    data[base + 2 * stride] = t13 + z1;
    data[base + 6 * stride] = t13 - z1;

    t10 = t4 + t5;
    t11 = t5 + t6;
    t12 = t6 + t7;

    z5 = mp_fdct_mul(t10 - t12, MP_FDCT_FIX_0_382683433);
    z2 = mp_fdct_mul(t10, MP_FDCT_FIX_0_541196100) + z5;
    z4 = mp_fdct_mul(t12, MP_FDCT_FIX_1_306562965) + z5;
    z3 = mp_fdct_mul(t11, MP_FDCT_FIX_0_707106781);

    z11 = t7 + z3;
    z13 = t7 - z3;

    data[base + 5 * stride] = z13 + z2;
    data[base + 3 * stride] = z13 - z2;
    data[base + 1 * stride] = z11 + z4;
    data[base + 7 * stride] = z11 - z4;
}

void mp_fdct(const short *block, long *out)
{
    int i;
    for (i = 0; i < 64; ++i) out[i] = (long)block[i];
    for (i = 0; i < 8; ++i) mp_fdct_pass(out, i * 8, 1);
    for (i = 0; i < 8; ++i) mp_fdct_pass(out, i, 8);
}

int mp_quantize_aan(long raw, int q, long aan_scale)
{
    long num = raw * 2048L;
    long den = (long)q * aan_scale;
    if (num < 0) return (int)(-(((-num) + den / 2) / den));
    return (int)((num + den / 2) / den);
}

/* Reciprocal quantisation for the hot JPEG coefficient path.
 *
 * mp_quantize_aan() above is intentionally kept as the simple reference
 * implementation for tests and for the very unlikely out-of-range fallback.
 * On a classic 68k, its 32-bit / 32-bit division is expensive enough to
 * dominate JPEG encoding once the flat-block shortcut has removed most of
 * the easy work.
 *
 * For a divisor den, mp_jpeg_begin() precomputes:
 *
 *     reciprocal = floor(2^26 / den)
 *
 * The rounded positive numerator is below 2^26 for every coefficient the
 * current integer AAN FDCT can produce from 8-bit samples.  Shift it by 10
 * first, leaving a 16-bit value, then a single small multiply gives a Q26
 * quotient estimate:
 *
 *     q0 = ((rounded >> 10) * reciprocal) >> 16
 *
 * Both truncations are downward, so q0 never overshoots.  A short exact
 * multiply/compare correction closes the at-most-tiny gap without division.
 * If a future transform ever hands us a numerator outside the proven Q26
 * range, fall back to ordinary division rather than risk changing output. */
#define MP_QUANT_RECIP_ONE 67108864UL /* 1 << 26 */

static unsigned long mp_mul_u16_u32(unsigned int small, unsigned long value)
{
    unsigned long lo;
    unsigned long hi;

    /* small is at most a few thousand in this use.  Split the 32-bit
     * divisor so an m68k compiler can use cheap 16x16 multiplies rather
     * than a general 32x32 helper. */
    lo = (unsigned long)(unsigned short)small *
         (unsigned long)(unsigned short)(value & 0xffffUL);
    hi = (unsigned long)(unsigned short)small *
         (unsigned long)(unsigned short)(value >> 16);
    return lo + (hi << 16);
}

int mp_quantize_aan_recip(long raw, unsigned long den,
                          unsigned int reciprocal)
{
    unsigned long magnitude;
    unsigned long rounded;
    unsigned long quotient;
    unsigned long threshold;
    int negative = raw < 0;

    if (!den) return 0;
    if (negative)
        magnitude = (unsigned long)(-(raw + 1L)) + 1UL;
    else
        magnitude = (unsigned long)raw;

    rounded = magnitude * 2048UL + den / 2UL;

    if (!reciprocal || rounded >= MP_QUANT_RECIP_ONE) {
        quotient = rounded / den;
    } else {
        unsigned int scaled = (unsigned int)(rounded >> 10);
        quotient = ((unsigned long)scaled * (unsigned long)reciprocal) >> 16;

        /* The reciprocal estimate is deliberately low.  Normally this loop
         * runs zero or one time; keeping it as a loop makes the exactness
         * self-correcting if a boundary lands two steps low. */
        threshold = mp_mul_u16_u32((unsigned int)(quotient + 1UL), den);
        while (threshold <= rounded) {
            ++quotient;
            threshold = mp_mul_u16_u32((unsigned int)(quotient + 1UL), den);
        }
    }

    return negative ? -(int)quotient : (int)quotient;
}

static int mp_emit_dc(MPJpegEncoder *e, int diff)
{
    int size = mp_category(diff);
    if (size > 11) size = 11;
    if (!mp_put_bits(e, (unsigned int)size, 4)) return 0;
    return mp_put_bits(e, mp_amplitude_bits(diff, size), size);
}

static int mp_ac_code_for(int run, int size)
{
    if (run == 0 && size == 0) return 0;
    if (run == 15 && size == 0) return 1;
    return 2 + run * 10 + (size - 1);
}

static int mp_emit_ac_symbol(MPJpegEncoder *e, int run, int size)
{
    int code = mp_ac_code_for(run, size);
    return mp_put_bits(e, (unsigned int)code, 8);
}

static int mp_block_constant(const short *samples, short *value_out)
{
    short value;
    int k;

    value = samples[0];
    for (k = 1; k < 64; ++k) {
        if (samples[k] != value) return 0;
    }
    if (value_out) *value_out = value;
    return 1;
}

static int mp_encode_block(MPJpegEncoder *e, const short *samples,
                           const unsigned long *qden,
                           const unsigned short *qrecip,
                           int *dc_pred)
{
    long coeff[64];
    int qcoeff[64];
    int k, run;
    int dc, diff;
    short constant_value;

    ++e->blocks_total;

    /* A constant 8x8 block has one DC coefficient and 63 exact zero AC
     * coefficients.  Printing spends a lot of time on white page margins
     * and flat colour areas, so avoid the complete AAN transform plus 64
     * quantisation work for those blocks.  Feeding the mathematically exact
     * raw AAN DC (64 * sample) through the reciprocal quantiser preserves the
     * current bitstream maths without reintroducing a hot 32-bit divide. */
    if (mp_block_constant(samples, &constant_value)) {
        ++e->blocks_constant;
        dc = mp_quantize_aan_recip((long)constant_value * 64L,
                                   qden[0], (unsigned int)qrecip[0]);
        diff = dc - *dc_pred;
        *dc_pred = dc;
        if (!mp_emit_dc(e, diff)) return 0;
        return mp_emit_ac_symbol(e, 0, 0);
    }

    mp_fdct(samples, coeff);
    for (k = 0; k < 64; ++k)
        qcoeff[k] = mp_quantize_aan_recip(coeff[k], qden[k],
                                          (unsigned int)qrecip[k]);

    dc = qcoeff[0];
    diff = dc - *dc_pred;
    *dc_pred = dc;
    if (!mp_emit_dc(e, diff)) return 0;

    run = 0;
    for (k = 1; k < 64; ++k) {
        int idx = (int)mp_zigzag[k];
        int v = qcoeff[idx];
        int size;
        if (v == 0) {
            ++run;
            continue;
        }
        while (run >= 16) {
            if (!mp_emit_ac_symbol(e, 15, 0)) return 0;
            run -= 16;
        }
        size = mp_category(v);
        if (size > 10) size = 10;
        if (!mp_emit_ac_symbol(e, run, size)) return 0;
        if (!mp_put_bits(e, mp_amplitude_bits(v, size), size)) return 0;
        run = 0;
    }
    if (run) {
        if (!mp_emit_ac_symbol(e, 0, 0)) return 0;
    }
    return 1;
}

static unsigned char mp_get_rgb(const MPJpegEncoder *e, int row,
                                unsigned long x, int c)
{
    unsigned long sx = x < e->width ? x : (e->width - 1);
    int sr = row;
    if (sr < 0) sr = 0;
    if (sr > 15) sr = 15;
    return e->scratch[((unsigned long)sr * e->width + sx) * 3UL +
                      (unsigned long)c];
}

static short mp_y_from_rgb(int r, int g, int b)
{
    return (short)(((77 * r + 150 * g + 29 * b + 128) >> 8) - 128);
}

static short mp_cb_from_rgb(int r, int g, int b)
{
    return (short)(((-43 * r - 85 * g + 128 * b + 128) >> 8));
}

static short mp_cr_from_rgb(int r, int g, int b)
{
    return (short)(((128 * r - 107 * g - 21 * b + 128) >> 8));
}

static int mp_encode_mcu_row(MPJpegEncoder *e)
{
    unsigned long mx;
    unsigned long mcus = (e->width + 15UL) / 16UL;
    short block[64];
    short cb_block[64];
    short cr_block[64];

    for (mx = 0; mx < mcus; ++mx) {
        int by, bx, yy, xx;

        for (by = 0; by < 2; ++by) {
            for (bx = 0; bx < 2; ++bx) {
                for (yy = 0; yy < 8; ++yy) {
                    for (xx = 0; xx < 8; ++xx) {
                        unsigned long x = mx * 16UL +
                                          (unsigned long)(bx * 8 + xx);
                        int row = by * 8 + yy;
                        int r = mp_get_rgb(e, row, x, 0);
                        int g = mp_get_rgb(e, row, x, 1);
                        int b = mp_get_rgb(e, row, x, 2);
                        block[yy * 8 + xx] = mp_y_from_rgb(r, g, b);
                    }
                }
                if (!mp_encode_block(e, block,
                                     e->qden_luma, e->qrecip_luma,
                                     &e->dc_y))
                    return 0;
            }
        }

        /* Cb and Cr use the same 2x2 averaged RGB sample.  The old encoder
         * walked and averaged those four source pixels twice - once for Cb
         * and then again for Cr.  Build both chroma blocks in one pass. */
        for (yy = 0; yy < 8; ++yy) {
            for (xx = 0; xx < 8; ++xx) {
                unsigned long x0 = mx * 16UL + (unsigned long)(xx * 2);
                int row0 = yy * 2;
                int r = 0, g = 0, b = 0, dy, dx;
                int idx = yy * 8 + xx;

                for (dy = 0; dy < 2; ++dy) {
                    for (dx = 0; dx < 2; ++dx) {
                        r += mp_get_rgb(e, row0 + dy,
                                        x0 + (unsigned long)dx, 0);
                        g += mp_get_rgb(e, row0 + dy,
                                        x0 + (unsigned long)dx, 1);
                        b += mp_get_rgb(e, row0 + dy,
                                        x0 + (unsigned long)dx, 2);
                    }
                }
                r = (r + 2) >> 2;
                g = (g + 2) >> 2;
                b = (b + 2) >> 2;
                cb_block[idx] = mp_cb_from_rgb(r, g, b);
                cr_block[idx] = mp_cr_from_rgb(r, g, b);
            }
        }
        if (!mp_encode_block(e, cb_block,
                             e->qden_chroma, e->qrecip_chroma,
                             &e->dc_cb)) return 0;
        if (!mp_encode_block(e, cr_block,
                             e->qden_chroma, e->qrecip_chroma,
                             &e->dc_cr)) return 0;
    }
    ++e->mcu_rows_done;
    return 1;
}

static int mp_write_headers(MPJpegEncoder *e)
{
    int i, t;
    static const unsigned char jfif[14] = {
        'J','F','I','F',0, 1,1, 1, 1,44, 1,44, 0,0
    };

    if (!mp_put_marker(e, 0xd8)) return 0;
    if (!mp_put_marker(e, 0xe0) || !mp_put_u16(e, 16)) return 0;
    for (i = 0; i < 14; ++i) if (!mp_put_byte(e, jfif[i])) return 0;

    if (!mp_put_marker(e, 0xdb) || !mp_put_u16(e, 132)) return 0;
    for (t = 0; t < 2; ++t) {
        const unsigned char *q = t ? mp_q_chroma : mp_q_luma;
        if (!mp_put_byte(e, (unsigned int)t)) return 0;
        for (i = 0; i < 64; ++i) {
            int idx = (int)mp_zigzag[i];
            if (!mp_put_byte(e, q[idx])) return 0;
        }
    }

    if (!mp_put_marker(e, 0xc0) || !mp_put_u16(e, 17)) return 0;
    if (!mp_put_byte(e, 8) || !mp_put_u16(e, (unsigned int)e->height) ||
        !mp_put_u16(e, (unsigned int)e->width) || !mp_put_byte(e, 3))
        return 0;
    if (!mp_put_byte(e, 1) || !mp_put_byte(e, 0x22) ||
        !mp_put_byte(e, 0)) return 0;
    if (!mp_put_byte(e, 2) || !mp_put_byte(e, 0x11) ||
        !mp_put_byte(e, 1)) return 0;
    if (!mp_put_byte(e, 3) || !mp_put_byte(e, 0x11) ||
        !mp_put_byte(e, 1)) return 0;

    if (!mp_put_marker(e, 0xc4) || !mp_put_u16(e, 418)) return 0;
    for (t = 0; t < 2; ++t) {
        if (!mp_put_byte(e, (unsigned int)t)) return 0;
        for (i = 1; i <= 16; ++i)
            if (!mp_put_byte(e, i == 4 ? 12 : 0)) return 0;
        for (i = 0; i < 12; ++i)
            if (!mp_put_byte(e, i)) return 0;
    }
    for (t = 0; t < 2; ++t) {
        int run, size;
        if (!mp_put_byte(e, 0x10U | (unsigned int)t)) return 0;
        for (i = 1; i <= 16; ++i)
            if (!mp_put_byte(e, i == 8 ? 162 : 0)) return 0;
        if (!mp_put_byte(e, 0x00) || !mp_put_byte(e, 0xf0)) return 0;
        for (run = 0; run < 16; ++run)
            for (size = 1; size <= 10; ++size)
                if (!mp_put_byte(e, (unsigned int)((run << 4) | size)))
                    return 0;
    }

    if (!mp_put_marker(e, 0xda) || !mp_put_u16(e, 12) ||
        !mp_put_byte(e, 3)) return 0;
    if (!mp_put_byte(e, 1) || !mp_put_byte(e, 0x00)) return 0;
    if (!mp_put_byte(e, 2) || !mp_put_byte(e, 0x11)) return 0;
    if (!mp_put_byte(e, 3) || !mp_put_byte(e, 0x11)) return 0;
    if (!mp_put_byte(e, 0) || !mp_put_byte(e, 63) ||
        !mp_put_byte(e, 0)) return 0;
    return mp_flush_out(e);
}

unsigned long mp_jpeg_scratch_size(unsigned long width)
{
    if (!width || width > 0x05555555UL) return 0;
    return width * 3UL * 16UL;
}

int mp_jpeg_begin(MPJpegEncoder *e, unsigned long width, unsigned long height,
                  unsigned char *scratch, unsigned long scratch_size,
                  MPJpegWriteFn write_fn, void *write_ctx)
{
    unsigned long need;
    unsigned long i;

    if (!e || !width || !height || width > 65535UL || height > 65535UL ||
        !scratch || !write_fn)
        return 0;
    need = mp_jpeg_scratch_size(width);
    if (!need || scratch_size < need) return 0;

    e->width = width;
    e->height = height;
    e->rows_in = 0;
    e->mcu_rows_done = 0;
    e->blocks_total = 0;
    e->blocks_constant = 0;
    for (i = 0; i < 64UL; ++i) {
        unsigned long den_luma =
            (unsigned long)mp_q_luma[i] * (unsigned long)mp_fdct_aan_scale[i];
        unsigned long den_chroma =
            (unsigned long)mp_q_chroma[i] * (unsigned long)mp_fdct_aan_scale[i];
        e->qden_luma[i] = den_luma;
        e->qden_chroma[i] = den_chroma;
        e->qrecip_luma[i] =
            (unsigned short)(MP_QUANT_RECIP_ONE / den_luma);
        e->qrecip_chroma[i] =
            (unsigned short)(MP_QUANT_RECIP_ONE / den_chroma);
    }
    e->scratch = scratch;
    e->scratch_size = scratch_size;
    e->write_fn = write_fn;
    e->write_ctx = write_ctx;
    e->out_used = 0;
    e->bit_acc = 0;
    e->bit_count = 0;
    e->dc_y = e->dc_cb = e->dc_cr = 0;
    e->failed = 0;
    for (i = 0; i < need; ++i) scratch[i] = 255;
    return mp_write_headers(e);
}

int mp_jpeg_write_scanline(MPJpegEncoder *e, const unsigned char *rgb)
{
    unsigned long row;
    unsigned long bytes;
    unsigned long i;

    if (!e || e->failed || !rgb || e->rows_in >= e->height) return 0;
    row = e->rows_in & 15UL;
    bytes = e->width * 3UL;
    for (i = 0; i < bytes; ++i)
        e->scratch[row * bytes + i] = rgb[i];
    ++e->rows_in;
    if ((e->rows_in & 15UL) == 0) {
        if (!mp_encode_mcu_row(e)) return 0;
    }
    return !e->failed;
}

int mp_jpeg_finish(MPJpegEncoder *e)
{
    unsigned long bytes;
    unsigned long row;
    unsigned long i;

    if (!e || e->failed) return 0;
    if (e->rows_in != e->height) return 0;
    bytes = e->width * 3UL;
    row = e->rows_in & 15UL;
    if (row != 0) {
        unsigned long srcrow = row ? (row - 1UL) : 0UL;
        while (row < 16UL) {
            for (i = 0; i < bytes; ++i)
                e->scratch[row * bytes + i] = e->scratch[srcrow * bytes + i];
            ++row;
        }
        if (!mp_encode_mcu_row(e)) return 0;
    }
    if (!mp_flush_bits(e)) return 0;
    if (!mp_flush_out(e)) return 0;
    if (!mp_put_marker(e, 0xd9)) return 0;
    return mp_flush_out(e) && !e->failed;
}
