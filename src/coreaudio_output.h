#ifndef PPC_SPOTI_COREAUDIO_OUTPUT_H
#define PPC_SPOTI_COREAUDIO_OUTPUT_H

#include <stddef.h>

/*
 * Real audio output, with two backends chosen automatically at runtime
 * depending on which OS actually loaded the binary: AudioQueue
 * (AudioToolbox.framework) on Mac OS X 10.5+, since that API doesn't exist
 * before Leopard, and the older AudioDeviceAddIOProc HAL device API
 * (CoreAudio.framework) as a fallback that works back to 10.0. Callers
 * don't need to know or care which one is actually in use.
 */

typedef struct coreaudio_output coreaudio_output;

/* Opens an AudioQueue output for 16-bit signed little-endian PCM at the
 * given sample rate/channel count (matching what alac.c's decode
 * produces). Returns NULL on failure. */
coreaudio_output *coreaudio_output_open(int sample_rate, int channels);

/* Enqueues one buffer of already-decoded 16-bit PCM for playback. Returns
 * 0 on success, negative on error. */
int coreaudio_output_write(coreaudio_output *out, const unsigned char *pcm, size_t len);

int coreaudio_output_pause(coreaudio_output *out);

int coreaudio_output_resume(coreaudio_output *out);

/* Sets linear gain (0.0 = silent, 1.0 = full volume, matching
 * AudioQueue's own kAudioQueueParam_Volume range) - added for real AirPlay
 * SET_PARAMETER volume support (airplay_rtsp.c converts the wire protocol's
 * -30.0..0.0 dB / -144.0-mute convention to this linear range before
 * calling in). Returns 0 on success, negative on error. */
int coreaudio_output_set_volume(coreaudio_output *out, float linear_gain);

void coreaudio_output_interrupt(coreaudio_output *out);

void coreaudio_output_close(coreaudio_output *out);

#endif
