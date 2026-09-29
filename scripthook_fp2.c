/* TU25 on-foot FP2 core. The existing GhostHook camera manager owns the
 * placement; FP2 head visibility uses the native visibility call. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "fp2_internal.h"
#include "fp2_timing.h"
#include "player_peek.h"

#define FP_ARGS_RVA 0x0A074190u
#define FP_HEAD_RVA 0x0188CE00u
#define FP_ADS_OUT_RVA 0x147FF653u
#define FP_ADS_IN_RVA 0x147FF669u
#define FP_ADS_OUT_POST_RVA 0x147FF658u
#define FP_ADS_IN_POST_RVA 0x147FF66Eu
#define FP_VIS_RVA 0x02A257C0u
#define FP_HEAD_ROOT_RVA 0x04B90638u
#define FP_HEAD_TAG_MAX 0x20u
#define FP_HEAD_WRITE 0x60u
#define FP_RING_SLOTS 256
#define FP_GEN_SHIFT 40
#define FP_NEAR_SQ 16.0f
#define FP_HOLD_MS 1500u
#define FP_PEEK_FALLBACK_MS 80u
#define FP_BASIS_MIN_SQ 1.0e-8
#define FP_SNAPSHOT_CALLS 256
#define FP2_DRY_RUN 0
#define FP2_ENABLE_CAMERA 1
#define FP2_ENABLE_ADS_ALIGNMENT 1
#define FP2_ENABLE_ADS_FOV 0

enum {
    FP_API_INSTALL,
    FP_API_READY,
    FP_API_ENABLE,
    FP_API_OFFSET,
    FP_API_GATE,
    FP_API_HEAD_SHOW
};

enum {
    FP_UNINSTALLED,
    FP_INSTALLING,
    FP_READY,
    FP_ACTIVE,
    FP_FAILED
};

typedef void (__attribute__((ms_abi)) *HeadFn)(uint64_t, uint64_t,
                                                uint64_t, uint64_t);
typedef void (__attribute__((ms_abi)) *VisFn)(uint64_t, uint64_t);

static volatile struct {
    uint64_t arg[FP_RING_SLOTS];
    uint64_t a2[FP_RING_SLOTS];
    uint64_t a3[FP_RING_SLOTS];
    uint64_t stamp[FP_RING_SLOTS];
} g_ring;
static volatile uint64_t g_ringStamp;
static volatile uint32_t g_generation = 1;
static volatile LONG g_life = FP_UNINSTALLED;

int ShFp2LifeState(void) { return g_life; }
static volatile LONG g_installFailed;
static volatile LONG64 g_traceSeq;
static volatile LONG g_liveArmed;
static FILE *g_liveFile;
static int64_t g_liveQpcFreq;
static SRWLOCK g_traceFileLock = SRWLOCK_INIT;
static volatile LONG g_want;
static volatile LONG g_headShow = 1;
static volatile uint64_t g_showUntil;
static volatile uint8_t g_ads;
static volatile LONG g_alignLast = -1;
static volatile uint64_t g_lastPlacedAt;
static float g_lastIncoming[3];
static double g_savedBasis[3][3], g_localOffset[3];
static uint64_t g_lastPlacedEntity;
static uint32_t g_lastPlacedGeneration;
static volatile LONG g_peekFallbackReady;
static volatile LONG g_offSeq;
static volatile float g_offset[3];
static volatile LONG g_loading;
static volatile LONG g_seenWorld;
static uint64_t g_playerEntity;
static uint64_t g_noPlayerAt;
static int g_noPlayerForgot;
static uint64_t g_heldArg, g_heldA2, g_heldA3, g_heldAt;
static uint32_t g_heldGen;
static SRWLOCK g_installLock = SRWLOCK_INIT;
static SRWLOCK g_offsetLock = SRWLOCK_INIT;
static uint8_t *g_argsStub, *g_adsOutStub, *g_adsInStub;
static uint8_t *g_adsOutPostStub, *g_adsInPostStub;
static volatile uint64_t g_adsNativeTarget __attribute__((used));
static volatile uint64_t g_adsOutPostReturn __attribute__((used));
static volatile uint64_t g_adsInPostReturn __attribute__((used));
#define ADS_SLOT(field, index) \
    _Static_assert(offsetof(Fp2AdsRegs, field) == (index) * 8, \
                   "ADS bridge register layout mismatch")
ADS_SLOT(rax, 0); ADS_SLOT(rbx, 1); ADS_SLOT(rcx, 2); ADS_SLOT(rdx, 3);
ADS_SLOT(r8, 4);  ADS_SLOT(r9, 5);  ADS_SLOT(r10, 6); ADS_SLOT(r11, 7);
ADS_SLOT(r12, 8); ADS_SLOT(r13, 9); ADS_SLOT(r14, 10); ADS_SLOT(r15, 11);
ADS_SLOT(rbp, 12); ADS_SLOT(rsi, 13); ADS_SLOT(rdi, 14); ADS_SLOT(rsp, 15);
_Static_assert(sizeof(Fp2AdsRegs) == 0x80, "ADS bridge snapshot size mismatch");
#undef ADS_SLOT

typedef struct {
    volatile LONG64 sequence;
    uint64_t args[4];
    int64_t entryQpc, exitQpc;
    uint64_t result;
    DWORD threadId;
    LONG entryLife, exitLife;
    int api, hasResult;
    volatile LONG completed;
} FpTraceRecord;

typedef struct {
    LONG64 sequence;
    int64_t entryQpc;
    DWORD threadId;
    int api;
    int liveAtEntry;
} FpTraceToken;

static FpTraceRecord g_trace[FP_SNAPSHOT_CALLS];

extern void *ShAllocNear(uint64_t target);
extern void ShSetError(int err);
extern void ShPluginRegisterDefault(void *address);
extern int ShStateReadOnly(void);
extern uint64_t ShReadQ(uint64_t address);
extern int ShReadMem(uint64_t address, void *out, size_t len);
extern float ShFovPeekEngine(void);
extern int ShFovObserveEngine(void);

static int Fp2ReadyState(void) {
    return g_life == FP_READY || g_life == FP_ACTIVE;
}

int ShFp2CameraReady(void) { return Fp2ReadyState(); }

static int HeadWritable(uint64_t address, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t start, offset;
    if (!address || !VirtualQuery((void *)(uintptr_t)address, &mbi,
                                  sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_NOACCESS) ||
        ((mbi.Protect & PAGE_GUARD) &&
         !ShFp2TraceGuardOwns(address)) ||
        !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        mbi.Type != MEM_PRIVATE) return 0;
    start = (uint64_t)(uintptr_t)mbi.BaseAddress;
    if (address < start) return 0;
    offset = address - start;
    return offset <= mbi.RegionSize && len <= mbi.RegionSize - offset;
}

/* Reforged's live head argument for FN_VIS, not a six-node controller. */
static uint64_t HeadPtrChain(unsigned *tagOut, const char **failure) {
    uint64_t a, c;
    uint16_t tag;
    *failure = "root";
    a = ShReadQ(SH_IMG(FP_HEAD_ROOT_RVA)); if (!a) return 0;
    *failure = "root+0x10";
    a = ShReadQ(a + 0x10); if (!a) return 0;
    *failure = "link+0x10";
    a = ShReadQ(a + 0x10); if (!a) return 0;
    *failure = "link_deref";
    a = ShReadQ(a); if (!a) return 0;
    *failure = "state+0x78";
    a = ShReadQ(a + 0x78); if (!a) return 0;
    *failure = "tag+0x10";
    c = ShReadQ(a + 0x10); if (!c) return 0;
    *failure = "tag_link+0x10";
    c = ShReadQ(c + 0x10); if (!c) return 0;
    *failure = "tag_read";
    if (!ShReadMem(c + 3, &tag, sizeof(tag))) return 0;
    tag &= 0xFFu;
    *tagOut = tag;
    *failure = "tag_out_of_range";
    if (tag > FP_HEAD_TAG_MAX) return 0;
    *failure = "table+0x27";
    a = ShReadQ(a + 0x27); if (!a) return 0;
    *failure = "table_slot";
    a = ShReadQ(a + (uint64_t)tag * 8u); if (!a) return 0;
    *failure = "node_not_writable_private";
    if (!HeadWritable(a, FP_HEAD_WRITE)) return 0;
    *failure = NULL;
    return a;
}

static int Fp2LivePlay(void) {
    int state = ShStateReadOnly();
    if (state != SH_STATE_INGAME && state != SH_STATE_DRONE &&
        state != SH_STATE_BINOCULAR && state != SH_STATE_CINEMATIC)
        return 0;
    if (ShGetUiState() & (SH_UI_LOADING | SH_UI_GAMEOVER)) return 0;
    return !ShInPauseMenu();
}

static void HeadVis(int hide, const char *reason) {
    const char *failure;
    unsigned tag = 0;
    uint64_t head;
    uint16_t before = 0, after = 0;
    LARGE_INTEGER beforeQpc = {0}, afterQpc = {0};
    int probe, beforeValid = 0, afterValid = 0;
    if (!Fp2ReadyState()) {
        ShFp2TimingHeadSkip(hide, FP2_TIMING_SKIP_NOT_READY); return;
    }
    if (!ShIsTU25Build() ||
        g_adsNativeTarget != ShImageBase() + FP_VIS_RVA) {
        ShFp2TimingHeadSkip(hide, FP2_TIMING_SKIP_BUILD); return;
    }
    if (!Fp2LivePlay()) {
        ShFp2TimingHeadSkip(hide, FP2_TIMING_SKIP_NOT_LIVE); return;
    }
    head = HeadPtrChain(&tag, &failure);
    if (!head) {
        ShFp2TimingHeadFound(hide, 0, tag, failure);
        ShFp2TraceHeadChainFail(failure, tag, reason);
        return;
    }
    if (hide) {
        DWORD savedLastError = GetLastError();
        ShFp2TraceDirectBefore(head);
        SetLastError(savedLastError);
    }
    probe = ShFp2TraceHeadProbeWanted(hide);
    if (probe) {
        DWORD savedLastError = GetLastError();
        beforeValid = ShReadMem(head + 0x54, &before, sizeof(before));
        QueryPerformanceCounter(&beforeQpc);
        SetLastError(savedLastError);
    }
    if (hide) {
        DWORD savedLastError = GetLastError();
        ShFp2TraceDirectAfter(head);
        SetLastError(savedLastError);
    }
    if (hide) ShFp2TraceHeadCamera(head, tag, "entry");
    ((VisFn)(uintptr_t)g_adsNativeTarget)(head, hide ? 1u : 0u);
    /* Observe the native call's result without delaying its hide request. */
    ShFp2TimingHeadFound(hide, head, tag, NULL);
    ShFp2TimingHeadCalled();
    if (hide) ShFp2TraceHeadCamera(head, tag, "exit");
    if (probe) {
        DWORD savedLastError = GetLastError();
        QueryPerformanceCounter(&afterQpc);
        afterValid = ShReadMem(head + 0x54, &after, sizeof(after));
        ShFp2TraceHeadProbe(head, tag, reason, g_adsNativeTarget, probe,
                              beforeValid, before, afterValid, after,
                              beforeQpc.QuadPart, afterQpc.QuadPart);
        SetLastError(savedLastError);
    }
    ShFp2TraceHeadCall(head, tag, hide, reason, g_playerEntity);
}

static const char *LifeName(LONG life) {
    switch (life) {
    case FP_UNINSTALLED: return "UNINSTALLED";
    case FP_INSTALLING: return "INSTALLING";
    case FP_READY: return "READY";
    case FP_ACTIVE: return "ACTIVE";
    case FP_FAILED: return "FAILED";
    default: return "INVALID";
    }
}

static void Diag(FILE *f, const char *fmt, ...) {
    va_list ap;
    if (!f) return;
    AcquireSRWLockExclusive(&g_traceFileLock);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    ReleaseSRWLockExclusive(&g_traceFileLock);
}

static const char *ApiName(int api) {
    static const char *names[] = {
        "ShFp2Install", "ShFp2Ready", "ShFp2Enable",
        "ShFp2SetOffset", "ShFp2Gate", "ShFp2HeadShow"
    };
    return api >= 0 && api < 6 ? names[api] : "UNKNOWN";
}

static float FloatFromArg(uint64_t arg) {
    uint32_t bits = (uint32_t)arg;
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t FloatArg(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void TraceArgs(int api, const uint64_t args[4],
                      char *out, size_t cap) {
    switch (api) {
    case FP_API_ENABLE:
        snprintf(out, cap, "on=%d", (int)args[0]);
        break;
    case FP_API_OFFSET:
        snprintf(out, cap, "right=%.6g forward=%.6g up=%.6g",
                 FloatFromArg(args[0]), FloatFromArg(args[1]),
                 FloatFromArg(args[2]));
        break;
    case FP_API_GATE:
        snprintf(out, cap, "menu_ptr=0x%llX drone_ptr=0x%llX "
                 "ads_ptr=0x%llX fresh_ptr=0x%llX",
                 (unsigned long long)args[0],
                 (unsigned long long)args[1],
                 (unsigned long long)args[2],
                 (unsigned long long)args[3]);
        break;
    case FP_API_HEAD_SHOW:
        snprintf(out, cap, "show=%d", (int)args[0]);
        break;
    default:
        snprintf(out, cap, "none");
        break;
    }
}

static FpTraceToken TraceEnter(int api, uint64_t a, uint64_t b,
                               uint64_t c, uint64_t d) {
    FpTraceToken token;
    LARGE_INTEGER qpc;
    uint64_t args[4] = {a, b, c, d};
    LONG life = g_life;
    QueryPerformanceCounter(&qpc);
    token.sequence = InterlockedIncrement64(&g_traceSeq);
    token.entryQpc = qpc.QuadPart;
    token.threadId = GetCurrentThreadId();
    token.api = api;
    token.liveAtEntry = InterlockedCompareExchange(&g_liveArmed, 0, 0);
    if (token.sequence <= FP_SNAPSHOT_CALLS) {
        FpTraceRecord *r = &g_trace[token.sequence - 1];
        memcpy(r->args, args, sizeof(args));
        r->entryQpc = token.entryQpc;
        r->threadId = token.threadId;
        r->entryLife = life;
        r->api = api;
        InterlockedExchange64(&r->sequence, token.sequence);
    }
    if (token.liveAtEntry) {
        char argText[160];
        TraceArgs(api, args, argText, sizeof(argText));
        Diag(g_liveFile, "ENTRY seq=%lld api=%s thread=%lu "
             "qpc=%lld life=%s args={%s}",
             (long long)token.sequence, ApiName(api),
             (unsigned long)token.threadId,
             (long long)token.entryQpc, LifeName(life), argText);
    }
    return token;
}

static void TraceExit(FpTraceToken token, int hasResult,
                      uint64_t result) {
    LARGE_INTEGER qpc;
    LONG life = g_life;
    QueryPerformanceCounter(&qpc);
    if (token.sequence <= FP_SNAPSHOT_CALLS) {
        FpTraceRecord *r = &g_trace[token.sequence - 1];
        if (InterlockedCompareExchange64(&r->sequence, 0, 0) ==
            token.sequence) {
            r->result = result;
            r->hasResult = hasResult;
            r->exitLife = life;
            r->exitQpc = qpc.QuadPart;
            InterlockedExchange(&r->completed, 1);
        }
    }
    if (token.liveAtEntry) {
        double elapsedMs = g_liveQpcFreq > 0 ?
            (double)(qpc.QuadPart - token.entryQpc) * 1000.0 /
            (double)g_liveQpcFreq : 0.0;
        if (hasResult)
            Diag(g_liveFile, "EXIT seq=%lld api=%s thread=%lu "
                 "return=%llu qpc=%lld life=%s elapsed_ms=%.3f",
                 (long long)token.sequence, ApiName(token.api),
                 (unsigned long)token.threadId,
                 (unsigned long long)result, (long long)qpc.QuadPart,
                 LifeName(life), elapsedMs);
        else
            Diag(g_liveFile, "EXIT seq=%lld api=%s thread=%lu "
                 "return=void qpc=%lld life=%s elapsed_ms=%.3f",
                 (long long)token.sequence, ApiName(token.api),
                 (unsigned long)token.threadId,
                 (long long)qpc.QuadPart, LifeName(life), elapsedMs);
    }
}

/* Keep the validated installer while compiling optional tracing as no-ops. */
#define TraceEnter(...) ((FpTraceToken){0})
#define TraceExit(...) ((void)0)
#define Diag(...) ((void)0)


static void HexBytes(const uint8_t *bytes, size_t n, char *out,
                     size_t capacity) {
    size_t i, at = 0;
    if (!capacity) return;
    out[0] = 0;
    for (i = 0; i < n && at + 3 < capacity; ++i) {
        int written = snprintf(out + at, capacity - at,
                               "%s%02X", i ? " " : "", bytes[i]);
        if (written < 0 || (size_t)written >= capacity - at) break;
        at += (size_t)written;
    }
}

static int ReadAt(uint64_t addr, void *out, size_t length) {
    SIZE_T got = 0;
    return addr >= 0x10000 &&
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr,
                          out, length, &got) && got == length;
}

static uint64_t ReadQ(uint64_t addr) {
    uint64_t value = 0;
    return ReadAt(addr, &value, sizeof(value)) ? value : 0;
}

static int CallTarget(uint64_t site, uint64_t *out) {
    uint8_t bytes[5];
    int32_t displacement;
    if (!ReadAt(site, bytes, sizeof(bytes)) || bytes[0] != 0xE8)
        return 0;
    memcpy(&displacement, bytes + 1, sizeof(displacement));
    *out = (uint64_t)((int64_t)site + 5 + displacement);
    return 1;
}

static int CheckSite(FILE *diag, const char *name, uint32_t rva,
                     const uint8_t *expected, size_t length,
                     uint32_t targetRva) {
    uint8_t live[16] = {0};
    uint64_t base = ShImageBase(), target = 0;
    char expectedText[64], liveText[64];
    int read = length <= sizeof(live) &&
               ReadAt(base + rva, live, length);
    int decoded = CallTarget(base + rva, &target);
    int bytesMatch = read && memcmp(live, expected, length) == 0;
    int targetMatch = decoded && target == base + targetRva;
    int match = bytesMatch && targetMatch;
    HexBytes(expected, length, expectedText, sizeof(expectedText));
    if (read) HexBytes(live, length, liveText, sizeof(liveText));
    else strcpy(liveText, "UNREADABLE");
    Diag(diag, "%s RVA=0x%08X live=[%s] expected=[%s] "
         "read=%s bytes_match=%s target_decoded=%s "
         "decoded_target=0x%llX expected_target=GRW+0x%08X "
         "target_match=%s %s",
         name, rva, liveText, expectedText,
         read ? "PASS" : "FAIL", bytesMatch ? "PASS" : "FAIL",
         decoded ? "PASS" : "FAIL", (unsigned long long)target,
         targetRva, targetMatch ? "PASS" : "FAIL",
         match ? "PASS" : "FAIL");
    return match;
}

static int CheckThunk(FILE *diag, const char *name, uint32_t rva,
                      const uint8_t *expected) {
    uint8_t live[5] = {0};
    char expectedText[24], liveText[24];
    int read = ReadAt(ShImageBase() + rva, live, 5);
    int match = read && memcmp(live, expected, 5) == 0;
    HexBytes(expected, 5, expectedText, sizeof(expectedText));
    if (read) HexBytes(live, 5, liveText, sizeof(liveText));
    else strcpy(liveText, "UNREADABLE");
    Diag(diag, "%s RVA=0x%08X live=[%s] expected=[%s] %s",
         name, rva, liveText, expectedText,
         match ? "PASS" : "FAIL");
    return match;
}

/* All three live TU25 calls, their surrounding bytes, and both original
 * targets are checked before any executable byte is changed. */
static int ValidateTU25(FILE *diag, const char **failed) {
    static const uint8_t args[] = {
        0xE8,0x6B,0x8C,0x81,0xF7,0x0F,0x28,0x43,0x40
    };
    static const uint8_t adsOut[] = {
        0xE8,0x68,0x61,0x22,0xEE,0x48,0x8D,0x4C,0x24,0x20
    };
    static const uint8_t adsIn[] = {
        0xE8,0x52,0x61,0x22,0xEE,0x48,0x8D,0x4C,0x24,0x20
    };
    static const uint8_t headThunk[] = {0xE9,0x1B,0x78,0x59,0x0D};
    static const uint8_t visThunk[] = {0xE9,0xFB,0xDA,0xE8,0x11};
    int build = ShIsTU25Build();
    int site1, site2, site3, head, vis;
    Diag(diag, "BUILD image_base=0x%llX timestamp=0x%08X "
         "expected_timestamp=0x%08X image_size=0x%llX "
         "expected_image_size=0x%llX TU25_identity=%s",
         (unsigned long long)ShImageBase(), ShImageTimestamp(),
         SH_TU25_TIMESTAMP, (unsigned long long)ShImageSize(),
         (unsigned long long)SH_TU25_IMAGE_SIZE,
         build ? "PASS" : "FAIL");
    site1 = CheckSite(diag, "SITE1 head argument call", FP_ARGS_RVA,
                      args, sizeof(args), FP_HEAD_RVA);
    site2 = CheckSite(diag, "SITE2 ADS out", FP_ADS_OUT_RVA,
                      adsOut, sizeof(adsOut), FP_VIS_RVA);
    site3 = CheckSite(diag, "SITE3 ADS in", FP_ADS_IN_RVA,
                      adsIn, sizeof(adsIn), FP_VIS_RVA);
    head = CheckThunk(diag, "HEAD_THUNK", FP_HEAD_RVA, headThunk);
    vis = CheckThunk(diag, "VIS_THUNK", FP_VIS_RVA, visThunk);
    if (!build) *failed = "BUILD";
    else if (!site1) *failed = "SITE1";
    else if (!site2) *failed = "SITE2";
    else if (!site3) *failed = "SITE3";
    else if (!head) *failed = "HEAD_THUNK";
    else if (!vis) *failed = "VIS_THUNK";
    return build && site1 && site2 && site3 && head && vis;
}

static int EmitJump(uint8_t *stub, int offset, uint64_t to) {
    int64_t rel = (int64_t)to -
                  ((int64_t)(uintptr_t)(stub + offset) + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) return -1;
    stub[offset++] = 0xE9;
    *(int32_t *)(stub + offset) = (int32_t)rel;
    return offset + 4;
}

static uint8_t *NewStub(uint64_t site) {
    uint8_t *stub = (uint8_t *)ShAllocNear(site);
    if (stub) memset(stub, 0xCC, 0x1000);
    return stub;
}

/* The site is a call. This tail-jump preserves its return address and
 * enters the original engine function with the original four arguments. */
static uint8_t *BuildArgs(FILE *diag) {
    uint8_t *s = NewStub(ShImageBase()+FP_ARGS_RVA);
    int o = 0, jumpAt, jumpValid, flushed;
    int32_t jumpRel = 0;
    uint64_t decoded = 0;
    if (!s) {
        Diag(diag, "STUB head arguments allocation=FAIL source=GRW+0x%08X",
             FP_ARGS_RVA);
        return NULL;
    }
#if FP2_ENABLE_CAMERA
    s[o++] = 0x52;                         /* push rdx */
    s[o++] = 0x49; s[o++] = 0xBA;          /* mov r10, ring */
    *(uint64_t *)(s+o) = (uint64_t)(uintptr_t)&g_ring; o += 8;
    s[o++] = 0x89; s[o++] = 0xCA;          /* mov edx,ecx */
    s[o++] = 0xC1; s[o++] = 0xEA; s[o++] = 0x04;
    s[o++] = 0x31; s[o++] = 0xCA;          /* xor edx,ecx */
    s[o++] = 0xC1; s[o++] = 0xEA; s[o++] = 0x04;
    s[o++] = 0x0F; s[o++] = 0xB6; s[o++] = 0xD2;
    s[o++] = 0x49; s[o++] = 0x89; s[o++] = 0x0C; s[o++] = 0xD2;
    s[o++] = 0x4D; s[o++] = 0x89; s[o++] = 0x84; s[o++] = 0xD2;
    *(uint32_t *)(s+o) = FP_RING_SLOTS*8; o += 4;
    s[o++] = 0x4D; s[o++] = 0x89; s[o++] = 0x8C; s[o++] = 0xD2;
    *(uint32_t *)(s+o) = FP_RING_SLOTS*16; o += 4;
    s[o++] = 0x49; s[o++] = 0xBB;
    *(uint64_t *)(s+o) = (uint64_t)(uintptr_t)&g_ringStamp; o += 8;
    s[o++] = 0x4D; s[o++] = 0x8B; s[o++] = 0x1B;
    s[o++] = 0x4D; s[o++] = 0x89; s[o++] = 0x9C; s[o++] = 0xD2;
    *(uint32_t *)(s+o) = FP_RING_SLOTS*24; o += 4;
    s[o++] = 0x5A;                         /* pop rdx */
#endif
    jumpAt = o;
    o = EmitJump(s, o, ShImageBase()+FP_HEAD_RVA);
    if (o >= 0) {
        memcpy(&jumpRel, s+jumpAt+1, 4);
        decoded = (uint64_t)((int64_t)(uintptr_t)(s+jumpAt+5) +
                             jumpRel);
    }
    jumpValid = o >= 0 && s[jumpAt] == 0xE9 &&
                decoded == ShImageBase()+FP_HEAD_RVA;
    flushed = jumpValid &&
              FlushInstructionCache(GetCurrentProcess(), s, (size_t)o);
    Diag(diag, "STUB head arguments allocation=PASS address=0x%llX "
         "source=GRW+0x%08X jump_rel32=%lld representable=%s "
         "decoded_destination=0x%llX expected_destination=GRW+0x%08X "
         "flush=%s %s",
         (unsigned long long)(uintptr_t)s, FP_ARGS_RVA,
         (long long)((int64_t)(ShImageBase()+FP_HEAD_RVA) -
                     (int64_t)(uintptr_t)(s+jumpAt+5)),
         o >= 0 ? "YES" : "NO", (unsigned long long)decoded,
         FP_HEAD_RVA, flushed ? "PASS" : "FAIL",
         jumpValid && flushed ? "PASS" : "FAIL");
    if (!jumpValid || !flushed) {
        VirtualFree(s, 0, MEM_RELEASE);
        return NULL;
    }
    return s;
}

/* The original call has already pushed its return address. Preserve all
 * integer and volatile vector registers, flags, and that exact stack before
 * the state helper; then tail-jump to the validated native target. */
#define ADS_BRIDGE_BODY(site) \
    "pushfq\n\t" \
    "sub $0x120, %%rsp\n\t" \
    "mov %%rax, 0x20(%%rsp)\n\t" \
    "mov %%rbx, 0x28(%%rsp)\n\t" \
    "mov %%rcx, 0x30(%%rsp)\n\t" \
    "mov %%rdx, 0x38(%%rsp)\n\t" \
    "mov %%r8,  0x40(%%rsp)\n\t" \
    "mov %%r9,  0x48(%%rsp)\n\t" \
    "mov %%r10, 0x50(%%rsp)\n\t" \
    "mov %%r11, 0x58(%%rsp)\n\t" \
    "mov %%r12, 0x60(%%rsp)\n\t" \
    "mov %%r13, 0x68(%%rsp)\n\t" \
    "mov %%r14, 0x70(%%rsp)\n\t" \
    "mov %%r15, 0x78(%%rsp)\n\t" \
    "mov %%rbp, 0x80(%%rsp)\n\t" \
    "mov %%rsi, 0x88(%%rsp)\n\t" \
    "mov %%rdi, 0x90(%%rsp)\n\t" \
    "lea 0x128(%%rsp), %%rax\n\t" \
    "mov %%rax, 0x98(%%rsp)\n\t" \
    "movdqu %%xmm0, 0xA0(%%rsp)\n\t" \
    "movdqu %%xmm1, 0xB0(%%rsp)\n\t" \
    "movdqu %%xmm2, 0xC0(%%rsp)\n\t" \
    "movdqu %%xmm3, 0xD0(%%rsp)\n\t" \
    "movdqu %%xmm4, 0xE0(%%rsp)\n\t" \
    "movdqu %%xmm5, 0xF0(%%rsp)\n\t" \
    "lea 0x20(%%rsp), %%rcx\n\t" \
    "mov $" #site ", %%edx\n\t" \
    "call ShFp2AdsRecord\n\t" \
    /* Record native state first; only FP2's forwarded visibility is ours. */ \
    "test %%eax, %%eax\n\t" \
    "jz 1f\n\t" \
    "movb $1, 0x38(%%rsp)\n\t" \
    "1:\n\t" \
    "movdqu 0xA0(%%rsp), %%xmm0\n\t" \
    "movdqu 0xB0(%%rsp), %%xmm1\n\t" \
    "movdqu 0xC0(%%rsp), %%xmm2\n\t" \
    "movdqu 0xD0(%%rsp), %%xmm3\n\t" \
    "movdqu 0xE0(%%rsp), %%xmm4\n\t" \
    "movdqu 0xF0(%%rsp), %%xmm5\n\t" \
    "mov 0x20(%%rsp), %%rax\n\t" \
    "mov 0x28(%%rsp), %%rbx\n\t" \
    "mov 0x30(%%rsp), %%rcx\n\t" \
    "mov 0x38(%%rsp), %%rdx\n\t" \
    "mov 0x40(%%rsp), %%r8\n\t" \
    "mov 0x48(%%rsp), %%r9\n\t" \
    "mov 0x50(%%rsp), %%r10\n\t" \
    "mov 0x58(%%rsp), %%r11\n\t" \
    "mov 0x60(%%rsp), %%r12\n\t" \
    "mov 0x68(%%rsp), %%r13\n\t" \
    "mov 0x70(%%rsp), %%r14\n\t" \
    "mov 0x78(%%rsp), %%r15\n\t" \
    "mov 0x80(%%rsp), %%rbp\n\t" \
    "mov 0x88(%%rsp), %%rsi\n\t" \
    "mov 0x90(%%rsp), %%rdi\n\t" \
    "add $0x120, %%rsp\n\t" \
    "popfq\n\t" \
    "jmp *g_adsNativeTarget(%%rip)\n\t"

__attribute__((naked, used, noinline))
static void ShFpAdsOutBridge(void) { __asm__ volatile(ADS_BRIDGE_BODY(0) ::: "memory"); }

__attribute__((naked, used, noinline))
static void ShFpAdsInBridge(void) { __asm__ volatile(ADS_BRIDGE_BODY(1) ::: "memory"); }

/* Native FN_VIS returns to the original five-byte LEA. The state
 * bridge preserves its result state, observes it, replays the LEA, and
 * continues at the original following call. RSP is 16-aligned here. */
#define ADS_POST_BRIDGE_BODY(site, ret) \
    "pushfq\n\t" \
    "sub $0xC8, %%rsp\n\t" \
    "mov %%rax, 0x20(%%rsp)\n\t" \
    "mov %%rcx, 0x28(%%rsp)\n\t" \
    "mov %%rdx, 0x30(%%rsp)\n\t" \
    "mov %%r8,  0x38(%%rsp)\n\t" \
    "mov %%r9,  0x40(%%rsp)\n\t" \
    "mov %%r10, 0x48(%%rsp)\n\t" \
    "mov %%r11, 0x50(%%rsp)\n\t" \
    "movdqu %%xmm0, 0x60(%%rsp)\n\t" \
    "movdqu %%xmm1, 0x70(%%rsp)\n\t" \
    "movdqu %%xmm2, 0x80(%%rsp)\n\t" \
    "movdqu %%xmm3, 0x90(%%rsp)\n\t" \
    "movdqu %%xmm4, 0xA0(%%rsp)\n\t" \
    "movdqu %%xmm5, 0xB0(%%rsp)\n\t" \
    "mov $" #site ", %%ecx\n\t" \
    "call ShFp2AdsPost\n\t" \
    "movdqu 0x60(%%rsp), %%xmm0\n\t" \
    "movdqu 0x70(%%rsp), %%xmm1\n\t" \
    "movdqu 0x80(%%rsp), %%xmm2\n\t" \
    "movdqu 0x90(%%rsp), %%xmm3\n\t" \
    "movdqu 0xA0(%%rsp), %%xmm4\n\t" \
    "movdqu 0xB0(%%rsp), %%xmm5\n\t" \
    "mov 0x20(%%rsp), %%rax\n\t" \
    "mov 0x28(%%rsp), %%rcx\n\t" \
    "mov 0x30(%%rsp), %%rdx\n\t" \
    "mov 0x38(%%rsp), %%r8\n\t" \
    "mov 0x40(%%rsp), %%r9\n\t" \
    "mov 0x48(%%rsp), %%r10\n\t" \
    "mov 0x50(%%rsp), %%r11\n\t" \
    "add $0xC8, %%rsp\n\t" \
    "popfq\n\t" \
    "lea 0x20(%%rsp), %%rcx\n\t" \
    "jmp *" #ret "(%%rip)\n\t"

__attribute__((naked, used, noinline))
static void ShFpAdsOutPostBridge(void) {
    __asm__ volatile(ADS_POST_BRIDGE_BODY(0, g_adsOutPostReturn) ::: "memory");
}

__attribute__((naked, used, noinline))
static void ShFpAdsInPostBridge(void) {
    __asm__ volatile(ADS_POST_BRIDGE_BODY(1, g_adsInPostReturn) ::: "memory");
}

static uint8_t *BuildAds(FILE *diag, const char *name, uint64_t site) {
    uint8_t *s = NewStub(site);
    uint64_t bridge;
    int valid, flushed;
    if (!s) {
        Diag(diag, "STUB %s allocation=FAIL source=0x%llX",
             name, (unsigned long long)site);
        return NULL;
    }
    bridge = (uint64_t)(uintptr_t)(site == ShImageBase()+FP_ADS_OUT_RVA ?
             ShFpAdsOutBridge : ShFpAdsInBridge);
    g_adsNativeTarget = ShImageBase()+FP_VIS_RVA;
    s[0] = 0xFF; s[1] = 0x25;              /* jmp qword [rip+0] */
    memset(s+2, 0, 4);
    memcpy(s+6, &bridge, 8);
    valid = site == ShImageBase()+FP_ADS_OUT_RVA ||
            site == ShImageBase()+FP_ADS_IN_RVA;
    flushed = valid && FlushInstructionCache(GetCurrentProcess(), s, 14);
    Diag(diag, "STUB %s allocation=PASS address=0x%llX "
         "source=0x%llX bridge=0x%llX "
         "native_destination=GRW+0x%08X "
         "flush=%s %s",
         name, (unsigned long long)(uintptr_t)s,
         (unsigned long long)site,
         (unsigned long long)bridge,
         FP_VIS_RVA, flushed ? "PASS" : "FAIL",
         valid && flushed ? "PASS" : "FAIL");
    if (!valid || !flushed) {
        VirtualFree(s, 0, MEM_RELEASE);
        return NULL;
    }
    return s;
}

static uint8_t *BuildAdsPost(FILE *diag, const char *name, uint64_t site,
                             uint64_t bridge) {
    uint8_t *s = NewStub(site);
    int flushed;
    if (!s) return NULL;
    s[0] = 0xFF; s[1] = 0x25;
    memset(s + 2, 0, 4);
    memcpy(s + 6, &bridge, 8);
    flushed = FlushInstructionCache(GetCurrentProcess(), s, 14) != 0;
    Diag(diag, "STUB %s source=GRW+0x%08llX address=0x%llX "
         "bridge=0x%llX flush=%s", name,
         (unsigned long long)(site - ShImageBase()),
         (unsigned long long)(uintptr_t)s,
         (unsigned long long)bridge, flushed ? "PASS" : "FAIL");
    if (!flushed) {
        VirtualFree(s, 0, MEM_RELEASE);
        return NULL;
    }
    return s;
}

typedef struct {
    const char *name;
    uint64_t site;
    uint8_t original[5];
    uint8_t patch[5];
} FpPatch;

static void LogPatchFailure(const char *stage, const FpPatch *p) {
    FILE *f = fopen("scripthook.log", "a");
    if (f) {
        fprintf(f, "FP2 %s failed: %s GRW+0x%08llX\n", stage,
                p->name, (unsigned long long)(p->site-ShImageBase()));
        fclose(f);
    }
}

static int SaveCall(FILE *diag, FpPatch *p, const char *name,
                    uint64_t site, uint64_t from) {
    uint64_t actual = 0;
    char bytes[24];
    int read, decoded, valid;
    p->name = name;
    p->site = site;
    read = ReadAt(site, p->original, 5);
    decoded = CallTarget(site, &actual);
    valid = read && decoded && actual == from;
    if (read) HexBytes(p->original, 5, bytes, sizeof(bytes));
    else strcpy(bytes, "UNREADABLE");
    Diag(diag, "SAVE %s source=0x%llX original=[%s] "
         "decoded_target=0x%llX expected_target=0x%llX %s",
         name, (unsigned long long)site, bytes,
         (unsigned long long)actual, (unsigned long long)from,
         valid ? "PASS" : "FAIL");
    return valid;
}

static int SavePost(FILE *diag, FpPatch *p, const char *name,
                    uint64_t site) {
    static const uint8_t lea[5] = {0x48, 0x8D, 0x4C, 0x24, 0x20};
    int read;
    p->name = name;
    p->site = site;
    read = ReadAt(site, p->original, 5);
    Diag(diag, "SAVE %s source=GRW+0x%08llX original_lea=%s",
         name, (unsigned long long)(site - ShImageBase()),
         read && !memcmp(p->original, lea, 5) ? "PASS" : "FAIL");
    return read && !memcmp(p->original, lea, 5);
}

static int PrepareCall(FILE *diag, FpPatch *p, uint64_t to) {
    int64_t rel = (int64_t)to - ((int64_t)p->site + 5);
    int32_t displacement;
    uint64_t decoded;
    int representable = rel <= INT32_MAX && rel >= INT32_MIN;
    if (!representable) {
        Diag(diag, "PREPARE %s source=0x%llX stub=0x%llX "
             "rel32=%lld representable=NO FAIL",
             p->name, (unsigned long long)p->site,
             (unsigned long long)to, (long long)rel);
        return 0;
    }
    p->patch[0] = 0xE8;
    displacement = (int32_t)rel;
    memcpy(p->patch+1, &displacement, 4);
    decoded = (uint64_t)((int64_t)p->site + 5 + displacement);
    Diag(diag, "PREPARE %s source=0x%llX stub=0x%llX rel32=%lld "
         "representable=YES decoded_destination=0x%llX %s",
         p->name, (unsigned long long)p->site,
         (unsigned long long)to, (long long)rel,
         (unsigned long long)decoded,
         decoded == to ? "PASS" : "FAIL");
    return decoded == to;
}

static int PreparePost(FILE *diag, FpPatch *p, uint64_t to) {
    int64_t rel = (int64_t)to - ((int64_t)p->site + 5);
    int32_t displacement;
    if (rel < INT32_MIN || rel > INT32_MAX) {
        Diag(diag, "PREPARE %s rel32=UNREPRESENTABLE FAIL", p->name);
        return 0;
    }
    displacement = (int32_t)rel;
    p->patch[0] = 0xE9;
    memcpy(p->patch + 1, &displacement, sizeof(displacement));
    Diag(diag, "PREPARE %s source=GRW+0x%08llX stub=0x%llX PASS",
         p->name, (unsigned long long)(p->site - ShImageBase()),
         (unsigned long long)to);
    return (uint64_t)((int64_t)p->site + 5 + displacement) == to;
}

static int WriteVerified(FILE *diag, const char *stage,
                         const FpPatch *p, const uint8_t *bytes) {
    void *at = (void *)(uintptr_t)p->site;
    uint8_t found[5] = {0};
    char wanted[24], live[24];
    SIZE_T put = 0;
    DWORD old, unused;
    int protection, wrote = 0, protectedBack = 0, flushed = 0;
    int read, equal;
    protection = VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old) != 0;
    if (protection) {
        wrote = WriteProcessMemory(GetCurrentProcess(), at, bytes, 5,
                                   &put) && put == 5;
        protectedBack = VirtualProtect(at, 5, old, &unused) != 0;
        flushed = FlushInstructionCache(GetCurrentProcess(), at, 5) != 0;
    }
    read = ReadAt(p->site, found, 5);
    equal = read && memcmp(found, bytes, 5) == 0;
    HexBytes(bytes, 5, wanted, sizeof(wanted));
    if (read) HexBytes(found, 5, live, sizeof(live));
    else strcpy(live, "UNREADABLE");
    Diag(diag, "%s %s site=0x%llX VirtualProtect=%s "
         "write=%s bytes_written=%llu restore_protection=%s "
         "FlushInstructionCache=%s reread=[%s] intended=[%s] "
         "byte_compare=%s %s",
         stage, p->name, (unsigned long long)p->site,
         protection ? "PASS" : "FAIL", wrote ? "PASS" : "FAIL",
         (unsigned long long)put, protectedBack ? "PASS" : "FAIL",
         flushed ? "PASS" : "FAIL", live, wanted,
         equal ? "PASS" : "FAIL",
         protection && wrote && protectedBack && flushed && equal ?
         "PASS" : "FAIL");
    return protection && wrote && protectedBack && flushed && equal;
}

static int Install(FILE *diag, const char **failedStage) {
    uint64_t base = ShImageBase();
    FpPatch sites[5];
    const char *failed = "NONE";
    int i, j, ok = 0, rollbackOk = 1;
    int saved[5], prepared[5];
    AcquireSRWLockExclusive(&g_installLock);
    if (Fp2ReadyState()) {
        Diag(diag, "INSTALL already ready: lifecycle=%s PASS",
             LifeName(g_life));
        ok = 1;
        goto done;
    }
    if (g_installFailed) {
        failed = "PERMANENT_ROLLBACK_FAILURE";
        Diag(diag, "INSTALL refused: uncertain prior rollback FAIL");
        goto fail;
    }
    g_life = FP_INSTALLING;
    Diag(diag, "INSTALL lifecycle=INSTALLING");
    if (!ValidateTU25(diag, &failed)) goto fail;
    saved[0] = SaveCall(diag, &sites[0], "head arguments",
                        base+FP_ARGS_RVA, base+FP_HEAD_RVA);
    saved[1] = SaveCall(diag, &sites[1], "ADS out",
                        base+FP_ADS_OUT_RVA, base+FP_VIS_RVA);
    saved[2] = SaveCall(diag, &sites[2], "ADS in",
                        base+FP_ADS_IN_RVA, base+FP_VIS_RVA);
    saved[3] = SavePost(diag, &sites[3], "ADS out post",
                        base+FP_ADS_OUT_POST_RVA);
    saved[4] = SavePost(diag, &sites[4], "ADS in post",
                        base+FP_ADS_IN_POST_RVA);
    if (!saved[0] || !saved[1] || !saved[2] || !saved[3] || !saved[4]) {
        failed = !saved[0] ? "SAVE_SITE1" :
                 !saved[1] ? "SAVE_SITE2" : !saved[2] ? "SAVE_SITE3" :
                 !saved[3] ? "SAVE_POST_OUT" : "SAVE_POST_IN";
        goto fail;
    }
    g_adsOutPostReturn = base + FP_ADS_OUT_POST_RVA + 5;
    g_adsInPostReturn = base + FP_ADS_IN_POST_RVA + 5;
    if (!g_argsStub) g_argsStub = BuildArgs(diag);
    else Diag(diag, "STUB head arguments reused address=0x%llX PASS",
              (unsigned long long)(uintptr_t)g_argsStub);
    if (!g_adsOutStub)
        g_adsOutStub = BuildAds(diag, "ADS out", base+FP_ADS_OUT_RVA);
    else Diag(diag, "STUB ADS out reused address=0x%llX PASS",
              (unsigned long long)(uintptr_t)g_adsOutStub);
    if (!g_adsInStub)
        g_adsInStub = BuildAds(diag, "ADS in", base+FP_ADS_IN_RVA);
    else Diag(diag, "STUB ADS in reused address=0x%llX PASS",
              (unsigned long long)(uintptr_t)g_adsInStub);
    if (!g_adsOutPostStub)
        g_adsOutPostStub = BuildAdsPost(diag, "ADS out post",
            base+FP_ADS_OUT_POST_RVA,
            (uint64_t)(uintptr_t)ShFpAdsOutPostBridge);
    if (!g_adsInPostStub)
        g_adsInPostStub = BuildAdsPost(diag, "ADS in post",
            base+FP_ADS_IN_POST_RVA,
            (uint64_t)(uintptr_t)ShFpAdsInPostBridge);
    if (!g_argsStub || !g_adsOutStub || !g_adsInStub ||
        !g_adsOutPostStub || !g_adsInPostStub) {
        failed = !g_argsStub ? "STUB_SITE1" :
                 !g_adsOutStub ? "STUB_SITE2" :
                 !g_adsInStub ? "STUB_SITE3" :
                 !g_adsOutPostStub ? "STUB_POST_OUT" : "STUB_POST_IN";
        goto fail;
    }
    prepared[0] = PrepareCall(diag, &sites[0],
                              (uint64_t)(uintptr_t)g_argsStub);
    prepared[1] = PrepareCall(diag, &sites[1],
                              (uint64_t)(uintptr_t)g_adsOutStub);
    prepared[2] = PrepareCall(diag, &sites[2],
                              (uint64_t)(uintptr_t)g_adsInStub);
    prepared[3] = PreparePost(diag, &sites[3],
                             (uint64_t)(uintptr_t)g_adsOutPostStub);
    prepared[4] = PreparePost(diag, &sites[4],
                             (uint64_t)(uintptr_t)g_adsInPostStub);
    if (!prepared[0] || !prepared[1] || !prepared[2] ||
        !prepared[3] || !prepared[4]) {
        failed = !prepared[0] ? "PREPARE_SITE1" :
                 !prepared[1] ? "PREPARE_SITE2" :
                 !prepared[2] ? "PREPARE_SITE3" :
                 !prepared[3] ? "PREPARE_POST_OUT" : "PREPARE_POST_IN";
        goto fail;
    }
    g_ringStamp = ((uint64_t)g_generation << FP_GEN_SHIFT) |
                  (GetTickCount64() & (((uint64_t)1 << FP_GEN_SHIFT)-1));
    Diag(diag, "STATE ring generation=%u prepared PASS", g_generation);
    for (i = 0; i < 5; ++i) {
        uint8_t live[5];
        char liveText[24], originalText[24];
        int read = ReadAt(sites[i].site, live, 5);
        if (!read || memcmp(live, sites[i].original, 5) != 0) {
            if (read) HexBytes(live, 5, liveText, sizeof(liveText));
            else strcpy(liveText, "UNREADABLE");
            HexBytes(sites[i].original, 5, originalText,
                     sizeof(originalText));
            Diag(diag, "PREWRITE %s live=[%s] saved_original=[%s] FAIL",
                 sites[i].name, liveText, originalText);
            failed = i == 0 ? "PREWRITE_SITE1" :
                     i == 1 ? "PREWRITE_SITE2" :
                     i == 2 ? "PREWRITE_SITE3" :
                     i == 3 ? "PREWRITE_POST_OUT" : "PREWRITE_POST_IN";
            LogPatchFailure("pre-write check", &sites[i]);
            g_installFailed = 1;
            Diag(diag, "ROLLBACK begin reason=%s previous_sites=%d",
                 failed, i);
            for (j = i-1; j >= 0; --j)
                if (!WriteVerified(diag, "ROLLBACK", &sites[j],
                                   sites[j].original)) {
                    LogPatchFailure("rollback", &sites[j]);
                    rollbackOk = 0;
                }
            Diag(diag, "ROLLBACK final verified=%s permanent_failure=YES",
                 rollbackOk ? "YES" : "NO");
            goto fail;
        }
        Diag(diag, "PREWRITE %s saved-original comparison PASS",
             sites[i].name);
        if (!WriteVerified(diag, "PATCH", &sites[i], sites[i].patch)) {
            failed = i == 0 ? "PATCH_SITE1" :
                     i == 1 ? "PATCH_SITE2" :
                     i == 2 ? "PATCH_SITE3" :
                     i == 3 ? "PATCH_POST_OUT" : "PATCH_POST_IN";
            LogPatchFailure("install", &sites[i]);
            Diag(diag, "ROLLBACK begin reason=%s attempted_sites=%d",
                 failed, i+1);
            for (j = i; j >= 0; --j) {
                if (!WriteVerified(diag, "ROLLBACK", &sites[j],
                                   sites[j].original)) {
                    LogPatchFailure("rollback", &sites[j]);
                    rollbackOk = 0;
                }
            }
            if (!rollbackOk) g_installFailed = 1;
            Diag(diag, "ROLLBACK final verified=%s permanent_failure=%s",
                 rollbackOk ? "YES" : "NO",
                 g_installFailed ? "YES" : "NO");
            goto fail;
        }
    }
    g_life = FP_READY;
    ok = 1;
    Diag(diag, "ROLLBACK not triggered");
    goto done;
fail:
    g_life = FP_FAILED;
done:
    *failedStage = failed;
    ReleaseSRWLockExclusive(&g_installLock);
    return ok;
}

static uint64_t HeadTransform(uint64_t arg) {
    uint64_t a = ReadQ(arg);
    if (!a) return 0;
    a = ReadQ(a);
    return a ? ReadQ(a + 0x238u) : 0;
}

static int PickLocal(const ShVec3 *player, uint64_t *arg,
                     uint64_t *a2, uint64_t *a3) {
    uint64_t stamp = g_ringStamp;
    float best = FP_NEAR_SQ;
    int i, found = 0;
    for (i = 0; i < FP_RING_SLOTS; ++i) {
        uint64_t candidate = g_ring.arg[i];
        uint64_t tf;
        float pos[3], x, y, z, distance;
        if (!candidate ||
            (g_ring.stamp[i] >> FP_GEN_SHIFT) !=
            (stamp >> FP_GEN_SHIFT)) continue;
        tf = HeadTransform(candidate);
        if (!tf || !ReadAt(tf, pos, sizeof(pos))) continue;
        if (!isfinite(pos[0]) || !isfinite(pos[1]) ||
            !isfinite(pos[2])) continue;
        x = pos[0] - player->x;
        y = pos[1] - player->y;
        z = pos[2] - player->z;
        distance = x*x + y*y + z*z;
        if (!(distance < best)) continue;
        best = distance;
        *arg = candidate;
        *a2 = g_ring.a2[i];
        *a3 = g_ring.a3[i];
        found = 1;
    }
    return found;
}

static void ForgetCapture(void) {
    ++g_generation;
    InterlockedExchange(&g_peekFallbackReady, 0);
    g_lastPlacedEntity = 0;
    g_heldArg = 0;
    g_playerEntity = 0;
    g_ringStamp = ((uint64_t)g_generation << FP_GEN_SHIFT) |
                  (GetTickCount64() & (((uint64_t)1 << FP_GEN_SHIFT)-1));
}

/* The static player path never scans or waits. A changed soldier, or a
 * sustained absence during replacement, retires every old-world capture. */
static int PlayerSnapshot(ShVec3 *pos, uint64_t *entity,
                          enum ShPlayerPeekResult *reason) {
    ShPlayer player;
    *reason = ShPeekPlayerReason(&player);
    if (*reason != PEEK_OK) return 0;
    if (!player.entity ||
        !ReadAt(player.entity + 0x50u, pos, sizeof(*pos)) ||
        !isfinite(pos->x) || !isfinite(pos->y) || !isfinite(pos->z)) {
        return 0;
    }
    *entity = player.entity;
    return 1;
}

static int NormalizeFpAxis(const float axis[3], double unit[3]) {
    double lengthSq = 0.0;
    unsigned i;
    for (i = 0; i < 3; ++i) {
        if (!isfinite(axis[i])) return 0;
        lengthSq += (double)axis[i] * axis[i];
    }
    if (!isfinite(lengthSq) || lengthSq <= FP_BASIS_MIN_SQ) return 0;
    lengthSq = 1.0 / sqrt(lengthSq);
    for (i = 0; i < 3; ++i) unit[i] = (double)axis[i] * lengthSq;
    return 1;
}

void ShFp2PeekPlacementMissed(void) {
    InterlockedExchange(&g_peekFallbackReady, 0);
}

/* The manager callback runs this even after the camera bit is released. */
void ShFp2HeadFrame(void) {
    if (!Fp2ReadyState()) return;
    if (g_headShow && g_showUntil) {
        if (GetTickCount64() < g_showUntil)
            HeadVis(0, "show_window");
        else g_showUntil = 0;
    }
}

/* The capture ring belongs to a world, not to the plugin toggle. Track
 * replacement even while First Person is off so old captures cannot be
 * reused when a new session starts. */
void ShFp2WorldFrame(void) {
#if !FP2_ENABLE_CAMERA
    return;
#else
    int state;
    if (!Fp2ReadyState()) return;
    state = ShStateReadOnly();
    if (state != SH_STATE_INGAME) {
        InterlockedExchange(&g_peekFallbackReady, 0);
        if (g_seenWorld && (state == SH_STATE_RELOADING ||
            state == SH_STATE_MENU || state == SH_STATE_LOADING)) {
            if (!g_loading) {
                g_loading = 1;
                ShFp2TimingEvent(FP2_TIMING_EV_WORLD, 1, (uint64_t)state);
                ShFp2TraceWorldChange("world_left");
            }
        }
    } else {
        if (g_loading) {
            g_loading = 0;
            ShFp2TimingEvent(FP2_TIMING_EV_WORLD, 0, (uint64_t)state);
            ForgetCapture();
            ShFp2TraceWorldChange("world_entered");
        }
        g_seenWorld = 1;
    }
    g_ringStamp = ((uint64_t)g_generation << FP_GEN_SHIFT) |
                  (GetTickCount64() & (((uint64_t)1 << FP_GEN_SHIFT)-1));
#endif
}

int ShFp2PlaceEye(float *m, float *p) {
#if !FP2_ENABLE_CAMERA
    (void)m;
    (void)p;
    return 0;
#else
    __attribute__((aligned(16))) float out[8] = {0};
    uint64_t arg = 0, a2 = 0, a3 = 0, now, entity, transform;
    ShVec3 player;
    float ox, oy, oz, fx, fy, flat;
    float before[3] = {m[12], m[13], m[14]};
    float incomingAxes[3][3] = {
        {m[0], m[1], m[2]},
        {m[4], m[5], m[6]},
        {m[8], m[9], m[10]}
    };
    float alignTarget[3] = {0}, opticFov = 0.0f;
    Fp2TraceToken place = ShFp2TracePlaceBegin(before);
    const char *reason = "inactive";
    LONG seq0, seq1, priorAds;
    int spin = 0, state, anchor = 0, wrote = 0, snapshotOk;
    int validBasis = 1;
    int semanticAds = 0, alignEdge = 0, targetValid = 0;
    enum ShPlayerPeekResult peekReason = PEEK_OK;
    if (g_life != FP_ACTIVE || !g_want) {
        InterlockedExchange(&g_alignLast, -1);
        goto done;
    }
    semanticAds = !ShFp2NativeAdsState();
    priorAds = InterlockedExchange(&g_alignLast, semanticAds);
    alignEdge = priorAds >= 0 && priorAds != semanticAds;
    state = ShStateReadOnly();
    if (state != SH_STATE_INGAME || ShInPauseMenu()) {
        reason = "not_world_view";
        goto done;
    }
    if (!g_headShow) HeadVis(1, "camera");
    now = GetTickCount64();
#if FP2_ENABLE_ADS_ALIGNMENT
    if (semanticAds) {
        /* A magnified optic owns this frame before head-position work. */
        g_heldAt = now;
        opticFov = ShFovPeekEngine();
        if (opticFov > 0.0f && opticFov < 0.5f) {
            ShFp2TraceNativeFovOptic(opticFov);
            reason = "ads_optic";
            goto done;
        }
    }
#endif
    snapshotOk = PlayerSnapshot(&player, &entity, &peekReason);
    if (!snapshotOk) {
        uint64_t age = g_lastPlacedAt && now >= g_lastPlacedAt ?
                       now - g_lastPlacedAt : UINT64_MAX;
        int fallback = 0;
        if (peekReason == PEEK_LOCK_BUSY && !g_loading && g_seenWorld &&
            !g_headShow && age <= FP_PEEK_FALLBACK_MS &&
            g_lastPlacedEntity && g_lastPlacedEntity == g_playerEntity &&
            g_lastPlacedGeneration == g_generation) {
            float current[16], position[3];
            double basis[3][3];
            int valid = ShReadMem((uint64_t)(uintptr_t)m,
                                  current, sizeof(current));
            unsigned i, axis;
            if (valid) {
                for (i = 0; i < 3; ++i) {
                    if (!isfinite(current[12+i]) ||
                        !isfinite(g_lastIncoming[i]) ||
                        !isfinite(g_localOffset[i])) {
                        valid = 0;
                        break;
                    }
                }
            }
            if (valid) {
                for (axis = 0; axis < 3; ++axis) {
                    if (!NormalizeFpAxis(current + axis*4,
                                         basis[axis])) {
                        valid = 0;
                        break;
                    }
                    for (i = 0; i < 3; ++i) {
                        if (!isfinite(g_savedBasis[axis][i])) {
                            valid = 0;
                            break;
                        }
                    }
                    if (!valid) break;
                }
            }
            if (valid) {
                for (i = 0; i < 3; ++i) {
                    double value = current[12+i];
                    for (axis = 0; axis < 3; ++axis)
                        value += basis[axis][i] * g_localOffset[axis];
                    if (!isfinite(value) || fabs(value) > FLT_MAX) {
                        valid = 0;
                        break;
                    }
                    position[i] = (float)value;
                }
            }
            if (valid &&
                InterlockedCompareExchange(&g_peekFallbackReady, 0, 1) == 1) {
                m[12] = p[0] = position[0];
                m[13] = p[1] = position[1];
                m[14] = p[2] = position[2];
                m[15] = 1.0f;
                p[3] = 0.0f;
                alignTarget[0] = position[0];
                alignTarget[1] = position[1];
                alignTarget[2] = position[2];
                targetValid = wrote = fallback = 1;
                reason = "lock_busy_local_basis_fallback";
            }
        }
        if (fallback) goto done;
        InterlockedExchange(&g_peekFallbackReady, 0);
        if (!g_noPlayerAt) g_noPlayerAt = now;
        else if (!g_noPlayerForgot && now - g_noPlayerAt >= 500u) {
            ForgetCapture();
            g_noPlayerForgot = 1;
        }
        reason = peekReason == PEEK_OK ? "invalid_player_position" :
                 "no_player";
        goto done;
    }
    g_noPlayerAt = 0;
    g_noPlayerForgot = 0;
    if (g_playerEntity && g_playerEntity != entity) {
        ShFp2TimingEvent(FP2_TIMING_EV_PLAYER, g_playerEntity, entity);
        ForgetCapture();
    }
    g_playerEntity = entity;
    if (!PickLocal(&player, &arg, &a2, &a3)) {
        if (g_heldArg && g_heldGen == g_generation &&
            now-g_heldAt <= FP_HOLD_MS && HeadTransform(g_heldArg)) {
            arg = g_heldArg;
            a2 = g_heldA2;
            a3 = g_heldA3;
        } else {
            reason = "no_capture";
            goto done;
        }
    } else {
        g_heldArg = arg;
        g_heldA2 = a2;
        g_heldA3 = a3;
        g_heldAt = now;
        g_heldGen = g_generation;
    }
    transform = HeadTransform(arg);
    if (!transform) {
        reason = "invalid_transform";
        goto done;
    }
    anchor = 1;
    ShFp2TraceEyeCapture(arg, a2, a3, transform, entity);
    ((HeadFn)(uintptr_t)(ShImageBase()+FP_HEAD_RVA))(
        arg, (uint64_t)(uintptr_t)out, a2, a3);
    do {
        seq0 = g_offSeq;
        ox = g_offset[0];
        oy = g_offset[1];
        oz = g_offset[2];
        seq1 = g_offSeq;
    } while ((seq0 != seq1 || (seq0 & 1)) && ++spin < 8);
    if (seq0 != seq1 || (seq0 & 1)) {
        reason = "offset_race";
        goto done;
    }
    fx = m[4]; fy = m[5];
    flat = sqrtf(fx*fx + fy*fy);
    if (flat > 0.01f) { fx /= flat; fy /= flat; }
    else { fx = 0.0f; fy = 1.0f; }
    out[0] += m[0]*ox + fx*oy;
    out[1] += m[1]*ox + fy*oy;
    out[2] += m[2]*ox + oz;
    if (!isfinite(out[0]) || !isfinite(out[1]) ||
        !isfinite(out[2]) || fabsf(out[0]) > 1e6f ||
        fabsf(out[1]) > 1e6f || fabsf(out[2]) > 1e6f) {
        reason = "invalid_eye";
        goto done;
    }
    alignTarget[0] = out[0];
    alignTarget[1] = out[1];
    alignTarget[2] = out[2];
    targetValid = 1;
    m[12] = out[0]; m[13] = out[1]; m[14] = out[2]; m[15] = 1.0f;
    p[0] = out[0]; p[1] = out[1]; p[2] = out[2]; p[3] = 0.0f;
    g_lastPlacedAt = now;
    {
        double basis[3][3], delta[3], local[3];
        unsigned i, axis;
        for (i = 0; i < 3; ++i) {
            if (!isfinite(before[i])) validBasis = 0;
            delta[i] = (double)out[i] - before[i];
            if (!isfinite(delta[i])) validBasis = 0;
        }
        for (axis = 0; axis < 3; ++axis)
            if (!NormalizeFpAxis(incomingAxes[axis], basis[axis]))
                validBasis = 0;
        if (validBasis) {
            for (axis = 0; axis < 3; ++axis) {
                local[axis] = 0.0;
                for (i = 0; i < 3; ++i)
                    local[axis] += delta[i] * basis[axis][i];
                if (!isfinite(local[axis])) validBasis = 0;
            }
        }
        if (validBasis) {
            memcpy(g_savedBasis, basis, sizeof(basis));
            memcpy(g_localOffset, local, sizeof(local));
            memcpy(g_lastIncoming, before, sizeof(before));
        }
    }
    g_lastPlacedEntity = entity;
    g_lastPlacedGeneration = g_generation;
    InterlockedExchange(&g_peekFallbackReady, validBasis);
    wrote = 1;
    reason = "placed";
done:
    if (!wrote) InterlockedExchange(&g_peekFallbackReady, 0);
    ShFp2TimingReason(reason);
    if (alignEdge) {
        ShFp2TraceNativeFovEdge(semanticAds);
        ShFp2TraceAlignEvent(semanticAds, before, alignTarget, m + 12,
                            targetValid, opticFov, wrote, reason);
    }
    ShFp2TracePlaceEnd(place, reason, anchor, wrote, m + 12);
    return wrote;
#endif
}

SH_API int ShFp2Install(void) {
    FpTraceToken trace = TraceEnter(FP_API_INSTALL, 0, 0, 0, 0);
    FILE *diag = trace.liveAtEntry ? g_liveFile : NULL;
    const char *failed = "NONE";
    int ok;
    ShPluginRegisterDefault(__builtin_return_address(0));
    ok = Install(diag, &failed);
    Diag(diag, "FINAL lifecycle=%s ShFp2Install=%d ShFp2Ready=%d "
         "failed_stage=%s permanent_failure=%d",
         LifeName(g_life), ok, Fp2ReadyState(), failed,
         g_installFailed != 0);
    if (!ok) {
        ShSetError(SH_ERR_HOOK_FAILED);
        TraceExit(trace, 1, 0);
        return 0;
    }
    ShSetError(SH_OK);
    TraceExit(trace, 1, 1);
    return 1;
}

SH_API int ShFp2Ready(void) {
    FpTraceToken trace = TraceEnter(FP_API_READY, 0, 0, 0, 0);
    int ready = Fp2ReadyState();
    TraceExit(trace, 1, (uint64_t)ready);
    return ready;
}

int ShFp2OwnsEye(void) {
#if !FP2_ENABLE_CAMERA
    return 0;
#else
    return g_life == FP_ACTIVE && g_want;
#endif
}

int ShFp2HeadRehideActive(void) {
    return g_life == FP_ACTIVE && g_want && !g_headShow;
}

SH_API void ShFp2Enable(int on) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShFp2Enable",
        __builtin_return_address(0), "on=%d", on);
    FpTraceToken trace = TraceEnter(FP_API_ENABLE, (uint32_t)on,
                                    0, 0, 0);
    int wasEnabled = g_want != 0;
    if (!on) {
        InterlockedExchange(&g_alignLast, -1);
    }
    if (on) {
        if (!Fp2ReadyState() || g_installFailed) {
            TraceExit(trace, 0, 0);
            ShFp2TraceExit(diag, "void rejected=1");
            return;
        }
#if FP2_ENABLE_ADS_ALIGNMENT
        (void)ShFovObserveEngine();
#endif
#if FP2_ENABLE_ADS_FOV
        (void)ShFovEngine();
#endif
        if (!wasEnabled) {
            InterlockedExchange(&g_peekFallbackReady, 0);
            g_lastPlacedEntity = 0;
        }
        g_want = 1;
        g_headShow = 0;
        g_showUntil = 0;
        g_life = FP_ACTIVE;
        if (!wasEnabled) ShFp2TimingEvent(FP2_TIMING_EV_ENABLE, 1, 0);
        TraceExit(trace, 0, 0);
        ShFp2TraceExit(diag, "void accepted=1");
        return;
    }
    InterlockedExchange(&g_peekFallbackReady, 0);
    g_lastPlacedEntity = 0;
    if (g_life != FP_ACTIVE) {
        g_want = 0;
        TraceExit(trace, 0, 0);
        ShFp2TraceExit(diag, "void already_off=1");
        return;
    }
    g_want = 0;
    g_headShow = 1;
    g_showUntil = GetTickCount64() + 800u;
    g_heldArg = 0;
    g_life = FP_READY;
    if (wasEnabled) ShFp2TimingEvent(FP2_TIMING_EV_ENABLE, 0, 0);
    HeadVis(0, "enable_off");
    ShFp2TraceEnd("enable_off");
    TraceExit(trace, 0, 0);
    ShFp2TraceExit(diag, "void accepted=1");
}

SH_API void ShFp2SetOffset(float right, float forward, float up) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShFp2SetOffset",
        __builtin_return_address(0), "right=%.6g forward=%.6g up=%.6g",
        right, forward, up);
    FpTraceToken trace = TraceEnter(FP_API_OFFSET, FloatArg(right),
                                    FloatArg(forward), FloatArg(up), 0);
    if (!isfinite(right) || !isfinite(forward) || !isfinite(up) ||
        fabsf(right) > 1.0f || fabsf(forward) > 1.0f ||
        fabsf(up) > 1.0f) {
        TraceExit(trace, 0, 0);
        ShFp2TraceExit(diag, "void rejected=1");
        return;
    }
    AcquireSRWLockExclusive(&g_offsetLock);
    InterlockedIncrement(&g_offSeq);
    g_offset[0] = right;
    g_offset[1] = forward;
    g_offset[2] = up;
    InterlockedIncrement(&g_offSeq);
    ReleaseSRWLockExclusive(&g_offsetLock);
    TraceExit(trace, 0, 0);
    ShFp2TraceExit(diag, "void accepted=1");
}

SH_API void ShFp2Gate(int *menu, int *drone, int *ads, int *fresh) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShFp2Gate",
        __builtin_return_address(0), "menu=%p drone=%p ads=%p fresh=%p",
        (void *)menu, (void *)drone, (void *)ads, (void *)fresh);
    FpTraceToken trace = TraceEnter(FP_API_GATE,
                                    (uint64_t)(uintptr_t)menu,
                                    (uint64_t)(uintptr_t)drone,
                                    (uint64_t)(uintptr_t)ads,
                                    (uint64_t)(uintptr_t)fresh);
    int state = ShStateReadOnly();
    if (menu) *menu = state == SH_STATE_PAUSED ||
                      state == SH_STATE_MENU || ShInPauseMenu();
    if (drone) *drone = state == SH_STATE_DRONE ||
                        state == SH_STATE_BINOCULAR ||
                        state == SH_STATE_CINEMATIC;
    if (ads) {
        *ads = !ShFp2NativeAdsState();
        ShFp2TraceGateAds(*ads);
    }
    if (fresh) *fresh = g_lastPlacedAt &&
                        GetTickCount64()-g_lastPlacedAt < 250;
    TraceExit(trace, 0, 0);
    ShFp2TraceExit(diag, "void menu=%d drone=%d ads=%d fresh=%d",
        menu ? *menu : -1, drone ? *drone : -1,
        ads ? *ads : -1, fresh ? *fresh : -1);
}

SH_API void ShFp2HeadShow(int show) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShFp2HeadShow",
        __builtin_return_address(0), "show=%d", show);
    FpTraceToken trace = TraceEnter(FP_API_HEAD_SHOW,
                                    (uint32_t)show, 0, 0, 0);
    int wasShow = g_headShow != 0;
    g_headShow = show ? 1 : 0;
    if (wasShow != (show != 0))
        ShFp2TimingEvent(FP2_TIMING_EV_HEADSHOW, show != 0, 0);
    if (show) {
        g_showUntil = GetTickCount64() + 800u;
        HeadVis(0, "headshow");
    } else {
        g_showUntil = 0;
    }
    TraceExit(trace, 0, 0);
    ShFp2TraceExit(diag, "void stored_show=%ld", (long)g_headShow);
}
