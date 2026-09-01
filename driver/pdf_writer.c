/*
 * See pdf_writer.h for the overall design. Object numbering used
 * throughout this file:
 *   1 = Catalog     2 = Pages       3 = Page (this one page)
 *   4 = Image XObject (streamed - the JPEG bytes, verbatim)
 *   5 = Contents (the tiny fixed content stream that places the image)
 *   6 = deferred /Length integer for object 4, written after its stream
 *
 * Written to the file in the order 1, 2, 3, 5, 4, 6 - object 5's content
 * is fully known up front (unlike 4's), so it is written before 4 to
 * avoid needing any deferral for it too. PDF does not require objects to
 * appear in the file in numeric order; the xref table maps each object
 * number to wherever it actually landed.
 */

#include "pdf_writer.h"

static int mp_pdf_raw(MPPdfEncoder *e, const unsigned char *p, unsigned long n)
{
    if (e->failed) return 0;
    if (!n) return 1;
    if (!e->write_fn || e->write_fn(e->write_ctx, p, n) != (long)n) {
        e->failed = 1;
        return 0;
    }
    e->file_offset += n;
    return 1;
}

static unsigned long mp_pdf_strlen(const char *s)
{
    unsigned long n = 0;
    while (s && s[n]) ++n;
    return n;
}

static int mp_pdf_lit(MPPdfEncoder *e, const char *s)
{
    return mp_pdf_raw(e, (const unsigned char *)s, mp_pdf_strlen(s));
}

/* Digits of v, most-significant first, into buf. Returns the digit count
 * (always at least 1, for v == 0). buf must hold at least 10 bytes. */
static int mp_pdf_uint_to_buf(char *buf, unsigned long v)
{
    char tmp[10];
    int n = 0;
    int i;

    if (v == 0) {
        buf[0] = '0';
        return 1;
    }
    while (v && n < 10) {
        tmp[n++] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    for (i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    return n;
}

static int mp_pdf_uint(MPPdfEncoder *e, unsigned long v)
{
    char buf[10];
    int n = mp_pdf_uint_to_buf(buf, v);
    return mp_pdf_raw(e, (const unsigned char *)buf, (unsigned long)n);
}

/* Zero-padded to exactly `width` digits, truncating any higher digits -
 * only ever used for the xref table's 10-digit offsets and 5-digit
 * generation numbers, both far too small to realistically overflow for
 * anything this driver produces. */
static int mp_pdf_uint_fixed(MPPdfEncoder *e, unsigned long v, int width)
{
    char buf[10];
    int i;

    for (i = width - 1; i >= 0; --i) {
        buf[i] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    return mp_pdf_raw(e, (const unsigned char *)buf, (unsigned long)width);
}

static int mp_pdf_xref_entry(MPPdfEncoder *e, unsigned long offset, int free_entry)
{
    if (!mp_pdf_uint_fixed(e, offset, 10)) return 0;
    if (!mp_pdf_lit(e, " ")) return 0;
    if (!mp_pdf_uint_fixed(e, free_entry ? 65535UL : 0UL, 5)) return 0;
    return mp_pdf_lit(e, free_entry ? " f \n" : " n \n");
}

/* Bytes flowing through the JPEG encoder while the image XObject's stream
 * (object 4) is open - forwards to the real sink and counts them, both
 * for the running file offset (mp_pdf_raw already does that) and
 * separately for the stream's own byte count, needed for object 6. */
static long mp_pdf_jpeg_write_fn(void *ctx, const unsigned char *data, unsigned long len)
{
    MPPdfEncoder *e = (MPPdfEncoder *)ctx;
    if (!mp_pdf_raw(e, data, len)) return -1;
    e->image_stream_bytes += len;
    return (long)len;
}

unsigned long mp_pdf_scratch_size(unsigned long width)
{
    /* The JPEG encoder embedded verbatim is the only per-row scratch this
     * needs - every PDF-specific write above is small, fixed-size ASCII. */
    return mp_jpeg_scratch_size(width);
}

int mp_pdf_begin(MPPdfEncoder *e, unsigned long width, unsigned long height,
                 unsigned long dpi,
                 unsigned char *scratch, unsigned long scratch_size,
                 MPPdfWriteFn write_fn, void *write_ctx)
{
    unsigned long pdf_w, pdf_h;

    if (!e || !width || !height || width > 65535UL || height > 65535UL ||
        !scratch || !write_fn)
        return 0;

    e->width = width;
    e->height = height;
    e->write_fn = write_fn;
    e->write_ctx = write_ctx;
    e->file_offset = 0;
    e->image_stream_bytes = 0;
    e->failed = 0;
    if (!dpi) dpi = 300UL;

    /* MediaBox in points (1/72in), from pixels at the capture DPI -
     * matches the PWG Raster path's PageSize field exactly (see
     * pwg_writer.c). */
    pdf_w = (width * 72UL) / dpi;
    pdf_h = (height * 72UL) / dpi;
    if (!pdf_w) pdf_w = 1UL;
    if (!pdf_h) pdf_h = 1UL;

    if (!mp_pdf_lit(e, "%PDF-1.4\n")) return 0;

    e->obj_offset[1] = e->file_offset;
    if (!mp_pdf_lit(e, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) return 0;

    e->obj_offset[2] = e->file_offset;
    if (!mp_pdf_lit(e, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) return 0;

    e->obj_offset[3] = e->file_offset;
    if (!mp_pdf_lit(e, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ")) return 0;
    if (!mp_pdf_uint(e, pdf_w)) return 0;
    if (!mp_pdf_lit(e, " ")) return 0;
    if (!mp_pdf_uint(e, pdf_h)) return 0;
    if (!mp_pdf_lit(e, "] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>\nendobj\n")) return 0;

    /* Object 5 (content stream): fully known up front, so written whole
     * here rather than deferred like object 4's length. */
    {
        char cbuf[48];
        unsigned long clen = 0;
        int n;

        /* Scales by the MediaBox's point dimensions (pdf_w/pdf_h), not the
         * raw pixel width/height - PDF images occupy the unit square in
         * their own image space, and "cm" here maps that square onto the
         * page in USER SPACE UNITS (points), which is what MediaBox is
         * expressed in. Using pixel counts here would draw the image far
         * larger than the page itself. */
        n = mp_pdf_uint_to_buf(cbuf + clen, pdf_w); clen += (unsigned long)n;
        cbuf[clen++] = ' '; cbuf[clen++] = '0'; cbuf[clen++] = ' '; cbuf[clen++] = '0'; cbuf[clen++] = ' ';
        n = mp_pdf_uint_to_buf(cbuf + clen, pdf_h); clen += (unsigned long)n;
        cbuf[clen++] = ' '; cbuf[clen++] = '0'; cbuf[clen++] = ' '; cbuf[clen++] = '0';
        cbuf[clen++] = ' '; cbuf[clen++] = 'c'; cbuf[clen++] = 'm'; cbuf[clen++] = '\n';
        cbuf[clen++] = '/'; cbuf[clen++] = 'I'; cbuf[clen++] = 'm'; cbuf[clen++] = '0';
        cbuf[clen++] = ' '; cbuf[clen++] = 'D'; cbuf[clen++] = 'o'; cbuf[clen++] = '\n';

        e->obj_offset[5] = e->file_offset;
        if (!mp_pdf_lit(e, "5 0 obj\n<< /Length ")) return 0;
        if (!mp_pdf_uint(e, clen)) return 0;
        if (!mp_pdf_lit(e, " >>\nstream\n")) return 0;
        if (!mp_pdf_raw(e, (const unsigned char *)cbuf, clen)) return 0;
        if (!mp_pdf_lit(e, "\nendstream\nendobj\n")) return 0;
    }

    e->obj_offset[4] = e->file_offset;
    if (!mp_pdf_lit(e, "4 0 obj\n<< /Type /XObject /Subtype /Image /Width ")) return 0;
    if (!mp_pdf_uint(e, width)) return 0;
    if (!mp_pdf_lit(e, " /Height ")) return 0;
    if (!mp_pdf_uint(e, height)) return 0;
    if (!mp_pdf_lit(e, " /ColorSpace /DeviceRGB /BitsPerComponent 8"
                        " /Filter /DCTDecode /Length 6 0 R >>\nstream\n")) return 0;

    if (!mp_jpeg_begin_dpi(&e->jpeg, width, height, dpi,
                           scratch, scratch_size,
                           mp_pdf_jpeg_write_fn, e)) {
        e->failed = 1;
        return 0;
    }

    return !e->failed;
}

int mp_pdf_write_scanline(MPPdfEncoder *e, const unsigned char *rgb)
{
    if (!e || e->failed || !rgb) return 0;
    if (!mp_jpeg_write_scanline(&e->jpeg, rgb)) {
        e->failed = 1;
        return 0;
    }
    return 1;
}

int mp_pdf_finish(MPPdfEncoder *e)
{
    unsigned long xref_offset;
    int i;

    if (!e || e->failed) return 0;
    if (!mp_jpeg_finish(&e->jpeg)) {
        e->failed = 1;
        return 0;
    }

    if (!mp_pdf_lit(e, "endstream\nendobj\n")) return 0;

    e->obj_offset[6] = e->file_offset;
    if (!mp_pdf_lit(e, "6 0 obj\n")) return 0;
    if (!mp_pdf_uint(e, e->image_stream_bytes)) return 0;
    if (!mp_pdf_lit(e, "\nendobj\n")) return 0;

    xref_offset = e->file_offset;
    if (!mp_pdf_lit(e, "xref\n0 7\n")) return 0;
    if (!mp_pdf_xref_entry(e, 0UL, 1)) return 0;
    for (i = 1; i <= 6; ++i) {
        if (!mp_pdf_xref_entry(e, e->obj_offset[i], 0)) return 0;
    }

    if (!mp_pdf_lit(e, "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n")) return 0;
    if (!mp_pdf_uint(e, xref_offset)) return 0;
    if (!mp_pdf_lit(e, "\n%%EOF\n")) return 0;

    return !e->failed;
}
