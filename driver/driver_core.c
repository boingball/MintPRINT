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
/* Some strip