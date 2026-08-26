#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/media_size.h"
#include "../driver/pwg_writer.h"

struct TestSink {
    unsigned char header[1800];
    unsigned long bytes;
};

static long sink_write(void *ctx, const unsigned char *data,
                       unsigned long length)
{
    struct TestSink *sink = (struct TestSink *)ctx;
    unsigned long copy = length;

    if (sink->bytes >= sizeof(sink->header)) copy = 0;
    else if (copy > sizeof(sink->header) - sink->bytes)
        copy = sizeof(sink->header) - sink->bytes;
    if (copy) memcpy(sink->header + sink->bytes, data, copy);
    sink->bytes += length;
    return (long)length;
}

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) |
           ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

int main(void)
{
    unsigned long x = 0, y = 0;
    unsigned long width = 2478UL, height, row;
    unsigned long logical_rows, logical_pages, band_rows;
    unsigned long scratch_size;
    unsigned char *scratch, *rgb;
    struct TestSink sink;
    MPPwgEncoder encoder;
    struct TestSink page_sink;
    unsigned char tiny_rgb[3] = { 255, 255, 255 };
    unsigned char tiny_scratch[32];
    unsigned char reverse_row[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    const unsigned char *encoded_row;
    unsigned long encoded_length;
    long cross_feed, feed;

    assert(mp_media_dimensions_100mm("iso_a4_210x297mm", &x, &y));
    assert(x == 21000UL && y == 29700UL);
    assert(mp_media_dimensions_100mm("na_letter_8.5x11in", &x, &y));
    assert(x == 21590UL && y == 27940UL);
    assert(!mp_media_dimensions_100mm("unknown", &x, &y));

    assert(mp_media_page_points("iso_a4_210x297mm", 2478UL, 3507UL,
                                &x, &y));
    assert(x == 595UL && y == 842UL);
    assert(mp_media_page_points("iso_a4_210x297mm", 3507UL, 2478UL,
                                &x, &y));
    assert(x == 842UL && y == 595UL);
    assert(mp_media_page_points("na_letter_8.5x11in", 2550UL, 3300UL,
                                &x, &y));
    assert(x == 612UL && y == 792UL);
    assert(!mp_media_page_points("unknown", 2478UL, 3507UL, &x, &y));

    /* Exact dimensions observed in the Wordworth regression log. */
    assert(mp_media_target_height("iso_a4_210x297mm", 2478UL, 300UL) ==
           3505UL);
    assert(mp_media_target_height("iso_a4_210x297mm", 3508UL, 300UL) ==
           2480UL);
    assert(mp_media_target_height("na_letter_8.5x11in", 2550UL, 300UL) ==
           3300UL);
    assert(mp_media_target_height("unknown", 2478UL, 300UL) == 0UL);

    /* mp_media_expected_width_px() must tell an oversized-but-still-portrait
     * page apart from a genuine landscape one using BOTH reported
     * dimensions, not width alone. A4 at 300 DPI: portrait is 2480x3508,
     * landscape is 3508x2480. printer.device has been observed reporting a
     * 3287x3508 page for a DUMPRPORT-scaled portrait A4 request (see
     * driver_core.c's PWG page-width clamp) - width alone (3287) sits
     * closer to the landscape width (3508, 221px away) than the portrait
     * one (2480, 807px away), which would misread this as landscape and
     * never clamp it. Bringing height into it fixes that: 3287x3508 still
     * matches portrait overall (its height is exactly portrait's), so the
     * expected width comes back 2480 and the clamp fires as intended. A
     * real landscape page (3508x2480) matches landscape on both axes and
     * must NOT be clamped. */
    assert(mp_media_expected_width_px("iso_a4_210x297mm", 2480UL, 3508UL,
                                      300UL) == 2480UL);
    assert(mp_media_expected_width_px("iso_a4_210x297mm", 3287UL, 3508UL,
                                      300UL) == 2480UL);
    assert(mp_media_expected_width_px("iso_a4_210x297mm", 3508UL, 2480UL,
                                      300UL) == 3508UL);
    assert(mp_media_expected_width_px("unknown", 2480UL, 3508UL, 300UL) ==
           0UL);
    assert(mp_media_expected_width_px("iso_a4_210x297mm", 0UL, 3508UL,
                                      300UL) == 0UL);

    assert(mp_is_tiny_auxiliary_band(2478UL, 4UL));
    assert(!mp_is_tiny_auxiliary_band(2478UL, 100UL));
    assert(!mp_is_tiny_auxiliary_band(200UL, 4UL));
    assert(mp_is_tiny_leading_auxiliary_band(4UL));
    assert(mp_is_tiny_leading_auxiliary_band(8UL));
    assert(!mp_is_tiny_leading_auxiliary_band(0UL));
    assert(!mp_is_tiny_leading_auxiliary_band(9UL));

    /* Rev15 merged these real multi-page sequences into one over-tall PWG
     * page. A complete full-height raster is a page even when the app leaves
     * NOFORMFEED set; the customer's two strips plus its narrow blank tail
     * also reach one page. The older Wordworth 2100-row strip sequence plus
     * 962 rows of narrow auxiliary dumps is still short of A4 and must stay
     * open so finalisation can pad it to portrait media height. */
    assert(mp_media_page_complete(3507UL, 0UL, 3507UL));
    assert(mp_media_page_complete(3170UL, 285UL, 3437UL));
    assert(!mp_media_page_complete(2100UL, 962UL, 3505UL));
    assert(!mp_media_page_complete(3507UL, 0UL, 0UL));

    /* Rev35 still split this Wordsworth job at physical A4 row 3505 even
     * though the application's printable page ended at 3062: thirty
     * 100-row bands plus a final 62-row remainder. The following 443 rows
     * were therefore stolen from page 2. A short final full-width band and
     * the same short final auxiliary band must both delimit the logical page.
     * A full-sized band, an early short band, or unknown media must not. */
    assert(mp_short_strip_completes_logical_page(3062UL, 0UL, 3505UL,
                                                  100UL, 62UL));
    assert(mp_short_strip_completes_logical_page(700UL, 2362UL, 3505UL,
                                                  100UL, 62UL));
    assert(!mp_short_strip_completes_logical_page(3000UL, 0UL, 3505UL,
                                                   100UL, 100UL));
    assert(!mp_short_strip_completes_logical_page(2100UL, 0UL, 3505UL,
                                                   100UL, 62UL));
    assert(!mp_short_strip_completes_logical_page(3062UL, 0UL, 0UL,
                                                   100UL, 62UL));

    /* Wordsworth's 0.50-inch top border is sent through printer.device as
     * DEC top-margin line 4 at the normal 1/6-inch (36/216-inch) VMI. The
     * driver must turn the three preceding lines into 150 white rows at
     * 300 DPI before writing the application's 3062-row printable raster. */
    assert(mp_text_top_margin_rows(4UL, 36UL, 300UL) == 150UL);
    assert(mp_text_top_margin_rows(5UL, 27UL, 300UL) == 150UL);
    assert(mp_text_top_margin_rows(1UL, 36UL, 300UL) == 0UL);
    assert(mp_text_top_margin_rows(0UL, 36UL, 300UL) == 0UL);
    assert(mp_text_top_margin_rows(4UL, 0UL, 300UL) == 0UL);
    assert(mp_text_top_margin_rows(4UL, 36UL, 0UL) == 0UL);
    assert(mp_text_top_margin_rows(4UL, 36UL, 300UL) + 3062UL == 3212UL);
    assert(3505UL -
           (mp_text_top_margin_rows(4UL, 36UL, 300UL) + 3062UL) == 293UL);

    /* The rev37 trace proved Wordsworth does not send aSTBM: it sends aCAM
     * followed by aSLPP 70, then a 3062-row printable raster. Seventy lines
     * at 6 LPI is 3500 rows at 300 DPI, matching the width-derived 3505-row
     * A4 sheet within rounding. Restore the documented 0.50-inch top border
     * only for that matching form/media signature. */
    assert(mp_wordsworth_top_margin_rows(70UL, 3505UL, 300UL) == 150UL);
    assert(mp_wordsworth_top_margin_rows(66UL, 3300UL, 300UL) == 150UL);
    assert(mp_wordsworth_top_margin_rows(70UL, 7010UL, 600UL) == 300UL);
    assert(mp_wordsworth_top_margin_rows(70UL, 3505UL, 300UL) + 3062UL ==
           3212UL);
    assert(!mp_wordsworth_top_margin_rows(70UL, 3300UL, 300UL));
    assert(!mp_wordsworth_top_margin_rows(0UL, 3505UL, 300UL));
    assert(!mp_wordsworth_top_margin_rows(70UL, 0UL, 300UL));
    assert(!mp_wordsworth_top_margin_rows(70UL, 3505UL, 0UL));

    /* Three ordinary 1/6-inch advances are Wordsworth's 0.50-inch top
     * position when it communicates the placement through aIND/aNEL/LF. */
    assert(mp_vertical_advance_rows(3UL * 36UL, 300UL) == 150UL);
    assert(mp_vertical_advance_rows(3UL * 36UL, 600UL) == 300UL);
    assert(mp_vertical_advance_rows(4UL * 27UL, 300UL) == 150UL);
    assert(!mp_vertical_advance_rows(0UL, 300UL));
    assert(!mp_vertical_advance_rows(108UL, 0UL));

    logical_rows = 0;
    logical_pages = 0;
    for (row = 0; row < 31UL; ++row) {
        band_rows = row == 30UL ? 62UL : 100UL;
        logical_rows += band_rows;
        if (mp_short_strip_completes_logical_page(
                logical_rows, 0UL, 3505UL, 100UL, band_rows)) {
            ++logical_pages;
            logical_rows = 0;
        }
    }
    assert(logical_pages == 1UL && logical_rows == 0UL);

    /* The fixed Wordworth path starts PWG at the complete media height,
     * then writes/pads exactly that many rows. Its header must therefore
     * remain portrait-shaped and internally consistent. */
    height = mp_media_target_height("iso_a4_210x297mm", width, 300UL);
    scratch_size = mp_pwg_scratch_size(width);
    scratch = (unsigned char *)malloc(scratch_size);
    rgb = (unsigned char *)malloc(width * 3UL);
    assert(scratch && rgb);
    memset(&sink, 0, sizeof(sink));
    memset(rgb, 255, width * 3UL);
    assert(mp_pwg_begin(&encoder, width, height, 0, 0, 300UL,
                        scratch, scratch_size, sink_write, &sink));
    assert(memcmp(sink.header + 4UL, "PwgRaster", 9) == 0);
    assert(be32(sink.header + MP_PWG_PAGESIZE_Y_FIELD_OFFSET) == 841UL);
    assert(be32(sink.header + MP_PWG_HEIGHT_FIELD_OFFSET) == height);
    assert(be32(sink.header + MP_PWG_COMPRESSION_FIELD_OFFSET) == 0UL);
    assert(be32(sink.header + MP_PWG_ROWCOUNT_FIELD_OFFSET) == 0UL);
    for (row = 0; row < height; ++row)
        assert(mp_pwg_write_scanline(&encoder, rgb));
    assert(mp_pwg_finish(&encoder));
    free(rgb);
    free(scratch);

    /* A multi-page PWG document has RaS2 once, then a bare 1796-byte page
     * header for every additional page. */
    memset(&page_sink, 0, sizeof(page_sink));
    assert(mp_pwg_begin_page(&encoder, 1, 1, 0, 0, 300UL, 1,
                             1, 0, 1, 1,
                             tiny_scratch, sizeof(tiny_scratch),
                             sink_write, &page_sink));
    assert(page_sink.bytes == 1800UL);
    assert(memcmp(page_sink.header, "RaS2", 4) == 0);
    assert(be32(page_sink.header + 4UL + 272UL) == 1UL); /* Duplex */
    assert(be32(page_sink.header + 4UL + 368UL) == 0UL); /* Tumble */
    assert(be32(page_sink.header + 4UL + 456UL) == 1UL);
    assert(be32(page_sink.header + 4UL + 460UL) == 1UL);
    assert(mp_pwg_encode_scanline(&encoder, tiny_rgb,
                                  &encoded_row, &encoded_length));
    assert(page_sink.bytes == 1800UL);
    assert(encoder.rows_written == 0UL);
    assert(mp_pwg_write_encoded_scanline(&encoder, encoded_row,
                                         encoded_length));
    assert(mp_pwg_finish(&encoder));

    memset(&page_sink, 0, sizeof(page_sink));
    assert(mp_pwg_begin_page(&encoder, 1, 1, 0, 0, 300UL, 0,
                             1, 1, -1, -1,
                             tiny_scratch, sizeof(tiny_scratch),
                             sink_write, &page_sink));
    assert(page_sink.bytes == 1796UL);
    assert(memcmp(page_sink.header, "RaS2", 4) != 0);
    assert(memcmp(page_sink.header, "PwgRaster", 9) == 0);
    assert(be32(page_sink.header + MP_PWG_HEIGHT_HEADER_OFFSET) == 1UL);
    assert(be32(page_sink.header + MP_PWG_COMPRESSION_HEADER_OFFSET) == 0UL);
    assert(be32(page_sink.header + MP_PWG_ROWCOUNT_HEADER_OFFSET) == 0UL);
    assert(be32(page_sink.header + 272UL) == 1UL); /* Duplex */
    assert(be32(page_sink.header + 368UL) == 1UL); /* Tumble */
    assert(be32(page_sink.header + 456UL) == 0xffffffffUL);
    assert(be32(page_sink.header + 460UL) == 0xffffffffUL);
    assert(mp_pwg_write_scanline(&encoder, tiny_rgb));
    assert(mp_pwg_finish(&encoder));

    /* PWG 5102.4 Table 9 backside coordinate systems. */
    mp_pwg_backside_transform("two-sided-long-edge", "rotated",
                              &cross_feed, &feed);
    assert(cross_feed == -1 && feed == -1);
    mp_pwg_backside_transform("two-sided-short-edge", "rotated",
                              &cross_feed, &feed);
    assert(cross_feed == 1 && feed == 1);
    mp_pwg_reverse_rgb_row(reverse_row, 3UL);
    assert(reverse_row[0] == 7 && reverse_row[1] == 8 &&
           reverse_row[2] == 9 && reverse_row[3] == 4 &&
           reverse_row[4] == 5 && reverse_row[5] == 6 &&
           reverse_row[6] == 1 && reverse_row[7] == 2 &&
           reverse_row[8] == 3);
    mp_pwg_backside_transform("two-sided-long-edge", "flipped",
                              &cross_feed, &feed);
    assert(cross_feed == 1 && feed == -1);
    mp_pwg_backside_transform("two-sided-short-edge", "flipped",
                              &cross_feed, &feed);
    assert(cross_feed == -1 && feed == 1);
    mp_pwg_backside_transform("two-sided-long-edge", "manual-tumble",
                              &cross_feed, &feed);
    assert(cross_feed == 1 && feed == 1);
    mp_pwg_backside_transform("two-sided-short-edge", "manual-tumble",
                              &cross_feed, &feed);
    assert(cross_feed == -1 && feed == -1);
    mp_pwg_backside_transform("two-sided-long-edge", "normal",
                              &cross_feed, &feed);
    assert(cross_feed == 1 && feed == 1);

    puts("media/page geometry tests passed");
    return 0;
}
