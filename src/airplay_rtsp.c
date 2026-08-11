#include "airplay_rtsp.h"
#include "airplay_rsa.h"
#include "airplay_rtp.h"
#include "airplay_dmap.h"
#include "app_state.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mbedtls/base64.h"

/* strcasestr() is a BSD/GNU extension that doesn't exist in Mac OS X 10.2
 * Jaguar's libc (confirmed via a real dyld "undefined reference" error at
 * launch, not guessed) - this is the same case-insensitive substring search,
 * built only from strncasecmp/strlen, both of which are plain POSIX and
 * present on every target OS version. */
static const char *ppc_pod_strcasestr(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    const char *p;

    if (needle_len == 0) return haystack;

    for (p = haystack; *p != '\0'; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) return p;
    }

    return NULL;
}

static int read_line(int fd, char *out, size_t out_cap)
{
    size_t n = 0;
    for (;;) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') {
            if (n > 0 && out[n - 1] == '\r') n--;
            out[n] = '\0';
            return (int)n;
        }
        if (n + 1 < out_cap) out[n++] = c;
    }
}

static void trim(char *s)
{
    size_t len = strlen(s);
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
}

int airplay_rtsp_read_request(int fd, airplay_rtsp_request *req)
{
    char line[AIRPLAY_MAX_HEADER_KEY + AIRPLAY_MAX_HEADER_VAL + 8];
    int n;
    size_t content_length = 0;

    memset(req, 0, sizeof(*req));

    /* Request line: "<METHOD> <URI> RTSP/1.0" - same shape as an HTTP
     * request-line, just a different method set and version token. */
    n = read_line(fd, line, sizeof(line));
    if (n <= 0) return -1;
    {
        char *sp1 = strchr(line, ' ');
        char *sp2;
        if (sp1 == NULL) return -1;
        *sp1 = '\0';
        snprintf(req->method, sizeof(req->method), "%s", line);
        sp1++;
        sp2 = strchr(sp1, ' ');
        if (sp2 == NULL) return -1;
        *sp2 = '\0';
        snprintf(req->uri, sizeof(req->uri), "%s", sp1);
        /* remainder (sp2+1) is "RTSP/1.0" - not otherwise checked, matching
         * this project's generally lenient parsing style (json_min.c,
         * http_server.c) since this project only ever talks to one real
         * kind of client. */
    }

    for (;;) {
        char *colon;
        n = read_line(fd, line, sizeof(line));
        if (n < 0) return -1;
        if (n == 0) break; /* blank line = end of headers */
        colon = strchr(line, ':');
        if (colon == NULL) continue;
        *colon = '\0';
        if (req->header_count < AIRPLAY_MAX_HEADERS) {
            snprintf(req->header_keys[req->header_count], AIRPLAY_MAX_HEADER_KEY, "%s", line);
            snprintf(req->header_vals[req->header_count], AIRPLAY_MAX_HEADER_VAL, "%s", colon + 1);
            trim(req->header_vals[req->header_count]);
            req->header_count++;
        }
    }

    {
        const char *cl = airplay_rtsp_get_header(req, "Content-Length");
        if (cl != NULL) content_length = (size_t)atoi(cl);
    }

    if (content_length > 0) {
        size_t got = 0;
        if (content_length > sizeof(req->body)) {
            /* A SET_PARAMETER carrying now-playing metadata (album art can
             * easily be 100KB+) may exceed our body buffer - that's not an
             * error, just a body we don't need to parse. The TCP byte
             * stream still has to stay in sync for the next request, so
             * drain-and-discard exactly content_length bytes rather than
             * treating an oversized body as a fatal parse error (which
             * would tear down the whole RTSP session). */
            unsigned char discard[1024];
            size_t remaining = content_length;
            fprintf(stderr, "[airplay_rtsp] draining oversized request body (%zu bytes, not stored)\n",
                    content_length);
            while (remaining > 0) {
                size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
                ssize_t r = recv(fd, discard, chunk, 0);
                if (r <= 0) return -1;
                remaining -= (size_t)r;
            }
            req->body_len = 0;
            return 0;
        }
        while (got < content_length) {
            ssize_t r = recv(fd, req->body + got, content_length - got, 0);
            if (r <= 0) return -1;
            got += (size_t)r;
        }
        req->body_len = got;
    }

    return 0;
}

const char *airplay_rtsp_get_header(const airplay_rtsp_request *req, const char *key)
{
    int i;
    for (i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->header_keys[i], key) == 0) return req->header_vals[i];
    }
    return NULL;
}

/* SDP line-scanning approach ported from shairport-sync's handle_announce
 * (rtsp.c): walk the body line by line (SDP is plain-text "key=value"
 * lines separated by \r\n or \n), matching known prefixes -
 * "a=rsaaeskey:", "a=aesiv:", "a=fmtp:" - rather than a full SDP parser,
 * since those are the only three lines session setup needs. */
static void parse_announce_sdp(const unsigned char *body, size_t body_len, airplay_session *sess)
{
    const char *cp = (const char *)body;
    const char *end = (const char *)body + body_len;
    const char *paesiv = NULL;
    const char *prsaaeskey = NULL;
    size_t paesiv_len = 0, prsaaeskey_len = 0;

    while (cp < end) {
        const char *nl = memchr(cp, '\n', (size_t)(end - cp));
        const char *line_end = nl ? nl : end;
        size_t line_len = (size_t)(line_end - cp);
        while (line_len > 0 && (cp[line_len - 1] == '\r')) line_len--;

        if (line_len > 8 && strncmp(cp, "a=aesiv:", 8) == 0) {
            paesiv = cp + 8;
            paesiv_len = line_len - 8;
        } else if (line_len > 12 && strncmp(cp, "a=rsaaeskey:", 12) == 0) {
            prsaaeskey = cp + 12;
            prsaaeskey_len = line_len - 12;
        } else if (line_len > 7 && strncmp(cp, "a=fmtp:", 7) == 0) {
            /* Real classic-AirPlay ALAC fmtp defaults, byte-for-byte the
             * same as shairport-sync's rtsp.c handle_announce() hardcodes
             * for a standard 44100Hz/16-bit/stereo stream - a real,
             * deliberate scope limit: this project does not parse the
             * actual fmtp field values out of the SDP line (every real
             * classic-AirPlay-1 sender uses this exact configuration in
             * practice, per shairport-sync's own comment there), it just
             * confirms an ALAC stream was announced at all. */
            sess->fmtp[0] = 96;
            sess->fmtp[1] = 352;
            sess->fmtp[2] = 0;
            sess->fmtp[3] = 16;
            sess->fmtp[4] = 40;
            sess->fmtp[5] = 10;
            sess->fmtp[6] = 14;
            sess->fmtp[7] = 2;
            sess->fmtp[8] = 255;
            sess->fmtp[9] = 0;
            sess->fmtp[10] = 0;
            sess->fmtp[11] = 44100;
        }

        cp = nl ? nl + 1 : end;
    }

    if (paesiv == NULL && prsaaeskey == NULL) {
        sess->encrypted = 0;
    } else if (paesiv != NULL && prsaaeskey != NULL) {
        unsigned char aesiv_raw[64];
        unsigned char rsaaeskey_raw[512];
        size_t aesiv_raw_len = 0, rsaaeskey_raw_len = 0;
        char tmp[700];

        sess->encrypted = 1;

        if (paesiv_len < sizeof(tmp)) {
            memcpy(tmp, paesiv, paesiv_len);
            tmp[paesiv_len] = '\0';
            if (mbedtls_base64_decode(aesiv_raw, sizeof(aesiv_raw), &aesiv_raw_len,
                                       (const unsigned char *)tmp, paesiv_len) == 0 &&
                aesiv_raw_len == 16) {
                memcpy(sess->aes_iv, aesiv_raw, 16);
            } else {
                fprintf(stderr, "[airplay_rtsp] bad aesiv (decoded %zu bytes, wanted 16)\n", aesiv_raw_len);
            }
        }

        if (prsaaeskey_len < sizeof(tmp)) {
            memcpy(tmp, prsaaeskey, prsaaeskey_len);
            tmp[prsaaeskey_len] = '\0';
            {
                unsigned char b64raw[512];
                size_t b64raw_len = 0;
                if (mbedtls_base64_decode(b64raw, sizeof(b64raw), &b64raw_len,
                                           (const unsigned char *)tmp, prsaaeskey_len) == 0) {
                    rsaaeskey_raw_len = sizeof(rsaaeskey_raw);
                    if (airplay_rsa_decrypt_aeskey(b64raw, b64raw_len, rsaaeskey_raw, &rsaaeskey_raw_len) == 0 &&
                        rsaaeskey_raw_len == 16) {
                        memcpy(sess->aes_key, rsaaeskey_raw, 16);
                        sess->have_keys = 1;
                    } else {
                        fprintf(stderr, "[airplay_rtsp] bad rsaaeskey (decrypted %zu bytes, wanted 16)\n",
                                rsaaeskey_raw_len);
                    }
                } else {
                    fprintf(stderr, "[airplay_rtsp] rsaaeskey base64 decode failed\n");
                }
            }
        }
    } else {
        fprintf(stderr, "[airplay_rtsp] ANNOUNCE missing aesiv or rsaaeskey (need both or neither)\n");
    }
}

/* Real AirPlay volume wire format, confirmed against shairport-sync's
 * rtsp.c handle_set_parameter_parameter(): a SET_PARAMETER request with
 * Content-Type "text/parameters" carries a plain-text body with a
 * "volume: <float>" line (other lines, e.g. "progress: ...", are a
 * documented scope limit, not handled here). The value is a real dB
 * attenuation in the range -30.0 (quietest) to 0.0 (loudest), or exactly
 * -144.0 meaning muted - this exact range/mute-sentinel is standard,
 * publicly-documented AirPlay protocol convention (not a shairport-sync-
 * specific software feature - shairport's own configurable multi-curve
 * volume mapping on top of this is a shairport feature this project does
 * not need to replicate, since it only has one simple CoreAudio output to
 * drive, not shairport's configurable hardware/software mixer split).
 * Converts directly to the linear 0.0..1.0 gain coreaudio_output_set_
 * volume expects via the standard dB-to-linear-amplitude formula
 * 10^(db/20). */
static int parse_set_parameter_volume(const unsigned char *body, size_t body_len, float *out_linear)
{
    const char *cp = (const char *)body;
    const char *end = (const char *)body + body_len;
    static const char *prefix = "volume: ";
    size_t prefix_len = strlen(prefix);

    while (cp < end) {
        const char *nl = memchr(cp, '\n', (size_t)(end - cp));
        const char *line_end = nl ? nl : end;
        size_t line_len = (size_t)(line_end - cp);
        while (line_len > 0 && (cp[line_len - 1] == '\r')) line_len--;

        if (line_len > prefix_len && strncmp(cp, prefix, prefix_len) == 0) {
            char numbuf[32];
            double db;
            size_t numlen = line_len - prefix_len;
            if (numlen >= sizeof(numbuf)) numlen = sizeof(numbuf) - 1;
            memcpy(numbuf, cp + prefix_len, numlen);
            numbuf[numlen] = '\0';
            db = atof(numbuf);

            if (db <= -144.0) {
                *out_linear = 0.0f;
            } else {
                if (db > 0.0) db = 0.0;
                if (db < -30.0) db = -30.0;
                *out_linear = (float)pow(10.0, db / 20.0);
            }
            return 0;
        }

        cp = nl ? nl + 1 : end;
    }
    return -1;
}

/* Mirrors app_state's app_track_info shape, fed from whatever subset of
 * sess->track_title/artist/album/cover_art_path is known so far - see
 * airplay_session.h's comment on why these accumulate across pushes
 * rather than requiring every field to arrive in one message. Duration is
 * left at 0 (unknown): real AirPlay 1 doesn't push a reliable track-length
 * field this project parses (a "progress:" text/parameters line gives RTP
 * timestamps, not milliseconds, and is a documented scope limit - the
 * seek bar is display-only for AirPlay, confirmed acceptable). */
static void airplay_sync_track_to_app_state(const airplay_session *sess)
{
    app_track_info info;
    memset(&info, 0, sizeof(info));
    snprintf(info.title, sizeof(info.title), "%s", sess->track_title);
    snprintf(info.artist, sizeof(info.artist), "%s", sess->track_artist);
    snprintf(info.album, sizeof(info.album), "%s", sess->track_album);
    snprintf(info.album_art_path, sizeof(info.album_art_path), "%s", sess->cover_art_path);
    app_state_set_track(app_state_shared(), &info);
}

/* ~/Library/Application Support/PowerPC Pod/artcache - same convention as
 * app_settings_default_path(). */
/* Plain mkdir() only creates the last path component and fails if its own
 * parent doesn't exist yet, unlike `mkdir -p`. Walk the whole path and
 * mkdir() every component in order instead of assuming which levels
 * already exist. */
static void mkdir_recursive(char *path)
{
    char *p;
    for (p = path + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755); /* ignore EEXIST/other errors - best effort */
            *p = '/';
        }
    }
    mkdir(path, 0755);
}

/* Alternates between two fixed filenames on each call so the path itself
 * changes on every update - cocoa_ui.m's reload check only compares the
 * path string, not file contents, so a fixed filename would never trigger
 * a reload on a track change even though the file's contents changed.
 * Bounds disk usage to at most 2 cached images. */
static void airplay_cover_art_path(char *out, size_t out_cap, const char *ext)
{
    static int toggle = 0;
    const char *home = getenv("HOME");
    char dir1[512], dir2[512];
    if (home == NULL) home = "";
    /* ~/Library/Application Support/PowerPC Pod - the real Mac OS X
     * convention for per-user app data, matching app_settings.c's
     * settings.txt (see its comment for why ~/dev/ppc-pod was wrong). */
    snprintf(dir1, sizeof(dir1), "%s/Library/Application Support/PowerPC Pod", home);
    mkdir_recursive(dir1);
    snprintf(dir2, sizeof(dir2), "%s/artcache", dir1);
    mkdir(dir2, 0755);
    toggle = !toggle;
    snprintf(out, out_cap, "%s/airplay_cover_%d.%s", dir2, toggle, ext);
}

static int open_udp_socket(unsigned short *out_port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0; /* let the OS pick a free port, matching shairport-sync's real behavior */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(fd);
        return -1;
    }

    /* Real bug this fixes: UDP has no connection-closed signal like TCP -
     * a plain blocking recv() on this socket (airplay_rtp.c's rtp_thread_
     * main) would wait forever once the sender stops sending packets,
     * which is exactly what happens on every normal disconnect. That
     * thread's own loop already correctly re-checks sess->recording after
     * every recv() call and handles a timeout as a harmless retry - the
     * only missing piece was recv() itself never timing out at all, so it
     * never got the chance to notice sess->recording had been cleared.
     * Without this, airplay_rtp_stop()'s pthread_join() on that thread
     * blocked forever too, freezing the single accept() loop that
     * services every incoming AirPlay connection - a device disconnecting
     * permanently wedged the receiver until the app was force-quit. A 1s
     * timeout is frequent enough to notice a stop request promptly without
     * meaningfully affecting real packet reception. */
    {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    *out_port = ntohs(addr.sin_port);
    return fd;
}

static void send_response(int fd, int status_code, const char *cseq,
                           const char *apple_response, const char *extra_header,
                           const char *body, size_t body_len)
{
    char header[1024];
    const char *status_text = (status_code == 200) ? "OK" : "Error";
    int n = snprintf(header, sizeof(header),
                      "RTSP/1.0 %d %s\r\n"
                      "CSeq: %s\r\n"
                      "Server: AirTunes/105.1\r\n",
                      status_code, status_text, cseq != NULL ? cseq : "0");
    if (apple_response != NULL) {
        n += snprintf(header + n, sizeof(header) - (size_t)n, "Apple-Response: %s\r\n", apple_response);
    }
    if (extra_header != NULL) {
        n += snprintf(header + n, sizeof(header) - (size_t)n, "%s\r\n", extra_header);
    }
    n += snprintf(header + n, sizeof(header) - (size_t)n, "Content-Length: %zu\r\n\r\n", body_len);

    send(fd, header, (size_t)n, 0);
    if (body_len > 0) send(fd, body, body_len, 0);
}


void airplay_rtsp_serve_connection(int conn_fd, unsigned int server_ipv4_be,
                                    const unsigned char device_id6[6],
                                    const char *device_name)
{
    airplay_session sess;
    int done = 0;

    airplay_session_init(&sess);
    fprintf(stderr, "[airplay] connection accepted for \"%s\"\n", device_name);

    while (!done) {
        airplay_rtsp_request req;
        const char *cseq;
        char apple_response[400];
        const char *apple_response_ptr = NULL;
        int status = 200;
        char extra_header[300];
        int have_extra_header = 0;
        const char *body_out = NULL;
        size_t body_out_len = 0;

        if (airplay_rtsp_read_request(conn_fd, &req) != 0) break;

        cseq = airplay_rtsp_get_header(&req, "CSeq");
        fprintf(stderr, "[airplay] %s %s\n", req.method, req.uri);

        /* Apple-Challenge/Apple-Response: real AirPlay 1 device-
         * verification step, checked on every request the same way
         * shairport-sync's main dispatch loop does (apple_challenge() is
         * called unconditionally per request and no-ops if the header is
         * absent - rtsp.c:4196), not just on OPTIONS specifically. */
        {
            const char *chall_b64 = airplay_rtsp_get_header(&req, "Apple-Challenge");
            if (chall_b64 != NULL) {
                unsigned char chall_raw[64];
                size_t chall_len = 0;
                if (mbedtls_base64_decode(chall_raw, sizeof(chall_raw), &chall_len,
                                           (const unsigned char *)chall_b64, strlen(chall_b64)) == 0) {
                    if (airplay_rsa_challenge_response(chall_raw, chall_len, server_ipv4_be, device_id6,
                                                        apple_response, sizeof(apple_response)) == 0) {
                        apple_response_ptr = apple_response;
                    }
                }
            }
        }

        if (strcmp(req.method, "OPTIONS") == 0) {
            snprintf(extra_header, sizeof(extra_header),
                     "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, "
                     "OPTIONS, GET_PARAMETER, SET_PARAMETER");
            have_extra_header = 1;
        } else if (strcmp(req.method, "ANNOUNCE") == 0) {
            parse_announce_sdp(req.body, req.body_len, &sess);
            if (sess.encrypted && !sess.have_keys) {
                status = 456; /* Header Field Not Valid for Resource - matches rtsp.c */
            }
        } else if (strcmp(req.method, "SETUP") == 0) {
            const char *transport = airplay_rtsp_get_header(&req, "Transport");
            unsigned short cport = 0, tport = 0;
            if (transport != NULL) {
                const char *p = strstr(transport, "control_port=");
                if (p != NULL) cport = (unsigned short)atoi(strchr(p, '=') + 1);
                p = strstr(transport, "timing_port=");
                if (p != NULL) tport = (unsigned short)atoi(strchr(p, '=') + 1);
            }
            sess.client_control_port = cport;
            sess.client_timing_port = tport;

            sess.audio_fd = open_udp_socket(&sess.audio_port);
            sess.control_fd = open_udp_socket(&sess.control_port);
            /* Scope limit: opens a timing UDP port (so SETUP's response
             * can name one, matching the Transport: header shape a client
             * expects) but doesn't implement the timing-sync request/
             * response exchange itself - see airplay_rtp.h. */
            {
                int timing_fd = open_udp_socket(&sess.timing_port);
                if (timing_fd >= 0) close(timing_fd); /* port number is all SETUP's response needs */
            }

            if (sess.audio_fd < 0 || sess.control_fd < 0) {
                status = 451;
            } else {
                snprintf(extra_header, sizeof(extra_header),
                         "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
                         "control_port=%u;timing_port=%u;server_port=%u\r\nSession: 1",
                         sess.control_port, sess.timing_port, sess.audio_port);
                have_extra_header = 1;
            }
        } else if (strcmp(req.method, "RECORD") == 0) {
            if (!sess.have_keys && sess.encrypted) {
                status = 456;
            } else {
                airplay_rtp_start(&sess);
            }
        } else if (strcmp(req.method, "TEARDOWN") == 0) {
            done = 1;
            /* Some senders tear down and re-establish the whole RTSP
             * session on pause/resume rather than sending FLUSH and
             * keeping the connection open. Only stop the playing/paused
             * indicator here; leave the last-known track/art displayed
             * until something new replaces it (a fresh RECORD's own
             * metadata, or a backend switch) - airplay_main.c's
             * post-connection cleanup still clears peer/backend so the UI
             * correctly shows "not connected". */
            app_state_set_stopped(app_state_shared());
        } else if (strcmp(req.method, "SET_PARAMETER") == 0) {
            /* Handles the two metadata push kinds this project's UI needs
             * (title/artist/album via a DMAP-tagged push, cover art via a
             * separate image push) and volume changes - see airplay_dmap.h
             * and airplay_sync_track_to_app_state. */
            const char *ct = airplay_rtsp_get_header(&req, "Content-Type");
            if (ct != NULL && strcasecmp(ct, "text/parameters") == 0 && req.body_len > 0) {
                float linear = 1.0f;
                if (parse_set_parameter_volume(req.body, req.body_len, &linear) == 0) {
                    sess.volume_linear = linear;
                    if (sess.output != NULL) {
                        coreaudio_output_set_volume(sess.output, linear);
                    }
                    fprintf(stderr, "[airplay] SET_PARAMETER volume -> linear gain %.3f\n", (double)linear);
                }
            } else if (ct != NULL && strncasecmp(ct, "application/x-dmap-tagged", 25) == 0 && req.body_len > 0) {
                airplay_dmap_metadata md;
                airplay_dmap_parse(req.body, req.body_len, &md);
                if (md.have_title) snprintf(sess.track_title, sizeof(sess.track_title), "%s", md.title);
                if (md.have_artist) snprintf(sess.track_artist, sizeof(sess.track_artist), "%s", md.artist);
                if (md.have_album) snprintf(sess.track_album, sizeof(sess.track_album), "%s", md.album);
                fprintf(stderr, "[airplay] metadata: title=\"%s\" artist=\"%s\" album=\"%s\"\n",
                        sess.track_title, sess.track_artist, sess.track_album);
                airplay_sync_track_to_app_state(&sess);
            } else if (ct != NULL && strncasecmp(ct, "image", 5) == 0 && req.body_len > 0) {
                /* The Content-Type subtype isn't reliably jpeg/png, only
                 * the "image" prefix is trustworthy - sniff by substring
                 * for the cache file's extension, defaulting to .jpg. */
                const char *ext = (ppc_pod_strcasestr(ct, "png") != NULL) ? "png" : "jpg";
                char path[512];
                FILE *f;
                airplay_cover_art_path(path, sizeof(path), ext);
                f = fopen(path, "wb");
                if (f != NULL) {
                    fwrite(req.body, 1, req.body_len, f);
                    fclose(f);
                    snprintf(sess.cover_art_path, sizeof(sess.cover_art_path), "%s", path);
                    fprintf(stderr, "[airplay] cover art cached: %s (%zu bytes)\n", path, req.body_len);
                    airplay_sync_track_to_app_state(&sess);
                } else {
                    fprintf(stderr, "[airplay] failed to write cover art cache file %s\n", path);
                }
            }
        } else if (strcmp(req.method, "FLUSH") == 0 || strcmp(req.method, "GET_PARAMETER") == 0) {
            /* Scope limit: acknowledged but not acted on. No local pause
             * tracking or remote control, no metadata pull. */
        } else {
            status = 501; /* Not Implemented */
        }

        send_response(conn_fd, status, cseq, apple_response_ptr,
                       have_extra_header ? extra_header : NULL, body_out, body_out_len);
    }

    airplay_session_close(&sess);
    close(conn_fd);

    fprintf(stderr, "[airplay] connection closed for \"%s\"\n", device_name);
}
