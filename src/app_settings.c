#include "app_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ~/Library/Application Support/<AppName>/ is the standard Mac OS X
 * location for a per-user app data directory that holds more than a
 * single simple preference value (settings.txt here, plus a whole
 * artcache/ subdirectory of cached cover art - see airplay_rtsp.c). A
 * single simple value alone would more idiomatically live in
 * ~/Library/Preferences/<bundle-id>.plist via NSUserDefaults, but that
 * doesn't fit well alongside a cache directory. Hand-rolled in plain C
 * since this project has no Foundation/NSFileManager dependency
 * elsewhere. */
void app_settings_default_path(char *out, size_t out_cap)
{
    const char *home = getenv("HOME");
    if (home == NULL) home = "";
    snprintf(out, out_cap, "%s/Library/Application Support/PowerPC Pod/settings.txt", home);
}

/* First-launch default device name, used only when no settings.txt exists
 * yet. A Mac's configured hostname commonly comes back with a ".local"
 * suffix (e.g. "imacg4.local") - stripped since it reads as noise in a
 * plain friendly-name textbox. Falls back to a fixed name if
 * gethostname() fails or returns empty. */
void app_settings_hostname_default(char *out, size_t out_cap)
{
    char host[256];
    char *dot;

    if (gethostname(host, sizeof(host)) != 0 || host[0] == '\0') {
        snprintf(out, out_cap, "PowerPC Pod");
        return;
    }
    host[sizeof(host) - 1] = '\0';
    dot = strchr(host, '.');
    if (dot != NULL) *dot = '\0';
    if (host[0] == '\0') {
        snprintf(out, out_cap, "PowerPC Pod");
        return;
    }
    snprintf(out, out_cap, "%s", host);
}

/* Plain mkdir() only creates the last path component and fails if its own
 * parent doesn't exist yet, unlike `mkdir -p`. Walk the path and mkdir()
 * every component in order instead. */
static void mkdir_recursive(char *path)
{
    char *p;
    for (p = path + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755); /* ignore EEXIST/other errors - best effort */
            *p = '/';
        }
    }
    mkdir(path, 0755);
}

static void ensure_parent_dir(const char *file_path)
{
    char dir[1024];
    char *slash;
    snprintf(dir, sizeof(dir), "%s", file_path);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        mkdir_recursive(dir);
    }
}

int app_settings_read(const char *path, char *device_name_out, size_t cap)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int found = -1;

    if (f == NULL) return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (strncmp(line, "device_name=", 12) == 0) {
            snprintf(device_name_out, cap, "%s", line + 12);
            found = 0;
        }
    }
    fclose(f);
    return found;
}

int app_settings_write(const char *path, const char *device_name)
{
    FILE *f;
    ensure_parent_dir(path);
    f = fopen(path, "w");
    if (f == NULL) return -1;
    fprintf(f, "device_name=%s\n", device_name);
    fclose(f);
    return 0;
}
