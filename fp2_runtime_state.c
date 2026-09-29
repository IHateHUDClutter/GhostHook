#include <windows.h>
#define FP2_INTERNAL_IMPLEMENTATION 1
#include "fp2_internal.h"

extern int ShFp2HeadRehideActive(void);

static volatile LONG64 g_nativeHeadCandidate __attribute__((aligned(8)));
static volatile LONG g_nativeAdsState;
static volatile LONG g_nativeAdsKnown;

/* Called by the existing native ADS bridge. */
int ShFp2AdsRecord(const Fp2AdsRegs *regs, int site) {
    DWORD lastError = GetLastError();
    int owned;
    if (!regs || (site != 0 && site != 1)) {
        SetLastError(lastError);
        return 0;
    }
    owned = ShFp2HeadRehideActive();
    InterlockedExchange64(&g_nativeHeadCandidate, (LONG64)regs->rcx);
    InterlockedExchange(&g_nativeAdsState, site);
    InterlockedExchange(&g_nativeAdsKnown, 1);
    SetLastError(lastError);
    return owned;
}

/* The already-installed ADS post bridge remains a native pass-through. */
void ShFp2AdsPost(int site) { (void)site; }

uint64_t ShFp2NativeHeadCandidate(void) {
    return (uint64_t)InterlockedCompareExchange64(
        &g_nativeHeadCandidate, 0, 0);
}

int ShFp2NativeAdsState(void) {
    return InterlockedCompareExchange(&g_nativeAdsState, 0, 0) ? 1 : 0;
}

int ShFp2AdsActive(void) {
    return InterlockedCompareExchange(&g_nativeAdsKnown, 0, 0) &&
           !ShFp2NativeAdsState();
}
