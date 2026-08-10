#include "airplay_rtp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>

#include "mbedtls/aes.h"

/* Real classic-AirPlay-1 RTP audio: 352 frames (samples per channel) per
 * packet, 16-bit stereo - confirmed in shairport-sync's player.c comment
 * ("It always has a length of 352 frames per packet. And it's always
 * 16-bit interleaved stereo.") for the ALAC_44100_S16_2 case, which is what
 * classic AirPlay 1 always sends. */
#define AIRPLAY_FRAMES_PER_PACKET 352
#define AIRPLAY_CHANNELS 2
#define AIRPLAY_SAMPLE_RATE 44100

#define RTP_HEADER_LEN 12
#define MAX_UDP_PACKET 2048

static void *rtp_thread_main(void *arg)
{
    airplay_session *sess = (airplay_session *)arg;
    unsigned char packet[MAX_UDP_PACKET];
    unsigned char decrypted[MAX_UDP_PACKET];
    /* alac_decode_frame's real output sizing (per shairport-sync's own
     * caller in player.c): frames_per_packet * channels * 2 bytes/sample,
     * plus generous headroom since this project doesn't strictly need to
     * trust the peer's declared packet size. */
    unsigned char pcm[AIRPLAY_FRAMES_PER_PACKET * AIRPLAY_CHANNELS * 2 + 4096];
    mbedtls_aes_context aes;
    int have_aes_key = 0;

    fprintf(stderr, "[airplay_rtp] receiver thread started (fd=%d)\n", sess->audio_fd);

    if (sess->have_keys) {
        mbedtls_aes_init(&aes);
        if (mbedtls_aes_setkey_dec(&aes, sess->aes_key, 128) == 0) {
            have_aes_key = 1;
        } else {
            fprintf(stderr, "[airplay_rtp] mbedtls_aes_setkey_dec failed\n");
        }
    }

    /* Real bug caught before compiling, not found via trial and error:
     * alac_set_info() expects a full raw ALAC "magic cookie" buffer with a
     * 24-byte atom-style header (size/frma/alac/size/alac/reserved) before
     * its first real field - it is NOT what a 12-int32 fmtp array can
     * satisfy, and it turns out real shairport-sync never actually calls
     * alac_set_info() at all (confirmed by grepping its own source - the
     * only call site is inside alac.c itself). The real integration point,
     * ported directly from shairport-sync's player.c (init_alac_decoder,
     * ~line 292), is to set every alac_file field directly from the fmtp
     * array, then call alac_allocate_buffers(). */
    sess->decoder = alac_create(16, AIRPLAY_CHANNELS);
    if (sess->decoder == NULL) {
        fprintf(stderr, "[airplay_rtp] alac_create failed\n");
    } else {
        sess->decoder->setinfo_max_samples_per_frame = (uint32_t)sess->fmtp[1];
        sess->decoder->setinfo_7a = (uint8_t)sess->fmtp[2];
        sess->decoder->setinfo_sample_size = (uint8_t)sess->fmtp[3];
        sess->decoder->setinfo_rice_historymult = (uint8_t)sess->fmtp[4];
        sess->decoder->setinfo_rice_initialhistory = (uint8_t)sess->fmtp[5];
        sess->decoder->setinfo_rice_kmodifier = (uint8_t)sess->fmtp[6];
        sess->decoder->setinfo_7f = (uint8_t)sess->fmtp[7];
        sess->decoder->setinfo_80 = (uint16_t)sess->fmtp[8];
        sess->decoder->setinfo_82 = (uint32_t)sess->fmtp[9];
        sess->decoder->setinfo_86 = (uint32_t)sess->fmtp[10];
        sess->decoder->setinfo_8a_rate = (uint32_t)sess->fmtp[11];
        alac_allocate_buffers(sess->decoder);
    }

    while (sess->recording) {
        ssize_t n = recv(sess->audio_fd, packet, sizeof(packet), 0);
        const unsigned char *payload;
        size_t payload_len;
        int outsize = 0;

        if (n <= 0) {
            if (!sess->recording) break;
            continue; /* transient recv error/timeout - just retry */
        }
        if (n <= RTP_HEADER_LEN) continue; /* too short to be a real audio packet */

        payload = packet + RTP_HEADER_LEN;
        payload_len = (size_t)n - RTP_HEADER_LEN;
        if (payload_len > sizeof(decrypted)) continue;

        if (have_aes_key && sess->encrypted) {
            /* Real, confirmed-from-source encryption scope (player.c): only
             * the payload truncated down to a whole 16-byte block is
             * actually AES-CBC decrypted; any trailing remainder passes
             * through unencrypted. The session IV is reused unchanged for
             * every packet (does not chain across packets) - each call
             * gets a fresh copy since mbedtls_aes_crypt_cbc mutates its iv
             * argument in place as it processes blocks. */
            unsigned char iv[16];
            size_t aeslen = payload_len & ~(size_t)0xf;
            memcpy(iv, sess->aes_iv, 16);
            if (aeslen > 0) {
                mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, aeslen, iv, payload, decrypted);
            }
            if (payload_len > aeslen) {
                memcpy(decrypted + aeslen, payload + aeslen, payload_len - aeslen);
            }
        } else {
            memcpy(decrypted, payload, payload_len);
        }

        if (sess->decoder != NULL) {
            /* Real bug caught before compiling: alac_decode_frame's
             * outputsize is an IN/OUT byte count, not a frame count and
             * not output-only - on input it must already hold the output
             * buffer's real capacity in bytes (alac.c checks
             * "*outputsize > outbuffer_allocation_size" and zeros the
             * result if the decode would overflow it), and on output it's
             * already the number of PCM BYTES written (outputsamples *
             * alac->bytespersample - already includes both channels), not
             * a per-channel sample/frame count needing further
             * multiplication. */
            outsize = (int)sizeof(pcm);
            alac_decode_frame(sess->decoder, decrypted, pcm, &outsize);
        }

        if (outsize > 0) {
            size_t pcm_bytes = (size_t)outsize;
            if (sess->output == NULL) {
                sess->output = coreaudio_output_open(AIRPLAY_SAMPLE_RATE, AIRPLAY_CHANNELS);
                if (sess->output == NULL) {
                    fprintf(stderr, "[airplay_rtp] coreaudio_output_open failed\n");
                } else {
                    fprintf(stderr, "[airplay_rtp] real audio output opened (%d Hz, %d ch)\n",
                            AIRPLAY_SAMPLE_RATE, AIRPLAY_CHANNELS);
                    /* Apply whatever volume was already set (a real
                     * SET_PARAMETER can arrive right after RECORD, before
                     * this first real packet) - see airplay_session.h's
                     * volume_linear comment. */
                    coreaudio_output_set_volume(sess->output, sess->volume_linear);
                }
            }
            if (sess->output != NULL) {
                coreaudio_output_write(sess->output, pcm, pcm_bytes);
            }
        }
    }

    if (have_aes_key) mbedtls_aes_free(&aes);
    fprintf(stderr, "[airplay_rtp] receiver thread exiting\n");
    sess->rtp_thread_running = 0;
    return NULL;
}

void airplay_rtp_start(airplay_session *sess)
{
    pthread_attr_t attr;

    if (sess->audio_fd < 0) {
        fprintf(stderr, "[airplay_rtp] cannot start - no audio socket (SETUP not done?)\n");
        return;
    }

    sess->recording = 1;
    sess->rtp_thread_running = 1;

    /* 4MB stack - see airplay_rtp.h's comment on why. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    pthread_create(&sess->rtp_thread, &attr, rtp_thread_main, sess);
    pthread_attr_destroy(&attr);
}

void airplay_rtp_stop(airplay_session *sess)
{
    if (!sess->rtp_thread_running) return;
    sess->recording = 0;
    pthread_join(sess->rtp_thread, NULL);
}
