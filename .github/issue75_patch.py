#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text, old, new, label):
    if old not in text:
        raise SystemExit(f"missing patch anchor: {label}")
    if text.count(old) != 1:
        raise SystemExit(f"patch anchor not unique ({text.count(old)}): {label}")
    return text.replace(old, new, 1)

# ---------------------------------------------------------------------------
# Platform-independent mDNS endpoint parser.  Keep the DNS parsing out of the
# already-large GadTools source so it can be host-tested without an Amiga SDK.
# ---------------------------------------------------------------------------
mdns_h = r'''#ifndef MINTPRINT_MDNS_ENDPOINT_H
#define MINTPRINT_MDNS_ENDPOINT_H

#include <stddef.h>

#define MP_MDNS_NAME_MAX 256
#define MP_MDNS_PATH_MAX 128
#define MP_MDNS_LABEL_MAX 96

struct MPMdnsEndpoint {
    char instance[MP_MDNS_NAME_MAX];
    char path[MP_MDNS_PATH_MAX];
    char label[MP_MDNS_LABEL_MAX];
    int port;
    int is_ipp;
};

/* Parse PTR/SRV/TXT records from one mDNS response.  The endpoint structure
 * is deliberately cumulative: callers may feed the initial PTR response and
 * later SRV/TXT detail responses into the same object. */
int mp_mdns_parse_endpoint(const unsigned char *packet, size_t packet_len,
                           struct MPMdnsEndpoint *endpoint);

#endif
'''

mdns_c = r'''#include "mdns_endpoint.h"

#include <string.h>

#define MP_DNS_TYPE_PTR 12U
#define MP_DNS_TYPE_TXT 16U
#define MP_DNS_TYPE_SRV 33U

static unsigned short mp_u16(const unsigned char *p)
{
    return (unsigned short)(((unsigned short)p[0] << 8) | p[1]);
}

static int mp_ascii_equal_nocase(const char *a, const char *b)
{
    unsigned char ca, cb;
    if (!a || !b) return 0;
    while (*a && *b) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int mp_ascii_ends_nocase(const char *text, const char *suffix)
{
    size_t tl, sl;
    if (!text || !suffix) return 0;
    tl = strlen(text);
    sl = strlen(suffix);
    if (sl > tl) return 0;
    return mp_ascii_equal_nocase(text + tl - sl, suffix);
}

static void mp_copy(char *dst, size_t size, const char *src)
{
    size_t n;
    if (!dst || size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int mp_dns_name(const unsigned char *packet, size_t packet_len,
                       size_t *offset, char *out, size_t out_size)
{
    size_t pos, next, used;
    unsigned int jumps = 0;
    int jumped = 0;

    if (!packet || !offset || !out || out_size == 0) return 0;
    pos = *offset;
    next = pos;
    used = 0;
    out[0] = '\0';

    while (pos < packet_len) {
        unsigned char len = packet[pos];
        if (len == 0) {
            if (!jumped) next = pos + 1;
            out[used] = '\0';
            *offset = next;
            return 1;
        }
        if ((len & 0xc0U) == 0xc0U) {
            size_t ptr;
            if (pos + 1 >= packet_len) return 0;
            ptr = (size_t)(((unsigned int)(len & 0x3fU) << 8) | packet[pos + 1]);
            if (ptr >= packet_len || ++jumps > 24U) return 0;
            if (!jumped) { next = pos + 2; jumped = 1; }
            pos = ptr;
            continue;
        }
        if ((len & 0xc0U) != 0 || len > 63U) return 0;
        ++pos;
        if (pos + len > packet_len) return 0;
        if (used) {
            if (used + 1 >= out_size) return 0;
            out[used++] = '.';
        }
        if (used + len >= out_size) return 0;
        memcpy(out + used, packet + pos, len);
        used += len;
        pos += len;
        if (!jumped) next = pos;
    }
    return 0;
}

static int mp_name_relevant(const char *owner, const struct MPMdnsEndpoint *ep)
{
    if (!owner || !ep) return 0;
    if (ep->instance[0] && mp_ascii_equal_nocase(owner, ep->instance)) return 1;
    return mp_ascii_ends_nocase(owner, "._ipp._tcp.local") ||
           mp_ascii_equal_nocase(owner, "_ipp._tcp.local");
}

static void mp_txt(struct MPMdnsEndpoint *ep,
                   const unsigned char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t item_len, key_len, value_len;
        const unsigned char *item, *eq;
        char value[MP_MDNS_NAME_MAX];

        item_len = data[off++];
        if (item_len == 0) continue;
        if (off + item_len > len) break;
        item = data + off;
        eq = (const unsigned char *)memchr(item, '=', item_len);
        if (eq) {
            key_len = (size_t)(eq - item);
            value_len = item_len - key_len - 1;
            if (value_len >= sizeof(value)) value_len = sizeof(value) - 1;
            memcpy(value, eq + 1, value_len);
            value[value_len] = '\0';

            if (key_len == 2 && (item[0] == 'r' || item[0] == 'R') &&
                (item[1] == 'p' || item[1] == 'P')) {
                if (value[0]) {
                    if (value[0] == '/') mp_copy(ep->path, sizeof(ep->path), value);
                    else {
                        ep->path[0] = '/';
                        mp_copy(ep->path + 1, sizeof(ep->path) - 1, value);
                    }
                }
            } else if (key_len == 2 && (item[0] == 't' || item[0] == 'T') &&
                       (item[1] == 'y' || item[1] == 'Y')) {
                if (value[0]) mp_copy(ep->label, sizeof(ep->label), value);
            }
        }
        off += item_len;
    }
}

int mp_mdns_parse_endpoint(const unsigned char *packet, size_t packet_len,
                           struct MPMdnsEndpoint *ep)
{
    unsigned int qd, an, ns, ar, total, i;
    size_t off;
    int relevant = 0;

    if (!packet || !ep || packet_len < 12) return 0;
    qd = mp_u16(packet + 4);
    an = mp_u16(packet + 6);
    ns = mp_u16(packet + 8);
    ar = mp_u16(packet + 10);
    total = an + ns + ar;
    off = 12;

    for (i = 0; i < qd; ++i) {
        char ignored[MP_MDNS_NAME_MAX];
        if (!mp_dns_name(packet, packet_len, &off, ignored, sizeof(ignored))) return 0;
        if (off + 4 > packet_len) return 0;
        off += 4;
    }

    for (i = 0; i < total; ++i) {
        char owner[MP_MDNS_NAME_MAX];
        unsigned int type;
        size_t rdata, end;
        unsigned int rdlen;

        if (!mp_dns_name(packet, packet_len, &off, owner, sizeof(owner))) return relevant;
        if (off + 10 > packet_len) return relevant;
        type = mp_u16(packet + off);
        rdlen = mp_u16(packet + off + 8);
        off += 10;
        rdata = off;
        end = off + rdlen;
        if (end > packet_len) return relevant;

        if (type == MP_DNS_TYPE_PTR &&
            mp_ascii_equal_nocase(owner, "_ipp._tcp.local")) {
            size_t p = rdata;
            char target[MP_MDNS_NAME_MAX];
            if (mp_dns_name(packet, packet_len, &p, target, sizeof(target))) {
                mp_copy(ep->instance, sizeof(ep->instance), target);
                ep->is_ipp = 1;
                relevant = 1;
            }
        } else if (type == MP_DNS_TYPE_SRV && rdlen >= 6 &&
                   mp_name_relevant(owner, ep)) {
            ep->port = (int)mp_u16(packet + rdata + 4);
            ep->is_ipp = 1;
            relevant = 1;
        } else if (type == MP_DNS_TYPE_TXT && mp_name_relevant(owner, ep)) {
            mp_txt(ep, packet + rdata, rdlen);
            ep->is_ipp = 1;
            relevant = 1;
        }

        off = end;
    }
    return relevant;
}
'''

mdns_test = r'''#include "mdns_endpoint.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t put_name(unsigned char *p, size_t off, const char *name)
{
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t n = dot ? (size_t)(dot - s) : strlen(s);
        p[off++] = (unsigned char)n;
        memcpy(p + off, s, n);
        off += n;
        if (!dot) break;
        s = dot + 1;
    }
    p[off++] = 0;
    return off;
}

static void u16(unsigned char *p, size_t off, unsigned int v)
{
    p[off] = (unsigned char)(v >> 8);
    p[off + 1] = (unsigned char)v;
}

static size_t rr_hdr(unsigned char *p, size_t off, const char *owner,
                     unsigned int type, unsigned int rdlen)
{
    off = put_name(p, off, owner);
    u16(p, off, type); off += 2;
    u16(p, off, 1); off += 2;
    p[off++] = p[off++] = p[off++] = p[off++] = 0;
    u16(p, off, rdlen); off += 2;
    return off;
}

int main(void)
{
    unsigned char p[1024];
    size_t off = 12, start, rdlen_pos, rstart;
    struct MPMdnsEndpoint ep;
    const char *instance = "EPSON XP-345._ipp._tcp.local";
    const char txt1[] = "rp=ipp/print";
    const char txt2[] = "ty=EPSON XP-345";

    memset(p, 0, sizeof(p));
    p[7] = 3; /* ANCOUNT */

    start = off;
    off = put_name(p, off, "_ipp._tcp.local");
    u16(p, off, 12); off += 2; u16(p, off, 1); off += 2;
    off += 4; rdlen_pos = off; off += 2; rstart = off;
    off = put_name(p, off, instance);
    u16(p, rdlen_pos, (unsigned int)(off - rstart));
    (void)start;

    off = rr_hdr(p, off, instance, 33, 6);
    p[off++] = p[off++] = p[off++] = p[off++] = 0;
    u16(p, off, 631); off += 2;

    off = put_name(p, off, instance);
    u16(p, off, 16); off += 2; u16(p, off, 1); off += 2;
    off += 4; rdlen_pos = off; off += 2; rstart = off;
    p[off++] = (unsigned char)strlen(txt1); memcpy(p + off, txt1, strlen(txt1)); off += strlen(txt1);
    p[off++] = (unsigned char)strlen(txt2); memcpy(p + off, txt2, strlen(txt2)); off += strlen(txt2);
    u16(p, rdlen_pos, (unsigned int)(off - rstart));

    memset(&ep, 0, sizeof(ep));
    assert(mp_mdns_parse_endpoint(p, off, &ep));
    assert(ep.is_ipp);
    assert(ep.port == 631);
    assert(strcmp(ep.path, "/ipp/print") == 0);
    assert(strcmp(ep.label, "EPSON XP-345") == 0);
    assert(strcmp(ep.instance, instance) == 0);
    puts("mDNS endpoint parser tests passed");
    return 0;
}
'''

(ROOT / "src/mdns_endpoint.h").write_text(mdns_h)
(ROOT / "src/mdns_endpoint.c").write_text(mdns_c)
(ROOT / "tests/test_mdns_endpoint.c").write_text(mdns_test)

# ---------------------------------------------------------------------------
# Wire the parser into MintPrintSettings discovery.
# ---------------------------------------------------------------------------
p = ROOT / "src/MintPrintSettings.c"
s = p.read_text()
s = replace_once(s, '#include "ipp_enum.h"\n#include "lodepng.h"',
                 '#include "ipp_enum.h"\n#include "mdns_endpoint.h"\n#include "lodepng.h"', 'mdns include')

s = replace_once(s,
'''struct DiscoveredPrinter {
    char ip[16];
    char label[80];
};''',
'''struct DiscoveredPrinter {
    char ip[16];
    char label[80];
    char ipp_path[MP_MDNS_PATH_MAX];
    char mdns_instance[MP_MDNS_NAME_MAX];
    int ipp_port;
    BOOL mdns_details_asked;
};''', 'discovered printer fields')

start_marker = '''/* ---------------------------------------------------------------------
 * LAN printer discovery (mDNS / Bonjour / AirPrint)'''
end_marker = '''/* Runs both discovery mechanisms and merges the results: SSDP catches'''
start = s.index(start_marker)
end = s.index(end_marker, start)
new_mdns = r'''/* ---------------------------------------------------------------------
 * LAN printer discovery (mDNS / Bonjour / AirPrint)
 *
 * Issue #75 exposed the limit of the original deliberately-minimal mDNS
 * pass: it remembered only the source IP and discarded DNS-SD's SRV port
 * and TXT rp= resource path.  That is enough for common printers at
 * 631:/ipp/print, but not for older/quirkier AirPrint devices whose actual
 * endpoint is explicitly advertised.  Parse just the DNS-SD records needed
 * for IPP and ask for SRV/TXT details when the initial PTR response does not
 * include them.  The byte parser lives in mdns_endpoint.c so it is host-
 * tested independently of bsdsocket/GadTools.
 * ------------------------------------------------------------------- */
static int build_mdns_name_query(unsigned char *buf, int buf_size,
                                 const char *name, unsigned int qtype) {
    static const unsigned char header[12] = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    const char *label;
    int off;

    if (!buf || !name || buf_size < 32) return 0;
    memcpy(buf, header, sizeof(header));
    off = (int)sizeof(header);
    label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        int len = dot ? (int)(dot - label) : (int)strlen(label);
        if (len <= 0 || len > 63 || off + len + 6 >= buf_size) return 0;
        buf[off++] = (unsigned char)len;
        memcpy(buf + off, label, (size_t)len);
        off += len;
        if (!dot) break;
        label = dot + 1;
    }
    buf[off++] = 0;
    buf[off++] = (unsigned char)((qtype >> 8) & 0xffU);
    buf[off++] = (unsigned char)(qtype & 0xffU);
    buf[off++] = 0x80; buf[off++] = 0x01; /* IN + QU */
    return off;
}

static int discovery_find_ip(struct DiscoveredPrinter *results, int count,
                             const char *ip) {
    int i;
    for (i = 0; i < count; ++i)
        if (strcmp(results[i].ip, ip) == 0) return i;
    return -1;
}

static void mdns_send_detail_query(int sockfd, struct sockaddr_in *dest,
                                   struct DiscoveredPrinter *result) {
    unsigned char query[384];
    int len;

    if (!result || result->mdns_details_asked || !result->mdns_instance[0])
        return;
    result->mdns_details_asked = TRUE;

    len = build_mdns_name_query(query, sizeof(query), result->mdns_instance, 33U);
    if (len > 0)
        sendto(sockfd, (char *)query, len, 0,
               (struct sockaddr *)dest, sizeof(*dest));
    len = build_mdns_name_query(query, sizeof(query), result->mdns_instance, 16U);
    if (len > 0)
        sendto(sockfd, (char *)query, len, 0,
               (struct sockaddr *)dest, sizeof(*dest));
}

static int mdns_discover_printers(struct DiscoveredPrinter *results, int count_io, int max_results) {
    int sockfd;
    struct sockaddr_in dest;
    unsigned char query[96];
    int query_len;
    unsigned char *buf;
    int count = count_io;
    int poll_num;
    const int max_polls = 10;

    if (count >= max_results) return count;
    query_len = build_mdns_name_query(query, sizeof(query),
                                      "_ipp._tcp.local", 12U);
    if (query_len <= 0) {
        printf("Discovery: could not build mDNS query\n");
        return count;
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        printf("Discovery: could not create mDNS socket\n");
        return count;
    }
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(5353);
    dest.sin_addr.s_addr = inet_addr((STRPTR)"224.0.0.251");

    if (sendto(sockfd, (char *)query, query_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        printf("Discovery: mDNS send failed (no route to 224.0.0.251?)\n");
        CloseSocket(sockfd);
        return count;
    }

    buf = malloc(1500);
    if (!buf) { CloseSocket(sockfd); return count; }

    for (poll_num = 0; poll_num < max_polls; ++poll_num) {
        fd_set readfds;
        struct timeval tv;
        long ready;
        struct sockaddr_in from;
        socklen_t fromlen;
        ssize_t received;
        char ipstr[16];
        const unsigned char *addr_bytes;
        int idx;
        struct MPMdnsEndpoint ep;

        if (window) {
            struct IntuiMessage *imsg;
            while ((imsg = GT_GetIMsg(window->UserPort))) GT_ReplyIMsg(imsg);
        }

        FD_ZERO(&readfds); FD_SET(sockfd, &readfds);
        tv.tv_sec = 0; tv.tv_usec = 500000;
        ready = WaitSelect(sockfd + 1, &readfds, NULL, NULL, &tv, NULL);
        if (ready <= 0) continue;

        fromlen = sizeof(from);
        memset(&from, 0, sizeof(from));
        received = recvfrom(sockfd, (char *)buf, 1500, 0,
                            (struct sockaddr *)&from, &fromlen);
        if (received < 12 || from.sin_port != htons(5353)) continue;

        addr_bytes = (const unsigned char *)&from.sin_addr;
        if (addr_bytes[0] == 127) continue;
        snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u",
                 addr_bytes[0], addr_bytes[1], addr_bytes[2], addr_bytes[3]);
        idx = discovery_find_ip(results, count, ipstr);

        memset(&ep, 0, sizeof(ep));
        if (idx >= 0) {
            strncpy(ep.instance, results[idx].mdns_instance, sizeof(ep.instance) - 1);
            strncpy(ep.path, results[idx].ipp_path, sizeof(ep.path) - 1);
            ep.port = results[idx].ipp_port;
        }
        if (!mp_mdns_parse_endpoint(buf, (size_t)received, &ep) || !ep.is_ipp)
            continue;

        if (idx < 0) {
            if (count >= max_results) continue;
            idx = count++;
            memset(&results[idx], 0, sizeof(results[idx]));
            strncpy(results[idx].ip, ipstr, sizeof(results[idx].ip) - 1);
            strcpy(results[idx].ipp_path, "/ipp/print");
            results[idx].ipp_port = 631;
        }

        if (ep.instance[0]) {
            strncpy(results[idx].mdns_instance, ep.instance,
                    sizeof(results[idx].mdns_instance) - 1);
            results[idx].mdns_instance[sizeof(results[idx].mdns_instance) - 1] = '\0';
        }
        if (ep.path[0]) {
            strncpy(results[idx].ipp_path, ep.path, sizeof(results[idx].ipp_path) - 1);
            results[idx].ipp_path[sizeof(results[idx].ipp_path) - 1] = '\0';
        }
        if (ep.port > 0) results[idx].ipp_port = ep.port;

        if (ep.label[0])
            snprintf(results[idx].label, sizeof(results[idx].label),
                     "%s (%s)", ipstr, ep.label);
        else
            snprintf(results[idx].label, sizeof(results[idx].label),
                     "%s (mDNS/IPP %d%s)", ipstr, results[idx].ipp_port,
                     results[idx].ipp_path);

        printf("Discovery: found %s\n", results[idx].label);
        mdns_send_detail_query(sockfd, &dest, &results[idx]);
    }

    free(buf);
    CloseSocket(sockfd);
    return count;
}

'''
s = s[:start] + new_mdns + s[end:]

s = replace_once(s,
'''static BOOL run_discovery_selection(struct Window *parent,
                                     struct DiscoveredPrinter *results,
                                     int count,
                                     char *chosen_ip,
                                     int chosen_ip_size) {''',
'''static BOOL run_discovery_selection(struct Window *parent,
                                     struct DiscoveredPrinter *results,
                                     int count,
                                     char *chosen_ip,
                                     int chosen_ip_size,
                                     char *chosen_path,
                                     int chosen_path_size,
                                     int *chosen_port) {''', 'selection signature')

s = replace_once(s,
'''                        strncpy(chosen_ip, results[selected].ip, chosen_ip_size - 1);
                        chosen_ip[chosen_ip_size - 1] = '\\0';
                        picked = TRUE;''',
'''                        strncpy(chosen_ip, results[selected].ip, chosen_ip_size - 1);
                        chosen_ip[chosen_ip_size - 1] = '\\0';
                        if (chosen_path && chosen_path_size > 0) {
                            strncpy(chosen_path, results[selected].ipp_path,
                                    chosen_path_size - 1);
                            chosen_path[chosen_path_size - 1] = '\\0';
                        }
                        if (chosen_port) *chosen_port = results[selected].ipp_port;
                        picked = TRUE;''', 'selection output')

case_start = s.index('                        case GAD_DISCOVER_BUTTON:')
case_end = s.index('                        case GAD_PRINT_BUTTON:', case_start)
new_case = r'''                        case GAD_DISCOVER_BUTTON:
                        {
                            struct DiscoveredPrinter found[MAX_DISCOVERY_RESULTS];
                            int found_count;
                            char chosen_ip[16];
                            char chosen_path[MP_MDNS_PATH_MAX];
                            int chosen_port = 0;

                            memset(found, 0, sizeof(found));
                            chosen_path[0] = '\0';
                            GT_RefreshWindow(win, NULL);
                            printf("CLEAR");

                            found_count = discover_printers_on_lan(found, MAX_DISCOVERY_RESULTS);

                            if (found_count <= 0) {
                                printf("No printers found via SSDP or mDNS.\n");
                                printf("Enter the printer IP manually and press Query.\n");
                            } else {
                                printf("Found %d candidate device(s).\n", found_count);
                                if (run_discovery_selection(win, found, found_count,
                                                            chosen_ip, sizeof(chosen_ip),
                                                            chosen_path, sizeof(chosen_path),
                                                            &chosen_port)) {
                                    struct Gadget *disc_ip_gadget = glist;
                                    struct Gadget *disc_path_gadget = glist;

                                    if (chosen_port > 0 && chosen_port != 631)
                                        snprintf(ip_buffer, sizeof(ip_buffer), "%s:%d",
                                                 chosen_ip, chosen_port);
                                    else {
                                        strncpy(ip_buffer, chosen_ip, sizeof(ip_buffer) - 1);
                                        ip_buffer[sizeof(ip_buffer) - 1] = '\0';
                                    }

                                    while (disc_ip_gadget && disc_ip_gadget->GadgetID != GAD_IP_STRING)
                                        disc_ip_gadget = disc_ip_gadget->NextGadget;
                                    if (disc_ip_gadget)
                                        GT_SetGadgetAttrs(disc_ip_gadget, win, NULL,
                                                          GTST_String, (ULONG)ip_buffer,
                                                          TAG_DONE);

                                    if (chosen_path[0]) {
                                        strncpy(driver_path_buffer, chosen_path,
                                                sizeof(driver_path_buffer) - 1);
                                        driver_path_buffer[sizeof(driver_path_buffer) - 1] = '\0';
                                        while (disc_path_gadget && disc_path_gadget->GadgetID != GAD_IPP_PATH)
                                            disc_path_gadget = disc_path_gadget->NextGadget;
                                        if (disc_path_gadget)
                                            GT_SetGadgetAttrs(disc_path_gadget, win, NULL,
                                                              GTST_String, (ULONG)driver_path_buffer,
                                                              TAG_DONE);
                                        printf("Discovery: using advertised IPP endpoint %s:%d%s\n",
                                               chosen_ip,
                                               chosen_port > 0 ? chosen_port : 631,
                                               driver_path_buffer);
                                    }

                                    perform_query_flow_allocated(win, chosen_ip, chosen_port);
                                } else {
                                    printf("Discovery selection cancelled.\n");
                                }
                            }
                        }
                        break;

'''
s = s[:case_start] + new_case + s[case_end:]
p.write_text(s)

# ---------------------------------------------------------------------------
# Makefile: link parser into GUI and run a host regression test in check.
# ---------------------------------------------------------------------------
p = ROOT / "Makefile"
s = p.read_text()
s = replace_once(s, 'test-ipp-enum test-postscript',
                 'test-ipp-enum test-mdns test-postscript', 'phony mdns')
s = replace_once(s,
'MintPrintSettings: src/MintPrintSettings.c src/http_response.c src/http_response.h src/dpi_options.c src/dpi_options.h src/ipp_enum.c src/ipp_enum.h driver/media_size.c',
'MintPrintSettings: src/MintPrintSettings.c src/http_response.c src/http_response.h src/dpi_options.c src/dpi_options.h src/ipp_enum.c src/ipp_enum.h src/mdns_endpoint.c src/mdns_endpoint.h driver/media_size.c',
'gui dependencies')
s = replace_once(s,
'src/MintPrintSettings.c src/http_response.c src/dpi_options.c src/ipp_enum.c src/lodepng.c driver/media_size.c',
'src/MintPrintSettings.c src/http_response.c src/dpi_options.c src/ipp_enum.c src/mdns_endpoint.c src/lodepng.c driver/media_size.c',
'gui command')
anchor = '''test-ipp-enum: | $(TEST_BUILD)
\t$(HOSTCC) -std=c89 -Wall -Wextra -Werror -Isrc \\
\t\ttests/test_ipp_enum.c src/ipp_enum.c -o $(TEST_BUILD)/test_ipp_enum
\t$(TEST_BUILD)/test_ipp_enum
'''
addition = anchor + '''\n\ntest-mdns: | $(TEST_BUILD)
\t$(HOSTCC) -std=c89 -Wall -Wextra -Werror -Isrc \\
\t\ttests/test_mdns_endpoint.c src/mdns_endpoint.c -o $(TEST_BUILD)/test_mdns_endpoint
\t$(TEST_BUILD)/test_mdns_endpoint
'''
s = replace_once(s, anchor, addition, 'mdns test target')
s = replace_once(s,
'check: test test-http test-ipp-enum test-postscript test-graphics-boundary',
'check: test test-http test-ipp-enum test-mdns test-postscript test-graphics-boundary',
'check mdns')
p.write_text(s)

# Diagnostic probe should mirror MintPRINT's actual IPP/1.1 query.  Older
# AirPrint implementations are exactly where using IPP/2.0 in the helper can
# produce a misleading failure that the Amiga executable would not reproduce.
p = ROOT / "windows_ipp_probe.py"
s = p.read_text()
s = replace_once(s,
'    body += b"\\x02\\x00"                 # IPP/2.0; widely accepted by IPP Everywhere printers.',
'    body += b"\\x01\\x01"                 # IPP/1.1; mirrors MintPRINT and older AirPrint devices.',
'probe ipp version')
s = replace_once(s,
'        raise IPPError("IPP response is shorter than the 8-byte header")',
'        raise IPPError("IPP response is shorter than the 8-byte header (%d bytes; check the advertised rp= path/port)" % len(data))',
'probe short response diagnostic')
p.write_text(s)

# Docs: discovery now consumes DNS-SD endpoint metadata rather than merely the
# responder address.
p = ROOT / "docs/MINTPRINT_PREFS.md"
s = p.read_text()
old = '''Both passes only look at *which address replied*, not the reply's content
(no SSDP header or DNS record parsing) - that keeps the scan simple and
predictable. Distinct, non-loopback responders from either pass are merged
into one list.

Results appear in a small selection window. Picking one and choosing
**Use Selected** fills in the Printer IPv4 field and runs the same
capability query as the **Query** button (trying the given port, then 631),
so the fetched media/colour/quality/scaling values and the printer's
supported document formats are pulled in immediately - this is where the
printer's actual name/details come from, not the discovery scan itself.'''
new = '''SSDP remains address-only. The mDNS pass now parses the IPP service's DNS-SD
PTR/SRV/TXT records as well: the advertised SRV port and TXT `rp=` resource
path are retained, with `/ipp/print` and port 631 as conservative fallbacks.
If the first PTR response omits the detail records, Settings asks the service
instance directly for SRV and TXT before the discovery window closes. This
matters for older AirPrint printers that do not use the most common endpoint.

Results appear in a small selection window. Picking one and choosing
**Use Selected** fills in Printer IPv4, applies any advertised IPP path/port,
and runs the same capability query as the **Query** button. The fetched
media/colour/quality/scaling values and document formats still come from the
IPP query; discovery only supplies the endpoint needed to reach it.'''
s = replace_once(s, old, new, 'discovery docs')
p.write_text(s)

# Remove the temporary applicator and workflow from the resulting commit.
for rel in ('.github/issue75_patch.py', '.github/workflows/apply-issue75.yml'):
    q = ROOT / rel
    if q.exists(): q.unlink()

print('issue #75 patch applied')
