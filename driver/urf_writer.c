/*
 * Apple Raster (image/urf) file layout, page header fields, and the row
 * compression scheme are per CUPS's own "Apple" raster write mode -
 * verified against the public CUPS reference implementation
 * (cups/raster.h, cups/raster-stream.c) rather than from memory alone,
 * for the same reason pwg_writer.c documents: a byte-layout mistake here
 * would only surface as garbled physical output with no useful error
 * message.
 *
 * File layout:
 *   offset 0-3:   sync word "UNIR" (CUPS_RASTER_SYNCapple)
 *   offset 4-7:   "AST" + 0x00
 *   offset 8-11:  page count, big-endian u32 - written once, with the
 *                 first page (see MP_URF_PAGECOUNT_FIELD_OFFSET). A
 *                 single-page job (mp_urf_begin()) writes the real count
 *                 (1) immediately; a caller streaming more than one page
 *                 (duplex) writes the placeholder 0xffffffff ("unknown/
 *                 streaming", the same sentinel CUPS itself uses) and
 *                 must patch this field with the true total once known,
 *                 before closing the file.
 *   offset 12-43: 32-byte page header (below) - repeated per page, unlike
 *                 the file header above
 *   offset 44...: compressed rows, then another page header + rows for
 *                 each subsequent page
 *
 * Page header (32 bytes), all multi-byte fields big-endian:
 *   byte 0:      cupsBitsPerPixel (24 - 8-bit sRGB, chunked RGB)
 *   byte 1:      colorspace (1 - sRGB)
 *   byte 2:      duplex/tumble mode: 1 = simplex, 2 = duplex-tumble
 *                (two-sided-short-edge), 3 = duplex-no-tumble
 *                (two-sided-long-edge)
 *   byte 3:      print quality (0 - unspecified)
 *   byte 4:      media type (0 - unspecified)
 *   byte 5:      media position/tray (0 - auto)
 *   bytes 6-11:  reserved, zero
 *   bytes 12-15: width in pixels
 *   bytes 16-19: height in pixels
 *   bytes 20-23: resolution (dpi; Apple Raster declares one figure used
 *                for both axes, unlike PWG's HWResolution[2])
 *   bytes 24-31: reserved, zero
 *
 * Row compression is CUPS's shared PackBits-style scheme (identical to
 * pwg_writer.c's): a 1-byte "this line's data appears once" prefix (always
 * 0, since nothing here relies on line-repeat dedup), then only the
 * "repeat run" half (control byte 0-127, meaning "next pixel repeated
 * (byte+1) times", 1-128 pixels) - see pwg_writer.c and urf_writer.h for
 * why the format's other, more failure-prone "literal run" half is never
 * needed for a fully valid, decodable stream.
 */

#include "urf_writer.h"

static int mp_urf_raw(MPUrfEncoder *e, const unsigned char *p, unsigned long n)
{
    if (e->failed) return 0;
    if (!n) return 1;
    if (!e->write_fn || e->write_fn(e->write_ctx, p, n) != (long)n) {
        e->failed = 1;
        return 0;
    }
    return 1;
}

static int mp_urf_zeros(MPUrfEncoder *e, unsigned long n)
{
    static const unsigned char zero32[32] = { 0 };
    while (n > 0) {
        unsigned long chunk = n > 32UL ? 32UL : n;
        if (!mp_urf_raw(e, zero32, chunk)) return 0;
        n -= chunk;
    }
    return 1;
}

static int mp_urf_u32(MPUrfEncoder *e, unsigned long v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
    return mp_urf_raw(e, b, 4);
}

static int mp_urf_write_file_header(MPUrfEncoder *e, int duplex)
{
    static const unsigned char sync[4] = { 'U', 'N', 'I', 'R' };
    static const unsigned char rest[4] = { 'A', 'S', 'T', 0 };

    if (!mp_urf_raw(e, sync, 4)) return 0;
    if (!mp_urf_raw(e, rest, 4)) return 0;
    /* Single page: the real, final count. Duplex: an honest "unknown/
     * streaming" placeholder - the caller patches this once the true
     * total is known, before closing the file (see urf_writer.h). */
    return mp_urf_u32(e, duplex ? 0xffffffffUL : 1UL);
}

static int mp_urf_write_page_header(MPUrfEncoder *e, int duplex, int tumble)
{
    unsigned char b[6];

    b[0] = 24;  /* cupsBitsPerPixel: 8-bit sRGB, chunked RGB */
    b[1] = 1;   /* colorspace: sRGB */
    b[2] = duplex ? (tumble ? 2 : 3) : 1; /* duplex/tumble mode */
    b[3] = 0;   /* print quality: unspecified */
    b[4] = 0;   /* media type: unspecified */
    b[5] = 0;   /* media position: auto */
    if (!mp_urf_raw(e, b, 6)) return 0;

    if (!mp_urf_zeros(e, 6UL)) return 0; /* bytes 6-11: reserved */

    if (!mp_urf_u32(e, e->width)) return 0;   /* bytes 12-15 */
    if (!mp_urf_u32(e, e->height)) return 0;  /* bytes 16-19 */
    if (!mp_urf_u32(e, e->dpi)) return 0;     /* bytes 20-23 */

    return mp_urf_zeros(e, 8UL); /* bytes 24-31: reserved */
}

/* Identical algorithm to mp_pwg_build_row() in pwg_writer.c - see that
 * file's comment and this file's header comment for why only the
 * "repeat run" half of the shared PackBits scheme is ever emitted. */
static int mp_urf_build_row(MPUrfEncoder *e, const unsigned char *rgb,
                            unsigned long *encoded_length)
{
    unsigned long px = 0;
    unsigned long out = 0;
    unsigned char *scratch = e->scratch;

    if (out >= e->scratch_size) return 0;
    scratch[out++] = 0; /* line-repeat count: this line's data appears once */

    while (px < e->width) {
        const unsigned char *p0 = rgb + px * 3UL;
        unsigned long run = 1;

        while (run < 128UL && (px + run) < e->width) {
            const unsigned char *pn = rgb + (px + run) * 3UL;
            if (pn[0] != p0[0] || pn[1] != p0[1] || pn[2] != p0[2]) break;
            ++run;
        }

        if (out + 1UL + 3UL > e->scratch_size) return 0;
        scratch[out++] = (unsigned char)(run - 1UL);
        scratch[out++] = p0[0];
        scratch[out++] = p0[1];
        scratch[out++] = p0[2];

        px += run;
    }

    if (encoded_length) *encoded_length = out;
    return 1;
}

unsigned long mp_urf_scratch_size(unsigned long width)
{
    if (!width || width > 0x0fffffffUL) return 0;
    return width * 4UL + 16UL;
}

int mp_urf_begin(MPUrfEncoder *e, unsigned long width, unsigned long height,
                 unsigned long dpi,
                 unsigned char *scratch, unsigned long scratch_size,
                 MPUrfWriteFn write_fn, void *write_ctx)
{
    return mp_urf_begin_page(e, width, height, dpi, 1, 0, 0,
                             scratch, scratch_size, write_fn, write_ctx);
}

int mp_urf_begin_page(MPUrfEncoder *e,
                      unsigned long width, unsigned long height,
                      unsigned long dpi, int write_file_header,
                      int duplex, int tumble,
                      unsigned char *scratch, unsigned long scratch_size,
                      MPUrfWriteFn write_fn, void *write_ctx)
{
    unsigned long need;
    if (!e || !width || !height || width > 65535UL || height > 65535UL ||
        !scratch || !write_fn)
        return 0;
    need = mp_urf_scratch_size(width);
    if (!need || scratch_size < need) return 0;

    e->width = width;
    e->height = height;
    e->rows_written = 0;
    e->dpi = dpi ? dpi : 300UL;
    e->scratch = scratch;
    e->scratch_size = scratch_size;
    e->write_fn = write_fn;
    e->write_ctx = write_ctx;
    e->failed = 0;

    if (write_file_header && !mp_urf_write_file_header(e, duplex)) return 0;
    return mp_urf_write_page_header(e, duplex, tumble);
}

int mp_urf_write_scanline(MPUrfEncoder *e, const unsigned char *rgb)
{
    unsigned long encoded_length;

    if (!e || e->failed || !rgb || e->rows_written >= e->height) return 0;
    if (!mp_urf_build_row(e, rgb, &encoded_length)) {
        e->failed = 1;
        return 0;
    }
    if (!mp_urf_raw(e, e->scratch, encoded_length)) return 0;
    ++e->rows_written;
    return 1;
}

int mp_urf_finish(MPUrfEncoder *e)
{
    if (!e || e->failed) return 0;
    return e->rows_written == e->height;
}
