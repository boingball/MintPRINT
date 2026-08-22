#include "media_size.h"

static unsigned long mp_media_len(const char *s)
{
    unsigned long n = 0;
    while (s && s[n]) ++n;
    return n;
}

static int mp_decimal_1000(const char *s, unsigned long len,
                           unsigned long *value)
{
    unsigned long whole = 0, frac = 0, frac_digits = 0, i;
    int dot = 0, any = 0;

    if (!s || !len || !value) return 0;
    for (i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '.' && !dot) { dot = 1; continue; }
        if (c < '0' || c > '9') return 0;
        any = 1;
        if (!dot) {
            if (whole > 100000UL) return 0;
            whole = whole * 10UL + (unsigned long)(c - '0');
        } else if (frac_digits < 3) {
            frac = frac * 10UL + (unsigned long)(c - '0');
            ++frac_digits;
        }
    }
    if (!any) return 0;
    while (frac_digits < 3) { frac *= 10UL; ++frac_digits; }
    *value = whole * 1000UL + frac;
    return 1;
}

int mp_media_dimensions_100mm(const char *media,
                              unsigned long *x,
                              unsigned long *y)
{
    unsigned long len, i, start = 0, sep = 0, sx, sy;
    unsigned long factor;

    if (!media || !x || !y) return 0;
    len = mp_media_len(media);
    if (len < 6) return 0;

    for (i = 0; i < len; ++i) {
        if (media[i] == '_') start = i + 1;
    }
    for (i = start; i < len; ++i) {
        if (media[i] == 'x') { sep = i; break; }
    }
    if (!sep || sep <= start) return 0;

    if (len >= 2 && media[len - 2] == 'm' && media[len - 1] == 'm')
        factor = 100UL;
    else if (len >= 2 && media[len - 2] == 'i' && media[len - 1] == 'n')
        factor = 2540UL;
    else
        return 0;

    if (!mp_decimal_1000(media + start, sep - start, &sx) ||
        !mp_decimal_1000(media + sep + 1,
                         (len - 2) - (sep + 1), &sy))
        return 0;

    *x = (sx * factor + 500UL) / 1000UL;
    *y = (sy * factor + 500UL) / 1000UL;
    return (*x && *y) ? 1 : 0;
}

static unsigned long mp_abs_diff(unsigned long a, unsigned long b)
{
    return a >= b ? a - b : b - a;
}

unsigned long mp_media_target_height(const char *media,
                                     unsigned long width_px,
                                     unsigned long dpi)
{
    unsigned long x, y, expected_x, expected_y;
    unsigned long numerator, denominator, target;

    if (!width_px || !dpi ||
        !mp_media_dimensions_100mm(media, &x, &y))
        return 0;

    expected_x = (x * dpi + 1270UL) / 2540UL;
    expected_y = (y * dpi + 1270UL) / 2540UL;

    if (mp_abs_diff(width_px, expected_x) <=
        mp_abs_diff(width_px, expected_y)) {
        numerator = y;
        denominator = x;
    } else {
        numerator = x;
        denominator = y;
    }

    if (!denominator || width_px > 0xffffffffUL / numerator) return 0;
    target = (width_px * numerator + denominator / 2UL) / denominator;
    return target && target <= 65535UL ? target : 0;
}

unsigned long mp_media_expected_width_px(const char *media,
                                         unsigned long page_width_px,
                                         unsigned long page_height_px,
                                         unsigned long dpi)
{
    unsigned long x, y, media_x_px, media_y_px;
    unsigned long portrait_error, landscape_error;

    if (!page_width_px || !dpi ||
        !mp_media_dimensions_100mm(media, &x, &y))
        return 0;

    media_x_px = (x * dpi + 1270UL) / 2540UL;
    media_y_px = (y * dpi + 1270UL) / 2540UL;

    /* Match the full (width, height) pair against each orientation, not
     * just width - see the header comment for why width alone is
     * ambiguous for an oversized portrait page. */
    portrait_error = mp_abs_diff(page_width_px, media_x_px) +
                     mp_abs_diff(page_height_px, media_y_px);
    landscape_error = mp_abs_diff(page_width_px, media_y_px) +
                      mp_abs_diff(page_height_px, media_x_px);

    return portrait_error <= landscape_error ? media_x_px : media_y_px;
}

int mp_is_tiny_auxiliary_band(unsigned long page_width,
                              unsigned long band_width)
{
    return band_width > 0 && band_width <= 8UL &&
           page_width >= band_width * 64UL;
}

int mp_is_tiny_leading_auxiliary_band(unsigned long band_width)
{
    return band_width > 0 && band_width <= 8UL;
}

int mp_media_page_complete(unsigned long raster_rows,
                           unsigned long auxiliary_rows,
                           unsigned long target_rows)
{
    if (!target_rows) return 0;
    if (raster_rows >= target_rows) return 1;

    /* Written this way instead of adding the two row counts so malformed
     * input cannot wrap an unsigned long and hide a completed page. */
    return auxiliary_rows >= target_rows - raster_rows;
}
