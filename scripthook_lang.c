#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scripthook.h"

#define BASE_MAX 2048
#define ROW_MAX 2048
#define CACHE_MAX 1024
#define OWNER_MAX 128

typedef struct {
    char *owner;
    char *lang;
    char *key;
    char *text;
} BaseRow;

static BaseRow g_base[BASE_MAX];
static int g_nbase;
static SRWLOCK g_textLock = SRWLOCK_INIT;
typedef struct { char owner[48], key[512], text[768]; } LangRow;
typedef struct { char *owner, *key, *text; } CacheRow;
static LangRow g_rows[ROW_MAX];
static int g_nrows;
static CacheRow g_cache[CACHE_MAX];
static int g_ncache;
static char g_loaded[OWNER_MAX][48];
static int g_nloaded, g_frameworkLoaded;
static char g_langName[16];
static char g_pickLine[384];
static volatile LONG g_langReady;
static char g_names[32][16], g_labels[32][32];
static int g_nnames;
static __thread char g_uncached[512];
static void Readable(const char *key, char *out, size_t cap);
static const struct { const char *code, *label; } kBuiltin[] = {
    {"en-US", "English"}, {"zh-CN", "简体中文"},
    {"fr-FR", "Français"}, {"it-IT", "Italiano"},
    {"de-DE", "Deutsch"}, {"es-ES", "Español (España)"},
    {"ar-SA", "العربية"}, {"zh-TW", "繁體中文"},
    {"ko-KR", "한국어"}, {"ja-JP", "日本語"},
    {"nl-NL", "Nederlands"}, {"pl-PL", "Polski"},
    {"pt-BR", "Português (Brasil)"}, {"ru-RU", "Русский"},
    {"cs-CZ", "Čeština"}, {"es-MX", "Español (Latinoamérica)"}
};

extern void ShSetError(int err);
extern int ShConfigLanguageExplicit(void);

static char *TextDup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)HeapAlloc(GetProcessHeap(), 0, n);
    if (p) memcpy(p, s, n);
    return p;
}

static void TextFree(BaseRow *r) {
    if (r->owner) HeapFree(GetProcessHeap(), 0, r->owner);
    if (r->lang) HeapFree(GetProcessHeap(), 0, r->lang);
    if (r->key) HeapFree(GetProcessHeap(), 0, r->key);
    if (r->text) HeapFree(GetProcessHeap(), 0, r->text);
    memset(r, 0, sizeof(*r));
}

static void CopyText(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

/* Reforged searches the declaring owner first, then framework rows.
 * Declared rows are copied here so a plugin's temporary input cannot dangle. */
static const char *BaseFind(const char *owner, const char *lang,
                            const char *key) {
    int i, pass;
    if (!key) return NULL;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 0 && (!owner || !owner[0])) continue;
        for (i = 0; i < g_nbase; i++) {
            BaseRow *r = &g_base[i];
            if (_stricmp(r->lang, lang) || strcmp(r->key, key)) continue;
            if (pass == 0 && !_stricmp(r->owner, owner)) return r->text;
            if (pass == 1 && !r->owner[0]) return r->text;
        }
    }
    return NULL;
}

SH_API int ShLangDeclare(const char *owner, const char *lang,
                         const ShText *rows, int n) {
    int i, kept = 0;
    if (!lang || !lang[0] || !rows || n <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (!owner) owner = "";
    AcquireSRWLockExclusive(&g_textLock);
    for (i = 0; i < n && g_nbase < BASE_MAX; i++) {
        BaseRow r = {0};
        if (!rows[i].id || !rows[i].id[0]) continue;
        r.owner = TextDup(owner);
        r.lang = TextDup(lang);
        r.key = TextDup(rows[i].id);
        r.text = TextDup(rows[i].text ? rows[i].text : "");
        if (!r.owner || !r.lang || !r.key || !r.text) {
            TextFree(&r);
            break;
        }
        g_base[g_nbase++] = r;
        kept++;
    }
    ReleaseSRWLockExclusive(&g_textLock);
    ShSetError(SH_OK);
    return kept != 0;
}

int ShLangCopyEnglish(const char *owner, const char *key,
                      char *dst, size_t cap) {
    const char *v;
    int found;
    AcquireSRWLockShared(&g_textLock);
    v = BaseFind(owner, "en-US", key);
    found = v != NULL;
    if (found) CopyText(dst, cap, v);
    ReleaseSRWLockShared(&g_textLock);
    return found;
}

void ShLangCopySort(const char *owner, const char *key,
                    char *dst, size_t cap) {
    if (!cap) return;
    if (!key) key = "";
    if (key[0] != '@') { CopyText(dst, cap, key); return; }
    if (ShLangCopyEnglish(owner, key, dst, cap)) return;
    Readable(key, dst, cap);
}

int ShLangHasDeclared(const char *owner, const char *key) {
    return ShLangHas(owner, key);
}

void ShLangCopyMenu(const char *owner, const char *key,
                    char *dst, size_t cap) {
    if (!cap) return;
    if (!key) key = "";
    if (key[0] != '@') { CopyText(dst, cap, key); return; }
    CopyText(dst, cap, ShLangText(owner, key));
}

SH_API int ShLangMatch(const char *a, const char *b) {
    return a && b && !_stricmp(a, b);
}

static void EnsureLanguage(void) {
    char chosen[16], sys[16] = "";
    char offered[16][16], setting[256], *part, *next;
    WCHAR tag[LOCALE_NAME_MAX_LENGTH];
    LANGID id;
    int i, count = 0, found = 0;
    for (;;) {
        LONG state = InterlockedCompareExchange(&g_langReady, 0, 0);
        if (state == 1) return;
        if (state == 2) { Sleep(0); continue; }
        if (InterlockedCompareExchange(&g_langReady, 2, 0) == 0) break;
    }
    ShConfigGetStr("Settings", "Language", "", chosen, sizeof(chosen));
    if (!chosen[0]) {
        int explicitLanguage = ShConfigLanguageExplicit();
        ShConfigGetStr("Settings", "Languages", "", setting, sizeof(setting));
        part = setting;
        while (*part && count < 16) {
            next = strchr(part, ',');
            if (next) *next = 0;
            while (*part == ' ' || *part == '\t') part++;
            {
                char *end = part + strlen(part);
                while (end > part && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
            }
            if (*part) CopyText(offered[count++], sizeof(offered[0]), part);
            if (!next) break;
            part = next + 1;
        }
        if (!count) for (i = 0; i < 16; i++)
            CopyText(offered[count++], sizeof(offered[0]), kBuiltin[i].code);
        if (explicitLanguage) {
            CopyText(chosen, sizeof(chosen), offered[0]);
        } else {
            id = GetUserDefaultUILanguage();
            if (!id) id = GetSystemDefaultUILanguage();
            if (id && LCIDToLocaleName(MAKELCID(id, SORT_DEFAULT), tag,
                                       LOCALE_NAME_MAX_LENGTH, 0))
                WideCharToMultiByte(CP_UTF8, 0, tag, -1, sys,
                                    sizeof(sys), NULL, NULL);
            CopyText(chosen, sizeof(chosen), "en-US");
            if (sys[0]) {
                for (i = 0; i < count; i++) {
                    if (ShLangMatch(offered[i], sys)) {
                        CopyText(chosen, sizeof(chosen), offered[i]);
                        found = 1; break;
                    }
                }
                if (!found) for (i = 0; i < count; i++) {
                    const char *a = offered[i], *b = sys;
                    while (*a && *a != '-' && *b && *b != '-' &&
                           ((*a | 32) == (*b | 32))) { a++; b++; }
                    if ((!*a || *a == '-') && (!*b || *b == '-')) {
                        CopyText(chosen, sizeof(chosen), offered[i]);
                        break;
                    }
                }
            }
            snprintf(g_pickLine, sizeof(g_pickLine),
                     "picked on this run - no [Settings] Language= line; "
                     "Windows UI language %s", sys[0] ? sys : "unavailable");
            if (!ShConfigSetStr("Settings", "Language", chosen)) {
                size_t n = strlen(g_pickLine);
                snprintf(g_pickLine + n, sizeof(g_pickLine) - n,
                         "; scripthook.ini could not be written");
            }
        }
    }
    AcquireSRWLockExclusive(&g_textLock);
    CopyText(g_langName, sizeof(g_langName), chosen);
    InterlockedExchange(&g_langReady, 1);
    ReleaseSRWLockExclusive(&g_textLock);
}

static char *Trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

static void AddName(const char *code, const char *label) {
    int i;
    for (i = 0; i < g_nnames; i++)
        if (ShLangMatch(g_names[i], code)) {
            CopyText(g_labels[i], sizeof(g_labels[i]), label);
            return;
        }
    if (g_nnames >= 32) return;
    CopyText(g_names[g_nnames], sizeof(g_names[0]), code);
    CopyText(g_labels[g_nnames], sizeof(g_labels[0]), label);
    g_nnames++;
}

/* Called under the text lock, once per owner. Missing files are remembered. */
static void LoadLangFile(const char *owner) {
    char path[MAX_PATH], *data, *p, section[64] = "";
    FILE *file;
    size_t n;
    int i;
    if (owner && owner[0]) {
        for (i = 0; i < g_nloaded; i++)
            if (ShLangMatch(g_loaded[i], owner)) return;
        if (g_nloaded >= OWNER_MAX) return;
        CopyText(g_loaded[g_nloaded++], sizeof(g_loaded[0]), owner);
        if (!ShPluginLangPath(owner, path, sizeof(path))) return;
    } else {
        char dir[MAX_PATH];
        if (g_frameworkLoaded) return;
        g_frameworkLoaded = 1;
        if (!ShGameDir(dir, sizeof(dir)) ||
            snprintf(path, sizeof(path), "%s\\lang.ini", dir) >= (int)sizeof(path))
            return;
    }
    file = fopen(path, "rb");
    if (!file) return;
    data = (char *)malloc(65536);
    if (!data) { fclose(file); return; }
    n = fread(data, 1, 65535, file);
    fclose(file);
    data[n] = 0;
    p = data;
    if (n >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p) {
        char line[1024], *s, *eq, *key, *value, *end;
        size_t len = 0;
        while (*p && *p != '\n' && *p != '\r') {
            if (len < sizeof(line) - 1) line[len++] = *p;
            p++;
        }
        line[len] = 0;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        s = Trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            end = strchr(s, ']');
            if (end) { *end = 0; CopyText(section, sizeof(section), s + 1); }
            continue;
        }
        eq = NULL;
        if (*s == '"' || *s == '\'') {
            char q = *s;
            end = strchr(s + 1, q);
            if (end) {
                char *after = Trim(end + 1);
                if (*after == '=') { *end = 0; eq = after; s++; }
            }
        }
        if (!eq) { eq = strstr(s, " = "); if (eq) eq++; }
        if (!eq) eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        key = Trim(s); value = Trim(eq + 1);
        if (*value == '"' || *value == '\'') {
            char q = *value++;
            end = value + strlen(value);
            if (end > value && end[-1] == q) end[-1] = 0;
        }
        if (!*key || !*value) continue;
        if (ShLangMatch(section, "LanguageNames")) {
            AddName(key, value);
        } else if (ShLangMatch(section, g_langName) && g_nrows < ROW_MAX) {
            LangRow *r = &g_rows[g_nrows++];
            CopyText(r->owner, sizeof(r->owner), owner ? owner : "");
            CopyText(r->key, sizeof(r->key), key);
            CopyText(r->text, sizeof(r->text), value);
        }
    }
    free(data);
}

static const char *RowFind(const char *owner, const char *key) {
    int i, pass;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 0 && (!owner || !owner[0])) continue;
        for (i = 0; i < g_nrows; i++) {
            LangRow *r = &g_rows[i];
            if (strcmp(r->key, key)) continue;
            if (pass == 0 && ShLangMatch(r->owner, owner)) return r->text;
            if (pass == 1 && !r->owner[0]) return r->text;
        }
    }
    return NULL;
}

static void Readable(const char *key, char *out, size_t cap) {
    size_t n = 0;
    int upper = 1;
    if (key[0] != '@') { CopyText(out, cap, key); return; }
    for (key++; *key && n < cap - 1; key++) {
        char c = *key;
        if (c == '.' || c == '_' || c == '-') { c = ' '; upper = 1; }
        else if (upper) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            upper = 0;
        }
        out[n++] = c;
    }
    out[n] = 0;
}

static const char *Lookup(const char *owner, const char *key) {
    const char *v;
    LoadLangFile(NULL);
    if (owner && owner[0]) LoadLangFile(owner);
    v = RowFind(owner, key);
    if (!v) v = BaseFind(owner, g_langName, key);
    if (!v && !ShLangMatch(g_langName, "en-US"))
        v = BaseFind(owner, "en-US", key);
    return v;
}

SH_API const char *ShLangText(const char *owner, const char *key) {
    int i;
    const char *v;
    if (!key) return "";
    EnsureLanguage();
    AcquireSRWLockExclusive(&g_textLock);
    for (i = 0; i < g_ncache; i++) {
        if (!strcmp(g_cache[i].key, key) &&
            ShLangMatch(g_cache[i].owner, owner ? owner : "")) {
            v = g_cache[i].text;
            ReleaseSRWLockExclusive(&g_textLock);
            return v;
        }
    }
    v = Lookup(owner, key);
    if (!v) {
        Readable(key, g_uncached, sizeof(g_uncached));
        v = g_uncached;
    }
    if (g_ncache < CACHE_MAX) {
        CacheRow *r = &g_cache[g_ncache];
        r->owner = TextDup(owner ? owner : "");
        r->key = TextDup(key);
        r->text = TextDup(v);
        if (r->owner && r->key && r->text) {
            g_ncache++;
            v = r->text;
        } else {
            if (r->owner) HeapFree(GetProcessHeap(), 0, r->owner);
            if (r->key) HeapFree(GetProcessHeap(), 0, r->key);
            if (r->text) HeapFree(GetProcessHeap(), 0, r->text);
            memset(r, 0, sizeof(*r));
        }
    }
    ReleaseSRWLockExclusive(&g_textLock);
    return v;
}

SH_API int ShLangHas(const char *owner, const char *key) {
    int found;
    if (!key || !key[0]) return 0;
    EnsureLanguage();
    AcquireSRWLockExclusive(&g_textLock);
    found = Lookup(owner, key) != NULL;
    ReleaseSRWLockExclusive(&g_textLock);
    return found;
}

SH_API const char *ShLang(const char *text) { return ShLangText(NULL, text); }
SH_API const char *ShLangFor(const char *scope, const char *text) {
    (void)scope; return ShLangText(NULL, text);
}
SH_API const char *ShLangForOwned(const char *owner, const char *scope,
                                  const char *text) {
    (void)scope; return ShLangText(owner, text);
}
SH_API const char *ShLangGet(void) {
    EnsureLanguage(); return g_langName;
}
SH_API int ShLangPickLine(char *buf, int size) {
    if (!buf || size <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    EnsureLanguage();
    CopyText(buf, (size_t)size, g_pickLine);
    ShSetError(SH_OK);
    return g_pickLine[0] != 0;
}

SH_API const char *ShLangLabel(const char *code) {
    static char label[32];
    char key[48];
    const char *v;
    int i;
    if (!code || !code[0]) return "";
    EnsureLanguage();
    AcquireSRWLockExclusive(&g_textLock);
    LoadLangFile(NULL);
    for (i = 0; i < g_nnames; i++) {
        if (ShLangMatch(g_names[i], code)) {
            CopyText(label, sizeof(label), g_labels[i]);
            ReleaseSRWLockExclusive(&g_textLock);
            return label;
        }
    }
    snprintf(key, sizeof(key), "@lang.name.%s", code);
    for (i = 0; i < g_nbase; i++) {
        if (ShLangMatch(g_base[i].key, key)) {
            v = g_base[i].text;
            CopyText(label, sizeof(label), v);
            ReleaseSRWLockExclusive(&g_textLock);
            return label;
        }
    }
    for (i = 0; i < 16; i++) {
        if (ShLangMatch(kBuiltin[i].code, code)) {
            CopyText(label, sizeof(label), kBuiltin[i].label);
            ReleaseSRWLockExclusive(&g_textLock);
            return label;
        }
    }
    ReleaseSRWLockExclusive(&g_textLock);
    return code;
}

SH_API int ShLangBuiltin(int index, char *buf, int size) {
    int n;
    if (!buf || size <= 0 || index < 0) {
        ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    if (index >= 16) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    n = snprintf(buf, (size_t)size, "%s\t%s", kBuiltin[index].code,
                 ShLangLabel(kBuiltin[index].code));
    ShSetError(n < 0 ? SH_ERR_BAD_ARG : SH_OK);
    return n >= 0 && n < size;
}

/* Reforged's formatter uses the en-US conversion list as the argument
 * contract. A malformed or mistyped translation falls back to en-US. */
typedef enum {
    CK_INT, CK_LONG, CK_LLONG, CK_DOUBLE, CK_LDOUBLE,
    CK_STR, CK_PTR, CK_PCT, CK_BAD
} ConvKind;

typedef struct {
    int len, index, idxAt, idxLen;
    ConvKind kind;
} Conv;

#define CONV_MAX 16

static int ScanConv(const char *p, Conv *c) {
    int i = 1, len = 0;
    ConvKind kind;
    memset(c, 0, sizeof(*c));
    c->kind = CK_BAD;
    if (p[0] != '%') return 0;
    if (p[1] == '%') { c->len = 2; c->kind = CK_PCT; return 2; }
    while (p[i] && strchr("-+ #0'", p[i])) i++;
    if (p[i] >= '0' && p[i] <= '9') {
        const char *d = p + i;
        int n = 0;
        while (*d >= '0' && *d <= '9') { n = n * 10 + (*d - '0'); d++; }
        if (*d == '$') {
            c->index = n;
            c->idxAt = i;
            c->idxLen = (int)(d - (p + i)) + 1;
            i += c->idxLen;
            while (p[i] && strchr("-+ #0'", p[i])) i++;
            while (p[i] >= '0' && p[i] <= '9') i++;
        } else i = (int)(d - p);
    }
    if (p[i] == '*') return 0;
    if (p[i] == '.') {
        i++;
        if (p[i] == '*') return 0;
        while (p[i] >= '0' && p[i] <= '9') i++;
    }
    switch (p[i]) {
    case 'h': i++; if (p[i] == 'h') i++; len = 1; break;
    case 'l': i++; if (p[i] == 'l') { i++; len = 3; } else len = 2; break;
    case 'L': i++; len = 4; break;
    case 'z': case 'j': case 't': i++; len = 3; break;
    case 'I':
        i++;
        if ((p[i] == '3' && p[i + 1] == '2') ||
            (p[i] == '6' && p[i + 1] == '4')) i += 2;
        len = 3;
        break;
    default: break;
    }
    switch (p[i]) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
        kind = len <= 1 ? CK_INT : len == 2 ? CK_LONG : CK_LLONG;
        break;
    case 'c': kind = len <= 1 ? CK_INT : CK_BAD; break;
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
    case 'a': case 'A': kind = len == 4 ? CK_LDOUBLE : CK_DOUBLE; break;
    case 's': kind = len == 0 ? CK_STR : CK_BAD; break;
    case 'p': kind = len == 0 ? CK_PTR : CK_BAD; break;
    default: return 0;
    }
    c->kind = kind;
    c->len = i + 1;
    return c->len;
}

static int CollectConvs(const char *fmt, Conv *list) {
    int n = 0;
    while (fmt && *fmt) {
        Conv c;
        int step;
        if (*fmt != '%') { fmt++; continue; }
        step = ScanConv(fmt, &c);
        if (step <= 0) return -1;
        if (c.kind != CK_PCT) {
            if (n >= CONV_MAX) return -1;
            list[n++] = c;
        }
        fmt += step;
    }
    return n;
}

static int AppendText(char *dst, size_t cap, int at, const char *s) {
    size_t n = strlen(s);
    if ((size_t)at >= cap - 1) return at;
    if (n > cap - 1 - (size_t)at) n = cap - 1 - (size_t)at;
    memcpy(dst + at, s, n);
    at += (int)n;
    dst[at] = 0;
    return at;
}

static void SkipArg(va_list *ap, ConvKind kind) {
    switch (kind) {
    case CK_INT: (void)va_arg(*ap, int); break;
    case CK_LONG: (void)va_arg(*ap, long); break;
    case CK_LLONG: (void)va_arg(*ap, long long); break;
    case CK_DOUBLE: (void)va_arg(*ap, double); break;
    case CK_LDOUBLE: (void)va_arg(*ap, long double); break;
    case CK_STR: (void)va_arg(*ap, const char *); break;
    case CK_PTR: (void)va_arg(*ap, void *); break;
    default: break;
    }
}

static int RenderOne(char *dst, size_t cap, int at, const char *fmt,
                     ConvKind kind, va_list *ap) {
    char piece[384];
    int n;
    switch (kind) {
    case CK_INT: n = snprintf(piece, sizeof(piece), fmt, va_arg(*ap, int)); break;
    case CK_LONG: n = snprintf(piece, sizeof(piece), fmt, va_arg(*ap, long)); break;
    case CK_LLONG:
        n = snprintf(piece, sizeof(piece), fmt, va_arg(*ap, long long)); break;
    case CK_DOUBLE:
        n = snprintf(piece, sizeof(piece), fmt, va_arg(*ap, double)); break;
    case CK_LDOUBLE:
        n = snprintf(piece, sizeof(piece), fmt, va_arg(*ap, long double)); break;
    case CK_STR: {
        const char *v = va_arg(*ap, const char *);
        n = snprintf(piece, sizeof(piece), fmt, v ? v : "");
        break;
    }
    case CK_PTR: n = snprintf(piece, sizeof(piece), fmt, va_arg(*ap, void *)); break;
    default: n = 0; piece[0] = 0; break;
    }
    if (n < 0) piece[0] = 0;
    return AppendText(dst, cap, at, piece);
}

static int FormatPositional(char *dst, size_t cap, const char *fmt,
                            const Conv *en, va_list *base) {
    int at = 0;
    dst[0] = 0;
    while (*fmt) {
        char norm[40];
        Conv c;
        va_list ap;
        int i, n, step;
        if (*fmt != '%') {
            const char *pc = strchr(fmt, '%');
            n = pc ? (int)(pc - fmt) : (int)strlen(fmt);
            if (n > (int)sizeof(norm) - 1) n = (int)sizeof(norm) - 1;
            memcpy(norm, fmt, (size_t)n);
            norm[n] = 0;
            at = AppendText(dst, cap, at, norm);
            fmt += n;
            continue;
        }
        step = ScanConv(fmt, &c);
        if (step <= 0) break;
        if (c.kind == CK_PCT) {
            at = AppendText(dst, cap, at, "%");
            fmt += step;
            continue;
        }
        n = 0;
        for (i = 0; i < c.idxAt && n < (int)sizeof(norm) - 1; i++)
            norm[n++] = fmt[i];
        for (i = c.idxAt + c.idxLen;
             i < c.len && n < (int)sizeof(norm) - 1; i++)
            norm[n++] = fmt[i];
        norm[n] = 0;
        va_copy(ap, *base);
        for (i = 1; i < c.index; i++) SkipArg(&ap, en[i - 1].kind);
        at = RenderOne(dst, cap, at, norm, en[c.index - 1].kind, &ap);
        va_end(ap);
        fmt += step;
    }
    return at;
}

int ShLangFormatMenuV(char *dst, size_t cap, const char *en,
                      const char *tr, va_list ap) {
    Conv translated[CONV_MAX], english[CONV_MAX];
    int ntr, nen, indexed = 0, used[CONV_MAX + 1] = {0};
    int i, valid = 1, n;
    va_list copy;
    if (!dst || !cap) return 0;
    dst[0] = 0;
    if (!tr || !tr[0]) tr = en;
    if (!tr || !tr[0]) return 0;
    if (!en || !en[0]) en = tr;
    ntr = CollectConvs(tr, translated);
    nen = CollectConvs(en, english);
    if (nen < 0) {
        va_copy(copy, ap);
        n = vsnprintf(dst, cap, tr, copy);
        va_end(copy);
        dst[cap - 1] = 0;
        return n < 0 ? 0 : n;
    }
    if (ntr < 0 || ntr != nen) valid = 0;
    for (i = 0; valid && i < ntr; i++) {
        if (translated[i].kind == CK_BAD) valid = 0;
        if (translated[i].index) indexed = 1;
    }
    if (valid && indexed) {
        for (i = 0; i < ntr; i++) {
            int ix = translated[i].index;
            if (ix < 1 || ix > nen || used[ix] ||
                translated[i].kind != english[ix - 1].kind) {
                valid = 0;
                break;
            }
            used[ix] = 1;
        }
        for (i = 1; i <= nen; i++) if (!used[i]) valid = 0;
    } else if (valid) {
        for (i = 0; i < ntr; i++)
            if (translated[i].kind != english[i].kind) valid = 0;
    }
    va_copy(copy, ap);
    if (!valid) n = vsnprintf(dst, cap, en, copy);
    else if (indexed) n = FormatPositional(dst, cap, tr, english, &copy);
    else n = vsnprintf(dst, cap, tr, copy);
    va_end(copy);
    dst[cap - 1] = 0;
    return n < 0 ? 0 : n;
}
