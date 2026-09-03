/*
 * MintPRINT runtime configuration.
 *
 * Live settings are read from ENV:MintPRINT/Unit0.  If that file does not
 * exist, ENVARC:MintPRINT/Unit0 is used.  Missing or invalid values fall back
 * to the known-good development defaults.
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <devices/printer.h>
#include <devices/prtbase.h>
#include <devices/prtgfx.h>
#include <proto/dos.h>

#include "config.h"
#include "ipp_client.h"
#include "media_size.h"
#include "postscript_writer.h"

extern struct DosLibrary *DOSBase;
extern LONG PRT_STDARGS Render(LONG ct, LONG x, LONG y, LONG status, ...);

static char g_config_line[192];

/*
 * Compatibility state for variable-width SPECIAL_NOFORMFEED producers.
 *
 * Wordworth's proven path is deliberately untouched: it repeats one raster
 * width, so every callback reaches Render() with exactly the same arguments
 * it did before this shim existed. Final Writer 97 behaves differently: the
 * real trace establishes a 2176-pixel page but trims successive 128-row bands
 * to widths such as 1811, 1853, 623 and 993. driver_core.c quite reasonably
 * used width equality as its continuation test, which turned each trimmed
 * band into a new page.
 *
 * The PED Render hook now points at MintPRINTCompatRender() below. It keeps a
 * stable horizontal canvas only after a variable-width stream is actually
 * observed, and forwards that stable width to the existing renderer. The
 * renderer still owns all row conversion, Wordworth short-band boundaries,
 * top-margin restoration, encoder handling and page finalisation.
 */
static ULONG g_render_compat_special = 0;
static ULONG g_render_compat_canvas_width = 0;
static ULONG g_render_compat_nominal_rows = 0;
static ULONG g_render_compat_raster_rows = 0;
static ULONG g_render_compat_aux_rows = 0;
static ULONG g_render_compat_leading_rows = 0;
static ULONG g_render_compat_target_rows = 0;
static ULONG g_render_compat_real_bands = 0;
static ULONG g_render_compat_resolution = 300;
static BOOL g_render_compat_page_active = FALSE;
static BOOL g_render_compat_variable_page = FALSE;
static BOOL g_render_compat_variable_job = FALSE;
static BOOL g_render_compat_current_tiny = FALSE;
static ULONG g_render_compat_current_rows = 0;
static BOOL g_render_compat_swallow_band = FALSE;
static BOOL g_render_compat_hold_tiny = FALSE;
static ULONG g_render_compat_hold_rows = 0;
static char g_render_compat_media[MP_CONFIG_OPTION_MAX];

static ULONG mp_cfg_len(const char *s)
{
    ULONG n = 0;
    while (s && s[n]) ++n;
    return n;
}

static BOOL mp_cfg_starts(const char *s, const char *prefix)
{
    ULONG i = 0;
    if (!s || !prefix) return FALSE;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return FALSE;
        ++i;
    }
    return TRUE;
}

static BOOL mp_cfg_copy(char *dst, ULONG cap, const char *src)
{
    ULONG n;
    if (!dst || !cap || !src) return FALSE;
    n = mp_cfg_len(src);
    if (n == 0 || n >= cap) return FALSE;
    while (n) {
        dst[n - 1] = src[n - 1];
        --n;
    }
    dst[mp_cfg_len(src)] = 0;
    return TRUE;
}

static void mp_render_compat_finish_page(void)
{
    g_render_compat_page_active = FALSE;
    g_render_compat_variable_page = FALSE;
    g_render_compat_nominal_rows = 0;
    g_render_compat_raster_rows = 0;
    g_render_compat_aux_rows = 0;
    g_render_compat_target_rows = 0;
    g_render_compat_real_bands = 0;
    g_render_compat_current_tiny = FALSE;
    g_render_compat_current_rows = 0;
    g_render_compat_swallow_band = FALSE;
    g_render_compat_hold_tiny = FALSE;
    g_render_compat_hold_rows = 0;

    /* Fixed-width streams do not need cross-page width memory at all.
     * Keeping it only after real width variation was observed is what makes
     * this shim invisible to Wordworth and other established producers. */
    if (!g_render_compat_variable_job)
        g_render_compat_canvas_width = 0;
}

static void mp_render_compat_reset(void)
{
    g_render_compat_special = 0;
    g_render_compat_canvas_width = 0;
    g_render_compat_leading_rows = 0;
    g_render_compat_variable_job = FALSE;
    mp_render_compat_finish_page();
}

void MintPRINTCompatEndPage(void)
{
    mp_render_compat_finish_page();
    g_render_compat_leading_rows = 0;
    g_render_compat_special = 0;
}

static void mp_render_compat_capture_config(const struct MPConfig *cfg)
{
    g_render_compat_media[0] = 0;
    g_render_compat_resolution = 300;
    if (!cfg) return;

    if (cfg->media[0])
        mp_cfg_copy(g_render_compat_media,
                    sizeof(g_render_compat_media), cfg->media);
    if (cfg->resolution)
        g_render_compat_resolution = cfg->resolution;
}

static void mp_render_compat_add_rows(ULONG *value, ULONG rows)
{
    if (!value) return;
    if (*value > 0xffffffffUL - rows)
        *value = 0xffffffffUL;
    else
        *value += rows;
}

static void mp_cfg_trim_eol(char *s)
{
    ULONG n = mp_cfg_len(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                 s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
}

static ULONG mp_cfg_parse_ulong(const char *s, BOOL *ok)
{
    ULONG v = 0;
    ULONG i = 0;
    BOOL any = FALSE;

    if (ok) *ok = FALSE;
    if (!s) return 0;

    while (s[i] >= '0' && s[i] <= '9') {
        ULONG digit = (ULONG)(s[i] - '0');
        if (v > 429496729UL) return 0;
        v = v * 10UL + digit;
        any = TRUE;
        ++i;
    }

    if (!any || s[i] != 0) return 0;
    if (ok) *ok = TRUE;
    return v;
}

void mp_config_defaults(struct MPConfig *cfg)
{
    if (!cfg) return;

    /* Empty, not a real address: mp_ipp_query_imageable_margins() and
     * mp_ipp_print_document() (ipp_client.c) both already refuse to run
     * when cfg->host[0] is 0 - the guard that matters is having nothing
     * here, not what the "nothing configured yet" placeholder looks like.
     * A real IP here meant a Unit0 that was never saved (no ENV:/ENVARC:
     * MintPRINT/Unit0 at all) still had this driver try to print to
     * someone else's LAN device instead of failing cleanly. */
    cfg->host[0] = 0;
    cfg->port = 80;
    mp_cfg_copy(cfg->path, sizeof(cfg->path), "/ipp/print");
    cfg->debug = FALSE;
    cfg->resolution = 300;
    mp_cfg_copy(cfg->engine, sizeof(cfg->engine), "jpeg");
    cfg->media[0] = 0;
    cfg->source[0] = 0;
    cfg->color[0] = 0;
    cfg->quality[0] = 0;
    cfg->scaling[0] = 0;
    /* Preserve the historical no-attribute behaviour for an old Unit0
     * file that predates SIDES=.  Settings writes one-sided explicitly
     * when the user next saves, but an upgrade alone must not add a new
     * job-template attribute to printers that were already working. */
    cfg->sides[0] = 0;
    mp_cfg_copy(cfg->pwg_sheet_back, sizeof(cfg->pwg_sheet_back), "normal");
    mp_cfg_copy(cfg->spool, sizeof(cfg->spool), "RAM");
    cfg->spool_keep = FALSE;
    cfg->capture_only = FALSE;
    cfg->capture_path[0] = 0;
    cfg->margin_left_100mm = 0;
    cfg->margin_right_100mm = 0;
    cfg->margin_top_100mm = 0;
    cfg->margin_bottom_100mm = 0;
}

LONG mp_config_load(struct MPConfig *cfg)
{
    BPTR fh = 0;
    LONG source = MP_CONFIG_SOURCE_DEFAULTS;

    if (!cfg) return MP_CONFIG_SOURCE_DEFAULTS;
    mp_config_defaults(cfg);

    /* Config loading occurs synchronously before a new print starts. Reset
     * the Render compatibility state here rather than adding another public
     * Open hook solely for the shim. Init can load once too; DriverOpen's
     * subsequent load simply resets it again before the first raster band. */
    mp_render_compat_reset();
    mp_render_compat_capture_config(cfg);

    /* Keep the PostScript writer in step with the configuration actually
     * used by this job. Empty/default scaling intentionally maps to its
     * historical auto-fit placement until a saved SCALING= overrides it. */
    mp_postscript_set_scaling(cfg->scaling);
    mp_postscript_set_margins(cfg->margin_left_100mm,
                              cfg->margin_right_100mm,
                              cfg->margin_top_100mm,
                              cfg->margin_bottom_100mm);

    if (!DOSBase) return MP_CONFIG_SOURCE_DEFAULTS;

    /* MintPrint Settings' regression suite writes a complete, one-job
     * override here. It deliberately wins over Unit0 so the suite never
     * mutates the user's saved printer profile. DriverOpen removes it
     * after loading; Init may read it first when the driver is cold, so
     * config.c itself must not consume/delete it. */
    fh = Open((CONST_STRPTR)"T:MintPRINT-testsuite.cfg", MODE_OLDFILE);
    if (fh) {
        source = MP_CONFIG_SOURCE_TEST;
    } else {
        fh = Open((CONST_STRPTR)"ENV:MintPRINT/Unit0", MODE_OLDFILE);
        if (fh) {
            source = MP_CONFIG_SOURCE_ENV;
        } else {
            fh = Open((CONST_STRPTR)"ENVARC:MintPRINT/Unit0", MODE_OLDFILE);
            if (fh) source = MP_CONFIG_SOURCE_ENVARC;
        }
    }

    if (!fh) return source;

    while (FGets(fh, (STRPTR)g_config_line, sizeof(g_config_line))) {
        const char *value;
        BOOL ok;
        ULONG n;

        mp_cfg_trim_eol(g_config_line);
        if (!g_config_line[0] || g_config_line[0] == '#' ||
            g_config_line[0] == ';') continue;

        if (mp_cfg_starts(g_config_line, "HOST=")) {
            value = g_config_line + 5;
            if (value[0]) mp_cfg_copy(cfg->host, sizeof(cfg->host), value);
            continue;
        }

        if (mp_cfg_starts(g_config_line, "PORT=")) {
            value = g_config_line + 5;
            n = mp_cfg_parse_ulong(value, &ok);
            if (ok && n >= 1UL && n <= 65535UL) cfg->port = (UWORD)n;
            continue;
        }

        if (mp_cfg_starts(g_config_line, "PATH=")) {
            value = g_config_line + 5;
            if (value[0] == '/') mp_cfg_copy(cfg->path, sizeof(cfg->path), value);
            continue;
        }

        if (mp_cfg_starts(g_config_line, "DEBUG=")) {
            value = g_config_line + 6;
            cfg->debug = (value[0] == '0') ? FALSE : TRUE;
            continue;
        }

        if (mp_cfg_starts(g_config_line, "KEEPJOB=")) {
            /* Legacy alias: old configs used retention as the sole debug
             * control. Preserve their intent when upgrading. */
            value = g_config_line + 8;
            cfg->debug = (value[0] == '0') ? FALSE : TRUE;
            continue;
        }

        if (mp_cfg_starts(g_config_line, "RESOLUTION=")) {
            value = g_config_line + 11;
            n = mp_cfg_parse_ulong(value, &ok);
            if (ok && (n == 300UL || n == 360UL ||
                       n == 600UL || n == 720UL))
                cfg->resolution = (UWORD)n;
            else
                cfg->resolution = 300;
            continue;
        }

        if (mp_cfg_starts(g_config_line, "ENGINE=")) {
            value = g_config_line + 7;
            if (mp_cfg_starts(value, "pwg-raster") && mp_cfg_len(value) == 10) {
                mp_cfg_copy(cfg->engine, sizeof(cfg->engine), "pwg-raster");
            } else if (mp_cfg_starts(value, "pdf") && mp_cfg_len(value) == 3) {
                mp_cfg_copy(cfg->engine, sizeof(cfg->engine), "pdf");
            } else if (mp_cfg_starts(value, "postscript") && mp_cfg_len(value) == 10) {
                mp_cfg_copy(cfg->engine, sizeof(cfg->engine), "postscript");
            } else if (mp_cfg_starts(value, "urf") && mp_cfg_len(value) == 3) {
                mp_cfg_copy(cfg->engine, sizeof(cfg->engine), "urf");
            } else {
                mp_cfg_copy(cfg->engine, sizeof(cfg->engine), "jpeg");
            }
            continue;
        }

        if (mp_cfg_starts(g_config_line, "MEDIA=")) {
            value = g_config_line + 6;
            if (value[0]) mp_cfg_copy(cfg->media, sizeof(cfg->media), value);
            continue;
        }
        if (mp_cfg_starts(g_config_line, "SOURCE=")) {
            value = g_config_line + 7;
            if (value[0]) mp_cfg_copy(cfg->source, sizeof(cfg->source), value);
            continue;
        }
        if (mp_cfg_starts(g_config_line, "COLOR=")) {
            value = g_config_line + 6;
            if (value[0]) mp_cfg_copy(cfg->color, sizeof(cfg->color), value);
            continue;
        }
        if (mp_cfg_starts(g_config_line, "QUALITY=")) {
            value = g_config_line + 8;
            if (value[0]) mp_cfg_copy(cfg->quality, sizeof(cfg->quality), value);
            continue;
        }
        if (mp_cfg_starts(g_config_line, "SCALING=")) {
            value = g_config_line + 8;
            if (value[0]) mp_cfg_copy(cfg->scaling, sizeof(cfg->scaling), value);
            continue;
        }
        if (mp_cfg_starts(g_config_line, "SIDES=")) {
            value = g_config_line + 6;
            if ((mp_cfg_starts(value, "one-sided") &&
                 mp_cfg_len(value) == 9) ||
                (mp_cfg_starts(value, "two-sided-long-edge") &&
                 mp_cfg_len(value) == 19) ||
                (mp_cfg_starts(value, "two-sided-short-edge") &&
                 mp_cfg_len(value) == 20)) {
                mp_cfg_copy(cfg->sides, sizeof(cfg->sides), value);
            }
            continue;
        }
        if (mp_cfg_starts(g_config_line, "SPOOL=")) {
            value = g_config_line + 6;
            if (value[0]) mp_cfg_copy(cfg->spool, sizeof(cfg->spool), value);
            continue;
        }
        if (mp_cfg_starts(g_config_line, "SPOOL_KEEP=")) {
            value = g_config_line + 11;
            cfg->spool_keep = (value[0] == '0') ? FALSE : TRUE;
            continue;
        }
        if (mp_cfg_starts(g_config_line, "CAPTURE_ONLY=")) {
            value = g_config_line + 13;
            cfg->capture_only = (value[0] == '0') ? FALSE : TRUE;
            continue;
        }
        if (mp_cfg_starts(g_config_line, "CAPTURE_PATH=")) {
            value = g_config_line + 13;
            /* Regression captures are intentionally confined to T:. */
            if (value[0] == 'T' && value[1] == ':')
                mp_cfg_copy(cfg->capture_path, sizeof(cfg->capture_path), value);
            continue;
        }
        if (mp_cfg_starts(g_config_line, "PWG_SHEET_BACK=")) {
            value = g_config_line + 15;
            if ((mp_cfg_starts(value, "normal") && mp_cfg_len(value) == 6) ||
                (mp_cfg_starts(value, "rotated") && mp_cfg_len(value) == 7) ||
                (mp_cfg_starts(value, "flipped") && mp_cfg_len(value) == 7) ||
                (mp_cfg_starts(value, "manual-tumble") &&
                 mp_cfg_len(value) == 13)) {
                mp_cfg_copy(cfg->pwg_sheet_back,
                            sizeof(cfg->pwg_sheet_back), value);
            }
            continue;
        }
        if (mp_cfg_starts(g_config_line, "MARGIN_LEFT=")) {
            n = mp_cfg_parse_ulong(g_config_line + 12, &ok);
            if (ok) cfg->margin_left_100mm = n;
            continue;
        }
        if (mp_cfg_starts(g_config_line, "MARGIN_RIGHT=")) {
            n = mp_cfg_parse_ulong(g_config_line + 13, &ok);
            if (ok) cfg->margin_right_100mm = n;
            continue;
        }
        if (mp_cfg_starts(g_config_line, "MARGIN_TOP=")) {
            n = mp_cfg_parse_ulong(g_config_line + 11, &ok);
            if (ok) cfg->margin_top_100mm = n;
            continue;
        }
        if (mp_cfg_starts(g_config_line, "MARGIN_BOTTOM=")) {
            n = mp_cfg_parse_ulong(g_config_line + 14, &ok);
            if (ok) cfg->margin_bottom_100mm = n;
            continue;
        }
    }

    Close(fh);

    /* CAPTURE_* is never a persisted Unit0 feature. Even a hand-edited
     * ENV:/ENVARC: file containing those keys cannot disable printing.
     * Only the dedicated T: test-suite override may enable it. */
    if (source != MP_CONFIG_SOURCE_TEST) {
        cfg->capture_only = FALSE;
        cfg->capture_path[0] = 0;
    } else if (cfg->capture_only) {
        cfg->debug = TRUE;
        mp_cfg_copy(cfg->spool, sizeof(cfg->spool), "RAM");
        cfg->spool_keep = FALSE;
    }

    /* Config loading already runs inside MintPRINT's dedicated spool
     * Process (see spool.c), where bsdsocket calls are safe. For explicit
     * PostScript Fit, resolve the real printer imageable area here so an old
     * saved Unit0 gains the fix immediately without requiring the Settings
     * GUI to be opened or re-saved. Fill deliberately keeps rev28 full-sheet
     * cover/crop geometry. Manual MARGIN_* overrides remain available for
     * diagnostics; otherwise the IPP query is cached per endpoint by
     * ipp_client.c. Missing/conflicting IPP values resolve to zero, preserving
     * rev28's full-page target rather than guessing. */
    if (!cfg->capture_only &&
        mp_cfg_starts(cfg->engine, "postscript") &&
        mp_cfg_len(cfg->engine) == 10 &&
        mp_cfg_starts(cfg->scaling, "fit") && mp_cfg_len(cfg->scaling) == 3 &&
        cfg->margin_left_100mm == 0 && cfg->margin_right_100mm == 0 &&
        cfg->margin_top_100mm == 0 && cfg->margin_bottom_100mm == 0) {
        ULONG left = 0, right = 0, top = 0, bottom = 0;
        if (mp_ipp_query_imageable_margins(cfg, &left, &right,
                                           &top, &bottom) == 0) {
            cfg->margin_left_100mm = left;
            cfg->margin_right_100mm = right;
            cfg->margin_top_100mm = top;
            cfg->margin_bottom_100mm = bottom;
        }
    }

    mp_postscript_set_scaling(cfg->scaling);
    mp_postscript_set_margins(cfg->margin_left_100mm,
                              cfg->margin_right_100mm,
                              cfg->margin_top_100mm,
                              cfg->margin_bottom_100mm);
    mp_render_compat_capture_config(cfg);
    return source;
}

/*
 * Render compatibility front-end used by both printer-driver ABIs.
 *
 * The V44 PED points directly here and this calls driver_core.c's Render().
 * The classic PED also points here; in that build the public Render symbol is
 * classic_render_shim.c, which in turn calls MintPRINT_RenderCore with the
 * classic-gun conversion enabled. One implementation therefore covers OS2.x,
 * OS3.1 and V44+ without duplicating any raster logic.
 */
LONG PRT_STDARGS MintPRINTCompatRender(LONG ct, LONG x, LONG y,
                                       LONG status, ...)
{
    LONG rc;
    ULONG raw_width = x > 0 ? (ULONG)x : 0UL;
    ULONG raw_rows = y > 0 ? (ULONG)y : 0UL;
    BOOL noformfeed;

    if (status == 5) {
        g_render_compat_special = (ULONG)x;
        return Render(ct, x, y, status);
    }

    noformfeed = (g_render_compat_special & SPECIAL_NOFORMFEED) ?
                 TRUE : FALSE;

    if (status == 0) {
        ULONG send_width = raw_width;

        g_render_compat_current_tiny = FALSE;
        g_render_compat_current_rows = raw_rows;
        g_render_compat_swallow_band = FALSE;

        /* A candidate full-height tiny tail was held back because accepting
         * it immediately would make the existing media-height fallback close
         * the page before Final Writer's following short 1x100 terminator.
         * If the next band is not that shorter tiny terminator, replay the
         * held blank control band's HEIGHT now through the existing tiny-aux
         * path before processing the new band. Tiny bands never contribute
         * pixels to the page canvas, so no raster data needs buffering. */
        if (g_render_compat_hold_tiny) {
            BOOL this_tiny = noformfeed && raw_width > 0 && raw_width <= 8UL;
            BOOL this_short = this_tiny && g_render_compat_nominal_rows &&
                              raw_rows > 0 &&
                              raw_rows < g_render_compat_nominal_rows;

            if (this_short || !noformfeed) {
                /* Final Writer pattern: 1x128 then 1x100. The full-height
                 * control band is redundant blank extent because the normal
                 * page finalizer pads to the physical target; let the short
                 * band be the strong logical delimiter instead. If the app
                 * clears NOFORMFEED here, also drop the held blank tail and
                 * let the normal final-band path close the page unchanged. */
                g_render_compat_hold_tiny = FALSE;
                g_render_compat_hold_rows = 0;
            } else {
                LONG replay_rc;
                ULONG held = g_render_compat_hold_rows;

                g_render_compat_hold_tiny = FALSE;
                g_render_compat_hold_rows = 0;
                replay_rc = Render(0, 1, (LONG)held, 0);
                if (replay_rc != PDERR_NOERR)
                    return replay_rc;
                replay_rc = Render(0, (LONG)g_render_compat_special, 0, 4);
                if (replay_rc != PDERR_NOERR)
                    return replay_rc;

                mp_render_compat_finish_page();
                g_render_compat_leading_rows = 0;
            }
        }

        /* Normal/single-shot output is not part of this compatibility path.
         * Do not touch its dimensions or state. A standard strip producer
         * that finally clears NOFORMFEED also reaches Render unchanged. */
        if (!noformfeed) {
            return Render(ct, x, y, status);
        }

        if (raw_width > 0 && raw_width <= 8UL) {
            g_render_compat_current_tiny = TRUE;

            if (!g_render_compat_page_active) {
                mp_render_compat_add_rows(&g_render_compat_leading_rows,
                                          raw_rows);
                return Render(ct, x, y, status);
            }

            /* Only variable-width streams get this one-band lookahead, and
             * only when THIS full-height tiny band would itself trip the
             * media-height page-complete test. Wordworth never enters the
             * variable-width mode, so its 4px auxiliary/62-row terminator
             * sequence reaches driver_core.c exactly as before. */
            if (g_render_compat_variable_job &&
                !g_render_compat_hold_tiny &&
                g_render_compat_nominal_rows &&
                raw_rows == g_render_compat_nominal_rows &&
                g_render_compat_real_bands >= 2UL &&
                mp_media_page_complete(g_render_compat_raster_rows,
                                       raw_rows,
                                       g_render_compat_target_rows)) {
                g_render_compat_hold_tiny = TRUE;
                g_render_compat_hold_rows = raw_rows;
                g_render_compat_swallow_band = TRUE;
                return PDERR_NOERR;
            }

            mp_render_compat_add_rows(&g_render_compat_aux_rows, raw_rows);
            return Render(ct, x, y, status);
        }

        if (!g_render_compat_page_active) {
            if (!g_render_compat_canvas_width ||
                !g_render_compat_variable_job) {
                g_render_compat_canvas_width = raw_width;
            } else {
                g_render_compat_canvas_width = mp_strip_canvas_width(
                    g_render_compat_canvas_width, raw_width);
            }

            send_width = g_render_compat_canvas_width;
            g_render_compat_page_active = TRUE;
            g_render_compat_variable_page =
                raw_width != send_width ? TRUE : FALSE;
            if (g_render_compat_variable_page)
                g_render_compat_variable_job = TRUE;
            g_render_compat_nominal_rows = raw_rows;
            g_render_compat_raster_rows = g_render_compat_leading_rows;
            mp_render_compat_add_rows(&g_render_compat_raster_rows, raw_rows);
            g_render_compat_leading_rows = 0;
            g_render_compat_aux_rows = 0;
            g_render_compat_real_bands = 1;
            g_render_compat_target_rows = mp_media_target_height(
                g_render_compat_media, send_width,
                g_render_compat_resolution);
        } else if (mp_strip_band_can_continue(
                       g_render_compat_canvas_width, raw_width)) {
            send_width = g_render_compat_canvas_width;
            if (raw_width != send_width) {
                g_render_compat_variable_page = TRUE;
                g_render_compat_variable_job = TRUE;
            }
            ++g_render_compat_real_bands;
            mp_render_compat_add_rows(&g_render_compat_raster_rows, raw_rows);
            if (raw_rows > g_render_compat_nominal_rows)
                g_render_compat_nominal_rows = raw_rows;
        } else {
            /* A substantial width increase is not a trimmed continuation.
             * Preserve driver_core.c's established behaviour: the differing
             * width closes the pending page and starts a new one. Mirror that
             * transition here so subsequent compatibility decisions describe
             * the same page the renderer is actually holding. */
            mp_render_compat_finish_page();
            g_render_compat_canvas_width = raw_width;
            g_render_compat_page_active = TRUE;
            g_render_compat_nominal_rows = raw_rows;
            g_render_compat_raster_rows = raw_rows;
            g_render_compat_aux_rows = 0;
            g_render_compat_real_bands = 1;
            g_render_compat_target_rows = mp_media_target_height(
                g_render_compat_media, raw_width,
                g_render_compat_resolution);
            send_width = raw_width;
        }

        return Render(ct, (LONG)send_width, y, status);
    }

    /* The held candidate tiny band's row and close callbacks are swallowed
     * with its case-0 call. The next status-0 callback decides whether to
     * discard that blank extent (Final Writer's short terminator followed)
     * or replay its height through the normal tiny-aux path. */
    if (g_render_compat_swallow_band) {
        if (status == 4)
            g_render_compat_swallow_band = FALSE;
        return PDERR_NOERR;
    }

    rc = Render(ct, x, y, status);

    if (status == 4 && g_render_compat_page_active) {
        BOOL boundary = FALSE;

        if (!noformfeed) {
            boundary = TRUE;
        } else if (g_render_compat_current_tiny) {
            if (g_render_compat_nominal_rows &&
                g_render_compat_current_rows > 0 &&
                g_render_compat_current_rows <
                    g_render_compat_nominal_rows &&
                mp_short_strip_completes_logical_page(
                    g_render_compat_raster_rows,
                    g_render_compat_aux_rows,
                    g_render_compat_target_rows,
                    g_render_compat_nominal_rows,
                    g_render_compat_current_rows)) {
                boundary = TRUE;
            } else if (mp_media_page_complete(
                           g_render_compat_raster_rows,
                           g_render_compat_aux_rows,
                           g_render_compat_target_rows)) {
                boundary = TRUE;
            }
        } else {
            if (g_render_compat_nominal_rows &&
                g_render_compat_current_rows > 0 &&
                g_render_compat_current_rows <
                    g_render_compat_nominal_rows &&
                mp_short_strip_completes_logical_page(
                    g_render_compat_raster_rows,
                    g_render_compat_aux_rows,
                    g_render_compat_target_rows,
                    g_render_compat_nominal_rows,
                    g_render_compat_current_rows)) {
                boundary = TRUE;
            } else if (mp_media_page_complete(
                           g_render_compat_raster_rows,
                           g_render_compat_aux_rows,
                           g_render_compat_target_rows)) {
                boundary = TRUE;
            }
        }

        if (boundary)
            mp_render_compat_finish_page();
    }

    return rc;
}
