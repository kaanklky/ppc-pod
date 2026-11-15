#include "app_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

void app_settings_defaults(app_settings *s)
{
    memset(s, 0, sizeof(*s));
    s->shutdown_listener = 0;
    s->shutdown_port = APP_SETTINGS_SHUTDOWN_PORT_DEFAULT;
    snprintf(s->shutdown_key, sizeof(s->shutdown_key), "%s", APP_SETTINGS_SHUTDOWN_KEY_DEFAULT);
}

static const char *value_for(const char *line, const char *name)
{
    size_t n = strlen(name);
    if (strncmp(line, name, n) == 0 && line[n] == '=') return line + n + 1;
    return NULL;
}

static void trim_both(char *s)
{
    size_t l = strlen(s);
    size_t lead = 0;
    while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = '\0';
    while (s[lead] == ' ' || s[lead] == '\t') lead++;
    if (lead > 0) memmove(s, s + lead, strlen(s + lead) + 1);
}

static void apply_line(app_settings *s, const char *line)
{
    const char *v;
    char small[32];

    if ((v = value_for(line, "device_name")) != NULL) {
        snprintf(s->device_name, sizeof(s->device_name), "%s", v);
    } else if ((v = value_for(line, "shutdown_listener")) != NULL) {
        snprintf(small, sizeof(small), "%s", v);
        trim_both(small);
        if (strcasecmp(small, "on") == 0) s->shutdown_listener = 1;
        else if (strcasecmp(small, "off") == 0) s->shutdown_listener = 0;
    } else if ((v = value_for(line, "shutdown_port")) != NULL) {
        char *end;
        long port;
        snprintf(small, sizeof(small), "%s", v);
        trim_both(small);
        port = strtol(small, &end, 10);
        if (small[0] != '\0' && *end == '\0' && port >= 1 && port <= 65535) {
            s->shutdown_port = (int)port;
        }
    } else if ((v = value_for(line, "shutdown_key")) != NULL) {
        snprintf(s->shutdown_key, sizeof(s->shutdown_key), "%s", v);
    }
}

int app_settings_read(const char *path, app_settings *s)
{
    FILE *f;
    char line[512];

    app_settings_defaults(s);
    f = fopen(path, "r");
    if (f == NULL) return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        apply_line(s, line);
    }
    fclose(f);
    return 0;
}

#define APP_SETTINGS_FILE_MAX (64 * 1024)

static int read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long size;
    size_t n;
    char *buf;

    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || size > APP_SETTINGS_FILE_MAX ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -2;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return -2;
    }
    n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return 0;
}

static int line_is_key(const char *line, size_t line_len, const char *key)
{
    size_t klen = strlen(key);
    return line_len > klen && strncmp(line, key, klen) == 0 && line[klen] == '=';
}

static int text_has_key(const char *text, size_t len, const char *key)
{
    size_t pos = 0;

    while (pos < len) {
        const char *line = text + pos;
        size_t line_len = 0;
        while (pos + line_len < len && line[line_len] != '\n') line_len++;
        if (line_is_key(line, line_len, key)) return 1;
        pos += line_len + 1;
    }
    return 0;
}

int app_settings_ensure_defaults(const char *path)
{
    app_settings d;
    char *text = NULL;
    size_t len = 0;
    int rc = read_file(path, &text, &len);
    int need_listener;
    int need_port;
    int need_key;
    FILE *f;

    if (rc == -2) return -1;
    app_settings_defaults(&d);

    need_listener = (rc != 0) || !text_has_key(text, len, "shutdown_listener");
    need_port = (rc != 0) || !text_has_key(text, len, "shutdown_port");
    need_key = (rc != 0) || !text_has_key(text, len, "shutdown_key");
    if (!need_listener && !need_port && !need_key) {
        free(text);
        return 0;
    }

    ensure_parent_dir(path);
    f = fopen(path, "ab");
    if (f == NULL) {
        free(text);
        return -1;
    }
    if (rc == 0 && len > 0 && text[len - 1] != '\n') fputc('\n', f);
    if (need_listener) fprintf(f, "shutdown_listener=%s\n", d.shutdown_listener ? "on" : "off");
    if (need_port) fprintf(f, "shutdown_port=%d\n", d.shutdown_port);
    if (need_key) fprintf(f, "shutdown_key=%s\n", d.shutdown_key);
    rc = (fclose(f) == 0) ? 0 : -1;
    free(text);
    return rc;
}

int app_settings_set_device_name(const char *path, const char *name)
{
    char *text = NULL;
    size_t len = 0;
    size_t pos = 0;
    int rc = read_file(path, &text, &len);
    int replaced = 0;
    char tmp_path[1100];
    FILE *f;

    if (rc == -2) return -1;
    ensure_parent_dir(path);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    f = fopen(tmp_path, "wb");
    if (f == NULL) {
        free(text);
        return -1;
    }

    while (pos < len) {
        const char *line = text + pos;
        size_t line_len = 0;
        while (pos + line_len < len && line[line_len] != '\n') line_len++;
        if (line_is_key(line, line_len, "device_name")) {
            if (!replaced) {
                if (name[0] != '\0') fprintf(f, "device_name=%s\n", name);
                replaced = 1;
            }
        } else {
            fwrite(line, 1, line_len, f);
            fputc('\n', f);
        }
        pos += line_len + 1;
    }
    if (!replaced && name[0] != '\0') fprintf(f, "device_name=%s\n", name);
    free(text);

    if (fclose(f) != 0 || rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return -1;
    }
    return 0;
}
