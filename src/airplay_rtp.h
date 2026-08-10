#ifndef PPC_SPOTI_AIRPLAY_RTP_H
#define PPC_SPOTI_AIRPLAY_RTP_H

#include "airplay_session.h"

/*
 * AirPlay 1 RTP audio reception.
 *
 * Each UDP packet on the negotiated audio port is: a standard 12-byte RTP
 * header (version/padding/extension/CSRC-count byte, marker/payload-type
 * byte, 16-bit sequence number, 32-bit timestamp, 32-bit SSRC), followed
 * by AES-CBC-encrypted ALAC-encoded audio. Only `len & ~0xf` bytes (the
 * payload truncated down to the nearest whole 16-byte AES block) are
 * actually encrypted; the same session AES key/IV is reused unchanged for
 * every packet (no chaining/reset across packets), and any trailing
 * partial-block remainder passes through unencrypted.
 *
 * Deliberate scope limit: no jitter buffer, no out-of-order/dropped-packet
 * resequencing, no control-channel retransmit-request handling - audio
 * frames are decoded and handed to coreaudio_output_write() in the order
 * they arrive on the socket. Fine on a low-latency LAN; a lossy/reordering
 * network path would audibly glitch.
 */

/* Starts the RTP receiver thread (4MB stack - any thread whose call chain
 * might touch a large local buffer uses this convention throughout the
 * project). Sets sess->recording=1 and sess->rtp_thread_running=1 on
 * success. Safe to call only after SETUP has opened sess->audio_fd. */
void airplay_rtp_start(airplay_session *sess);

/* Signals the RTP thread to stop and joins it. Safe to call even if the
 * thread was never started. */
void airplay_rtp_stop(airplay_session *sess);

#endif
