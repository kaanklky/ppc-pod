#include "app_state.h"

#include <string.h>

static app_state g_app_state;
static int g_app_state_initialized = 0;
static pthread_mutex_t g_app_state_init_lock = PTHREAD_MUTEX_INITIALIZER;

app_state *app_state_shared(void)
{
    pthread_mutex_lock(&g_app_state_init_lock);
    if (!g_app_state_initialized) {
        app_state_init(&g_app_state);
        g_app_state_initialized = 1;
    }
    pthread_mutex_unlock(&g_app_state_init_lock);
    return &g_app_state;
}

void app_state_init(app_state *st)
{
    memset(st, 0, sizeof(*st));
    pthread_mutex_init(&st->lock, NULL);
    st->active_backend = APP_BACKEND_NONE;
}

void app_state_set_backend(app_state *st, app_backend backend)
{
    pthread_mutex_lock(&st->lock);
    st->active_backend = backend;
    pthread_mutex_unlock(&st->lock);
}

void app_state_clear_backend_if(app_state *st, app_backend backend)
{
    pthread_mutex_lock(&st->lock);
    if (st->active_backend == backend) st->active_backend = APP_BACKEND_NONE;
    pthread_mutex_unlock(&st->lock);
}

void app_state_set_track(app_state *st, const app_track_info *info)
{
    pthread_mutex_lock(&st->lock);
    st->track = *info;
    st->have_track = 1;
    pthread_mutex_unlock(&st->lock);
}

void app_state_clear_track(app_state *st)
{
    pthread_mutex_lock(&st->lock);
    memset(&st->track, 0, sizeof(st->track));
    st->have_track = 0;
    st->is_playing = 0;
    st->is_paused = 0;
    st->track_started_at = 0;
    st->paused_at = 0;
    st->paused_accum_ms = 0;
    st->current_track_key[0] = '\0';
    pthread_mutex_unlock(&st->lock);
}

void app_state_set_track_key(app_state *st, const char *key)
{
    pthread_mutex_lock(&st->lock);
    strncpy(st->current_track_key, key, sizeof(st->current_track_key) - 1);
    st->current_track_key[sizeof(st->current_track_key) - 1] = '\0';
    pthread_mutex_unlock(&st->lock);
}

void app_state_set_album_art_path(app_state *st, const char *key, const char *path)
{
    pthread_mutex_lock(&st->lock);
    if (st->have_track && strcmp(st->current_track_key, key) == 0) {
        strncpy(st->track.album_art_path, path, sizeof(st->track.album_art_path) - 1);
        st->track.album_art_path[sizeof(st->track.album_art_path) - 1] = '\0';
    }
    pthread_mutex_unlock(&st->lock);
}

void app_state_set_playing(app_state *st)
{
    pthread_mutex_lock(&st->lock);
    if (st->is_paused && st->paused_at != 0) {
        st->paused_accum_ms += (unsigned long)(time(NULL) - st->paused_at) * 1000UL;
    }
    if (st->track_started_at == 0) {
        st->track_started_at = time(NULL);
    }
    st->is_playing = 1;
    st->is_paused = 0;
    st->paused_at = 0;
    pthread_mutex_unlock(&st->lock);
}

void app_state_set_paused(app_state *st)
{
    pthread_mutex_lock(&st->lock);
    st->is_paused = 1;
    st->paused_at = time(NULL);
    pthread_mutex_unlock(&st->lock);
}

void app_state_set_stopped(app_state *st)
{
    pthread_mutex_lock(&st->lock);
    st->is_playing = 0;
    st->is_paused = 0;
    st->track_started_at = 0;
    st->paused_at = 0;
    st->paused_accum_ms = 0;
    pthread_mutex_unlock(&st->lock);
}

unsigned long app_state_get_position_ms(app_state *st)
{
    unsigned long result;
    time_t now;
    long elapsed_s;

    pthread_mutex_lock(&st->lock);
    if (st->track_started_at == 0) {
        pthread_mutex_unlock(&st->lock);
        return 0;
    }
    now = st->is_paused ? st->paused_at : time(NULL);
    elapsed_s = (long)(now - st->track_started_at) - (long)(st->paused_accum_ms / 1000);
    if (elapsed_s < 0) elapsed_s = 0;
    result = (unsigned long)elapsed_s * 1000UL;
    pthread_mutex_unlock(&st->lock);
    return result;
}

void app_state_set_peer(app_state *st, const char *name, const char *ip)
{
    pthread_mutex_lock(&st->lock);
    if (name != NULL) {
        strncpy(st->peer_name, name, sizeof(st->peer_name) - 1);
        st->peer_name[sizeof(st->peer_name) - 1] = '\0';
    } else {
        st->peer_name[0] = '\0';
    }
    if (ip != NULL) {
        strncpy(st->peer_ip, ip, sizeof(st->peer_ip) - 1);
        st->peer_ip[sizeof(st->peer_ip) - 1] = '\0';
    } else {
        st->peer_ip[0] = '\0';
    }
    st->peer_connected = 1;
    pthread_mutex_unlock(&st->lock);
}

void app_state_clear_peer(app_state *st)
{
    pthread_mutex_lock(&st->lock);
    st->peer_name[0] = '\0';
    st->peer_ip[0] = '\0';
    st->peer_connected = 0;
    pthread_mutex_unlock(&st->lock);
}

void app_state_set_device_name(app_state *st, const char *name)
{
    pthread_mutex_lock(&st->lock);
    strncpy(st->device_name, name, sizeof(st->device_name) - 1);
    st->device_name[sizeof(st->device_name) - 1] = '\0';
    pthread_mutex_unlock(&st->lock);
}

void app_state_get_snapshot(app_state *st, app_state_snapshot *out)
{
    pthread_mutex_lock(&st->lock);
    out->active_backend = st->active_backend;
    out->is_playing = st->is_playing;
    out->is_paused = st->is_paused;
    out->have_track = st->have_track;
    out->track = st->track;
    memcpy(out->peer_name, st->peer_name, sizeof(out->peer_name));
    memcpy(out->peer_ip, st->peer_ip, sizeof(out->peer_ip));
    out->peer_connected = st->peer_connected;
    memcpy(out->device_name, st->device_name, sizeof(out->device_name));
    pthread_mutex_unlock(&st->lock);

    out->position_ms = app_state_get_position_ms(st);
}
