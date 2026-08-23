#ifndef MINTPRINT_POSTSCRIPT_WRITER_H
#define MINTPRINT_POSTSCRIPT_WRITER_H

#include "jpeg_writer.h"

/*
 * Minimal streaming single-page PostScript Level 2 encoder.
 *
 * The existing JPEG encoder supplies the compressed image data, but the
 * printer receives a real application/postscript document rather than a
 * direct image/jpeg job. JPEG bytes are ASCIIHex-wrapped and consumed by
 * PostScript's standard ASCIIHexDecode and DCTDecode filters. ASCIIHex is
 * larger on the wire than ASCII85, but its encoding is only two nibble
 * lookups per byte - deliberately trading LAN bytes for much less 68k CPU.
 */

typedef long (*MPPostScriptWriteFn)(void *ctx,
                                    const unsigned char *data,
                                    unsigned long len);

#define MP_POSTSCRIPT_OUTPUT_BUFFER 4096UL

typedef struct MPPostScriptEncoder {
    unsigned long width;
    unsigned long height;
    MPPostScriptWriteFn write_fn;
    void *write_ctx;
    MPJpegEncoder jpeg;
    unsigned int asciihex_column;
    unsigned char outbuf[MP_POSTSCRIPT_OUTPUT_BUFFER];
    unsigned long out_used;
    unsigned long output_bytes;
    unsigned long write_calls;
    int failed;
} MPPostScriptEncoder;

/*
 * Sets the PostScript placement policy used by subsequent jobs. The driver
 * updates this from Unit0's SCALING= value when configuration is loaded.
 * Unknown/empty values fall back to the historical auto-fit behaviour.
 */
void mp_postscript_set_scaling(const char *scaling);

unsigned long mp_postscript_scratch_size(unsigned long width);
int mp_postscript_begin(MPPostScriptEncoder *enc,
                        unsigned long width, unsigned long height,
                        unsigned long page_width_points,
                        unsigned long page_height_points,
                        unsigned long dpi,
                        unsigned char *scratch, unsigned long scratch_size,
                        MPPostScriptWriteFn write_fn, void *write_ctx);
int mp_postscript_write_scanline(MPPostScriptEncoder *enc,
                                 const unsigned char *rgb);
int mp_postscript_finish(MPPostScriptEncoder *enc);

#endif
