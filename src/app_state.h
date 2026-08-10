#ifndef PPC_SPOTI_APP_STATE_H
#define PPC_SPOTI_APP_STATE_H

#include <time.h>
#include <pthread.h>

/*
 * app_state - the seam between the headless AirPlay backend
 * (airplay_main.c/airplay_rtsp.c) and the Cocoa UI (cocoa_ui.m). The
 * backend never touches AppKit directly - touching NSTextField/
 * NSImageView/etc. from a pthread is not safe on this era's AppKit (no
 * GCD/dispatch_async on this SDK to hop threads). Instead, backend code
 * calls the setters below at the same points it already changes its own
 * internal state, and the Cocoa side polls a lock-free snapshot
 * (app_state_get_snapshot) from an NSTimer on the main thread. No caller
 * ever calls into CoreAudio/Cocoa while holding the lock - every setter
 * here only locks, mutates plain fields, and unlocks.
 */

typedef enum {
    APP_BACKEND_NONE = 0,
    APP_BACKEND_AIRPLAY
} app_backend;

#define APP_TRACK_TITLE_MAX 256
#define APP_TRACK_ARTIST_MAX 512
#define APP_TRACK_ALBUM_MAX 256
#define APP_ART_PATH_MAX 1024
#define APP_PEER_NAME_MAX 256
#define APP_PEER_IP_MAX 64
#define APP_DEVICE_NAME_MAX 256

typedef struct {
    char title[APP_TRACK_TITLE_MAX];
    char artist[APP_TRACK_ARTIST_MAX];
    char album[APP_TRACK_ALBUM_MAX];
    char album_art_path[APP_ART_PATH_MAX]; /* local cache file; empty = none yet */
    unsigned long duration_ms;
} app_track_info;

typedef struct {
    pthread_mutex_t lock;

    app_backend active_backend;
    int is_playing;
    int is_paused;
    int have_track;
    app_track_info track;

    /* Position bookkeeping - independent of the backend's own tracking,
     * so the UI has its own copy without reaching into backend internals. */
    time_t track_started_at;
    time_t paused_at;
    unsigned long paused_accum_ms;

    char peer_name[APP_PEER_NAME_MAX];  /* may be empty - not always available, see cocoa_ui.m */
    char peer_ip[APP_PEER_IP_MAX];
    int peer_connected;

    /* Identifies which track album_art_path setters below apply to - not
     * shown in the UI, purely internal bookkeeping so a slow cover-art
     * fetch for a track that has since ended/changed can't stamp its
     * result onto whatever track is current by the time it finishes. */
    char current_track_key[40];

    char device_name[APP_DEVICE_NAME_MAX];
} app_state;

/* The one process-wide instance. */
app_state *app_state_shared(void);

void app_state_init(app_state *st);

void app_state_set_backend(app_state *st, app_backend backend);
/* Only resets active_backend to NONE if it still equals `backend` (i.e.
 * nothing else has taken over since) - see airplay_main.c's use at
 * connection-end. */
void app_state_clear_backend_if(app_state *st, app_backend backend);
void app_state_set_track(app_state *st, const app_track_info *info);
void app_state_clear_track(app_state *st);

/* `key` is an opaque per-track identifier - set once alongside
 * app_state_set_track, then passed again to app_state_set_album_art_path
 * once a decoupled cover-art fetch finishes, so a slow/late art fetch for
 * a track that has since ended or been replaced can't stamp its result
 * onto whatever's current by then (silently dropped instead). */
void app_state_set_track_key(app_state *st, const char *key);
/* Updates only the album-art path (leaving title/artist/album/duration
 * untouched) if `key` still matches the current track. */
void app_state_set_album_art_path(app_state *st, const char *key, const char *path);

/* Starts/resumes the position clock (track_started_at et al). */
void app_state_set_playing(app_state *st);
void app_state_set_paused(app_state *st);
void app_state_set_stopped(app_state *st);
unsigned long app_state_get_position_ms(app_state *st);

void app_state_set_peer(app_state *st, const char *name, const char *ip);
void app_state_clear_peer(app_state *st);

void app_state_set_device_name(app_state *st, const char *name);

/* UI never touches the struct directly - copies out under the lock into a
 * plain snapshot, so no Cocoa call ever happens while the mutex is held. */
typedef struct {
    app_backend active_backend;
    int is_playing;
    int is_paused;
    int have_track;
    app_track_info track;
    unsigned long position_ms;
    char peer_name[APP_PEER_NAME_MAX];
    char peer_ip[APP_PEER_IP_MAX];
    int peer_connected;
    char device_name[APP_DEVICE_NAME_MAX];
} app_state_snapshot;

void app_state_get_snapshot(app_state *st, app_state_snapshot *out);

#endif
