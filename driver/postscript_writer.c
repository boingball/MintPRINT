#include "postscript_writer.h"

#define MP_PS_ASCIIHEX_LINE 76U
#define MP_PS_HEX_CHUNK 64U

#define MP_PS_SCALE_AUTO 0
#define MP_PS_SCALE_FIT  1
#define MP_PS_SCALE_FILL 2

static int g_mp_ps_scaling = MP_PS_SCALE_AUTO;

static unsigned long mp_ps_strlen(const char *s)
{
    unsigned long n = 0;
    while (s && s[n]) ++n;
    return n;
}

static int mp_ps_streq(const char *a, const char *b)
{
    unsigned long i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i] && a[i] == b[i]) ++i;
    return a[i] == 0 && b[i] == 0;
}

void mp_postscript_set_scaling(const char *scaling)
{
    if (mp_ps_streq(scaling, "fit"))
        g_mp_ps_scaling = MP_PS_SCALE_FIT;
    else if (mp_ps_streq(scaling, "fill"))
        g_mp_ps_scaling = MP_PS_SCALE_FILL;
    else
        g_mp_ps_scaling = MP_PS_SCALE_AUTO;
}

static int mp_ps_flush(MPPostScriptEncoder *e)
{
    unsigned long len;

    if (!e || e->failed) return 0;
    if (!e->out_used) return 1;
    len = e->out_used;
    if (!e->write_fn ||
        e->write_fn(e->write_ctx, e->outbuf, len) != (long)len) {
        e->failed = 1;
        return 0;
    }
    e->out_used = 0;
    e->output_bytes += len;
    ++e->write_calls;
    return 1;
}

static int mp_ps_raw(MPPostScriptEncoder *e,
                     const unsigned char *data, unsigned long len)
{
    unsigned long pos = 0;

    if (!e || e->failed || (!data && len)) return 0;
    while (pos < len) {
        unsigned long room = sizeof(e->outbuf) - e->out_used;
        unsigned long chunk = len - pos;
        unsigned long i;

        if (chunk > room) chunk = room;
        for (i = 0; i < chunk; ++i)
            e->outbuf[e->out_used + i] = data[pos + i];
        e->out_used += chunk;
        pos += chunk;
        if (e->out_used == sizeof(e->outbuf) && !mp_ps_flush(e))
            return 0;
    }
    return 1;
}

static int mp_ps_lit(MPPostScriptEncoder *e, const char *s)
{
    return mp_ps_raw(e, (const unsigned char *)s, mp_ps_strlen(s));
}

static int mp_ps_uint(MPPostScriptEncoder *e, unsigned long value)
{
    char buf[10];
    unsigned long pos = sizeof(buf);

    do {
        buf[--pos] = (char)('0' + (value % 10UL));
        value /= 10UL;
    } while (value && pos);

    return mp_ps_raw(e, (const unsigned char *)(buf + pos),
                     (unsigned long)sizeof(buf) - pos);
}

static int mp_ps_int(MPPostScriptEncoder *e, long value)
{
    unsigned long magnitude;

    if (value < 0) {
        if (!mp_ps_lit(e, "-")) return 0;
        magnitude = (unsigned long)(-value);
    } else {
        magnitude = (unsigned long)value;
    }
    return mp_ps_uint(e, magnitude);
}

/* ASCII85 made the PostScript payload compact, but converting every four
 * JPEG bytes required five 32-bit /85 and %85 operations. Those are costly
 * on classic 68k. ASCIIHex doubles the compressed JPEG bytes on the wire,
 * but encoding becomes two table lookups per byte and no general division.
 * Ethernet bandwidth is cheap compared with 68030/040/060 integer division. */
static long mp_ps_jpeg_write_fn(void *ctx,
                                const unsigned char *data,
                                unsigned long len)
{
    static const char hex[] = "0123456789ABCDEF";
    MPPostScriptEncoder *e = (MPPostScriptEncoder *)ctx;
    unsigned long pos = 0;
    char out[MP_PS_HEX_CHUNK * 2U];

    if (!e || e->failed) return -1;

    while (pos < len) {
        unsigned int room_chars;
        unsigned long input_room;
        unsigned long chunk;
        unsigned long i;

        if (e->asciihex_column >= MP_PS_ASCIIHEX_LINE) {
            if (!mp_ps_lit(e, "\n")) return -1;
            e->asciihex_column = 0;
        }

        room_chars = MP_PS_ASCIIHEX_LINE - e->asciihex_column;
        input_room = (unsigned long)(room_chars / 2U);
        if (!input_room) {
            if (!mp_ps_lit(e, "\n")) return -1;
            e->asciihex_column = 0;
            continue;
        }

        chunk = len - pos;
        if (chunk > input_room) chunk = input_room;
        if (chunk > MP_PS_HEX_CHUNK) chunk = MP_PS_HEX_CHUNK;

        for (i = 0; i < chunk; ++i) {
            unsigned int b = data[pos + i];
            out[i * 2UL] = hex[(b >> 4) & 15U];
            out[i * 2UL + 1UL] = hex[b & 15U];
        }
        if (!mp_ps_raw(e, (const unsigned char *)out, chunk * 2UL))
            return -1;
        e->asciihex_column += (unsigned int)(chunk * 2UL);
        pos += chunk;
    }

    return (long)len;
}

unsigned long mp_postscript_scratch_size(unsigned long width)
{
    return mp_jpeg_scratch_size(width);
}

int mp_postscript_begin(MPPostScriptEncoder *e,
                        unsigned long width, unsigned long height,
                        unsigned long page_width_points,
                        unsigned long page_height_points,
                        unsigned long dpi,
                        unsigned char *scratch, unsigned long scratch_size,
                        MPPostScriptWriteFn write_fn, void *write_ctx)
{
    unsigned long page_w;
    unsigned long page_h;
    unsigned long image_w;
    unsigned long image_h;
    unsigned long draw_w;
    unsigned long draw_h;
    long draw_x;
    long draw_y;

    if (!e || !width || !height || width > 65535UL || height > 65535UL ||
        !scratch || !write_fn)
        return 0;

    e->width = width;
    e->height = height;
    e->write_fn = write_fn;
    e->write_ctx = write_ctx;
    e->asciihex_column = 0;
    e->out_used = 0;
    e->output_bytes = 0;
    e->write_calls = 0;
    e->failed = 0;
    if (!dpi) dpi = 300UL;

    image_w = (width * 72UL + dpi / 2UL) / dpi;
    image_h = (height * 72UL + dpi / 2UL) / dpi;
    if (!image_w) image_w = 1UL;
    if (!image_h) image_h = 1UL;
    page_w = page_width_points ? page_width_points : image_w;
    page_h = page_height_points ? page_height_points : image_h;

    draw_w = image_w;
    draw_h = image_h;

    if (g_mp_ps_scaling == MP_PS_SCALE_FIT) {
        if (image_w * page_h > image_h * page_w) {
            draw_w = page_w;
            draw_h = (image_h * page_w + image_w / 2UL) / image_w;
        } else {
            draw_h = page_h;
            draw_w = (image_w * page_h + image_h / 2UL) / image_h;
        }
    } else if (g_mp_ps_scaling == MP_PS_SCALE_FILL) {
        if (image_w * page_h > image_h * page_w) {
            draw_h = page_h;
            draw_w = (image_w * page_h + image_h / 2UL) / image_h;
        } else {
            draw_w = page_w;
            draw_h = (image_h * page_w + image_w / 2UL) / image_w;
        }
    } else if (draw_w > page_w || draw_h > page_h) {
        if (draw_w * page_h > draw_h * page_w) {
            draw_h = (draw_h * page_w + draw_w / 2UL) / draw_w;
            draw_w = page_w;
        } else {
            draw_w = (draw_w * page_h + draw_h / 2UL) / draw_h;
            draw_h = page_h;
        }
    }

    if (!draw_w) draw_w = 1UL;
    if (!draw_h) draw_h = 1UL;

    if (draw_w <= page_w)
        draw_x = (long)((page_w - draw_w) / 2UL);
    else
        draw_x = -(long)((draw_w - page_w) / 2UL);
    if (draw_h <= page_h)
        draw_y = (long)((page_h - draw_h) / 2UL);
    else
        draw_y = -(long)((draw_h - page_h) / 2UL);

    if (!mp_ps_lit(e, "%!PS-Adobe-3.0\n"
                       "%%Creator: MintPRINT\n"
                       "%%Pages: 1\n"
                       "%%LanguageLevel: 2\n"
                       "%%BoundingBox: 0 0 ")) return 0;
    if (!mp_ps_uint(e, page_w) || !mp_ps_lit(e, " ") ||
        !mp_ps_uint(e, page_h)) return 0;
    if (!mp_ps_lit(e, "\n%%EndComments\n"
                       "<< /PageSize [")) return 0;
    if (!mp_ps_uint(e, page_w) || !mp_ps_lit(e, " ") ||
        !mp_ps_uint(e, page_h)) return 0;
    if (!mp_ps_lit(e, "] >> setpagedevice\n"
                       "%%Page: 1 1\n"
                       "gsave\n")) return 0;
    if (!mp_ps_int(e, draw_x) || !mp_ps_lit(e, " ") ||
        !mp_ps_int(e, draw_y) || !mp_ps_lit(e, " translate\n")) return 0;
    if (!mp_ps_uint(e, draw_w) || !mp_ps_lit(e, " ") ||
        !mp_ps_uint(e, draw_h)) return 0;
    if (!mp_ps_lit(e, " scale\n"
                       "/DeviceRGB setcolorspace\n"
                       "<< /ImageType 1 /Width ")) return 0;
    if (!mp_ps_uint(e, width) || !mp_ps_lit(e, " /Height ") ||
        !mp_ps_uint(e, height)) return 0;
    if (!mp_ps_lit(e,
            " /BitsPerComponent 8\n"
            "   /Decode [0 1 0 1 0 1]\n"
            "   /ImageMatrix [")) return 0;
    if (!mp_ps_uint(e, width) || !mp_ps_lit(e, " 0 0 -") ||
        !mp_ps_uint(e, height) || !mp_ps_lit(e, " 0 ") ||
        !mp_ps_uint(e, height)) return 0;
    if (!mp_ps_lit(e,
            "]\n"
            "   /DataSource currentfile /ASCIIHexDecode filter"
            " /DCTDecode filter\n"
            ">> image\n")) return 0;

    if (!mp_jpeg_begin(&e->jpeg, width, height, scratch, scratch_size,
                       mp_ps_jpeg_write_fn, e)) {
        e->failed = 1;
        return 0;
    }

    return !e->failed;
}

int mp_postscript_write_scanline(MPPostScriptEncoder *e,
                                 const unsigned char *rgb)
{
    if (!e || e->failed || !rgb) return 0;
    if (!mp_jpeg_write_scanline(&e->jpeg, rgb)) {
        e->failed = 1;
        return 0;
    }
    return 1;
}

int mp_postscript_finish(MPPostScriptEncoder *e)
{
    if (!e || e->failed) return 0;
    if (!mp_jpeg_finish(&e->jpeg)) {
        e->failed = 1;
        return 0;
    }

    if (e->asciihex_column && !mp_ps_lit(e, "\n")) return 0;
    if (!mp_ps_lit(e, ">\n"
                       "grestore\n"
                       "showpage\n"
                       "%%PageTrailer\n"
                       "%%Trailer\n"
                       "%%Pages: 1\n"
                       "%%EOF\n")) return 0;

    if (!mp_ps_flush(e)) return 0;
    return !e->failed;
}
