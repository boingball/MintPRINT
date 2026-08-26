#ifndef MINTPRINT_URF_WRITER_H
#define MINTPRINT_URF_WRITER_H

/*
 * Minimal streaming Apple Raster (image/urf) encoder, single page.
 *
 * Writes a "UNIRAST" stream: an 8-byte magic ('U','N','I','R','A','S','T',
 * 0x00) followed by a 4-byte big-endian page count, then one 32-byte page
 * header and its compressed rows.
 *
 * The 12-byte file header and 32-byte page header layouts, and the row
 * compression scheme, were verified against the CUPS reference
 * implementation (cups/raster.h's CUPS_RASTER_SYNCapple/write-Apple-header
 * path in cups/raster-stream.c) rather than from memory alone, the same
 * standard this codebase's PWG Raster encoder (pwg_writer.h) already holds
 * itself to.
 *
 * Row compression is the identical PackBits-style scheme PWG Raster uses -
 * CUPS's own raster library shares one row-compression routine between its
 * PWG and Apple output modes - so this encoder reuses the same
 * "repeat-run-only" subset already proven there: every matching run of
 * identical pixels is compressed normally (control byte 0-127, meaning
 * "next pixel repeated (byte+1) times", 1-128 pixels), and any pixel that
 * does not extend a run is simply emitted as its own trivial run of one.
 * See pwg_writer.h for why this avoids the format's other, more
 * failure-prone "literal run" encoding entirely.
 *
 * Only a single page is supported - the same starting scope PDF and
 * PostScript already have in this codebase.
 */

typedef long (*MPUrfWriteFn)(void *ctx, const unsigned char *data, unsigned long len);

typedef struct MPUrfEncoder {
    unsigned long width;
    unsigned long height;
    unsigned long rows_written;
    unsigned long dpi;
    unsigned char *scratch;
    unsigned long scratch_size;
    MPUrfWriteFn write_fn;
    void *write_ctx;
    int failed;
} MPUrfEncoder;

unsigned long mp_urf_scratch_size(unsigned long width);

/* dpi is the capture resolution the raster was rendered at - written into
 * the page header's single HWResolution value (Apple Raster, unlike PWG,
 * declares only one resolution figure, used for both axes). Pass 0 to fall
 * back to 300dpi. */
int mp_urf_begin(MPUrfEncoder *enc, unsigned long width, unsigned long height,
                 unsigned long dpi,
                 unsigned char *scratch, unsigned long scratch_size,
                 MPUrfWriteFn write_fn, void *write_ctx);
int mp_urf_write_scanline(MPUrfEncoder *enc, const unsigned char *rgb);
int mp_urf_finish(MPUrfEncoder *enc);

#endif
