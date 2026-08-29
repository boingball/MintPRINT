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
 * built the same DDMMYYHHMMSS-less-than-set-clock candidate collides
 * with the last one, so every job after the first gets the next free
 * suffix). Giving up after 99 collisions and reusing that last candidate
 * (silently overwriting it) is an acceptable last resort - reaching that
 * many same-second jobs in practice is not realistic. */
static void mp_spool_proc_unique_name(const char *candidate, char *resolved,
                                      ULONG cap)
{
    ULONG n;
    BPTR test;

    mp_spool_copy_bounded(resolved, cap, candidate);

    for (n = 0; n <= 99; ++n) {
        if (n > 0)
            mp_spool_build_suffixed_name(candidate, n, resolved, cap);
        test = Lock((CONST_STRPTR)resolved, ACCESS_READ);
        if (!test) return; /* name is free */
        UnLock(test);
    }
}

/* Single-sided documents submit one Print-Job per page (see driver_core.c's
 * mp_page_submit_and_track) as soon as each page finishes rendering. A real
 * printer (Canon TS8300) is still busy engine-printing page N when page
 * N+1's Print-Job request arrives and correctly rejects it with IPP
 * server-error-busy (0x0507) rather than queuing it - so without a retry,
 * only the first page of a one-sided job ever prints. Duplex jobs never hit
 * this because they accumulate every page into one job file and submit
 * once, at DriverClose. Retry only this specific, well-defined "try again
 * later" status; any other error status is left to fail immediately as
 * before. */
#define MP_IPP_STATUS_SERVER_BUSY 0x0507
#define MP_IPP_BUSY_RETRY_LIMIT 30
#define MP_IPP_BUSY_RETRY_TICKS 50 /* Delay() ticks, ~1 second */

static LONG mp_spool_proc_ipp_submit_retrying(const struct MPConfig *cfg,
                                              CONST_STRPTR filename,
                                              CONST_STRPTR document_format,
                                              struct MPIPPResult *result)
{
    LONG rc;
    ULONG attempt = 0;

    for (;;) {
        rc = mp_ipp_print_document(cfg, filename, document_format, result);
        if (rc != -16 || !result ||
            result->ipp_status != MP_IPP_STATUS_SERVER_BUSY ||
            ++attempt >= MP_IPP_BUSY_RETRY_LIMIT)
            break;
        if (cfg && cfg->debug)
            mp_spool_proc_append_log(
                "MintPRINT: IPP server busy, retrying page submission");
        Delay(MP_IPP_BUSY_RETRY_TICKS);
    }
    return rc;
}

static LONG mp_spool_entry(void)
{
    struct MsgPort *port;
    struct MsgPort *startup_reply_to = g_spool_startup_port;
    struct MPSpoolMsg *quit_msg = NULL;
    BOOL running = TRUE;
    BPTR job_fh = 0;
    BPTR aux_fh = 0;

    /* This is a real AmigaDOS Process, unlike an arbitrary caller of a
     * printer.device callback. Probe bsdsocket here so socket() is never
     * invoked from a potentially bare Task. If the probe fails, leave port
     * NULL; the normal startup handshake below wakes Init(), which then
     * rejects the driver cleanly before raster generation starts. */
    port = NULL;
    if (mp_ipp_socket_available())
        port = CreateMsgPort();
    if (port) {
        port->mp_Node.ln_Name = (char *)MP_SPOOL_PORT_NAME;
        port->mp_Node.ln_Pri = 0;
        Forbid();
        AddPort(port);
        Permit();
    }

    /* Tell the parent we're ready (or that we failed) either way - it is
     * blocked in WaitPort() waiting for exactly this. */
    if (startup_reply_to) {
        g_spool_ready_msg.mn_Node.ln_Type = NT_MESSAGE;
        g_spool_ready_msg.mn_Length = sizeof(g_spool_ready_msg);
        g_spool_ready_msg.mn_ReplyPort = NULL;
        PutMsg(startup_reply_to, &g_spool_ready_msg);
    }

    if (!port) return 0;

    while (running) {
        struct MPSpoolMsg *m;

        WaitPort(port);
        while ((m = (struct MPSpoolMsg *)GetMsg(port)) != NULL) {
            switch (m->cmd) {
                case MP_SPOOL_CMD_LOG:
                    mp_spool_proc_append_log(m->text);
                    m->result = 0;
                    break;

                case MP_SPOOL_CMD_JOB_OPEN:
                    mp_spool_proc_close_job(&job_fh);
                    job_fh = Open(m->filename, MODE_NEWFILE);
                    m->result = job_fh ? 0 : -1;
                    break;

                case MP_SPOOL_CMD_JOB_OPEN_UNIQUE: {
                    char resolved[MP_SPOOL_UNIQUE_NAME_MAX];

                    mp_spool_proc_close_job(&job_fh);
                    mp_spool_proc_unique_name(m->filename, resolved,
                                              sizeof(resolved));
                    job_fh = Open((CONST_STRPTR)resolved, MODE_NEWFILE);
                    if (job_fh && m->out_buf)
                        mp_spool_copy_bounded(m->out_buf, m->out_cap,
                                              resolved);
                    m->result = job_fh ? 0 : -1;
                    break;
                }

                case MP_SPOOL_CMD_STATUS_WRITE: {
                    BPTR st = Open(m->filename, MODE_NEWFILE);
                    if (st) {
                        if (m->data && m->length)
                            Write(st, (APTR)m->data, (LONG)m->length);
                        Close(st);
                        m->result = 0;
                    } else {
                        m->result = -1;
                    }
                    break;
                }

                case MP_SPOOL_CMD_JOB_WRITE:
                    if (job_fh && m->data && m->length) {
                        ULONG done = 0;
                        m->result = 0;
                        while (done < m->length) {
                            LONG n = Write(job_fh, (APTR)(m->data + done),
                                          (LONG)(m->length - done));
                            if (n <= 0) { m->result = -1; break; }
                            done += (ULONG)n;
                        }
                    } else {
                        m->result = -1;
                    }
                    break;

                case MP_SPOOL_CMD_JOB_PATCH:
                    if (job_fh && m->data && m->length &&
                        Seek(job_fh, (LONG)m->offset, OFFSET_BEGINNING) != -1) {
                        LONG n = Write(job_fh, (APTR)m->data, (LONG)m->length);
                        m->result = (n == (LONG)m->length) ? 0 : -1;
                        Seek(job_fh, 0, OFFSET_END);
                    } else {
                        m->result = -1;
                    }
                    break;

                case MP_SPOOL_CMD_JOB_CLOSE:
                    mp_spool_proc_close_job(&job_fh);
                    m->result = 0;
                    break;

                case MP_SPOOL_CMD_JOB_DELETE:
                    m->result = DeleteFile(m->filename) ? 0 : -1;
                    break;

                case MP_SPOOL_CMD_AUX_OPEN:
                    mp_spool_proc_close_job(&aux_fh);
                    aux_fh = Open(m->filename, MODE_NEWFILE);
                    m->result = aux_fh ? 0 : -1;
                    break;

                case MP_SPOOL_CMD_AUX_WRITE:
                    if (aux_fh && m->data && m->length) {
                        ULONG done = 0;
                        m->result = 0;
                        while (done < m->length) {
                            LONG n = Write(aux_fh, (APTR)(m->data + done),
                                          (LONG)(m->length - done));
                            if (n <= 0) { m->result = -1; break; }
                            done += (ULONG)n;
                        }
                    } else {
                        m->result = -1;
                    }
                    break;

                case MP_SPOOL_CMD_AUX_READ:
                    if (aux_fh && m->data && m->length &&
                        Seek(aux_fh, (LONG)m->offset, OFFSET_BEGINNING) != -1) {
                        LONG n = Read(aux_fh, (APTR)m->data, (LONG)m->length);
                        m->result = (n == (LONG)m->length) ? 0 : -1;
                    } else {
                        m->result = -1;
                    }
                    break;

                case MP_SPOOL_CMD_AUX_CLOSE:
                    mp_spool_proc_close_job(&aux_fh);
                    m->result = 0;
                    break;

                case MP_SPOOL_CMD_IPP_SUBMIT:
                    m->result = mp_spool_proc_ipp_submit_retrying(
                                     m->cfg, m->filename, m->document_format,
                                     &m->ipp_result);
                    break;

                case MP_SPOOL_CMD_CONFIG_LOAD:
                    m->result = mp_config_load(m->cfg_io);
                    break;

                case MP_SPOOL_CMD_QUIT:
                default:
                    /*
                     * Do not wake the shutdown caller yet.
                     *
                     * This Process is executing code from the MintPRINT
                     * driver segment.  If we ReplyMsg() here, classic
                     * printer.device can continue through CloseDevice(),
                     * call Expunge(), and unload that segment while this
                     * Process is still executing its teardown.
                     *
                     * Keep the caller blocked until the public spool port
                     * has been removed/deleted.  The final reply is sent as
                     * the last useful action before this entry function
                     * returns to dos.library's process startup code.
                     */
                    mp_spool_proc_close_job(&job_fh);
                    mp_spool_proc_close_job(&aux_fh);
                    m->result = 0;
                    quit_msg = m;
                    running = FALSE;
                    break;
            }

            if (m != quit_msg)
                ReplyMsg((struct Message *)m);

            if (!running)
                break;
        }
    }

    Forbid();
    RemPort(port);
    Permit();
    DeleteMsgPort(port);

    /*
     * The worker is created above the normal caller's priority.  Once this
     * reply wakes CloseDevice()/Expunge, the worker therefore keeps the CPU
     * long enough to complete the tiny return epilogue and leave the driver
     * segment before the caller can unload it.
     */
    if (quit_msg)
        ReplyMsg((struct Message *)quit_msg);

    return 0;
}

/* ------------------------------------------------------------------- *
 * Client side. Runs in the caller's context - exec.library only, never
 * dos.library (CreateNewProc is the one confirmed-safe exception: it is
 * exec/dos.library's own supported way to bootstrap a process from a
 * plain Task).
 * ------------------------------------------------------------------- */

static struct MsgPort *mp_spool_find_port(void)
{
    struct MsgPort *p;
    Forbid();
    p = FindPort(MP_SPOOL_PORT_NAME);
    Permit();
    return p;
}

BOOL mp_spool_ensure_running(void)
{
    struct MsgPort *startup_port;

    g_spool_port = mp_spool_find_port();
    if (g_spool_port) return TRUE;

    startup_port = CreateMsgPort();
    if (!startup_port) return FALSE;

    g_spool_startup_port = startup_port;

    /* CreateNewProc()'s undocumented-to-first-glance danger: any of these
     * tags left unspecified default to "duplicate the CALLING process's own
     * field" (current dir, home dir, console task, window pointer) - which
     * means dereferencing pr_CurrentDir/pr_HomeDir/pr_ConsoleTask/
     * pr_WindowPtr on whatever FindTask(NULL) returns, cast straight to
     * struct Process* with no check that it actually is one. From a bare
     * Task (exactly the DPaint background-print case this file exists for)
     * those fields do not exist, so the "duplicate from caller" default is
     * itself an unsafe dos.library access - pin every one of them
     * explicitly so CreateNewProc never reads anything from the caller. */
    if (!CreateNewProcTags(NP_Entry, (ULONG)mp_spool_entry,
                       NP_StackSize, 8192L,
                       NP_Name, (ULONG)"MintPRINT spool",
                       /* See deferred QUIT reply in mp_spool_entry(). */
                       NP_Priority, 1L,
                       NP_CurrentDir, 0L,
                       NP_Input, 0L,
                       NP_CloseInput, FALSE,
                       NP_Output, 0L,
                       NP_CloseOutput, FALSE,
                       NP_ConsoleTask, 0L,
                       NP_WindowPtr, -1L,
                       TAG_DONE)) {
        g_spool_startup_port = NULL;
        DeleteMsgPort(startup_port);
        return FALSE;
    }

    WaitPort(startup_port);
    GetMsg(startup_port);

    g_spool_startup_port = NULL;
    DeleteMsgPort(startup_port);

    g_spool_port = mp_spool_find_port();
    return g_spool_port != NULL;
}

static BOOL mp_spool_send(struct MPSpoolMsg *m)
{
    struct MsgPort *reply_port;

    if (!g_spool_port && !mp_spool_ensure_running()) return FALSE;
    if (!g_spool_port) return FALSE;

    reply_port = CreateMsgPort();
    if (!reply_port) return FALSE;

    m->mn.mn_Node.ln_Type = NT_MESSAGE;
    m->mn.mn_Length = sizeof(*m);
    m->mn.mn_ReplyPort = reply_port;

    PutMsg(g_spool_port, (struct Message *)m);
    WaitPort(reply_port);
    GetMsg(reply_port);

    DeleteMsgPort(reply_port);

    return m->result == 0;
}

void mp_spool_log(const char *line)
{
    struct MPSpoolMsg m;
    if (!line) return;
    m.cmd = MP_SPOOL_CMD_LOG;
    m.text = line;
    mp_spool_send(&m); /* best-effort: a lost log line never fails the job */
}

BOOL mp_spool_job_open(CONST_STRPTR filename)
{
    struct MPSpoolMsg m;
    m.cmd = MP_SPOOL_CMD_JOB_OPEN;
    m.filename = filename;
    return mp_spool_send(&m);
}

BOOL mp_spool_job_open_unique(CONST_STRPTR candidate, char *resolved_out,
                              ULONG resolved_cap)
{
    struct MPSpoolMsg m;
    if (resolved_out && resolved_cap) resolved_out[0] = 0;
    m.cmd = MP_SPOOL_CMD_JOB_OPEN_UNIQUE;
    m.filename = candidate;
    m.out_buf = resolved_out;
    m.out_cap = resolved_cap;
    return mp_spool_send(&m);
}

void mp_spool_status_write(CONST_STRPTR filename, const char *text,
                           ULONG length)
{
    struct MPSpoolMsg m;
    if (!filename) return;
    m.cmd = MP_SPOOL_CMD_STATUS_WRITE;
    m.filename = filename;
    m.data = (const UBYTE *)text;
    m.length = length;
    mp_spool_send(&m); /* best-effort: a lost status update never fails the job */
}

BOOL mp_spool_job_write(const UBYTE *data, ULONG length)
{
    struct MPSpoolMsg m;
    m.cmd = MP_SPOOL_CMD_JOB_WRITE;
    m.data = data;
    m.length = length;
    return mp_spool_send(&m);
}

BOOL mp_spool_job_patch(ULONG offset, const UBYTE *data, ULONG length)
{
    struct MPSpoolMsg m;
    m.cmd = MP_SPOOL_CMD_JOB_PATCH;
    m.offset = offset;
    m.data = data;
    m.length = length;
    return mp_spool_send(&m);
}

void mp_spool_job_close(void)
{
    struct MPSpoolMsg m;
    m.cmd = MP_SPOOL_CMD_JOB_CLOSE;
    mp_spool_send(&m);
}

void mp_spool_job_delete(CONST_STRPTR filename)
{
    struct MPSpoolMsg m;
    m.cmd = MP_SPOOL_CMD_JOB_DELETE;
    m.filename = filename;
    mp_spool_send(&m);
}

BOOL mp_spool_aux_open(CONST_STRPTR filename)
{
    struct MPSpoolMsg m;
    m.cmd = MP_SPOOL_CMD_AUX_OPEN;
    m.filename = filename;
    return mp_spool_send(&m);
}

BOOL mp_spool_aux_write(const UBYTE *data, ULONG length)
{
    struct MPSpoolMsg m;
    m.cmd = MP_SPOOL_CMD_AUX_WRITE;
    m.data = data;
    m.length = length;
    return mp_spool_send(&m);
}

BOOL mp_spool_aux_read(ULONG offset, UBYTE *data, ULONG length)
{
    struct MPSpoolMsg m;
    m.cmd = MP_SPOOL_CMD_AUX_READ;
    m.offset = offset;
    m.data = data;
    m.length = length;
    return mp_spool_send(&m);
}

void mp_spool_aux_close(void)
{
    struct MPSpoolMsg m;
    m.cmd = MP_SPOOL_CMD_AUX_CLOSE;
    mp_spool_send(&m);
}

LONG mp_spool_ipp_submit(const struct MPConfig *cfg, CONST_STRPTR filename,
                         CONST_STRPTR document_format,
                         struct MPIPPResult *result)
{
    struct MPSpoolMsg m;

    m.cmd = MP_SPOOL_CMD_IPP_SUBMIT;
    m.cfg = cfg;
    m.filename = filename;
    m.document_format = document_format;
    m.ipp_result.error = -1;
    m.ipp_result.http_status = 0;
    m.ipp_result.ipp_status = 0xffff;
    m.ipp_result.document_bytes = 0;
    m.result = -1;

    mp_spool_send(&m); /* m.result carries mp_ipp_print_document()'s return */

    if (result) *result = m.ipp_result;
    return m.result;
}

LONG mp_spool_config_load(struct MPConfig *cfg)
{
    struct MPSpoolMsg m;

    if (!cfg) return MP_CONFIG_SOURCE_DEFAULTS;

    /* Task-safe fallback: pure memory, no dos.library. Overwritten below if
     * the spool process is reachable and does the real ENV:/ENVARC: load. */
    mp_config_defaults(cfg);

    m.cmd = MP_SPOOL_CMD_CONFIG_LOAD;
    m.cfg_io = cfg;
    /* mp_spool_send()'s BOOL return means "result==0", but 0 is itself a
     * valid MP_CONFIG_SOURCE_* (defaults) here, not a failure code - so use
     * a sentinel to tell "never delivered" apart from "delivered, and the
     * spool process reported defaults were used". */
    m.result = -1;
    mp_spool_send(&m);

    return (m.result >= 0) ? m.result : MP_CONFIG_SOURCE_DEFAULTS;
}

void mp_spool_shutdown(void)
{
    struct MPSpoolMsg m;
    if (!g_spool_port) g_spool_port = mp_spool_find_port();
    if (!g_spool_port) return;
    m.cmd = MP_SPOOL_CMD_QUIT;
    mp_spool_send(&m);
    g_spool_port = NULL;
}
