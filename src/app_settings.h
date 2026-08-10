#ifndef PPC_SPOTI_APP_SETTINGS_H
#define PPC_SPOTI_APP_SETTINGS_H

#include <stddef.h>

/*
 * app_settings - persists the user-visible device name across restarts, in
 * a plain key=value text file. Deliberately just one field today
 * (device_name) - not a general preferences store.
 */

#define APP_SETTINGS_DEVICE_NAME_MAX 256

/* ~/Library/Application Support/PowerPC Pod/settings.txt */
void app_settings_default_path(char *out, size_t out_cap);

/* First-launch fallback device name, derived from the machine's real
 * hostname (via gethostname(), ".local" suffix stripped) - used only when
 * app_settings_read() below finds no settings file yet. */
void app_settings_hostname_default(char *out, size_t out_cap);

/* Returns 0 and fills device_name_out if the file exists and has a
 * device_name= line; returns -1 (device_name_out left untouched) if the
 * file doesn't exist yet or has no such line - callers should fall back to
 * a sensible default in that case, not treat it as a fatal error. */
int app_settings_read(const char *path, char *device_name_out, size_t cap);

/* Creates the parent directory if needed (matching ensure_parent_dir's
 * best-effort semantics). Returns 0 on success, -1 on failure to open the
 * file for writing. */
int app_settings_write(const char *path, const char *device_name);

#endif
