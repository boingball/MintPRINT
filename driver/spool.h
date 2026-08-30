#ifndef MINTPRINT_SPOOL_H
#define MINTPRINT_SPOOL_H

/*
 * dos.library functions may only be called from a Process, not a bare
 * Exec Task - and printer.device explicitly documents that a driver's
 * callbacks (Init/DriverOpen/DriverClose/Render) run "in the context of
 * the requesting task", i.e. whatever task/process the calling
 * application used to open printer.device. Well-behaved AmigaDOS
 * programs are Processes themselves, so this works fine for most
 * callers - but at least one real caller (DPaint, printing via a
 * background Task rather than its own Process) hits this directly:
 * every dos.library call this driver made from inside those callbacks
 * (Open/Write/Seek/Close for the log and job spool files) was a
 * correctness bug waiting for the wrong kind of caller, surfacing as an
 * unpredictable CPU exception (illegal instruction, address error, ...)
 * from memory corruption rather than a clean, diagnosable error.
 *
 * The fix: one small dedicated Process, spawned lazily via
 * CreateNewProc() (itself an exec-callable bootstrap, safe from a Task),
 * owns every dos.library call the driver needs - the log file, the job
 * spool file, and IPP submission (which also reads the finished job file
 * back off disk). The driver's callbacks, still running in whatever
 * context the caller gave them, only ever touch exec.library primitives
 * (CreateMsgPort/PutMsg/WaitPort/GetMsg/DeleteMsgPort, all Task-safe) to
 * hand a request to that process and block for its reply - never dos.library
 * directly.
 */

#include <exec/types.h>
#include "config.h"
#include "ipp_client.h"

BOOL mp_spool_ensure_running(void);
void mp_spool_shutdown(void);

/* Best-effort: logging never blocks the job on failure. */
void mp_spool_log(const char *line);

/* Runs mp_config_load() (ENV:/ENVARC: Open/FGets/Close) inside the spool
 * process; on failure to reach the spool process cfg is still left holding
 * plain in-memory defaults (mp_config_defaults() touches no dos.library
 * call, so that fallback is always Task-safe). Returns the MP_CONFIG_SOURCE_*
 * that was actually loaded. */
LONG mp_spool_config_load(struct MPConfig *cfg);

BOOL mp_spool_job_open(CONST_STRPTR filename);
BOOL mp_spool_job_write(const UBYTE *data, ULONG length);
void mp_spool_job_close(void);
void mp_spool_job_delete(CONST_STRPTR filename);

/* Resolves `candidate` (a full desired path, e.g.
 * "DH0:MPSPOOL/MintPRINT-job-290826172011.jpg") to a name that does not
 * already exist on disk - first adding the current DOS timestamp inside
 * the spool Process, then inserting "-1", "-2", ... immediately before
 * the last '.' if needed. It fails after 99 collisions rather than
 * overwriting an existing job. The resolved name is copied into
 * resolved_out (bounded by resolved_cap, always left NUL-terminated) so
 * the caller's own g_job_file_* buffer and any status sidecar use the
 * name that was actually opened. Only used when a Spooler HDD location
 * is keeping multiple named jobs - see driver_core.c's mp_job_begin(). */
BOOL mp_spool_job_open_unique(CONST_STRPTR candidate, char *resolved_out,
                              ULONG resolved_cap);

/* Writes a small status sidecar file - state (RENDERING/SUBMITTING/DONE/
 * FAILED) plus error detail, see driver_core.c's mp_spool_write_status()
 * - in one shot: open, write, close. Best-effort, like mp_spool_log()
 * above - a lost status update never fails the job itself. */
void mp_spool_status_write(CONST_STRPTR filename, const char *text,
                           ULONG length);

/* Rewrites length bytes at byte offset `offset` (from the start of the
 * currently-open job file), then seeks back to the end so a following
 * mp_spool_job_write() continues appending exactly where it left off.
 * Used to keep a PWG page header's PageSize and raster height fields in
 * sync with the final media-sized strip page - see driver_core.c's
 * strip-printing accumulation. */
BOOL mp_spool_job_patch(ULONG offset, const UBYTE *data, ULONG length);

/* A second store used only while compressed duplex backside rows must be
 * reordered. It keeps the main PWG stream open while rows are read back in
 * the printer's native coordinate order. */
BOOL mp_spool_aux_open(CONST_STRPTR filename);
BOOL mp_spool_aux_write(const UBYTE *data, ULONG length);
BOOL mp_spool_aux_read(ULONG offset, UBYTE *data, ULONG length);
void mp_spool_aux_close(void);

/* Runs mp_ipp_print_document() inside the spool process, since it also
 * needs dos.library to reopen and read back the finished job file. */
LONG mp_spool_ipp_submit(const struct MPConfig *cfg, CONST_STRPTR filename,
                         CONST_STRPTR document_format,
                         struct MPIPPResult *result);

#endif
