/*
 * MintPRINT printer.device integration working driver path.
 *
 * Converts printer.device raster rows into a low-memory streaming document
 * (JPEG, PWG Raster, PDF, PostScript, or Apple Raster/URF per Unit0's
 * ENGINE= setting) and submits it to
 * the configured IPP Print-Job endpoint.
 *
 * Trace output: T:MintPRINT-driver.log
 *
 * Job files (Debug JPEG/PWG Raster/PDF/PostScript/URF: MintPRINT-job.<ext>)
 * spool under T: by default, or under the Spooler option's configured hard
 * drive device (e.g. DH0:) on memory-tight systems - see SPOOL= in
 * driver/config.c and mp_build_spool_paths() below.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <utility/tagitem.h>
#include <devices/printer.h>
#include <devices/prtbase.h>
#include <devices/prtgfx.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "config.h"
#include "jpeg_writer.h"
#include "pwg_writer.h"
#include "pdf_writer.h"
#include "postscript_writer.h"
#include "urf_writer.h"
#include "ipp_client.h"
#include "media_size.h"
#include "spool.h"

/* Keep in sync with the $VER: string in printertag.s and
 * printertag_classic.s - logged at Init so a driver.log always says
 * exactly which build produced it, rather than relying on whoever's
 * reading it to separately check About or remember what they last
 * copied to DEVS:Printers/.
 *
 * version.revision, not a flat incrementing counter - the same
 * version.revision pairing AmigaOS's own $VER: convention uses (e.g.
 * Workbench 3.9's 47.102), so a "driver revision" is never just a bare
 * number that could be misread as printer.device's own fixed V44
 * PrinterSegment ABI marker in printertag.s.
 *
 * As of 1.2.2: MP_DRIVER_SUBREV is what moves for an ordinary driver
 * rebuild - 41.1, 41.2, 41.3, and so on - the same way a real Amiga
 * library keeps one version number across many small revisions.
 * MP_DRIVER_REV (the version half) only bumps for something that
 * warrants a new version number outright, not on every rebuild. */
#define MP_DRIVER_REV 41
#define MP_DRIVER_SUBREV 10

struct ExecBase *SysBase = NULL;
struct DosLibrary *DOSBase = NULL;
struct PrinterData *PD = NULL;
struct PrinterExtendedData *PED = NULL;
/* Owned here rather than by ipp_client.c itself, which only `extern`s it -
 * see that file's own comment on why (its GUI-build link target,
 * MintPrintSettings.c, already defines its own). ipp_client.c's own
 * mp_ipp_socket_available()/mp_ipp_query_imageable_margins()/
 * mp_ipp_print_document() (all called only from spool.c's dedicated
 * Process) open and close it around each bsdsocket.library use, same as
 * before this moved. */
struct Library *SocketBase = NULL;

extern struct PrinterExtendedData PEDData;

/*
 * OS 3.5+/3.2 extended printer-driver features:
 * - PRTA_NoIO: printer.device must not open parallel/serial transport.
 *   Tried removing this once to test a theory about printer.device's own
 *   NoIO code path; it made things worse (crashed WinUAE itself, not just
 *   the guest OS), so it stays on. See docs/DRIVER_SPOOL_PROCESS.md.
 * - PRTA_8BitGuns: request 8-bit Y/M/C/B intensity components, which is
 *   exactly what the future JPEG scanline backend wants.
 */
struct TagItem DriverTags[] = {
    { PRTA_NoIO,     TRUE },
    { PRTA_8BitGuns, TRUE },
    { TAG_DONE,      0 }
};

/* Job file base names, combined with the configured spool location
 * (g_config.spool: "RAM", meaning T: exactly as MintPRINT has always used
 * - whatever T: is assigned to, normally RAM: on a stock system - or a
 * real device such as "DH0:") into the g_job_file_* buffers below by
 * mp_build_spool_paths(), once per Open() bracket right after g_config is
 * loaded. See the Spooler option in MintPrint Settings and
 * driver/config.c's SPOOL=. */
/* const char *, not CONST_STRPTR (const UBYTE *): these are only ever
 * passed to mp_build_spool_path()'s plain-char base_name parameter below,
 * never straight to a dos.library call - the CONST_STRPTR cast belongs on
 * the finished g_job_file_* path instead (see its own call sites). */
#define MP_JOB_BASE_JPEG ((const char *)"MintPRINT-job.jpg")
#define MP_JOB_BASE_PWG  ((const char *)"MintPRINT-job.pwg")
#define MP_JOB_BASE_PDF  ((const char *)"MintPRINT-job.pdf")
#define MP_JOB_BASE_PS   ((const char *)"MintPRINT-job.ps")
#define MP_JOB_BASE_URF  ((const char *)"MintPRINT-job.urf")
#define MP_JOB_BASE_BACK ((const char *)"MintPRINT-back.rgb")

#define MP_SPOOL_PATH_MAX (MP_CONFIG_OPTION_MAX + 40) /* + "MPSPOOL/" + base name */
static char g_job_file_jpeg[MP_SPOOL_PATH_MAX];
static char g_job_file_pwg[MP_SPOOL_PATH_MAX];
static char g_job_file_pdf[MP_SPOOL_PATH_MAX];
static char g_job_file_ps[MP_SPOOL_PATH_MAX];
static char g_job_file_urf[MP_SPOOL_PATH_MAX];
static char g_job_file_back[MP_SPOOL_PATH_MAX];

/* Non-empty only while the current job is a tracked (Spooler HDD +
 * "Keep spooled jobs") one - the status sidecar mp_write_job_status()
 * writes to as the job progresses. Empty means "not tracked": no sidecar,
 * and the job file's original Debug-only retention behaviour applies
 * unchanged. Reset at the top of every mp_job_begin() (see there) so a
 * job that turns out untracked never inherits a stale path from the
 * previous one. */
static char g_job_status_path[MP_SPOOL_PATH_MAX + 8]; /* + ".status" */

/* Multiple Render(status=0 begin -> rows -> status=4 end) cycles inside one
 * Open()/Close() bracket is legitimate - that's how a real multi-page
 * document is printed, one physical page/IPP job per cycle. It's also how
 * real *strip* printing works (RKM "Printer Device": Strip Printing,
 * confirmed for real against Wordworth driver logs): an app splits
 * content taller than one Render(0) call into several same-width bands
 * and can add tiny auxiliary graphics dumps to the same physical page,
 * setting SPECIAL_NOFORMFEED (io_Special, read at case 5's pre-master call
 * and re-checked at each case 4) on every band but the last so the driver
 * knows not to eject/submit yet - RKM's own words for what the flag is
 * for: "multiple graphics dump on a page oriented printer". This driver
 * used to ignore that flag entirely and submit every band as its own
 * complete IPP job; with scaling=auto that turned one page into several
 * massive, blown-up strips. See case 4 below and mp_page_finalize().
 *
 * MP_TINY_PAGE_ROWS/STREAK_LIMIT remain as a separate safety net for
 * genuinely pathological dumps that DON'T carry legitimate strip
 * semantics (no SPECIAL_NOFORMFEED, just many tiny real pages in a row) -
 * with NOFORMFEED now honoured, a legitimate strip-printed page is merged
 * into one submission before this guard ever sees it, so it only fires on
 * the pathological case it was built for. */
#define MP_TINY_PAGE_ROWS 20
#define MP_TINY_PAGE_STREAK_LIMIT 3

enum {
    MP_ENGINE_JPEG = 0,
    MP_ENGINE_PWG = 1,
    MP_ENGINE_PDF = 2,
    MP_ENGINE_POSTSCRIPT = 3,
    MP_ENGINE_URF = 4
};

static ULONG g_page_width = 0;
static ULONG g_page_height = 0;
static ULONG g_rows_seen = 0;
static ULONG g_tiny_page_streak = 0;
static BOOL g_page_pending = FALSE;
/* Set when the PWG oversized-page-width clamp (below) actually rewrites
 * g_page_width away from printer.device's own reported width. When that
 * happens, printer.device's pi_xpos (observed ~839px for the same
 * DUMPRPORT-scaled source that reports the oversized ~3287px width) was
 * computed for the ORIGINAL, now-discarded width, not the clamped one - so
 * it no longer describes a valid offset into the now-narrower output
 * buffer and would run content off the right edge instead of merely
 * failing to center it. mp_job_write_row() uses this, in addition to
 * SPECIAL_CENTER, to decide when pi_xpos must be discarded in favour of a
 * freshly computed centered offset. Reset at the start of every new page
 * (never for a NOFORMFEED continuation band of the same page, which reuses
 * the same already-decided g_page_width). */
static BOOL g_recenter_clamped_page = FALSE;
static ULONG g_accum_width = 0;
static ULONG g_accum_height = 0;
static ULONG g_aux_height = 0;
/* Some strip-printing applications describe the blank top margin as one or
 * more impossible-width NOFORMFEED bands before the first real raster band.
 * Keep their vertical extent pending without opening a page; when a normal
 * width band arrives, materialise the pending height as white PWG rows. */
static ULONG g_leading_aux_height = 0;
static ULONG g_page_target_height = 0;
/* A strip-printing application normally renders fixed-height bands and ends
 * its logical printable page with one shorter remainder band. Wordworth
 * keeps SPECIAL_NOFORMFEED set even across that boundary, so rev35's
 * media-height-only splitter took the first 443 rows of page 2 to finish the
 * 3505-row A4 raster. Keep the normal band height and mark the current band
 * when it is shorter; case 4 confirms enough logical page height has accrued
 * before treating it as a page delimiter. */
static ULONG g_strip_nominal_band_height = 0;
static BOOL g_strip_current_band_short = FALSE;
/* Set by case 0 when a continuation band would push the accumulating page
 * past g_page_target_height: only the first g_split_at_row of that band's
 * rows belong to the page being finished, the rest start a fresh one. See
 * the case-1 split point and mp_begin_split_page() - without this, the
 * overshooting rows (and any of the next page's own leading margin caught
 * up in them) silently landed on the wrong physical page. */
static BOOL g_split_pending = FALSE;
static ULONG g_split_at_row = 0;
static BOOL g_runaway_tripped = FALSE;
static BOOL g_page_had_noformfeed = FALSE;
static BOOL g_discard_aux_band = FALSE;
static BOOL g_discard_aux_band_has_ink = FALSE;
static BOOL g_discard_leading_aux_band = FALSE;
/* io_Special from the most recent case-5 pre-master call - the one place
 * the RKM docs confirm carries it. Case 4 also receives io_Special as its
 * own x parameter per the same docs; case 4 logs its own x purely to
 * confirm that against real hardware, but decisions are made from this
 * captured value rather than trusting case 4's x, since only case 5's has
 * been directly verified in this codebase. */
static ULONG g_current_special = 0;
/* Graphics-oriented applications can set their page borders with the
 * standard DEC aSTBM command before issuing PRD_DUMPRPORT bands. MintPRINT
 * used to discard that command in DoSpecial(), so the printable raster had
 * the right height but was written at physical row zero. Keep the command's
 * one-based top line and current VMI until the first strip band can convert
 * them to device-resolution rows. */
static ULONG g_text_top_margin_line = 0;
static ULONG g_text_margin_vmi = 0;
static ULONG g_text_form_length_lines = 0;
static BOOL g_text_margins_cleared = FALSE;
static ULONG g_text_vertical_units = 0;
static ULONG g_text_vertical_advances = 0;
static BOOL g_sizing_pass = FALSE;

static BOOL g_job_open = FALSE;
static UBYTE *g_rgb_row = NULL;
static ULONG g_rgb_row_bytes = 0;
static UBYTE *g_jpeg_scratch = NULL;
static ULONG g_jpeg_scratch_bytes = 0;
static UBYTE *g_pwg_scratch = NULL;
static ULONG g_pwg_scratch_bytes = 0;
static UBYTE *g_pdf_scratch = NULL;
static ULONG g_pdf_scratch_bytes = 0;
static UBYTE *g_postscript_scratch = NULL;
static ULONG g_postscript_scratch_bytes = 0;
static UBYTE *g_urf_scratch = NULL;
static ULONG g_urf_scratch_bytes = 0;
static ULONG g_job_rows_written = 0;
static BOOL g_job_failed = FALSE;
static int g_engine = MP_ENGINE_JPEG;
static MPJpegEncoder g_jpeg;
static MPPwgEncoder g_pwg;
static MPPdfEncoder g_pdf;
static MPPostScriptEncoder g_postscript;
static MPUrfEncoder g_urf;
static struct MPConfig g_config;
static LONG g_config_source = MP_CONFIG_SOURCE_DEFAULTS;
static BOOL g_duplex_job_failed = FALSE;
static ULONG g_duplex_page_count = 0;
/* Tallest REAL accumulated height any strip-printed page in this duplex
 * job has finalized at so far (mp_page_finalize(), below). A physical
 * duplex sheet's front and back must share one height - real strip-printed
 * content can legitimately land above the media-derived target on one
 * side (its bands just don't divide evenly into the target) while another
 * side falls short and gets padded to only the target, producing a
 * front/back mismatch a real duplex unit can reject outright. Using this
 * as an extra floor alongside the media target for every later page keeps
 * the whole job converging on one consistent height without ever
 * truncating real content - see the strip-target computation in
 * Render()'s case 0. Reset per job in DriverOpen(); never decreases. */
static ULONG g_duplex_max_page_height = 0;
static ULONG g_job_file_bytes = 0;
static ULONG g_pwg_page_header_offset = 0;
/* Byte offset of the URF file header's page-count placeholder, patched
 * with the true total once a duplex job's last page is known - see
 * DriverClose(). Set once per job (the file header itself, unlike
 * g_urf_page_header_offset below, is written only once). */
static ULONG g_urf_file_header_offset = 0;
/* Byte offset of the CURRENT URF page's own 32-byte header - refreshed at
 * the start of every page, exactly like g_pwg_page_header_offset. Used to
 * patch that page's height field once a strip-printed page's true
 * accumulated height is known - see mp_page_finalize(). */
static ULONG g_urf_page_header_offset = 0;
static BOOL g_pwg_defer_rows = FALSE;
static BOOL g_pwg_reverse_x = FALSE;
static BOOL g_pwg_reverse_y = FALSE;
static BOOL g_pwg_aux_open = FALSE;
static ULONG g_pwg_aux_bytes = 0;

/* config.c only writes one of the five known engine literal strings. */
static int mp_detect_engine(const struct MPConfig *cfg)
{
    if (cfg->engine[0] == 'p' && cfg->engine[1] == 'w') return MP_ENGINE_PWG;
    if (cfg->engine[0] == 'p' && cfg->engine[1] == 'd') return MP_ENGINE_PDF;
    if (cfg->engine[0] == 'p' && cfg->engine[1] == 'o') return MP_ENGINE_POSTSCRIPT;
    if (cfg->engine[0] == 'u' && cfg->engine[1] == 'r') return MP_ENGINE_URF;
    return MP_ENGINE_JPEG;
}

static CONST_STRPTR mp_job_filename(void)
{
    switch (g_engine) {
        case MP_ENGINE_PWG: return (CONST_STRPTR)g_job_file_pwg;
        case MP_ENGINE_PDF: return (CONST_STRPTR)g_job_file_pdf;
        case MP_ENGINE_POSTSCRIPT: return (CONST_STRPTR)g_job_file_ps;
        case MP_ENGINE_URF: return (CONST_STRPTR)g_job_file_urf;
        default:            return (CONST_STRPTR)g_job_file_jpeg;
    }
}

static CONST_STRPTR mp_document_format(void)
{
    switch (g_engine) {
        case MP_ENGINE_PWG: return (CONST_STRPTR)"image/pwg-raster";
        case MP_ENGINE_PDF: return (CONST_STRPTR)"application/pdf";
        case MP_ENGINE_POSTSCRIPT:
            return (CONST_STRPTR)"application/postscript";
        case MP_ENGINE_URF: return (CONST_STRPTR)"image/urf";
        default:            return (CONST_STRPTR)"image/jpeg";
    }
}

static BOOL mp_duplex_requested(void)
{
    return (g_engine == MP_ENGINE_PWG || g_engine == MP_ENGINE_URF) &&
           g_config.sides[0] == 't';
}

/* PWG Raster and URF are the only two engines whose page header declares a
 * growable height that can be patched after the fact (mp_pwg_grow()/
 * mp_urf_grow(), MP_PWG_HEIGHT_HEADER_OFFSET/MP_URF_HEIGHT_FIELD_OFFSET) -
 * so they're the only two that can safely accumulate several
 * SPECIAL_NOFORMFEED bands into one physical page. JPEG/PDF/PostScript
 * remain single-band: a NOFORMFEED band under those engines finalizes
 * immediately as its own (usually wrong) page, exactly as before this
 * predicate existed. */
static BOOL mp_engine_supports_strip_accumulation(void)
{
    return g_engine == MP_ENGINE_PWG || g_engine == MP_ENGINE_URF;
}

static ULONG mp_strlen(const char *s)
{
    ULONG n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static BOOL mp_streq(const char *a, const char *b)
{
    ULONG i = 0;
    if (!a || !b) return FALSE;
    while (a[i] && b[i] && a[i] == b[i]) ++i;
    return a[i] == 0 && b[i] == 0;
}

/* Builds "<prefix><base_name>" into dst (bounded by cap). prefix is "T:"
 * when g_config.spool is empty or "RAM" - MintPRINT's original,
 * unconfigurable behaviour, spooling flat exactly as before - or
 * "<device>MPSPOOL/" for a configured hard drive device: MintPrint
 * Settings creates that drawer (hidden, no icon) the first time it's
 * saved - see MP_SPOOL_DIR_NAME (config.h) and
 * mp_ensure_hidden_spool_dir() (src/MintPrintSettings.c). No snprintf/
 * strcpy here: this driver is built freestanding (-nostartfiles, no
 * libc), same as every other string helper in this file. */
static void mp_build_spool_path(char *dst, ULONG cap, const char *base_name)
{
    BOOL use_ram = !g_config.spool[0] || mp_streq(g_config.spool, "RAM");
    const char *prefix = use_ram ? "T:" : g_config.spool;
    ULONG i, j;

    i = 0;
    while (prefix[i] && i + 1 < cap) { dst[i] = prefix[i]; ++i; }
    if (!use_ram) {
        const char *dirname = MP_SPOOL_DIR_NAME "/";
        for (j = 0; dirname[j] && i + 1 < cap; ++j, ++i) dst[i] = dirname[j];
    }
    for (j = 0; base_name[j] && i + 1 < cap; ++j, ++i) dst[i] = base_name[j];
    dst[i] = 0;
}

/* Called once per Open() bracket, right after g_config is (re)loaded in
 * Init()/DriverOpen(), so every job filename this job uses reflects
 * whichever Spooler location was in effect when the config was saved -
 * "RAM" (the historical T: behaviour) or a real hard drive partition for
 * memory-tight systems where even T:'s usual RAM: backing is scarce. */
static void mp_build_spool_paths(void)
{
    mp_build_spool_path(g_job_file_jpeg, sizeof(g_job_file_jpeg), MP_JOB_BASE_JPEG);
    mp_build_spool_path(g_job_file_pwg,  sizeof(g_job_file_pwg),  MP_JOB_BASE_PWG);
    mp_build_spool_path(g_job_file_pdf,  sizeof(g_job_file_pdf),  MP_JOB_BASE_PDF);
    mp_build_spool_path(g_job_file_ps,   sizeof(g_job_file_ps),   MP_JOB_BASE_PS);
    mp_build_spool_path(g_job_file_urf,  sizeof(g_job_file_urf),  MP_JOB_BASE_URF);
    mp_build_spool_path(g_job_file_back, sizeof(g_job_file_back), MP_JOB_BASE_BACK);
}

/* Timestamped job naming is implemented inside spool.c so all DOS clock
 * access remains inside the dedicated spool Process. */
static void mp_copy_bounded(char *dst, ULONG cap, const char *src)
{
    ULONG i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static void mp_append_bounded(char *dst, ULONG cap, const char *suffix)
{
    ULONG i = mp_strlen(dst);
    ULONG j;
    for (j = 0; suffix[j] && i + 1 < cap; ++j, ++i)
        dst[i] = suffix[j];
    dst[i] = 0;
}

/* The mutable counterpart of mp_job_filename() - a pointer to the same
 * g_job_file_* buffer that function reads, so mp_job_begin() can rewrite
 * it in place with a per-job unique name (tracked jobs only - see
 * there). Every later mp_job_filename() call this job's lifecycle makes
 * (mp_page_submit_and_track(), DriverClose()'s duplex submit, the
 * Debug-retention delete) then naturally sees the resolved name instead
 * of the fixed per-engine one. */
static char *mp_current_job_file_buf(void)
{
    switch (g_engine) {
        case MP_ENGINE_PWG: return g_job_file_pwg;
        case MP_ENGINE_PDF: return g_job_file_pdf;
        case MP_ENGINE_POSTSCRIPT: return g_job_file_ps;
        case MP_ENGINE_URF: return g_job_file_urf;
        default:            return g_job_file_jpeg;
    }
}

/* Every log call below builds one line into this buffer, then hands it to
 * the spool process (spool.c) with a single mp_spool_log() call - never a
 * direct dos.library call from here. See spool.h for why. */
#define MP_LOG_LINE_MAX 256
static char g_log_line[MP_LOG_LINE_MAX];
static ULONG g_log_pos;

static void mp_log_reset(void)
{
    g_log_pos = 0;
    g_log_line[0] = 0;
}

static void mp_log_append(const char *s)
{
    ULONG len = mp_strlen(s);
    ULONG i;
    for (i = 0; i < len && g_log_pos + 1 < MP_LOG_LINE_MAX; ++i)
        g_log_line[g_log_pos++] = s[i];
    g_log_line[g_log_pos] = 0;
}

static void mp_log_append_long(LONG value)
{
    char buf[16];
    ULONG pos = sizeof(buf) - 1;
    ULONG v;

    buf[sizeof(buf) - 1] = 0;

    if (value < 0) {
        mp_log_append("-");
        v = (ULONG)(-value);
    } else {
        v = (ULONG)value;
    }

    if (v == 0) {
        mp_log_append("0");
        return;
    }

    while (v && pos) {
        buf[--pos] = (char)('0' + (v % 10));
        v /= 10;
    }
    mp_log_append(&buf[pos]);
}

/* Overwrites the current job's status sidecar (g_job_status_path) with
 * STATE=<state>, HOST=/PORT=<the endpoint this job was rendered for>, and,
 * if result names a failure, ERROR=<error> <http> <ipp status> - the same
 * numeric triple mp_log_ipp_result() already logs, kept numeric here too
 * rather than inventing separate English text for it. HOST=/PORT= are
 * rewritten on every call (not just the first) since each call replaces
 * the whole file - g_config.host/g_config.port are what mp_job_begin()
 * itself is about to render against, so a Spooler window can show which
 * destination a retained job is tied to, and Retry knows whether it is
 * still pointed at the same printer. Reuses g_log_line/g_log_pos, the
 * same scratch buffer every mp_log_*() function above builds into - safe
 * because nothing else touches it between this function's own
 * mp_log_reset() and its own mp_spool_status_write() call. A no-op when
 * this job isn't tracked (RAM spool, or "Keep spooled jobs" off) - see
 * mp_job_begin(). */
static void mp_write_job_status(const char *state,
                                const struct MPIPPResult *result)
{
    if (!g_job_status_path[0]) return;

    mp_log_reset();
    mp_log_append("STATE=");
    mp_log_append(state);
    mp_log_append("\n");
    mp_log_append("HOST=");
    mp_log_append(g_config.host);
    mp_log_append("\n");
    mp_log_append("PORT=");
    mp_log_append_long((LONG)g_config.port);
    mp_log_append("\n");
    if (result) {
        mp_log_append("ERROR=");
        mp_log_append_long(result->error);
        mp_log_append(" ");
        mp_log_append_long(result->http_status);
        mp_log_append(" ");
        mp_log_append_long((LONG)result->ipp_status);
        mp_log_append("\n");
    }
    mp_spool_status_write((CONST_STRPTR)g_job_status_path, g_log_line,
                          g_log_pos);
}

static void mp_log_text(const char *event)
{
    if (!g_config.debug) return;
    mp_log_reset();
    mp_log_append("MintPRINT: ");
    mp_log_append(event);
    mp_spool_log(g_log_line);
}

static void mp_log_3(const char *event, LONG a, LONG b, LONG c)
{
    if (!g_config.debug) return;
    mp_log_reset();
    mp_log_append("MintPRINT: ");
    mp_log_append(event);
    mp_log_append(" ");
    mp_log_append_long(a);
    mp_log_append(" ");
    mp_log_append_long(b);
    mp_log_append(" ");
    mp_log_append_long(c);
    mp_spool_log(g_log_line);
}

/* Same "error/http/status" triple every IPP submission already logs via
 * mp_log_3(), plus a plain-English line for the two timeout-specific error
 * codes (see ipp_client.c's mp_connect_with_timeout()/
 * mp_recv_with_timeout()) that would otherwise look like any other -10/-14
 * failure in the log - e.g. issue #66, a printer that accepts a job and
 * then silently hangs instead of ever responding. */
static void mp_log_ipp_result(const char *event, const struct MPIPPResult *result)
{
    mp_log_3(event, result->error, result->http_status,
             (LONG)result->ipp_status);
    if (result->error == -17) {
        mp_log_text("No response from printer (timed out waiting to read "
                    "the IPP reply - printer accepted the job, then hung)");
    } else if (result->error == -18) {
        mp_log_text("No response from printer (timed out waiting to "
                    "connect - printer unreachable/offline)");
    }
}

static void mp_log_config(const struct MPConfig *cfg, LONG source)
{
    if (!cfg || !cfg->debug) return;

    mp_log_reset();
    mp_log_append("MintPRINT: Config source=");
    if (source == MP_CONFIG_SOURCE_ENV)
        mp_log_append("ENV");
    else if (source == MP_CONFIG_SOURCE_ENVARC)
        mp_log_append("ENVARC");
    else
        mp_log_append("defaults");
    mp_log_append(" host=");
    mp_log_append(cfg->host);
    mp_log_append(" port=");
    mp_log_append_long((LONG)cfg->port);
    mp_log_append(" path=");
    mp_log_append(cfg->path);
    mp_log_append(" debug=");
    mp_log_append_long(cfg->debug ? 1 : 0);
    if (cfg->media[0]) { mp_log_append(" media="); mp_log_append(cfg->media); }
    if (cfg->source[0]) { mp_log_append(" source="); mp_log_append(cfg->source); }
    if (cfg->color[0]) { mp_log_append(" color="); mp_log_append(cfg->color); }
    if (cfg->quality[0]) { mp_log_append(" quality="); mp_log_append(cfg->quality); }
    if (cfg->scaling[0]) { mp_log_append(" scaling="); mp_log_append(cfg->scaling); }
    if (cfg->sides[0]) { mp_log_append(" sides="); mp_log_append(cfg->sides); }
    if (cfg->pwg_sheet_back[0]) {
        mp_log_append(" pwg-sheet-back=");
        mp_log_append(cfg->pwg_sheet_back);
    }
    mp_log_append(" engine=");
    mp_log_append(cfg->engine[0] ? cfg->engine : "jpeg");
    mp_log_append(" spool=");
    mp_log_append(cfg->spool[0] ? cfg->spool : "RAM");
    mp_spool_log(g_log_line);
}

static long mp_job_file_write(void *ctx, const unsigned char *data, unsigned long length)
{
    (void)ctx;
    if (!mp_spool_job_write((const UBYTE *)data, (ULONG)length)) return -1;
    g_job_file_bytes += (ULONG)length;
    return (long)length;
}

static void mp_job_release_buffers(void)
{
    if (g_rgb_row) {
        FreeMem(g_rgb_row, g_rgb_row_bytes);
        g_rgb_row = NULL;
    }
    if (g_jpeg_scratch) {
        FreeMem(g_jpeg_scratch, g_jpeg_scratch_bytes);
        g_jpeg_scratch = NULL;
    }
    if (g_pwg_scratch) {
        FreeMem(g_pwg_scratch, g_pwg_scratch_bytes);
        g_pwg_scratch = NULL;
    }
    if (g_pdf_scratch) {
        FreeMem(g_pdf_scratch, g_pdf_scratch_bytes);
        g_pdf_scratch = NULL;
    }
    if (g_postscript_scratch) {
        FreeMem(g_postscript_scratch, g_postscript_scratch_bytes);
        g_postscript_scratch = NULL;
    }
    if (g_urf_scratch) {
        FreeMem(g_urf_scratch, g_urf_scratch_bytes);
        g_urf_scratch = NULL;
    }
    g_rgb_row_bytes = 0;
    g_jpeg_scratch_bytes = 0;
    g_pwg_scratch_bytes = 0;
    g_pdf_scratch_bytes = 0;
    g_postscript_scratch_bytes = 0;
    g_urf_scratch_bytes = 0;
}

static void mp_pwg_close_aux(void)
{
    if (g_pwg_aux_open) {
        mp_spool_aux_close();
        g_pwg_aux_open = FALSE;
    }
    mp_spool_job_delete((CONST_STRPTR)g_job_file_back);
    g_pwg_defer_rows = FALSE;
    g_pwg_reverse_x = FALSE;
    g_pwg_reverse_y = FALSE;
    g_pwg_aux_bytes = 0;
}

static void mp_job_cleanup(void)
{
    mp_pwg_close_aux();
    if (g_job_open) {
        mp_spool_job_close();
        g_job_open = FALSE;
    }
    mp_job_release_buffers();
}

static BOOL mp_job_begin(ULONG width, ULONG height)
{
    ULONG need;
    BOOL append_duplex_page;
    BOOL duplex;
    BOOL tumble;
    BOOL backside;
    LONG cross_feed = 1;
    LONG feed = 1;

    g_engine = mp_detect_engine(&g_config);
    append_duplex_page = mp_duplex_requested() && g_job_open &&
                         g_duplex_page_count > 0 &&
                         !g_duplex_job_failed;
    if (append_duplex_page)
        mp_job_release_buffers();
    else {
        mp_job_cleanup();
        g_job_file_bytes = 0;
    }
    g_job_rows_written = 0;
    g_job_failed = FALSE;
    g_pwg_defer_rows = FALSE;
    g_pwg_reverse_x = FALSE;
    g_pwg_reverse_y = FALSE;

    duplex = mp_duplex_requested();
    tumble = duplex && mp_streq(g_config.sides,
                                "two-sided-short-edge");
    backside = duplex && ((g_duplex_page_count + 1UL) & 1UL) == 0;

    /* PWG-only: Apple Raster's compact page header has no
     * CrossFeedTransform/FeedTransform-equivalent fields for describing a
     * pre-mirrored backside (see urf_writer.h), so a URF backside is never
     * reversed here - it streams in the same natural row/column order as
     * any front page, trusting the printer's own duplex mechanism to
     * orient it from the page header's duplex/tumble byte alone. */
    if (backside && g_engine == MP_ENGINE_PWG) {
        mp_pwg_backside_transform(g_config.sides,
                                  g_config.pwg_sheet_back,
                                  &cross_feed, &feed);
        g_pwg_reverse_x = cross_feed < 0 ? TRUE : FALSE;
        g_pwg_reverse_y = feed < 0 ? TRUE : FALSE;
        /* Horizontal reversal is possible one scanline at a time. Only a
         * vertical reversal needs compressed rows deferred to the aux file. */
        g_pwg_defer_rows = g_pwg_reverse_y;
    }

    if (!DOSBase || width == 0 || height == 0 || width > 65535UL || height > 65535UL) {
        mp_log_text("Job begin rejected invalid dimensions");
        return FALSE;
    }

    g_rgb_row_bytes = width * 3UL;

    switch (g_engine) {
        case MP_ENGINE_PWG: need = mp_pwg_scratch_size(width); break;
        case MP_ENGINE_PDF: need = mp_pdf_scratch_size(width); break;
        case MP_ENGINE_POSTSCRIPT: need = mp_postscript_scratch_size(width); break;
        case MP_ENGINE_URF: need = mp_urf_scratch_size(width); break;
        default:            need = mp_jpeg_scratch_size(width); break;
    }
    if (!need) {
        mp_log_text("Encoder scratch size rejected width");
        mp_job_cleanup();
        return FALSE;
    }

    /* Preflight both allocations below as one combined estimate against the
     * largest free MEMF_PUBLIC block, before touching AllocMem() at all.
     * On a memory-tight system (AmigaOS 2.04 minimum-spec, ~2MB total) a
     * page that is going to fail belongs failing here - with one clear log
     * line naming what was needed and what was free - rather than after
     * g_rgb_row's own AllocMem() already succeeded, fragmenting the free
     * list, only for the (usually larger) encoder scratch buffer to fail
     * right after it. MEMF_LARGEST asks for the size of the biggest
     * contiguous free block, which is exactly what a single AllocMem() call
     * of that size needs - a fragmented free list can show plenty of total
     * free memory while still being unable to satisfy either request. */
    if (AvailMem(MEMF_LARGEST | MEMF_PUBLIC) < (ULONG)(g_rgb_row_bytes + need)) {
        mp_log_3("Job begin rejected: insufficient memory needed/free/width",
                 (LONG)(g_rgb_row_bytes + need),
                 (LONG)AvailMem(MEMF_LARGEST | MEMF_PUBLIC),
                 (LONG)width);
        mp_job_cleanup();
        return FALSE;
    }

    g_rgb_row = (UBYTE *)AllocMem(g_rgb_row_bytes, MEMF_PUBLIC);
    if (!g_rgb_row) {
        mp_log_text("RGB row allocation failed");
        mp_job_cleanup();
        return FALSE;
    }

    switch (g_engine) {
        case MP_ENGINE_PWG:
            g_pwg_scratch_bytes = need;
            g_pwg_scratch = (UBYTE *)AllocMem(need, MEMF_PUBLIC);
            if (!g_pwg_scratch) {
                mp_log_text("PWG scratch allocation failed");
                mp_job_cleanup();
                return FALSE;
            }
            break;
        case MP_ENGINE_PDF:
            g_pdf_scratch_bytes = need;
            g_pdf_scratch = (UBYTE *)AllocMem(need, MEMF_PUBLIC);
            if (!g_pdf_scratch) {
                mp_log_text("PDF scratch allocation failed");
                mp_job_cleanup();
                return FALSE;
            }
            break;
        case MP_ENGINE_POSTSCRIPT:
            g_postscript_scratch_bytes = need;
            g_postscript_scratch = (UBYTE *)AllocMem(need, MEMF_PUBLIC);
            if (!g_postscript_scratch) {
                mp_log_text("PostScript scratch allocation failed");
                mp_job_cleanup();
                return FALSE;
            }
            break;
        case MP_ENGINE_URF:
            g_urf_scratch_bytes = need;
            g_urf_scratch = (UBYTE *)AllocMem(need, MEMF_PUBLIC);
            if (!g_urf_scratch) {
                mp_log_text("URF scratch allocation failed");
                mp_job_cleanup();
                return FALSE;
            }
            break;
        default:
            g_jpeg_scratch_bytes = need;
            g_jpeg_scratch = (UBYTE *)AllocMem(need, MEMF_PUBLIC);
            if (!g_jpeg_scratch) {
                mp_log_text("JPEG scratch allocation failed");
                mp_job_cleanup();
                return FALSE;
            }
            break;
    }

    if (!g_job_open) {
        /* Only a hard drive Spooler location with "Keep spooled jobs" on
         * gets a unique per-job name and a tracked status sidecar - RAM
         * (or Keep off) reuses the fixed per-engine name exactly as
         * before, unconditionally overwritten by mp_job_open()'s
         * MODE_NEWFILE every time, same as MintPRINT has always done. */
        BOOL track = g_config.spool_keep && g_config.spool[0] &&
                    !mp_streq(g_config.spool, "RAM");

        g_job_status_path[0] = 0;

        if (track) {
            char resolved[MP_SPOOL_PATH_MAX];

            /* Timestamp generation happens inside the spool Process; this
             * callback may be running in a bare Exec Task. */
            g_job_open = mp_spool_job_open_unique(
                (CONST_STRPTR)mp_job_filename(), resolved, sizeof(resolved));
            if (g_job_open) {
                mp_copy_bounded(mp_current_job_file_buf(), MP_SPOOL_PATH_MAX,
                                resolved);
                mp_copy_bounded(g_job_status_path, sizeof(g_job_status_path),
                                resolved);
                mp_append_bounded(g_job_status_path,
                                  sizeof(g_job_status_path), ".status");
            }
        } else {
            g_job_open = mp_spool_job_open(mp_job_filename());
        }

        if (!g_job_open) {
            mp_log_text("Job open failed for output file");
            mp_job_cleanup();
            return FALSE;
        }
        g_job_file_bytes = 0;
        mp_write_job_status("RENDERING", NULL);
    }

    switch (g_engine) {
        case MP_ENGINE_PWG: {
            /* PageSize is deliberately derived from the actual raster
             * pixels (page_pts_x/y = 0 -> mp_pwg_begin's own pixel/dpi
             * fallback), NOT from the configured media=. A real hardware
             * test proved that route wrong: declaring PageSize as full
             * A4 while the raster only covered a few inches of it made
             * the printer treat the raster as one tile and paginate
             * copies of it across multiple physical sheets to fill the
             * declared page (3 sheets for content ~1/3 of A4's height -
             * not a coincidence). PageSize needs to say "this raster IS
             * the whole page" so the printer doesn't try to fill more of
             * one than we actually sent. */
            BOOL write_sync = g_job_file_bytes == 0 ? TRUE : FALSE;
            g_pwg_page_header_offset = g_job_file_bytes +
                                       (write_sync ? 4UL : 0UL);
            if (!mp_pwg_begin_page(&g_pwg, width, height, 0, 0,
                                   g_config.resolution, write_sync,
                                   duplex, tumble, cross_feed, feed,
                                   g_pwg_scratch, g_pwg_scratch_bytes,
                                   mp_job_file_write, NULL)) {
                mp_log_text("PWG encoder begin failed");
                g_job_failed = TRUE;
                mp_job_cleanup();
                return FALSE;
            }
            if (g_pwg_defer_rows) {
                g_pwg_aux_open = mp_spool_aux_open((CONST_STRPTR)g_job_file_back);
                if (!g_pwg_aux_open) {
                    mp_log_text("PWG backside row store open failed");
                    g_job_failed = TRUE;
                    g_duplex_job_failed = TRUE;
                    mp_job_cleanup();
                    return FALSE;
                }
                mp_log_3("PWG backside transform x/y/page",
                         g_pwg_reverse_x ? -1 : 1,
                         g_pwg_reverse_y ? -1 : 1,
                         (LONG)(g_duplex_page_count + 1UL));
            }
            mp_log_3("PWG begin width/height/scratch",
                     (LONG)width, (LONG)height, (LONG)g_pwg_scratch_bytes);
            break;
        }
        case MP_ENGINE_PDF:
            if (!mp_pdf_begin(&g_pdf, width, height, g_config.resolution,
                              g_pdf_scratch,
                              g_pdf_scratch_bytes, mp_job_file_write, NULL)) {
                mp_log_text("PDF encoder begin failed");
                g_job_failed = TRUE;
                mp_job_cleanup();
                return FALSE;
            }
            mp_log_3("PDF begin width/height/scratch",
                     (LONG)width, (LONG)height, (LONG)g_pdf_scratch_bytes);
            break;
        case MP_ENGINE_POSTSCRIPT:
        {
            ULONG page_width_points = 0;
            ULONG page_height_points = 0;

            mp_media_page_points(g_config.media, width, height,
                                 &page_width_points, &page_height_points);
            if (!mp_postscript_begin(&g_postscript, width, height,
                                     page_width_points, page_height_points,
                                     g_config.resolution,
                                     g_postscript_scratch,
                                     g_postscript_scratch_bytes,
                                     mp_job_file_write, NULL)) {
                mp_log_text("PostScript encoder begin failed");
                g_job_failed = TRUE;
                mp_job_cleanup();
                return FALSE;
            }
            mp_log_3("PostScript begin width/height/scratch",
                     (LONG)width, (LONG)height,
                     (LONG)g_postscript_scratch_bytes);
            mp_log_3("PostScript page points width/height/dpi",
                     (LONG)page_width_points, (LONG)page_height_points,
                     (LONG)g_config.resolution);
            break;
        }
        case MP_ENGINE_URF: {
            /* write_file_header only for this job's very first page - the
             * 12-byte file header (including the duplex page-count
             * placeholder) is written exactly once, mirroring PWG's
             * write_sync just above. */
            BOOL write_file_header = g_job_file_bytes == 0 ? TRUE : FALSE;
            if (write_file_header)
                g_urf_file_header_offset = g_job_file_bytes;
            g_urf_page_header_offset = g_job_file_bytes +
                                       (write_file_header ? 12UL : 0UL);
            if (!mp_urf_begin_page(&g_urf, width, height, g_config.resolution,
                                   write_file_header, duplex, tumble,
                                   g_urf_scratch, g_urf_scratch_bytes,
                                   mp_job_file_write, NULL)) {
                mp_log_text("URF encoder begin failed");
                g_job_failed = TRUE;
                mp_job_cleanup();
                return FALSE;
            }
            mp_log_3("URF begin width/height/scratch",
                     (LONG)width, (LONG)height, (LONG)g_urf_scratch_bytes);
            break;
        }
        default:
            if (!mp_jpeg_begin(&g_jpeg, width, height, g_jpeg_scratch,
                               g_jpeg_scratch_bytes, mp_job_file_write, NULL)) {
                mp_log_text("JPEG encoder begin failed");
                g_job_failed = TRUE;
                mp_job_cleanup();
                return FALSE;
            }
            mp_log_3("JPEG begin width/height/scratch",
                     (LONG)width, (LONG)height, (LONG)g_jpeg_scratch_bytes);
            break;
    }

    return TRUE;
}

static BOOL mp_pwg_accept_row(const UBYTE *rgb)
{
    const unsigned char *encoded;
    unsigned long encoded_length;
    UBYTE be[4];

    if (!g_pwg_defer_rows)
        return mp_pwg_write_scanline(&g_pwg, rgb) ? TRUE : FALSE;

    if (!g_pwg_aux_open ||
        !mp_pwg_encode_scanline(&g_pwg, rgb, &encoded, &encoded_length) ||
        encoded_length > 0xffffffffUL - 4UL - g_pwg_aux_bytes) {
        return FALSE;
    }

    be[0] = (UBYTE)(encoded_length >> 24);
    be[1] = (UBYTE)(encoded_length >> 16);
    be[2] = (UBYTE)(encoded_length >> 8);
    be[3] = (UBYTE)encoded_length;
    if (!mp_spool_aux_write((const UBYTE *)encoded,
                            (ULONG)encoded_length) ||
        !mp_spool_aux_write(be, 4UL)) {
        return FALSE;
    }
    g_pwg_aux_bytes += (ULONG)encoded_length + 4UL;
    return TRUE;
}

/* True when cfg->color is one of PWG5100.3/RFC 8011's print-color-mode
 * keywords meaning "no colour, black ink only". mp_spool_ipp_submit()
 * (ipp_client.c) forwards cfg->color to the printer verbatim as the
 * print-color-mode job attribute, but that is only ever a REQUEST -
 * printer firmware is free to ignore it, and real hardware seen during
 * testing did: a "monochrome" job still carried full-colour raster/JPEG
 * pixel data and printed in colour. mp_job_write_row() below desaturates
 * the actual pixel data when this is true, so monochrome mode no longer
 * depends on the printer choosing to honour the hint. */
static BOOL mp_color_mode_wants_grayscale(const char *color)
{
    return mp_streq(color, "monochrome") ||
           mp_streq(color, "auto-monochrome") ||
           mp_streq(color, "bi-level") ||
           mp_streq(color, "process-bi-level") ||
           mp_streq(color, "process-monochrome");
}

/* The classic pre-V44 printer.device supplies 4-bit Y/M/C/B guns.
 * Expand at the point of consumption instead of allocating and copying a
 * second PrtInfo row in classic_render_shim.c. The >15 guard keeps this
 * harmless with replacement printer.device implementations that already
 * provide 8-bit values through the classic ABI. */
static UBYTE mp_gun_to_8bit(UBYTE value)
{
#ifdef MINTPRINT_CLASSIC_GUNS
    if (value <= 15)
        return (UBYTE)((value << 4) | value);
#endif
    return value;
}

static BOOL mp_job_write_row(struct PrtInfo *pi, ULONG row_number)
{
    ULONG src_x;
    ULONG dst_x;
    ULONG i;
    ULONG scaled_total = 0;

    if (!g_job_open || !g_rgb_row || !pi || !pi->pi_ColorInt) return FALSE;

    for (i = 0; i < g_rgb_row_bytes; ++i) g_rgb_row[i] = 255;

    if (pi->pi_ScaleX) {
        for (i = 0; i < (ULONG)pi->pi_width; ++i)
            scaled_total += (ULONG)pi->pi_ScaleX[i];
    } else {
        scaled_total = (ULONG)pi->pi_width;
    }

    /* printer.device's own pi_xpos has been observed (via a decoded test
     * job's actual PWG raster bytes) to place a DestCols/DestRows-scaled
     * DUMPRPORT source hard against one edge - 839px of blank on the left,
     * 31px on the right for a 2478px-wide image on a 3287px page - rather
     * than centering it, even with SPECIAL_CENTER set. Center
     * narrower-than-page content ourselves instead of trusting pi_xpos for
     * that case. A page-filling row (the common real-document case) is
     * unaffected: scaled_total >= g_page_width there, so this still falls
     * through to pi_xpos (normally 0) exactly as before.
     *
     * Gated on SPECIAL_CENTER || g_recenter_clamped_page, not SPECIAL_CENTER
     * alone: without ANY gate this used to fire for ANY narrower-than-page
     * row on ANY engine (JPEG/PWG/PDF all funnel through this function),
     * overriding a deliberately-positioned narrower image with a centered
     * one instead. SPECIAL_CENTER alone under-covers real hardware,
     * though: on the exact printer.device path this comment describes,
     * pi_xpos (~839px) was computed against the ORIGINAL oversized page
     * width (~3287px) before the PWG clamp above rewrites g_page_width down
     * to the true media width (~2480px) - so once clamped, that stale
     * pi_xpos is no longer even a valid offset into the now-narrower output
     * buffer, and using it runs content off the right edge rather than just
     * failing to center it. g_recenter_clamped_page (set only when that
     * clamp actually changed g_page_width, reset every new page) covers
     * that case without widening this override to any other narrower
     * content that never went through the clamp. */
    dst_x = (((g_current_special & SPECIAL_CENTER) || g_recenter_clamped_page) &&
              g_page_width > scaled_total)
                ? (g_page_width - scaled_total) / 2UL
                : (ULONG)pi->pi_xpos;

    if (row_number == 0)
        mp_log_3("Row xpos printer/used/scaled",
                 (LONG)pi->pi_xpos, (LONG)dst_x, (LONG)scaled_total);

    for (src_x = 0; src_x < (ULONG)pi->pi_width && dst_x < g_page_width; ++src_x) {
        union colorEntry *pixel = &pi->pi_ColorInt[src_x];
        ULONG repeat = pi->pi_ScaleX ? (ULONG)pi->pi_ScaleX[src_x] : 1UL;
        UBYTE red = (UBYTE)(255U -
            mp_gun_to_8bit(pixel->colorByte[PCMCYAN]));
        UBYTE green = (UBYTE)(255U -
            mp_gun_to_8bit(pixel->colorByte[PCMMAGENTA]));
        UBYTE blue = (UBYTE)(255U -
            mp_gun_to_8bit(pixel->colorByte[PCMYELLOW]));

        while (repeat-- && dst_x < g_page_width) {
            ULONG out = dst_x * 3UL;
            g_rgb_row[out + 0] = red;
            g_rgb_row[out + 1] = green;
            g_rgb_row[out + 2] = blue;
            ++dst_x;
        }
    }

    /* Desaturate before dispatching to whichever encoder is active below -
     * one place covers PWG Raster, PDF, PostScript, URF and JPEG alike,
     * rather than teaching each encoder about colour modes separately.
     * Runs over the whole row (including the white margin the loop above
     * never touched, still white component-wise averaged), not just the
     * printed columns - simpler than tracking the exact written range and
     * free of visible effect either way. */
    if (mp_color_mode_wants_grayscale(g_config.color)) {
        ULONG px;
        for (px = 0; px < g_rgb_row_bytes; px += 3UL) {
            UBYTE r = g_rgb_row[px + 0];
            UBYTE g = g_rgb_row[px + 1];
            UBYTE b = g_rgb_row[px + 2];
            /* Integer luma, ITU-R BT.601 weights fixed-point /256 -
             * avoids floating point for the classic 68000 build. */
            UBYTE gray = (UBYTE)((77UL * r + 150UL * g + 29UL * b) >> 8);
            g_rgb_row[px + 0] = gray;
            g_rgb_row[px + 1] = gray;
            g_rgb_row[px + 2] = gray;
        }
    }

    /* g_rows_seen (not g_job_rows_written) is the right comparison here:
     * it resets at the start of every Render(0), including a continuation
     * band of an accumulating page, matching row_number's own per-band
     * numbering (see the strip-printing accumulation in Render()'s case
     * 0) - g_job_rows_written deliberately keeps counting across bands of
     * the same page instead. */
    if (row_number != g_rows_seen - 1) {
        mp_log_3("Non-sequential row got/expected/height",
                 (LONG)row_number, (LONG)(g_rows_seen - 1),
                 (LONG)g_page_height);
    }

    switch (g_engine) {
        case MP_ENGINE_PWG:
            if (g_pwg_reverse_x)
                mp_pwg_reverse_rgb_row(g_rgb_row, g_page_width);
            if (!mp_pwg_accept_row(g_rgb_row)) {
                mp_log_3("PWG scanline failed row/written/expected",
                         (LONG)row_number, (LONG)g_job_rows_written,
                         (LONG)g_page_height);
                g_job_failed = TRUE;
                if (mp_duplex_requested()) g_duplex_job_failed = TRUE;
                return FALSE;
            }
            break;
        case MP_ENGINE_PDF:
            if (!mp_pdf_write_scanline(&g_pdf, g_rgb_row)) {
                mp_log_3("PDF scanline failed row/written/expected",
                         (LONG)row_number, (LONG)g_job_rows_written,
                         (LONG)g_page_height);
                g_job_failed = TRUE;
                return FALSE;
            }
            break;
        case MP_ENGINE_POSTSCRIPT:
            if (!mp_postscript_write_scanline(&g_postscript, g_rgb_row)) {
                mp_log_3("PostScript scanline failed row/written/expected",
                         (LONG)row_number, (LONG)g_job_rows_written,
                         (LONG)g_page_height);
                g_job_failed = TRUE;
                return FALSE;
            }
            break;
        case MP_ENGINE_URF:
            if (!mp_urf_write_scanline(&g_urf, g_rgb_row)) {
                mp_log_3("URF scanline failed row/written/expected",
                         (LONG)row_number, (LONG)g_job_rows_written,
                         (LONG)g_page_height);
                g_job_failed = TRUE;
                if (mp_duplex_requested()) g_duplex_job_failed = TRUE;
                return FALSE;
            }
            break;
        default:
            if (!mp_jpeg_write_scanline(&g_jpeg, g_rgb_row)) {
                mp_log_3("JPEG scanline failed row/written/expected",
                         (LONG)row_number, (LONG)g_job_rows_written,
                         (LONG)g_page_height);
                g_job_failed = TRUE;
                return FALSE;
            }
            break;
    }

    ++g_job_rows_written;
    return TRUE;
}

/* expected_rows is the total row count this job's encoder should have
 * received by now - g_page_height (this band's own height) for a normal
 * single-band page, or g_accum_height (the true accumulated total) when
 * finalizing a strip-printed page made of several bands. */
static BOOL mp_job_finish(ULONG expected_rows)
{
    BOOL ok = FALSE;
    const char *label;

    if (!g_job_failed && g_job_open && g_job_rows_written == expected_rows) {
        switch (g_engine) {
            case MP_ENGINE_PWG: ok = mp_pwg_finish(&g_pwg) ? TRUE : FALSE; break;
            case MP_ENGINE_PDF: ok = mp_pdf_finish(&g_pdf) ? TRUE : FALSE; break;
            case MP_ENGINE_POSTSCRIPT:
                ok = mp_postscript_finish(&g_postscript) ? TRUE : FALSE;
                break;
            case MP_ENGINE_URF: ok = mp_urf_finish(&g_urf) ? TRUE : FALSE; break;
            default:            ok = mp_jpeg_finish(&g_jpeg) ? TRUE : FALSE; break;
        }
        if (!ok) g_job_failed = TRUE;
    } else {
        g_job_failed = TRUE;
    }

    switch (g_engine) {
        case MP_ENGINE_PWG: label = "PWG end rows/expected/failed"; break;
        case MP_ENGINE_PDF: label = "PDF end rows/expected/failed"; break;
        case MP_ENGINE_POSTSCRIPT:
            label = "PostScript end rows/expected/failed";
            break;
        case MP_ENGINE_URF: label = "URF end rows/expected/failed"; break;
        default:            label = "JPEG end rows/expected/failed"; break;
    }
    mp_log_3(label, (LONG)g_job_rows_written, (LONG)expected_rows,
             g_job_failed ? 1 : 0);
    if (g_engine == MP_ENGINE_POSTSCRIPT) {
        mp_log_3("PostScript output bytes/writes/buffer",
                 (LONG)g_postscript.output_bytes,
                 (LONG)g_postscript.write_calls,
                 (LONG)MP_POSTSCRIPT_OUTPUT_BUFFER);
    }
    if (ok && mp_duplex_requested())
        mp_job_release_buffers();
    else
        mp_job_cleanup();
    return ok;
}

/* Ensures the currently-open encoder can accept total_rows. PWG and URF
 * offer a growable-declared-height contract (mp_pwg_grow()/mp_urf_grow()):
 * a media-sized strip job usually has the full row cap from its first band,
 * but unknown media (or - since driver revision 40 - the leading-margin
 * restoration below, which is not gated to any one engine) can still need
 * to grow it as more rows are accounted for. JPEG/PDF/PostScript have no
 * such contract - mp_job_begin() must have already sized them for
 * total_rows up front (which every current caller does: case 0 folds any
 * leading margin into encoder_height before calling mp_job_begin(), for
 * every engine, precisely so this never needs to grow one of them) - so
 * total_rows exceeding what they were already sized for is a real error,
 * not something this function can fix up after the fact. */
static BOOL mp_job_reserve_page(ULONG total_rows)
{
    if (!g_job_open || g_job_failed) return FALSE;
    switch (g_engine) {
        case MP_ENGINE_URF:
            if (total_rows <= g_urf.height) return TRUE;
            if (!mp_urf_grow(&g_urf, total_rows - g_urf.height)) {
                mp_log_text("URF grow failed (accumulated page too tall)");
                g_job_failed = TRUE;
                return FALSE;
            }
            return TRUE;
        case MP_ENGINE_PWG:
            if (total_rows <= g_pwg.height) return TRUE;
            if (!mp_pwg_grow(&g_pwg, total_rows - g_pwg.height)) {
                mp_log_text("PWG grow failed (accumulated page too tall)");
                g_job_failed = TRUE;
                return FALSE;
            }
            return TRUE;
        case MP_ENGINE_PDF:
            if (total_rows <= g_pdf.height) return TRUE;
            mp_log_text("PDF page too tall for its declared height");
            g_job_failed = TRUE;
            return FALSE;
        case MP_ENGINE_POSTSCRIPT:
            if (total_rows <= g_postscript.height) return TRUE;
            mp_log_text("PostScript page too tall for its declared height");
            g_job_failed = TRUE;
            return FALSE;
        default: /* MP_ENGINE_JPEG */
            if (total_rows <= g_jpeg.height) return TRUE;
            mp_log_text("JPEG page too tall for its declared height");
            g_job_failed = TRUE;
            return FALSE;
    }
}

static BOOL mp_job_pad_page(ULONG target_rows)
{
    ULONG i;

    if (!g_job_open || g_job_failed || !g_rgb_row) return FALSE;
    if (!mp_job_reserve_page(target_rows)) return FALSE;

    for (i = 0; i < g_rgb_row_bytes; ++i) g_rgb_row[i] = 255;
    while (g_job_rows_written < target_rows) {
        BOOL ok;

        switch (g_engine) {
            case MP_ENGINE_URF:
                ok = mp_urf_write_scanline(&g_urf, g_rgb_row) ? TRUE : FALSE;
                break;
            case MP_ENGINE_PWG:
                ok = mp_pwg_accept_row(g_rgb_row);
                break;
            case MP_ENGINE_PDF:
                ok = mp_pdf_write_scanline(&g_pdf, g_rgb_row) ? TRUE : FALSE;
                break;
            case MP_ENGINE_POSTSCRIPT:
                ok = mp_postscript_write_scanline(&g_postscript, g_rgb_row)
                    ? TRUE : FALSE;
                break;
            default: /* MP_ENGINE_JPEG */
                ok = mp_jpeg_write_scanline(&g_jpeg, g_rgb_row) ? TRUE : FALSE;
                break;
        }
        if (!ok) {
            mp_log_text("Blank page padding failed");
            g_job_failed = TRUE;
            return FALSE;
        }
        ++g_job_rows_written;
    }
    return TRUE;
}

static BOOL mp_pwg_replay_backside(ULONG rows)
{
    ULONG out_row;
    ULONG cursor;

    if (!g_pwg_defer_rows) return TRUE;
    if (!g_pwg_aux_open || !g_pwg_scratch || !g_pwg_scratch_bytes)
        return FALSE;

    cursor = g_pwg_aux_bytes;

    for (out_row = 0; out_row < rows; ++out_row) {
        UBYTE be[4];
        ULONG encoded_length;

        if (cursor < 4UL ||
            !mp_spool_aux_read(cursor - 4UL, be, 4UL)) {
            mp_log_text("PWG backside row length read failed");
            return FALSE;
        }
        encoded_length = ((ULONG)be[0] << 24) |
                         ((ULONG)be[1] << 16) |
                         ((ULONG)be[2] << 8) |
                         (ULONG)be[3];
        if (!encoded_length || encoded_length > g_pwg_scratch_bytes ||
            cursor < encoded_length + 4UL) {
            mp_log_text("PWG backside row length invalid");
            return FALSE;
        }
        cursor -= encoded_length + 4UL;
        if (!mp_spool_aux_read(cursor, g_pwg_scratch, encoded_length)) {
            mp_log_text("PWG backside row read failed");
            return FALSE;
        }
        if (!mp_pwg_write_encoded_scanline(&g_pwg, g_pwg_scratch,
                                            encoded_length)) {
            mp_log_text("PWG backside encode failed");
            return FALSE;
        }
    }

    if (cursor != 0) {
        mp_log_text("PWG backside row store trailing data");
        return FALSE;
    }

    mp_pwg_close_aux();
    return TRUE;
}

static BOOL mp_row_has_ink(struct PrtInfo *pi)
{
    ULONG i;

    if (!pi || !pi->pi_ColorInt) return FALSE;
    for (i = 0; i < (ULONG)pi->pi_width; ++i) {
        union colorEntry *pixel = &pi->pi_ColorInt[i];
        if (pixel->colorByte[PCMYELLOW] ||
            pixel->colorByte[PCMMAGENTA] ||
            pixel->colorByte[PCMCYAN] ||
            pixel->colorByte[PCMBLACK])
            return TRUE;
    }
    return FALSE;
}

/* Submits the just-finished job file and applies the same runaway-storm
 * bookkeeping either page-submission path needs. One-sided output deliberately
 * keeps the old one-Print-Job-per-page route. Duplex PWG pages stay queued in
 * one open multi-page RaS2 file; DriverClose submits that file once.
 * rows_for_streak is the height the tiny-page-storm guard should judge
 * this submission by - the real total for a strip-accumulated page, not
 * any one band's own height. Returns the IPP operation's result. */
static LONG mp_page_submit_and_track(ULONG rows_for_streak)
{
    struct MPIPPResult result;
    LONG ipp_rc;
    CONST_STRPTR fname = mp_job_filename();
    CONST_STRPTR fmt = mp_document_format();

    if (mp_duplex_requested()) {
        ++g_duplex_page_count;
        ipp_rc = 0;
        result.error = 0;
        result.http_status = 0;
        result.ipp_status = 0;
        result.document_bytes = g_job_file_bytes;
        mp_log_3("Duplex page queued pages/rows/bytes",
                 (LONG)g_duplex_page_count, (LONG)rows_for_streak,
                 (LONG)g_job_file_bytes);
    } else {
        mp_write_job_status("SUBMITTING", NULL);
        ipp_rc = mp_spool_ipp_submit(&g_config, fname, fmt, &result);
        mp_write_job_status(ipp_rc == 0 ? "DONE" : "FAILED",
                            ipp_rc == 0 ? NULL : &result);
    }
    mp_log_ipp_result("IPP result error/http/status", &result);

    if (ipp_rc == 0) {
        if (rows_for_streak < MP_TINY_PAGE_ROWS) {
            ++g_tiny_page_streak;
            if (g_tiny_page_streak >= MP_TINY_PAGE_STREAK_LIMIT) {
                g_runaway_tripped = TRUE;
                mp_log_3("Runaway tiny-page storm detected, refusing further pages this print - height/limit/streak",
                         (LONG)rows_for_streak, (LONG)MP_TINY_PAGE_ROWS,
                         (LONG)g_tiny_page_streak);
            }
        } else {
            g_tiny_page_streak = 0;
        }
    }
    /* A tracked (spool_keep) job's file is never auto-deleted, success or
     * failure alike - that retention is the entire point of the Spooler
     * management window. Untracked jobs keep the original Debug-only
     * retention behaviour. */
    if (!g_config.debug && !mp_duplex_requested() && !g_job_status_path[0])
        mp_spool_job_delete(fname);
    return ipp_rc;
}

/* Ends the page currently open - which may be a single band (the common
 * case: SPECIAL_NOFORMFEED was never set, so case 4 finalizes on its very
 * first call) or the last of several bands accumulated across multiple
 * Render(0)/.../Render(4) cycles while SPECIAL_NOFORMFEED held it open
 * (see case 0/4 below). A media-sized multi-band PWG page is padded with
 * white rows before finishing; an unknown-media page is grown as bands
 * arrive. The final header geometry is patched to match the rows actually
 * written. JPEG/PDF/PostScript remain single-band because they cannot support this
 * kind of post-hoc streaming adjustment.
 *
 * Returns TRUE if there was nothing to finalize, or finalizing succeeded;
 * FALSE only if a page was actually finalized here and failed - in the
 * common single-band case this runs synchronously inside the same
 * Render(4) call that's about to return to printer.device, so that
 * caller can still turn a real failure into PDERR_CANCEL. Only the
 * earlier, NOFORMFEED-held-open bands of a multi-band page lose that
 * synchronous signal (they already returned PDERR_NOERR before this
 * could be known); the outcome is still fully visible in the driver
 * log. */
static BOOL mp_page_finalize(void)
{
    BOOL job_ok;

    if (!g_page_pending) return TRUE;
    g_page_pending = FALSE;

    if (g_engine == MP_ENGINE_PWG && !g_job_failed) {
        UBYTE be[4];
        ULONG page_points_y;

        /* A NOFORMFEED strip dump describes content placed on a physical
         * page, not a page whose height ends at the final ink row. Pad the
         * streamed raster to the complete media-derived height selected at
         * the first band. Without this, a short portrait document such as
         * 2478x2100 is advertised as landscape and printers auto-rotate it. */
        if (g_page_had_noformfeed && g_pwg.height > g_accum_height) {
            ULONG before = g_accum_height;
            if (!mp_job_pad_page(g_pwg.height)) {
                g_job_failed = TRUE;
            } else {
                g_accum_height = g_pwg.height;
                mp_log_3("PWG padded strip page rows/from/to",
                         (LONG)(g_accum_height - before),
                         (LONG)before, (LONG)g_accum_height);
            }
        }

        be[0] = (UBYTE)(g_accum_height >> 24);
        be[1] = (UBYTE)(g_accum_height >> 16);
        be[2] = (UBYTE)(g_accum_height >> 8);
        be[3] = (UBYTE)g_accum_height;
        if (!mp_spool_job_patch(g_pwg_page_header_offset +
                                MP_PWG_HEIGHT_HEADER_OFFSET, be, 4)) {
            mp_log_text("Failed to patch accumulated PWG page height");
            g_job_failed = TRUE;
        }

        /* mp_pwg_begin used the first band's height in older builds. Keep
         * PageSizeY consistent with the final cupsHeight even when media
         * parsing failed and the encoder had to grow as bands arrived. */
        page_points_y = (g_accum_height * 72UL) /
                        (g_config.resolution ? g_config.resolution : 300UL);
        be[0] = (UBYTE)(page_points_y >> 24);
        be[1] = (UBYTE)(page_points_y >> 16);
        be[2] = (UBYTE)(page_points_y >> 8);
        be[3] = (UBYTE)page_points_y;
        if (!mp_spool_job_patch(g_pwg_page_header_offset +
                                MP_PWG_PAGESIZE_Y_HEADER_OFFSET, be, 4)) {
            mp_log_text("Failed to patch accumulated PWG PageSizeY");
            g_job_failed = TRUE;
        }

        if (!g_job_failed && !mp_pwg_replay_backside(g_accum_height)) {
            g_job_failed = TRUE;
            g_duplex_job_failed = TRUE;
        }
    } else if (g_engine == MP_ENGINE_URF && !g_job_failed) {
        UBYTE be[4];

        /* Same strip-page padding PWG needs above, using URF's own
         * growable height (mp_urf_grow(), via mp_job_pad_page()). */
        if (g_page_had_noformfeed && g_urf.height > g_accum_height) {
            ULONG before = g_accum_height;
            if (!mp_job_pad_page(g_urf.height)) {
                g_job_failed = TRUE;
            } else {
                g_accum_height = g_urf.height;
                mp_log_3("URF padded strip page rows/from/to",
                         (LONG)(g_accum_height - before),
                         (LONG)before, (LONG)g_accum_height);
            }
        }

        /* Patch this page's own height field with the final accumulated
         * total - URF has no PageSizeY-equivalent second field, and no
         * backside replay (see urf_writer.h: no row/column reversal is
         * ever performed for a URF backside). */
        be[0] = (UBYTE)(g_accum_height >> 24);
        be[1] = (UBYTE)(g_accum_height >> 16);
        be[2] = (UBYTE)(g_accum_height >> 8);
        be[3] = (UBYTE)g_accum_height;
        if (!mp_spool_job_patch(g_urf_page_header_offset +
                                MP_URF_HEIGHT_FIELD_OFFSET, be, 4)) {
            mp_log_text("Failed to patch accumulated URF page height");
            g_job_failed = TRUE;
        }
    }

    job_ok = mp_job_finish(g_accum_height);
    if (!job_ok) {
        if (mp_duplex_requested()) g_duplex_job_failed = TRUE;
        return FALSE;
    }
    if (mp_duplex_requested() && g_accum_height > g_duplex_max_page_height)
        g_duplex_max_page_height = g_accum_height;
    return mp_page_submit_and_track(g_accum_height) == 0;
}

/* Starts the page that holds a split band's overshoot rows - the part of a
 * SPECIAL_NOFORMFEED band that didn't fit on the page mp_page_finalize()
 * just closed (see the case-1 split point in Render()). Mirrors case 0's
 * own "begin new page" path (media/duplex-floor target, mp_job_begin(),
 * the g_page_pending/g_accum_width/g_accum_height/g_page_target_height
 * reset), minus the leading-whitespace and tiny-auxiliary-band handling
 * that never applies here - a split page always starts with real content
 * rows already in hand, never a leading blank band still to be
 * discovered. */
static BOOL mp_begin_split_page(ULONG width, ULONG remaining_height)
{
    ULONG encoder_height = remaining_height;
    ULONG media_height = 0;

    g_page_width = width;
    g_page_height = remaining_height;
    g_recenter_clamped_page = FALSE;

    if (mp_detect_engine(&g_config) == MP_ENGINE_PWG ||
        mp_detect_engine(&g_config) == MP_ENGINE_URF) {
        media_height = mp_media_target_height(g_config.media, width,
                                              g_config.resolution);
        if (mp_duplex_requested() && g_duplex_max_page_height > media_height)
            media_height = g_duplex_max_page_height;
        if (media_height > encoder_height)
            encoder_height = media_height;
    }

    if (!mp_job_begin(width, encoder_height)) {
        if (mp_duplex_requested()) g_duplex_job_failed = TRUE;
        return FALSE;
    }

    g_page_pending = TRUE;
    g_accum_width = width;
    g_accum_height = remaining_height;
    g_aux_height = 0;
    g_page_target_height = media_height;
    g_page_had_noformfeed = TRUE;
    return TRUE;
}

static void mp_log_row(struct PrtInfo *pi, ULONG row)
{
    ULONG i;
    ULONG scaled_width = 0;

    if (!pi || !g_config.debug) return;

    if (pi->pi_ScaleX) {
        for (i = 0; i < pi->pi_width; ++i)
            scaled_width += pi->pi_ScaleX[i];
    } else {
        scaled_width = pi->pi_width;
    }

    mp_log_reset();
    mp_log_append("MintPRINT: row=");
    mp_log_append_long((LONG)row);
    mp_log_append(" sourceWidth=");
    mp_log_append_long((LONG)pi->pi_width);
    mp_log_append(" scaledWidth=");
    mp_log_append_long((LONG)scaled_width);
    mp_log_append(" xpos=");
    mp_log_append_long((LONG)pi->pi_xpos);
    mp_log_append(" pageWidth=");
    mp_log_append_long((LONG)g_page_width);

    if (pi->pi_ColorInt && pi->pi_width) {
        union colorEntry *p = &pi->pi_ColorInt[0];
        mp_log_append(" firstYMCB=");
        mp_log_append_long(p->colorByte[PCMYELLOW]);
        mp_log_append(",");
        mp_log_append_long(p->colorByte[PCMMAGENTA]);
        mp_log_append(",");
        mp_log_append_long(p->colorByte[PCMCYAN]);
        mp_log_append(",");
        mp_log_append_long(p->colorByte[PCMBLACK]);
    }

    mp_spool_log(g_log_line);
}

LONG PRT_STDARGS Init(struct PrinterData *pd)
{
    /* SysBase has to be valid before calling any exec.library function. */
    SysBase = *(struct ExecBase **)4L;
    PD = pd;
    PED = &PEDData;

    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
    if (!DOSBase) {
        return -1;
    }

    mp_config_defaults(&g_config);
    /* mp_spool_ensure_running() performs the functional socket probe inside
     * its dedicated Process. That matters because printer.device callbacks
     * can originate from a bare Task, where calling bsdsocket directly is
     * not a safe assumption. Fail before any raster rows are accepted. */
    if (!mp_spool_ensure_running()) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
        return -1;
    }
    g_config_source = mp_spool_config_load(&g_config);
    mp_build_spool_paths();
    mp_log_text("Init");
    if (g_config.debug) {
        mp_log_reset();
        mp_log_append("MintPRINT: Driver revision ");
        mp_log_append_long((LONG)MP_DRIVER_REV);
        mp_log_append(".");
        mp_log_append_long((LONG)MP_DRIVER_SUBREV);
        mp_spool_log(g_log_line);
    }
    return 0;
}

VOID PRT_STDARGS Expunge(void)
{
    mp_log_text("Expunge");
    mp_job_cleanup();
    mp_spool_shutdown();

    if (DOSBase) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }

    PD = NULL;
    PED = NULL;
}

int PRT_STDARGS DriverOpen(struct IORequest *ior)
{
    (void)ior;
    g_tiny_page_streak = 0;
    g_runaway_tripped = FALSE;
    g_page_pending = FALSE;
    g_recenter_clamped_page = FALSE;
    g_sizing_pass = FALSE;
    g_current_special = 0;
    g_text_top_margin_line = 0;
    g_text_margin_vmi = 0;
    g_text_form_length_lines = 0;
    g_text_margins_cleared = FALSE;
    g_text_vertical_units = 0;
    g_text_vertical_advances = 0;
    g_page_had_noformfeed = FALSE;
    g_discard_aux_band = FALSE;
    g_discard_aux_band_has_ink = FALSE;
    g_discard_leading_aux_band = FALSE;
    g_aux_height = 0;
    g_leading_aux_height = 0;
    g_page_target_height = 0;
    g_strip_nominal_band_height = 0;
    g_strip_current_band_short = FALSE;
    g_split_pending = FALSE;
    g_split_at_row = 0;
    g_duplex_job_failed = FALSE;
    g_duplex_page_count = 0;
    g_duplex_max_page_height = 0;
    g_job_file_bytes = 0;
    g_pwg_page_header_offset = 0;
    g_urf_file_header_offset = 0;
    g_urf_page_header_offset = 0;
    /* Loaded once per Open() bracket (i.e. once per print job) rather than
     * per-page inside Render() case 0: case 5, which sets the PED
     * resolution/MaxDots fields below, fires BEFORE case 0 on every job's
     * first page, so it needs the real configured resolution available
     * here already, not just Init()'s one-time compiled-in default. */
    g_config_source = mp_spool_config_load(&g_config);
    mp_build_spool_paths();
    g_engine = mp_detect_engine(&g_config);
    if (g_config.sides[0] == 't' &&
        g_engine != MP_ENGINE_PWG && g_engine != MP_ENGINE_URF) {
        mp_log_text("Duplex requires PWG Raster or URF; using one-sided");
        g_config.sides[0] = 0;
    }
    mp_log_config(&g_config, g_config_source);
    mp_log_text("Open");
    return 0;
}

VOID PRT_STDARGS DriverClose(struct IORequest *ior)
{
    struct MPIPPResult result;
    LONG ipp_rc;

    (void)ior;
    /* A strip-printed page can still be mid-accumulation when the whole
     * print request ends normally - finalize and submit it rather than
     * silently discarding its last (possibly only) bands. */
    if (g_page_pending) mp_page_finalize();

    /* Duplex pages have been appended to one multi-page PWG Raster or URF
     * stream. Close the local file before reopening it for the one normal
     * IPP Print-Job that carries sides=. A failed page invalidates the
     * complete stream rather than printing a misleading partial
     * document. */
    if (g_duplex_page_count > 0) {
        /* URF's page count lives once in the file header, written as an
         * "unknown/streaming" placeholder when the first page began (see
         * urf_writer.h) - patch in the true total now that it's known,
         * while the file is still open (mp_spool_job_patch() rewrites
         * bytes in the currently-open job file). PWG needs no equivalent:
         * its per-page header carries no file-wide page count at all. */
        if (g_engine == MP_ENGINE_URF && g_job_open && !g_duplex_job_failed) {
            UBYTE be[4];
            be[0] = (UBYTE)(g_duplex_page_count >> 24);
            be[1] = (UBYTE)(g_duplex_page_count >> 16);
            be[2] = (UBYTE)(g_duplex_page_count >> 8);
            be[3] = (UBYTE)g_duplex_page_count;
            if (!mp_spool_job_patch(g_urf_file_header_offset +
                                    MP_URF_PAGECOUNT_FIELD_OFFSET, be, 4)) {
                mp_log_text("Failed to patch URF duplex page count");
                g_duplex_job_failed = TRUE;
            }
        }
        if (g_job_open) {
            mp_spool_job_close();
            g_job_open = FALSE;
        }
        if (!g_duplex_job_failed) {
            mp_write_job_status("SUBMITTING", NULL);
            ipp_rc = mp_spool_ipp_submit(&g_config, mp_job_filename(),
                                          mp_document_format(), &result);
            mp_log_ipp_result("IPP duplex Print-Job error/http/status",
                              &result);
            if (ipp_rc != 0) g_duplex_job_failed = TRUE;
            mp_write_job_status(ipp_rc == 0 ? "DONE" : "FAILED",
                                ipp_rc == 0 ? NULL : &result);
        } else {
            mp_log_text("Duplex job discarded after page failure");
            mp_write_job_status("FAILED", NULL);
        }
        if (!g_config.debug && !g_job_status_path[0])
            mp_spool_job_delete(mp_job_filename());
    }
    mp_job_cleanup();
    mp_log_text("Close");
}

LONG PRT_STDARGS DoSpecial(UWORD *command, UBYTE output_buffer[],
                           BYTE *current_line_position,
                           BYTE *current_line_spacing,
                           BYTE *crlf_flag, STRPTR params)
{
    (void)output_buffer;

    /* Keep a complete trace of the page-layout command range. Several old
     * applications maintain document borders in their own UI but expose only
     * some subset of this state to printer.device. Logging every command and
     * both parameter slots, plus printer.device's live layout state, makes
     * missing information distinguishable from information MintPRINT merely
     * failed to interpret. Commands 55..66 are aVERP0 through aCAM. */
    if (command && *command >= aVERP0 && *command <= aCAM) {
        mp_log_3("Text layout command id/param0/param1",
                 (LONG)*command,
                 params ? (LONG)((UBYTE *)params)[0] : -1,
                 params ? (LONG)((UBYTE *)params)[1] : -1);
        mp_log_3("Text layout state linePos/VMI/crlf",
                 current_line_position ?
                     (LONG)*current_line_position : -1,
                 current_line_spacing ?
                     (LONG)(UBYTE)*current_line_spacing : -1,
                 crlf_flag ? (LONG)*crlf_flag : -1);
    }

    /* Wordsworth uses the printer.device command stream to position its
     * graphics printable area. aSTBM's first parameter is a one-based top
     * line; current_line_spacing is VMI in 1/216-inch units. Retaining both
     * lets Render case 0 prepend the exact physical top border before the
     * first raster band. The bottom parameter need not be stored: the
     * media-height finalizer already supplies all remaining white rows at
     * the bottom after the logical printable page ends. */
    if (command && *command == aVERP0) {
        g_text_margin_vmi = 27UL; /* 1/8 inch = 27/216 inch */
        if (current_line_spacing) *current_line_spacing = 27;
        mp_log_3("Text VMI command/id/units", (LONG)*command, 27, 0);
    } else if (command && *command == aVERP1) {
        g_text_margin_vmi = 36UL; /* 1/6 inch = 36/216 inch */
        if (current_line_spacing) *current_line_spacing = 36;
        mp_log_3("Text VMI command/id/units", (LONG)*command, 36, 0);
    } else if (command && *command == aSTBM && params && current_line_spacing) {
        g_text_top_margin_line = (ULONG)((UBYTE *)params)[0];
        g_text_margin_vmi = (ULONG)(UBYTE)*current_line_spacing;
        g_text_margins_cleared = FALSE;
        mp_log_3("Text page margins topLine/VMI/bottomLine",
                 (LONG)g_text_top_margin_line,
                 (LONG)g_text_margin_vmi,
                 (LONG)((UBYTE *)params)[1]);
    } else if (command && *command == aCAM) {
        g_text_top_margin_line = 0;
        g_text_margin_vmi = 0;
        g_text_margins_cleared = TRUE;
        mp_log_text("Text page margins cleared");
    } else if (command && *command == aSLPP && params) {
        g_text_form_length_lines = (ULONG)((UBYTE *)params)[0];
        mp_log_3("Text form length lines/VMI/marginsCleared",
                 (LONG)g_text_form_length_lines,
                 current_line_spacing ?
                     (LONG)(UBYTE)*current_line_spacing : -1,
                 g_text_margins_cleared ? 1 : 0);
    } else if (command &&
               (*command == aTMS || *command == aBMS ||
                *command == aPERF ||
                *command == aPERF0)) {
        mp_log_3("Text page command id/param0/VMI",
                 (LONG)*command,
                 params ? (LONG)((UBYTE *)params)[0] : -1,
                 current_line_spacing ?
                     (LONG)(UBYTE)*current_line_spacing : -1);
    }

    /* No physical escape language is emitted: raster and captured-text jobs
     * are submitted through IPP instead of a parallel/serial transport. */
    return 0;
}

/* Called by command_table.c for aIND/aNEL and literal LF traffic. Keep the
 * physical vertical motion separate from captured text: a graphics-oriented
 * application can advance the paper before PRD_DUMPRPORT without ever
 * emitting printable characters. A zero VMI means printer.device supplied no
 * current spacing; use the last explicit VMI, then the normal 1/6-inch
 * default. */
VOID MintPRINTNoteVerticalAdvance(ULONG vmi_216ths)
{
    ULONG vmi = vmi_216ths;

    if (!vmi) vmi = g_text_margin_vmi;
    if (!vmi) vmi = 36UL;
    if (g_text_vertical_units <= 0xffffffffUL - vmi) {
        g_text_vertical_units += vmi;
        ++g_text_vertical_advances;
    }
    mp_log_3("Text vertical advance count/VMI/units",
             (LONG)g_text_vertical_advances, (LONG)vmi,
             (LONG)g_text_vertical_units);
}

VOID MintPRINTResetVerticalAdvances(void)
{
    g_text_vertical_units = 0;
    g_text_vertical_advances = 0;
    mp_log_text("Text vertical position reset");
}

LONG PRT_STDARGS Render(LONG ct, LONG x, LONG y, LONG status, ...)
{
    switch (status) {
        case 5: /* Pre-master initialisation. x = io_Special density flags. */
            g_current_special = (ULONG)x;
            if (PED) {
                /* g_config was loaded in DriverOpen() for this job, so the
                 * user's configured capture resolution is already known
                 * here. 4096x6144 @ 300dpi are the historical bounds; at
                 * 600dpi they are doubled so common paper sizes (e.g. A4
                 * at 600dpi is ~4960x7014 dots) aren't clipped. */
                BOOL hires = (g_config.resolution == 600);
                PED->ped_MaxXDots = hires ? 8192 : 4096;
                PED->ped_MaxYDots = hires ? 12288 : 6144;
                PED->ped_XDotsInch = hires ? 600 : 300;
                PED->ped_YDotsInch = hires ? 600 : 300;
                PED->ped_NumRows = 1;
            }
            mp_log_3("Render pre-master special/maxX/maxY", x,
                     PED ? (LONG)PED->ped_MaxXDots : 0,
                     PED ? (LONG)PED->ped_MaxYDots : 0);
            return PDERR_NOERR;

        case 0: { /* Master initialisation: x/y are final output dimensions. */
            BOOL continuation;
            ULONG encoder_height;
            ULONG media_height = 0;
            ULONG leading_height = 0;
            struct IODRPReq *request = (struct IODRPReq *)ct;
            g_rows_seen = 0;
            g_discard_aux_band = FALSE;
            g_discard_aux_band_has_ink = FALSE;
            g_discard_leading_aux_band = FALSE;
            g_strip_current_band_short = FALSE;
            /* g_config was already loaded once for this job in DriverOpen()
             * - case 5 (which fires before this on the job's first page)
             * needs it that early for the PED resolution fields below. */
            /* ct's meaning here is undocumented in this codebase - logging
             * its raw value (not just whether it's nonzero) in case it
             * turns out to carry a page/band number we've been discarding.
             * See the "multiple Render(0) cycles inside one Open/Close"
             * investigation in the project history around this line. */
            mp_log_3("Render begin width/height/ct", x, y, ct);
            if (request) {
                mp_log_3("IODRP source x/y/width",
                         (LONG)request->io_SrcX,
                         (LONG)request->io_SrcY,
                         (LONG)request->io_SrcWidth);
                mp_log_3("IODRP sourceHeight/destCols/destRows",
                         (LONG)request->io_SrcHeight,
                         request->io_DestCols,
                         request->io_DestRows);
                mp_log_3("IODRP special/modes/command",
                         (LONG)request->io_Special,
                         (LONG)request->io_Modes,
                         (LONG)request->io_Command);
            }

            /* SPECIAL_NOPRINT: a sizing-only pass (RKM) - the app is
             * asking how big its dump would be, not asking for output.
             * Take no action and produce nothing; case 1/4 below check
             * g_sizing_pass and stay inert too. */
            if (g_current_special & SPECIAL_NOPRINT) {
                g_sizing_pass = TRUE;
                mp_log_text("Sizing-only pass (SPECIAL_NOPRINT) - no output");
                return PDERR_NOERR;
            }
            g_sizing_pass = FALSE;

            if (g_runaway_tripped) {
                mp_log_text("Refusing page: runaway tiny-page storm already tripped this print");
                return PDERR_CANCEL;
            }

            /* Some applications issue narrow blank bands before their real
             * page. The Canon trace contained five 4x50 bands before one
             * 4962-pixel-wide page: discarding them removed 250 vertical
             * rows (10.6mm at 600dpi) from the document's top margin. Keep
             * suppressing the impossible-width raster itself, but case 4
             * retains its height until the first normal-width page begins. */
            if (!g_page_pending && mp_engine_supports_strip_accumulation() &&
                (g_current_special & SPECIAL_NOFORMFEED) &&
                mp_is_tiny_leading_auxiliary_band((ULONG)x)) {
                g_discard_aux_band = TRUE;
                g_discard_leading_aux_band = TRUE;
                g_page_width = (ULONG)x;
                g_page_height = (ULONG)y;
                mp_log_3("Ignoring leading tiny NOFORMFEED band width/height/zero",
                         x, y, 0);
                return PDERR_NOERR;
            }

            /* Wordworth can also issue narrow graphics dumps under a page
             * already being accumulated. They cannot be appended as
             * horizontal rows to the full-width PWG raster, so ignore their
             * pixels but retain their height as page-boundary evidence. */
            if (g_page_pending && mp_engine_supports_strip_accumulation() &&
                (g_current_special & SPECIAL_NOFORMFEED) &&
                mp_is_tiny_auxiliary_band(g_accum_width, (ULONG)x)) {
                g_discard_aux_band = TRUE;
                g_page_height = (ULONG)y;
                g_page_had_noformfeed = TRUE;
                g_strip_current_band_short =
                    g_strip_nominal_band_height && (ULONG)y > 0 &&
                    (ULONG)y < g_strip_nominal_band_height;
                mp_log_3("Ignoring tiny NOFORMFEED auxiliary band width/height/pageWidth",
                         x, y, (LONG)g_accum_width);
                return PDERR_NOERR;
            }

            /* Same width as the page currently being accumulated -> this
             * is another band of it, not a new page - matches whatever
             * case 4 decided last time it saw SPECIAL_NOFORMFEED (below). */
            continuation = g_page_pending && mp_engine_supports_strip_accumulation() &&
                           (ULONG)x == g_accum_width;

            if (g_page_pending && !continuation) {
                /* A page was left pending without ever getting a
                 * matching-width continuation - width changed (or the
                 * engine did) without a NOFORMFEED-suppressed close.
                 * Finish it as-is rather than losing it. */
                mp_page_finalize();
            }

            if (continuation) {
                ULONG new_total = g_accum_height + (ULONG)y;
                g_page_width = (ULONG)x;
                g_page_height = (ULONG)y;
                g_strip_current_band_short =
                    g_strip_nominal_band_height && (ULONG)y > 0 &&
                    (ULONG)y < g_strip_nominal_band_height;
                if ((ULONG)y > g_strip_nominal_band_height)
                    g_strip_nominal_band_height = (ULONG)y;
                /* A band that would carry the page past its target isn't
                 * wholly this page's - accepting all of it (the previous
                 * behaviour) silently stole however many rows overshot,
                 * including any of the NEXT page's own leading margin
                 * caught up in the same band, and welded them onto the
                 * bottom of this one. Only reserve up to target here; the
                 * remainder starts a fresh page at the case-1 split point
                 * below once this band's rows actually arrive. */
                if (g_page_target_height &&
                    g_accum_height < g_page_target_height &&
                    new_total > g_page_target_height) {
                    g_split_pending = TRUE;
                    g_split_at_row = g_page_target_height - g_accum_height;
                    if (!mp_job_reserve_page(g_page_target_height))
                        return PDERR_BUFFERMEMORY;
                    mp_log_3("Strip band will split at row/bandHeight/target",
                             (LONG)g_split_at_row, y,
                             (LONG)g_page_target_height);
                    return PDERR_NOERR;
                }
                if (!mp_job_reserve_page(new_total))
                    return PDERR_BUFFERMEMORY;
                g_accum_height = new_total;
                mp_log_3("Strip band appended width/thisHeight/accumHeight",
                         x, y, (LONG)g_accum_height);
                return PDERR_NOERR;
            }

            g_page_width = (ULONG)x;
            g_page_height = (ULONG)y;
            g_recenter_clamped_page = FALSE;
            g_strip_nominal_band_height = (ULONG)y;

            /* printer.device's own page width for a DUMPRPORT source (no
             * SPECIAL_NOFORMFEED - a single-shot page, e.g. MintPrint
             * Settings' test page) has been observed not to match the
             * configured media at all: requesting a 2480px-wide (A4) target
             * via IODRPReq produced a 3287px page here, wide enough that
             * the printer split the job across two physical sheets. This is
             * printer.device's own DUMPRPORT geometry report, not a PWG
             * artifact, so it happens the same way regardless of which
             * engine ends up encoding the page - originally scoped to PWG
             * only (issue #29), a JPEG-engine printer (issue #30: HP
             * OfficeJet 8014e) has since shown the identical oversized
             * width producing a needlessly huge JPEG encode (3287x3508
             * instead of 2480x3508 - ~30% more pixels than the page needs)
             * on real hardware, slow enough to look like a hang. Clamp back
             * down to the configured media's own width on any engine
             * whenever it's both known and substantially (>10%) narrower
             * than what printer.device reported - a real page that's
             * already close to the configured media (the common case) is
             * left untouched. */
            if (!(g_current_special & SPECIAL_NOFORMFEED)) {
                ULONG dpi = g_config.resolution ? g_config.resolution : 300UL;
                /* mp_media_expected_width_px() weighs BOTH reported
                 * dimensions, not width alone - width alone cannot tell an
                 * oversized portrait page (this exact 3287x3508 case) apart
                 * from a genuine landscape one (3508x2480): 3287 lands
                 * closer to a portrait A4's landscape-width (3508) than its
                 * portrait width (2480), so a width-only comparison reads
                 * it as landscape and never clamps it - letting the very
                 * oversized page this clamp exists for straight through.
                 * Bringing g_page_height into it resolves that: the
                 * oversized page's height (3508) still matches portrait
                 * overall, while a real landscape page matches on both
                 * axes at once. See media_size.c/.h and
                 * tests/test_media_size.c for the exact regression. */
                ULONG expected_width_px = (ULONG)mp_media_expected_width_px(
                    g_config.media, g_page_width, g_page_height, dpi);
                if (expected_width_px &&
                    expected_width_px < g_page_width &&
                    (g_page_width - expected_width_px) * 10UL > g_page_width) {
                    mp_log_3("Clamping oversized page width raw/media/dpi",
                             (LONG)g_page_width, (LONG)expected_width_px,
                             (LONG)dpi);
                    g_page_width = expected_width_px;
                    g_recenter_clamped_page = TRUE;
                }
            }

            leading_height = g_leading_aux_height;
            if (g_current_special & SPECIAL_NOFORMFEED) {
                ULONG dpi = g_config.resolution ?
                            g_config.resolution : 300UL;
                ULONG target_height = mp_media_target_height(
                    g_config.media, g_page_width, dpi);
                ULONG vertical_margin = mp_vertical_advance_rows(
                    g_text_vertical_units, dpi);
                ULONG command_margin = mp_text_top_margin_rows(
                    g_text_top_margin_line, g_text_margin_vmi,
                    dpi);

                /* Actual paper movement is the strongest placement signal.
                 * aSTBM is next; Wordsworth's form-length reconstruction is
                 * used only when neither appeared. Reject implausibly large
                 * pre-dump movement so ordinary captured text cannot consume
                 * most of a graphics page. */
                if (vertical_margin && target_height &&
                    vertical_margin <= target_height / 4UL) {
                    command_margin = vertical_margin;
                    mp_log_3("Restoring vertical advance rows/count/units",
                             (LONG)vertical_margin,
                             (LONG)g_text_vertical_advances,
                             (LONG)g_text_vertical_units);
                } else if (!command_margin && g_text_margins_cleared) {
                    command_margin = mp_wordsworth_top_margin_rows(
                        g_text_form_length_lines, target_height,
                        dpi);
                }

                /* A narrow leading control band and aSTBM can describe the
                 * same physical whitespace. Use the larger observation,
                 * never their sum, so applications which provide both do
                 * not get a doubled top margin. */
                if (command_margin > leading_height)
                    leading_height = command_margin;
                if (command_margin > 0)
                    mp_log_3("Restoring top margin rows/line/formLength",
                             (LONG)command_margin,
                             (LONG)g_text_top_margin_line,
                             (LONG)g_text_form_length_lines);
            }
            /* A vertical movement positions only the next new graphics page,
             * never every continuation band. Later pages can supply their
             * own advances; if Wordsworth does not, its form-length fallback
             * above still restores the documented border consistently. */
            g_text_vertical_units = 0;
            g_text_vertical_advances = 0;
            if (leading_height > 0xffffffffUL - g_page_height) {
                mp_log_text("Leading strip whitespace height overflow");
                g_leading_aux_height = 0;
                return PDERR_BUFFERMEMORY;
            }
            encoder_height = leading_height + g_page_height;
            if ((mp_detect_engine(&g_config) == MP_ENGINE_PWG ||
                 mp_detect_engine(&g_config) == MP_ENGINE_URF) &&
                (g_current_special & SPECIAL_NOFORMFEED)) {
                media_height = mp_media_target_height(
                    g_config.media, g_page_width, g_config.resolution);
                /* A physical duplex sheet's two sides must share one
                 * height. Real strip-printed content can legitimately
                 * overshoot the media-derived target on one side (its
                 * bands just don't divide evenly into it) while another
                 * side falls short and only gets padded to the plain
                 * target - a front/back mismatch a real duplex unit can
                 * reject outright. Flooring every page's target at the
                 * tallest page this duplex job has finalized so far keeps
                 * the whole job converging on one height without ever
                 * truncating real content - see g_duplex_max_page_height. */
                if (mp_duplex_requested() &&
                    g_duplex_max_page_height > media_height) {
                    media_height = g_duplex_max_page_height;
                }
                if (media_height > encoder_height) {
                    encoder_height = media_height;
                    mp_log_3("Strip target width/bandHeight/pageHeight",
                             (LONG)g_page_width, (LONG)g_page_height,
                             (LONG)encoder_height);
                }
            }
            if (!mp_job_begin(g_page_width, encoder_height)) {
                if (mp_duplex_requested()) g_duplex_job_failed = TRUE;
                return PDERR_BUFFERMEMORY;
            }

            /* The narrow leading bands carry no usable horizontal raster,
             * but their height is real page geometry. Write that height as
             * white rows before accepting the first full-width content row.
             * Pending bands alone never open a job, so an all-auxiliary
             * sequence still cannot resurrect the old blank-page storm. */
            if (leading_height > 0) {
                if (!mp_job_pad_page(leading_height)) {
                    g_leading_aux_height = 0;
                    if (mp_duplex_requested()) g_duplex_job_failed = TRUE;
                    return PDERR_BUFFERMEMORY;
                }
                mp_log_3("Restored leading whitespace rows/pending/pageHeight",
                         (LONG)leading_height, 0, (LONG)encoder_height);
            }
            g_leading_aux_height = 0;

            /* Every page tentatively starts pending; case 4 is the only
             * place that decides whether to finish now or wait for more
             * bands (SPECIAL_NOFORMFEED), so a normal single-band page
             * (the overwhelming majority) finalizes synchronously inside
             * its own case 4 call exactly as before this change. */
            g_page_pending = TRUE;
            g_accum_width = g_page_width;
            g_accum_height = leading_height + g_page_height;
            g_aux_height = 0;
            g_page_target_height = media_height;
            g_page_had_noformfeed =
                (g_current_special & SPECIAL_NOFORMFEED) ? TRUE : FALSE;

            return PDERR_NOERR;
        }

        case 1: { /* Raster row: ct -> PrtInfo, y = output row number. */
            struct PrtInfo *pi = (struct PrtInfo *)ct;
            ++g_rows_seen;

            if (g_sizing_pass) return PDERR_NOERR; /* SPECIAL_NOPRINT - discard */

            if ((ULONG)y == 0 ||
                (g_page_height && (ULONG)y == (g_page_height / 2)) ||
                (g_page_height && ((ULONG)y + 1) == g_page_height)) {
                mp_log_row(pi, (ULONG)y);
            }

            if (g_discard_aux_band) {
                if (mp_row_has_ink(pi)) g_discard_aux_band_has_ink = TRUE;
                return PDERR_NOERR;
            }

            /* The split point case 0 flagged when this band was announced:
             * row g_split_at_row is the first one that belongs on a new
             * page, not this one. Close the current page out at exactly
             * its target height (no overshoot, so no padding needed
             * either), then start the new page before writing this row -
             * mp_job_write_row() below always writes into whichever
             * encoder is current. */
            if (g_split_pending && (ULONG)y == g_split_at_row) {
                ULONG split_width = g_accum_width;
                ULONG remaining = g_page_height - g_split_at_row;

                g_split_pending = FALSE;
                g_accum_height = g_page_target_height;
                if (g_job_failed || g_job_rows_written != g_accum_height ||
                    !mp_page_finalize()) {
                    g_job_failed = TRUE;
                    if (mp_duplex_requested()) g_duplex_job_failed = TRUE;
                    return PDERR_CANCEL;
                }
                if (!mp_begin_split_page(split_width, remaining))
                    return PDERR_CANCEL;
                mp_log_3("Strip page split rows/newPageHeight/target",
                         (LONG)g_split_at_row, (LONG)remaining,
                         (LONG)g_page_target_height);
            }

            if (!g_job_failed && !mp_job_write_row(pi, (ULONG)y))
                return PDERR_CANCEL;

            return PDERR_NOERR;
        }

        case 2: /* Printer buffer would normally be sent here. */
            return PDERR_NOERR;

        case 3: /* Printer buffer would normally be cleared here. */
            return PDERR_NOERR;

        case 4: /* Close down. ct = final error code. x = io_Special per
                 * the same RKM convention as case 5 - logged raw here
                 * (not just used) to confirm that against real hardware,
                 * since only case 5's x has actually been verified so
                 * far; decisions below use g_current_special (captured at
                 * the case-5 call that preceded this cycle) rather than
                 * trusting this x, until that's confirmed. */
            mp_log_3("Render end ct/special/rows", ct, x, (LONG)g_rows_seen);

            if (g_sizing_pass) {
                g_sizing_pass = FALSE;
                return PDERR_NOERR;
            }

            if (g_discard_aux_band) {
                if (ct != 0) g_job_failed = TRUE;
                if (g_discard_leading_aux_band) {
                    if (ct == 0) {
                        if (g_leading_aux_height >
                            0xffffffffUL - g_page_height)
                            g_leading_aux_height = 0xffffffffUL;
                        else
                            g_leading_aux_height += g_page_height;
                    }
                    mp_log_3(g_discard_aux_band_has_ink
                        ? "Deferred nonblank leading tiny band width/height/pending"
                        : "Deferred blank leading tiny band width/height/pending",
                        (LONG)g_page_width, (LONG)g_page_height,
                        (LONG)g_leading_aux_height);
                } else {
                    mp_log_text(g_discard_aux_band_has_ink
                        ? "Discarded nonblank tiny NOFORMFEED auxiliary band"
                        : "Discarded blank tiny NOFORMFEED auxiliary band");
                }
                if (ct == 0 && !g_discard_leading_aux_band) {
                    if (g_aux_height > 0xffffffffUL - g_page_height)
                        g_aux_height = 0xffffffffUL;
                    else
                        g_aux_height += g_page_height;
                }
                g_discard_aux_band = FALSE;
                g_discard_aux_band_has_ink = FALSE;
                g_discard_leading_aux_band = FALSE;
                if (g_page_pending && !g_job_failed &&
                    g_strip_current_band_short &&
                    mp_short_strip_completes_logical_page(
                        g_accum_height, g_aux_height,
                        g_page_target_height,
                        g_strip_nominal_band_height, g_page_height)) {
                    mp_log_3("Strip logical boundary rows/aux/shortBand",
                             (LONG)g_accum_height, (LONG)g_aux_height,
                             (LONG)g_page_height);
                    return mp_page_finalize() ? PDERR_NOERR : PDERR_CANCEL;
                }
                if (g_page_pending && !g_job_failed && mp_media_page_complete(
                        g_accum_height, g_aux_height,
                        g_page_target_height)) {
                    mp_log_3("Strip media boundary reached rows/aux/target",
                             (LONG)g_accum_height, (LONG)g_aux_height,
                             (LONG)g_page_target_height);
                    return mp_page_finalize() ? PDERR_NOERR : PDERR_CANCEL;
                }
                return PDERR_NOERR;
            }

            if (ct != 0 || g_job_failed ||
                g_job_rows_written != g_accum_height) {
                g_job_failed = TRUE;
            }

            /* SPECIAL_NOFORMFEED: "here's another band of the same page,
             * don't eject/submit yet" (RKM: "multiple graphics dump on a
             * page oriented printer"). Only PWG and URF can stay pending
             * across bands (see mp_job_reserve_page/mp_page_finalize) -
             * JPEG/PDF/PostScript always finalize below exactly as before
             * this change. */
            if (mp_engine_supports_strip_accumulation() &&
                (g_current_special & SPECIAL_NOFORMFEED)) {
                g_page_had_noformfeed = TRUE;
                if (g_strip_current_band_short &&
                    mp_short_strip_completes_logical_page(
                        g_accum_height, g_aux_height,
                        g_page_target_height,
                        g_strip_nominal_band_height, g_page_height)) {
                    mp_log_3("Strip logical boundary rows/aux/shortBand",
                             (LONG)g_accum_height, (LONG)g_aux_height,
                             (LONG)g_page_height);
                    return mp_page_finalize() ? PDERR_NOERR : PDERR_CANCEL;
                }
                if (mp_media_page_complete(g_accum_height, g_aux_height,
                                           g_page_target_height)) {
                    mp_log_3("Strip media boundary reached rows/aux/target",
                             (LONG)g_accum_height, (LONG)g_aux_height,
                             (LONG)g_page_target_height);
                    return mp_page_finalize() ? PDERR_NOERR : PDERR_CANCEL;
                }
                mp_log_text("SPECIAL_NOFORMFEED set - deferring, more bands expected");
                return PDERR_NOERR;
            }

            return mp_page_finalize() ? PDERR_NOERR : PDERR_CANCEL;

        case 6: /* Multi-pass colour change; not used by PCC_YMCB. */
            return PDERR_NOERR;

        default:
            mp_log_3("Render unknown status/x/y", status, x, y);
            return PDERR_NOERR;
    }
}
