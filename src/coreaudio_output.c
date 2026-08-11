#include "coreaudio_output.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudio.h>

#define NUM_BUFFERS 8
#define BUFFER_BYTES (16 * 1024)
#define HAL_RING_BYTES (256 * 1024)

/* AudioQueue (AudioToolbox) is a real Leopard-only API - it does not exist
 * in Mac OS X 10.4 Tiger's AudioToolbox at all, confirmed via a real crash:
 * cctools' ld64 resolves the symbol reference at static link time against
 * the 10.5 SDK's stub library with no error, but Tiger's dyld leaves the
 * resulting lazy symbol pointer at 0 instead of refusing to launch, and the
 * first real call jumps straight to address 0 (same silent-null pattern
 * this project already hit once before with the Leopard-only
 * LSSharedFileList Login Items API). #pragma weak makes that intentional
 * instead of accidental: it marks the symbol as an allowed-missing weak
 * import, so checking "AudioQueueNewOutput != NULL" below is a well-defined
 * way to detect whether this OS actually has it, rather than relying on
 * this toolchain's already-observed-but-unofficial leniency. */
#pragma weak AudioQueueNewOutput

struct coreaudio_output {
    int use_hal;

    /* AudioQueue path (Mac OS X 10.5+) */
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[NUM_BUFFERS];
    int buffer_free[NUM_BUFFERS]; /* 1 = ours to fill, 0 = queued with CoreAudio */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int started;
    int paused;
    int stopping;

    /* HAL device-IOProc path (pre-Leopard - back to 10.0) - a byte ring
     * buffer feeding a pull-based render callback, since AudioDeviceAddIOProc
     * asks us for the next chunk rather than letting us hand buffers ahead
     * of time the way AudioQueueEnqueueBuffer does. */
    AudioDeviceID hal_device;
    int hal_running;
    int hal_paused;
    unsigned char *ring_buf;
    size_t ring_capacity;
    size_t ring_head;
    size_t ring_tail;
    size_t ring_filled;
    pthread_mutex_t ring_lock;
    pthread_cond_t ring_cond;
    int ring_stopping;
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

static int coreaudio_output_open_aq(coreaudio_output *out, int sample_rate, int channels)
{
    AudioStreamBasicDescription fmt;
    OSStatus status;
    int i;

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
        return -1;
    }

    for (i = 0; i < NUM_BUFFERS; i++) {
        status = AudioQueueAllocateBuffer(out->queue, BUFFER_BYTES, &out->buffers[i]);
        if (status != noErr) {
            fprintf(stderr, "[coreaudio] AudioQueueAllocateBuffer[%d] failed: %ld\n", i, (long)status);
            AudioQueueDispose(out->queue, true);
            return -1;
        }
        out->buffer_free[i] = 1;
    }

    fprintf(stderr, "[coreaudio] queue opened: %d Hz, %d ch, %d buffers of %d bytes\n",
            sample_rate, channels, NUM_BUFFERS, BUFFER_BYTES);

    return 0;
}

/* Runs on CoreAudio's own real-time HAL I/O thread, pulled on demand rather
 * than pushed ahead of time - fills exactly outputData->mBuffers[0] from
 * the ring buffer, padding with silence on underrun (a slow decoder should
 * cause an audible gap, not read of uninitialized/stale memory). */
static OSStatus hal_render_proc(AudioDeviceID device, const AudioTimeStamp *now,
                                 const AudioBufferList *inputData, const AudioTimeStamp *inputTime,
                                 AudioBufferList *outputData, const AudioTimeStamp *outputTime,
                                 void *clientData)
{
    coreaudio_output *out = (coreaudio_output *)clientData;
    unsigned char *dst = (unsigned char *)outputData->mBuffers[0].mData;
    UInt32 want = outputData->mBuffers[0].mDataByteSize;
    size_t take;
    size_t i;
    (void)device;
    (void)now;
    (void)inputData;
    (void)inputTime;
    (void)outputTime;

    pthread_mutex_lock(&out->ring_lock);
    take = (out->ring_filled < (size_t)want) ? out->ring_filled : (size_t)want;
    for (i = 0; i < take; i++) {
        dst[i] = out->ring_buf[out->ring_head];
        out->ring_head = (out->ring_head + 1) % out->ring_capacity;
    }
    out->ring_filled -= take;
    pthread_cond_broadcast(&out->ring_cond);
    pthread_mutex_unlock(&out->ring_lock);

    if (take < (size_t)want) {
        memset(dst + take, 0, (size_t)want - take);
    }

    return kAudioHardwareNoError;
}

static int coreaudio_output_open_hal(coreaudio_output *out, int sample_rate, int channels)
{
    UInt32 size = sizeof(out->hal_device);
    OSStatus status;
    AudioStreamBasicDescription fmt;

    status = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &size, &out->hal_device);
    if (status != noErr || out->hal_device == kAudioDeviceUnknown) {
        fprintf(stderr, "[coreaudio-hal] no default output device: %ld\n", (long)status);
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = (Float64)sample_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mBitsPerChannel = 16;
    fmt.mChannelsPerFrame = (UInt32)channels;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = (UInt32)(channels * 2);
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;

    /* This is the direct/simple path: ask the device to accept our PCM
     * format outright, same as real pre-AudioQueue apps (iTunes, QuickTime)
     * did on this era's hardware. If a real device out there rejects 16-bit
     * interleaved PCM and only speaks its own native format (typically
     * Float32), this fails cleanly here rather than silently producing
     * garbled audio - the fix at that point is a real sample-format
     * conversion step in hal_render_proc, added once a real device actually
     * demonstrates the need for it, not guessed at now. */
    status = AudioDeviceSetProperty(out->hal_device, NULL, 0, false,
                                     kAudioDevicePropertyStreamFormat, sizeof(fmt), &fmt);
    if (status != noErr) {
        fprintf(stderr, "[coreaudio-hal] device rejected %d Hz %d ch 16-bit PCM format directly "
                        "(status %ld) - needs sample-format conversion support, not yet implemented\n",
                sample_rate, channels, (long)status);
        return -1;
    }

    out->ring_capacity = HAL_RING_BYTES;
    out->ring_buf = (unsigned char *)malloc(out->ring_capacity);
    if (out->ring_buf == NULL) return -1;
    out->ring_head = 0;
    out->ring_tail = 0;
    out->ring_filled = 0;
    pthread_mutex_init(&out->ring_lock, NULL);
    pthread_cond_init(&out->ring_cond, NULL);

    status = AudioDeviceAddIOProc(out->hal_device, hal_render_proc, out);
    if (status != noErr) {
        fprintf(stderr, "[coreaudio-hal] AudioDeviceAddIOProc failed: %ld\n", (long)status);
        free(out->ring_buf);
        return -1;
    }

    fprintf(stderr, "[coreaudio-hal] HAL device output opened (%d Hz, %d ch) - pre-Leopard audio path\n",
            sample_rate, channels);

    return 0;
}

coreaudio_output *coreaudio_output_open(int sample_rate, int channels)
{
    coreaudio_output *out = (coreaudio_output *)calloc(1, sizeof(coreaudio_output));
    int ok;

    if (out == NULL) return NULL;

    out->use_hal = (AudioQueueNewOutput == NULL);
    ok = out->use_hal ? coreaudio_output_open_hal(out, sample_rate, channels)
                       : coreaudio_output_open_aq(out, sample_rate, channels);
    if (ok != 0) {
        free(out);
        return NULL;
    }

    return out;
}

/* Blocks (via the mutex/cond above) until a queue buffer is free, copies up
 * to BUFFER_BYTES of `pcm` into it, and enqueues it for playback. Called
 * repeatedly by the caller with successive chunks of decoded PCM - this is
 * why NUM_BUFFERS/BUFFER_BYTES need to give CoreAudio enough lookahead to
 * avoid underruns on a 700MHz machine that's also busy decoding/downloading. */
static int coreaudio_output_write_aq(coreaudio_output *out, const unsigned char *pcm, size_t len)
{
    size_t offset = 0;

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

/* Blocks until ring buffer space frees up (mirrors coreaudio_output_write_aq's
 * blocking contract) - hal_render_proc drains it from the HAL's own thread. */
static int coreaudio_output_write_hal(coreaudio_output *out, const unsigned char *pcm, size_t len)
{
    size_t offset = 0;

    if (!out->hal_running) {
        OSStatus status = AudioDeviceStart(out->hal_device, hal_render_proc);
        if (status != noErr) {
            fprintf(stderr, "[coreaudio-hal] AudioDeviceStart failed: %ld\n", (long)status);
            return -1;
        }
        out->hal_running = 1;
    }

    pthread_mutex_lock(&out->ring_lock);
    while (offset < len) {
        size_t free_bytes = out->ring_capacity - out->ring_filled;
        size_t chunk;
        size_t i;

        if (out->ring_stopping) { pthread_mutex_unlock(&out->ring_lock); return -1; }

        if (free_bytes == 0) {
            pthread_cond_wait(&out->ring_cond, &out->ring_lock);
            continue;
        }

        chunk = len - offset;
        if (chunk > free_bytes) chunk = free_bytes;

        for (i = 0; i < chunk; i++) {
            out->ring_buf[out->ring_tail] = pcm[offset + i];
            out->ring_tail = (out->ring_tail + 1) % out->ring_capacity;
        }
        out->ring_filled += chunk;
        offset += chunk;
    }
    pthread_mutex_unlock(&out->ring_lock);

    return 0;
}

int coreaudio_output_write(coreaudio_output *out, const unsigned char *pcm, size_t len)
{
    if (out == NULL) return -1;
    return out->use_hal ? coreaudio_output_write_hal(out, pcm, len)
                         : coreaudio_output_write_aq(out, pcm, len);
}

int coreaudio_output_pause(coreaudio_output *out)
{
    OSStatus status;

    if (out == NULL) return 0;

    if (out->use_hal) {
        if (!out->hal_running || out->hal_paused) return 0;
        status = AudioDeviceStop(out->hal_device, hal_render_proc);
        if (status != noErr) {
            fprintf(stderr, "[coreaudio-hal] AudioDeviceStop failed: %ld\n", (long)status);
            return -1;
        }
        out->hal_paused = 1;
        return 0;
    }

    if (!out->started || out->paused) return 0;
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

    if (out == NULL) return 0;

    if (out->use_hal) {
        if (!out->hal_running || !out->hal_paused) return 0;
        status = AudioDeviceStart(out->hal_device, hal_render_proc);
        if (status != noErr) {
            fprintf(stderr, "[coreaudio-hal] AudioDeviceStart (resume) failed: %ld\n", (long)status);
            return -1;
        }
        out->hal_paused = 0;
        return 0;
    }

    if (!out->started || !out->paused) return 0;
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

    if (out->use_hal) {
        Float32 vol = linear_gain;
        /* Master channel (0) volume isn't exposed on every device - many
         * expose only per-channel (1, 2, ...) volume instead. */
        status = AudioDeviceSetProperty(out->hal_device, NULL, 0, false,
                                         kAudioDevicePropertyVolumeScalar, sizeof(vol), &vol);
        if (status != noErr) {
            OSStatus s1 = AudioDeviceSetProperty(out->hal_device, NULL, 1, false,
                                                  kAudioDevicePropertyVolumeScalar, sizeof(vol), &vol);
            OSStatus s2 = AudioDeviceSetProperty(out->hal_device, NULL, 2, false,
                                                  kAudioDevicePropertyVolumeScalar, sizeof(vol), &vol);
            if (s1 != noErr && s2 != noErr) {
                fprintf(stderr, "[coreaudio-hal] AudioDeviceSetProperty(Volume) failed on master/ch1/ch2: %ld/%ld/%ld\n",
                        (long)status, (long)s1, (long)s2);
                return -1;
            }
        }
        return 0;
    }

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

    if (out->use_hal) {
        pthread_mutex_lock(&out->ring_lock);
        out->ring_stopping = 1;
        pthread_cond_broadcast(&out->ring_cond);
        pthread_mutex_unlock(&out->ring_lock);
        return;
    }

    pthread_mutex_lock(&out->lock);
    out->stopping = 1;
    pthread_cond_broadcast(&out->cond);
    pthread_mutex_unlock(&out->lock);
}

static void coreaudio_output_close_hal(coreaudio_output *out)
{
    if (out->hal_running && !out->ring_stopping) {
        /* Wait for the ring buffer to fully drain (mirrors the AudioQueue
         * path's own drain-before-stop wait) so the tail of the track
         * isn't cut off. */
        pthread_mutex_lock(&out->ring_lock);
        while (out->ring_filled > 0) {
            pthread_cond_wait(&out->ring_cond, &out->ring_lock);
        }
        pthread_mutex_unlock(&out->ring_lock);
    }

    AudioDeviceStop(out->hal_device, hal_render_proc);
    AudioDeviceRemoveIOProc(out->hal_device, hal_render_proc);
    pthread_mutex_destroy(&out->ring_lock);
    pthread_cond_destroy(&out->ring_cond);
    free(out->ring_buf);
}

void coreaudio_output_close(coreaudio_output *out)
{
    int i, all_free;

    if (out == NULL) return;

    if (out->use_hal) {
        coreaudio_output_close_hal(out);
        free(out);
        return;
    }

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
