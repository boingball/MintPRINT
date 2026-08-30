/*
 * MintPRINT runtime configuration.
 *
 * Live settings are read from ENV:MintPRINT/Unit0.  If that file does not
 * exist, ENVARC:MintPRINT/Unit0 is used.  Missing or invalid values fall back
 * to the known-good development defaults.
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include "config.h"
#include "ipp_client.h"
#include "postscript_writer.h"

extern struct DosLibrary *DOSBase;

static char g_config_line[192];

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

    /* Keep the PostScript writer in step with the configuration actually
     * used by this job. Empty/default scaling intentionally maps to its
     * historical auto-fit placement until a saved SCALING= overrides it. */
    mp_postscript_set_scaling(cfg->scaling);
    mp_postscript_set_margins(cfg->margin_left_100mm,
                              cfg->margin_right_100mm,
                              cfg->margin_top_100mm,
                              cfg->margin_bottom_100mm);

    if (!DOSBase) return MP_CONFIG_SOURCE_DEFAULTS;

    fh = Open((CONST_STRPTR)"ENV:MintPRINT/Unit0", MODE_OLDFILE);
    if (fh) {
        source = MP_CONFIG_SOURCE_ENV;
    } else {
        fh = Open((CONST_STRPTR)"ENVARC:MintPRINT/Unit0", MODE_OLDFILE);
        if (fh) source = MP_CONFIG_SOURCE_ENVARC;
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
            cfg->resolution = (ok && n == 600UL) ? 600 : 300;
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

    /* Config loading already runs inside MintPRINT's dedicated spool
     * Process (see spool.c), where bsdsocket calls are safe. For explicit
     * PostScript Fit, resolve the real printer imageable area here so an old
     * saved Unit0 gains the fix immediately without requiring the Settings
     * GUI to be opened or re-saved. Fill deliberately keeps rev28 full-sheet
     * cover/crop geometry. Manual MARGIN_* overrides remain available for
     * diagnostics; otherwise the IPP query is cached per endpoint by
     * ipp_client.c. Missing/conflicting IPP values resolve to zero, preserving
     * rev28's full-page target rather than guessing. */
    if (mp_cfg_starts(cfg->engine, "postscript") &&
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
    return source;
}
