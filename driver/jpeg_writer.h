#ifndef MINTPRINT_JPEG_WRITER_H
#define MINTPRINT_JPEG_WRITER_H

typedef long (*MPJpegWriteFn)(void *ctx, const unsigned char *data, unsigned long len);

typedef struct MPJpegEncoder {
    unsigned long width;
    unsigned long height;
    unsigned long dpi;
    unsigned long rows_in;
    unsigned long mcu_rows_done;
    unsigned long blocks_total;
    unsigned long blocks_constant;
    unsigned char *scratch;
    unsigned long scratch_size;
    MPJpegWriteFn write_fn;
    void *write_ctx;
    unsigned char outbuf[4096];
    unsigned long out_used;
    unsigned long bit_acc;
    int bit_count;
    int dc_y;
    int dc_cb;
    int dc_cr;
    int failed;
} MPJpegEncoder;

unsigned long mp_jpeg_scratch_size(unsigned long width);
int mp_jpeg_begin_dpi(MPJpegEncoder *enc, unsigned long width, unsigned long height,
                      unsigned long dpi,
                      unsigned char *scratch, unsigned long scratch_size,
                      MPJpegWriteFn write_fn, void *write_ctx);
int mp_jpeg_begin(MPJpegEncoder *enc, unsigned long width, unsigned long height,
                  unsigned char *scratch, unsigned long scratch_size,
                  MPJpegWriteFn write_fn, void *write_ctx);
int mp_jpeg_write_scanline(MPJpegEncoder *enc, const unsigned char *rgb);
int mp_jpeg_finish(MPJpegEncoder *enc);

/* Below this line: otherwise-internal AAN forward-DCT/quantiser pieces,
 * exposed only so tests/test_jpeg_writer.c can drive them directly and
 * verify the DCT math (encode -> dequantise -> reference IDCT -> compare
 * against the original block) without needing a full JPEG bitstream
 * decoder on the host. Not meant for use outside the encoder and its
 * test. */
void mp_fdct(const short *block, long *out);
int mp_quantize_aan(long raw, int q, long aan_scale);
extern const long mp_fdct_aan_scale[64];

#endif
