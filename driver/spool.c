/*
 * See spool.h for why this exists. Everything in this file that touches
 * dos.library (Open/Write/Seek/Close/DeleteFile, mp_ipp_print_document
 * which itself reopens the finished job file, and mp_config_load which
 * reads ENV:/ENVARC:) runs only inside
 * mp_spool_entry() - the body of a dedicated Process this file spawns via
 * CreateNewProc(). Every other function here runs in whatever context the
 * caller (driver_core.c's Init/DriverOpen/DriverClose/Render, therefore
 * whatever task the calling application used) gives it, and touches only
 * exec.library primitives, which are safe from any Task.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "spool.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define MP_SPOOL_PORT_NAME ((CONST_STRPTR)"MintPRINT.spool")

enum {
    MP_SPOOL_CMD_LOG = 1,
    MP_SPOOL_CMD_JOB_OPEN,
    MP_SPOOL_CMD_JOB_OPEN_UNIQUE,
    MP_SPOOL_CMD_JOB_WRITE,
    MP_SPOOL_CMD_JOB_PATCH,
    MP_SPOOL_CMD_JOB_CLOSE,
    MP_SPOOL_CMD_JOB_DELETE,
    MP_SPOOL_CMD_AUX_OPEN,
    MP_SPOOL_CMD_AUX_WRITE,
    MP_SPOOL_CMD_AUX_READ,
    MP_SPOOL_CMD_AUX_CLOSE,
    MP_SPOOL_CMD_IPP_SUBMIT,
    MP_SPOOL_CMD_CONFIG_LOAD,
    MP_SPOOL_CMD_STATUS_WRITE,
    MP_SPOOL_CMD_QUIT
};

struct MPSpoolMsg {
    struct Message mn;
    UWORD cmd;
    CONST_STRPTR filename;
    const UBYTE *data;
    ULONG length;
    ULONG offset;
    const char *text;
    const struct MPConfig *cfg;
    struct MPConfig *cfg_io;
    CONST_STRPTR document_format;
    struct MPIPPResult ipp_result;
    /* Only read/written by MP_SPOOL_CMD_JOB_OPEN_UNIQUE: the name it
     * actually opened (candidate, or candidate with a "-N" collision
     * suffix inserted), copied out for the caller's own bookkeeping. */
    char *out_buf;
    ULONG out_cap;
    LONG result;
};

static struct MsgPort *g_spool_port = NULL;
static struct MsgPort *g_spool_startup_port = NULL;
static struct Message g_spool_ready_msg;

/* ------------------------------------------------------------------- *
 * Spool process body. Everything below this point up to mp_spool_entry
 * itself runs ONLY inside the spool process.
 * ------------------------------------------------------------------- */

static void mp_spool_proc_append_log(const char *text)
{
    BPTR fh;
    ULONG len;

    if (!DOSBase || !text) return;

    fh = Open((CONST_STRPTR)"T:MintPRINT-driver.log", MODE_READWRITE);
    if (!fh) fh = Open((CONST_STRPTR)"T:MintPRINT-driver.log", MODE_NEWFILE);
    if (!fh) return;

    Seek(fh, 0, OFFSET_END);
    for (len = 0; text[len]; ++len) { /* count */ }
    if (len) Write(fh, (APTR)text, (LONG)len);
    Write(fh, (APTR)"\n", 1);
    Close(fh);
}

static void mp_spool_proc_close_job(BPTR *job_fh)
{
    if (*job_fh) {
        Close(*job_fh);
        *job_fh = 0;
    }
}

#define MP_SPOOL_UNIQUE_NAME_MAX 192

static void mp_spool_civil_from_days(ULONG z, UWORD *y, UWORD *m, UWORD *d)
{
    ULONG era, doe, yoe, doy, mp;
    z += 719468UL;
    era = z / 146097UL;
    doe = z - era * 146097UL;
    yoe = (doe - doe / 1460UL + doe / 36524UL - doe / 146096UL) / 365UL;
    doy = doe - (365UL * yoe + yoe / 4UL - yoe / 100UL);
    mp = (5UL * doy + 2UL) / 153UL;
    *d = (UWORD)(doy - (153UL * mp + 2UL) / 5UL + 1UL);
    *m = (UWORD)(mp + (mp < 10UL ? 3UL : (ULONG)(-9)));
    *y = (UWORD)(yoe + era * 400UL + (*m <= 2 ? 1UL : 0UL));
}

/* Called only by the spool Process: DateStamp is a dos.library call and
 * must not run in printer.device callbacks, which may originate in a bare
 * Exec Task. */
static void mp_spool_timestamp(char *out)
{
    struct DateStamp ds;
    UWORD year, month, day, hour, minute, second, yy;

    DateStamp(&ds);
    mp_spool_civil_from_days((ULONG)ds.ds_Days + 2922UL,
                             &year, &month, &day);
    hour = (UWORD)(ds.ds_Minute / 60);
    minute = (UWORD)(ds.ds_Minute % 60);
    second = (UWORD)(ds.ds_Tick / 50);
    yy = (UWORD)(year % 100U);
    out[0] = (char)('0' + (day / 10) % 10); out[1] = (char)('0' + day % 10);
    out[2] = (char)('0' + (month / 10) % 10); out[3] = (char)('0' + month % 10);
    out[4] = (char)('0' + (yy / 10) % 10); out[5] = (char)('0' + yy % 10);
    out[6] = (char)('0' + (hour / 10) % 10); out[7] = (char)('0' + hour % 10);
    out[8] = (char)('0' + (minute / 10) % 10); out[9] = (char)('0' + minute % 10);
    out[10] = (char)('0' + (second / 10) % 10); out[11] = (char)('0' + second % 10);
    out[12] = 0;
}

static void mp_spool_insert_suffix(const char *src, const char *suffix,
                                    char *dst, ULONG cap)
{
    ULONG len = 0, dot, i, j;
    while (src[len]) ++len;
    dot = len;
    for (i = len; i > 0; --i) {
        if (src[i - 1] == '.') { dot = i - 1; break; }
    }
    i = 0;
    for (j = 0; j < dot && i + 1 < cap; ++j, ++i) dst[i] = src[j];
    if (i + 1 < cap) dst[i++] = '-';
    for (j = 0; suffix[j] && i + 1 < cap; ++j, ++i) dst[i] = suffix[j];
    for (j = dot; j < len && i + 1 < cap; ++j, ++i) dst[i] = src[j];
    dst[i] = 0;
}

/* Copies src into dst (bounded by cap, always NUL-terminated) - a plain
 * loop rather than strncpy(): this file has no libc, same as every other
 * translation unit here (see driver_core.c's mp_streq() comment for why). */
static void mp_spool_copy_bounded(char *dst, ULONG cap, const char *src)
{
    ULONG i = 0;
    if (!dst || !cap) return;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

/* Inserts "-<n>" (n = 1..99) immediately before the last '.' in
 * candidate (or at candidate's own end, if it has no '.'), into buf
 * (bounded by cap). Only called once mp_spool_proc_unique_name() below
 * has found candidate itself already names an existing file. */
static void mp_spool_build_suffixed_name(const char *candidate, ULONG n,
                                         char *buf, ULONG cap)
{
    ULONG len = 0, dot, i, j;
    char digits[3];
    ULONG ndigits = 0;

    while (candidate[len]) ++len;
    dot = len;
    for (i = len; i > 0; --i) {
        if (candidate[i - 1] == '.') { dot = i - 1; break; }
    }

    if (n >= 10) digits[ndigits++] = (char)('0' + (n / 10));
    digits[ndigits++] = (char)('0' + (n % 10));

    i = 0;
    for (j = 0; j < dot && i + 1 < cap; ++j, ++i) buf[i] = candidate[j];
    if (i + 1 < cap) buf[i++] = '-';
    for (j = 0; j < ndigits && i + 1 < cap; ++j, ++i) buf[i] = digits[j];
    for (j = dot; j < len && i + 1 < cap; ++j, ++i) buf[i] = candidate[j];
    buf[i] = 0;
}

/* Resolves candidate to a name free of any existing file, trying
 * candidate itself first, then "-1", "-2", ... up to "-99" - the same
 * scheme a machine with no real-time clock relies on entirely (every job
 * built the same candidate, so every job after the first gets the next free
 * suffix). Exhaustion is reported to the caller; an existing job is never
 * silently overwritten. */
static BOOL mp_spool_proc_unique_name(const char *candidate, char *resolved,
                                      ULONG cap)
{
    ULONG n;
    BPTR test;

    mp_spool_copy_bounded(resolved, cap, candidate);

    for (n = 0; n <= 99; ++n) {
        if (n > 0)
            mp_spool_build_suffixed_name(candidate, n, resolved, cap);
        test = Lock((CONST_STRPTR)resolved, ACCESS_READ);
        if (!test) return TRUE; /* name is free */
        UnLock(test);
    }

    return FALSE;
}

/* Single-sided documents submit one Print-Job per page (see driver_core.c's
 * mp_page_submit_and_track) as soon as each page finishes rendering. A real
 * printer (Canon TS8300) is still busy engine-printing page N when page
 * N+1's Print-Job request arrives and correctly rejects it with IPP
 * server-error-busy (0x0507) rather than queuing it - so without a retry,
 * only the first page of a one-sided job ever prints. Duplex jobs never hit
 * this because they accumulate every page into one job file and submit
 * once, at DriverClose. Retry only this specific, well-defined "try again
 * later" status; any other error status is left to 