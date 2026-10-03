/* Scoped plugin file rules for Skip Intro: OPEN and ATTR only.
 * The public layouts and six Win32 targets follow pinned plugin
 * 4359412559db795d1d13ab086f6355e6fa39c8f5. No GRW site is patched. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "third_party/minhook/include/MinHook.h"

#define RULE_MAX 32
#define NAME_MAX 64
#define PATH_MAX 260
#define SUPPORTED (SH_FILE_OPEN | SH_FILE_ATTR)

extern void ShSetError(int err);

struct ShFileRule {
    int live;
    int seq;
    uint32_t group;
    wchar_t name[NAME_MAX];
    char nameA[NAME_MAX];
    wchar_t suffix[PATH_MAX];
    char suffixA[PATH_MAX];
    ShFileAfterFn after;
    void *user;
    volatile LONG callbacks;
};

static struct ShFileRule g_rules[RULE_MAX];
static SRWLOCK g_rulesLock = SRWLOCK_INIT;
static SRWLOCK g_adminLock = SRWLOCK_INIT;
static volatile LONG g_live;
static volatile LONG g_mask;
static volatile LONG g_calls;
static int g_nextSeq;
static int g_hooked;
static __thread int t_depth;

static HANDLE (WINAPI *realOpenA)(LPCSTR, DWORD, DWORD,
                                  LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *realOpenW)(LPCWSTR, DWORD, DWORD,
                                  LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static DWORD (WINAPI *realAttrA)(LPCSTR);
static DWORD (WINAPI *realAttrW)(LPCWSTR);
static BOOL (WINAPI *realAttrExA)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static BOOL (WINAPI *realAttrExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

static HANDLE WINAPI HookOpenA(LPCSTR, DWORD, DWORD,
                               LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI HookOpenW(LPCWSTR, DWORD, DWORD,
                               LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static DWORD WINAPI HookAttrA(LPCSTR);
static DWORD WINAPI HookAttrW(LPCWSTR);
static BOOL WINAPI HookAttrExA(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static BOOL WINAPI HookAttrExW(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

typedef struct {
    const char *name;
    void *hook;
    void **real;
    void *target;
    int created;
} Target;

static Target g_targets[] = {
    { "CreateFileA", (void *)HookOpenA, (void **)&realOpenA, NULL, 0 },
    { "CreateFileW", (void *)HookOpenW, (void **)&realOpenW, NULL, 0 },
    { "GetFileAttributesA", (void *)HookAttrA, (void **)&realAttrA, NULL, 0 },
    { "GetFileAttributesW", (void *)HookAttrW, (void **)&realAttrW, NULL, 0 },
    { "GetFileAttributesExA", (void *)HookAttrExA, (void **)&realAttrExA, NULL, 0 },
    { "GetFileAttributesExW", (void *)HookAttrExW, (void **)&realAttrExW, NULL, 0 }
};

static void UnhookAll(void) {
    unsigned i;
    for (i = 0; i < sizeof(g_targets) / sizeof(g_targets[0]); ++i)
        if (g_targets[i].created) MH_DisableHook(g_targets[i].target);
    for (i = 0; i < sizeof(g_targets) / sizeof(g_targets[0]); ++i) {
        if (g_targets[i].created) {
            MH_RemoveHook(g_targets[i].target);
            g_targets[i].created = 0;
        }
    }
    g_hooked = 0;
}

/* All-or-nothing: a registration cannot claim success with missing targets. */
static int HookAll(void) {
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    MH_STATUS status;
    unsigned i;
    if (g_hooked) return 1;
    if (!kernel) return 0;
    status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return 0;
    for (i = 0; i < sizeof(g_targets) / sizeof(g_targets[0]); ++i) {
        Target *t = &g_targets[i];
        t->target = (void *)GetProcAddress(kernel, t->name);
        if (!t->target) { UnhookAll(); return 0; }
        status = MH_CreateHook(t->target, t->hook, t->real);
        if (status != MH_OK) { UnhookAll(); return 0; }
        t->created = 1;
        status = MH_EnableHook(t->target);
        if (status != MH_OK) { UnhookAll(); return 0; }
    }
    g_hooked = 1;
    return 1;
}

static const wchar_t *BaseW(const wchar_t *p) {
    const wchar_t *base = p;
    for (; *p; ++p) if (*p == L'\\' || *p == L'/') base = p + 1;
    return base;
}

static const char *BaseA(const char *p) {
    const char *base = p;
    for (; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

static int Match(const struct ShFileRule *r, uint32_t group,
                 const void *path, int wide) {
    if (!(r->group & group) || !path) return 0;
    if (wide) {
        const wchar_t *p = (const wchar_t *)path;
        size_t n = wcslen(p), m = wcslen(r->suffix);
        if (r->name[0] && _wcsicmp(BaseW(p), r->name)) return 0;
        if (m && (n < m || _wcsicmp(p + n - m, r->suffix))) return 0;
    } else {
        const char *p = (const char *)path;
        size_t n = strlen(p), m = strlen(r->suffixA);
        if (r->nameA[0] && _stricmp(BaseA(p), r->nameA)) return 0;
        if (m && (n < m || _stricmp(p + n - m, r->suffixA))) return 0;
    }
    return 1;
}

/* A hide is the only action this scoped contract accepts. After callbacks
 * run in registration order and are pinned until deletion has drained them. */
static int Hide(const char *api, uint32_t group, const void *path, int wide,
                DWORD access, DWORD share, DWORD disp, DWORD flags,
                void *buffer) {
    struct ShFileRule *after[RULE_MAX], *pick;
    ShFileCall call;
    DWORD previous = GetLastError();
    int n = 0, i, j;
    if (t_depth || !InterlockedCompareExchange(&g_live, 0, 0) ||
        !(InterlockedCompareExchange(&g_mask, 0, 0) & (LONG)group))
        return 0;
    ++t_depth;
    InterlockedIncrement(&g_calls);
    AcquireSRWLockShared(&g_rulesLock);
    pick = NULL;
    for (i = 0; i < RULE_MAX; ++i) {
        struct ShFileRule *r = &g_rules[i];
        if (!r->live || !Match(r, group, path, wide)) continue;
        if (!pick || r->seq < pick->seq) pick = r;
        if (r->after) {
            InterlockedIncrement(&r->callbacks);
            after[n++] = r;
        }
    }
    ReleaseSRWLockShared(&g_rulesLock);
    if (!pick) { --t_depth; SetLastError(previous); return 0; }
    for (i = 1; i < n; ++i) {
        struct ShFileRule *r = after[i];
        for (j = i; j > 0 && after[j - 1]->seq > r->seq; --j)
            after[j] = after[j - 1];
        after[j] = r;
    }
    memset(&call, 0, sizeof(call));
    call.api = api;
    call.group = group;
    call.wide = wide;
    if (wide) call.path = call.asked = (const wchar_t *)path;
    else call.pathA = call.askedA = (const char *)path;
    call.access = access;
    call.share = share;
    call.disp = disp;
    call.flags = flags;
    call.buffer = buffer;
    call.error = ERROR_FILE_NOT_FOUND;
    call.answered = call.matched = 1;
    for (i = 0; i < n; ++i) {
        after[i]->after(&call, after[i]->user);
        InterlockedDecrement(&after[i]->callbacks);
    }
    --t_depth;
    SetLastError(ERROR_FILE_NOT_FOUND);
    return 1;
}

static HANDLE WINAPI HookOpenA(LPCSTR p, DWORD a, DWORD s,
                                LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f,
                                HANDLE t) {
    if (Hide("CreateFileA", SH_FILE_OPEN, p, 0, a, s, d, f, NULL))
        return INVALID_HANDLE_VALUE;
    return realOpenA(p, a, s, sa, d, f, t);
}

static HANDLE WINAPI HookOpenW(LPCWSTR p, DWORD a, DWORD s,
                                LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f,
                                HANDLE t) {
    if (Hide("CreateFileW", SH_FILE_OPEN, p, 1, a, s, d, f, NULL))
        return INVALID_HANDLE_VALUE;
    return realOpenW(p, a, s, sa, d, f, t);
}

static DWORD WINAPI HookAttrA(LPCSTR p) {
    if (Hide("GetFileAttributesA", SH_FILE_ATTR, p, 0, 0, 0, 0, 0, NULL))
        return INVALID_FILE_ATTRIBUTES;
    return realAttrA(p);
}

static DWORD WINAPI HookAttrW(LPCWSTR p) {
    if (Hide("GetFileAttributesW", SH_FILE_ATTR, p, 1, 0, 0, 0, 0, NULL))
        return INVALID_FILE_ATTRIBUTES;
    return realAttrW(p);
}

static BOOL WINAPI HookAttrExA(LPCSTR p, GET_FILEEX_INFO_LEVELS level,
                               LPVOID info) {
    if (Hide("GetFileAttributesExA", SH_FILE_ATTR, p, 0, 0, 0, 0, 0, info))
        return FALSE;
    return realAttrExA(p, level, info);
}

static BOOL WINAPI HookAttrExW(LPCWSTR p, GET_FILEEX_INFO_LEVELS level,
                               LPVOID info) {
    if (Hide("GetFileAttributesExW", SH_FILE_ATTR, p, 1, 0, 0, 0, 0, info))
        return FALSE;
    return realAttrExW(p, level, info);
}

SH_API ShFileRule *ShFileRuleAdd(const ShFileRuleDesc *desc) {
    struct ShFileRule *r = NULL;
    int i;
    if (!desc || !(desc->group & SUPPORTED) ||
        (desc->group & ~SUPPORTED) || desc->action != SH_FILE_HIDE ||
        (desc->name && wcslen(desc->name) >= NAME_MAX) ||
        (desc->suffix && wcslen(desc->suffix) >= PATH_MAX)) {
        ShSetError(SH_ERR_BAD_ARG);
        return NULL;
    }
    AcquireSRWLockExclusive(&g_adminLock);
    if (!HookAll()) {
        ReleaseSRWLockExclusive(&g_adminLock);
        ShSetError(SH_ERR_HOOK_FAILED);
        return NULL;
    }
    AcquireSRWLockExclusive(&g_rulesLock);
    for (i = 0; i < RULE_MAX; ++i)
        if (!g_rules[i].live && !g_rules[i].callbacks) {
            r = &g_rules[i];
            break;
        }
    if (r) {
        memset(r, 0, sizeof(*r));
        r->seq = ++g_nextSeq;
        r->group = desc->group;
        r->after = desc->after;
        r->user = desc->user;
        if (desc->name) wcscpy(r->name, desc->name);
        if (desc->suffix) wcscpy(r->suffix, desc->suffix);
        if ((r->name[0] &&
             !WideCharToMultiByte(CP_ACP, 0, r->name, -1, r->nameA,
                                  NAME_MAX, NULL, NULL)) ||
            (r->suffix[0] &&
             !WideCharToMultiByte(CP_ACP, 0, r->suffix, -1, r->suffixA,
                                  PATH_MAX, NULL, NULL))) {
            r = NULL;
        } else {
            r->live = 1;
            InterlockedIncrement(&g_live);
            InterlockedOr(&g_mask, (LONG)desc->group);
        }
    }
    ReleaseSRWLockExclusive(&g_rulesLock);
    if (!InterlockedCompareExchange(&g_live, 0, 0)) UnhookAll();
    ReleaseSRWLockExclusive(&g_adminLock);
    ShSetError(r ? SH_OK : SH_ERR_NO_CANDIDATE);
    return (ShFileRule *)r;
}

SH_API int ShFileRuleDel(ShFileRule *rule) {
    struct ShFileRule *r = (struct ShFileRule *)rule;
    LONG mask = 0;
    int i;
    if (!r || (uintptr_t)r < (uintptr_t)g_rules ||
        (uintptr_t)r >= (uintptr_t)(g_rules + RULE_MAX) ||
        ((uintptr_t)r - (uintptr_t)g_rules) % sizeof(g_rules[0]))
        return 0;
    AcquireSRWLockExclusive(&g_adminLock);
    AcquireSRWLockExclusive(&g_rulesLock);
    if (!r->live) {
        ReleaseSRWLockExclusive(&g_rulesLock);
        ReleaseSRWLockExclusive(&g_adminLock);
        return 0;
    }
    r->live = 0;
    InterlockedDecrement(&g_live);
    for (i = 0; i < RULE_MAX; ++i)
        if (g_rules[i].live) mask |= (LONG)g_rules[i].group;
    InterlockedExchange(&g_mask, mask);
    ReleaseSRWLockExclusive(&g_rulesLock);
    while (InterlockedCompareExchange(&r->callbacks, 0, 0)) Sleep(0);
    if (!InterlockedCompareExchange(&g_live, 0, 0)) UnhookAll();
    ReleaseSRWLockExclusive(&g_adminLock);
    ShSetError(SH_OK);
    return 1;
}

SH_API uint32_t ShFileCallCount(void) {
    return (uint32_t)InterlockedCompareExchange(&g_calls, 0, 0);
}
