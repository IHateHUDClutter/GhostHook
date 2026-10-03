/* Camera access. The engine rebuilds the transform every
 * frame, so an override is applied per call from inside
 * the engine's own call chain, never from a thread. */
/* Struct offsets, the write authority of +0x000 and the
 * yaw/pitch convention are Firejumper93's findings, MIT.
 */
/* https://github.com/Firejumper93/GhostReconWildlandsVR */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "fp2_internal.h"
#include "fp2_timing.h"

/* The projection selector. It takes the camera in RCX and
 * runs every frame, which is what makes it hookable.
 */
#define CAM_THUNK   SH_IMG(0x13781B0)
#define CAM_IMPL    SH_IMG(0xD7C0610)

/* Verified live: +0x2B0 is vertical fov in radians, planes
 * beside it. +0x2BC is an ASPECT multiplier, which the
 * selector scales by width over height. */
#define CAM_POSE    0x000
#define CAM_MODE    0x290
#define CAM_FOV     0x2B0
#define CAM_NEAR    0x2B4
#define CAM_FAR     0x2B8
#define CAM_ASPECT  0x2BC
#define CAM_SKEWX   0x2C4
#define CAM_SKEWY   0x2C8

/* Nine derived matrices, 0x40 apart, view and projection
 * and their inverses. Read only in practice.
 */
#define CAM_MATS    0x420
#define CAM_MAT_N   9

/* The camera manager, one frame ahead of the camera build.
 * The behaviour's transform lands here first, so an override
 * placed here reaches culling and the matrices together. */
#define MGR_SITE    SH_IMG(0x7E888FE)
#define MGR_LEN     5
#define MGR_NEXT    SH_IMG(0x10D8890)

/* Verified live in gameplay: the mode at +0x6C reads 3, so
 * consumers take the position from +0x170 while the render
 * camera takes the rows at +0x190. Both are restated. */
#define MGR_MODE    0x6C
#define MGR_POS     0x170
#define MGR_FOV     0x180
#define MGR_XFORM   0x190

/* Ownership is per field, so two plugins can hold different
 * parts of the camera at once. Orbit is a private bit: it
 * owns position, but derives it instead of storing it. */
#define CAM_ORBIT_BIT 0x100u
#define CAM_HEAD_BIT  0x200u
#define CAM_DERIVED   (CAM_ORBIT_BIT | CAM_HEAD_BIT)

static volatile uint64_t g_cam = 0;
static volatile uint64_t g_calls = 0;
static volatile uint64_t g_writes = 0;

/* Special views can run mode 0 like the main
 * camera (measured: the drone), so mode gates nothing.
 */
static volatile uint64_t g_otherAt = 0;
static volatile int g_otherMode = 0;

/* The pause menu never leaves the Playing game state, but
 * its camera is a template: a bit exact identity basis,
 * which no steered camera ever holds. Measured live. */
static volatile uint64_t g_uiAt = 0;
static volatile uint64_t g_headNearAt = 0;
static volatile uint64_t g_headWroteAt = 0;
static volatile uint64_t g_viewWantAt = 0;

static ShVec3 g_absPos;
static volatile float g_back = 0.0f;
static volatile float g_right = 0.0f;
static volatile float g_up = 0.0f;
static volatile float g_yaw = 0.0f;
static volatile float g_pitch = 0.0f;
static volatile float g_roll = 0.0f;
static volatile float g_fov = 0.0f;
static volatile float g_skewX = 0.0f;
static volatile float g_skewY = 0.0f;
static volatile int g_modeSet = 0;
static volatile uint32_t g_apply = 0;
static volatile LONG g_ffpTrackNextWrite;

void ShFp2CameraStateSnapshot(Fp2CameraState *out) {
    SIZE_T got = 0;
    memset(out, 0, sizeof(*out));
    out->apply = g_apply;
    out->pos[0] = g_absPos.x;
    out->pos[1] = g_absPos.y;
    out->pos[2] = g_absPos.z;
    out->back = g_back;
    out->up = g_up;
    out->yaw = g_yaw;
    out->pitch = g_pitch;
    out->roll = g_roll;
    out->fov = g_fov;
    out->mode = g_modeSet;
    out->camera = g_cam;
    if (out->camera)
        out->liveValid = ReadProcessMemory(GetCurrentProcess(),
            (LPCVOID)(uintptr_t)(out->camera + CAM_POSE + 12u*4u),
            out->live, sizeof(out->live), &got) && got == sizeof(out->live);
}

static uint8_t *g_camStub = NULL;
static int g_camHooked;
static volatile LONG g_camInstalled;
static SRWLOCK g_installLock = SRWLOCK_INIT;
static int ThunkPointsAtStub(void);
static uint8_t  g_thunkOrig[5];

static uint8_t *g_mgrStub = NULL;
static int      g_mgrHooked = 0;

extern void ShSetError(int err);
extern void ShVisibilityPump(void);
extern void ShTransformPump(void);
extern void ShDominoPump(void);
extern void ShHeadPump(void);
extern void ShHeadWant(void);
extern void ShFp2HeadFrame(void);
extern void ShFp2WorldFrame(void);
extern int ShFp2CameraReady(void);
extern int ShFp2PlaceEye(float *matrix, float *position);
extern void ShFp2PeekPlacementMissed(void);
extern int ShFp2OwnsEye(void);
extern int ShFovSet(float radians);
extern void ShFovClear(void);
extern int ShHeadCached(ShVec3 *out);
extern int ShReadableAddr(uint64_t addr, size_t len);
extern void *ShAllocNear(uint64_t target);

/* Row 3 is the translation, rows 0 to 2 the basis: row 0
 * right, row 1 forward, row 2 up. Engine owns the basis.
 */
/* The engine consumes this with no checks of its own, so a
 * NaN or a runaway value crashes it far from here. Refuse
 * the write and keep the engine's frame instead. */
static void WritePos(float *m, float x, float y, float z) {
    if (x != x || y != y || z != z) return;
    if (fabsf(x) > 1e6f || fabsf(y) > 1e6f || fabsf(z) > 1e6f)
        return;
    m[12] = x;
    m[13] = y;
    m[14] = z;
    m[15] = 1.0f;
    g_writes++;
}

/* Derived from the player and the engine's own basis, never
 * from the slot we write, so nothing can accumulate.
 */
static void ApplyOrbit(float *m) {
    ShVec3 p;
    float x, y, z, right;

    if (!ShGetPlayerPosition(&p)) return;
    x = p.x - m[4] * g_back;
    y = p.y - m[5] * g_back;
    z = p.z - m[6] * g_back + g_up;
    right = g_right;
    if (right != 0.0f) {
        x += m[0] * right;
        y += m[1] * right;
        z += m[2] * right;
    }
    WritePos(m, x, y, z);
}

/* Game basis: x right, y forward, z up. Rebuilt absolutely
 * from yaw and pitch, so roll is dropped.
 */
static void WriteRot(float *m) {
    float cy = cosf(g_yaw), sy = sinf(g_yaw);
    float cp = cosf(g_pitch), sp = sinf(g_pitch);
    float r[3], f[3], u[3];
    float cr, sr, i;

    r[0] = cy;       r[1] = -sy;      r[2] = 0.0f;
    f[0] = sy * cp;  f[1] = cy * cp;  f[2] = sp;
    u[0] = -sy * sp; u[1] = -cy * sp; u[2] = cp;

    /* Roll turns right and up about the forward axis, so
     * the view tilts without changing where it looks.
     */
    if (g_roll != 0.0f) {
        cr = cosf(g_roll);
        sr = sinf(g_roll);
        for (i = 0; i < 3; i += 1.0f) {
            int k = (int)i;
            float rr = r[k] * cr + u[k] * sr;
            float uu = -r[k] * sr + u[k] * cr;
            r[k] = rr;
            u[k] = uu;
        }
    }

    m[0] = r[0]; m[1] = r[1]; m[2] = r[2];
    m[4] = f[0]; m[5] = f[1]; m[6] = f[2];
    m[8] = u[0]; m[9] = u[1]; m[10] = u[2];
}

/* Iron sight ADS pulls the engine's hidden aim camera in
 * next to the head. Free roam keeps it metres back, which
 * is what separates the two cases below. */
#define CAM_ADS_NEAR 1.0f
#define CAM_ADS_FAR  1.7f

/* Iron sights run their ray through the eye. Over the
 * shoulder aim keeps it a fist or more to the right, and
 * that one should feel like hip fire, so it stays put. */
#define CAM_ADS_PMIN 0.12f
#define CAM_ADS_PMAX 0.22f

/* Firejumper93's gates. An engine camera beyond a chase
 * arm of the head is a drone or a remote view, and a
 * zoomed one is a scope drawing its own overlay. */
#define CAM_ARM_MAX  4.0f
#define CAM_ZOOM_FOV 0.30f

/* Vehicles run longer chase arms, so the head pump feeds
 * this hint on its own slow cadence. The frame path must
 * stay call free: player lookups here crashed the menu. */
static volatile int g_vehHint = 0;

void ShCameraVehicleHint(int inVehicle) {
    g_vehHint = inVehicle;
}

/* First person. The eye is the head bone, nudged forward
 * along the engine's own view axis to clear the face.
 */
static void ApplyHead(float *m, float fov) {
    ShVec3 h;
    float fx = m[4], fy = m[5], fz = m[6];
    float ex = m[12], ey = m[13], ez = m[14];
    float gx = fx, gy = fy, gl, len;
    float px, py, pz, d;

    if (!ShHeadCached(&h)) return;

    /* A zoomed camera is a scope. The mask and reticle
     * anchor to the engine's own view, so moving the eye
     * displaces them. Skip and let it render. */
    if (fov < CAM_ZOOM_FOV) return;

    d = sqrtf((ex - h.x) * (ex - h.x) + (ey - h.y) * (ey - h.y)
              + (ez - h.z) * (ez - h.z));

    /* On foot, an engine camera beyond a chase arm is not
     * looking through the soldier: a drone, a cutscene, a
     * tacmap. Vehicles keep longer arms, so they pass. */
    if (d > CAM_ARM_MAX && !g_vehHint) return;

    /* Flattened: nudging along a downward view would drop
     * the eye to the chest.
     */
    gl = sqrtf(gx * gx + gy * gy);
    if (gl > 0.01f) { gx /= gl; gy /= gl; }
    else { gx = 0.0f; gy = 1.0f; }
    px = h.x + gx * g_back;
    py = h.y + gy * g_back;
    pz = h.z + g_up;

    /* ADS only: ease the eye the few cm onto the engine's
     * aim ray, at head depth, so the sights and the
     * bullets pass through screen center. */
    len = sqrtf(fx * fx + fy * fy + fz * fz);
    if (d < CAM_ADS_FAR && len > 0.01f) {
        float t, w, perp;

        fx /= len; fy /= len; fz /= len;
        t = (h.x - ex) * fx + (h.y - ey) * fy + (h.z - ez) * fz;

        /* How far the aim ray misses the head. Small is a
         * sight line, large is the shoulder camera.
         */
        perp = d * d - t * t;
        perp = (perp > 0.0f) ? sqrtf(perp) : 0.0f;

        if (perp < CAM_ADS_PMAX) {
            w = (CAM_ADS_FAR - d) / (CAM_ADS_FAR - CAM_ADS_NEAR);
            if (w > 1.0f) w = 1.0f;
            if (perp > CAM_ADS_PMIN)
                w *= (CAM_ADS_PMAX - perp)
                   / (CAM_ADS_PMAX - CAM_ADS_PMIN);
            t += g_back;
            px += (ex + fx * t - px) * w;
            py += (ey + fy * t - py) * w;
            pz += (ez + fz * t - pz) * w;
        }
    }
    WritePos(m, px, py, pz);
    if (px == px && py == py && pz == pz &&
        fabsf(px) <= 1e6f && fabsf(py) <= 1e6f && fabsf(pz) <= 1e6f)
        g_headWroteAt = GetTickCount64();
}

/* Each field is written only if its bit is set, so the
 * engine keeps ownership of everything else.
 */
static void ApplyPose(float *m, float fov) {
    if (g_apply & SH_CAM_ROT) WriteRot(m);
    if (g_apply & CAM_HEAD_BIT) {
        if (!ShFp2OwnsEye()) ApplyHead(m, fov);
    }
    else if (g_apply & CAM_ORBIT_BIT) ApplyOrbit(m);
    else if (g_apply & SH_CAM_POS)
        WritePos(m, g_absPos.x, g_absPos.y, g_absPos.z);
}

/* Skew and mode belong to the render camera, so they stay
 * on the camera build. Position, rotation and fov are all
 * taken further up, at their own source. */
static void ApplyFields(uint64_t cam) {
    if (g_apply & SH_CAM_SKEW) {
        *(float *)(uintptr_t)(cam + CAM_SKEWX) = g_skewX;
        *(float *)(uintptr_t)(cam + CAM_SKEWY) = g_skewY;
    }
    if (g_apply & SH_CAM_MODE)
        *(int *)(uintptr_t)(cam + CAM_MODE) = g_modeSet;
}

/* Runs on the engine's own thread, immediately before the
 * transform is consumed, so there is no race to lose.
 */
static void __attribute__((ms_abi)) CamCallback(uint64_t rcx) {
    const float *f;
    int mode, ui;
    Fp2TimingSample timing;
    int capture = ShFp2TimingEnabled();

    if (capture) ShFp2TimingBegin(&timing, FP2_TIMING_CAMERA, rcx, g_calls + 1);

    if (!rcx || !ShReadableAddr(rcx, CAM_FOV + 4)) {
        if (capture) { timing.early = 1; ShFp2TimingFinish(&timing); }
        return;
    }
    mode = *(const int *)(uintptr_t)(rcx + CAM_MODE);
    if (capture) timing.mode = mode;
    if (mode != 0) {
        g_otherMode = mode;
        g_otherAt = g_calls;
        if (capture) {
            timing.early = 2;
            timing.handoff = 2;
            timing.apply = g_apply;
            ShFp2TimingFinish(&timing);
        }
        return;
    }

    g_cam = rcx;
    g_calls++;

    f = (const float *)(uintptr_t)(rcx + CAM_POSE);
    ui = f[0] == 1.0f && f[1] == 0.0f && f[2] == 0.0f &&
         f[4] == 0.0f && f[5] == 1.0f && f[6] == 0.0f &&
         f[8] == 0.0f && f[9] == 0.0f && f[10] == 1.0f;
    if (ui) g_uiAt = g_calls;
    if (capture) {
        timing.cameraSequence = g_calls;
        timing.ui = ui;
        timing.apply = g_apply;
        timing.handoff = ui ? 1 : 0;
    }

    /* The engine stamps visibility back here, so a held
     * override is reapplied in the same window.
     */
    if (capture) timing.visibilityBegin = ShFp2TimingNow();
    ShVisibilityPump();
    if (capture) timing.visibilityEnd = ShFp2TimingNow();
    if (capture) timing.transformBegin = ShFp2TimingNow();
    ShTransformPump();
    if (capture) timing.transformEnd = ShFp2TimingNow();
    if (capture) timing.dominoBegin = ShFp2TimingNow();
    ShDominoPump();
    if (capture) timing.dominoEnd = ShFp2TimingNow();

    /* The menu camera never takes the head, so its frames
     * do no head work at all. The pump keeps its state and
     * resumes on the first world frame. */
    if (capture) timing.headBegin = ShFp2TimingNow();
    if (!ui) {
        uint64_t now = GetTickCount64();
        if (!ShFp2OwnsEye()) {
            if ((g_apply & CAM_HEAD_BIT) || now < g_viewWantAt) ShHeadWant();
            ShHeadPump();
        }
        if (now < g_viewWantAt && !ShFp2OwnsEye()) {
            ShVec3 h;
            if (ShHeadCached(&h)) {
                float x = f[12] - h.x, y = f[13] - h.y, z = f[14] - h.z;
                if (x*x + y*y + z*z < 1.0f) g_headNearAt = now;
            }
        }
        if (capture) timing.headEnd = ShFp2TimingNow();
        if (g_apply) {
            if (capture) timing.fieldsBegin = ShFp2TimingNow();
            ApplyFields(rcx);
            if (capture) timing.fieldsEnd = ShFp2TimingNow();
        }
    }
    if (capture) {
        if (ui) timing.headEnd = ShFp2TimingNow();
        ShFp2TimingFinish(&timing);
    }
}

/* The manager's own transform, before any consumer reads
 * it. Rows match CAM_POSE: 0 right, 1 forward, 2 up, 3 the
 * translation. RAX holds the manager at the patch site. */
static void __attribute__((ms_abi)) MgrCallback(uint64_t cm) {
    float *m, *p;
    float before[3];
    int track;
    Fp2TimingSample timing;
    int capture = ShFp2TimingEnabled();

    if (capture) {
        ShFp2TimingBegin(&timing, FP2_TIMING_MANAGER, cm, g_calls);
        timing.apply = g_apply;
        timing.headBegin = ShFp2TimingNow();
        ShFp2TimingManagerCurrent(&timing);
    }

    ShFp2WorldFrame();
    ShFp2HeadFrame();
    if (!cm || !g_apply) {
        ShFp2PeekPlacementMissed();
        if (capture) {
            timing.early = FP2_TIMING_MGR_NO_CLAIM;
            timing.headEnd = ShFp2TimingNow(); ShFp2TimingFinish(&timing);
        }
        return;
    }
    if (!ShReadableAddr(cm + MGR_XFORM, 0x40)) {
        ShFp2PeekPlacementMissed();
        if (capture) {
            timing.early = FP2_TIMING_MGR_UNREADABLE;
            timing.headEnd = ShFp2TimingNow(); ShFp2TimingFinish(&timing);
        }
        return;
    }
    /* Same identity basis test ShInPauseMenu reports on. */
    if (g_uiAt != 0 && g_calls - g_uiAt <= 4) {
        ShFp2PeekPlacementMissed();
        if (capture) {
            timing.early = FP2_TIMING_MGR_UI_RECENT;
            timing.handoff = 1;
            timing.headEnd = ShFp2TimingNow(); ShFp2TimingFinish(&timing);
        }
        return;
    }

    m = (float *)(uintptr_t)(cm + MGR_XFORM);
    p = (float *)(uintptr_t)(cm + MGR_POS);
    track = g_ffpTrackNextWrite != 0;
    if (track) {
        before[0] = m[12];
        before[1] = m[13];
        before[2] = m[14];
    }

    /* FP2 owns this manager frame outright. A declined menu, drone or
     * optic frame remains the engine's; do not restate its position. */
    if ((g_apply & CAM_HEAD_BIT) && ShFp2CameraReady()) {
        if (ShFp2PlaceEye(m, p)) {
            if (capture) {
                timing.transformWritten = 1;
                timing.positionWritten = 1;
            }
            g_headWroteAt = GetTickCount64();
            g_writes++;
            if (track && InterlockedCompareExchange(&g_ffpTrackNextWrite,
                                                     0, 1) == 1)
                ShFp2TraceManagerWrite(g_apply, before, m + 12);
        } else if (capture) timing.early = FP2_TIMING_MGR_FP2_DECLINED;
        if (capture) {
            timing.headEnd = ShFp2TimingNow();
            ShFp2TimingFinish(&timing);
        }
        return;
    }

    ShFp2PeekPlacementMissed();
    if (capture) timing.poseRan = 1;
    ApplyPose(m, *(const float *)(uintptr_t)(cm + MGR_FOV));
    if (capture) {
        timing.transformWritten =
            (g_apply & (SH_CAM_ROT | SH_CAM_POS | CAM_DERIVED)) != 0;
        timing.headEnd = ShFp2TimingNow();
    }

    /* The engine fills the position vector from a second
     * call, so the row above is restated here rather than
     * left to disagree with it. */
    p[0] = m[12];
    p[1] = m[13];
    p[2] = m[14];
    p[3] = 0.0f;
    if (capture) timing.positionWritten = 1;
    g_writes++;
    if (track && InterlockedCompareExchange(&g_ffpTrackNextWrite, 0, 1) == 1)
        ShFp2TraceManagerWrite(g_apply, before, m + 12);
    if (capture) ShFp2TimingFinish(&timing);
}

/* The site keeps its call opcode, so the stub is entered
 * with the return address already pushed and tail jumps to
 * the original target, which returns past the site. */
/* The site's frame sits 8 off a 16 byte boundary, which
 * faults the callback's movaps spills. Align through rbp
 * rather than assume, then restore the exact rsp. */
static int BuildMgrStub(void) {
    uint8_t *s = (uint8_t *)ShAllocNear(MGR_SITE);
    int64_t rel;
    int o = 0;

    if (!s) return 0;
    memset(s, 0xCC, 0x1000);

    s[o++] = 0x55;
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xE5;
    s[o++] = 0x48; s[o++] = 0x83; s[o++] = 0xE4; s[o++] = 0xF0;
    s[o++] = 0x48; s[o++] = 0x83; s[o++] = 0xEC; s[o++] = 0x20;
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xC1;
    s[o++] = 0x48; s[o++] = 0xB8;
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)MgrCallback;
    o += 8;
    s[o++] = 0xFF; s[o++] = 0xD0;
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xEC;
    s[o++] = 0x5D;

    rel = (int64_t)MGR_NEXT - ((int64_t)(uintptr_t)(s + o) + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    s[o++] = 0xE9;
    *(int32_t *)(s + o) = (int32_t)rel;

    g_mgrStub = s;
    return 1;
}

/* Refuse anything but the call we decoded, so a build we do
 * not know keeps its own camera. */
static int MgrInstall(void) {
    uint8_t *at = (uint8_t *)(uintptr_t)MGR_SITE;
    int64_t rel;
    DWORD old;

    if (!ShReadableAddr(MGR_SITE, MGR_LEN)) return 0;
    if (at[0] != 0xE8) return 0;
    rel = (int64_t)MGR_SITE + MGR_LEN + *(int32_t *)(at + 1);
    g_mgrHooked = g_mgrStub && (uint64_t)rel == (uint64_t)(uintptr_t)g_mgrStub;
    if (g_mgrHooked) return 1;
    if ((uint64_t)rel != MGR_NEXT) return 0;
    if (!g_mgrStub && !BuildMgrStub()) return 0;

    rel = (int64_t)(uintptr_t)g_mgrStub - ((int64_t)MGR_SITE + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    if (!VirtualProtect(at, MGR_LEN, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    if (at[0] != 0xE8 || (uint64_t)((int64_t)MGR_SITE + MGR_LEN +
            *(int32_t *)(at + 1)) != MGR_NEXT) {
        VirtualProtect(at, MGR_LEN, old, &old);
        return 0;
    }
    *(int32_t *)(at + 1) = (int32_t)rel;
    VirtualProtect(at, MGR_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, MGR_LEN);
    g_mgrHooked = 1;
    return 1;
}

static int BuildStub(void) {
    static const uint8_t SAVE[] = {
        0x48,0x81,0xEC,0xC8,0x00,0x00,0x00,
        0x48,0x89,0x4C,0x24,0x20,
        0x48,0x89,0x54,0x24,0x28,
        0x4C,0x89,0x44,0x24,0x30,
        0x4C,0x89,0x4C,0x24,0x38,
        0x4C,0x89,0x54,0x24,0x40,
        0x4C,0x89,0x5C,0x24,0x48,
        0x48,0x89,0x44,0x24,0x50,
        0x0F,0x11,0x44,0x24,0x60,
        0x0F,0x11,0x4C,0x24,0x70,
        0x0F,0x11,0x94,0x24,0x80,0x00,0x00,0x00,
        0x0F,0x11,0x9C,0x24,0x90,0x00,0x00,0x00,
        0x0F,0x11,0xA4,0x24,0xA0,0x00,0x00,0x00,
        0x0F,0x11,0xAC,0x24,0xB0,0x00,0x00,0x00,
        0x48,0x8B,0x4C,0x24,0x20
    };
    static const uint8_t REST[] = {
        0x0F,0x10,0xAC,0x24,0xB0,0x00,0x00,0x00,
        0x0F,0x10,0xA4,0x24,0xA0,0x00,0x00,0x00,
        0x0F,0x10,0x9C,0x24,0x90,0x00,0x00,0x00,
        0x0F,0x10,0x94,0x24,0x80,0x00,0x00,0x00,
        0x0F,0x10,0x4C,0x24,0x70,
        0x0F,0x10,0x44,0x24,0x60,
        0x48,0x8B,0x44,0x24,0x50,
        0x4C,0x8B,0x5C,0x24,0x48,
        0x4C,0x8B,0x54,0x24,0x40,
        0x4C,0x8B,0x4C,0x24,0x38,
        0x4C,0x8B,0x44,0x24,0x30,
        0x48,0x8B,0x54,0x24,0x28,
        0x48,0x8B,0x4C,0x24,0x20,
        0x48,0x81,0xC4,0xC8,0x00,0x00,0x00
    };
    uint8_t *s = (uint8_t *)ShAllocNear(CAM_THUNK);
    int o = 0;

    if (!s) return 0;
    memset(s, 0xCC, 0x1000);

    memcpy(s + o, SAVE, sizeof(SAVE)); o += (int)sizeof(SAVE);
    s[o++] = 0x48; s[o++] = 0xB8;
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)CamCallback; o += 8;
    s[o++] = 0xFF; s[o++] = 0xD0;
    memcpy(s + o, REST, sizeof(REST)); o += (int)sizeof(REST);
    s[o++] = 0x48; s[o++] = 0xB8;
    *(uint64_t *)(s + o) = CAM_IMPL; o += 8;
    s[o++] = 0xFF; s[o++] = 0xE0;

    g_camStub = s;
    return 1;
}

/* A 5 byte jmp in a 16 byte slot, so the hook is a rel32
 * rewrite with nothing displaced or relocated.
 */
static int PatchThunk(void) {
    uint8_t *t = (uint8_t *)(uintptr_t)CAM_THUNK;
    int64_t rel;
    DWORD old;

    if (!g_camStub || !ShReadableAddr(CAM_THUNK, 5) || t[0] != 0xE9 ||
        (uint64_t)((int64_t)CAM_THUNK + 5 + *(int32_t *)(t + 1)) != CAM_IMPL)
        return 0;
    rel = (int64_t)(uintptr_t)g_camStub - ((int64_t)CAM_THUNK + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    if (t[0] != 0xE9 || (uint64_t)((int64_t)CAM_THUNK + 5 +
            *(int32_t *)(t + 1)) != CAM_IMPL) {
        VirtualProtect(t, 5, old, &old);
        return 0;
    }
    memcpy(g_thunkOrig, t, 5);
    *(int32_t *)(t + 1) = (int32_t)rel;
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    return 1;
}

static void RollbackThunk(void) {
    uint8_t *t = (uint8_t *)(uintptr_t)CAM_THUNK;
    DWORD old;
    if (ThunkPointsAtStub() && VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) {
        if (ThunkPointsAtStub()) {
            memcpy(t + 1, g_thunkOrig + 1, 4);
            FlushInstructionCache(GetCurrentProcess(), t, 5);
        }
        VirtualProtect(t, 5, old, &old);
    }
    g_camHooked = ThunkPointsAtStub();
}

SH_API int ShCameraHookInstall(void) {
    uint8_t *t = (uint8_t *)(uintptr_t)CAM_THUNK;
    int ok = 0;

    AcquireSRWLockExclusive(&g_installLock);
    g_camHooked = ThunkPointsAtStub();
    if (!g_camHooked) {
        InterlockedExchange(&g_camInstalled, 0);
        if (!ShReadableAddr(CAM_THUNK, 5) || t[0] != 0xE9 ||
            (uint64_t)((int64_t)CAM_THUNK + 5 + *(int32_t *)(t + 1)) != CAM_IMPL)
            goto done;
        if (!g_camStub && !BuildStub()) goto done;
        if (!PatchThunk()) goto done;
        g_camHooked = 1;
    }
    g_mgrHooked = 0;
    if (!MgrInstall()) {
        InterlockedExchange(&g_camInstalled, 0);
        RollbackThunk();
        goto done;
    }
    InterlockedExchange(&g_camInstalled, 1);
    ok = 1;
done:
    if (!ok) InterlockedExchange(&g_camInstalled, 0);
    ReleaseSRWLockExclusive(&g_installLock);
    ShSetError(ok ? SH_OK : SH_ERR_HOOK_FAILED);
    return ok;
}

/* The patch is code, so it survives a level change. Verify
 * rather than assume, and re-arm if anything restored it.
 */
static int ThunkPointsAtStub(void) {
    const uint8_t *t = (const uint8_t *)(uintptr_t)CAM_THUNK;
    int64_t tgt;

    if (!g_camStub) return 0;
    if (!ShReadableAddr(CAM_THUNK, 5) || t[0] != 0xE9) return 0;
    tgt = (int64_t)CAM_THUNK + 5 + *(const int32_t *)(t + 1);
    return (uint64_t)tgt == (uint64_t)(uintptr_t)g_camStub;
}

/* Called on the transition into Playing, so the camera is
 * available to plugins without anyone asking for it.
 */
void ShCameraOnEnterPlaying(void) {
    g_cam = 0;
    g_headNearAt = 0;
    g_headWroteAt = 0;
    ShCameraHookInstall();
}

SH_API int ShCameraViewMode(void) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShCameraViewMode",
        __builtin_return_address(0), "-");
    uint64_t now = GetTickCount64();
    int result;
    g_viewWantAt = now + 2000;
    if (!ShCameraReady() || ShInPauseMenu()) {
        result = SH_VIEW_UNKNOWN;
        ShFp2TraceExit(diag, "view=%d", result);
        return result;
    }
    if (((g_apply & CAM_HEAD_BIT) && g_headWroteAt &&
         now - g_headWroteAt < 250) ||
        (g_headNearAt && now - g_headNearAt < 250))
        result = SH_VIEW_FIRST_PERSON;
    else if (!g_headWroteAt && !g_headNearAt)
        result = SH_VIEW_UNKNOWN;
    else result = SH_VIEW_THIRD_PERSON;
    ShFp2TraceExit(diag, "view=%d", result);
    return result;
}

SH_API int ShCameraReady(void) {
    return InterlockedCompareExchange(&g_camInstalled, 0, 0) != 0 && g_cam != 0;
}

SH_API uint64_t ShCameraCalls(void) { return g_calls; }
SH_API uint64_t ShCameraWrites(void) { return g_writes; }

/* The last nonzero camera mode seen, and how many mode 0
 * frames ago, allowing special views to be identified.
 */
SH_API int ShCameraOtherMode(uint64_t *framesAgo) {
    if (framesAgo) *framesAgo = g_calls - g_otherAt;
    return g_otherMode;
}

/** True while the pause menu's camera is rendering. */
SH_API int ShInPauseMenu(void) {
    return g_uiAt != 0 && g_calls - g_uiAt <= 4;
}

SH_API int ShGetCamera(ShCamera *out) {
    uint64_t cam = g_cam;
    const float *m;

    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!cam || !ShReadableAddr(cam, CAM_FOV + 4)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    m = (const float *)(uintptr_t)(cam + CAM_POSE);
    memset(out, 0, sizeof(*out));
    out->camera = cam;
    out->right.x = m[0];   out->right.y = m[1];   out->right.z = m[2];
    out->forward.x = m[4]; out->forward.y = m[5]; out->forward.z = m[6];
    out->up.x = m[8];      out->up.y = m[9];      out->up.z = m[10];
    out->pos.x = m[12];    out->pos.y = m[13];    out->pos.z = m[14];
    out->fov = *(const float *)(uintptr_t)(cam + CAM_FOV);
    out->mode = *(const int *)(uintptr_t)(cam + CAM_MODE);
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShSetCamera(const ShVec3 *pos) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShSetCamera",
        __builtin_return_address(0), "pos=%p xyz=%.6g,%.6g,%.6g",
        (const void *)pos, pos ? pos->x : 0.0f,
        pos ? pos->y : 0.0f, pos ? pos->z : 0.0f);
    if (!pos) { ShSetError(SH_ERR_BAD_ARG); ShFp2TraceExit(diag, "int=0"); return 0; }
    if (!ShCameraHookInstall()) { ShFp2TraceExit(diag, "int=0"); return 0; }
    g_absPos = *pos;
    g_right = 0.0f;
    g_apply = (g_apply & ~CAM_DERIVED) | SH_CAM_POS;
    ShSetError(SH_OK);
    ShFp2TraceExit(diag, "int=1");
    return 1;
}

/* Metres behind the player along the camera's forward axis,
 * and height above. It follows the player because the
 * engine still owns the basis and the position. */
SH_API int ShCameraOrbit(float back, float up) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShCameraOrbit",
        __builtin_return_address(0), "back=%.6g up=%.6g", back, up);
    if (!ShCameraHookInstall()) { ShFp2TraceExit(diag, "int=0"); return 0; }
    g_back = back;
    g_right = 0.0f;
    g_up = up;
    g_apply = (g_apply & ~CAM_HEAD_BIT) | SH_CAM_POS | CAM_ORBIT_BIT;
    ShSetError(SH_OK);
    ShFp2TraceExit(diag, "int=1");
    return 1;
}

/* Private plugin compatibility; offsets use the engine's live basis. */
SH_API int ShCameraOrbitAdvanced(float back, float right, float up) {
    if (!ShCameraHookInstall()) return 0;
    g_back = back;
    g_right = right;
    g_up = up;
    g_apply = (g_apply & ~CAM_HEAD_BIT) | SH_CAM_POS | CAM_ORBIT_BIT;
    ShSetError(SH_OK);
    return 1;
}

/* First person: the eye tracks the head bone every frame
 * and eases onto the aim ray during ADS, so sights stay
 * centered. forward clears the face. */
SH_API int ShCameraFirstPerson(float forward, float up) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShCameraFirstPerson",
        __builtin_return_address(0), "forward=%.6g up=%.6g", forward, up);
    if (!ShCameraHookInstall()) { ShFp2TraceExit(diag, "int=0"); return 0; }
    g_back = forward;
    g_right = 0.0f;
    g_up = up;
    g_apply = (g_apply & ~CAM_ORBIT_BIT) | SH_CAM_POS | CAM_HEAD_BIT;
    ShSetError(SH_OK);
    ShFp2TraceExit(diag, "int=1");
    return 1;
}

/* Position and aim both ours. Angles are radians: yaw 0
 * faces +y, pitch is positive looking up.
 */
SH_API int ShCameraFree(const ShVec3 *pos, float yaw, float pitch) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShCameraFree",
        __builtin_return_address(0),
        "pos=%p xyz=%.6g,%.6g,%.6g yaw=%.6g pitch=%.6g",
        (const void *)pos, pos ? pos->x : 0.0f,
        pos ? pos->y : 0.0f, pos ? pos->z : 0.0f, yaw, pitch);
    if (!pos) { ShSetError(SH_ERR_BAD_ARG); ShFp2TraceExit(diag, "int=0"); return 0; }
    if (!ShCameraHookInstall()) { ShFp2TraceExit(diag, "int=0"); return 0; }
    if (pitch > 1.55f) pitch = 1.55f;
    if (pitch < -1.55f) pitch = -1.55f;
    g_absPos = *pos;
    g_yaw = yaw;
    g_pitch = pitch;
    g_right = 0.0f;
    g_apply = (g_apply & ~CAM_DERIVED) | SH_CAM_POS | SH_CAM_ROT;
    ShSetError(SH_OK);
    ShFp2TraceExit(diag, "int=1");
    return 1;
}

/* Where the engine's camera is aiming right now, so a free
 * camera can start from the current view.
 */
SH_API int ShCameraAngles(float *yaw, float *pitch) {
    ShCamera c;

    if (!ShGetCamera(&c)) return 0;
    if (yaw) *yaw = atan2f(c.forward.x, c.forward.y);
    if (pitch) {
        float s = c.forward.z;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        *pitch = asinf(s);
    }
    return 1;
}

SH_API int ShCameraApply(const ShCameraOverride *o) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShCameraApply",
        __builtin_return_address(0),
        "override=%p mask=0x%X pos=%.6g,%.6g,%.6g yaw=%.6g "
        "pitch=%.6g roll=%.6g fov=%.6g mode=%d",
        (const void *)o, o ? o->apply : 0u,
        o ? o->pos.x : 0.0f, o ? o->pos.y : 0.0f,
        o ? o->pos.z : 0.0f, o ? o->yaw : 0.0f,
        o ? o->pitch : 0.0f, o ? o->roll : 0.0f,
        o ? o->fov : 0.0f, o ? o->mode : 0);
    if (!o || !o->apply) { ShSetError(SH_ERR_BAD_ARG); ShFp2TraceExit(diag, "int=0"); return 0; }
    if (!ShCameraHookInstall()) { ShFp2TraceExit(diag, "int=0"); return 0; }

    if (o->apply & SH_CAM_POS) g_absPos = o->pos;
    if (o->apply & SH_CAM_ROT) {
        float p = o->pitch;
        if (p > 1.55f) p = 1.55f;
        if (p < -1.55f) p = -1.55f;
        g_yaw = o->yaw;
        g_pitch = p;
        g_roll = o->roll;
    }
    if (o->apply & SH_CAM_FOV) {
        g_fov = o->fov;
        if (!ShFovSet(o->fov)) {
            ShSetError(SH_ERR_NO_CANDIDATE);
            ShFp2TraceExit(diag, "int=0 fov_set=failed");
            return 0;
        }
    }
    if (o->apply & SH_CAM_SKEW) {
        g_skewX = o->skewX;
        g_skewY = o->skewY;
    }
    if (o->apply & SH_CAM_MODE) g_modeSet = o->mode;

    /* Merged, so applying fov leaves another plugin's
     * position and rotation alone.
     */
    if (o->apply & SH_CAM_POS) {
        g_right = 0.0f;
        g_apply &= ~CAM_DERIVED;
    }
    g_apply |= o->apply;
    ShSetError(SH_OK);
    ShFp2TraceExit(diag, "int=1");
    return 1;
}

SH_API int ShCameraMatrix(int index, float *out16) {
    uint64_t cam = g_cam;
    uint64_t at;

    if (!out16 || index < 0 || index >= CAM_MAT_N) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (!cam) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    at = cam + CAM_MATS + (uint64_t)index * 0x40;
    if (!ShReadableAddr(at, 0x40)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    memcpy(out16, (const void *)(uintptr_t)at, 0x40);
    ShSetError(SH_OK);
    return 1;
}

SH_API void ShCameraRelease(void) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShCameraRelease",
        __builtin_return_address(0), "-");
    g_apply = 0;
    g_right = 0.0f;
    ShFovClear();
    ShFp2TraceExit(diag, "void");
}

/* Give back only what you took, so releasing a free camera
 * leaves another plugin's fov override running.
 */
SH_API void ShCameraReleaseFields(uint32_t fields) {
    Fp2TraceToken diag = ShFp2TraceEnter("ShCameraReleaseFields",
        __builtin_return_address(0), "fields=0x%X", fields);
    if ((fields & CAM_HEAD_BIT) && (g_apply & CAM_HEAD_BIT))
        fields |= SH_CAM_POS;
    if (fields & SH_CAM_POS) fields |= CAM_DERIVED;
    if (fields & SH_CAM_FOV) ShFovClear();
    g_apply &= ~fields;
    if (fields & (SH_CAM_POS | CAM_ORBIT_BIT)) g_right = 0.0f;
    if (diag.sequence && (g_apply & SH_CAM_POS) &&
        !(g_apply & CAM_DERIVED))
        InterlockedExchange(&g_ffpTrackNextWrite, 1);
    ShFp2TraceExit(diag, "void effective_fields=0x%X", fields);
}

/** Which fields are currently overridden. */
SH_API uint32_t ShCameraOwned(void) {
    return g_apply & 0xFFu;
}
