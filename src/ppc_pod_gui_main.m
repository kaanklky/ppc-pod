#include <pthread.h>
#include <signal.h>
#include <stdio.h>

#include "receiver_entry.h"
#include "app_state.h"
#include "app_settings.h"
#include "cocoa_ui.h"
#include "login_items.h"

#define PPC_POD_BIG_STACK_BYTES (4 * 1024 * 1024)

/* AirPlay's per-connection RTSP work needs a real 4MB stack (not the
 * default): AIRPLAY_MAX_BODY is sized to fit embedded cover art (see
 * airplay_rtsp.h), making airplay_rtsp_request a large struct as a stack
 * local. */
static void spawn_detached(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PPC_POD_BIG_STACK_BYTES);
    pthread_create(&t, &attr, fn, arg);
    pthread_detach(t);
    pthread_attr_destroy(&attr);
}

int main(int argc, char **argv)
{
    static char device_name[APP_SETTINGS_DEVICE_NAME_MAX];
    char settings_path[1024];

    (void)argc;
    (void)argv;

    /* An unhandled SIGPIPE from writing to a peer-reset connection would
     * otherwise silently kill the process with no crash report. */
    signal(SIGPIPE, SIG_IGN);

    app_settings_default_path(settings_path, sizeof(settings_path));
    if (app_settings_read(settings_path, device_name, sizeof(device_name)) != 0) {
        /* First launch, nothing saved yet - default to this Mac's real
         * hostname. cocoa_ui.m's buildUI independently does the same
         * read+fallback for the textbox's initial value, so both agree on
         * what shows up the first time the app runs. */
        app_settings_hostname_default(device_name, sizeof(device_name));
    }
    app_state_set_device_name(app_state_shared(), device_name);

    /* `device_name` is `static` (not a plain main()-stack local) precisely
     * because airplay_backend_thread_main keeps its `arg` pointer for its
     * whole lifetime, not just at thread-creation time. Renaming via the
     * UI's Save button does not hot-reload the already-running mDNS
     * advertisement - see app_settings.h's comment - so this one
     * read-at-startup is deliberately the only place the backend thread
     * learns the device name. */
    spawn_detached(airplay_backend_thread_main, device_name);

    register_self_as_login_item();

    cocoa_ui_run(); /* blocks - Cocoa's run loop owns this thread until the window closes */

    return 0;
}
