#include "coreaudio_output.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include <AudioToolbox/AudioQueue.h>

#define NUM_BUFFERS 8
#define BUFFER_BYTES (16 * 1024)

struct coreaudio_output {
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[NUM_BUFFERS];
    int buffer_free[NUM_BUFFERS]; /* 1 = ours to fill, 0 = queued with CoreAudio */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int started;
    int paused;
    int stopping;
};

/* Called on CoreAudio's own internal playback thread whenever a buffer it
 * was playing has been fully consumed and is available for reuse. Must not
 * block for long - just mark the buffer free and wake up any writer
 * blocked waiting for one. */
static void output_callback(void *user_data, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    coreaudio_output *out = (coreaudio_output *)user_data;
    int i;
    (void)queue;

    pthread_mutex_lock(&out->lock);
    for (i = 0; i < NUM_BUFFERS; i++) {
        if (out->buffers[i] == buffer) {
            out->buffer_free[i] = 1;
            break;
        }
    }
    pthread_cond_broadcast(&out->cond);
    pthread_mutex_unlock(&out->lock);
}

coreaudio_output *coreaudio_output_open(int sample_rate, int channels)
{
    coreaudio_output *out = (coreaudio_output *)calloc(1, sizeof(coreaudio_output));
    AudioStreamBasicDescription fmt;
    OSStatus status;
    int i;

    if (out == NULL) return NULL;

    pthread_mutex_init(&out->lock, NULL);
    pthread_cond_init(&out->cond, NULL);

    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = (Float64)sample_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    /* No kLinearPCMFormatFlagIsBigEndian: the decode pipeline calls
     * ov_read() with bigendianp=0, so the PCM handed to us is always
     * little-endian regardless of this (big-endian) host's native order -
     * AudioQueue must be told the buffer contents are little-endian, not
     * "whatever the host is". Confirmed against audio_pipeline_test.c's
     * ov_read call before writing this, not assumed. */
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mBitsPerChannel = 16;
    fmt.mChannelsPerFrame = (UInt32)channels;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = (UInt32)(channels * 2);
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;

    status = AudioQueueNewOutput(&fmt, output_callback, out, NULL, NULL, 0, &out->queue);
    if (status != noErr) {
        fprintf(stderr, "[coreaudio] AudioQueueNewOutput failed: %ld\n", (long)status);
        free(out);
        return NULL;
    }

    for (i = 0; i < NUM_BUFFERS; i++) {
        status = AudioQueueAllocateBuffer(out->queue, BUFFER_BYTES, &out->buffers[i]);
        if (status != noErr) {
            fprintf(stderr, "[coreaudio] AudioQueueAllocateBuffer[%d] failed: %ld\n", i, (long)status);
            AudioQueueDispose(out->queue, true);
            free(out);
            return NULL;
        }
        out->buffer_free[i] = 1;
    }

    fprintf(stderr, "[coreaudio] queue opened: %d Hz, %d ch, %d buffers of %d bytes\n",
            sample_rate, channels, NUM_BUFFERS, BUFFER_BYTES);

    return out;
}

/* Blocks (via the mutex/cond above) until a queue buffer is free, copies up
 * to BUFFER_BYTES of `pcm` into it, and enqueues it for playback. Called
 * repeatedly by the caller with successive chunks of decoded PCM - this is
 * why NUM_BUFFERS/BUFFER_BYTES need to give CoreAudio enough lookahead to
 * avoid underruns on a 700MHz machine that's also busy decoding/downloading. */
int coreaudio_output_write(coreaudio_output *out, const unsigned char *pcm, size_t len)
{
    size_t offset = 0;

    if (out == NULL) return -1;

    if (!out->started) {
        OSStatus status = AudioQueueStart(out->queue, NULL);
        if (status != noErr) {
            fprintf(stderr, "[coreaudio] AudioQueueStart failed: %ld\n", (long)status);
            return -1;
        }
        out->started = 1;
    }

    while (offset < len) {
        size_t chunk = len - offset;
        int slot = -1;
        int i;
        AudioQueueBufferRef buf;
        OSStatus status;

        if (chunk > BUFFER_BYTES) chunk = BUFFER_BYTES;

        pthread_mutex_lock(&out->lock);
        for (;;) {
            if (out->stopping) { pthread_mutex_unlock(&out->lock); return -1; }
            for (i = 0; i < NUM_BUFFERS; i++) {
                if (out->buffer_free[i]) { slot = i; break; }
            }
            if (slot >= 0) break;
            pthread_cond_wait(&out->cond, &out->lock);
        }
        out->buffer_free[slot] = 0;
        pthread_mutex_unlock(&out->lock);

        buf = out->buffers[slot];
        memcpy(buf->mAudioData, pcm + offset, chunk);
        buf->mAudioDataByteSize = (UInt32)chunk;

        status = AudioQueueEnqueueBuffer(out->queue, buf, 0, NULL);
        if (status != noErr) {
            fprintf(stderr, "[coreaudio] AudioQueueEnqueueBuffer failed: %ld\n", (long)status);
            return -1;
        }

        offset += chunk;
    }

    return 0;
}

int coreaudio_output_pause(coreaudio_output *out)
{
    OSStatus status;

    if (out == NULL || !out->started || out->paused) return 0;

    status = AudioQueuePause(out->queue);
    if (status != noErr) {
        fprintf(stderr, "[coreaudio] AudioQueuePause failed: %ld\n", (long)status);
        return -1;
    }
    out->paused = 1;
    return 0;
}

int coreaudio_output_resume(coreaudio_output *out)
{
    OSStatus status;

    if (out == NULL || !out->started || !out->paused) return 0;

    status = AudioQueueStart(out->queue, NULL);
    if (status != noErr) {
        fprintf(stderr, "[coreaudio] AudioQueueStart (resume) failed: %ld\n", (long)status);
        return -1;
    }
    out->paused = 0;
    return 0;
}

int coreaudio_output_set_volume(coreaudio_output *out, float linear_gain)
{
    OSStatus status;

    if (out == NULL) return -1;
    if (linear_gain < 0.0f) linear_gain = 0.0f;
    if (linear_gain > 1.0f) linear_gain = 1.0f;

    status = AudioQueueSetParameter(out->queue, kAudioQueueParam_Volume, linear_gain);
    if (status != noErr) {
        fprintf(stderr, "[coreaudio] AudioQueueSetParameter(Volume) failed: %ld\n", (long)status);
        return -1;
    }
    return 0;
}

void coreaudio_output_interrupt(coreaudio_output *out)
{
    if (out == NULL) return;

    pthread_mutex_lock(&out->lock);
    out->stopping = 1;
    pthread_cond_broadcast(&out->cond);
    pthread_mutex_unlock(&out->lock);
}

void coreaudio_output_close(coreaudio_output *out)
{
    int i, all_free;

    if (out == NULL) return;

    if (out->started && !out->stopping) {
        /* Wait for every buffer to come back free, i.e. everything queued
         * has actually finished playing, before stopping - AudioQueueStop
         * with inImmediate=true (below) discards anything still queued, so
         * without this wait the tail of the track would be cut off. */
        pthread_mutex_lock(&out->lock);
        for (;;) {
            all_free = 1;
            for (i = 0; i < NUM_BUFFERS; i++) {
                if (!out->buffer_free[i]) { all_free = 0; break; }
            }
            if (all_free) break;
            pthread_cond_wait(&out->cond, &out->lock);
        }
        pthread_mutex_unlock(&out->lock);
    }

    AudioQueueStop(out->queue, true);
    AudioQueueDispose(out->queue, true);
    pthread_mutex_destroy(&out->lock);
    pthread_cond_destroy(&out->cond);
    free(out);
}
