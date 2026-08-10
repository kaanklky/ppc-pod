#ifndef PPC_SPOTI_AIRPLAY_RTSP_H
#define PPC_SPOTI_AIRPLAY_RTSP_H

#include <stddef.h>

#include "airplay_session.h"

/*
 * AirPlay 1 ("classic AirPlay") RTSP session handshake, cross-referenced
 * against shairport-sync's rtsp.c. RTSP's wire format is HTTP/1.1-shaped
 * (request-line + headers + optional body) but with its own method set and
 * request-line format ("ANNOUNCE rtsp://... RTSP/1.0") and headers (CSeq,
 * Apple-Challenge, Transport, Session) - a dedicated small parser here.
 *
 * Methods handled, in the order a client sends them:
 *   OPTIONS   -> respond with Public: header listing supported methods,
 *                plus Apple-Response if the request carried Apple-Challenge.
 *   ANNOUNCE  -> parse the SDP body for a=rsaaeskey/a=aesiv (recover the
 *                per-session AES key via airplay_rsa.h) and a=fmtp (ALAC
 *                codec parameters).
 *   SETUP     -> open UDP sockets for audio/control/timing, reply with a
 *                Transport: header naming our chosen ports.
 *   RECORD    -> start the RTP audio receiver thread (airplay_rtp.h).
 *   TEARDOWN  -> stop the RTP thread and close sockets.
 * GET_PARAMETER/SET_PARAMETER are acknowledged with a bare 200 (clients use
 * SET_PARAMETER for volume/metadata - not acted on, a documented scope
 * limit).
 */

#define AIRPLAY_MAX_HEADERS 24
#define AIRPLAY_MAX_HEADER_KEY 64
#define AIRPLAY_MAX_HEADER_VAL 512
#define AIRPLAY_MAX_URI 256
/* Was 8192 (metadata/volume-only, real embedded cover art was drained and
 * discarded unread - see airplay_rtsp_read_request's oversized-body
 * comment). Sized to fit a SET_PARAMETER image/jpeg|png push. A struct
 * this large as a stack local requires the 4MB-stack-thread convention
 * used for airplay_rtsp_serve_connection's caller
 * (airplay_backend_thread_main in ppc_pod_gui_main.m). */
#define AIRPLAY_MAX_BODY (512 * 1024)

typedef struct {
    char method[16];
    char uri[AIRPLAY_MAX_URI];
    char header_keys[AIRPLAY_MAX_HEADERS][AIRPLAY_MAX_HEADER_KEY];
    char header_vals[AIRPLAY_MAX_HEADERS][AIRPLAY_MAX_HEADER_VAL];
    int header_count;
    unsigned char body[AIRPLAY_MAX_BODY];
    size_t body_len;
} airplay_rtsp_request;

/* Reads and parses exactly one RTSP request from a connected TCP socket.
 * Blocking. Returns 0 on success, -1 on error/EOF (connection closed). */
int airplay_rtsp_read_request(int fd, airplay_rtsp_request *req);

/* Case-insensitive header lookup (RTSP/HTTP header names are conventionally
 * case-insensitive) - returns NULL if absent. */
const char *airplay_rtsp_get_header(const airplay_rtsp_request *req, const char *key);

/* Handles requests on one already-accepted RTSP connection until the client
 * disconnects or a TEARDOWN is processed. `server_ipv4_be` and `device_id6`
 * are needed for the Apple-Challenge response (see airplay_rsa.h).
 * `device_name` is used only for logging. Owns and tears down `sess` (via
 * airplay_session_close) before returning. */
void airplay_rtsp_serve_connection(int conn_fd, unsigned int server_ipv4_be,
                                    const unsigned char device_id6[6],
                                    const char *device_name);

#endif
