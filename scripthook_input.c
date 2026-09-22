/* Input blocking. Keys: the game reads DirectInput through
 * our proxy (scripthook_dinput.c). Look: GetCursorPos, via
 * its import slot. GetAsyncKeyState: the modifiers only. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define IAT_ASYNCKEY   SH_IMG(0x1880FBB0)
#define IAT_CURSORPOS  SH_IMG(0x1880FBC0)

extern void ShSetError(int err);
extern int ShReadableAddr(uint64_t addr, size_t len);

typedef SHORT (WINAPI *AsyncKey_t)(int);
typedef BOOL  (WINAPI *CursorPos_t)(LPPOINT);

static AsyncKey_t  g_realKey = NULL;
static CursorPos_t g_realPos = NULL;
static int g_hooked = 0;
static SRWLOCK g_installLock = SRWLOCK_INIT;

static volatile uint32_t g_block = 0;
static POINT g_frozen;
static volatile int g_haveFrozen = 0;
static volatile uint8_t g_keyBlock[256];
static volatile int g_anyKeyBlock = 0;

/* Never swallowed, so a player can always pause, alt tab
 * or reach the menu whatever a mod is doing.
 */
static int Escapes(int vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_TAB || vk == VK_F4 || vk == VK_ESCAPE ||
           vk == VK_LWIN || vk == VK_RWIN;
}

static volatile int g_capture;
static int Install(void);

static volatile int g_menuSuppress;
static volatile int g_menuDrain;
static volatile int g_menuToggle = VK_F4;

static int MenuOwned(int vk) {
    return vk == VK_UP || vk == VK_DOWN ||
           vk == VK_LEFT || vk == VK_RIGHT ||
           vk == VK_RETURN || vk == VK_BACK ||
           vk == VK_ESCAPE || vk == g_menuToggle;
}

void ShMenuSuppressKeys(int on, int toggleVk) {
    int vk;

    g_menuToggle = toggleVk;
    if (on) {
        if (!g_menuSuppress) Install();
        g_menuSuppress = 1;
        g_menuDrain = 0;
    } else {
        if (g_menuSuppress) g_menuDrain = 1;
        g_menuSuppress = 0;
        if (g_menuDrain) {
            for (vk = 1; vk < 256; ++vk)
                if (MenuOwned(vk) && (GetAsyncKeyState(vk) & 0x8000))
                    return;
            g_menuDrain = 0;
        }
    }
}

/* one rule for the poll stub and the DirectInput wrapper */
static int Suppressed(int vk) {
    uint32_t b = g_block;

    if (vk <= 0 || vk >= 256) return 0;
    if ((g_menuSuppress || g_menuDrain) && MenuOwned(vk)) return 1;
    if (Escapes(vk)) return 0;
    if (g_capture) return 1;
    if (g_anyKeyBlock && g_keyBlock[vk]) return 1;
    if (!b) return 0;
    if (b & SH_INPUT_KEYS) return 1;
    if ((b & SH_INPUT_MOVE) &&
        (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' ||
         vk == VK_SPACE || vk == VK_SHIFT || vk == VK_CONTROL))
        return 1;
    if ((b & SH_INPUT_FIRE) && vk == VK_LBUTTON) return 1;
    if ((b & SH_INPUT_AIM) && vk == VK_RBUTTON) return 1;
    return 0;
}

static SHORT WINAPI KeyStub(int vk) {
    if (Suppressed(vk)) return 0;
    return g_realKey ? g_realKey(vk) : 0;
}

/* for the DirectInput proxy in scripthook_dinput.c */
int ShKeySuppressedVk(int vk) {
    return Suppressed(vk);
}

/* keyboard capture: every key but the escapes hidden */
SH_API int ShCaptureKeys(int on) {
    if (on && !Install()) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    g_capture = on ? 1 : 0;
    return 1;
}


/* The game turns by the change between polls, so handing
 * back the same point every time means it never turns.
 */
static BOOL WINAPI PosStub(LPPOINT p) {
    BOOL ok = g_realPos ? g_realPos(p) : FALSE;

    if (!p) return ok;
    if (g_block & SH_INPUT_LOOK) {
        if (!g_haveFrozen) {
            g_frozen = *p;
            g_haveFrozen = 1;
        }
        *p = g_frozen;
    } else {
        g_haveFrozen = 0;
    }
    return ok;
}

static int ValidTarget(void *target) {
    MEMORY_BASIC_INFORMATION mbi;
    DWORD access;

    if ((uintptr_t)target < 0x10000 || target == (void *)KeyStub ||
        target == (void *)PosStub || !VirtualQuery(target, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return 0;
    access = mbi.Protect & 0xFF;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

static int Install(void) {
    void *volatile *key;
    void *volatile *pos;
    void *keyOrig, *posOrig;
    DWORD keyProtect = 0, posProtect = 0, ignored;
    int keyWritable = 0, posWritable = 0, ok = 0;

    AcquireSRWLockExclusive(&g_installLock);
    if (g_hooked) { ok = 1; goto done; }
    key = (void *volatile *)(uintptr_t)IAT_ASYNCKEY;
    pos = (void *volatile *)(uintptr_t)IAT_CURSORPOS;
    if (key == pos || ((uintptr_t)key & 7) || ((uintptr_t)pos & 7) ||
        !ShReadableAddr((uint64_t)(uintptr_t)key, sizeof(void *)) ||
        !ShReadableAddr((uint64_t)(uintptr_t)pos, sizeof(void *))) goto done;
    keyOrig = *key;
    posOrig = *pos;
    if (!ValidTarget(keyOrig) || !ValidTarget(posOrig)) goto done;
    if ((g_realKey && (void *)g_realKey != keyOrig) ||
        (g_realPos && (void *)g_realPos != posOrig)) goto done;

    if (!VirtualProtect((void *)key, sizeof(void *), PAGE_READWRITE, &keyProtect))
        goto done;
    keyWritable = 1;
    if (!VirtualProtect((void *)pos, sizeof(void *), PAGE_READWRITE, &posProtect))
        goto restore;
    posWritable = 1;
    if (*key != keyOrig || *pos != posOrig) goto restore;

    g_realKey = (AsyncKey_t)keyOrig;
    g_realPos = (CursorPos_t)posOrig;
    MemoryBarrier();
    if (InterlockedCompareExchangePointer(key, (void *)KeyStub, keyOrig) != keyOrig)
        goto restore;
    if (InterlockedCompareExchangePointer(pos, (void *)PosStub, posOrig) != posOrig) {
        InterlockedCompareExchangePointer(key, keyOrig, (void *)KeyStub);
        goto restore;
    }
    g_hooked = 1;
    ok = 1;

restore:
    /* Reverse order preserves the original protection when slots share a page. */
    if (posWritable && !VirtualProtect((void *)pos, sizeof(void *), posProtect, &ignored))
        ok = 0;
    if (keyWritable && !VirtualProtect((void *)key, sizeof(void *), keyProtect, &ignored))
        ok = 0;
done:
    ReleaseSRWLockExclusive(&g_installLock);
    return ok;
}

/** Which inputs the game is told it is not receiving. 0
 *  hands everything back. See the SH_INPUT_ bits.
 */
SH_API int ShBlockInput(uint32_t mask) {
    if (mask && !Install()) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    if (!(mask & SH_INPUT_LOOK)) g_haveFrozen = 0;
    g_block = mask;
    ShSetError(SH_OK);
    return 1;
}

SH_API uint32_t ShBlockedInput(void) {
    return g_block;
}

/* one key hidden from the game, for UI that consumed it */
SH_API int ShBlockKey(int vk, int on) {
    int i, any = 0;
    if (vk <= 0 || vk >= 256) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (on && !Install()) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    g_keyBlock[vk] = on ? 1 : 0;
    for (i = 1; i < 256; i++) if (g_keyBlock[i]) any = 1;
    g_anyKeyBlock = any;
    return 1;
}
