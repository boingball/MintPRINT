/* Filled with production callbacks by test_graphics_boundary.py.
 * Only the OS-facing collaborators and page finalizer are mocks. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "media_size.h"

typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned short UWORD;
typedef unsigned char UBYTE;
typedef signed char BYTE;
typedef char *STRPTR;
typedef int BOOL;
#define VOID void
#define TRUE 1
#define FALSE 0
#define PRT_STDARGS
#define MP_CONFIG_OPTION_MAX 64
#define SPECIAL_NOFORMFEED 0x800
#define PDERR_NOERR 0
#define PDERR_CANCEL 1
enum { aRIS, aRIN, aIND, aNEL, aVERP0 = 55, aVERP1, aSLPP, aPERF,
       aPERF0, aLMS, aRMS, aTMS, aBMS, aSLRM, aSTBM, aCAM };

/* CONFIG_STATE */

static BOOL g_page_pending, g_graphics_boundary_failed, g_duplex_job_failed;
static BOOL g_strip_current_band_short, g_split_pending, g_discard_aux_band;
static BOOL g_discard_aux_band_has_ink, g_discard_leading_aux_band;
static BOOL g_page_had_noformfeed, g_text_margins_cleared;
static ULONG g_accum_height, g_accum_width, g_aux_height, g_page_target_height;
static ULONG g_leading_aux_height, g_strip_nominal_band_height, g_split_at_row;
static ULONG g_text_vertical_units, g_text_vertical_advances;
static ULONG g_text_top_margin_line, g_text_margin_vmi, g_text_form_length_lines;
static BOOL g_text_capture_failed, g_text_last_was_cr, g_text_seen;
static char captured[256];
static unsigned captured_size;
static int finalized, fail_finalize, duplex, queued, submitted;
static ULONG finalized_rows, forwarded_width, mock_special;

static void mp_log_text(const char *text) { (void)text; }
static void mp_log_3(const char *s, LONG a, LONG b, LONG c)
{ (void)s; (void)a; (void)b; (void)c; }
static BOOL mp_duplex_requested(void) { return duplex; }
static BOOL mp_page_finalize(void)
{
    assert(g_page_pending);
    ++finalized;
    finalized_rows = g_accum_height;
    g_page_pending = FALSE;
    if (fail_finalize) return FALSE;
    if (duplex) ++queued;
    else ++submitted;
    return TRUE;
}
static BOOL mp_text_append(UBYTE c)
{
    assert(captured_size < sizeof(captured));
    captured[captured_size++] = (char)c;
    return TRUE;
}

/* This collaborator records the dimensions the real compatibility wrapper
 * forwards. It deliberately does NOT emulate the core's page heuristics. */
LONG PRT_STDARGS Render(LONG ct, LONG x, LONG y, LONG status, ...)
{
    (void)ct;
    if (status == 5) mock_special = (ULONG)x;
    if (status == 0) {
        forwarded_width = (ULONG)x;
        if (x > 8) {
            if (!g_page_pending) g_accum_height = 0;
            g_page_pending = TRUE;
            g_accum_width = (ULONG)x;
            g_accum_height += (ULONG)y;
        } else if (g_page_pending) g_aux_height += (ULONG)y;
        else g_leading_aux_height += (ULONG)y;
    }
    if (status == 4 && !(mock_special & SPECIAL_NOFORMFEED) && g_page_pending)
        return mp_page_finalize() ? PDERR_NOERR : PDERR_CANCEL;
    return PDERR_NOERR;
}

/* REAL_CALLBACKS */

static void reset_test(void)
{
    fail_finalize = duplex = finalized = queued = submitted = 0;
    g_page_pending = g_graphics_boundary_failed = g_duplex_job_failed = FALSE;
    mp_render_compat_reset();
    MintPRINTGraphicsFormFeed();
    g_text_capture_failed = g_text_last_was_cr = g_text_seen = FALSE;
    g_text_top_margin_line = g_text_margin_vmi = g_text_form_length_lines = 0;
    g_text_margins_cleared = FALSE;
    captured_size = 0;
    finalized_rows = forwarded_width = mock_special = 0;
    strcpy(g_render_compat_media, "iso_a4_210x297mm");
    g_render_compat_resolution = 300;
}

static void band(ULONG width, ULONG height)
{
    assert(MintPRINTCompatRender(0, SPECIAL_NOFORMFEED, 0, 5) == PDERR_NOERR);
    assert(MintPRINTCompatRender(0, (LONG)width, (LONG)height, 0) == PDERR_NOERR);
    assert(MintPRINTCompatRender(0, SPECIAL_NOFORMFEED, 0, 4) == PDERR_NOERR);
}

static void command(UWORD cmd)
{
    BYTE line = 0, spacing = 36, crlf = 0;
    char params[2] = { 0, 0 };
    assert(TextDoSpecial(&cmd, NULL, &line, &spacing, &crlf, params) == 0);
}

static void assert_page_reset(void)
{
    assert(!g_page_pending);
    assert(!g_leading_aux_height && !g_aux_height && !g_accum_height);
    assert(!g_accum_width && !g_page_target_height);
    assert(!g_split_pending && !g_split_at_row);
    assert(!g_strip_nominal_band_height && !g_strip_current_band_short);
    assert(!g_discard_aux_band && !g_discard_aux_band_has_ink);
    assert(!g_discard_leading_aux_band && !g_page_had_noformfeed);
    assert(!g_text_vertical_units && !g_text_vertical_advances);
    assert(!g_render_compat_page_active && !g_render_compat_leading_rows);
    assert(!g_render_compat_nominal_rows && !g_render_compat_raster_rows);
    assert(!g_render_compat_aux_rows && !g_render_compat_target_rows);
    assert(!g_render_compat_current_tiny && !g_render_compat_current_rows);
    assert(!g_render_compat_real_bands && !g_render_compat_special);
    assert(!g_render_compat_hold_tiny && !g_render_compat_hold_rows);
    assert(!g_render_compat_swallow_band);
}

int main(void)
{
    int mode;
    /* Short equal-width pages: no media/terminal-strip heuristic can help.
     * A real FF must reach the finalizer once, before the next page starts. */
    for (mode = 0; mode < 2; ++mode) {
        reset_test();
        duplex = mode;
        band(2478, 100);
        band(2478, 100);
        assert(finalized == 0 && g_accum_height == 200);
        assert(ConvFunc(NULL, '\f', 0) == 0);
        assert(finalized == 1 && finalized_rows == 200);
        assert_page_reset();
        assert(!g_text_seen);
        band(2478, 100);
        command(aRIS);
        assert(finalized == 2 && finalized_rows == 100);
        assert_page_reset();
        assert(queued == (mode ? 2 : 0));
        assert(submitted == (mode ? 0 : 2));
        command(aRIS);
        ConvFunc(NULL, '\f', 0);
        assert(finalized == 2); /* no blank page or duplicate submission */
    }

    /* FinalWriter width memory survives FF, but held tails and leading
     * controls must never leak into the following page. */
    reset_test();
    band(2176, 128);
    band(623, 128);
    assert(forwarded_width == 2176 && g_render_compat_variable_job);
    g_render_compat_hold_tiny = TRUE;
    g_render_compat_hold_rows = 128;
    g_render_compat_leading_rows = 256;
    g_leading_aux_height = 256;
    g_split_pending = g_discard_aux_band = TRUE;
    g_split_at_row = 40;
    g_text_top_margin_line = 4;
    g_text_margin_vmi = 36;
    g_text_form_length_lines = 70;
    g_text_vertical_units = 108;
    g_text_vertical_advances = 3;
    ConvFunc(NULL, '\f', 0);
    assert_page_reset();
    assert(g_render_compat_canvas_width == 2176);
    assert(g_render_compat_variable_job);
    assert(g_text_top_margin_line == 4 && g_text_margin_vmi == 36);
    assert(g_text_form_length_lines == 70); /* retain WW margin settings */
    band(993, 128);
    assert(forwarded_width == 2176 && g_accum_height == 128);
    command(aRIS);
    assert(finalized == 2);

    /* Tiny controls alone are not a physical page. */
    reset_test();
    band(1, 128);
    assert(g_leading_aux_height == 128);
    ConvFunc(NULL, '\f', 0);
    assert_page_reset();
    assert(finalized == 0);

    /* A final dump that cleared NOFORMFEED already finished its page. */
    reset_test();
    band(2478, 100);
    MintPRINTCompatRender(0, 0, 0, 5);
    MintPRINTCompatRender(0, 2478, 100, 0);
    MintPRINTCompatRender(0, 0, 0, 4);
    assert(finalized == 1);
    ConvFunc(NULL, '\f', 0);
    command(aRIS);
    assert(finalized == 1);

    /* Plain text and unrelated commands keep their existing semantics. */
    reset_test();
    ConvFunc(NULL, 'A', 0);
    ConvFunc(NULL, '\r', 0);
    ConvFunc(NULL, '\n', 0);
    ConvFunc(NULL, '\f', 0);
    ConvFunc(NULL, 'B', 0);
    assert(g_text_seen && captured_size == 4);
    assert(memcmp(captured, "A\n\fB", 4) == 0 && finalized == 0);
    band(2478, 100);
    command(aRIN); /* initialize is not an eject */
    command(aIND);
    command(aNEL);
    assert(g_page_pending && finalized == 0);
    assert(ConvFunc(NULL, 0x1b, 0) == -1);
    assert(ConvFunc(NULL, 0x9b, 0) == -1);
    assert(ConvFunc(NULL, 0xff, 0) == -1);

    /* Text OOM cannot block a graphics FF. Failure remains sticky across
     * redundant commands, and invalidates a duplex stream. */
    reset_test();
    band(2478, 100);
    g_text_capture_failed = TRUE;
    fail_finalize = duplex = 1;
    ConvFunc(NULL, '\f', 0);
    assert(finalized == 1 && captured_size == 0);
    assert(g_graphics_boundary_failed && g_duplex_job_failed);
    assert_page_reset();
    command(aRIS);
    ConvFunc(NULL, '\f', 0);
    assert(finalized == 1 && g_graphics_boundary_failed);
    assert(!queued && !submitted);

    puts("Graphics FF/reset callback and strip-wrapper tests passed");
    return 0;
}
