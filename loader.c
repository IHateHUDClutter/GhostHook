#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "log.h"
#include "scripthook.h"
#include "image.h"

typedef HRESULT (WINAPI *DirectInput8Create_t)(
    HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT (WINAPI *DllCanUnloadNow_t)(void);
typedef HRESULT (WINAPI *DllGetClassObject_t)(
    REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *DllRegisterServer_t)(void);
typedef HRESULT (WINAPI *DllUnregisterServer_t)(void);

typedef struct {
    uint32_t rva;
    int count;
} PoolCandidate;

static HMODULE g_realDinput8;

static DirectInput8Create_t p_DirectInput8Create;
static DllCanUnloadNow_t p_DllCanUnloadNow;
static DllGetClassObject_t p_DllGetClassObject;
static DllRegisterServer_t p_DllRegisterServer;
static DllUnregisterServer_t p_DllUnregisterServer;

static volatile LONG g_poolState;
static uint32_t g_poolRva;
static volatile LONG g_pluginsLoaded;

extern void ShStateStartup(void);
extern void ShCrashStartup(void);
extern void ShWrapDirectInput(void *di);

static int ReadMem(
    uint64_t addr,
    void *out,
    size_t len)
{
    SIZE_T got = 0;

    if (!addr || !out || !len)
        return 0;

    if (!ReadProcessMemory(
            GetCurrentProcess(),
            (const void *)(uintptr_t)addr,
            out,
            len,
            &got))
        return 0;

    return got == len;
}

static uint64_t ReadQ(uint64_t addr)
{
    uint64_t v = 0;

    ReadMem(addr, &v, sizeof(v));
    return v;
}

static int Readable(
    uint64_t addr,
    size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t end;

    if (addr < 0x10000ULL)
        return 0;

    if (!VirtualQuery(
            (const void *)(uintptr_t)addr,
            &mbi,
            sizeof(mbi)))
        return 0;

    if (mbi.State != MEM_COMMIT)
        return 0;

    if (mbi.Protect &
        (PAGE_NOACCESS | PAGE_GUARD))
        return 0;

    end =
        (uint64_t)(uintptr_t)mbi.BaseAddress +
        mbi.RegionSize;

    return addr + len >= addr &&
           addr + len <= end;
}

static uint32_t FollowOneE9(uint32_t rva)
{
    uint8_t b[5];
    int32_t rel;

    if (!rva ||
        rva + 5 > ShImageSize())
        return 0;

    if (!ReadMem(
            ShImageBase() + rva,
            b,
            sizeof(b)))
        return 0;

    if (b[0] != 0xE9)
        return rva;

    memcpy(&rel, b + 1, 4);

    return (uint32_t)(
        (int64_t)rva +
        5 +
        (int64_t)rel);
}

static int Tu25PoolRefOk(
    uint32_t movRva,
    uint32_t callRva,
    uint32_t poolRva,
    uint32_t allocCtx)
{
    uint8_t mov[7];
    uint8_t call[5];
    int32_t disp;
    int32_t rel;
    uint32_t slot;
    uint32_t target;

    if (!ReadMem(
            ShImageBase() + movRva,
            mov,
            sizeof(mov)) ||
        !ReadMem(
            ShImageBase() + callRva,
            call,
            sizeof(call)))
        return 0;

    if (mov[0] != 0x4C ||
        mov[1] != 0x8B ||
        mov[2] != 0x05 ||
        call[0] != 0xE8)
        return 0;

    memcpy(&disp, mov + 3, 4);
    memcpy(&rel, call + 1, 4);

    slot =
        (uint32_t)(
            (int64_t)movRva +
            7 +
            (int64_t)disp);

    target =
        (uint32_t)(
            (int64_t)callRva +
            5 +
            (int64_t)rel);

    if (slot != poolRva)
        return 0;

    if (target != allocCtx &&
        FollowOneE9(target) != allocCtx)
        return 0;

    return 1;
}

static uint32_t ResolveTu25PoolFast(void)
{
    const uint32_t poolRva = 0x04D78D80u;
    const uint32_t allocCtx = 0x016BF890u;
    uint64_t obj;

    if (poolRva + 8 > ShImageSize())
        return 0;

    if (!Tu25PoolRefOk(
            0x001539B2u,
            0x001539C6u,
            poolRva,
            allocCtx) ||
        !Tu25PoolRefOk(
            0x00164DD9u,
            0x00164DF1u,
            poolRva,
            allocCtx) ||
        !Tu25PoolRefOk(
            0x001916F2u,
            0x0019170Bu,
            poolRva,
            allocCtx))
        return 0;

    obj = ReadQ(
        ShImageBase() + poolRva);

    if (obj < 0x10000ULL ||
        obj >= 0x800000000000ULL ||
        !Readable(obj, 8))
        return 0;

    Log(
        "G_POOL GRW+0x%08X fast",
        poolRva);

    return poolRva;
}

static void AddPoolCandidate(
    PoolCandidate *c,
    int *n,
    uint32_t rva)
{
    int i;

    for (i = 0; i < *n; i++) {
        if (c[i].rva == rva) {
            c[i].count++;
            return;
        }
    }

    if (*n >= 32)
        return;

    c[*n].rva = rva;
    c[*n].count = 1;
    (*n)++;
}

static uint32_t ResolveTu25Pool(void)
{
    const uint32_t allocCtx = 0x016BF890u;
    const uint32_t oldPool = 0x04D78D00u;
    const uint32_t radius = 0x00004000u;
    const size_t chunkSize = 0x10000;

    PoolCandidate cand[32];
    int nc = 0;

    const IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;

    uint8_t *buf;
    int si;
    int best = -1;
    int tied = 0;

    memset(cand, 0, sizeof(cand));

    nt = ShImageNt();

    if (!nt)
        return 0;

    sec =
        IMAGE_FIRST_SECTION(
            (IMAGE_NT_HEADERS64 *)(uintptr_t)nt);

    buf =
        (uint8_t *)HeapAlloc(
            GetProcessHeap(),
            0,
            chunkSize + 64);

    if (!buf)
        return 0;

    for (si = 0;
         si < nt->FileHeader.NumberOfSections;
         si++) {

        uint32_t size =
            sec[si].Misc.VirtualSize;

        uint32_t off;

        for (off = 0;
             off < size;
             off += (uint32_t)chunkSize) {

            uint32_t primary = size - off;
            size_t want;
            SIZE_T got = 0;
            size_t i;

            if (primary > chunkSize)
                primary = (uint32_t)chunkSize;

            want = (size_t)primary + 64;

            if ((uint64_t)off + want > size)
                want = size - off;

            if (!ReadProcessMemory(
                    GetCurrentProcess(),
                    (const void *)(uintptr_t)(
                        ShImageBase() +
                        sec[si].VirtualAddress +
                        off),
                    buf,
                    want,
                    &got))
                continue;

            if (got < 53)
                continue;

            for (i = 48;
                 i + 5 <= got;
                 i++) {

                int32_t rel;
                uint32_t site;
                uint32_t raw;
                uint32_t final;
                int back;

                if (buf[i] != 0xE8)
                    continue;

                memcpy(
                    &rel,
                    buf + i + 1,
                    4);

                site =
                    sec[si].VirtualAddress +
                    off +
                    (uint32_t)i;

                raw =
                    (uint32_t)(
                        (int64_t)site +
                        5 +
                        (int64_t)rel);

                if (raw >= ShImageSize())
                    continue;

                final = FollowOneE9(raw);

                if (raw != allocCtx &&
                    final != allocCtx)
                    continue;

                for (back = 7;
                     back <= 48;
                     back++) {

                    size_t j =
                        i - (size_t)back;

                    int32_t disp;
                    uint32_t slot;
                    uint64_t obj;

                    if (j + 7 > got)
                        continue;

                    if (buf[j] != 0x4C ||
                        buf[j + 1] != 0x8B ||
                        buf[j + 2] != 0x05)
                        continue;

                    memcpy(
                        &disp,
                        buf + j + 3,
                        4);

                    slot =
                        (uint32_t)(
                            (int64_t)
                                sec[si].VirtualAddress +
                            off +
                            j +
                            7 +
                            (int64_t)disp);

                    if (slot < oldPool - radius ||
                        slot > oldPool + radius)
                        continue;

                    obj =
                        ReadQ(
                            ShImageBase() +
                            slot);

                    if (obj < 0x10000ULL ||
                        obj >= 0x800000000000ULL ||
                        !Readable(obj, 8))
                        continue;

                    AddPoolCandidate(
                        cand,
                        &nc,
                        slot);

                    break;
                }
            }
        }
    }

    HeapFree(
        GetProcessHeap(),
        0,
        buf);

    for (si = 0; si < nc; si++) {
        if (best < 0 ||
            cand[si].count >
                cand[best].count) {

            best = si;
            tied = 0;
        }
        else if (
            cand[si].count ==
                cand[best].count) {

            tied = 1;
        }
    }

    if (best < 0 || tied)
        return 0;

    Log(
        "G_POOL GRW+0x%08X refs=%d",
        cand[best].rva,
        cand[best].count);

    return cand[best].rva;
}

uint32_t ShTu25PoolRvaShared(void)
{
    LONG state;

    if (!ShIsTU25Build())
        return 0x04D78D00u;

    state =
        InterlockedCompareExchange(
            &g_poolState,
            1,
            0);

    if (state == 0) {
        g_poolRva =
            ResolveTu25PoolFast();

        if (!g_poolRva) {
            Log(
                "G_POOL fast validation failed; "
                "scanning");

            g_poolRva =
                ResolveTu25Pool();
        }

        InterlockedExchange(
            &g_poolState,
            2);

        return g_poolRva;
    }

    while (
        InterlockedCompareExchange(
            &g_poolState,
            2,
            2) != 2)
        Sleep(1);

    return g_poolRva;
}

static uint64_t __attribute__((ms_abi))
Tu25LabelUpdate(
    uint64_t priv,
    uint64_t result)
{
    int32_t zero = 0;

    (void)priv;

    if (result &&
        Readable(result, sizeof(zero))) {

        memcpy(
            (void *)(uintptr_t)result,
            &zero,
            sizeof(zero));
    }

    return result;
}

static uint64_t __attribute__((ms_abi))
Tu25LabelRegister(uint64_t priv)
{
    (void)priv;
    return 1;
}

uint64_t ShTu25SpecialAddress(
    uint64_t legacyRva)
{
    if (!ShIsTU25Build())
        return 0;

    if (legacyRva == 0x17490300ULL)
        return
            (uint64_t)(uintptr_t)
            Tu25LabelUpdate;

    if (legacyRva == 0x033359F0ULL)
        return
            (uint64_t)(uintptr_t)
            Tu25LabelRegister;

    return 0;
}

static int LoadASIPlugins(void)
{
    char dir[MAX_PATH];
    char pluginDir[MAX_PATH];
    char pat[MAX_PATH];
    char full[MAX_PATH];

    WIN32_FIND_DATAA fd;
    HANDLE h;
    char *slash;
    DWORD pathLen;
    int n;

    int loaded = 0;
    int failed = 0;

    if (InterlockedCompareExchange(
            &g_pluginsLoaded,
            1,
            0) != 0)
        return 1;

    pathLen = GetModuleFileNameA(NULL, dir, MAX_PATH);
    if (!pathLen || pathLen >= MAX_PATH) {

        Log("ASI: cannot find game directory");
        return 0;
    }

    slash = strrchr(dir, '\\');

    if (!slash) {
        Log("ASI: cannot find game directory");
        return 0;
    }
    slash[1] = 0;

    n = snprintf(pluginDir, sizeof(pluginDir), "%sGhostHookPlugins", dir);
    if (n < 0 || n >= (int)sizeof(pluginDir)) {
        Log("ASI: plugin directory path too long");
        return 0;
    }

    if (!CreateDirectoryA(pluginDir, NULL)) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            Log("ASI: cannot create GhostHookPlugins error=%lu", error);
            return 0;
        }
    }

    n = snprintf(pat, sizeof(pat), "%s\\*.asi", pluginDir);
    if (n < 0 || n >= (int)sizeof(pat)) {
        Log("ASI: plugin search path too long");
        return 0;
    }

    h =
        FindFirstFileA(
            pat,
            &fd);

    if (h ==
        INVALID_HANDLE_VALUE) {

        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            Log("ASI: none found in GhostHookPlugins");
            return 1;
        }
        Log("ASI: cannot scan GhostHookPlugins error=%lu", error);
        return 0;
    }

    do {
        HMODULE mod;

        if (fd.dwFileAttributes &
            FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (_strnicmp(
                fd.cFileName,
                "GRWTU25Probe_",
                14) == 0)
            continue;

        n = snprintf(
            full,
            sizeof(full),
            "%s\\%s",
            pluginDir,
            fd.cFileName);
        if (n < 0 || n >= (int)sizeof(full)) {
            failed++;
            Log("ASI failed: %s path too long", fd.cFileName);
            continue;
        }

        mod = LoadLibraryA(full);

        if (mod) {
            loaded++;

            Log(
                "ASI loaded: %s",
                fd.cFileName);
        }
        else {
            failed++;

            Log(
                "ASI failed: %s error=%lu",
                fd.cFileName,
                GetLastError());
        }

    } while (
        FindNextFileA(
            h,
            &fd));

    FindClose(h);

    Log(
        "ASI loaded=%d failed=%d",
        loaded,
        failed);

    return failed == 0;
}

static DWORD WINAPI LoaderThread(
    LPVOID p)
{
    (void)p;

    LoadASIPlugins();
    return 0;
}

static void LoadRealDinput8(void)
{
    char sysdir[MAX_PATH];

    GetSystemDirectoryA(
        sysdir,
        MAX_PATH);

    strcat(
        sysdir,
        "\\dinput8.dll");

    g_realDinput8 =
        LoadLibraryA(
            sysdir);

    if (!g_realDinput8) {
        Log(
            "real dinput8 load failed");
        return;
    }

    p_DirectInput8Create =
        (DirectInput8Create_t)
        GetProcAddress(
            g_realDinput8,
            "DirectInput8Create");

    p_DllCanUnloadNow =
        (DllCanUnloadNow_t)
        GetProcAddress(
            g_realDinput8,
            "DllCanUnloadNow");

    p_DllGetClassObject =
        (DllGetClassObject_t)
        GetProcAddress(
            g_realDinput8,
            "DllGetClassObject");

    p_DllRegisterServer =
        (DllRegisterServer_t)
        GetProcAddress(
            g_realDinput8,
            "DllRegisterServer");

    p_DllUnregisterServer =
        (DllUnregisterServer_t)
        GetProcAddress(
            g_realDinput8,
            "DllUnregisterServer");
}

static BOOL IsGRW(void)
{
    char path[MAX_PATH];
    char *name;

    GetModuleFileNameA(
        NULL,
        path,
        MAX_PATH);

    name = strrchr(path, '\\');
    name = name ? name + 1 : path;

    return
        _stricmp(
            name,
            "GRW.exe") == 0;
}

BOOL WINAPI DllMain(
    HINSTANCE inst,
    DWORD reason,
    LPVOID reserved)
{
    (void)reserved;

    if (reason ==
        DLL_PROCESS_ATTACH) {

        HANDLE thread;

        if (!IsGRW())
            return TRUE;

        DisableThreadLibraryCalls(inst);

        LogInit(
            "scripthook.log");

        Log(
            "GRW ScriptHook loader");

        Log(
            "built " __DATE__ " " __TIME__);

        LoadRealDinput8();

        ShCrashStartup();

        if (!ShIsLegacyBuild() &&
            !ShIsTU25Build()) {

            Log(
                "unsupported GRW build "
                "timestamp=%08X size=%llX",
                ShImageTimestamp(),
                (unsigned long long)
                    ShImageSize());

            return TRUE;
        }

        if (ShIsTU25Build())
            Log("TU25 build accepted");
        else
            Log("legacy build accepted");

        ShStateStartup();

        thread =
            CreateThread(
                NULL,
                0,
                LoaderThread,
                NULL,
                0,
                NULL);

        if (thread)
            CloseHandle(thread);
        else
            Log(
                "ASI loader thread failed "
                "error=%lu",
                GetLastError());
    }
    else if (
        reason ==
        DLL_PROCESS_DETACH) {

        if (g_logFile) {
            Log("unloading");
            LogClose();
        }

        if (g_realDinput8) {
            FreeLibrary(
                g_realDinput8);

            g_realDinput8 = NULL;
        }
    }

    return TRUE;
}

__declspec(dllexport)
HRESULT WINAPI DirectInput8Create(
    HINSTANCE inst,
    DWORD ver,
    REFIID iid,
    LPVOID *out,
    LPUNKNOWN outer)
{
    HRESULT hr;

    if (!p_DirectInput8Create)
        return E_FAIL;

    hr =
        p_DirectInput8Create(
            inst,
            ver,
            iid,
            out,
            outer);

    if (SUCCEEDED(hr) &&
        out &&
        *out)
        ShWrapDirectInput(*out);

    return hr;
}

__declspec(dllexport)
HRESULT WINAPI DllCanUnloadNow(void)
{
    if (p_DllCanUnloadNow)
        return p_DllCanUnloadNow();

    return S_FALSE;
}

__declspec(dllexport)
HRESULT WINAPI DllGetClassObject(
    REFCLSID clsid,
    REFIID iid,
    LPVOID *out)
{
    if (p_DllGetClassObject)
        return p_DllGetClassObject(
            clsid,
            iid,
            out);

    return E_FAIL;
}

__declspec(dllexport)
HRESULT WINAPI DllRegisterServer(void)
{
    if (p_DllRegisterServer)
        return p_DllRegisterServer();

    return E_FAIL;
}

__declspec(dllexport)
HRESULT WINAPI DllUnregisterServer(void)
{
    if (p_DllUnregisterServer)
        return p_DllUnregisterServer();

    return E_FAIL;
}