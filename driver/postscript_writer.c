#include "postscript_writer.h"

#define MP_PS_ASCII85_LINE 75U

#define MP_PS_SCALE_AUTO 0
#define MP_PS_SCALE_FIT  1
#define MP_PS_SCALE_FILL 2

static int g_mp_ps_scaling = MP_PS_SCALE_AUTO;
static unsigned long g_mp_ps_margin_left_100mm = 0;
static unsigned long g_mp_ps_margin_right_100mm = 0;
static unsigned long g_mp_ps_margin_top_100mm = 0;
static unsigned long g_mp_ps_margin_bottom_100mm = 0;

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

void mp_postscript_set_margins(unsigned long left_100mm,
                               unsigned long right_100mm,
                               unsigned long top_100mm,
                               unsigned long bottom_100mm)
{
    g_mp_ps_margin_left_100mm = left_100mm;
    g_mp_ps_margin_right_100mm = right_100mm;
    g_mp_ps_margin_top_100mm = top_100mm;
    g_mp_ps_margin_bottom_100mm = bottom_100mm;
}

/* IPP media margins are hundredths of a millimetre. One PostScript point
 * is 1/72 inch = 25.4/72 mm, so points = margin * 72 / 2540. These are
 * NON-imageable margins: round upward, never to nearest, so conversion to
 * integer PostScript coordinates cannot move content back into the dead
 * hardware edge. Samsung's 4.4 mm (440) therefore becomes 13 pt, not 12. */
static unsigned long mp_ps_margin_points(unsigned long margin_100mm)
{
    if (!margin_100mm) return 0;
    return (margin_100mm * 72UL + 2539UL) / 2540UL;
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

static int mp_ps_ascii85_chars(MPPostScriptEncoder *e,
                               const char *chars, unsigned int count)
{
    unsigned int pos = 0;

    while (pos < count) {
        unsigned int room;
        unsigned int chunk;

        if (e->ascii85_column >= MP_PS_ASCII85_LINE) {
            if (!mp_ps_lit(e, "\n")) return 0;
            e->ascii85_column = 0;
        }

        room = MP_PS_ASCII85_LINE - e->ascii85_column;
        chunk = count - pos;
        if (chunk > room) chunk = room;
        if (!mp_ps_raw(e, (const unsigned char *)(chars + pos), chunk))
            return 0;
        e->ascii85_column += chunk;
        pos += chunk;
    }

    return 1;
}

static int mp_ps_ascii85_tuple(MPPostScriptEncoder *e, unsigned int input_count)
{
    unsigned long value;
    char out[5];
    int i;

    value = ((unsigned long)e->ascii85_tuple[0] << 24) |
            ((unsigned long)e->ascii85_tuple[1] << 16) |
            ((unsigned long)e->ascii85_tuple[2] << 8) |
            (unsigned long)e->ascii85_tuple[3];

    if (input_count == 4U && value == 0UL)
        return mp_ps_ascii85_chars(e, "z", 1U);

    for (i = 4; i >= 0; --i) {
        out[i] = (char)((value % 85UL) + 33UL);
        value /= 85UL;
    }

    return mp_ps_ascii85_chars(e, out, input_count + 1U);
}

static long mp_ps_jpeg_write_fn(void *ctx,
                                const unsigned char *data,
                                unsigned long len)
{
    MPPostScriptEncoder *e = (MPPostScriptEncoder *)ctx;
    unsigned long i;

    if (!e || e->failed) return -1;

    for (i = 0; i < len; ++i) {
        e->ascii85_tuple[e->ascii85_count++] = data[i];
        if (e->ascii85_count == 4U) {
            if (!mp_ps_ascii85_tuple(e, 4U)) return -1;
            e->ascii85_count = 0;
        }
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
    unsigned long target_x = 0;
    unsigned long target_y = 0;
    unsigned long target_w;
    unsigned long target_h;
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
    e->ascii85_count = 0;
    e->ascii85_column = 0;
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
    target_w = page_w;
    target_h = page_h;

    /*
     * Keep the rev27 placement policy for auto/auto-fit/none: preserve the
     * printer.device raster's physical DPI size when it already fits, and
     * only reduce oversized content so it cannot run off the selected sheet.
     *
     * Explicit fit/fill are different. printer.device can intentionally
     * deliver a lower-resolution raster to keep classic-Amiga CPU/memory use
     * reasonable (the built-in PostScript test page is 4.2 x 5.94 inches at
     * 300 DPI), while /PageSize still describes the real A4/Letter sheet.
     * Once that raster is embedded in a full-size PostScript page, an IPP
     * print-scaling attribute can no longer enlarge the image itself. Apply
     * those two user-selected modes here, changing only PostScript geometry;
     * the JPEG stream, raster dimensions and transfer size stay unchanged.
     *
     * Fit means preserve the complete image inside the printer's imageable
     * area. If the spool process resolved unambiguous IPP media margins,
     * Fit therefore targets that printable rectangle. Fill deliberately does
     * not: it retains rev28's cover-the-physical-sheet/crop semantics.
     * Auto/auto-fit/none also retain their established rev27 geometry.
     * Invalid/impossible margin combinations are ignored, the same zero-
     * margin compatibility fallback used when a printer does not report them.
     */
    if (g_mp_ps_scaling == MP_PS_SCALE_FIT) {
        unsigned long left = mp_ps_margin_points(g_mp_ps_margin_left_100mm);
        unsigned long right = mp_ps_margin_points(g_mp_ps_margin_right_100mm);
        unsigned long top = mp_ps_margin_points(g_mp_ps_margin_top_100mm);
        unsigned long bottom = mp_ps_margin_points(g_mp_ps_margin_bottom_100mm);

        if (left + right < page_w && top + bottom < page_h) {
            target_x = left;
            target_y = bottom;
            target_w = page_w - left - right;
            target_h = page_h - top - bottom;
        }
    }

    draw_w = image_w;
    draw_h = image_h;

    if (g_mp_ps_scaling == MP_PS_SCALE_FIT) {
        if (image_w * target_h > image_h * target_w) {
            draw_w = target_w;
            draw_h = (image_h * target_w + image_w / 2UL) / image_w;
        } else {
            draw_h = target_h;
            draw_w = (image_w * target_h + image_h / 2UL) / image_h;
        }
    } else if (g_mp_ps_scaling == MP_PS_SCALE_FILL) {
        if (image_w * target_h > image_h * target_w) {
            draw_h = target_h;
            draw_w = (image_w * target_h + image_h / 2UL) / image_h;
        } else {
            draw_w = target_w;
            draw_h = (image_h * target_w + image_w / 2UL) / image_w;
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

    if (g_mp_ps_scaling == MP_PS_SCALE_FIT ||
        g_mp_ps_scaling == MP_PS_SCALE_FILL) {
        if (draw_w <= target_w)
            draw_x = (long)(target_x + (target_w - draw_w) / 2UL);
        else
            draw_x = (long)target_x - (long)((draw_w - target_w) / 2UL);
        if (draw_h <= target_h)
            draw_y = (long)(target_y + (target_h - draw_h) / 2UL);
        else
            draw_y = (long)target_y - (long)((draw_h - target_h) / 2UL);
    } else {
        if (draw_w <= page_w)
            draw_x = (long)((page_w - draw_w) / 2UL);
        else
            draw_x = -(long)((draw_w - page_w) / 2UL);
        if (draw_h <= page_h)
            draw_y = (long)((page_h - draw_h) / 2UL);
        else
            draw_y = -(long)((draw_h - page_h) / 2UL);
    }

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
            "   /DataSource currentfile /ASCII85Decode filter"
            " /DCTDecode filter\n"
            ">> image\n")) return 0;

    if (!mp_jpeg_begin_dpi(&e->jpeg, width, height, dpi,
                           scratch, scratch_size,
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
    unsigned int i;

    if (!e || e->failed) return 0;
    if (!mp_jpeg_finish(&e->jpeg)) {
        e->failed = 1;
        return 0;
    }

    if (e->ascii85_count) {
        unsigned int count = e->ascii85_count;
        for (i = count; i < 4U; ++i) e->ascii85_tuple[i] = 0;
        if (!mp_ps_ascii85_tuple(e, count)) return 0;
        e->ascii85_count = 0;
    }

    if (e->ascii85_column && !mp_ps_lit(e, "\n")) return 0;
    if (!mp_ps_lit(e, "~>\n"
                       "grestore\n"
                       "showpage\n"
                       "%%PageTrailer\n"
                       "%%Trailer\n"
                       "%%Pages: 1\n"
                       "%%EOF\n")) return 0;

    if (!mp_ps_flush(e)) return 0;

    return !e->failed;
}