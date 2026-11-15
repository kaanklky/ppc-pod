#ifndef PPC_SPOTI_APP_SETTINGS_H
#define PPC_SPOTI_APP_SETTINGS_H

#include <stddef.h>

/*
 * app_settings - persists the user-visible device name and the shutdown
 * listener options across restarts, in a plain key=value text file.
 * Deliberately a small fixed set of fields (device_name, shutdown_listener,
 * shutdown_port, shutdown_key) - not a general preferences store.
 */

#define APP_SETTINGS_DEVICE_NAME_MAX 256
#define APP_SETTINGS_SHUTDOWN_KEY_MAX 128
#define APP_SETTINGS_SHUTDOWN_PORT_DEFAULT 8990
#define APP_SETTINGS_SHUTDOWN_KEY_DEFAULT "wololo"

typedef struct {
    char device_name[APP_SETTINGS_DEVICE_NAME_MAX];
    int shutdown_listener;
    int shutdown_port;
    char shutdown_key[APP_SETTINGS_SHUTDOWN_KEY_MAX];
} app_settings;

void app_settings_defaults(app_settings *s);

/* ~/Library/Application Support/PowerPC Pod/settings.txt */
void app_settings_default_path(char *out, size_t out_cap);

/* First-launch fallback device name, derived from the machine's real
 * hostname (via gethostname(), ".local" suffix stripped) - used only when
 * app_settings_read() below finds no settings file yet. */
void app_settings_hostname_default(char *out, size_t out_cap);

/* Always fills `s` with the defaults first (shutdown_listener=off,
 * shutdown_port=8990, shutdown_key=wololo, device_name empty), then
 * overrides whatever the file at `path` actually sets - a missing line
 * keeps its default, an explicit empty shutdown_key= stays empty. Returns
 * 0 if the file exists, -1 if it doesn't (`s` still holds the defaults) -
 * callers should treat that as normal first-launch state, not a fatal
 * error. An empty device_name means none was saved - callers fall back to
 * app_settings_hostname_default(). */
int app_settings_read(const char *path, app_settings *s);

int app_settings_ensure_defaults(const char *path);

int app_settings_set_device_name(const char *path, const char *name);

#endif
