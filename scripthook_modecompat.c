/* Narrow mode gate for the Reforged FOV Changer. The game supplies a
 * CreateGameMode description; an unknown description is never allowed. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "third_party/minhook/include/MinHook.h"

#define MODE_SITE_RVA  0x09B985E8u
#define MODE_PROOF_RVA 0x0391DFF0u
#define MODE_PROBE     0x180u
#define MODE_FRESH     30
#define ENTRY_MAX      32
#define MODE_UNKNOWN  (-1)

typedef void *(*CreateModeFn)(void *, void *, void *, void *);
typedef struct {
    HMODULE module;
    char name[48];
    uint32_t mask;
    ShPluginBlockedFn callback;
    void *user;
    int lastAllowed;
} Entry;

static SRWLOCK g_lock = SRWLOCK_INIT;
static Entry g_entries[ENTRY_MAX];
static int g_count;
static volatile LONG g_started;
static volatile LONG g_hooked;
static volatile LONG g_mode = MODE_UNKNOWN;
static volatile LONG g_fresh;
static volatile LONG g_sequence;
static volatile PVOID g_description;
static CreateModeFn g_original;

extern void ShSetError(int err);

static int ReadAt(uint64_t addr, void *out, size_t length) {
    SIZE_T got = 0;
    return addr >= 0x10000 &&
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr,
                          out, length, &got) && got == length;
}

/* Both a known entry sequence and the function's own RIP-relative log
 * reference must agree on the qualified TU25 image before it is hooked. */
static int SiteVerified(void) {
    static const uint8_t first[] = {
        0x48,0x89,0x74,0x24,0x50,0x48,0x85,0xC9,
        0x74,0x05,0xE8,0x99,0x84,0x8D,0xF6,0x48
    };
    uint8_t bytes[MODE_PROBE];
    uint64_t base = ShImageBase();
    uint64_t site = base + MODE_SITE_RVA;
    unsigned i;
    if (!ShIsTU25Build() || !ReadAt(site, bytes, sizeof(bytes)) ||
        memcmp(bytes, first, sizeof(first)) != 0) return 0;
    for (i = 0; i + 7 <= sizeof(bytes); ++i) {
        int32_t disp;
        if ((bytes[i] != 0x48 && bytes[i] != 0x4C) ||
            bytes[i+1] != 0x8D || (bytes[i+2] & 0xC7) != 0x05)
            continue;
        memcpy(&disp, bytes+i+3, sizeof(disp));
        if ((uint64_t)((int64_t)(site+i+7) + disp) ==
            base + MODE_PROOF_RVA) return 1;
    }
    return 0;
}

/* The detour does no dereference or formatting on the game's thread. */
static void *CreateModeDetour(void *self, void *description,
                              void *a3, void *a4) {
    InterlockedExchange(&g_mode, MODE_UNKNOWN);
    InterlockedExchange(&g_fresh, MODE_FRESH);
    InterlockedExchangePointer(&g_description, description);
    InterlockedIncrement(&g_sequence);
    return g_original(self, description, a3, a4);
}

static void TryInstall(void) {
    void *site;
    MH_STATUS status;
    if (g_hooked || !SiteVerified()) return;
    status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return;
    site = (void *)(uintptr_t)(ShImageBase() + MODE_SITE_RVA);
    status = MH_CreateHook(site, (void *)CreateModeDetour,
                           (void **)&g_original);
    if (status != MH_OK) return;
    status = MH_EnableHook(site);
    if (status != MH_OK) {
        MH_RemoveHook(site);
        g_original = NULL;
        return;
    }
    InterlockedExchange(&g_hooked, 1);
}

/* These are Reforged's observed table rows, accepted only after the
 * independent TU25 site/proof check above. Unknown rows stay blocked. */
static int ResolveDescription(void *description) {
    static const uint32_t safe[] = {0x038DC7F0u, 0x038DCD80u,
                                    0x038DCF80u};
    static const uint32_t blockedWar[] = {0x03908CB8u, 0x03908D98u};
    uint64_t pointer, base = ShImageBase();
    uint32_t rva;
    unsigned i;
    if (!description ||
        !ReadAt((uint64_t)(uintptr_t)description, &pointer, sizeof(pointer)) ||
        pointer < base || pointer - base >= ShImageSize())
        return MODE_UNKNOWN;
    rva = (uint32_t)(pointer - base);
    for (i = 0; i < sizeof(safe)/sizeof(safe[0]); ++i)
        if (rva == safe[i] || rva + 0x90u == safe[i]) return 0;
    if (rva == 0x038DD178u || rva + 0x90u == 0x038DD178u)
        return SH_MODE_BLACKLIST_MERCENARIES;
    for (i = 0; i < sizeof(blockedWar)/sizeof(blockedWar[0]); ++i)
        if (rva == blockedWar[i] || rva + 0x90u == blockedWar[i])
            return SH_MODE_BLACKLIST_GHOST_WAR;
    return MODE_UNKNOWN;
}

static int Allowed(const Entry *entry, LONG mode) {
    return mode != MODE_UNKNOWN &&
           ((entry->mask & (uint32_t)mode) == 0);
}

static DWORD WINAPI ModeThread(void *unused) {
    LONG seen = 0;
    int kept = MODE_UNKNOWN;
    unsigned ticks = 0;
    (void)unused;
    for (;;) {
        ShPluginBlockedFn callbacks[ENTRY_MAX];
        void *users[ENTRY_MAX];
        int allowed[ENTRY_MAX], blocked[ENTRY_MAX];
        int count = 0, i;
        LONG sequence;
        Sleep(200);
        if (!g_hooked && ++ticks % 10u == 0) TryInstall();
        sequence = InterlockedCompareExchange(&g_sequence, 0, 0);
        if (sequence != seen) {
            seen = sequence;
            kept = MODE_UNKNOWN;
        }
        if (seen && kept == MODE_UNKNOWN && g_fresh > 0) {
            int found = ResolveDescription((void *)g_description);
            if (found != MODE_UNKNOWN) {
                kept = found;
                InterlockedExchange(&g_mode, found);
            }
            InterlockedDecrement(&g_fresh);
        }
        AcquireSRWLockExclusive(&g_lock);
        for (i = 0; i < g_count; ++i) {
            Entry *e = &g_entries[i];
            int now = Allowed(e, g_mode);
            if (!e->callback || now == e->lastAllowed) continue;
            e->lastAllowed = now;
            callbacks[count] = e->callback;
            users[count] = e->user;
            allowed[count] = now;
            blocked[count] = now ? 0 :
                (g_mode == MODE_UNKNOWN ? (int)e->mask :
                 (int)(e->mask & (uint32_t)g_mode));
            ++count;
        }
        ReleaseSRWLockExclusive(&g_lock);
        for (i = 0; i < count; ++i)
            callbacks[i](allowed[i], blocked[i], users[i]);
    }
    return 0;
}

static void Start(void) {
    HANDLE thread;
    if (InterlockedCompareExchange(&g_started, 1, 0)) return;
    TryInstall();
    thread = CreateThread(NULL, 0, ModeThread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

static HMODULE Caller(void *address) {
    HMODULE module = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &module)) return NULL;
    return module == GetModuleHandleA(NULL) ||
           module == GetModuleHandleA("dinput8.dll") ? NULL : module;
}

static Entry *EntryFor(HMODULE module, int create) {
    int i;
    for (i = 0; i < g_count; ++i)
        if (g_entries[i].module == module) return &g_entries[i];
    if (!create || g_count == ENTRY_MAX) return NULL;
    g_entries[g_count].module = module;
    {
        char path[MAX_PATH];
        const char *base;
        size_t length;
        path[0] = 0;
        GetModuleFileNameA(module, path, sizeof(path));
        base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        length = strlen(base);
        if (length > 4 && (!_stricmp(base + length - 4, ".asi") ||
                           !_stricmp(base + length - 4, ".dll")))
            length -= 4;
        if (length >= sizeof(g_entries[g_count].name))
            length = sizeof(g_entries[g_count].name) - 1;
        memcpy(g_entries[g_count].name, base, length);
        g_entries[g_count].name[length] = 0;
    }
    g_entries[g_count].mask = SH_MODE_BLACKLIST_GHOST_WAR |
                              SH_MODE_BLACKLIST_MERCENARIES;
    g_entries[g_count].lastAllowed = 0;
    return &g_entries[g_count++];
}

/* Reforged's undeclared-plugin default, scoped to the FP2 caller.
 * It leaves an explicit registration, including FOV Changer's, intact. */
void ShPluginRegisterDefault(void *address) {
    HMODULE caller = Caller(address);
    if (!caller) return;
    Start();
    AcquireSRWLockExclusive(&g_lock);
    EntryFor(caller, 1);
    ReleaseSRWLockExclusive(&g_lock);
}

/* Menu-only query; the model and its alphabetical ordering stay intact. */
int ShPluginHidden(const char *owner) {
    int i, hidden = 0;
    if (!owner || !*owner) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_count; ++i)
        if (!_stricmp(g_entries[i].name, owner)) {
            hidden = !Allowed(&g_entries[i], g_mode);
            break;
        }
    ReleaseSRWLockShared(&g_lock);
    return hidden;
}

SH_API int ShPluginBlacklist(uint32_t modes) {
    HMODULE caller = Caller(__builtin_return_address(0));
    Entry *entry;
    if (!caller || (modes & ~(SH_MODE_BLACKLIST_GHOST_WAR |
                             SH_MODE_BLACKLIST_MERCENARIES))) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    Start();
    AcquireSRWLockExclusive(&g_lock);
    entry = EntryFor(caller, 1);
    if (entry) entry->mask = modes;
    ReleaseSRWLockExclusive(&g_lock);
    if (!entry) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShPluginAllowed(void) {
    HMODULE caller = Caller(__builtin_return_address(0));
    Entry *entry;
    int result;
    if (!caller) return 1;
    Start();
    AcquireSRWLockExclusive(&g_lock);
    entry = EntryFor(caller, 0);
    result = entry ? Allowed(entry, g_mode) : 0;
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

SH_API int ShPluginOnBlocked(ShPluginBlockedFn fn, void *user) {
    HMODULE caller = Caller(__builtin_return_address(0));
    Entry *entry;
    if (!caller) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    Start();
    AcquireSRWLockExclusive(&g_lock);
    entry = EntryFor(caller, 1);
    if (entry) {
        entry->callback = fn;
        entry->user = user;
        entry->lastAllowed = Allowed(entry, g_mode);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!entry) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    ShSetError(SH_OK);
    return 1;
}
