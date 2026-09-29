#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SH_BUILD 1
#include "scripthook.h"

#define CONFIG_MAX 65536u
#define ENTRIES_MAX 256

typedef struct {
    char section[64];
    char key[96];
    char value[256];
} CfgEntry;

static CfgEntry g_entries[ENTRIES_MAX];
static int g_nentries;
static int g_configReady;
static int g_langSettingPresent;
static SRWLOCK g_configLock = SRWLOCK_INIT;
static volatile LONG g_logLevel = -1;

extern void ShSetError(int err);

static const char *DEFAULT_CONFIG =
    "[loader]\n"
    "load_plugins=1\n"
    "cpu_boot=0\n"
    "cpu_window=0\n"
    "cpu_eco_boot=0\n"
    "cpu_play=0\n"
    "cpu_prio_play=0\n"
    "cpu_cores=0\n"
    "\n"
    "[plugins]\n"
    "\n"
    "[forgemod]\n"
    "enabled=0\n"
    "\n"
    "[Settings]\n"
    "; LogLevel: none/error/warn/info/debug (a release build defaults to\n"
    "; warn, any other to info). Uncomment to change.\n"
    ";LogLevel=info\n"
    "\n"
    "; Menu language: left out - as here - the first launch picks one from\n"
    "; the Windows user language and writes it below. Delete the line to\n"
    "; have it picked again.\n"
    ";Languages=zh-CN,en-US\n"
    ";Language=zh-CN\n";

static void CopyN(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void Utf8Trim(char *s) {
    size_t n = strlen(s), keep = n, end;
    unsigned char b;
    int need;
    while (keep > 0 && ((unsigned char)s[keep - 1] & 0xC0) == 0x80) keep--;
    if (!keep) { s[0] = 0; return; }
    b = (unsigned char)s[keep - 1];
    need = b < 0xC2 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    end = keep - 1 + (size_t)need;
    s[end > n ? keep - 1 : end] = 0;
}

static int GameDir(char *out, size_t cap) {
    char *slash;
    if (!cap || !GetModuleFileNameA(NULL, out, (DWORD)cap)) return 0;
    out[cap - 1] = 0;
    slash = strrchr(out, '\\');
    if (!slash) { out[0] = 0; return 0; }
    slash[1] = 0;
    return 1;
}

static int PathPrint(char *out, int size, const char *fmt,
                     const char *a, const char *b, const char *c) {
    int n;
    if (!out || size <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    n = snprintf(out, (size_t)size, fmt, a, b, c);
    if (n < 0 || n >= size) {
        out[0] = 0;
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShGameDir(char *buf, int size) {
    char dir[MAX_PATH];
    size_t len;
    if (!buf || size <= 0 || !GameDir(dir, sizeof(dir))) {
        ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    len = strlen(dir);
    if (len && dir[len - 1] == '\\') len--;
    if (len >= (size_t)size) len = (size_t)size - 1;
    memcpy(buf, dir, len);
    buf[len] = 0;
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShPluginsDir(char *buf, int size) {
    char dir[MAX_PATH];
    if (!GameDir(dir, sizeof(dir))) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    return PathPrint(buf, size, "%splugins\\", dir, "", "");
}

SH_API int ShScriptsDir(char *buf, int size) { return ShPluginsDir(buf, size); }

SH_API int ShLogPath(const char *name, char *buf, int size) {
    char dir[MAX_PATH], logs[MAX_PATH];
    int n;
    if (!name || !buf || size <= 0 || !GameDir(dir, sizeof(dir))) {
        ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    n = snprintf(logs, sizeof(logs), "%slogs", dir);
    if (n < 0 || n >= (int)sizeof(logs)) {
        ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    CreateDirectoryA(logs, NULL);
    return PathPrint(buf, size, "%s\\%s", logs, name, "");
}

SH_API int ShPluginIniPath(const char *plugin, char *buf, int size) {
    char dir[MAX_PATH];
    if (!plugin || !buf || size <= 0 || !GameDir(dir, sizeof(dir))) {
        ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    return PathPrint(buf, size, "%splugins\\%s\\%s.ini",
                     dir, plugin, plugin);
}

SH_API int ShPluginLangPath(const char *plugin, char *buf, int size) {
    char dir[MAX_PATH];
    if (!plugin || !buf || size <= 0 || !GameDir(dir, sizeof(dir))) {
        ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    return PathPrint(buf, size, "%splugins\\%s\\lang.ini",
                     dir, plugin, "");
}

static void ConfigPath(char *path, size_t cap) {
    char dir[MAX_PATH];
    if (!GameDir(dir, sizeof(dir))) { path[0] = 0; return; }
    if (snprintf(path, cap, "%sscripthook.ini", dir) >= (int)cap)
        path[0] = 0;
}

static void GhostHookConfigPath(char *path, size_t cap) {
    char dir[MAX_PATH];
    if (!GameDir(dir, sizeof(dir))) { path[0] = 0; return; }
    if (snprintf(path, cap, "%sGhostHook.ini", dir) >= (int)cap)
        path[0] = 0;
}

int ShGhostSettingsGetStr(const char *key, char *out, int size) {
    char path[MAX_PATH];
    DWORD n;

    if (!key || !out || size <= 0) return 0;

    GhostHookConfigPath(path, sizeof(path));
    if (!path[0]) {
        out[0] = 0;
        return 0;
    }

    n = GetPrivateProfileStringA("GhostHook Settings", key, "",
                                 out, (DWORD)size, path);
    out[size - 1] = 0;
    return n > 0;
}

int ShGhostSettingsSetStr(const char *key, const char *value) {
    char path[MAX_PATH];

    if (!key || !value) return 0;

    GhostHookConfigPath(path, sizeof(path));
    if (!path[0]) return 0;

    return WritePrivateProfileStringA("GhostHook Settings",
                                      key, value, path) != 0;
}

static char *Trim(char *s) {
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return s;
}

static void ParseLine(char *line, char *section, size_t sectionCap) {
    char *p = Trim(line), *eq, *key, *value, *q;
    CfgEntry *entry;
    if (!*p || *p == ';' || *p == '#') return;
    if (*p == '[') {
        q = strchr(p, ']');
        if (q) { *q = 0; CopyN(section, sectionCap, p + 1); }
        return;
    }
    eq = strchr(p, '=');
    if (!eq) return;
    *eq = 0;
    key = Trim(p);
    value = Trim(eq + 1);
    if (*value == '"' || *value == '\'') {
        char quote = *value++;
        q = value + strlen(value);
        if (q > value && q[-1] == quote) q[-1] = 0;
    } else {
        for (q = value; *q; q++) {
            if ((*q == ';' || *q == '#') && q > value &&
                (q[-1] == ' ' || q[-1] == '\t')) {
                *q = 0;
                value = Trim(value);
                break;
            }
        }
    }
    if (!strcmp(section, "Settings") && !strcmp(key, "Language"))
        g_langSettingPresent = 1;
    if (!*value || g_nentries >= ENTRIES_MAX) return;
    entry = &g_entries[g_nentries++];
    CopyN(entry->section, sizeof(entry->section), section);
    CopyN(entry->key, sizeof(entry->key), key);
    CopyN(entry->value, sizeof(entry->value), value);
}

static void LoadConfig(void) {
    char path[MAX_PATH], *data, *p, section[48] = "";
    FILE *file;
    size_t n;
    AcquireSRWLockExclusive(&g_configLock);
    if (g_configReady) { ReleaseSRWLockExclusive(&g_configLock); return; }
    ConfigPath(path, sizeof(path));
    if (!path[0]) { g_configReady = 1; ReleaseSRWLockExclusive(&g_configLock); return; }
    file = fopen(path, "rb");
    if (!file) {
        FILE *created = fopen(path, "wb");
        if (created) { fputs(DEFAULT_CONFIG, created); fclose(created); }
        file = fopen(path, "rb");
    }
    if (!file) { g_configReady = 1; ReleaseSRWLockExclusive(&g_configLock); return; }
    data = (char *)malloc(CONFIG_MAX);
    if (!data) { fclose(file); g_configReady = 1; ReleaseSRWLockExclusive(&g_configLock); return; }
    n = fread(data, 1, CONFIG_MAX - 1, file);
    fclose(file);
    data[n] = 0;
    p = data;
    if (n >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p) {
        char line[512];
        size_t len = 0;
        while (*p && *p != '\n' && *p != '\r') {
            if (len < sizeof(line) - 1) line[len++] = *p;
            p++;
        }
        line[len] = 0;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        ParseLine(line, section, sizeof(section));
    }
    free(data);
    g_configReady = 1;
    ReleaseSRWLockExclusive(&g_configLock);
}

int ShConfigLanguageExplicit(void) {
    int present;
    LoadConfig();
    AcquireSRWLockShared(&g_configLock);
    present = g_langSettingPresent;
    ReleaseSRWLockShared(&g_configLock);
    return present;
}

static int FindEntryCopy(const char *section, const char *key,
                         char *out, size_t cap) {
    int i, found = 0;
    AcquireSRWLockShared(&g_configLock);
    for (i = 0; i < g_nentries; i++) {
        if (!strcmp(g_entries[i].section, section) &&
            !strcmp(g_entries[i].key, key)) {
            CopyN(out, cap, g_entries[i].value);
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_configLock);
    if (!found && cap) out[0] = 0;
    return found;
}

SH_API void ShConfigInit(void) {
    LoadConfig();
    (void)ShLangGet();
}

SH_API int ShConfigGetInt(const char *section, const char *key, int def) {
    char value[256], *end;
    long result;
    if (!section || !key) { ShSetError(SH_ERR_BAD_ARG); return def; }
    LoadConfig();
    if (!FindEntryCopy(section, key, value, sizeof(value))) {
        ShSetError(SH_OK); return def;
    }
    result = strtol(value, &end, 10);
    ShSetError(SH_OK);
    return end == value ? def : (int)result;
}

SH_API int ShConfigGetBool(const char *section, const char *key, int def) {
    char value[256];
    if (!section || !key) { ShSetError(SH_ERR_BAD_ARG); return def; }
    LoadConfig();
    if (!FindEntryCopy(section, key, value, sizeof(value))) {
        ShSetError(SH_OK); return def;
    }
    ShSetError(SH_OK);
    if (!_stricmp(value, "1") || !_stricmp(value, "true") ||
        !_stricmp(value, "yes") || !_stricmp(value, "on")) return 1;
    if (!_stricmp(value, "0") || !_stricmp(value, "false") ||
        !_stricmp(value, "no") || !_stricmp(value, "off")) return 0;
    return def;
}

SH_API int ShConfigGetStr(const char *section, const char *key,
                          const char *def, char *out, int size) {
    char value[256];
    if (!out || size <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!section || !key) {
        out[0] = 0; ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    LoadConfig();
    if (FindEntryCopy(section, key, value, sizeof(value)))
        CopyN(out, (size_t)size, value);
    else CopyN(out, (size_t)size, def ? def : "");
    Utf8Trim(out);
    ShSetError(SH_OK);
    return 1;
}

static int Append(char *out, size_t cap, size_t *at,
                  const char *text, size_t len) {
    if (len > cap - *at) return 0;
    memcpy(out + *at, text, len);
    *at += len;
    return 1;
}

static int AppendSetting(char *out, size_t cap, size_t *at,
                         const char *key, const char *value,
                         const char *ending, size_t endingLen) {
    return Append(out, cap, at, key, strlen(key)) &&
           Append(out, cap, at, "=", 1) &&
           Append(out, cap, at, value, strlen(value)) &&
           Append(out, cap, at, ending, endingLen);
}

static int WriteValue(const char *section, const char *key,
                      const char *value) {
    char path[MAX_PATH], tmp[MAX_PATH], *src = NULL, *dst = NULL;
    char current[64] = "";
    const char *eol = "\n";
    FILE *file;
    long len;
    size_t cap, at = 0, pos = 0, valueLen = strlen(value);
    int inSection = 0, seenSection = 0, replaced = 0, ok = 0;
    ConfigPath(path, sizeof(path));
    if (!path[0]) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) || (len = ftell(file)) < 0 ||
        (unsigned long)len > CONFIG_MAX || fseek(file, 0, SEEK_SET)) {
        fclose(file); return 0;
    }
    src = (char *)malloc((size_t)len + 1);
    cap = (size_t)len + strlen(section) + strlen(key) + valueLen + 512;
    dst = (char *)malloc(cap);
    if (!src || !dst) { fclose(file); goto done; }
    if (len && fread(src, 1, (size_t)len, file) != (size_t)len) {
        fclose(file); goto done;
    }
    fclose(file);
    src[len] = 0;
    if (strstr(src, "\r\n")) eol = "\r\n";
    while (pos < (size_t)len) {
        size_t start = pos, content, total;
        char line[512], *p, *close, *eq;
        while (pos < (size_t)len && src[pos] != '\n') pos++;
        content = pos - start;
        if (content && src[start + content - 1] == '\r') content--;
        if (pos < (size_t)len) pos++;
        total = pos - start;
        {
            size_t parseLen = content;
            if (parseLen >= sizeof(line)) parseLen = sizeof(line) - 1;
            memcpy(line, src + start, parseLen);
            line[parseLen] = 0;
        }
        if (start == 0 && (size_t)len >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            memmove(line, line + 3, strlen(line + 3) + 1);
        p = Trim(line);
        if (*p == '[' && (close = strchr(p, ']')) != NULL) {
            if (inSection && !replaced) {
                if (!AppendSetting(dst, cap, &at, key, value,
                                   eol, strlen(eol))) goto done;
                replaced = 1;
            }
            *close = 0; CopyN(current, sizeof(current), p + 1);
            inSection = !strcmp(current, section);
            if (inSection) seenSection = 1;
        } else if (inSection && !replaced && *p != ';' && *p != '#') {
            eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                if (!strcmp(Trim(p), key)) {
                    if (!AppendSetting(dst, cap, &at, key, value,
                                       src + start + content,
                                       total - content)) goto done;
                    replaced = 1;
                    continue;
                }
            }
        }
        if (!Append(dst, cap, &at, src + start, total)) goto done;
    }
    if (!replaced) {
        if (at && dst[at - 1] != '\n' && !Append(dst, cap, &at, eol, strlen(eol))) goto done;
        if (!seenSection) {
            if ((at && !Append(dst, cap, &at, eol, strlen(eol))) ||
                !Append(dst, cap, &at, "[", 1) ||
                !Append(dst, cap, &at, section, strlen(section)) ||
                !Append(dst, cap, &at, "]", 1) ||
                !Append(dst, cap, &at, eol, strlen(eol))) goto done;
        }
        if (!AppendSetting(dst, cap, &at, key, value,
                           eol, strlen(eol))) goto done;
    }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) goto done;
    file = fopen(tmp, "wb");
    if (!file) goto done;
    if (fwrite(dst, 1, at, file) != at) {
        fclose(file);
        remove(tmp); goto done;
    }
    if (fclose(file) != 0) {
        remove(tmp); goto done;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp); goto done;
    }
    ok = 1;
done:
    free(src);
    free(dst);
    return ok;
}

SH_API int ShConfigSetStr(const char *section, const char *key,
                          const char *value) {
    int i;
    if (!section || !key || !value) {
        ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    LoadConfig();
    AcquireSRWLockExclusive(&g_configLock);
    if (!WriteValue(section, key, value)) {
        ReleaseSRWLockExclusive(&g_configLock);
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    for (i = 0; i < g_nentries; i++) {
        if (!strcmp(g_entries[i].section, section) &&
            !strcmp(g_entries[i].key, key)) break;
    }
    if (i == g_nentries && g_nentries < ENTRIES_MAX) g_nentries++;
    if (i < ENTRIES_MAX) {
        CopyN(g_entries[i].section, sizeof(g_entries[i].section), section);
        CopyN(g_entries[i].key, sizeof(g_entries[i].key), key);
        CopyN(g_entries[i].value, sizeof(g_entries[i].value), value);
    }
    if (!strcmp(section, "Settings") && !strcmp(key, "Language"))
        g_langSettingPresent = 1;
    ReleaseSRWLockExclusive(&g_configLock);
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShConfigSetInt(const char *section, const char *key, int value) {
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    return ShConfigSetStr(section, key, text);
}

SH_API int ShConfigSetBool(const char *section, const char *key, int value) {
    return ShConfigSetStr(section, key, value ? "1" : "0");
}

SH_API int ShLogLevel(void) {
    LONG cached = InterlockedCompareExchange(&g_logLevel, -1, -1);
    char text[24];
    int level = SH_LOG_WARN;
    if (cached >= 0) return (int)cached;
    if (ShConfigGetStr("Settings", "LogLevel", "", text, sizeof(text))) {
        if (!_stricmp(text, "none")) level = SH_LOG_NONE;
        else if (!_stricmp(text, "error")) level = SH_LOG_ERR;
        else if (!_stricmp(text, "warn")) level = SH_LOG_WARN;
        else if (!_stricmp(text, "info")) level = SH_LOG_INFO;
        else if (!_stricmp(text, "debug")) level = SH_LOG_DBG;
    }
    InterlockedCompareExchange(&g_logLevel, level, -1);
    return (int)InterlockedCompareExchange(&g_logLevel, -1, -1);
}