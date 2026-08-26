#ifndef MINTPRINT_URF_WRITER_H
#define MINTPRINT_URF_WRITER_H

/*
 * Minimal streaming Apple Raster (image/urf) encoder.
 *
 * Writes a "UNIRAST" stream: an 8-byte magic ('U','N','I','R','A','S','T',
 * 0x00) followed by a 4-byte big-endian page count, then one 32-byte page
 * header and compressed rows PER PAGE (unlike PWG Raster, whose page
 * header repeats unconditionally, Apple Raster's file-level page count is
 * written ONCE, up front - see mp_urf_begin_page()'s write_file_header
 * argument and MP_URF_PAGECOUNT_FIELD_OFFSET below for how a caller with
 * more than one page (duplex) patches it in after the fact, once the true
 * total is known).
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
 * Duplex: unlike PWG Raster's page header, Apple Raster's compact 32-byte
 * page header has no CrossFeedTransform/FeedTransform-equivalent fields
 * for describing a pre-mirrored backside - only a single duplex/tumble
 * byte (simplex/duplex-tumble/duplex-no-tumble). There is therefore
 * nothing for a sender to pre-flip: every page's raster rows stream in the
 * same natural top-to-bottom, left-to-right order regardless of front or
 * back side, and the printer's own duplex mechanism is trusted to handle
 * physical sheet orientation from the duplex/tumble hint alone. This is
 * the main assumption worth checking against a real physical duplex
 * print - if a backside comes out upside-down or mirrored, that is where
 * to look first.
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

/* Byte offset, from the start of the file, of the 4-byte big-endian page
 * count field in the 12-byte file header. A caller that streams more than
 * one page (duplex) writes a placeholder count at mp_urf_begin_page() time
 * and must patch this offset with the true total once known, before
 * closing the file - see driver_core.c's DriverClose() duplex flow and
 * MP_PWG_HEIGHT_HEADER_OFFSET's equivalent PWG precedent. */
#define MP_URF_PAGECOUNT_FIELD_OFFSET 8UL

unsigned long mp_urf_scratch_size(unsigned long width);

/* dpi is the capture resolution the raster was rendered at - written into
 * the page header's single HWResolution value (Apple Raster, unlike PWG,
 * declares only one resolution figure, used for both axes). Pass 0 to fall
 * back to 300dpi. Single page only - equivalent to
 * mp_urf_begin_page(enc, width, height, dpi, 1, 0, 0, ...). */
int mp_urf_begin(MPUrfEncoder *enc, unsigned long width, unsigned long height,
                 unsigned long dpi,
                 unsigned char *scratch, unsigned long scratch_size,
                 MPUrfWriteFn write_fn, void *write_ctx);

/* Starts another page in the same Apple Raster stream. write_file_header
 * must be true for the first page and false for every later page (see the
 * file comment above - the 12-byte file header, including the page count
 * placeholder, is written exactly once). duplex/tumble set the page
 * header's duplex byte: simplex (both false), duplex-tumble (duplex &&
 * tumble - matches IPP sides=two-sided-short-edge), or duplex-no-tumble
 * (duplex && !tumble - two-sided-long-edge). */
int mp_urf_begin_page(MPUrfEncoder *enc,
                      unsigned long width, unsigned long height,
                      unsigned long dpi, int write_file_header,
                      int duplex, int tumble,
                      unsigned char *scratch, unsigned long scratch_size,
                      MPUrfWriteFn write_fn, void *write_ctx);
int mp_urf_write_scanline(MPUrfEncoder *enc, const unsigned char *rgb);
int mp_urf_finish(MPUrfEncoder *enc);

#endif
