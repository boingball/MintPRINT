#ifndef MINTPRINT_MDNS_ENDPOINT_H
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
