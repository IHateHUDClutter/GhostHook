#ifndef FP2_INTERNAL_H
#define FP2_INTERNAL_H

#include <stdint.h>

typedef struct {
    uint32_t apply;
    float pos[3];
    float back, up, yaw, pitch, roll, fov;
    int mode;
    uint64_t camera;
    int liveValid;
    float live[3];
} Fp2CameraState;

typedef struct {
    uint64_t sequence;
    int64_t qpc;
    const char *api;
} Fp2TraceToken;

typedef struct {
    uint64_t regions, rwRegions, bytesPresented;
    uint64_t readChunks, failedChunks, vtHits, ownershipCalls;
} Fp2ScanMetrics;

typedef struct {
    uint64_t directFields, directMatches, oneHopFields, oneHopMatches;
    int components, renderNodes, oneHopCandidates, readableChildren;
} Fp2DirectMetrics;

typedef struct {
    uint64_t rax, rbx, rcx, rdx, r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15, rbp, rsi, rdi, rsp;
} Fp2AdsRegs;

Fp2TraceToken ShFp2TraceEnter(const char *api, void *caller,
                            const char *fmt, ...);
void ShFp2TraceExit(Fp2TraceToken token, const char *fmt, ...);
void ShFp2TraceManagerWrite(uint32_t mask, const float before[3],
                           const float after[3]);
Fp2TraceToken ShFp2TracePlaceBegin(const float before[3]);
void ShFp2TracePlaceEnd(Fp2TraceToken token, const char *reason,
                       int anchor, int wrote, const float after[3]);
void ShFp2TraceEyeCapture(uint64_t arg, uint64_t a2, uint64_t a3,
                         uint64_t transform, uint64_t entity);
enum { FFP_VIS_ACQUIRE, FFP_VIS_OWN, FFP_VIS_RELEASE,
       FFP_VIS_REQUEST, FFP_VIS_KIND_COUNT };
void ShFp2TraceVisEvent(int kind, const char *fmt, ...);
void ShFp2TraceVisNodes(uint64_t entity, const uint64_t *nodes, int count);
enum { FFP_VISIBILITY_ACTIVATE, FFP_VISIBILITY_HOLD, FFP_VISIBILITY_ACTIVATE_RESULT,
       FFP_VISIBILITY_HOLD_LOST, FFP_VISIBILITY_DISABLE, FFP_VISIBILITY_KIND_COUNT };
void ShFp2TraceVisibilityEvent(int kind, const char *fmt, ...);
enum { FFP_DISABLE_ENTRY, FFP_DISABLE_RELEASE,
       FFP_DISABLE_SHOW_PUMP, FFP_DISABLE_EXIT,
       FFP_DISABLE_KIND_COUNT };
void ShFp2TraceDisableEvent(int kind, const char *fmt, ...);
int ShFp2TraceShowPumpWanted(void);
enum { FFP_CLEANUP_ENTRY, FFP_CLEANUP_NODE,
       FFP_CLEANUP_EXIT, FFP_CLEANUP_KIND_COUNT };
void ShFp2TraceCleanupEvent(int kind, const char *fmt, ...);
enum { FFP_REHIDE_BEGIN, FFP_REHIDE_SLOT,
       FFP_REHIDE_END, FFP_REHIDE_KIND_COUNT };
void ShFp2TraceRehideEvent(int kind, const char *fmt, ...);
enum { FFP_LIFECYCLE_BEGIN, FFP_LIFECYCLE_CHAIN, FFP_LIFECYCLE_END,
       FFP_LIFECYCLE_KIND_COUNT };
int ShFp2TraceLifecycleWanted(void);
double ShFp2TraceEnableElapsedMs(void);
void ShFp2TraceLifecycleEvent(int kind, const char *fmt, ...);
enum { FFP_ADS_TRACE_CHAIN_STEP, FFP_ADS_TRACE_CHAIN_CAPTURE,
       FFP_ADS_TRACE_COMPARE, FFP_ADS_TRACE_MATCH,
       FFP_ADS_TRACE_MISMATCH, FFP_ADS_TRACE_KIND_COUNT };
int ShFp2TraceAdsWanted(void);
void ShFp2TraceAdsEvent(int kind, const char *fmt, ...);
enum { FFP_STATE_ACQUIRE_BEGIN, FFP_STATE_RESOLVE,
       FFP_STATE_ACQUIRE_END, FFP_STATE_ADS_RECOVERY,
       FFP_STATE_RELEASE, FFP_STATE_UPDATE, FFP_STATE_KIND_COUNT };
void ShFp2TraceStateEvent(int kind, const char *fmt, ...);
int ShFp2TracePumpWanted(void);
Fp2TraceToken ShFp2TracePumpBegin(uint64_t entity, uint64_t node,
                               uint64_t sig, int visible, int once,
                               int flagsValid, uint16_t flags);
void ShFp2TracePumpEnd(Fp2TraceToken token, int result,
                      int flagsValid, uint16_t flags);
int ShFp2TraceAcquireAvailable(void);
Fp2TraceToken ShFp2TraceAcquireBegin(void);
Fp2TraceToken ShFp2TraceAcquireStageBegin(const char *stage);
void ShFp2TraceAcquireStageEnd(Fp2TraceToken token, const char *fmt, ...);
void ShFp2TraceAcquireEnd(Fp2TraceToken token, int success,
                         const char *reason);
Fp2TraceToken ShFp2TraceScanBegin(void);
void ShFp2TraceScanEnd(Fp2TraceToken token, const Fp2ScanMetrics *metrics,
                      uint64_t controller);
void ShFp2TraceCensusEvent(const char *fmt, ...);
Fp2TraceToken ShFp2TraceDirectBegin(void);
void ShFp2TraceDirectEvent(const char *fmt, ...);
void ShFp2TraceDirectEnd(Fp2TraceToken token,
                        const Fp2DirectMetrics *metrics,
                        const char *result);
int ShFp2AdsRecord(const Fp2AdsRegs *regs, int site);
void ShFp2TraceHeadCall(uint64_t head, unsigned tag, int hide,
                          const char *reason, uint64_t player);
void ShFp2TraceHeadChainFail(const char *failure, unsigned tag,
                           const char *reason);
void ShFp2TraceWorldChange(const char *reason);
void ShFp2TraceEnd(const char *reason);
int ShFp2TraceHeadProbeWanted(int hide);
void ShFp2TraceHeadProbe(uint64_t head, unsigned tag,
                           const char *reason, uint64_t target, int phase,
                           int beforeValid, uint16_t before,
                           int afterValid, uint16_t after,
                           int64_t beforeQpc, int64_t afterQpc);
void ShFp2TraceDirectBefore(uint64_t head);
void ShFp2TraceDirectAfter(uint64_t head);
void ShFp2AdsPost(int site);
void ShFp2TraceHeadCamera(uint64_t head, unsigned tag, const char *phase);
void ShFp2TraceAdsEntry(uint64_t head, int site, int owned);
void ShFp2TraceAdsPost(int site);
int ShFp2TraceGuardOwns(uint64_t head);
void ShFp2TraceAdsGround(uint64_t controller, uint64_t array,
                        uint64_t entity, const uint64_t *heads,
                        int headCount, const uint64_t *renderNodes,
                        int renderCount);
uint64_t ShFp2NativeHeadCandidate(void);
int ShFp2NativeAdsState(void);
int ShFp2AdsActive(void);
void ShFp2TraceGateAds(int returned);
void ShFp2TraceAlignEvent(int entering, const float before[3],
                         const float target[3], const float after[3],
                         int targetValid, float opticFov,
                         int wrote, const char *result);
void ShFp2TraceNativeFovEdge(int entering);
void ShFp2TraceNativeFovOptic(float engineFov);
Fp2TraceToken ShFp2TraceHeadResolveBegin(uint64_t entity, uint64_t cached,
                                       uint64_t candidate);
void ShFp2TraceHeadResolveCandidate(Fp2TraceToken token, uint64_t candidate,
                                    int alive, int64_t startQpc);
Fp2TraceToken ShFp2TraceFallbackBegin(Fp2TraceToken token);
void ShFp2TraceFallbackEnd(Fp2TraceToken token);
void ShFp2TraceHeadResolveEnd(Fp2TraceToken token, const char *source,
                              uint64_t controller);
void ShFp2CameraStateSnapshot(Fp2CameraState *out);
void ShFp2FovStateSnapshot(uint32_t out[5]);
int ShFp2LifeState(void);

/* Production build retains only native ADS state. */
#ifndef FP2_INTERNAL_IMPLEMENTATION
#define ShFp2TraceEnter(...) ((Fp2TraceToken){0})
#define ShFp2TraceExit(...) ((void)0)
#define ShFp2TraceManagerWrite(...) ((void)0)
#define ShFp2TracePlaceBegin(...) ((Fp2TraceToken){0})
#define ShFp2TracePlaceEnd(...) ((void)0)
#define ShFp2TraceEyeCapture(...) ((void)0)
#define ShFp2TraceVisEvent(...) ((void)0)
#define ShFp2TraceVisNodes(...) ((void)0)
#define ShFp2TraceVisibilityEvent(...) ((void)0)
#define ShFp2TraceDisableEvent(...) ((void)0)
#define ShFp2TraceShowPumpWanted() 0
#define ShFp2TraceCleanupEvent(...) ((void)0)
#define ShFp2TraceRehideEvent(...) ((void)0)
#define ShFp2TraceLifecycleWanted() 0
#define ShFp2TraceEnableElapsedMs() 0.0
#define ShFp2TraceLifecycleEvent(...) ((void)0)
#define ShFp2TraceAdsWanted() 0
#define ShFp2TraceAdsEvent(...) ((void)0)
#define ShFp2TraceStateEvent(...) ((void)0)
#define ShFp2TracePumpWanted() 0
#define ShFp2TracePumpBegin(...) ((Fp2TraceToken){0})
#define ShFp2TracePumpEnd(...) ((void)0)
#define ShFp2TraceAcquireAvailable() 0
#define ShFp2TraceAcquireBegin() ((Fp2TraceToken){0})
#define ShFp2TraceAcquireStageBegin(...) ((Fp2TraceToken){0})
#define ShFp2TraceAcquireStageEnd(...) ((void)0)
#define ShFp2TraceAcquireEnd(...) ((void)0)
#define ShFp2TraceScanBegin() ((Fp2TraceToken){0})
#define ShFp2TraceScanEnd(...) ((void)0)
#define ShFp2TraceCensusEvent(...) ((void)0)
#define ShFp2TraceDirectBegin() ((Fp2TraceToken){0})
#define ShFp2TraceDirectEvent(...) ((void)0)
#define ShFp2TraceDirectEnd(...) ((void)0)
#define ShFp2TraceAdsGround(...) ((void)0)
#define ShFp2TraceHeadCall(...) ((void)0)
#define ShFp2TraceHeadChainFail(...) ((void)0)
#define ShFp2TraceWorldChange(...) ((void)0)
#define ShFp2TraceEnd(...) ((void)0)
#define ShFp2TraceHeadProbeWanted(...) 0
#define ShFp2TraceHeadProbe(...) ((void)0)
#define ShFp2TraceDirectBefore(...) ((void)0)
#define ShFp2TraceDirectAfter(...) ((void)0)
#define ShFp2AdsPost(...) ((void)0)
#define ShFp2TraceHeadCamera(...) ((void)0)
#define ShFp2TraceAdsEntry(...) ((void)0)
#define ShFp2TraceAdsPost(...) ((void)0)
#define ShFp2TraceGuardOwns(...) 0
#define ShFp2TraceGateAds(...) ((void)0)
#define ShFp2TraceAlignEvent(...) ((void)0)
#define ShFp2TraceNativeFovEdge(...) ((void)0)
#define ShFp2TraceNativeFovOptic(...) ((void)0)
#define ShFp2TraceHeadResolveBegin(...) ((Fp2TraceToken){0})
#define ShFp2TraceHeadResolveCandidate(...) ((void)0)
#define ShFp2TraceFallbackBegin(...) ((Fp2TraceToken){0})
#define ShFp2TraceFallbackEnd(...) ((void)0)
#define ShFp2TraceHeadResolveEnd(...) ((void)0)
#endif

#endif
