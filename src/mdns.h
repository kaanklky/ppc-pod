#ifndef PPC_SPOTI_MDNS_H
#define PPC_SPOTI_MDNS_H

#include <stdint.h>
#include <stddef.h>

/*
 * Minimal mDNS (RFC 6762) responder for advertising a single DNS-SD
 * service (PTR+SRV+TXT+A records).
 *
 * Deliberate simplification: this does not do full DNS question parsing /
 * matching / known-answer suppression per RFC 6762. It filters incoming
 * packets with a crude substring search for the configured service type
 * or instance name, and if a plausibly-relevant packet arrives (or a
 * periodic timer fires) it multicasts the full record set
 * unconditionally. Not a general-purpose mDNS responder - intentionally
 * narrow, sized to this project's single fixed service.
 */

int encode_name(const char *dotted, unsigned char *out, size_t out_max);
void put_u16(unsigned char *buf, size_t off, uint16_t v);

#define MDNS_MAX_TXT_RECORDS 16

typedef struct {
    const char *instance_name; /* e.g. "PowerPC Pod" */
    const char *hostname;      /* e.g. "ppc-pod", becomes ppc-pod.local */
    uint16_t port;
    uint32_t ipv4_be; /* our address, network byte order, e.g. from inet_addr() */

    /* service_type and txt_records/txt_record_count let a caller advertise
     * any DNS-SD service (e.g. "_raop._tcp" with AirPlay's TXT record
     * set). service_type is required. */
    const char *service_type;                 /* e.g. "_raop._tcp", no ".local" suffix */
    const char *txt_records[MDNS_MAX_TXT_RECORDS]; /* e.g. "sf=0x4", "am=..." - no length prefix */
    int txt_record_count;
} mdns_service_config;

/* Builds one multicast response packet (PTR+SRV+TXT+A) into buf. Returns
 * the packet length, or -1 if it doesn't fit in buf_max. Exposed separately
 * from the socket loop so it can be unit-tested by parsing the bytes back. */
int mdns_build_response(const mdns_service_config *cfg, unsigned char *buf, size_t buf_max);

/* Opens the mDNS multicast socket (224.0.0.251:5353), joins the multicast
 * group. Returns fd, or -1 on error. */
int mdns_open_socket(void);

/* Blocking loop: for up to `max_seconds` (0 = forever), waits for packets
 * with a periodic wake every ~5s to send an unsolicited announcement
 * regardless of traffic. Returns 0 normally (only returns early on error,
 * or after max_seconds if nonzero - used for bounded test runs). */
int mdns_responder_run(int fd, const mdns_service_config *cfg, int max_seconds);

#endif
