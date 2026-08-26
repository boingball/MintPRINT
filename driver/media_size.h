#ifndef MINTPRINT_MEDIA_SIZE_H
#define MINTPRINT_MEDIA_SIZE_H

/* Parse the dimensions encoded in a PWG media keyword. Dimensions are
 * returned in hundredths of a millimetre, matching IPP media-size. */
int mp_media_dimensions_100mm(const char *media,
                              unsigned long *x,
                              unsigned long *y);

/* Convert a PWG media keyword to PostScript points and orient the physical
 * sheet to match the raster supplied by printer.device. This keeps the
 * document's /PageSize on a real supported medium while allowing landscape
 * application output. Returns zero when the media keyword is unknown. */
int mp_media_page_points(const char *media,
                         unsigned long raster_width,
                         unsigned long raster_height,
                         unsigned long *page_width,
                         unsigned long *page_height);

/* Infer the complete raster height for the configured media from the width
 * printer.device actually supplied. The closest media edge determines
 * portrait versus landscape, while using the real raster width preserves
 * any small printable-area rounding difference (2478 rather than 2480 for
 * A4 at 300 DPI, for example). Returns zero for an unknown media keyword. */
unsigned long mp_media_target_height(const char *media,
                                     unsigned long width_px,
                                     unsigned long dpi);

/* Infer which of the configured media's two pixel widths (portrait or
 * landscape) a reported page actually matches, from BOTH of its reported
 * dimensions - not width alone. Width alone is ambiguous: an oversized
 * portrait page (e.g. printer.device reporting 3287x3508 for an A4 page
 * that should be 2480x3508) can land closer to the media's landscape width
 * than its portrait one, which would misread a too-wide portrait page as a
 * legitimate landscape page and let the oversized width straight through.
 * Comparing the full (width, height) pair against both the portrait and
 * landscape orientation resolves that: the oversized portrait page still
 * matches portrait orientation overall because its height is right where
 * portrait expects it, while a genuine landscape page matches on both
 * axes at once. Returns zero for an unknown media keyword. */
unsigned long mp_media_expected_width_px(const char *media,
                                         unsigned long page_width_px,
                                         unsigned long page_height_px,
                                         unsigned long dpi);

/* Wordworth can finish a NOFORMFEED page with a four-pixel auxiliary dump.
 * It is not a new physical page and cannot be appended as horizontal rows to
 * the already-streamed full-width PWG raster. Keep the classifier narrow so
 * ordinary differently-sized pages are never swallowed. */
int mp_is_tiny_auxiliary_band(unsigned long page_width,
                              unsigned long band_width);

/* Some applications emit a narrow NOFORMFEED control dump before the real
 * page. At that point the page width is not known yet, so keep this leading
 * classifier limited to widths that cannot plausibly be a printable page. */
int mp_is_tiny_leading_auxiliary_band(unsigned long band_width);

/* Decide whether the logical vertical extent accumulated under
 * SPECIAL_NOFORMFEED has reached the configured physical media height.
 * Auxiliary dumps are not written into the full-width PWG raster, but their
 * height still contributes useful page-boundary evidence. A zero target
 * means the media was unknown, so no automatic boundary is inferred. */
int mp_media_page_complete(unsigned long raster_rows,
                           unsigned long auxiliary_rows,
                           unsigned long target_rows);

/* Strip printers normally use a fixed-height band and finish a logical
 * printable page with one shorter remainder band. Some applications keep
 * SPECIAL_NOFORMFEED set across that boundary, so the short band is the only
 * reliable page delimiter left in the Render stream. Accept it only when the
 * accumulated raster plus discarded auxiliary height is already close to the
 * physical media height; this keeps an unusually short intermediate band
 * from prematurely ejecting a page. */
int mp_short_strip_completes_logical_page(unsigned long raster_rows,
                                          unsigned long auxiliary_rows,
                                          unsigned long target_rows,
                                          unsigned long nominal_band_rows,
                                          unsigned long current_band_rows);

/* Convert the DEC aSTBM top-margin parameter to raster rows. The parameter
 * names a one-based text line, while VMI is expressed in 1/216-inch units;
 * therefore line 4 at the normal 1/6-inch VMI means three blank lines, or
 * exactly 0.5 inch. A zero/default parameter or missing VMI is deliberately
 * left unresolved rather than inventing an application margin. */
unsigned long mp_text_top_margin_rows(unsigned long top_line_parameter,
                                      unsigned long vmi_216ths,
                                      unsigned long dpi);

/* Wordsworth clears printer margins and sends only the complete form length
 * before its NOFORMFEED raster strips; its separate Print Borders values are
 * not present in IODRPReq or the command stream. Recognise that form-length
 * signature only when it agrees with the configured physical page at the
 * standard six lines per inch, then restore the documented 0.50-inch top
 * border. Returns zero when the form and media do not agree. */
unsigned long mp_wordsworth_top_margin_rows(unsigned long form_length_lines,
                                            unsigned long target_rows,
                                            unsigned long dpi);

/* Convert accumulated printer vertical-motion units (VMI is 1/216 inch)
 * into device-resolution rows. Used for aIND, aNEL and literal line feeds
 * observed before a graphics dump. */
unsigned long mp_vertical_advance_rows(unsigned long units_216ths,
                                       unsigned long dpi);

#endif
