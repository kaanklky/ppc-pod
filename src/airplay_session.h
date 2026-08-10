#ifndef PPC_SPOTI_AIRPLAY_SESSION_H
#define PPC_SPOTI_AIRPLAY_SESSION_H

#include <stdint.h>
#include <pthread.h>

#include "coreaudio_output.h"
#include "alac.h"

/*
 * Shared per-connection AirPlay 1 session state - populated by the RTSP
 * handshake (airplay_rtsp.c) as ANNOUNCE/SETUP requests arrive, then read
 * by the RTP audio receiver thread (airplay_rtp.c) once RECORD starts
 * streaming. One of these exists per active RTSP TCP connection - only one
 * active AirPlay session at a time is supported.
 */

typedef struct {
    /* Recovered from ANNOUNCE's SDP body (a=rsaaeskey/a=aesiv, RSA-OAEP
     * decrypted / base64-decoded respectively - see airplay_rsa.h). */
    unsigned char aes_key[16];
    unsigned char aes_iv[16];
    int have_keys;
    int encrypted; /* 0 if ANNOUNCE carried neither aesiv nor rsaaeskey - real,
                     * legitimate unencrypted-session case per shairport-sync's
                     * rtsp.c, not just a failure path */

    /* ALAC magic-cookie fields recovered from ANNOUNCE's "a=fmtp:" line -
     * same 12-field layout shairport-sync's rtsp.c hardcodes as reasonable
     * defaults for a standard 44100Hz/16-bit/stereo AirPlay 1 stream (see
     * airplay_rtsp.c for the exact values and why fmtp parsing itself is a
     * documented, deliberate scope limit rather than full field-by-field
     * parsing). int32_t, NOT unsigned char - real type confirmed against
     * shairport-sync's player.h ("int32_t fmtp[12]"); field 11 alone (the
     * sample rate, 44100) would not fit in a byte, a real bug caught before
     * ever compiling this, not found via trial and error. */
    int32_t fmtp[12];

    /* UDP sockets opened during SETUP, one per negotiated channel. Real
     * AirPlay 1 SETUP negotiates three: audio (RTP data), control
     * (retransmit requests / sync), and timing (clock sync) - see
     * airplay_rtsp.c's handle_setup for exactly what this project does and
     * does not implement on the control/timing channels. */
    int audio_fd;
    int control_fd;
    unsigned short audio_port;
    unsigned short control_port;
    unsigned short timing_port;
    unsigned short client_control_port;
    unsigned short client_timing_port;

    /* Set once RECORD arrives; the RTP receiver thread runs only while this
     * is true. */
    volatile int recording;
    pthread_t rtp_thread;
    volatile int rtp_thread_running;

    /* Real audio pipeline: ALAC decode (per-packet) -> existing, already-
     * proven-working CoreAudio output. Opened lazily on the first real RTP
     * packet once the true sample rate/channel count are known (ANNOUNCE's
     * SDP always declares 44100/stereo for classic AirPlay 1 - see
     * airplay_rtsp.c - so in practice this opens right after RECORD). */
    alac_file *decoder;
    coreaudio_output *output;

    /* Real AirPlay SET_PARAMETER volume (text/parameters body, "volume: X"
     * line, X in -30.0..0.0 dB or -144.0 for mute - see airplay_rtsp.c's
     * handling, grounded in shairport-sync's rtsp.c handle_set_parameter_
     * parameter) - already converted to linear 0.0..1.0 gain here, since
     * that's what coreaudio_output_set_volume expects. Stored regardless of
     * whether sess->output is open yet (SET_PARAMETER can arrive before the
     * first real RTP packet opens it) - airplay_rtp.c applies this value
     * immediately after opening output. Defaults to 1.0 (full volume) so a
     * session that never receives a volume command plays at full volume,
     * matching real AirPlay client behavior of only sending SET_PARAMETER
     * when the user actually changes the volume slider. */
    float volume_linear;

    /* Real AirPlay 1 now-playing metadata, accumulated across whatever
     * SET_PARAMETER pushes actually arrive (title/artist/album via a
     * DMAP-tagged push, cover art via a separate image/jpeg|png push -
     * see airplay_rtsp.c's SET_PARAMETER handling and airplay_dmap.h).
     * Each push only overwrites the fields it actually carried, so a
     * later push missing e.g. the album tag doesn't blank out an
     * already-known one. Synced to app_state (app_state.h) after every
     * update so the Cocoa UI reflects whichever subset is known so far,
     * not just once everything has arrived. */
    char track_title[256];
    char track_artist[256];
    char track_album[256];
    char cover_art_path[512]; /* empty = none received yet this session */
} airplay_session;

void airplay_session_init(airplay_session *sess);
void airplay_session_close(airplay_session *sess);

#endif
