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
   ÒVÇ6R°¢f‚Ò÷Vâ‚„4ôå5Eõ5E%E"’$Tåd$3¤Ö–çE$”åBõVæ—C"ÂÔôDUôôÄDd”ÄR“°¢–b†f‚’6÷W&6RÒÕô4ôäd”uõ4õU$4UôTåd$3°¢Ð ¢–b‚f‚’&WGW&â6÷W&6S° ¢v†–ÆR„dvWG2†f‚Â…5E%E"–uö6öæf–uöÆ–æRÂ6—¦Vöb†uö6öæf–uöÆ–æR’’’°¢6öç7B6†"§fÇVS°¢$ôôÂö³°¢TÄôärã° ¢×ö6fu÷G&–ÕöVöÂ†uö6öæf–uöÆ–æR“°¢–b‚uö6öæf–uöÆ–æU³ÒÇÂuö6öæf–uöÆ–æU³ÒÓÒr2rÇÀ¢uö6öæf–uöÆ–æU³ÒÓÒs²r’6öçF–çVS° ¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ$„õ5CÒ"’’°¢fÇVRÒuö6öæf–uöÆ–æR²S°¢–b‡fÇVU³Ò’×ö6fuö6÷’†6frÓæ†÷7BÂ6—¦Vöb†6frÓæ†÷7B’ÂfÇVR“°¢6öçF–çVS°¢Ð ¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ%õ%CÒ"’’°¢fÇVRÒuö6öæf–uöÆ–æR²S°¢âÒ×ö6fu÷'6U÷VÆöær‡fÇVRÂfö²“°¢–b†ö²bbâãÒTÂbbâÃÒcSS3UTÂ’6frÓç÷'BÒ…Utõ$B–ã°¢6öçF–çVS°¢Ð ¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ%DƒÒ"’’°¢fÇVRÒuö6öæf–uöÆ–æR²S°¢–b‡fÇVU³ÒÓÒròr’×ö6fuö6÷’†6frÓçF‚Â6—¦Vöb†6frÓçF‚’ÂfÇVR“°¢6öçF–çVS°¢Ð ¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ$DT%TsÒ"’’°¢fÇVRÒuö6öæf–uöÆ–æR²c°¢6frÓæFV'VrÒ‡fÇVU³ÒÓÒsr’òdÅ4R¢E%TS°¢6öçF–çVS°¢Ð ¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ$´TU¤ô#Ò"’’°¢ò¢ÆVv7’Æ–3¢öÆB6öæf–w2W6VB&WFVçF–öâ2F†R6öÆRFV'Vp¢¢6öçG&öÂâ&W6W'fRF†V—"–çFVçBv†VâWw&F–ærâ¢ð¢fÇVRÒuö6öæf–uöÆ–æR²ƒ°¢6frÓæFV'VrÒ‡fÇVU³ÒÓÒsr’òdÅ4R¢E%TS°¢6öçF–çVS°¢Ð ¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ%$U4ôÅUD”ôãÒ"’’°¢fÇVRÒuö6öæf–uöÆ–æR²°¢âÒ×ö6fu÷'6U÷VÆöær‡fÇVRÂfö²“°¢6frÓç&W6öÇWF–öâÒ†ö²bbâÓÒcTÂ’òc¢3°¢6öçF–çVS°¢Ð ¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ$Tät”äSÒ"’’°¢fÇVRÒuö6öæf–uöÆ–æR²s°¢–b†×ö6fu÷7F'G2‡fÇVRÂ'vr×&7FW""’bb×ö6fuöÆVâ‡fÇVR’ÓÒ’°¢×ö6fuö6÷’†6frÓæVæv–æRÂ6—¦Vöb†6frÓæVæv–æR’Â'vr×&7FW""“°¢ÒVÇ6R–b†×ö6fu÷7F'G2‡fÇVRÂ'Fb"’bb×ö6fuöÆVâ‡fÇVR’ÓÒ2’°¢×ö6fuö6÷’†6frÓæVæv–æRÂ6—¦Vöb†6frÓæVæv–æR’Â'Fb"“°¢ÒVÇ6R–b†×ö6fu÷7F'G2‡fÇVRÂ'÷7G67&—B"’bb×ö6fuöÆVâ‡fÇVR’ÓÒ’°¢×ö6fuö6÷’†6frÓæVæv–æRÂ6—¦Vöb†6frÓæVæv–æR’Â'÷7G67&—B"“°¢ÒVÇ6R–b†×ö6fu÷7F'G2‡fÇVRÂ'W&b"’bb×ö6fuöÆVâ‡fÇVR’ÓÒ2’°¢×ö6fuö6÷’†6frÓæVæv–æRÂ6—¦Vöb†6frÓæVæv–æR’Â'W&b"“°¢ÒVÇ6R°¢×ö6fuö6÷’†6frÓæVæv–æRÂ6—¦Vöb†6frÓæVæv–æR’Â&§Vr"“°¢Ð¢6öçF–çVS°¢Ð ¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ$ÔTD”Ò"’’°¢fÇVRÒuö6öæf–uöÆ–æR²c°¢–b‡fÇVU³Ò’×ö6fuö6÷’†6frÓæÖVF–Â6—¦Vöb†6frÓæÖVF–’ÂfÇVR“°¢6öçF–çVS°¢Ð¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ%4õU$4SÒ"’’°¢fÇVRÒuö6öæf–uöÆ–æR²s°¢–b‡fÇVU³Ò’×ö6fuö6÷’†6frÓç6÷W&6RÂ6—¦Vöb†6frÓç6÷W&6R’ÂfÇVR“°¢6öçF–çVS°¢Ð¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ$4ôÄõ#Ò"’’°¢fÇVRÒuö6öæf–uöÆ–æR²c°¢–b‡fÇVU³Ò’×ö6fuö6÷’†6frÓæ6öÆ÷"Â6—¦Vöb†6frÓæ6öÆ÷"’ÂfÇVR“°¢6öçF–çVS°¢Ð¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ%TÄ•E“Ò"’’°¢fÇVRÒuö6öæf–uöÆ–æR²ƒ°¢–b‡fÇVU³Ò’×ö6fuö6÷’†6frÓçVÆ—G’Â6—¦Vöb†6frÓçVÆ—G’’ÂfÇVR“°¢6öçF–çVS°¢Ð¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ%44Ä”äsÒ"’’°¢fÇVRÒuö6öæf–uöÆ–æR²ƒ°¢–b‡fÇVU³Ò’×ö6fuö6÷’†6frÓç66Æ–ærÂ6—¦Vöb†6frÓç66Æ–ær’ÂfÇVR“°¢6öçF–çVS°¢Ð¢–b†×ö6fu÷7F'G2†uö6öæf–uöÆ–æRÂ%4”DU3Ò"’’°¢fÇVRÒuö6öæf–uöÆ–æR²c°¢–b‚†×ö6fu÷7F'G2‡fÇVRÂ&öæR×6–FVB"’b`¢×ö6fuöÆVâ‡fÇVR’ÓÒ’’ÇÀ¢†×ö6fu÷7F'G2‡fÇVRÂ'Gvò×6–FVBÖÆöærÖVFvR"’b`¢×ö6fuöÆVâ‡fÇVR’ÓÒ’’ÇÀ¢†×ö6fu÷7F'G2‡fÇVRÂ'Gvò×6–FVB×6†÷'BÖPdge") &&
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
