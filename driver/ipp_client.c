/*
 * Minimal IPP Print-Job transport for MintPRINT working driver path.
 *
 * Uses bsdsocket.library directly and submits an already-created JPEG with a
 * standards-shaped IPP/1.1 Print-Job request to /ipp/print.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
typedef long ssize_t;
#include <proto/bsdsocket.h>
#include <sys/ioctl.h> /* FIONBIO, for mp_connect_with_timeout() */
#include <errno.h>     /* EINPROGRESS/EWOULDBLOCK, for mp_connect_with_timeout() */

#include "ipp_client.h"
#include "media_size.h"
#include "http_response.h"

/* Bounds on how long a hung/half-responsive printer can hold up the spool
 * Process. Without these, connect() and recv() below block indefinitely -
 * the caller of mp_ipp_print_document()/mp_ipp_query_imageable_margins()
 * (printer.device, on behalf of whatever application is printing) would
 * never get control back. */
#define MP_IPP_CONNECT_TIMEOUT_SECS 8
#define MP_IPP_RECV_TIMEOUT_SECS 20
#define MP_IPP_CONTROL_SEND_TIMEOUT_SECS 20
/* A small-buffer printer may stop draining TCP while it processes or prints
 * data already received. Keep the short control timeout above for headers,
 * but allow a long document to make no upload progress for up to three
 * minutes before treating the connection as stalled (issue #91). */
#define MP_IPP_DOCUMENT_SEND_TIMEOUT_SECS 180

#define MP_IPP_SEND_FAILED 0
#define MP_IPP_SEND_OK 1
#define MP_IPP_SEND_TIMEOUT -1

/* Connect with a bound on how long a dead/unreachable printer can stall the
 * spool Process. Modeled on src/MintPrintSettings.c's mp_connect_with_timeout,
 * minus the GUI message pump (this runs in its own Process, not a Task
 * servicing a Window). Returns 0 on success, -1 on a real connect failure
 * (the stack reported a socket error), -2 specifically when the socket
 * never became writable within timeout_secs at all - callers that want to
 * tell "never got a response to the connection attempt" apart from an
 * ordinary connect failure (connection refused, etc.) can check for -2,
 * as mp_ipp_print_document() does below to report it distinctly. The
 * socket is always left blocking again before returning. */
static int mp_connect_with_timeout(int sockfd, struct sockaddr_in *addr,
                                   int timeout_secs)
{
    long nonblock = 1;
    long block = 0;
    int rc;
    int connect_errno;

    if (IoctlSocket(sockfd, FIONBIO, (char *)&nonblock) < 0) {
        /* Non-blocking mode unavailable on this stack - fall back to a
         * plain blocking connect rather than failing outright. */
        return connect(sockfd, (struct sockaddr *)addr, sizeof(*addr));
    }

    rc = connect(sockfd, (struct sockaddr *)addr, sizeof(*addr));
    connect_errno = (rc < 0) ? Errno() : 0;

    if (rc < 0 && (connect_errno == EINPROGRESS || connect_errno == EWOULDBLOCK)) {
        fd_set wfds, efds;
        struct timeval tv;
        long ready;

        FD_ZERO(&wfds);
        FD_SET(sockfd, &wfds);
        FD_ZERO(&efds);
        FD_SET(sockfd, &efds);
        tv.tv_sec = timeout_secs;
        tv.tv_usec = 0;

        ready = WaitSelect(sockfd + 1, NULL, &wfds, &efds, &tv, NULL);
        if (ready > 0 && (FD_ISSET(sockfd, &wfds) || FD_ISSET(sockfd, &efds))) {
            int so_err = 0;
            socklen_t optlen = sizeof(so_err);
            if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&so_err, &optlen) == 0) {
                rc = (so_err == 0) ? 0 : -1;
            } else {
                /* getsockopt(SO_ERROR) unsupported on this stack - trust
                 * write-readiness alone as success. */
                rc = 0;
            }
        } else {
            /* WaitSelect returned without the socket ever becoming ready -
             * genuinely timed out waiting for a SYN-ACK/RST, not a socket
             * error the stack reported. */
            rc = -2;
        }
    }

    IoctlSocket(sockfd, FIONBIO, (char *)&block);
    return rc;
}

/* recv() with a bound on how long a half-responsive printer (connected, but
 * never sending a full response) can stall the spool Process. Returns the
 * same values recv() would (>0 bytes, 0 on orderly close, <0 on error), but
 * -2 specifically (rather than -1) when nothing at all arrived before
 * timeout_secs elapsed and recv() itself was never even called - callers
 * that want to tell "printer never answered" apart from an ordinary read
 * failure (connection reset, orderly close, ...) can check for -2, as
 * mp_ipp_print_document() does below to report it distinctly. */
static LONG mp_recv_with_timeout(int sockfd, char *buf, ULONG cap,
                                 int timeout_secs)
{
    fd_set rfds;
    struct timeval tv;
    long ready;

    FD_ZERO(&rfds);
    FD_SET(sockfd, &rfds);
    tv.tv_sec = timeout_secs;
    tv.tv_usec = 0;

    ready = WaitSelect(sockfd + 1, &rfds, NULL, NULL, &tv, NULL);
    if (ready == 0) return -2;
    if (ready < 0) return -1;
    return recv(sockfd, buf, (LONG)cap, 0);
}

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
/* extern, not defined here: this file links into both the driver build
 * (driver_core.c owns the real definition, alongside SysBase/DOSBase) and
 * MintPrintSettings.c (which already has its own, from its Query/Discover
 * networking code, predating this file's addition to the GUI build for
 * the Spooler window's Retry/Reprint) - two non-extern definitions of the
 * same global in one link would collide. */
extern struct Library *SocketBase;

BOOL mp_ipp_socket_available(void)
{
    LONG probe_socket;

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) return FALSE;

    probe_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (probe_socket < 0) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
        return FALSE;
    }

    CloseSocket(probe_socket);
    CloseLibrary(SocketBase);
    SocketBase = NULL;
    return TRUE;
}

static ULONG mp_len(const char *s)
{
    ULONG n = 0;
    while (s && s[n]) ++n;
    return n;
}

static int mp_streq(const char *a, const char *b)
{
    ULONG i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i] && a[i] == b[i]) ++i;
    return a[i] == 0 && b[i] == 0;
}

static void mp_copy(char *dst, ULONG cap, const char *src)
{
    ULONG i = 0;
    if (!dst || !cap) return;
    if (!src) src = "";
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static int mp_append(char *dst, ULONG cap, ULONG *pos, const char *src)
{
    ULONG i = 0;
    while (src && src[i]) {
        if (*pos + 1 >= cap) return 0;
        dst[(*pos)++] = src[i++];
    }
    dst[*pos] = 0;
    return 1;
}

static int mp_append_ulong(char *dst, ULONG cap, ULONG *pos, ULONG value)
{
    char tmp[16];
    ULONG n = 0;
    if (value == 0) return mp_append(dst, cap, pos, "0");
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n) {
        char one[2];
        one[0] = tmp[--n]; one[1] = 0;
        if (!mp_append(dst, cap, pos, one)) return 0;
    }
    return 1;
}

/* send() bounded so a printer that accepts the connection and then simply
 * stops reading (TCP window stays full, never drains) cannot stall the
 * spool Process forever. timeout_secs is deliberately caller-selected:
 * control traffic remains tightly bounded, while document data gets enough
 * time for a small-buffer printer to process a page and resume reading.
 * sent_out records real forward progress even when a later wait/send fails. */
static int mp_safe_send(int sock, const UBYTE *buf, ULONG len,
                        int timeout_secs, ULONG *sent_out)
{
    ULONG sent_total = 0;
    long nonblock = 1;
    long block = 0;
    BOOL nonblocking = (IoctlSocket(sock, FIONBIO, (char *)&nonblock) == 0);

    if (sent_out) *sent_out = 0;

    while (sent_total < len) {
        ULONG left = len - sent_total;
        LONG want = (LONG)(left > 4096UL ? 4096UL : left);
        LONG sent;

        if (nonblocking) {
            fd_set wfds;
            struct timeval tv;
            long ready;

            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            tv.tv_sec = timeout_secs;
            tv.tv_usec = 0;

            ready = WaitSelect(sock + 1, NULL, &wfds, NULL, &tv, NULL);
            if (ready <= 0 || !FD_ISSET(sock, &wfds)) {
                IoctlSocket(sock, FIONBIO, (char *)&block);
                if (sent_out) *sent_out = sent_total;
                return ready == 0 ? MP_IPP_SEND_TIMEOUT : MP_IPP_SEND_FAILED;
            }
        }

        sent = send(sock, (char *)(buf + sent_total), want, 0);
        if (sent <= 0) {
            if (nonblocking) IoctlSocket(sock, FIONBIO, (char *)&block);
            if (sent_out) *sent_out = sent_total;
            return MP_IPP_SEND_FAILED;
        }
        sent_total += (ULONG)sent;
    }

    if (nonblocking) IoctlSocket(sock, FIONBIO, (char *)&block);
    if (sent_out) *sent_out = sent_total;
    return MP_IPP_SEND_OK;
}

static int mp_put8(UBYTE *p, ULONG cap, ULONG *off, UBYTE v)
{
    if (*off >= cap) return 0;
    p[(*off)++] = v;
    return 1;
}

static int mp_put16(UBYTE *p, ULONG cap, ULONG *off, UWORD v)
{
    return mp_put8(p, cap, off, (UBYTE)(v >> 8)) &&
           mp_put8(p, cap, off, (UBYTE)(v & 255));
}

static int mp_put32(UBYTE *p, ULONG cap, ULONG *off, ULONG v)
{
    return mp_put8(p, cap, off, (UBYTE)(v >> 24)) &&
           mp_put8(p, cap, off, (UBYTE)(v >> 16)) &&
           mp_put8(p, cap, off, (UBYTE)(v >> 8)) &&
           mp_put8(p, cap, off, (UBYTE)v);
}

static int mp_put_bytes(UBYTE *p, ULONG cap, ULONG *off,
                        const UBYTE *src, ULONG len)
{
    ULONG i;
    if (*off + len > cap) return 0;
    for (i = 0; i < len; ++i) p[(*off)++] = src[i];
    return 1;
}

static int mp_ipp_attr(UBYTE *p, ULONG cap, ULONG *off, UBYTE tag,
                       const char *name, const char *value)
{
    ULONG nl = mp_len(name), vl = mp_len(value);
    if (nl > 65535UL || vl > 65535UL) return 0;
    return mp_put8(p, cap, off, tag) &&
           mp_put16(p, cap, off, (UWORD)nl) &&
           mp_put_bytes(p, cap, off, (const UBYTE *)name, nl) &&
           mp_put16(p, cap, off, (UWORD)vl) &&
           mp_put_bytes(p, cap, off, (const UBYTE *)value, vl);
}

static int mp_ipp_additional_attr(UBYTE *p, ULONG cap, ULONG *off, UBYTE tag,
                                  const char *value)
{
    ULONG vl = mp_len(value);
    if (vl > 65535UL) return 0;
    return mp_put8(p, cap, off, tag) &&
           mp_put16(p, cap, off, 0) &&
           mp_put16(p, cap, off, (UWORD)vl) &&
           mp_put_bytes(p, cap, off, (const UBYTE *)value, vl);
}

static int mp_ipp_enum_attr(UBYTE *p, ULONG cap, ULONG *off,
                            const char *name, ULONG value)
{
    ULONG nl = mp_len(name);
    if (nl > 65535UL) return 0;
    return mp_put8(p, cap, off, 0x23) &&
           mp_put16(p, cap, off, (UWORD)nl) &&
           mp_put_bytes(p, cap, off, (const UBYTE *)name, nl) &&
           mp_put16(p, cap, off, 4) &&
           mp_put32(p, cap, off, value);
}

static int mp_member_name(UBYTE *p, ULONG cap, ULONG *off, const char *name)
{
    ULONG nl = mp_len(name);
    if (!nl || nl > 65535UL) return 0;
    return mp_put8(p, cap, off, 0x4a) &&
           mp_put16(p, cap, off, 0) &&
           mp_put16(p, cap, off, (UWORD)nl) &&
           mp_put_bytes(p, cap, off, (const UBYTE *)name, nl);
}

static int mp_member_keyword(UBYTE *p, ULONG cap, ULONG *off,
                             const char *name, const char *value)
{
    ULONG vl = mp_len(value);
    if (vl > 65535UL) return 0;
    return mp_member_name(p, cap, off, name) &&
           mp_put8(p, cap, off, 0x44) &&
           mp_put16(p, cap, off, 0) &&
           mp_put16(p, cap, off, (UWORD)vl) &&
           mp_put_bytes(p, cap, off, (const UBYTE *)value, vl);
}

static int mp_member_integer(UBYTE *p, ULONG cap, ULONG *off,
                             const char *name, ULONG value)
{
    return mp_member_name(p, cap, off, name) &&
           mp_put8(p, cap, off, 0x21) &&
           mp_put16(p, cap, off, 0) &&
           mp_put16(p, cap, off, 4) &&
           mp_put32(p, cap, off, value);
}

static int mp_collection_begin(UBYTE *p, ULONG cap, ULONG *off,
                               const char *name)
{
    ULONG nl = mp_len(name);
    if (nl > 65535UL) return 0;
    return mp_put8(p, cap, off, 0x34) &&
           mp_put16(p, cap, off, (UWORD)nl) &&
           mp_put_bytes(p, cap, off, (const UBYTE *)name, nl) &&
           mp_put16(p, cap, off, 0);
}

static int mp_nested_collection_begin(UBYTE *p, ULONG cap, ULONG *off)
{
    return mp_put8(p, cap, off, 0x34) &&
           mp_put16(p, cap, off, 0) &&
           mp_put16(p, cap, off, 0);
}

static int mp_collection_end(UBYTE *p, ULONG cap, ULONG *off)
{
    return mp_put8(p, cap, off, 0x37) &&
           mp_put16(p, cap, off, 0) &&
           mp_put16(p, cap, off, 0);
}

static int mp_media_col_attr(UBYTE *p, ULONG cap, ULONG *off,
                             ULONG x, ULONG y, const char *source)
{
    if (!mp_collection_begin(p, cap, off, "media-col")) return 0;
    if (!mp_member_name(p, cap, off, "media-size") ||
        !mp_nested_collection_begin(p, cap, off) ||
        !mp_member_integer(p, cap, off, "x-dimension", x) ||
        !mp_member_integer(p, cap, off, "y-dimension", y) ||
        !mp_collection_end(p, cap, off)) return 0;
    if (source && source[0] &&
        !mp_member_keyword(p, cap, off, "media-source", source)) return 0;
    return mp_collection_end(p, cap, off);
}

static ULONG mp_quality_enum(const char *quality)
{
    if (!quality || !quality[0]) return 0;
    if (quality[0] == '3' && quality[1] == 0) return 3;
    if (quality[0] == '4' && quality[1] == 0) return 4;
    if (quality[0] == '5' && quality[1] == 0) return 5;
    if (quality[0] == 'd' || quality[0] == 'D') return 3;
    if (quality[0] == 'n' || quality[0] == 'N') return 4;
    if (quality[0] == 'h' || quality[0] == 'H') return 5;
    return 0;
}

static ULONG mp_be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static void mp_margin_record(ULONG *value, BOOL *seen, BOOL *ambiguous,
                             ULONG candidate)
{
    if (!*seen) {
        *value = candidate;
        *seen = TRUE;
    } else if (*value != candidate) {
        *ambiguous = TRUE;
    }
}

static BOOL g_margin_cache_valid = FALSE;
static char g_margin_cache_host[MP_CONFIG_HOST_MAX];
static UWORD g_margin_cache_port = 0;
static char g_margin_cache_path[MP_CONFIG_PATH_MAX];
static ULONG g_margin_cache_left = 0;
static ULONG g_margin_cache_right = 0;
static ULONG g_margin_cache_top = 0;
static ULONG g_margin_cache_bottom = 0;
/* Keep network buffers out of the spool Process's deliberately small 8 KiB
 * stack. Config/margin lookup and Print-Job are serialized in that one
 * process, so a single static set is sufficient. */
static UBYTE g_margin_ipp[512];
static char g_margin_uri[192];
static char g_margin_http[512];
static char g_margin_response[4096];

LONG mp_ipp_query_imageable_margins(const struct MPConfig *cfg,
                                    ULONG *left_100mm,
                                    ULONG *right_100mm,
                                    ULONG *top_100mm,
                                    ULONG *bottom_100mm)
{
    int sock = -1;
    struct sockaddr_in addr = {0};
    ULONG io = 0;
    ULONG up = 0;
    ULONG hp = 0;
    ULONG response_used = 0;
    int body_pos = -1;
    int body_len = 0;
    LONG rc = -1;
    ULONG left = 0, right = 0, top = 0, bottom = 0;
    BOOL left_seen = FALSE, right_seen = FALSE;
    BOOL top_seen = FALSE, bottom_seen = FALSE;
    BOOL left_ambiguous = FALSE, right_ambiguous = FALSE;
    BOOL top_ambiguous = FALSE, bottom_ambiguous = FALSE;

    if (left_100mm) *left_100mm = 0;
    if (right_100mm) *right_100mm = 0;
    if (top_100mm) *top_100mm = 0;
    if (bottom_100mm) *bottom_100mm = 0;

    if (!cfg || !cfg->host[0] || cfg->port == 0 || cfg->path[0] != '/')
        return -1;

    if (g_margin_cache_valid &&
        g_margin_cache_port == cfg->port &&
        mp_streq(g_margin_cache_host, cfg->host) &&
        mp_streq(g_margin_cache_path, cfg->path)) {
        if (left_100mm) *left_100mm = g_margin_cache_left;
        if (right_100mm) *right_100mm = g_margin_cache_right;
        if (top_100mm) *top_100mm = g_margin_cache_top;
        if (bottom_100mm) *bottom_100mm = g_margin_cache_bottom;
        return 0;
    }

    g_margin_uri[0] = 0;
    if (!mp_append(g_margin_uri, sizeof(g_margin_uri), &up, "ipp://") ||
        !mp_append(g_margin_uri, sizeof(g_margin_uri), &up, cfg->host)) {
        rc = -2; goto done;
    }
    if (cfg->port != 631 &&
        (!mp_append(g_margin_uri, sizeof(g_margin_uri), &up, ":") ||
         !mp_append_ulong(g_margin_uri, sizeof(g_margin_uri), &up, cfg->port))) {
        rc = -2; goto done;
    }
    if (!mp_append(g_margin_uri, sizeof(g_margin_uri), &up, cfg->path)) {
        rc = -2; goto done;
    }

    /* IPP/1.1 Get-Printer-Attributes, requesting only the four margin
     * descriptions so the response stays tiny even on verbose printers. */
    if (!mp_put8(g_margin_ipp, sizeof(g_margin_ipp), &io, 1) ||
        !mp_put8(g_margin_ipp, sizeof(g_margin_ipp), &io, 1) ||
        !mp_put16(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x000b) ||
        !mp_put32(g_margin_ipp, sizeof(g_margin_ipp), &io, 2) ||
        !mp_put8(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x01) ||
        !mp_ipp_attr(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x47,
                     "attributes-charset", "utf-8") ||
        !mp_ipp_attr(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x48,
                     "attributes-natural-language", "en") ||
        !mp_ipp_attr(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x45,
                     "printer-uri", g_margin_uri) ||
        !mp_ipp_attr(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x44,
                     "requested-attributes", "media-left-margin-supported") ||
        !mp_ipp_additional_attr(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x44,
                                "media-right-margin-supported") ||
        !mp_ipp_additional_attr(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x44,
                                "media-top-margin-supported") ||
        !mp_ipp_additional_attr(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x44,
                                "media-bottom-margin-supported") ||
        !mp_put8(g_margin_ipp, sizeof(g_margin_ipp), &io, 0x03)) {
        rc = -3; goto done;
    }

    g_margin_http[0] = 0;
    if (!mp_append(g_margin_http, sizeof(g_margin_http), &hp, "POST ") ||
        !mp_append(g_margin_http, sizeof(g_margin_http), &hp, cfg->path) ||
        !mp_append(g_margin_http, sizeof(g_margin_http), &hp,
                   " HTTP/1.1\r\nHost: ") ||
        !mp_append(g_margin_http, sizeof(g_margin_http), &hp, cfg->host) ||
        !mp_append(g_margin_http, sizeof(g_margin_http), &hp, ":") ||
        !mp_append_ulong(g_margin_http, sizeof(g_margin_http), &hp, cfg->port) ||
        !mp_append(g_margin_http, sizeof(g_margin_http), &hp,
                   "\r\nContent-Type: application/ipp\r\nContent-Length: ") ||
        !mp_append_ulong(g_margin_http, sizeof(g_margin_http), &hp, io) ||
        !mp_append(g_margin_http, sizeof(g_margin_http), &hp,
                   "\r\nConnection: close\r\n\r\n")) {
        rc = -4; goto done;
    }

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) { rc = -5; goto done; }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { rc = -6; goto done; }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    addr.sin_addr.s_addr = inet_addr((STRPTR)cfg->host);
    if (addr.sin_addr.s_addr == INADDR_NONE) { rc = -7; goto done; }
    if (mp_connect_with_timeout(sock, &addr, MP_IPP_CONNECT_TIMEOUT_SECS) < 0) {
        rc = -8; goto done;
    }

    if (mp_safe_send(sock, (const UBYTE *)g_margin_http, hp,
                     MP_IPP_CONTROL_SEND_TIMEOUT_SECS, NULL) != MP_IPP_SEND_OK ||
        mp_safe_send(sock, g_margin_ipp, io,
                     MP_IPP_CONTROL_SEND_TIMEOUT_SECS, NULL) != MP_IPP_SEND_OK) {
        rc = -9; goto done;
    }

    for (;;) {
        int parsed;
        int status = 0;
        LONG got;

        parsed = mp_http_final_body(g_margin_response, (int)response_used,
                                    &status, &body_pos, &body_len);
        if (parsed == 1) {
            if (status != 200) { rc = -10; goto done; }
            break;
        }
        if (parsed < 0 || response_used >= sizeof(g_margin_response)) {
            rc = -10; goto done;
        }
        got = mp_recv_with_timeout(sock, g_margin_response + response_used,
                                   sizeof(g_margin_response) - response_used,
                                   MP_IPP_RECV_TIMEOUT_SECS);
        if (got <= 0) { rc = -10; goto done; }
        response_used += (ULONG)got;
    }

    if (body_pos < 0 || body_len < 8) { rc = -10; goto done; }
    {
        const UBYTE *body =
            (const UBYTE *)(g_margin_response + body_pos);
        ULONG pos = 8;
        ULONG end = (ULONG)body_len;
        char current_name[48];
        UWORD ipp_status = (UWORD)(((UWORD)body[2] << 8) | body[3]);

        current_name[0] = 0;
        if (ipp_status >= 0x0100) { rc = -11; goto done; }

        while (pos < end) {
            UBYTE tag = body[pos++];
            UWORD name_len;
            UWORD value_len;
            ULONG i;

            if (tag == 0x03) break;
            if (tag <= 0x0f) {
                current_name[0] = 0;
                continue;
            }
            if (pos + 2UL > end) { rc = -12; goto done; }
            name_len = (UWORD)(((UWORD)body[pos] << 8) | body[pos + 1]);
            pos += 2;
            if (pos + (ULONG)name_len + 2UL > end) { rc = -12; goto done; }
            if (name_len) {
                ULONG copy_len = name_len;
                if (copy_len >= sizeof(current_name))
                    copy_len = sizeof(current_name) - 1UL;
                for (i = 0; i < copy_len; ++i)
                    current_name[i] = (char)body[pos + i];
                current_name[copy_len] = 0;
            }
            pos += name_len;
            value_len = (UWORD)(((UWORD)body[pos] << 8) | body[pos + 1]);
            pos += 2;
            if (pos + (ULONG)value_len > end) { rc = -12; goto done; }

            if (tag == 0x21 && value_len == 4) {
                ULONG v = mp_be32(body + pos);
                if (mp_streq(current_name, "media-left-margin-supported"))
                    mp_margin_record(&left, &left_seen, &left_ambiguous, v);
                else if (mp_streq(current_name, "media-right-margin-supported"))
                    mp_margin_record(&right, &right_seen, &right_ambiguous, v);
                else if (mp_streq(current_name, "media-top-margin-supported"))
                    mp_margin_record(&top, &top_seen, &top_ambiguous, v);
                else if (mp_streq(current_name, "media-bottom-margin-supported"))
                    mp_margin_record(&bottom, &bottom_seen, &bottom_ambiguous, v);
            }
            pos += value_len;
        }
    }

    if (!left_seen || left_ambiguous) left = 0;
    if (!right_seen || right_ambiguous) right = 0;
    if (!top_seen || top_ambiguous) top = 0;
    if (!bottom_seen || bottom_ambiguous) bottom = 0;

    g_margin_cache_left = left;
    g_margin_cache_right = right;
    g_margin_cache_top = top;
    g_margin_cache_bottom = bottom;
    g_margin_cache_port = cfg->port;
    mp_copy(g_margin_cache_host, sizeof(g_margin_cache_host), cfg->host);
    mp_copy(g_margin_cache_path, sizeof(g_margin_cache_path), cfg->path);
    g_margin_cache_valid = TRUE;

    if (left_100mm) *left_100mm = left;
    if (right_100mm) *right_100mm = right;
    if (top_100mm) *top_100mm = top;
    if (bottom_100mm) *bottom_100mm = bottom;
    rc = 0;

done:
    if (sock >= 0) CloseSocket(sock);
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
    return rc;
}

static LONG mp_file_size(BPTR fh)
{
    LONG end;
    if (!fh) return -1;
    if (Seek(fh, 0, OFFSET_END) == -1) return -1;
    end = Seek(fh, 0, OFFSET_CURRENT);
    if (end < 0) return -1;
    if (Seek(fh, 0, OFFSET_BEGINNING) == -1) return -1;
    return end;
}


LONG mp_ipp_capture_request(const struct MPConfig *cfg,
                            CONST_STRPTR document_format,
                            CONST_STRPTR document_filename)
{
    static UBYTE ipp[1024];
    static char uri[192];
    static char output_path[MP_CONFIG_CAPTURE_PATH_MAX + 8];
    ULONG io = 0;
    ULONG up = 0;
    ULONG op = 0;
    BPTR fh;

    if (!DOSBase || !cfg || !cfg->host[0] || cfg->port == 0 ||
        cfg->path[0] != '/' || !document_format || !document_filename)
        return -1;

    uri[0] = 0;
    if (!mp_append(uri, sizeof(uri), &up, "ipp://") ||
        !mp_append(uri, sizeof(uri), &up, cfg->host))
        return -2;
    if (cfg->port != 631 &&
        (!mp_append(uri, sizeof(uri), &up, ":") ||
         !mp_append_ulong(uri, sizeof(uri), &up, cfg->port)))
        return -2;
    if (!mp_append(uri, sizeof(uri), &up, cfg->path))
        return -2;

    if (!mp_put8(ipp, sizeof(ipp), &io, 1) ||
        !mp_put8(ipp, sizeof(ipp), &io, 1) ||
        !mp_put16(ipp, sizeof(ipp), &io, 0x0002) ||
        !mp_put32(ipp, sizeof(ipp), &io, 1) ||
        !mp_put8(ipp, sizeof(ipp), &io, 0x01) ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x47,
                     "attributes-charset", "utf-8") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x48,
                     "attributes-natural-language", "en") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x45, "printer-uri", uri) ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x42,
                     "requesting-user-name", "Amiga") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x42,
                     "job-name", "MintPRINT AmigaOS") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x49,
                     "document-format", (const char *)document_format))
        return -3;

    if (cfg->media[0] || cfg->source[0] || cfg->color[0] || cfg->quality[0] ||
        cfg->scaling[0] || cfg->sides[0]) {
        ULONG quality_enum = mp_quality_enum(cfg->quality);
        ULONG media_x = 0, media_y = 0;
        int use_media_col = cfg->media[0] && cfg->source[0] &&
                            mp_media_dimensions_100mm(cfg->media,
                                                      &media_x, &media_y);

        if (!mp_put8(ipp, sizeof(ipp), &io, 0x02)) return -3;
        if (use_media_col) {
            if (!mp_media_col_attr(ipp, sizeof(ipp), &io, media_x, media_y,
                                   cfg->source)) return -3;
        } else if (cfg->media[0] &&
                   !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44,
                                "media", cfg->media)) return -3;
        if (cfg->color[0] &&
            !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44,
                         "print-color-mode", cfg->color)) return -3;
        if (quality_enum &&
            !mp_ipp_enum_attr(ipp, sizeof(ipp), &io,
                              "print-quality", quality_enum)) return -3;
        if (cfg->scaling[0] &&
            !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44,
                         "print-scaling", cfg->scaling)) return -3;
        if (cfg->sides[0] &&
            !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44,
                         "sides", cfg->sides)) return -3;
    }
    if (!mp_put8(ipp, sizeof(ipp), &io, 0x03)) return -3;

    output_path[0] = 0;
    if (!mp_append(output_path, sizeof(output_path), &op,
                   (const char *)document_filename) ||
        !mp_append(output_path, sizeof(output_path), &op, ".ipp"))
        return -4;

    fh = Open((CONST_STRPTR)output_path, MODE_NEWFILE);
    if (!fh) return -5;
    if (Write(fh, ipp, (LONG)io) != (LONG)io) {
        Close(fh);
        return -6;
    }
    Close(fh);
    return 0;
}

LONG mp_ipp_print_document(const struct MPConfig *cfg, CONST_STRPTR filename,
                           CONST_STRPTR document_format,
                           struct MPIPPResult *result)
{
    BPTR fh = 0;
    LONG fsize;
    int sock = -1;
    struct sockaddr_in addr = {0};
    static UBYTE ipp[1024];
    ULONG io = 0;
    static char uri[192];
    ULONG up = 0;
    static char http[512];
    ULONG hp = 0;
    static UBYTE filebuf[8192];
    static char response[8192];
    ULONG response_used = 0;
    int body_pos = -1;
    int body_len = 0;
    LONG rc = -1;

    if (result) {
        result->error = -1;
        result->http_status = 0;
        result->ipp_status = 0xffff;
        result->document_bytes = 0;
        result->document_bytes_sent = 0;
    }
    if (!DOSBase || !cfg || !cfg->host[0] || cfg->port == 0 ||
        cfg->path[0] != '/' || !filename || !document_format) return -1;

    fh = Open(filename, MODE_OLDFILE);
    if (!fh) { rc = -2; goto done; }
    fsize = mp_file_size(fh);
    if (fsize <= 0) { rc = -3; goto done; }
    if (result) result->document_bytes = (ULONG)fsize;

    /* 631 is the default/implied port for the ipp:// scheme and is safe to
     * omit; any other port must be stated explicitly, or this URI silently
     * claims a printer on 631 while the connection below actually goes
     * elsewhere (the Host: header and the real connect() already get the
     * port right - only this attribute value was missing it). */
    uri[0] = 0;
    if (!mp_append(uri, sizeof(uri), &up, "ipp://") ||
        !mp_append(uri, sizeof(uri), &up, cfg->host)) {
        rc = -4; goto done;
    }
    if (cfg->port != 631 &&
        (!mp_append(uri, sizeof(uri), &up, ":") ||
         !mp_append_ulong(uri, sizeof(uri), &up, cfg->port))) {
        rc = -4; goto done;
    }
    if (!mp_append(uri, sizeof(uri), &up, cfg->path)) {
        rc = -4; goto done;
    }

    /* IPP/1.1, Print-Job (0x0002), request-id 1. */
    if (!mp_put8(ipp, sizeof(ipp), &io, 1) ||
        !mp_put8(ipp, sizeof(ipp), &io, 1) ||
        !mp_put16(ipp, sizeof(ipp), &io, 0x0002) ||
        !mp_put32(ipp, sizeof(ipp), &io, 1) ||
        !mp_put8(ipp, sizeof(ipp), &io, 0x01) ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x47, "attributes-charset", "utf-8") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x48, "attributes-natural-language", "en") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x45, "printer-uri", uri) ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x42, "requesting-user-name", "Amiga") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x42, "job-name", "MintPRINT AmigaOS") ||
        !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x49, "document-format", (const char *)document_format)) {
        rc = -5; goto done;
    }

    /* Optional Unit0 job-template attributes. Empty values preserve the
     * already-proven minimal Print-Job path.  A tray/source choice is
     * encoded correctly inside media-col rather than as a top-level
     * media-source attribute. */
    if (cfg->media[0] || cfg->source[0] || cfg->color[0] || cfg->quality[0] ||
        cfg->scaling[0] || cfg->sides[0]) {
        ULONG quality_enum = mp_quality_enum(cfg->quality);
        ULONG media_x = 0, media_y = 0;
        int use_media_col = cfg->media[0] && cfg->source[0] &&
                            mp_media_dimensions_100mm(cfg->media,
                                                      &media_x, &media_y);

        if (!mp_put8(ipp, sizeof(ipp), &io, 0x02)) { rc = -5; goto done; }
        if (use_media_col) {
            if (!mp_media_col_attr(ipp, sizeof(ipp), &io, media_x, media_y,
                                   cfg->source))
                { rc = -5; goto done; }
        } else if (cfg->media[0] &&
                   !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44, "media", cfg->media))
            { rc = -5; goto done; }
        if (cfg->color[0] &&
            !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44, "print-color-mode", cfg->color))
            { rc = -5; goto done; }
        if (quality_enum &&
            !mp_ipp_enum_attr(ipp, sizeof(ipp), &io, "print-quality", quality_enum))
            { rc = -5; goto done; }
        if (cfg->scaling[0] &&
            !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44, "print-scaling", cfg->scaling))
            { rc = -5; goto done; }
        if (cfg->sides[0] &&
            !mp_ipp_attr(ipp, sizeof(ipp), &io, 0x44, "sides", cfg->sides))
            { rc = -5; goto done; }
    }

    if (!mp_put8(ipp, sizeof(ipp), &io, 0x03)) {
        rc = -5; goto done;
    }

    http[0] = 0;
    if (!mp_append(http, sizeof(http), &hp, "POST ") ||
        !mp_append(http, sizeof(http), &hp, cfg->path) ||
        !mp_append(http, sizeof(http), &hp, " HTTP/1.1\r\nHost: ") ||
        !mp_append(http, sizeof(http), &hp, cfg->host) ||
        !mp_append(http, sizeof(http), &hp, ":") ||
        !mp_append_ulong(http, sizeof(http), &hp, cfg->port) ||
        !mp_append(http, sizeof(http), &hp, "\r\nContent-Type: application/ipp\r\nContent-Length: ") ||
        !mp_append_ulong(http, sizeof(http), &hp, io + (ULONG)fsize) ||
        !mp_append(http, sizeof(http), &hp, "\r\nConnection: close\r\n\r\n")) {
        rc = -6; goto done;
    }

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) { rc = -7; goto done; }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { rc = -8; goto done; }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    addr.sin_addr.s_addr = inet_addr((STRPTR)cfg->host);
    if (addr.sin_addr.s_addr == INADDR_NONE) { rc = -9; goto done; }
    {
        int connect_rc = mp_connect_with_timeout(sock, &addr,
                                                 MP_IPP_CONNECT_TIMEOUT_SECS);
        /* -18, not -10: distinguishes "printer never answered the
         * connection attempt at all within MP_IPP_CONNECT_TIMEOUT_SECS"
         * from an ordinary connect failure (connection refused, no route),
         * so driver_core.c's log can name it specifically - same idea as
         * -17 for the recv() timeout below. */
        if (connect_rc == -2) { rc = -18; goto done; }
        if (connect_rc < 0) { rc = -10; goto done; }
    }

    if (mp_safe_send(sock, (const UBYTE *)http, hp,
                     MP_IPP_CONTROL_SEND_TIMEOUT_SECS, NULL) != MP_IPP_SEND_OK ||
        mp_safe_send(sock, ipp, io,
                     MP_IPP_CONTROL_SEND_TIMEOUT_SECS, NULL) != MP_IPP_SEND_OK) {
        rc = -11; goto done;
    }

    for (;;) {
        LONG got;
        ULONG chunk_sent = 0;
        int send_rc;

        got = Read(fh, filebuf, sizeof(filebuf));
        if (got < 0) { rc = -12; goto done; }
        if (got == 0) break;

        send_rc = mp_safe_send(sock, filebuf, (ULONG)got,
                               MP_IPP_DOCUMENT_SEND_TIMEOUT_SECS,
                               &chunk_sent);
        if (result) result->document_bytes_sent += chunk_sent;
        if (send_rc != MP_IPP_SEND_OK) {
            rc = send_rc == MP_IPP_SEND_TIMEOUT ? -19 : -13;
            goto done;
        }
    }

    for (;;) {
        int parsed;
        int status = 0;
        LONG got;

        parsed = mp_http_final_body(response, (int)response_used, &status,
                                    &body_pos, &body_len);
        if (parsed == 1) {
            if (result) result->http_status = status;
            break;
        }
        if (parsed < 0 || response_used >= sizeof(response)) {
            rc = -14;
            goto done;
        }
        got = mp_recv_with_timeout(sock, response + response_used,
                                   sizeof(response) - response_used,
                                   MP_IPP_RECV_TIMEOUT_SECS);
        /* -17, not -14: distinguishes "printer never sent anything within
         * MP_IPP_RECV_TIMEOUT_SECS" (e.g. issue #66 - a printer that
         * accepts the job, then silently hangs) from the other -14 causes
         * (malformed/oversized response, connection reset) so
         * driver_core.c's log can name it specifically. */
        if (got == -2) { rc = -17; goto done; }
        if (got <= 0) { rc = -14; goto done; }
        response_used += (ULONG)got;
    }
    if (body_pos < 0 || body_len < 8) { rc = -14; goto done; }

    if (result) {
        const UBYTE *ipp_response = (const UBYTE *)(response + body_pos);
        result->ipp_status = (UWORD)(((UWORD)ipp_response[2] << 8) |
                                     (UWORD)ipp_response[3]);
    }

    if (result && result->http_status != 200) { rc = -15; goto done; }
    if (result && result->ipp_status >= 0x0100) { rc = -16; goto done; }

    rc = 0;

done:
    if (sock >= 0) CloseSocket(sock);
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
    if (fh) Close(fh);
    if (result) result->error = rc;
    return rc;
}
