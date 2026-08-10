#ifndef PPC_SPOTI_AIRPLAY_RSA_H
#define PPC_SPOTI_AIRPLAY_RSA_H

#include <stddef.h>

/*
 * AirPlay 1 ("classic AirPlay") RSA operations - ported from shairport-sync's
 * common.c (rsa_apply(), CONFIG_MBEDTLS branch), cross-referenced directly
 * against that real, working, open-source implementation rather than
 * reverse-engineered from scratch. This project vendors the exact same
 * mbedTLS library shairport-sync itself supports (2.28.x), so the API calls
 * below are a direct, verified port, not a guess.
 *
 * The embedded RSA private key (airplay_rsa.c) is Apple's original AirPlay
 * accessory private key - it was leaked/extracted from real AirPlay
 * hardware years ago and has been public, widely-known key material ever
 * since; every open-source AirPlay 1 receiver (shairport-sync included)
 * embeds this exact same key. It is not a secret this project discovered or
 * needs to protect - using it is precisely what makes an unlicensed,
 * from-scratch AirPlay 1 receiver possible at all (unlike AirPlay 2 or
 * Google Cast, which this project investigated and found real trust/
 * certificate walls for - see README.md).
 */

/* Decrypts a client's RSA-OAEP(SHA-1)-encrypted per-session AES key (the
 * "rsaaeskey" SDP attribute in a real ANNOUNCE request, already base64-
 * decoded by the caller). `in`/`in_len` is the ciphertext; `out` must have
 * room for at least 256 bytes (this key is RSA-2048) but the real recovered
 * AES key is always 16 bytes (AES-128) - `*out_len` receives the actual
 * recovered length so the caller can sanity-check it's 16, matching
 * shairport-sync's own "wanted 16" check in rtsp.c's handle_announce().
 * Returns 0 on success, -1 on any mbedTLS failure. */
int airplay_rsa_decrypt_aeskey(const unsigned char *in, size_t in_len,
                                unsigned char *out, size_t *out_len);

/* Real AirPlay 1 device-verification step ("Apple-Challenge" header,
 * base64-decoded by the caller into `challenge`/`challenge_len`, which is
 * always <=16 bytes real client challenge nonce): computes and returns the
 * "Apple-Response" header value the client expects, base64-encoded with
 * trailing '=' padding stripped (exact behavior ported from shairport-
 * sync's rtsp.c apple_challenge()) - out_b64 must have room for at least
 * 350 bytes. `server_ipv4_be` is this device's own IPv4 address (network
 * byte order, e.g. from inet_addr()) and `device_id6` is this device's
 * 6-byte AirPlay device ID (see airplay_rtsp.h - the same 6 bytes used as
 * the mDNS instance name's hex prefix). Returns 0 on success, -1 on
 * failure. */
int airplay_rsa_challenge_response(const unsigned char *challenge, size_t challenge_len,
                                    unsigned int server_ipv4_be,
                                    const unsigned char device_id6[6],
                                    char *out_b64, size_t out_b64_cap);

#endif
