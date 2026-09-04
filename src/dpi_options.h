#ifndef MINTPRINT_DPI_OPTIONS_H
#define MINTPRINT_DPI_OPTIONS_H

#define MP_DPI_MAX_OPTIONS 8

struct MPDpiOptions {
    int values[MP_DPI_MAX_OPTIONS];
    int compatibility[MP_DPI_MAX_OPTIONS];
    int count;
    int active;
    int selected;
};

/* Only the two directly generated raster formats expose the unreported
 * 300-DPI override. Kept pure so the engine gating is host-testable. */
int mp_dpi_engine_allows_compat(const char *engine);

/* Builds the DPI choices shown by Settings without changing the printer's
 * reported capability list. PWG Raster and Apple Raster (URF) get a marked
 * 300-DPI compatibility choice when a printer reports resolutions but omits
 * 300 DPI. A saved or explicitly selected 300-DPI value may remain active;
 * otherwise a reported resolution stays the default. */
void mp_dpi_build_options(const int *reported, int reported_count,
                          int raster_compat, int requested,
                          int requested_explicit,
                          struct MPDpiOptions *out);

#endif
