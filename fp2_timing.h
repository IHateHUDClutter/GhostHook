#ifndef FP2_TIMING_H
#define FP2_TIMING_H

#include <windows.h>
#include <stdint.h>

#define FP2_TIMING_RING_CAPACITY 8192u

enum { FP2_TIMING_CAMERA = 1, FP2_TIMING_MANAGER, FP2_TIMING_EVENT };
enum { FP2_TIMING_EV_ENABLE = 1, FP2_TIMING_EV_HEADSHOW, FP2_TIMING_EV_ADS,
       FP2_TIMING_EV_PLAYER, FP2_TIMING_EV_WORLD, FP2_TIMING_EV_MARK };
enum { FP2_TIMING_SKIP_NONE = 0, FP2_TIMING_SKIP_NOT_READY, FP2_TIMING_SKIP_BUILD,
       FP2_TIMING_SKIP_NOT_LIVE, FP2_TIMING_SKIP_CHAIN };
enum { FP2_TIMING_MGR_NONE = 0, FP2_TIMING_MGR_NO_CLAIM, FP2_TIMING_MGR_UNREADABLE,
       FP2_TIMING_MGR_UI_RECENT, FP2_TIMING_MGR_FP2_DECLINED };

typedef struct {
    volatile LONG64 published;
    LONG64 sequence;
    int kind, event, early, mode, ui, apply;
    int fpActive, headShow, adsRaw, adsNormal, handoff;
    int headAttempt, headCalled, headSkip, headTag, headFieldsValid;
    int arraysReady, poseRan, transformWritten, positionWritten;
    uint64_t object, cameraSequence, head, player;
    uint64_t head40, head68, eventA, eventB;
    uint16_t head4a, head4c, head56;
    DWORD thread;
    const char *headFailure, *fpReason;
    LONGLONG entry, exit;
    LONGLONG visibilityBegin, visibilityEnd;
    LONGLONG transformBegin, transformEnd;
    LONGLONG dominoBegin, dominoEnd;
    LONGLONG headBegin, headEnd;
    LONGLONG fieldsBegin, fieldsEnd;
} Fp2TimingSample;

int ShFp2TimingEnabled(void);
LONGLONG ShFp2TimingNow(void);
void ShFp2TimingBegin(Fp2TimingSample *sample, int kind, uint64_t object,
                uint64_t cameraSequence);
void ShFp2TimingFinish(Fp2TimingSample *sample);
void ShFp2TimingManagerCurrent(Fp2TimingSample *sample);
void ShFp2TimingHeadSkip(int hide, int why);
void ShFp2TimingHeadFound(int hide, uint64_t head, unsigned tag,
                    const char *failure);
void ShFp2TimingHeadCalled(void);
void ShFp2TimingReason(const char *reason);
void ShFp2TimingEvent(int event, uint64_t a, uint64_t b);
int ShFp2TimingStart(void);
int ShFp2TimingMark(void);
void ShFp2TimingStop(void);

/* Production timing hooks are compile-time no-ops. */
#define ShFp2TimingEnabled() 0
#define ShFp2TimingNow() 0
#define ShFp2TimingBegin(...) ((void)0)
#define ShFp2TimingFinish(...) ((void)0)
#define ShFp2TimingManagerCurrent(...) ((void)0)
#define ShFp2TimingHeadSkip(...) ((void)0)
#define ShFp2TimingHeadFound(...) ((void)0)
#define ShFp2TimingHeadCalled(...) ((void)0)
#define ShFp2TimingReason(...) ((void)0)
#define ShFp2TimingEvent(...) ((void)0)

#endif
