// vendor/librw/src/gx/gxdevice.cpp
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// Wii GX Device 鈥?Complete Implementation
// 淇 "Failed to load TXD": DEVICEOPEN 瀹屾暣娉ㄥ唽鎵€鏈?Driver 鍥炶皟
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
#ifdef GAMECUBE

#include <gccore.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "rwgx.h"
#include "gxmemory.h"

// Define GX_PIPELINE_DIAGNOSTICS when targeted device tracing is required.
#ifndef GX_PIPELINE_DIAGNOSTICS
#define printf(...) ((void)sizeof((::printf)(__VA_ARGS__)))
#endif

// 鈽?Exposed for diagnostic heartbeat in gamecube.cpp (global C linkage)
extern "C" {
    void *s_gx_xfb[2] = { nullptr, nullptr };
}

namespace rw {
namespace gx {

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// 鍓嶅悜澹版槑 (gxraster.cpp 涓畾涔?
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
extern void    registerNativeRaster(void);
extern Raster* rasterCreate        (Raster *raster);
extern void    rasterDestroy       (Raster *raster);
extern uint8*  rasterLock          (Raster *raster, int32 level, int32 lockMode);
extern void    rasterUnlock        (Raster *raster, int32 level);
extern int32   rasterNumLevels     (Raster *raster);
extern bool32  rasterFromImage     (Raster *raster, Image *image);
extern Image*  rasterToImage       (Raster *raster);

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// 鍏ㄥ眬鍙橀噺
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
Mtx    gxInvCamLTM;
Mtx44  gxProjMtx;
uint8  gxProjType = GX_PERSPECTIVE;
uint32 gxFrameNum = 0;   // 鈻?璇婃柇鐢ㄥ叏灞€甯ц鏁板櫒 (rwgx.h 涓?extern 澹版槑)

// s_gx_xfb defined at global scope (extern "C") above 鈥?pull into namespace
using ::s_gx_xfb;
static int32   s_xfbIdx   = 0;
static const GXRModeObj *s_videoMode = nullptr;
static u16     s_fbWidth  = 640;
static u16     s_efbHeight = 480;
static u16     s_xfbHeight = 480;
static float32 s_fogStart = 100.0f;
static float32 s_fogEnd   = 1000.0f;
static GXColor s_fogColor = { 0, 0, 0, 0 };
static GXColor s_clearColor = { 0, 0, 0, 255 };

enum {
    GX_INPUT_GAME = 1,
    GX_INPUT_FREECAM = 2
};

static int     s_inputMode = GX_INPUT_GAME;
static bool    s_freeCamEnabled = false;
static bool    s_freeCamCpuScene = false;
static bool    s_freeCamValid = false;
static bool    s_freeCamViewActive = false;
static bool    s_fullbrightDebug = true;
static V3d     s_freeCamPos = { 0.0f, 0.0f, 0.0f };
static float32 s_freeCamYaw = 0.0f;
static float32 s_freeCamPitch = 0.0f;
static u16     s_freeCamLastButtons = 0;

static float32
gxClampF32(float32 v, float32 lo, float32 hi)
{
    if(v < lo)
        return lo;
    if(v > hi)
        return hi;
    return v;
}

static float32
gxStickUnit(s8 v)
{
    const int32 iv = (int32)v;
    if(iv > -10 && iv < 10)
        return 0.0f;
    return gxClampF32((float32)iv / 80.0f, -1.0f, 1.0f);
}

static void
snapFreeCamToRwCamera(Camera *cam)
{
    const Matrix *ltm = (const Matrix*)cam->getFrame()->getLTM();
    s_freeCamPos = ltm->pos;

    const float32 ax = ltm->at.x;
    const float32 ay = ltm->at.y;
    const float32 az = gxClampF32(ltm->at.z, -1.0f, 1.0f);
    s_freeCamYaw = atan2f(ax, ay);
    s_freeCamPitch = asinf(az);
    s_freeCamValid = true;
}

static void
buildFreeCamMatrix(Mtx dst)
{
    Matrix mtx;
    const float32 sy = sinf(s_freeCamYaw);
    const float32 cy = cosf(s_freeCamYaw);
    const float32 sp = sinf(s_freeCamPitch);
    const float32 cp = cosf(s_freeCamPitch);

    mtx.right.x = -cy;
    mtx.right.y = sy;
    mtx.right.z = 0.0f;
    mtx.flags = Matrix::TYPEORTHONORMAL;

    mtx.up.x = -sy * sp;
    mtx.up.y = -cy * sp;
    mtx.up.z = cp;
    mtx.pad1 = 0;

    mtx.at.x = sy * cp;
    mtx.at.y = cy * cp;
    mtx.at.z = sp;
    mtx.pad2 = 0;

    mtx.pos = s_freeCamPos;
    mtx.pad3 = 0;

    rwMatToGxMtx(dst, &mtx);
}

static void
buildFreeCamRwMatrix(Matrix *mtx)
{
    const float32 sy = sinf(s_freeCamYaw);
    const float32 cy = cosf(s_freeCamYaw);
    const float32 sp = sinf(s_freeCamPitch);
    const float32 cp = cosf(s_freeCamPitch);

    mtx->right.x = -cy;
    mtx->right.y = sy;
    mtx->right.z = 0.0f;
    mtx->flags = Matrix::TYPEORTHONORMAL;

    mtx->up.x = -sy * sp;
    mtx->up.y = -cy * sp;
    mtx->up.z = cp;
    mtx->pad1 = 0;

    mtx->at.x = sy * cp;
    mtx->at.y = cy * cp;
    mtx->at.z = sp;
    mtx->pad2 = 0;

    mtx->pos = s_freeCamPos;
    mtx->pad3 = 0;
}

static bool
updateFreeCam(Camera *cam, Mtx dst)
{
    const u16 buttons = PAD_ButtonsHeld(0);
    const bool debugChord = (buttons & PAD_BUTTON_START) &&
                            (buttons & PAD_TRIGGER_Z);
    const bool zHeld = (buttons & PAD_TRIGGER_Z) != 0;
    const bool togglePressed = zHeld &&
        debugChord &&
        (buttons & PAD_BUTTON_X) &&
        !(s_freeCamLastButtons & PAD_BUTTON_X);
    const bool snapPressed = zHeld &&
        debugChord &&
        (buttons & PAD_BUTTON_Y) &&
        !(s_freeCamLastButtons & PAD_BUTTON_Y);
    const bool cpuScenePressed = zHeld &&
        debugChord &&
        (buttons & PAD_BUTTON_A) &&
        !(s_freeCamLastButtons & PAD_BUTTON_A);
    const bool fullbrightPressed = zHeld &&
        debugChord &&
        (buttons & PAD_BUTTON_B) &&
        !(s_freeCamLastButtons & PAD_BUTTON_B);

    if(togglePressed) {
        if(!s_freeCamEnabled && !s_freeCamValid)
            snapFreeCamToRwCamera(cam);
        s_freeCamEnabled = !s_freeCamEnabled;
        if(!s_freeCamEnabled)
            s_freeCamCpuScene = false;
        s_inputMode = s_freeCamEnabled ? GX_INPUT_FREECAM : GX_INPUT_GAME;
        printf("[GX-FREECAM] %s pos=(%.3f,%.3f,%.3f) yaw=%.3f pitch=%.3f\n",
               s_freeCamEnabled ? "enabled" : "disabled",
               s_freeCamPos.x, s_freeCamPos.y, s_freeCamPos.z,
               s_freeCamYaw, s_freeCamPitch);
    }

    if(snapPressed) {
        snapFreeCamToRwCamera(cam);
        printf("[GX-FREECAM] snapped pos=(%.3f,%.3f,%.3f) yaw=%.3f pitch=%.3f\n",
               s_freeCamPos.x, s_freeCamPos.y, s_freeCamPos.z,
               s_freeCamYaw, s_freeCamPitch);
    }

    if(cpuScenePressed && s_freeCamEnabled) {
        s_freeCamCpuScene = !s_freeCamCpuScene;
        printf("[GX-FREECAM] cpu-scene+xray %s\n",
               s_freeCamCpuScene ? "enabled" : "disabled");
    }

    if(fullbrightPressed) {
        s_fullbrightDebug = !s_fullbrightDebug;
        printf("[GX-FULLBRIGHT] %s\n",
               s_fullbrightDebug ? "enabled" : "disabled");
    }

    s_freeCamLastButtons = buttons;

    if(!s_freeCamEnabled) {
        s_inputMode = GX_INPUT_GAME;
        s_freeCamCpuScene = false;
        return false;
    }

    if(!s_freeCamValid)
        snapFreeCamToRwCamera(cam);

    const float32 moveSpeed = (buttons & PAD_BUTTON_B) ? 0.62f : 0.18f;
    const float32 lookSpeed = 0.032f;

    const float32 strafe = -gxStickUnit(PAD_StickX(0));
    const float32 forward = gxStickUnit(PAD_StickY(0));
    const float32 lookX = gxStickUnit(PAD_SubStickX(0));
    const float32 lookY = gxStickUnit(PAD_SubStickY(0));
    const float32 lift = ((float32)PAD_TriggerR(0) -
                          (float32)PAD_TriggerL(0)) / 255.0f;

    s_freeCamYaw += lookX * lookSpeed;
    s_freeCamPitch = gxClampF32(s_freeCamPitch + lookY * lookSpeed,
                                -1.45f, 1.45f);

    const float32 sy = sinf(s_freeCamYaw);
    const float32 cy = cosf(s_freeCamYaw);
    const float32 sp = sinf(s_freeCamPitch);
    const float32 cp = cosf(s_freeCamPitch);

    const V3d right = { -cy, sy, 0.0f };
    const V3d at = { sy * cp, cy * cp, sp };

    s_freeCamPos.x += (right.x * strafe + at.x * forward) * moveSpeed;
    s_freeCamPos.y += (right.y * strafe + at.y * forward) * moveSpeed;
    s_freeCamPos.z += (right.z * strafe + at.z * forward + lift) * moveSpeed;

    buildFreeCamMatrix(dst);
    return true;
}

static void
load2DProjection(void)
{
    Mtx44 proj2d;
    guOrtho(proj2d, 0.0f, (float32)s_efbHeight, 0.0f, (float32)s_fbWidth, -1.0f, 1.0f);
    GX_LoadProjectionMtx(proj2d, GX_ORTHOGRAPHIC);

    Mtx view2d;
    guMtxIdentity(view2d);
    GX_LoadPosMtxImm(view2d, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
}

static float32
getCameraAspect(Camera *cam, float32 fallback)
{
    if(cam && cam->frameBuffer &&
       cam->frameBuffer->width > 0 &&
       cam->frameBuffer->height > 0)
        return (float32)cam->frameBuffer->width /
               (float32)cam->frameBuffer->height;

    if(s_fbWidth > 0 && s_efbHeight > 0)
        return (float32)s_fbWidth / (float32)s_efbHeight;

    return fallback;
}

void
gxGetFramebufferSize(uint16_t *fbWidth, uint16_t *efbHeight)
{
    if(fbWidth)
        *fbWidth = s_fbWidth;
    if(efbHeight)
        *efbHeight = s_efbHeight;
}

int
gxGetInputMode(void)
{
    return s_inputMode;
}

bool
gxFreeCamDebugActive(void)
{
    return s_freeCamViewActive;
}

bool
gxFreeCamCpuSceneActive(void)
{
    return s_freeCamViewActive && s_freeCamCpuScene;
}

bool
gxFullbrightDebugActive(void)
{
    return s_fullbrightDebug;
}

bool
gxGetFreeCamPosition(float *x, float *y, float *z)
{
    if(!s_freeCamViewActive)
        return false;
    if(x) *x = s_freeCamPos.x;
    if(y) *y = s_freeCamPos.y;
    if(z) *z = s_freeCamPos.z;
    return true;
}

bool
gxGetFreeCamFrame(float *out12)
{
    if(!s_freeCamViewActive || !out12)
        return false;

    Matrix mtx;
    buildFreeCamRwMatrix(&mtx);
    out12[0] = mtx.right.x;
    out12[1] = mtx.right.y;
    out12[2] = mtx.right.z;
    out12[3] = mtx.up.x;
    out12[4] = mtx.up.y;
    out12[5] = mtx.up.z;
    out12[6] = mtx.at.x;
    out12[7] = mtx.at.y;
    out12[8] = mtx.at.z;
    out12[9] = mtx.pos.x;
    out12[10] = mtx.pos.y;
    out12[11] = mtx.pos.z;
    return true;
}

static void
loadCameraProjection(Camera *cam)
{
    if(!cam->getFrame())
        return;

    Mtx camMtx;
    rwMatToGxMtx(camMtx, cam->getFrame()->getLTM());
    guMtxInverse(camMtx, gxInvCamLTM);
    gxInvCamLTM[2][0] = -gxInvCamLTM[2][0];
    gxInvCamLTM[2][1] = -gxInvCamLTM[2][1];
    gxInvCamLTM[2][2] = -gxInvCamLTM[2][2];
    gxInvCamLTM[2][3] = -gxInvCamLTM[2][3];
    GX_LoadPosMtxImm(gxInvCamLTM, GX_PNMTX0);

    Mtx44   proj;
    float32 vw = cam->viewWindow.x;
    float32 vh = cam->viewWindow.y;
    float32 np = cam->nearPlane;
    float32 fp = cam->farPlane;

    if(cam->projection == Camera::PERSPECTIVE) {
        float32 fovY   = s_freeCamViewActive ? 60.0f :
                         2.0f * atanf(vh) * (180.0f / 3.14159265f);
        // RenderWare viewWindow keeps the gameplay camera shape, but the Wii
        // output aspect must come from the active GX framebuffer/video mode.
        float32 aspect = getCameraAspect(cam, (s_efbHeight > 0) ?
                                              ((float32)s_fbWidth / (float32)s_efbHeight) :
                                              1.333f);
        guPerspective(proj, fovY, aspect, np, fp);
        // Keep the handedness correction in projection so the world is not
        // mirrored while depth stays stable.
        proj[0][0] = -proj[0][0];
        proj[0][1] = -proj[0][1];
        proj[0][2] = -proj[0][2];
        proj[0][3] = -proj[0][3];
        GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
        gxProjType = GX_PERSPECTIVE;
        static int s_camAspectLogs = 0;
        if(s_camAspectLogs < 24) {
            float32 winAspect = (vh > 0.0f) ? (vw / vh) : 0.0f;
            float32 fovX = 2.0f * atanf(tanf(fovY * 0.5f * (3.14159265f / 180.0f)) * aspect) *
                           (180.0f / 3.14159265f);
            printf("[GX-CAM] proj=persp fb=%ux%u mode=%ux%u vw=%.4f vh=%.4f winAsp=%.4f useAsp=%.4f fovX=%.2f fovY=%.2f\n",
                   cam && cam->frameBuffer ? cam->frameBuffer->width : 0,
                   cam && cam->frameBuffer ? cam->frameBuffer->height : 0,
                   (unsigned)s_fbWidth, (unsigned)s_efbHeight,
                   vw, vh, winAspect, aspect, fovX, fovY);
            s_camAspectLogs++;
        }
    } else {
        guOrtho(proj, vh, -vh, -vw, vw, np, fp);
        proj[0][0] = -proj[0][0];
        proj[0][1] = -proj[0][1];
        proj[0][2] = -proj[0][2];
        proj[0][3] = -proj[0][3];
        GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
        gxProjType = GX_ORTHOGRAPHIC;
    }
    memcpy(gxProjMtx, proj, sizeof(Mtx44));
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// 鍏紑 API
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲

void gxSetVideoMode(const GXRModeObj *mode)
{
    if(!mode)
        return;

    s_videoMode = mode;
    s_fbWidth = mode->fbWidth;
    s_efbHeight = mode->efbHeight;
    s_xfbHeight = mode->xfbHeight;
    printf("[GX] gxSetVideoMode: fb=%u efb=%u xfb=%u\n",
           s_fbWidth, s_efbHeight, s_xfbHeight);
}

// 鐢卞钩鍙颁唬鐮佸湪 VIDEO_Init 涔嬪悗璋冪敤, 娉ㄥ叆鍙岀紦鍐插抚缂撳啿鍦板潃
void gxSetXFBs(void *xfb0, void *xfb1)
{
    s_gx_xfb[0] = xfb0;
    s_gx_xfb[1] = xfb1;
    s_xfbIdx = 0;
    printf("[GX] gxSetXFBs: xfb0=%p  xfb1=%p\n", xfb0, xfb1);
}

// RW 鍙虫墜鍧愭爣绯荤煩闃?鈫?GX 3脳4 琛屼富搴?(X 杞村彇鍙嶄慨姝ｉ暅鍍?
void rwMatToGxMtx(Mtx dst, const void *rwMat)
{
    const Matrix *src = (const Matrix*)rwMat;
    // RW (+Z forward, left-handed) 鈫?GX (-Z forward, right-handed)
    // Handedness fix is in the projection matrix (Z-column flip), NOT here.
    // Never negate X 鈥?it breaks winding order (culling).
    dst[0][0] =  src->right.x;  dst[0][1] =  src->up.x;
    dst[0][2] =  src->at.x;     dst[0][3] =  src->pos.x;
    dst[1][0] =  src->right.y;  dst[1][1] =  src->up.y;
    dst[1][2] =  src->at.y;     dst[1][3] =  src->pos.y;
    dst[2][0] =  src->right.z;  dst[2][1] =  src->up.z;
    dst[2][2] =  src->at.z;     dst[2][3] =  src->pos.z;
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// Camera Callbacks
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲

static void
beginUpdate(Camera *cam)
{
    gxFrameNum++;   // 鈻?璇婃柇: 甯ц竟鐣屾爣璁?
    static bool s_freeCamBuildLogged = false;
    if(!s_freeCamBuildLogged) {
    printf("[GX-BUILD] gxdevice-global-cull-v4\n");
        s_freeCamBuildLogged = true;
    }
    GX_SetCullMode(GX_CULL_NONE);  // ensure no culling for full quad and tests
    // 鈹€鈹€ DIAG: 姣忓抚鎵撳嵃涓€娆″抚棣栨棩蹇?(disabled) 鈹€鈹€
#if 0
    if (gxFrameNum < 5 || (gxFrameNum % 60) == 0) {
        printf("[FRAME] === beginUpdate frame=%u clearRGBA=(%d,%d,%d,%d) ===\n",
               gxFrameNum,
               s_clearColor.r, s_clearColor.g, s_clearColor.b, s_clearColor.a);
    }
#endif

    {
        Mtx44 proj2d;
        const float32 fbWidth = (float32)s_fbWidth;
        const float32 efbHeight = (float32)s_efbHeight;
        guOrtho(proj2d, 0.0f, efbHeight, 0.0f, fbWidth, -1.0f, 1.0f);
        GX_LoadProjectionMtx(proj2d, GX_ORTHOGRAPHIC);

        Mtx view2d;
        guMtxIdentity(view2d);
        GX_LoadPosMtxImm(view2d, GX_PNMTX0);
        GX_SetCurrentMtx(GX_PNMTX0);

        // 棰滆壊鐩撮€?(PASSCLR), 鏃犵汗鐞?        GX_SetNumChans(1);
        GX_SetNumTexGens(0);
        GX_SetNumTevStages(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL,
                       GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

        // 鍚敤棰滆壊閫氶亾 鈥?鍚﹀垯 GX_PASSCLR 杈撳嚭鍏ㄩ浂
        GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE,
                       GX_SRC_REG, GX_SRC_VTX,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

        // 2D 椤剁偣鏍煎紡: POS + CLR0
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,
                         GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0,
                         GX_CLR_RGBA, GX_RGBA8, 0);

        // DIAG RED QUAD REMOVED 鈥?was coloring screen pink for debug
    }
    if(!cam->getFrame()) return;

    // 鈹€鈹€ 瑙嗗浘鐭╅樀 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    Mtx camMtx;
    s_freeCamViewActive = updateFreeCam(cam, camMtx);
    if(!s_freeCamViewActive)
        rwMatToGxMtx(camMtx, cam->getFrame()->getLTM());
    guMtxInverse(camMtx, gxInvCamLTM);

    // Handedness Z flip stays in view matrix.
    // Mirror fix (X flip) goes in projection to avoid depth breakage.
    gxInvCamLTM[2][0] = -gxInvCamLTM[2][0];
    gxInvCamLTM[2][1] = -gxInvCamLTM[2][1];
    gxInvCamLTM[2][2] = -gxInvCamLTM[2][2];
    gxInvCamLTM[2][3] = -gxInvCamLTM[2][3];

    GX_LoadPosMtxImm(gxInvCamLTM, GX_PNMTX0);

    // 鈹€鈹€ 鎶曞奖鐭╅樀 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    Mtx44   proj;
    float32 vw = cam->viewWindow.x;
    float32 vh = cam->viewWindow.y;
    float32 np = cam->nearPlane;
    float32 fp = cam->farPlane;

    if(cam->projection == Camera::PERSPECTIVE) {
        float32 fovY   = s_freeCamViewActive ? 60.0f :
                         2.0f * atanf(vh) * (180.0f / 3.14159265f);
        float32 aspect = getCameraAspect(cam, (s_efbHeight > 0) ?
                                              ((float32)s_fbWidth / (float32)s_efbHeight) :
                                              1.333f);
        guPerspective(proj, fovY, aspect, np, fp);
        // Flip X row in projection to fix scene mirroring
        proj[0][0] = -proj[0][0]; proj[0][1] = -proj[0][1];
        proj[0][2] = -proj[0][2]; proj[0][3] = -proj[0][3];
        GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
        gxProjType = GX_PERSPECTIVE;
        static int s_camAspectLogs = 0;
        if(s_camAspectLogs < 24){
            float32 winAspect = (vh > 0.0f) ? (vw / vh) : 0.0f;
            float32 fovX = 2.0f * atanf(tanf(fovY * 0.5f * (3.14159265f / 180.0f)) * aspect) *
                           (180.0f / 3.14159265f);
            printf("[GX-CAM] proj=persp fb=%ux%u mode=%ux%u vw=%.4f vh=%.4f winAsp=%.4f useAsp=%.4f fovX=%.2f fovY=%.2f\n",
                   cam && cam->frameBuffer ? cam->frameBuffer->width : 0,
                   cam && cam->frameBuffer ? cam->frameBuffer->height : 0,
                   (unsigned)s_fbWidth, (unsigned)s_efbHeight,
                   vw, vh, winAspect, aspect, fovX, fovY);
            s_camAspectLogs++;
        }
    } else {
        guOrtho(proj, vh, -vh, -vw, vw, np, fp);
        // Flip X row in projection to fix scene mirroring
        proj[0][0] = -proj[0][0]; proj[0][1] = -proj[0][1];
        proj[0][2] = -proj[0][2]; proj[0][3] = -proj[0][3];
        GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
        gxProjType = GX_ORTHOGRAPHIC;
    }
    memcpy(gxProjMtx, proj, sizeof(Mtx44));

    if(s_fullbrightDebug || s_freeCamViewActive) {
        GXColor noFog = { 0, 0, 0, 0 };
        GX_SetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, noFog);
        gxState.fog = false;
    }

    if(s_freeCamViewActive) {
        s_clearColor.r = 120;
        s_clearColor.g = 132;
        s_clearColor.b = 148;
        s_clearColor.a = 255;
        GX_SetCopyClear(s_clearColor, GX_MAX_Z24);
    }

    // 姣忓抚娓呯┖绾圭悊缁戝畾缂撳瓨, 闃叉璺ㄥ抚娈嬬暀
    memset(gxState.textures, 0, sizeof(gxState.textures));
    memset(gxState.texMinFilter, 0, sizeof(gxState.texMinFilter));
    memset(gxState.texMagFilter, 0, sizeof(gxState.texMagFilter));
    memset(gxState.texWrapS, 0, sizeof(gxState.texWrapS));
    memset(gxState.texWrapT, 0, sizeof(gxState.texWrapT));
}

static void
endUpdate(Camera * /*cam*/)
{
    void *fb = s_gx_xfb[s_xfbIdx];
    // Rate-limited: only log first 5 frames then every 60 (disabled)
#if 0
    {
        static int ec = 0;
        if (ec < 5 || ec % 60 == 0)
            printf("[GX] endUpdate #%d fb[%d]=%p clearRGBA=(%d,%d,%d,%d)\n",
                   ec, s_xfbIdx, fb,
                   s_clearColor.r, s_clearColor.g, s_clearColor.b, s_clearColor.a);
        ec++;
    }
#endif

    if(fb) {
        // Canonical libogc GX present order: queue the EFB->XFB copy, then
        // GX_DrawDone() to block until that copy has *actually finished*,
        // and only then register the framebuffer + VIDEO_Flush so the very
        // next retrace latches a fully-copied buffer. The outer Wii loop owns
        // the only per-frame VSync wait.
        //
        // NOTE: GX_DrawDone() must come AFTER GX_CopyDisp(). With it before
        // the copy it only waited for rendering, leaving the copy in flight
        // when VIDEO_Flush registered the buffer -> the copy intermittently
        // missed the upcoming retrace and the frame was shown one refresh
        // late (steady 16.68ms loops with sporadic 33.37ms = dropped frames,
        // even with CPU well under budget).
        GX_SetCopyClear(s_clearColor, GX_MAX_Z24);
        GX_CopyDisp(fb, GX_TRUE);
        GX_DrawDone();
        VIDEO_SetNextFramebuffer(fb);
        VIDEO_Flush();
        s_xfbIdx ^= 1;
#ifdef WII
        gxMemRunPendingCompactionAtGpuIdle("post-present");
#endif
    } else {
        // Both XFBs are NULL 鈥?can't output anything!
        static int nullWarn = 0;
        if (nullWarn < 3) {
            printf("[GX] ERROR: endUpdate with NULL XFB! s_gx_xfb[0]=%p s_gx_xfb[1]=%p\n",
                   s_gx_xfb[0], s_gx_xfb[1]);
            nullWarn++;
        }
    }
}

static u8
gxCullModeFromRwState(void)
{
    // Handedness is already compensated in the GX camera matrices/projection.
    // RenderWare culling now maps directly to GX culling.
    switch(gxState.cullMode) {
    case CULLNONE:  return GX_CULL_NONE;
    case CULLFRONT: return GX_CULL_FRONT;
    default:        return GX_CULL_BACK;
    }
}

static void
clearCameraZ(void)
{
    Mtx44 proj;
    Mtx view;

    guOrtho(proj, 0.0f, (f32)s_efbHeight, 0.0f, (f32)s_fbWidth, 0.0f, 1.0f);
    guMtxIdentity(view);

    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    GX_LoadPosMtxImm(view, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    GX_SetColorUpdate(GX_FALSE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE,
                   GX_SRC_REG, GX_SRC_VTX,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL,
                   GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    // guOrtho near=0/far=1 maps z=-1 to the far plane, matching GX_MAX_Z24.
    GX_Begin(GX_QUADS, GX_VTXFMT7, 4);
    GX_Position3f32(0.0f,          0.0f,           -1.0f);
    GX_Color4u8(0, 0, 0, 255);
    GX_Position3f32((f32)s_fbWidth, 0.0f,           -1.0f);
    GX_Color4u8(0, 0, 0, 255);
    GX_Position3f32((f32)s_fbWidth, (f32)s_efbHeight, -1.0f);
    GX_Color4u8(0, 0, 0, 255);
    GX_Position3f32(0.0f,          (f32)s_efbHeight, -1.0f);
    GX_Color4u8(0, 0, 0, 255);
    GX_End();

    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetZMode(gxState.zTest ? GX_TRUE : GX_FALSE,
                GX_LEQUAL,
                gxState.zWrite ? GX_TRUE : GX_FALSE);
    GX_SetCullMode(gxCullModeFromRwState());
}

static void
clearCamera(Camera * /*cam*/, RGBA *col, uint32 mode)
{
    if ((mode & Camera::CLEARIMAGE) && col) {
        s_clearColor.r = col->red;
        s_clearColor.g = col->green;
        s_clearColor.b = col->blue;
        s_clearColor.a = col->alpha;
    }
    GX_SetCopyClear(s_clearColor, GX_MAX_Z24);

    if(mode & Camera::CLEARZ)
        clearCameraZ();
}

// showRaster: 宸插湪 endUpdate 瀹屾垚缈婚〉, 姝ゅ涓虹┖妗?// 鑻?librw/Camera 鏈?showRasterCB 鎴愬憳鍒欏彲鍦ㄦ瀹炵幇
static void
showRaster(Raster * /*raster*/, uint32 flags)
{
    if(flags & 1)
        VIDEO_WaitVSync();
    // 甯у凡鍦?endUpdate 涓€氳繃 GX_CopyDisp 鏄剧ず
}

static bool32
rasterRenderFast(Raster * /*raster*/, int32 /*x*/, int32 /*y*/)
{
    return 0; // unsupported
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// Blend Factor 鏄犲皠琛?// 鈽?librw BlendFunction 鏋氫妇浠?1 寮€濮?(BLENDZERO = 1)锛?//   鎵€浠ョ储寮?0 鏄崰浣?fallback锛岀储寮?N 瀵瑰簲 librw 鏋氫妇鍊?N銆?// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
static const u8 blendMap[] = {
    GX_BL_ZERO,         // 0: 鏃犳晥鍗犱綅 (librw 涓嶄細浼?0)
    GX_BL_ZERO,         // 1: BLENDZERO
    GX_BL_ONE,          // 2: BLENDONE
    GX_BL_SRCCLR,       // 3: BLENDSRCCOLOR
    GX_BL_INVSRCCLR,    // 4: BLENDINVSRCCOLOR
    GX_BL_SRCALPHA,     // 5: BLENDSRCALPHA
    GX_BL_INVSRCALPHA,  // 6: BLENDINVSRCALPHA
    GX_BL_DSTALPHA,     // 7: BLENDDESTALPHA
    GX_BL_INVDSTALPHA,  // 8: BLENDINVDESTALPHA
    GX_BL_DSTCLR,       // 9: BLENDDESTCOLOR
    GX_BL_INVDSTCLR,    // 10: BLENDINVDESTCOLOR
    GX_BL_ONE,          // 11: BLENDSRCALPHASAT (GX 鏃犳妯″紡 鈫?ONE)
};
static const int blendMapSize = (int)(sizeof(blendMap) / sizeof(blendMap[0]));

static bool32
rasterRenderFastDiag(Raster *raster, int32 x, int32 y)
{
    Raster *src = raster;
    Raster *dst = Raster::getCurrentContext();

    static int s_copyLogs = 0;
    if(s_copyLogs < 24) {
        printf("[GX-COPYMISS] f%u src=%p(%d,%dx%d) dst=%p(%d,%dx%d) xy=(%d,%d)\n",
               gxFrameNum,
               src, src ? src->type : -1, src ? src->width : 0, src ? src->height : 0,
               dst, dst ? dst->type : -1, dst ? dst->width : 0, dst ? dst->height : 0,
               x, y);
        s_copyLogs++;
    }
    return rasterRenderFast(raster, x, y);
}

// librw alphaTestFunc 鏋氫妇 鈫?GX 姣旇緝鍑芥暟
static u8
alphaFuncToGX(int32 f)
{
    switch(f) {
    case ALPHAALWAYS:       return GX_ALWAYS;
    case ALPHAGREATEREQUAL: return GX_GEQUAL;
    case ALPHALESS:         return GX_LESS;
    default: return GX_ALWAYS;
    }
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// setRenderState
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
static void
setRenderState(int32 state, void *pParam)
{
    int32 v = (int32)(intptr_t)pParam;

    // 鈹€鈹€ DIAG: 鍏ュ彛鑺傛祦鏃ュ織, 鐪嬭彍鍗曟€佷笅娓告垙浠ｇ爜濡備綍閰嶇疆 GX (disabled) 鈹€鈹€
#if 0
    {
        static uint32 lastFrame = 0xFFFFFFFFu;
        static int    cnt = 0;
        if (gxFrameNum != lastFrame) { lastFrame = gxFrameNum; cnt = 0; }
        if (cnt < 40) {
            const char *n = "?";
            switch(state) {
            case TEXTURERASTER:   n = "TEXTURE";       break;
            case TEXTUREADDRESS:  n = "ADDR";          break;
            case TEXTUREADDRESSU: n = "ADDR_U";        break;
            case TEXTUREADDRESSV: n = "ADDR_V";        break;
            case TEXTUREFILTER:   n = "FILTER";        break;
            case VERTEXALPHA:     n = "VTX_ALPHA";     break;
            case ALPHATESTFUNC:   n = "A_TEST_FN";     break;
            case ALPHATESTREF:    n = "A_TEST_REF";    break;
            case SRCBLEND:        n = "SRC_BLEND";     break;
            case DESTBLEND:       n = "DST_BLEND";     break;
            case ZTESTENABLE:     n = "Z_TEST";        break;
            case ZWRITEENABLE:    n = "Z_WRITE";       break;
            case FOGENABLE:       n = "FOG";           break;
            case FOGCOLOR:        n = "FOG_COLOR";     break;
            case CULLMODE:        n = "CULL";          break;
            }
            if (state == FOGCOLOR && pParam) {
                RGBA *c = (RGBA*)pParam;
                printf("[RS] f%u#%d %s=(%d,%d,%d,%d)\n",
                       gxFrameNum, cnt, n, c->red, c->green, c->blue, c->alpha);
            } else if (state == TEXTURERASTER) {
                printf("[RS] f%u#%d %s=%p\n", gxFrameNum, cnt, n, pParam);
            } else {
                printf("[RS] f%u#%d %s=%d\n", gxFrameNum, cnt, n, (int)v);
            }
            cnt++;
        }
    }
#endif

    switch(state) {

    // 鈹€鈹€ 绾圭悊 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    case TEXTURERASTER:
        gxSetTexture(pParam, 0);
        break;

    case TEXTUREADDRESS:
        gxState.uAddr = v;
        gxState.vAddr = v;
        if(gxState.textures[0])
            gxSetTexture(gxState.textures[0], 0);
        break;

    case TEXTUREADDRESSU:
        gxState.uAddr = v;
        if(gxState.textures[0])
            gxSetTexture(gxState.textures[0], 0);
        break;

    case TEXTUREADDRESSV:
        gxState.vAddr = v;
        if(gxState.textures[0])
            gxSetTexture(gxState.textures[0], 0);
        break;

    case TEXTUREFILTER:
        gxState.texFilter = v;
        if(gxState.textures[0])
            gxSetTexture(gxState.textures[0], 0);
        break;

    // 鈹€鈹€ Vertex Alpha 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    case VERTEXALPHA:
        gxState.vertexAlpha = (v != 0);
        break;

    // 鈹€鈹€ Alpha Test 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    case ALPHATESTFUNC:
        gxState.alphaTestFunc = v;
        GX_SetAlphaCompare(alphaFuncToGX(v),
                           (u8)gxState.alphaTestRef,
                           GX_AOP_AND,
                           GX_ALWAYS, 0);
        break;

    case ALPHATESTREF:
        gxState.alphaTestRef = v & 0xFF;
        GX_SetAlphaCompare(alphaFuncToGX(gxState.alphaTestFunc),
                           (u8)gxState.alphaTestRef,
                           GX_AOP_AND,
                           GX_ALWAYS, 0);
        break;

    case GSALPHATEST:
        gxState.gsAlpha = (v != 0);
        break;

    case GSALPHATESTREF:
        gxState.gsAlphaRef = v & 0xFF;
        break;

    // 鈹€鈹€ Blend 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    case SRCBLEND:
        if(v >= 0 && v < blendMapSize)
            gxState.srcBlend = (int32)blendMap[v];
        GX_SetBlendMode(GX_BM_BLEND,
                        (u8)gxState.srcBlend,
                        (u8)gxState.dstBlend,
                        GX_LO_CLEAR);
        break;

    case DESTBLEND:
        if(v >= 0 && v < blendMapSize)
            gxState.dstBlend = (int32)blendMap[v];
        GX_SetBlendMode(GX_BM_BLEND,
                        (u8)gxState.srcBlend,
                        (u8)gxState.dstBlend,
                        GX_LO_CLEAR);
        break;

    // 鈹€鈹€ Depth 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    case ZTESTENABLE:
        gxState.zTest = (v != 0);
        GX_SetZMode(gxState.zTest  ? GX_TRUE : GX_FALSE,
                    GX_LEQUAL,
                    gxState.zWrite ? GX_TRUE : GX_FALSE);
        break;

    case ZWRITEENABLE:
        gxState.zWrite = (v != 0);
        GX_SetZMode(gxState.zTest  ? GX_TRUE : GX_FALSE,
                    GX_LEQUAL,
                    gxState.zWrite ? GX_TRUE : GX_FALSE);
        break;

    // 鈹€鈹€ Fog 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    case FOGENABLE:
        if(s_fullbrightDebug) {
            gxState.fog = false;
            GXColor nf = { 0, 0, 0, 0 };
            GX_SetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, nf);
            break;
        }
        if(s_freeCamViewActive) {
            gxState.fog = false;
            GXColor nf = { 0, 0, 0, 0 };
            GX_SetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, nf);
            break;
        }
        gxState.fog = (v != 0);
        if(!gxState.fog) {
            GXColor nf = { 0, 0, 0, 0 };
            GX_SetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, nf);
        } else {
            GX_SetFog(GX_FOG_LIN,
                      s_fogStart, s_fogEnd,
                      engine->device.zNear, engine->device.zFar,
                      s_fogColor);
        }
        break;

    case FOGCOLOR:
    {
        RGBA *c = (RGBA*)pParam;
        if(s_fullbrightDebug) {
            GXColor nf = { 0, 0, 0, 0 };
            GX_SetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, nf);
            break;
        }
        if(s_freeCamViewActive) {
            GXColor nf = { 0, 0, 0, 0 };
            GX_SetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, nf);
            break;
        }
        if(c) {
            s_fogColor.r = c->red;
            s_fogColor.g = c->green;
            s_fogColor.b = c->blue;
            s_fogColor.a = c->alpha;
            if(gxState.fog)
                GX_SetFog(GX_FOG_LIN,
                          s_fogStart, s_fogEnd,
                          engine->device.zNear, engine->device.zFar,
                          s_fogColor);
        }
        break;
    }

    // 鈹€鈹€ Cull 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    case CULLMODE:
    {
        gxState.cullMode = v;
        GX_SetCullMode(gxCullModeFromRwState());
        break;
    }

    default:
        break;
    }
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// getRenderState
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
static void*
getRenderState(int32 state)
{
    switch(state) {

    case TEXTURERASTER:
        return gxState.textures[0];

    case SRCBLEND:
        for(int i = 1; i < blendMapSize; i++)   // 鈽?璺宠繃 i=0 鍗犱綅
            if((int32)blendMap[i] == gxState.srcBlend)
                return (void*)(intptr_t)i;
        return (void*)0;

    case DESTBLEND:
        for(int i = 1; i < blendMapSize; i++)   // 鈽?璺宠繃 i=0 鍗犱綅
            if((int32)blendMap[i] == gxState.dstBlend)
                return (void*)(intptr_t)i;
        return (void*)0;

    case ZTESTENABLE:
        return (void*)(intptr_t)(gxState.zTest  ? 1 : 0);
    case ZWRITEENABLE:
        return (void*)(intptr_t)(gxState.zWrite ? 1 : 0);
    case FOGENABLE:
        return (void*)(intptr_t)(gxState.fog    ? 1 : 0);
    case VERTEXALPHA:
        return (void*)(intptr_t)(gxState.vertexAlpha ? 1 : 0);
    case ALPHATESTFUNC:
        return (void*)(intptr_t)gxState.alphaTestFunc;
    case ALPHATESTREF:
        return (void*)(intptr_t)gxState.alphaTestRef;
    case GSALPHATEST:
        return (void*)(intptr_t)(gxState.gsAlpha ? 1 : 0);
    case GSALPHATESTREF:
        return (void*)(intptr_t)gxState.gsAlphaRef;
    case CULLMODE:
        return (void*)(intptr_t)gxState.cullMode;
    case TEXTUREADDRESSU:
        return (void*)(intptr_t)gxState.uAddr;
    case TEXTUREADDRESSV:
        return (void*)(intptr_t)gxState.vAddr;
    case TEXTUREFILTER:
        return (void*)(intptr_t)gxState.texFilter;

    default:
        return nullptr;
    }
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// Im3D 鈥?Immediate Mode 3D Rendering (GX backend)
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲

// Primitive type 鈫?GX primitive
static uint8 s_primToGX[] = {
    0,                  // 0 = PRIMTYPENONE
    GX_LINES,           // 1 = PRIMTYPELINELIST
    GX_LINESTRIP,       // 2 = PRIMTYPEPOLYLINE
    GX_TRIANGLES,       // 3 = PRIMTYPETRILIST
    GX_TRIANGLESTRIP,   // 4 = PRIMTYPETRISTRIP
    GX_TRIANGLEFAN,     // 5 = PRIMTYPETRIFAN
    GX_POINTS,          // 6 = PRIMTYPEPOINTLIST
};

// Per-batch state (set by Transform, consumed by RenderPrimitive)
static Im3DVertex *s_im3dVerts    = nullptr;
static int32       s_im3dNumVerts = 0;

// 鈹€鈹€ 3D vertex descriptor (VTFMT1: POS + NRM + CLR0 + TEX0) 鈹€鈹€鈹€鈹€鈹€鈹€
static void im3dSetupVFmt(void)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_NRM,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_NRM,  GX_NRM_XYZ,  GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
}

// 鈹€鈹€ 3D TEV: texture-modulate or pass-through 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
static void im3dSetupTEV(void)
{
    if(gxState.textures[0]) {
        GX_SetNumChans(1);
        GX_SetNumTexGens(1);
        GX_SetNumTevStages(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    } else {
        GX_SetNumChans(1);
        GX_SetNumTexGens(0);
        GX_SetNumTevStages(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }
}

// 鈹€鈹€ Single vertex submission 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
static inline void
im3dEmit(const Im3DVertex *v)
{
    uint32 c = v->color;          // 0xRRGGBBAA (Big-Endian GX packing)
    GX_Position3f32(v->x, v->y, v->z);
    GX_Normal3f32(v->nx, v->ny, v->nz);
    GX_Color4u8((u8)(c >> 24),    // R = byte3 (big-endian MSB)
                (u8)(c >> 16),    // G = byte2
                (u8)(c >> 8),     // B = byte1
                (u8)c);           // A = byte0 (big-endian LSB)
    GX_TexCoord2f32(v->u, v->v);
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
// im3DTransform: cache verts, load world matrix, config lighting
// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
static void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
    s_im3dVerts    = (Im3DVertex*)vertices;
    s_im3dNumVerts = numVertices;

    // Effects rendered through Im2D leave GX in an orthographic camera state.
    // Im3D is world-space geometry, so restore the complete 3D camera state
    // before its vertices are transformed and depth-tested.
    GX_SetViewport(0.0f, 0.0f, (f32)s_fbWidth, (f32)s_efbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, s_fbWidth, s_efbHeight);
    GX_LoadProjectionMtx(gxProjMtx, gxProjType);

    // Load world matrix
    if(world) {
        Mtx worldMtx;
        rwMatToGxMtx(worldMtx, world);
        Mtx worldView;
        guMtxConcat(gxInvCamLTM, worldMtx, worldView);
        GX_LoadPosMtxImm(worldView, GX_PNMTX0);
    } else {
        GX_LoadPosMtxImm(gxInvCamLTM, GX_PNMTX0);
    }
    GX_SetCurrentMtx(GX_PNMTX0);

    // Lighting: GX_LIGHT0 used for im3d lit batches
    if(flags & im3d::LIGHTING) {
        GX_SetNumChans(1);
        GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE,
                       GX_SRC_REG, GX_SRC_VTX,
                       GX_LIGHT0, GX_DF_CLAMP, GX_AF_SPOT);
    } else {
        GX_SetNumChans(1);
        GX_SetChanCtrl(GX_COLOR0A0, GX_ENABLE,
                       GX_SRC_REG, GX_SRC_VTX,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    }

    // No UV in vertex 鈫?unbind texture
    if(!(flags & im3d::LIGHTING))
        GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE,
                       GX_SRC_REG, GX_SRC_VTX,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    if(!(flags & im3d::VERTEXUV))
        gxState.textures[0] = nullptr;
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
// im3DRenderPrimitive: draw all cached verts
// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
static void
im3DRenderPrimitive(PrimitiveType primType)
{
    if(s_im3dNumVerts < 1 || (uint32)primType >= sizeof(s_primToGX))
        return;

    im3dSetupVFmt();
    im3dSetupTEV();

    GX_Begin(s_primToGX[primType], GX_VTXFMT1, (u16)s_im3dNumVerts);
    for(int32 i = 0; i < s_im3dNumVerts; i++)
        im3dEmit(&s_im3dVerts[i]);
    GX_End();
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
// im3DRenderIndexedPrimitive: draw indexed subset
// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
static void
im3DRenderIndexedPrimitive(PrimitiveType primType,
                           void *indices, int32 numIndices)
{
    if(numIndices < 1 || (uint32)primType >= sizeof(s_primToGX))
        return;

    im3dSetupVFmt();
    im3dSetupTEV();

    uint16 *idx = (uint16*)indices;
    GX_Begin(s_primToGX[primType], GX_VTXFMT1, (u16)numIndices);
    for(int32 i = 0; i < numIndices; i++)
        im3dEmit(&s_im3dVerts[idx[i]]);
    GX_End();
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
// im3DEnd: cleanup after 3D batch
// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
static void
im3DEnd(void)
{
    s_im3dVerts    = nullptr;
    s_im3dNumVerts = 0;
}

static void lightingCB(Atomic * /*atom*/) {}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// imageFindRasterFormat
// TXD 鍔犺浇鏃剁敱 librw 璋冪敤, 纭畾鐩爣 Raster 鐨勬牸寮?// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
static bool32
imageFindRasterFormat(Image *img, int32 type,
                      int32 *width, int32 *height,
                      int32 *depth, int32 *format)
{
    if(!img || !width || !height || !depth || !format)
        return 0;

    // GX 缁熶竴浣跨敤 RGBA8888, 鐢?rasterFromImage 瀹屾垚 tiling 杞崲
    *width  = img->width;
    *height = img->height;
    *depth  = 32;
    // Raster::C8888 = 0x0500, type 浣庡瓧鑺?(TEXTURE=0x04 绛?
    *format = (Raster::C8888) | (type & 0x00FF);

    return 1;
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// GX 纭欢鍒濆鍖?/ 缁堟
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
#define GX_FIFO_SIZE (256 * 1024)
void *s_gxFifo = nullptr;  // exposed for memory diagnostic

static bool32
initGX(void)
{
    // 鈹€鈹€ FIFO 鍒嗛厤 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    s_gxFifo = memalign(32, GX_FIFO_SIZE);
    if(!s_gxFifo) {
        printf("[GX] initGX: FIFO alloc failed\n");
        return 0;
    }
    memset(s_gxFifo, 0, GX_FIFO_SIZE);
    GX_Init(s_gxFifo, GX_FIFO_SIZE);

    // 鈹€鈹€ EFB鈫扻FB 鎷疯礉鍙傛暟 (蹇呴』鍦?GX_Init 涔嬪悗閲嶆柊璁剧疆) 鈹€鈹€鈹€鈹€鈹€鈹€
    GX_SetDispCopySrc(0, 0, s_fbWidth, s_efbHeight);
    GX_SetDispCopyDst(s_fbWidth, s_xfbHeight);
    GX_SetDispCopyYScale((f32)s_xfbHeight / (f32)s_efbHeight);
    if(s_videoMode) {
        u8 samplePattern[12][2];
        u8 vfilter[7];
        bool progressive = (s_videoMode->viHeight == s_videoMode->xfbHeight) &&
                           s_videoMode->field_rendering == 0;
        memcpy(samplePattern, s_videoMode->sample_pattern, sizeof(samplePattern));
        memcpy(vfilter, s_videoMode->vfilter, sizeof(vfilter));
        if(progressive){
            // Wii progressive output does not need the VI deflicker filter.
            // Leaving it enabled makes vertical camera motion look smeared and
            // "jittery" even when the game is presenting at 60 Hz.
            u8 neutral[7] = { 0, 0, 21, 22, 21, 0, 0 };
            GX_SetCopyFilter(s_videoMode->aa, samplePattern, GX_FALSE, neutral);
        }else{
            GX_SetCopyFilter(s_videoMode->aa, samplePattern, GX_TRUE, vfilter);
        }
        GX_SetFieldMode(s_videoMode->field_rendering,
                        (s_videoMode->viHeight == 2 * s_videoMode->xfbHeight) ?
                        GX_ENABLE : GX_DISABLE);
    } else {
        u8 zpat[12][2] = {{0}};
        u8 vf[7] = {0};
        GX_SetCopyFilter(GX_FALSE, zpat, GX_FALSE, vf);
        GX_SetFieldMode(GX_COPY_PROGRESSIVE, GX_ENABLE);
    }
    GX_SetDispCopyGamma(GX_GM_1_0);

    // 鈹€鈹€ EFB 鍍忕礌鏍煎紡 鈥?GX_Init 榛樿涓?RGBA6_Z24锛岄渶鏄惧紡璁剧疆 RGB8 鈹€鈹€
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);

    // 鈹€鈹€ 娓呭睆棰滆壊 / 娣卞害 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    GXColor clearCol = { 0, 0, 0, 255 };
    GX_SetCopyClear(clearCol, GX_MAX_Z24);

    // 鈹€鈹€ Viewport / Scissor (瀹為檯灏哄鐢卞钩鍙颁唬鐮佸湪 gxSetVideoMode 娉ㄥ叆) 鈹€鈹€
    GX_SetViewport(0.0f, 0.0f, (f32)s_fbWidth, (f32)s_efbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, s_fbWidth, s_efbHeight);

    // 鈹€鈹€ 娣卞害娴嬭瘯 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetZCompLoc(GX_TRUE);
    gxState.zTest  = true;
    gxState.zWrite = true;

    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);

    // 鈹€鈹€ 娣峰悎: src=SrcAlpha, dst=InvSrcAlpha (Vice City 榛樿)鈹€
    GX_SetBlendMode(GX_BM_BLEND,
                    GX_BL_SRCALPHA, GX_BL_INVSRCALPHA,
                    GX_LO_CLEAR);
    gxState.srcBlend = (int32)GX_BL_SRCALPHA;
    gxState.dstBlend = (int32)GX_BL_INVSRCALPHA;

    // 鈹€鈹€ 闈㈠墧闄?鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    GX_SetCullMode(GX_CULL_NONE);  // 2D never culls
    gxState.cullMode = CULLBACK;

    // 鈹€鈹€ Alpha 娴嬭瘯: > 0 (VC 甯歌 cutout 鐢ㄦ硶) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    GX_SetAlphaCompare(GX_GEQUAL, 10, GX_AOP_AND, GX_ALWAYS, 0);
    gxState.alphaTestFunc = ALPHAGREATEREQUAL;
    gxState.alphaTestRef  = 10;
    gxState.gsAlpha = false;
    gxState.gsAlphaRef = 128;

    // 鈹€鈹€ 棰滆壊閫氶亾: 椤剁偣鑹?+ 鏃犲厜鐓?鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0,
                   GX_ENABLE,       // 鈽?蹇呴』 ENABLE锛屽惁鍒欓《鐐硅壊鍏ㄨ涓㈠純
                   GX_SRC_REG, GX_SRC_VTX,
                   GX_LIGHTNULL,
                   GX_DF_NONE, GX_AF_NONE);

    // 鈹€鈹€ TEV: 绾圭悊 脳 椤剁偣鑹?(MODULATE) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);

    // 鈹€鈹€ 椤剁偣鎻忚堪绗? POS + CLR0 + TEX0 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);

    // 鈹€鈹€ 闆惧叧闂?鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    GXColor noFog = { 0, 0, 0, 0 };
    GX_SetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, noFog);
    gxState.fog = false;

    printf("[GX] initGX: OK\n");
    return 1;
}

static void
termGX(void)
{
    GX_AbortFrame();
    GX_Flush();
    free(s_gxFifo);
    s_gxFifo = nullptr;
    printf("[GX] termGX: done\n");
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// 澹版槑涓€涓叏灞€鐨?Driver 缁撴瀯浣擄紝鐢ㄦ潵瀛樻斁鎴戜滑鐨勬覆鏌撳嚱鏁?// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
static Driver gxDriver;

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// System Callback 鈥?librw 閫氳繃 engine->device.system() 璋冪敤
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
static bool32
deviceSystem(DeviceReq request, void *arg0, int32 arg1)
{
    switch(request) {
    // DEVICEOPEN handled in gxOpen
    case DEVICEOPEN:
        printf("[GX] DEVICEOPEN Triggered\n");
        return 1;

    case DEVICECLOSE:
        printf("[GX] DEVICECLOSE\n");
        return 1;

    case DEVICEINIT:
        printf("[GX] DEVICEINIT\n");
        return initGX();

    case DEVICETERM:
        printf("[GX] DEVICETERM\n");
        termGX();
        return 1;

    case DEVICEFINALIZE:
        return 1;

    case DEVICEGETNUMSUBSYSTEMS:
        return 1;

    case DEVICEGETCURRENTSUBSYSTEM:
        return 0;

    case DEVICESETSUBSYSTEM:
        return arg1 == 0;

    case DEVICEGETSUBSSYSTEMINFO:
        if(arg1 != 0 || arg0 == nullptr)
            return 0;
        strncpy(((SubSystemInfo*)arg0)->name, "Wii GX", sizeof(SubSystemInfo::name) - 1);
        ((SubSystemInfo*)arg0)->name[sizeof(SubSystemInfo::name) - 1] = '\0';
        return 1;

    case DEVICEGETNUMVIDEOMODES:
        return 1;

    case DEVICEGETCURRENTVIDEOMODE:
        return 0;

    case DEVICESETVIDEOMODE:
        return arg1 == 0;

    case DEVICEGETVIDEOMODEINFO:
        if(arg1 != 0 || arg0 == nullptr)
            return 0;
        ((VideoMode*)arg0)->width  = 640;
        ((VideoMode*)arg0)->height = 480;
        ((VideoMode*)arg0)->depth  = 32;
        ((VideoMode*)arg0)->flags  = VIDEOMODEEXCLUSIVE;
        return 1;

    case DEVICEGETMAXMULTISAMPLINGLEVELS:
    case DEVICEGETMULTISAMPLINGLEVELS:
        return 1;

    case DEVICESETMULTISAMPLINGLEVELS:
        return arg1 == 1;

    default:
        printf("[GX] deviceSystem: unhandled request=%d arg1=%d\n", request, arg1);
        return 1;
    }
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// gxOpen / gxClose
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲

// Im2D entry points implemented in gxim2d.cpp
void im2DRenderLine(void *vertices, int32 numVertices,
                    int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices,
                        int32 vert1, int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType primType,
                         void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType primType,
                                void *vertices, int32 numVertices,
                                void *indices, int32 numIndices);

bool32
gxOpen(EngineOpenParams *params)
{
    (void)params;

    // 鍒濆鍖栧叏灞€鐭╅樀 鈥?闃叉棣栧抚 zero-matrix 宕╂簝
    guMtxIdentity(gxInvCamLTM);
    guMtx44Identity(gxProjMtx);

    // register device callbacks
    engine->device.system       = deviceSystem;
    engine->device.zNear        = 0.1f;
    engine->device.zFar         = 10000.0f;

    // engine->driver[PLATFORM_GX] 宸茬敱 Engine::open() 鍒嗛厤濂藉唴瀛橈紝
    // 鐩存帴濉厖瀛楁鍗冲彲锛屼笉鑳芥浛鎹㈡寚閽堬紙浼氭硠婕?rwNew 鍒嗛厤鐨勫唴瀛橈級
    Driver *drv = engine->driver[PLATFORM_GX];

    drv->rasterCreate          = rasterCreate;
    drv->rasterLock            = rasterLock;
    drv->rasterUnlock          = rasterUnlock;
    drv->rasterNumLevels       = rasterNumLevels;
    drv->imageFindRasterFormat = imageFindRasterFormat;
    drv->rasterFromImage       = rasterFromImage;
    drv->rasterToImage         = rasterToImage;

    engine->device.beginUpdate      = beginUpdate;
    engine->device.endUpdate        = endUpdate;
    engine->device.showRaster       = showRaster;
    engine->device.rasterRenderFast = rasterRenderFastDiag;
    engine->device.setRenderState   = setRenderState;
    engine->device.getRenderState   = getRenderState;
    engine->device.clearCamera      = clearCamera;

    engine->device.im2DRenderLine             = im2DRenderLine;
    engine->device.im2DRenderTriangle         = im2DRenderTriangle;
    engine->device.im2DRenderPrimitive        = im2DRenderPrimitive;
    engine->device.im2DRenderIndexedPrimitive = im2DRenderIndexedPrimitive;
    engine->device.im3DTransform              = im3DTransform;
    engine->device.im3DRenderPrimitive        = im3DRenderPrimitive;
    engine->device.im3DRenderIndexedPrimitive = im3DRenderIndexedPrimitive;
    engine->device.im3DEnd                    = im3DEnd;

    // 娉ㄥ唽鎻掍欢 + 榛樿 Pipeline + Skin Pipeline
    registerNativeRaster();
    engine->driver[PLATFORM_GX]->defaultPipeline = makeDefaultPipeline();
    initSkin();

    printf("[GX] gxOpen: device callbacks registered\n");
    return 1;
}

bool32
gxClose(void)
{
    printf("[GX] gxClose\n");
    return 1;
}

} // namespace gx
} // namespace rw

#endif // GAMECUBE
