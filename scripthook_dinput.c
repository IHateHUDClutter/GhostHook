#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"

extern int ShKeySuppressedVk(int vk);

typedef HRESULT (WINAPI *CreateDevice_t)(
    void *, const GUID *, void **, IUnknown *);
typedef HRESULT (WINAPI *GetState_t)(
    void *, DWORD, void *);
typedef HRESULT (WINAPI *GetData_t)(
    void *, DWORD, void *, DWORD *, DWORD);

static const GUID g_sysMouse = {
    0x6F1D2B60,
    0xD5A0,
    0x11CF,
    {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}
};

static const GUID g_sysKeyboard = {
    0x6F1D2B61,
    0xD5A0,
    0x11CF,
    {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}
};

enum {
    DEV_UNKNOWN = 0,
    DEV_KEYBOARD,
    DEV_MOUSE
};

#define MAX_VT 8
#define MAX_DEV 16

static void **g_diVt[MAX_VT];
static CreateDevice_t g_origCreate[MAX_VT];
static int g_nDi;

static void **g_devVt[MAX_VT];
static GetState_t g_origState[MAX_VT];
static GetData_t g_origData[MAX_VT];
static int g_nVt;
static int g_vtInstalled[MAX_VT];
static SRWLOCK g_wrapLock = SRWLOCK_INIT;

static void *g_devObj[MAX_DEV];
static int g_devKind[MAX_DEV];
static int g_nDev;

static int DikToVk(DWORD dik)
{
    UINT vk;

    switch (dik) {
    case 0xC8: return VK_UP;
    case 0xD0: return VK_DOWN;
    case 0xCB: return VK_LEFT;
    case 0xCD: return VK_RIGHT;
    case 0x9C: return VK_RETURN;
    case 0xC7: return VK_HOME;
    case 0xCF: return VK_END;
    case 0xC9: return VK_PRIOR;
    case 0xD1: return VK_NEXT;
    case 0xD2: return VK_INSERT;
    case 0xD3: return VK_DELETE;
    case 0x9D: return VK_RCONTROL;
    case 0xB8: return VK_RMENU;
    case 0xB5: return VK_DIVIDE;
    case 0xDB: return VK_LWIN;
    case 0xDC: return VK_RWIN;
    default:
        break;
    }

    if (dik & 0x80)
        vk = MapVirtualKeyA(
            0xE000u | (dik & 0x7Fu),
            MAPVK_VSC_TO_VK_EX);
    else
        vk = MapVirtualKeyA(
            dik,
            MAPVK_VSC_TO_VK_EX);

    return (int)vk;
}

static int KeySuppressed(DWORD dik)
{
    int vk;

    if (dik == 0 || dik > 255)
        return 0;

    vk = DikToVk(dik);

    return vk ? ShKeySuppressedVk(vk) : 0;
}

static int Patch(
    void **vt,
    int slot,
    void *fn,
    void **orig)
{
    DWORD old;

    if (!VirtualProtect(
            &vt[slot],
            sizeof(void *),
            PAGE_READWRITE,
            &old))
        return 0;

    *orig = vt[slot];
    vt[slot] = fn;

    VirtualProtect(
        &vt[slot],
        sizeof(void *),
        old,
        &old);

    return 1;
}

static int VtIndex(void *dev)
{
    void **vt;
    int i, found = -1;
    if (!dev) return -1;
    vt = *(void ***)dev;
    AcquireSRWLockShared(&g_wrapLock);
    /* Prepared originals remain valid for calls already in flight after rollback. */
    for (i = 0; i < MAX_VT; i++) {
        if (g_devVt[i] == vt) { found = i; break; }
    }
    ReleaseSRWLockShared(&g_wrapLock);
    return found;
}

static int DeviceKind(void *dev)
{
    int i, kind = DEV_UNKNOWN;
    AcquireSRWLockShared(&g_wrapLock);
    for (i = 0; i < g_nDev; i++) {
        if (g_devObj[i] == dev) { kind = g_devKind[i]; break; }
    }
    ReleaseSRWLockShared(&g_wrapLock);
    return kind;
}

static void RegisterDevice(
    void *dev,
    int kind)
{
    int i;

    for (i = 0; i < g_nDev; i++) {
        if (g_devObj[i] == dev) {
            g_devKind[i] = kind;
            return;
        }
    }

    if (g_nDev >= MAX_DEV)
        return;

    g_devObj[g_nDev] = dev;
    g_devKind[g_nDev] = kind;
    g_nDev++;
}

static void FilterMouseState(
    DWORD cb,
    void *data)
{
    uint32_t block;
    LONG *axis;
    uint8_t *buttons;

    if (!data || cb < 12)
        return;

    block = ShBlockedInput();
    axis = (LONG *)data;

    if (block & SH_INPUT_LOOK) {
        axis[0] = 0;
        axis[1] = 0;
    }

    if (cb < 14)
        return;

    buttons = (uint8_t *)data + 12;

    if (block & SH_INPUT_FIRE)
        buttons[0] = 0;

    if (block & SH_INPUT_AIM)
        buttons[1] = 0;
}

static void FilterMouseData(
    DWORD cb,
    void *data,
    DWORD n)
{
    uint8_t *p = (uint8_t *)data;
    uint32_t block = ShBlockedInput();
    DWORD i;

    if (!data || cb < 8)
        return;

    for (i = 0; i < n; i++) {
        uint8_t *rec = p + (size_t)i * cb;
        DWORD ofs;
        DWORD *value;

        memcpy(&ofs, rec, sizeof(ofs));
        value = (DWORD *)(void *)(rec + 4);

        if ((block & SH_INPUT_LOOK) &&
            (ofs == 0 || ofs == 4))
            *value = 0;

        if ((block & SH_INPUT_FIRE) &&
            ofs == 12)
            *value = 0;

        if ((block & SH_INPUT_AIM) &&
            ofs == 13)
            *value = 0;
    }
}

static HRESULT WINAPI HookGetState(
    void *dev,
    DWORD cb,
    void *data)
{
    int vi = VtIndex(dev);
    int kind = DeviceKind(dev);
    HRESULT hr;

    if (vi < 0)
        return E_FAIL;

    hr = g_origState[vi](
        dev,
        cb,
        data);

    if (!SUCCEEDED(hr) || !data)
        return hr;

    if (kind == DEV_KEYBOARD &&
        cb == 256) {

        uint8_t *k = (uint8_t *)data;
        DWORD d;

        for (d = 1; d < 256; d++) {
            if (k[d] && KeySuppressed(d))
                k[d] = 0;
        }
    }
    else if (kind == DEV_MOUSE) {
        FilterMouseState(cb, data);
    }

    return hr;
}

static HRESULT WINAPI HookGetData(
    void *dev,
    DWORD cb,
    void *data,
    DWORD *inout,
    DWORD flags)
{
    int vi = VtIndex(dev);
    int kind = DeviceKind(dev);
    HRESULT hr;

    if (vi < 0)
        return E_FAIL;

    hr = g_origData[vi](
        dev,
        cb,
        data,
        inout,
        flags);

    if (!SUCCEEDED(hr) ||
        !data ||
        !inout ||
        !cb)
        return hr;

    if (kind == DEV_KEYBOARD) {
        uint8_t *p = (uint8_t *)data;
        DWORD n = *inout;
        DWORD src;
        DWORD dst = 0;

        for (src = 0; src < n; src++) {
            DWORD ofs =
                *(DWORD *)(void *)
                (p + (size_t)src * cb);

            if (KeySuppressed(ofs))
                continue;

            if (dst != src) {
                memcpy(
                    p + (size_t)dst * cb,
                    p + (size_t)src * cb,
                    cb);
            }

            dst++;
        }

        *inout = dst;
    }
    else if (kind == DEV_MOUSE) {
        FilterMouseData(
            cb,
            data,
            *inout);
    }

    return hr;
}

static int ValidDeviceTarget(void *target)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD access;
    if (!target || target == (void *)HookGetState || target == (void *)HookGetData ||
        !VirtualQuery(target, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    access = mbi.Protect & 0xFF;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

static void WrapDevice(void *dev, int kind)
{
    void **vt;
    void *state, *data;
    DWORD oldState, oldData, ignored;
    int vi, freeSlot = -1;

    if (!dev) return;
    AcquireSRWLockExclusive(&g_wrapLock);
    RegisterDevice(dev, kind);
    vt = *(void ***)dev;
    for (vi = 0; vi < MAX_VT; vi++) {
        if (g_devVt[vi] == vt) break;
        if (!g_devVt[vi] && freeSlot < 0) freeSlot = vi;
    }
    if (vi == MAX_VT) vi = freeSlot;
    if (vi < 0 || g_vtInstalled[vi] || g_nVt >= MAX_VT) goto done;
    state = vt[9];
    data = vt[10];
    if (!ValidDeviceTarget(state) || !ValidDeviceTarget(data)) goto done;
    if (g_devVt[vi] && ((void *)g_origState[vi] != state ||
                        (void *)g_origData[vi] != data)) goto done;
    if (!VirtualProtect(&vt[9], sizeof(void *), PAGE_READWRITE, &oldState)) goto done;
    if (!VirtualProtect(&vt[10], sizeof(void *), PAGE_READWRITE, &oldData)) {
        VirtualProtect(&vt[9], sizeof(void *), oldState, &ignored);
        goto done;
    }
    if (vt[9] != state || vt[10] != data) goto restore;
    if (!g_devVt[vi]) {
        g_origState[vi] = (GetState_t)state;
        g_origData[vi] = (GetData_t)data;
        g_devVt[vi] = vt;
    }
    MemoryBarrier();
    if (InterlockedCompareExchangePointer(&vt[9], (void *)HookGetState, state) != state)
        goto restore;
    if (InterlockedCompareExchangePointer(&vt[10], (void *)HookGetData, data) != data) {
        InterlockedCompareExchangePointer(&vt[9], state, (void *)HookGetState);
        goto restore;
    }
    g_vtInstalled[vi] = 1;
    g_nVt++;
restore:
    /* Reverse order also restores correctly when both slots share a page. */
    VirtualProtect(&vt[10], sizeof(void *), oldData, &ignored);
    VirtualProtect(&vt[9], sizeof(void *), oldState, &ignored);
done:
    ReleaseSRWLockExclusive(&g_wrapLock);
}

static HRESULT WINAPI HookCreateDevice(
    void *di,
    const GUID *guid,
    void **out,
    IUnknown *outer)
{
    void **vt = *(void ***)di;
    int i;
    HRESULT hr;
    CreateDevice_t original;

    AcquireSRWLockShared(&g_wrapLock);
    for (i = 0; i < g_nDi; i++) {
        if (g_diVt[i] == vt)
            break;
    }

    original = i < g_nDi ? g_origCreate[i] : NULL;
    ReleaseSRWLockShared(&g_wrapLock);
    if (!original) return E_FAIL;

    hr = original(
        di,
        guid,
        out,
        outer);

    if (!SUCCEEDED(hr) ||
        !out ||
        !*out ||
        !guid)
        return hr;

    if (memcmp(
            guid,
            &g_sysKeyboard,
            sizeof(GUID)) == 0) {

        WrapDevice(
            *out,
            DEV_KEYBOARD);
    }
    else if (memcmp(
            guid,
            &g_sysMouse,
            sizeof(GUID)) == 0) {

        WrapDevice(
            *out,
            DEV_MOUSE);
    }

    return hr;
}

void ShWrapDirectInput(void *di)
{
    void **vt;
    int i;

    if (!di)
        return;

    AcquireSRWLockExclusive(&g_wrapLock);
    vt = *(void ***)di;

    for (i = 0; i < g_nDi; i++) {
        if (g_diVt[i] == vt)
            goto done;
    }

    if (g_nDi >= MAX_VT)
        goto done;

    g_diVt[g_nDi] = vt;

    if (Patch(
            vt,
            3,
            (void *)HookCreateDevice,
            (void **)&g_origCreate[g_nDi])) {

        g_nDi++;

    }
done:
    ReleaseSRWLockExclusive(&g_wrapLock);
}