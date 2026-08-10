#include "mdns.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MDNS_ADDR "224.0.0.251"
#define MDNS_PORT 5353

#define TYPE_A   1
#define TYPE_PTR 12
#define TYPE_TXT 16
#define TYPE_SRV 33
#define CLASS_IN 1

/* Encodes a dotted name ("foo.local", or a single raw label like an
 * instance name containing spaces/parens - DNS wire-format labels are raw
 * length-prefixed byte strings, not escaped text, so that's fine as long as
 * each label is <=63 bytes) as length-prefixed labels terminated by a zero
 * byte. `dotted` labels are split on '.'; pass an already-combined string
 * like "My Instance._raop._tcp.local" and it splits on the
 * literal dots between the service/domain parts. Returns bytes written, or
 * -1 if it doesn't fit. */
int encode_name(const char *dotted, unsigned char *out, size_t out_max)
{
    size_t out_pos = 0;
    const char *p = dotted;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t label_len = dot ? (size_t)(dot - p) : strlen(p);
        if (label_len > 63 || out_pos + 1 + label_len >= out_max) return -1;
        out[out_pos++] = (unsigned char)label_len;
        memcpy(out + out_pos, p, label_len);
        out_pos += label_len;
        p += label_len;
        if (*p == '.') p++;
    }
    if (out_pos + 1 >= out_max) return -1;
    out[out_pos++] = 0;
    return (int)out_pos;
}

void put_u16(unsigned char *buf, size_t off, uint16_t v)
{
    buf[off] = (unsigned char)(v >> 8);
    buf[off + 1] = (unsigned char)(v & 0xff);
}

static void put_u32(unsigned char *buf, size_t off, uint32_t v)
{
    buf[off] = (unsigned char)(v >> 24);
    buf[off + 1] = (unsigned char)(v >> 16);
    buf[off + 2] = (unsigned char)(v >> 8);
    buf[off + 3] = (unsigned char)(v & 0xff);
}

int mdns_build_response(const mdns_service_config *cfg, unsigned char *buf, size_t buf_max)
{
    char instance_fqdn[256];
    char hostname_fqdn[256];
    char service_fqdn[128];
    const char *service_type = cfg->service_type;
    size_t pos = 0;
    int n;

    snprintf(instance_fqdn, sizeof(instance_fqdn), "%s.%s.local", cfg->instance_name, service_type);
    snprintf(hostname_fqdn, sizeof(hostname_fqdn), "%s.local", cfg->hostname);
    snprintf(service_fqdn, sizeof(service_fqdn), "%s.local", service_type);

    if (buf_max < 12) return -1;
    /* Header: id=0, flags=response+authoritative, 0 questions, 4 answers */
    put_u16(buf, 0, 0);
    put_u16(buf, 2, 0x8400);
    put_u16(buf, 4, 0);
    put_u16(buf, 6, 4);
    put_u16(buf, 8, 0);
    put_u16(buf, 10, 0);
    pos = 12;

    /* --- PTR record: <service>._tcp.local -> instance_fqdn --- */
    n = encode_name(service_fqdn, buf + pos, buf_max - pos);
    if (n < 0) return -1;
    pos += (size_t)n;
    if (pos + 10 > buf_max) return -1;
    put_u16(buf, pos, TYPE_PTR); pos += 2;
    put_u16(buf, pos, CLASS_IN); pos += 2;
    put_u32(buf, pos, 120); pos += 4;
    {
        size_t rdlen_pos = pos;
        pos += 2;
        n = encode_name(instance_fqdn, buf + pos, buf_max - pos);
        if (n < 0) return -1;
        put_u16(buf, rdlen_pos, (uint16_t)n);
        pos += (size_t)n;
    }

    /* --- SRV record: instance_fqdn -> priority/weight/port/target --- */
    n = encode_name(instance_fqdn, buf + pos, buf_max - pos);
    if (n < 0) return -1;
    pos += (size_t)n;
    if (pos + 10 > buf_max) return -1;
    put_u16(buf, pos, TYPE_SRV); pos += 2;
    put_u16(buf, pos, CLASS_IN); pos += 2;
    put_u32(buf, pos, 120); pos += 4;
    {
        size_t rdlen_pos = pos;
        pos += 2;
        size_t rdata_start = pos;
        if (pos + 6 > buf_max) return -1;
        put_u16(buf, pos, 0); pos += 2; /* priority */
        put_u16(buf, pos, 0); pos += 2; /* weight */
        put_u16(buf, pos, cfg->port); pos += 2;
        n = encode_name(hostname_fqdn, buf + pos, buf_max - pos);
        if (n < 0) return -1;
        pos += (size_t)n;
        put_u16(buf, rdlen_pos, (uint16_t)(pos - rdata_start));
    }

    /* --- TXT record: instance_fqdn -> caller-provided records --- */
    n = encode_name(instance_fqdn, buf + pos, buf_max - pos);
    if (n < 0) return -1;
    pos += (size_t)n;
    if (pos + 10 > buf_max) return -1;
    put_u16(buf, pos, TYPE_TXT); pos += 2;
    put_u16(buf, pos, CLASS_IN); pos += 2;
    put_u32(buf, pos, 120); pos += 4;
    {
        size_t rdlen_pos = pos;
        size_t rdata_start;
        int i;
        pos += 2;
        rdata_start = pos;
        for (i = 0; i < cfg->txt_record_count; i++) {
            size_t l = strlen(cfg->txt_records[i]);
            if (l > 255 || pos + 1 + l > buf_max) return -1;
            buf[pos++] = (unsigned char)l;
            memcpy(buf + pos, cfg->txt_records[i], l);
            pos += l;
        }
        put_u16(buf, rdlen_pos, (uint16_t)(pos - rdata_start));
    }

    /* --- A record: hostname_fqdn -> ipv4 --- */
    n = encode_name(hostname_fqdn, buf + pos, buf_max - pos);
    if (n < 0) return -1;
    pos += (size_t)n;
    if (pos + 10 + 4 > buf_max) return -1;
    put_u16(buf, pos, TYPE_A); pos += 2;
    put_u16(buf, pos, CLASS_IN); pos += 2;
    put_u32(buf, pos, 120); pos += 4;
    put_u16(buf, pos, 4); pos += 2;
    memcpy(buf + pos, &cfg->ipv4_be, 4);
    pos += 4;

    return (int)pos;
}

int mdns_open_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(MDNS_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        close(fd);
        return -1;
    }

    unsigned char ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

static int buf_contains(const unsigned char *buf, ssize_t len, const char *needle)
{
    size_t needle_len = strlen(needle);
    ssize_t i;
    if (len < (ssize_t)needle_len) return 0;
    for (i = 0; i + (ssize_t)needle_len <= len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

static int packet_looks_relevant(const unsigned char *buf, ssize_t len, const char *service_type)
{
    /* Crude substring search over the raw bytes, in lieu of full DNS
     * question parsing - see the "deliberate simplification" note in
     * mdns.h. Matches whichever service type this responder was configured
     * for. */
    return buf_contains(buf, len, service_type);
}

#define MDNS_PERIODIC_ANNOUNCE_SECONDS 5

int mdns_responder_run(int fd, const mdns_service_config *cfg, int max_seconds)
{
    unsigned char in_buf[2048];
    unsigned char resp_buf[2048];
    struct sockaddr_in mcast_addr;
    int elapsed = 0;
    time_t last_announce;

    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MDNS_PORT);
    mcast_addr.sin_addr.s_addr = inet_addr(MDNS_ADDR);

    /* Relying on SO_RCVTIMEO elapsing to trigger a periodic unsolicited
     * announce doesn't work: that timer resets on ANY incoming packet, not
     * just relevant ones, so on a busy LAN recvfrom() may never time out
     * and the periodic announce never fires - only a matching incoming
     * QUERY would trigger a response. Track elapsed wall-clock time
     * explicitly instead of trusting the socket timeout to double as a
     * periodic clock. */
    last_announce = time(NULL);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t n = recvfrom(fd, in_buf, sizeof(in_buf), 0,
                              (struct sockaddr *)&peer, &peer_len);
        time_t now = time(NULL);

        int should_announce = 0;
        if (n > 0 && packet_looks_relevant(in_buf, n, cfg->service_type)) {
            should_announce = 1;
        }
        if (now - last_announce >= MDNS_PERIODIC_ANNOUNCE_SECONDS) {
            should_announce = 1;
        }
        if (n < 0) {
            elapsed += 5;
        }

        if (should_announce) {
            int resp_len = mdns_build_response(cfg, resp_buf, sizeof(resp_buf));
            if (resp_len > 0) {
                sendto(fd, resp_buf, (size_t)resp_len, 0,
                       (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
            }
            last_announce = now;
        }

        if (max_seconds > 0 && elapsed >= max_seconds) break;
    }
    return 0;
}
