// ============================================================
// wii.cpp  -  reVC Wii Platform Skeleton
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <malloc.h>
#include <unistd.h>
#include <reent.h>
#include <gccore.h>
#include <ogc/conf.h>
#include <ogc/lwp_watchdog.h>

#include "../gamecube/gcm_vfs.h"
#include "../gamecube/global_shim.h"

#define strlwr _dummy_strlwr
#define strupr _dummy_strupr
#include "crossplatform.h"
#undef strlwr
#undef strupr

#include "rwcore.h"
#include "common.h"
#include "Frontend.h"
#include "FileMgr.h"
#include "Game.h"
#include "Pad.h"
#include "PCSave.h"
#include "TxdStore.h"
#include "Text.h"
#include "Timer.h"
#include "main.h"
#include "DMAudio.h"
#include "sampman.h"
#include "MemoryMgr.h"     // GC: CMemoryHeap custom allocator
#include "HandlingMgr.h"
#include "Streaming.h"
#include "Vehicle.h"
#include "CutsceneMgr.h"
#include "wii_save.h"
#include "gxmemory.h"

namespace rw { namespace gx {
void texPoolEnforceBudgetImmediate(const char *reason, int maxSteps);
} }

static GXRModeObj *rmode   = NULL;
static void       *xfb     = NULL;
#define DEFAULT_FIFO_SIZE (256u * 1024u)
static void       *gp_fifo = NULL;
static bool        gGcDidSyntheticFirstRestart = false;
static volatile bool gWiiExitRequested = false;
static volatile int  gWiiExitReason    = 0;
static bool        gWiiPresentedThisFrame = false;
static double      gWiiLastFrontendLoopMarkMs = 0.0;
static double      gWiiLastGameLoopMarkMs = 0.0;
static uint32      gWiiOuterVsyncDiagSeq = 0;
static u32         gWiiNextGameRetrace = 0;
static bool        gWiiGameRetraceTargetValid = false;

extern "C" void WiiRecordOuterVSyncWait(double waitMs, double frameLoopMs);

static void
WiiResetLoopTimingMarks(void)
{
    gWiiLastFrontendLoopMarkMs = 0.0;
    gWiiLastGameLoopMarkMs = 0.0;
    gWiiNextGameRetrace = 0;
    gWiiGameRetraceTargetValid = false;
}

static void
WiiApplyFrontendAudioSettings(void)
{
    DMAudio.SetMusicMasterVolume(FrontEndMenuManager.m_PrefsMusicVolume);
    DMAudio.SetEffectsMasterVolume(FrontEndMenuManager.m_PrefsSfxVolume);
    DMAudio.SetMP3BoostVolume(FrontEndMenuManager.m_PrefsMP3BoostVolume);
    if (FrontEndMenuManager.m_PrefsRadioStation < WILDSTYLE ||
        FrontEndMenuManager.m_PrefsRadioStation > WAVE)
        FrontEndMenuManager.m_PrefsRadioStation = WILDSTYLE;
    DMAudio.SetRadioInCar(FrontEndMenuManager.m_PrefsRadioStation);
}

static void
WiiRestoreAudioFadeAfterLoad(void)
{
    DMAudio.SetEffectsFadeVol(MAX_VOLUME);
    DMAudio.SetMusicFadeVol(MAX_VOLUME);
}

static void
WiiWaitForVideoSyncAndMeasure(bool frontendLoop)
{
    const double beforeWaitMs = RsTimer();
    if (frontendLoop) {
        VIDEO_WaitVSync();
    } else {
        if (!gWiiGameRetraceTargetValid)
            gWiiNextGameRetrace = VIDEO_GetRetraceCount() + 1;

        const u32 actualRetrace = VIDEO_WaitForRetrace(gWiiNextGameRetrace);
        gWiiNextGameRetrace = actualRetrace + 2;
        gWiiGameRetraceTargetValid = true;
    }
    const double afterWaitMs = RsTimer();

    double &lastLoopMarkMs = frontendLoop ? gWiiLastFrontendLoopMarkMs : gWiiLastGameLoopMarkMs;
    double frameLoopMs = 0.0;
    if (lastLoopMarkMs > 0.0)
        frameLoopMs = afterWaitMs - lastLoopMarkMs;
    lastLoopMarkMs = afterWaitMs;

    WiiRecordOuterVSyncWait(afterWaitMs - beforeWaitMs, frameLoopMs);

    const double waitMs = afterWaitMs - beforeWaitMs;
    if (!frontendLoop) {
        const bool suspiciousWait =
            waitMs >= 12.0 ||
            (frameLoopMs >= 24.0 && frameLoopMs < 80.0);
        if (suspiciousWait) {
            (void)waitMs;
            (void)frameLoopMs;
        }
        gWiiOuterVsyncDiagSeq++;
    }
}

static const char*
WiiVideoStandardName(u32 viTVMode)
{
    switch (viTVMode >> 2) {
    case VI_NTSC:    return "NTSC";
    case VI_PAL:     return "PAL";
    case VI_MPAL:    return "MPAL";
    case VI_EURGB60: return "EURGB60";
    default:         return "UNKNOWN";
    }
}

static const char*
WiiScanModeName(u32 viTVMode)
{
    switch (viTVMode & 0x3) {
    case VI_INTERLACE:     return "INTERLACE";
    case VI_NON_INTERLACE: return "NON_INTERLACE";
    case VI_PROGRESSIVE:   return "PROGRESSIVE";
    default:               return "UNKNOWN";
    }
}

static const char*
WiiXfbModeName(u32 xfbMode)
{
    switch (xfbMode) {
    case VI_XFBMODE_SF:  return "SF";
    case VI_XFBMODE_DF:  return "DF";
    case VI_XFBMODE_PSF: return "PSF";
    default:             return "UNKNOWN";
    }
}

static const char*
WiiConfVideoName(s32 confVideo)
{
    switch (confVideo) {
    case CONF_VIDEO_NTSC: return "NTSC";
    case CONF_VIDEO_PAL:  return "PAL";
    case CONF_VIDEO_MPAL: return "MPAL";
    default:              return "UNKNOWN";
    }
}

static const char*
WiiConfAspectName(s32 confAspect)
{
    switch (confAspect) {
    case CONF_ASPECT_4_3:  return "4:3";
    case CONF_ASPECT_16_9: return "16:9";
    default:               return "UNKNOWN";
    }
}

static void
WiiRequestExit(int reason)
{
    if (!gWiiExitRequested) {
        gWiiExitRequested = true;
        gWiiExitReason = reason;
        RsGlobal.quit = true;
    }
}

static void
WiiLogVideoMode(const char *tag, const GXRModeObj *mode)
{
    if (mode == NULL) {
        SYS_Report("[reVC-WII] %s: <null>\n", tag);
        return;
    }

    SYS_Report(
        "[reVC-WII] %s: std=%s scan=%s viTVMode=0x%X fb=%u efb=%u xfb=%u vi=(%u,%u) xfbMode=%s field=%u aa=%u\n",
        tag,
        WiiVideoStandardName(mode->viTVMode),
        WiiScanModeName(mode->viTVMode),
        mode->viTVMode,
        mode->fbWidth,
        mode->efbHeight,
        mode->xfbHeight,
        mode->viWidth,
        mode->viHeight,
        WiiXfbModeName(mode->xfbMode),
        mode->field_rendering,
        mode->aa);
}

static void
WiiFinishExitRequest(void)
{
    if (!gWiiExitRequested)
        return;

    SYS_Report("[reVC-WII] Finishing exit request: reason=%d\n", gWiiExitReason);
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    SYS_ResetSystem(gWiiExitReason ? gWiiExitReason : SYS_POWEROFF, 0, 0);
}

static GXRModeObj*
WiiChooseStableVideoMode(void)
{
    GXRModeObj *preferred = VIDEO_GetPreferredMode(NULL);
    if (preferred == NULL)
        return preferred;

    WiiLogVideoMode("Preferred mode (system/libogc)", preferred);

    switch (preferred->viTVMode) {
    case VI_TVMODE_NTSC_INT:
    case VI_TVMODE_NTSC_DS:
        return &TVNtsc480Prog;
    case VI_TVMODE_EURGB60_INT:
    case VI_TVMODE_EURGB60_DS:
        return &TVEurgb60Hz480Prog;
    case VI_TVMODE_PAL_INT:
    case VI_TVMODE_PAL_DS:
        return &TVPal528Prog;
    default:
        break;
    }

    if ((preferred->viTVMode & 0x3) == VI_INTERLACE) {
        preferred->viTVMode = VI_TVMODE((preferred->viTVMode >> 2), VI_NON_INTERLACE);
        preferred->field_rendering = 0;
        preferred->xfbMode = VI_XFBMODE_SF;
    }

    return preferred;
}

static void
GcResetPadStateForGameplay(void)
{
    CPad::ResetCheats();
    CPad::StopPadsShaking();
    for (int i = 0; i < MAX_PADS; i++) {
        CPad *pad = CPad::GetPad(i);
        if (pad == nil)
            continue;
        pad->Clear(true);
        pad->SetMode(0);
    }
    SYS_Report("[reVC-WII] Pad state reset for gameplay handoff\n");
}

// Guard padding to detect memory corruption around RsGlobal
static uint32_t _guard_before[64];  // 256 bytes before
RsGlobalType RsGlobal;   // ONLY definition on GC (skeleton.cpp excluded by CMake)
static uint32_t _guard_after[64];   // 256 bytes after

static bool
WiiLogHasPrefix(const char *line, const char *prefix)
{
    if (line == NULL || prefix == NULL)
        return false;
    size_t prefixLen = strlen(prefix);
    return strncmp(line, prefix, prefixLen) == 0;
}

static bool
WiiShouldSuppressRuntimeLog(const char *line)
{
    static const char *s_prefixes[] = {
        "[DBG]: ",
        "[CARCTRL-",
        "[PED-",
        "[PEDSTAT-",
        "[PED-OBJ]",
        "[SCR-CAR",
        "[SCR-CARCHK]",
        "[SCR-CUT]",
        "[SCR-WARP]",
        "[SKIN]",
        "[SKIN-",
        "[ATOMIC-RENDER]",
        "[PIPE-",
        "[VEH-INIT]",
        "[TEX-FIND]",
        "[GX] gxSetVideoMode",
        "[GX] gxSetXFBs",
        "[GX-CAM]",
        "[GX-TEXMISS]",
        "[GX-TEXFOCUS]",
        "[GX-CMPRFIX]",
        "Start loading ",
        "Finish loading ",
        "[GX-TEXP] budget pressure after",
        "[GX-TEXP] shrink CMPR #",
        "[GX-TEXP] shrink RGBA8 #",
        "[GX-TEXP] safe alloc budget tighten",
        "[GX-TEXP] critical UI budget tighten",
        "[GX-TEXP] critical UI upload",
        "[GX-POOL] fallback to plain MEM1",
    };

    if (line == NULL || line[0] == '\0')
        return false;

    for (size_t i = 0; i < sizeof(s_prefixes) / sizeof(s_prefixes[0]); i++) {
        if (WiiLogHasPrefix(line, s_prefixes[i]))
            return true;
    }

    if (WiiLogHasPrefix(line, "[GX-TEXP] safeGxAlloc(") &&
        strstr(line, "SUCCESS after") != NULL)
        return true;

    return false;
}

// ============================================================
// [FIX-4] å«æ newlib _write_rï¼æ printf è·¯ç±å?EXI OSREPORT
//
// æ­£å¸¸æåµä¸?SYS_Report ç´æ¥å?EXI è®¾å¤ï¼ä¸ä¼è§¦å?printf éå½ã?
// ä½å¨å¼å¸¸å¤çè·¯å¾ä¸­ï¼SYS_Report å¯è½åéå?vfprintf å¯¼è´
// _write_r â?SYS_Report â?vfprintf â?_write_r æ éå¾ªç¯ã?
// ç¨éå¥å®å«é»æ­è¿ç§åºæ¯ã?
// ============================================================
extern "C" ssize_t _write_r(struct _reent *reent,
                             int           fd,
                             const void   *buf,
                             size_t        nbytes)
{
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        static int s_write_depth = 0;
        static char s_linebuf[1024];
        static size_t s_linelen = 0;
        if (s_write_depth > 0) {
            // Re-entered â?SYS_Report's internal vfprintf called us.
            // Return nbytes (pretend we wrote) to avoid vfprintf looping.
            return (ssize_t)nbytes;
        }
        s_write_depth = 1;

        const char *p    = (const char *)buf;
        size_t      left = nbytes;
        while (left > 0) {
            char c = *p++;
            left--;

            if (c == '\r')
                continue;

            if (s_linelen + 1 >= sizeof(s_linebuf)) {
                s_linebuf[s_linelen] = '\0';
                if (!WiiShouldSuppressRuntimeLog(s_linebuf))
                    SYS_Report("%s", s_linebuf);
                s_linelen = 0;
            }

            s_linebuf[s_linelen++] = c;

            if (c == '\n') {
                s_linebuf[s_linelen] = '\0';
                if (!WiiShouldSuppressRuntimeLog(s_linebuf))
                    SYS_Report("%s", s_linebuf);
                s_linelen = 0;
            }
        }

        s_write_depth = 0;
        return (ssize_t)nbytes;
    }
    reent->_errno = EBADF;
    return -1;
}

// ============================================================
// [NEW-FIX-12] fopen è·¯å¾è§èå?+ --wrap=fopen å®ç°
// ============================================================
extern "C" FILE *__real_fopen(const char *path, const char *mode);

static void gcm_normalize_path(const char *src, char *dst, size_t dstsize)
{
    char   tmp[512];
    size_t i;

    strncpy(tmp, src, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (i = 0; tmp[i] != '\0'; i++) {
        if (tmp[i] == '\\')
            tmp[i] = '/';
    }
    // Trim trailing whitespace â?original DAT/IDE files use fixed-width
    // columns, so parsed filenames often end with spaces. Windows FS APIs
    // silently strip them, but GameCube VFS uses exact FST matching.
    {
        int len = (int)strlen(tmp);
        while (len > 0 && (tmp[len - 1] == ' ' || tmp[len - 1] == '\t'))
            tmp[--len] = '\0';
    }
// On my avenue to jet airliner,avenue to my broken dream.
    if (strncmp(tmp, "dvd:/", 5) == 0 ||
        strncmp(tmp, "fat:/", 5) == 0 ||
        strncmp(tmp, "sd:/",  4) == 0)
    {
        strncpy(dst, tmp, dstsize - 1);
        dst[dstsize - 1] = '\0';
        char *slash = strchr(dst, '/');
        if (slash) {
            for (char *p = slash + 1; *p; p++)
                *p = (char)tolower((unsigned char)*p);
        }
        return;
    }

    if (tmp[0] == '/') {
        strncpy(dst, tmp, dstsize - 1);
        dst[dstsize - 1] = '\0';
        for (char *p = dst; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        return;
    }

    for (i = 0; tmp[i] != '\0'; i++)
        tmp[i] = (char)tolower((unsigned char)tmp[i]);

    snprintf(dst, dstsize, "dvd:/%s", tmp);
}

extern "C" FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (!path) return NULL;

    char normalized[512];
    gcm_normalize_path(path, normalized, sizeof(normalized));

    if (strcmp(path, normalized) != 0) {
        ((void)0); // [GC-DEBUG-DISABLED]
    }

    return __real_fopen(normalized, mode);
}

// ============================================================
// GX å¾å½¢ç®¡çº¿åå§å?
// ============================================================
void SetupGX(void) {
    // GX FIFO + å¨ç¶æåå§åå·²ç§»è?gxdevice.cpp ç?initGX()
    // (ç?RwEngineStart â?DEVICEINIT è§¦å)
    // è¿éçæ§åå§åå·²ç§»é¤, é¿ååé GX_Init å¯¼è´å¯å­å¨æ³æ¼?
    (void)gp_fifo; // ä¿çå£°æ, ææ¶æªç¨
}

// ============================================================
// RenderWare å¼æåå§å?
// ============================================================

// â¼â¼â?[NEW] 1. ååå£°æ GX åç«¯çæè½½å½æ?â¼â¼â?
namespace rw {
    namespace gx {
        struct EngineOpenParams;         // â?å¿é¡»å?rw::gx åï¼ä¸?rwgx.h / gxdevice.cpp ä¸è´ï¼
        extern int  gxOpen(EngineOpenParams* params);
        extern void gxSetVideoMode(const GXRModeObj *mode);
        extern void gxSetXFBs(void *xfb0, void *xfb1);
        extern void *s_gxFifo;
    }
}
// â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²

RwBool RsRwInitialize(void *param) {
    rmode = WiiChooseStableVideoMode();
    WiiLogVideoMode("Selected mode (engine)", rmode);
    rw::gx::gxSetVideoMode(rmode);
    xfb   = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    // Clear initial XFB to YUYV black (same as xfb2)
    {
        uint32_t *dst = (uint32_t*)xfb;
        size_t count = (rmode->fbWidth * rmode->xfbHeight * 2) / 4;
        for (size_t i = 0; i < count; i++)
            dst[i] = 0x00800080;
    }
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    SetupGX();

    // ââ MEM1 arena diagnostic ââ
    {
        void *arenaLo = SYS_GetArena1Lo();
        void *arenaHi = SYS_GetArena1Hi();
        u32   arenaSz = (u32)arenaHi - (u32)arenaLo;
        SYS_Report("[reVC-WII] Arena1: lo=%p hi=%p size=%uKB (%uMB)\n",
                   arenaLo, arenaHi, arenaSz/1024, arenaSz/(1024*1024));
    }
    {
        void *arena2Lo = SYS_GetArena2Lo();
        void *arena2Hi = SYS_GetArena2Hi();
        u32   arena2Sz = (u32)arena2Hi - (u32)arena2Lo;
        SYS_Report("[reVC-WII] Arena2: lo=%p hi=%p size=%uKB (%uMB)\n",
                   arena2Lo, arena2Hi, arena2Sz/1024, arena2Sz/(1024*1024));
    }

    SYS_Report("[reVC-WII] RsRwInit: Step 1 RwEngineInit\n");
    if (!RwEngineInit(&memFuncs, 0, 8 * 1024 * 1024)) {
        SYS_Report("[reVC-WII] FATAL: RwEngineInit failed!\n");
        return FALSE;
    }

    SYS_Report("[reVC-WII] RsRwInit: Step 2 rsPLUGINATTACH\n");
    if (RsEventHandler(rsPLUGINATTACH, NULL) == rsEVENTERROR) {
        SYS_Report("[reVC-WII] FATAL: rsPLUGINATTACH failed!\n");
        return FALSE;
    }

    SYS_Report("[reVC-WII] RsRwInit: Step 3 RwEngineOpen\n");
    RwEngineOpenParams openParams;
    memset(&openParams, 0, sizeof(openParams));
    openParams.displayID = (void*)1;

    if (!RwEngineOpen(&openParams)) {
        SYS_Report("[reVC-WII] FATAL: RwEngineOpen failed!\n");
        return FALSE;
    }

    // â¼â¼â?å?RwEngineOpen ä¹åæè½½ GX é©±å¨:
    // Engine::open() ä¼ç¨ null::renderdevice è¦ç engine->deviceï¼?
    // æä»¥å¿é¡»å¨å®ä¹ååè®¾ç½® GX åè°ï¼å¦å?DEVICEINIT ä¸ä¼è°å° initGX
    SYS_Report("[reVC-WII] Hooking Wii GX Driver...\n");
    rw::gx::gxOpen(NULL);

    // -- åç¼å?XFB åé + ç»å®å?GX åç«¯ ----------------------
    {
        void *xfb2 = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
        if (xfb2) {
            // YUYV black: Y=0, Cb=128, Cr=128 â?0x00800080 per 32-bit word
            {
                uint32_t *dst = (uint32_t*)xfb2;
                size_t count = (rmode->fbWidth * rmode->xfbHeight * 2) / 4;
                for (size_t i = 0; i < count; i++)
                    dst[i] = 0x00800080;
            }
            rw::gx::gxSetXFBs(xfb, xfb2);
            SYS_Report("[reVC-WII] GX XFBs: xfb0=%p xfb1=%p\n", xfb, xfb2);
        } else {
            rw::gx::gxSetXFBs(xfb, xfb);
            SYS_Report("[reVC-WII] GX XFBs: SINGLE-BUFFER fallback xfb=%p\n", xfb);
        }
    }
    // â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²â²

    SYS_Report("[reVC-WII] RsRwInit: Step 4 RwEngineSetSubSystem\n");
    if (!RwEngineSetSubSystem(0)) {
        SYS_Report("[reVC-WII] FATAL: RwEngineSetSubSystem failed!\n");
        return FALSE;
    }

    SYS_Report("[reVC-WII] RsRwInit: Step 5 RwEngineSetVideoMode\n");
    if (!RwEngineSetVideoMode(0)) {
        SYS_Report("[reVC-WII] FATAL: RwEngineSetVideoMode failed!\n");
        return FALSE;
    }

    SYS_Report("[reVC-WII] RsRwInit: Step 6 RwEngineStart\n");
    if (!RwEngineStart()) {
        SYS_Report("[reVC-WII] FATAL: RwEngineStart failed!\n");
        return FALSE;
    }

    SYS_Report("[reVC-WII] RsRwInit: SUCCESS!\n");
    return TRUE;
}

void   RsRwTerminate(void)      {}
RwBool RsInitialize(void)       { return TRUE; }
void   RsTerminate(void)        { GCM_VFS_Unmount(); }
RwBool RsInitializeEngine(void) { return TRUE; }
void   RsTerminateEngine(void)  {}

RwBool RsSelectDevice(void) {
    RsGlobal.width  = 640;
    RsGlobal.height = 480;
    printf("[reVC-WII] RsSelectDevice: set 640x480\n");
    return TRUE;
}

extern RsEventStatus AppEventHandler(RsEvent event, void *param);
RsEventStatus RsEventHandler(RsEvent event, void *param) {
    // Rate-limited: only log non-IDLE events + first few IDLEs
    if (event != rsIDLE) {
	//printf("[reVC-WII] RsEventHandler event=%d\n", (int)event);
    }
    RsEventStatus ret = AppEventHandler(event, param);
    return ret;
}

// ============================================================
// åºç¡å¼æåè£æ¡?
// ============================================================
static int gcCamDebug = 0;
RwBool RsCameraBeginUpdate(RwCamera *camera) {
    if (camera == nil) {
        printf("[reVC-WII] WARN: RsCameraBeginUpdate called with NULL camera!\n");
        return FALSE;
    }
    if (gcCamDebug < 5) {
		//SYS_Report("[reVC-WII] RsCameraBeginUpdate #%d\n", gcCamDebug);
        gcCamDebug++;
    }
    ::RwCameraBeginUpdate(camera);
    return TRUE;
}

void RsCameraShowRaster(RwCamera *camera) {
    if (camera == nil) {
        printf("[reVC-WII] WARN: RsCameraShowRaster called with NULL camera!\n");
        return;
    }
    gWiiPresentedThisFrame = true;
    ::RwCameraShowRaster(camera, NULL, 0);
}

RwImage* RsGrabScreen(RwCamera *camera)   { return NULL; }
void     RsMouseSetPos(RwV2d *pos)        {}
RwBool   RsInputDeviceAttach(RsInputDeviceType deviceType,
                              RsInputEventHandler eventHandler) { return 1; }
double   RsTimer(void) {
    return (double)ticks_to_microsecs(gettime()) / 1000.0;
}

// ============================================================
// PC / PS2 ä¸å±ç¡¬ä»¶å½æ°æ¡?
// ============================================================
extern "C" {
    void* _psGetVideoModeList()               { return NULL; }
    int   _psGetNumVideModes()                { return 0; }
    void  _InputMouseNeedsExclusive(void)     {}
    void  _InputShutdownMouse(void)           {}
    void  _InputInitialiseMouse(void)         {}
    char  _InputTranslateShiftKeyUpDown(int*) { return 0; }

    // HandleExit stub â?called from TexRead, Frontend, Game on exit
    void HandleExit() {
        printf("[WII] HandleExit called â?setting RsGlobal.quit\n");
        WiiRequestExit(SYS_POWEROFF);
    }
}
const char*
_psGetUserFilesFolder()
{
    return WiiSaveGetDataDir();
}

// ============================================================
// é«çº§æ¸²æç®¡çº¿æ¡?
// ============================================================
namespace WorldRender {
    int  numBlendInsts = 0;
    void AtomicFirstPass(rw::Atomic*, int)             {}
    void AtomicFullyTransparent(rw::Atomic*, int, int) {}
    void RenderBlendPass(int)                          {}
}

namespace CustomPipes {
    void CustomPipeInit()      {}
    void CustomPipeTerm()      {}
    void CreateVehiclePipe()   {}
    void CreateWorldPipe()     {}
    void CreateGlossPipe()     {}
    void CreateRimLightPipes() {}
    void DestroyVehiclePipe()  {}
    void DestroyWorldPipe()    {}
    void DestroyGlossPipe()    {}
    void DestroyRimLightPipes(){}
}

struct Im2DVertexUV2 {};
void openim2d_uv2()  {}
void closeim2d_uv2() {}
void RenderIndexedPrimitive_UV2(RwPrimitiveType, Im2DVertexUV2*,
                                int, unsigned short*, int) {}

class CParticleObject;
namespace ScreenDroplets {
    void Initialise()                     {}
    void InitDraw()                       {}
    void Process()                        {}
    void Render()                         {}
    void Shutdown()                       {}
    void RegisterSplash(CParticleObject*) {}
}

// â?å é¤ä»¥ä¸æ´ä¸ª namespace rw å?â?
// â?ä»¥ä¸ä»£ç ä¸?librw.a(charset.cpp.obj) éå¤å®ä¹ï¼å¯¼è´é¾æ¥å¤±è´?
// namespace rw {
//     int32    Charset::open()                                    { return 1; }
//     void     Charset::close()                                   {}
//     void     Charset::printBuffered(char const*, int, int, int) {}
//     void     Charset::flushBuffer()                             {}
//     Charset* Charset::create(RGBA const*, RGBA const*)          { return NULL; }
//     void     Charset::destroy()                                 {}
// }
// â?librw ç?charset.cpp å·²æä¾å®æ´å®ç°ï¼æ­¤å¤æ ééå¤å®ä¹

// â?XFB heartbeat: exposed from gxdevice.cpp (extern "C" global)
extern "C" void *s_gx_xfb[2];

// å¸§è®¡æ°è¯æ?(DIAG_LOG macros)
#include "diagnostic.h"

int  g_diagFrame   = 0;
int  g_diagVerbose = -1;   // å?5 å¸§æå°è¯¦ç»æ¥å¿?

// ============================================================
// [NEW-FIX] å¼å¸¸è¯æ­è¾å©
// libogc2 é»è®¤ DSI/ISI handler å·²è¾åºå° OSREPORTã?
// æ­¤åè°ä»å¨ç¨æ·æ Reset æé®æ¶è§¦åã?
// ============================================================

static void gc_reset_cb(void) {
    SYS_Report("[WII] RESET button pressed.\n");
    WiiRequestExit(SYS_RESTART);
}

static void gc_power_cb(void) {
    SYS_Report("[WII] POWER button pressed.\n");
    WiiRequestExit(SYS_POWEROFF);
}

// ============================================================
// GameCube å¥å£ç?
// ============================================================
int main(int argc, char *argv[]) {
	InitMemoryMgr();

    SYS_SetResetCallback(gc_reset_cb);
    SYS_SetPowerCallback(gc_power_cb);

    FrontEndMenuManager.m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;

    VIDEO_Init();
    PAD_Init();

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    SYS_Report("\n================================================\n");
    SYS_Report("[reVC-WII] Vice City Engine Booting...\n");
    SYS_Report("================================================\n");

    printf("[reVC-WII] printf -> OSREPORT routing: OK\n");

    if (CONF_Init() >= 0) {
        int confVideo = CONF_GetVideo();
        int confAspect = CONF_GetAspectRatio();
        int confProg = CONF_GetProgressiveScan();
        int confRgb60 = CONF_GetEuRGB60();
        SYS_Report("[reVC-WII] SYSCONF: video=%s(%d) aspect=%s(%d) progressive=%d eurgb60=%d component=%u retrace=%.2fHz\n",
            WiiConfVideoName(confVideo), confVideo,
            WiiConfAspectName(confAspect), confAspect,
            confProg,
            confRgb60,
            VIDEO_HaveComponentCable(),
            VIDEO_GetRetraceRate());
    } else {
        SYS_Report("[reVC-WII] SYSCONF: init failed\n");
    }

    DVD_Init();
    DVD_Mount();

    if (!GCM_VFS_Mount()) {
        SYS_Report("[reVC-WII] FATAL: VFS Mount FAILED!\n");
        while (true) { VIDEO_WaitVSync(); }
    }
    SYS_Report("[reVC-WII] VFS Mounted!\n");
    {
        const char *saveDir = WiiSaveGetDataDir();
        if (saveDir != nil && saveDir[0] != '\0')
            SYS_Report("[reVC-WII] Save backend ready: %s\n", saveDir);
        else
            SYS_Report("[reVC-WII] Save backend unavailable at boot\n");
    }

    // ââ GC Custom Allocator: grab a big block from MEM1 now (before heap fragments) ââ
    memset(&RsGlobal, 0, sizeof(RsGlobal));
    RsGlobal.appName       = "reVC";
    RsGlobal.maximumWidth  = 640;
    RsGlobal.maximumHeight = 480;
    RsGlobal.width         = 640;
    RsGlobal.height        = 480;
    RsGlobal.quit          = FALSE;

    if (RsEventHandler(rsINITIALIZE, NULL) == rsEVENTERROR) {
        SYS_Report("[reVC-WII] ERROR: rsINITIALIZE failed.\n");
        while (true) { VIDEO_WaitVSync(); }
    }

    if (RsEventHandler(rsRWINITIALIZE, NULL) == rsEVENTERROR) {
        SYS_Report("[reVC-WII] ERROR: rsRWINITIALIZE failed.\n");
        while (true) { VIDEO_WaitVSync(); }
    }

    SYS_Report("[reVC-WII] Engine Ready. Entering Game Loop...\n");

    SYS_Report("[reVC-WII] Running InitialiseOnceAfterRW before frontend loop...\n");
    if (!CGame::InitialiseOnceAfterRW()) {
        SYS_Report("[reVC-WII] FATAL: InitialiseOnceAfterRW failed.\n");
        while (true) { VIDEO_WaitVSync(); }
    }
    // Frontend resources and fade state are initialized by the shared menu
    // state machine.  Do not bind frontend1/frontend2 or set menu flags here:
    // that bypassed CMenuManager::Initialise/LoadAllTextures and loaded text
    // twice, which in turn broke the intro fade and scene labels.
    FrontEndMenuManager.LoadSettings();
    FrontEndMenuManager.m_bGameNotLoaded = true;
    FrontEndMenuManager.RequestFrontEndStartUp();
    WiiApplyFrontendAudioSettings();

    // FrontendIdle() â?PC/PS2 èåä¸»å¾ªç?(å®ä¹å?main.cpp)
    // æ¯å®æ?Idle() æ´è½»é? ä¸å è½½æ¸¸ææ°æ®ãä¸åå§åä¸çãä¸æ¸²æ3D
    extern void FrontendIdle(void);
    extern void Idle(void *arg);
    extern void InitialiseGame(void);
    extern bool b_FoundRecentSavedGameWantToLoad;

    SYS_Report("[reVC-WII] Entering Frontend Loop...\n");
    WiiResetLoopTimingMarks();

    while (!RsGlobal.quit) {
        WiiWaitForVideoSyncAndMeasure(true);

        PAD_ScanPads();
        if (gWiiExitRequested)
            break;

        gWiiPresentedThisFrame = false;

		//DIAG_LOG("[WII] -- Frame %d Start --\n", g_diagFrame);
        FrontendIdle();
        if (gWiiExitRequested)
            break;
		//DIAG_LOG("[WII] -- FrontendIdle done --\n");

        // â?New Game / Load Game transition
        // m_bWantToLoad: è¯»æ¡£æµç¨è®¾ç½® | m_bWantToRestart: æ°æ¸¸ææµç¨è®¾ç½?
        if (FrontEndMenuManager.m_bWantToLoad || FrontEndMenuManager.m_bWantToRestart) {
            const bool wantsInitialLoad = FrontEndMenuManager.m_bWantToLoad;
            if (FrontEndMenuManager.m_bSpritesLoaded) {
                SYS_Report("[reVC-WII] First transition: unloading frontend textures before InitialiseGame()\n");
                FrontEndMenuManager.UnloadTextures();
                FrontEndMenuManager.m_bMenuActive = false;
            }
            SYS_Report("[reVC-WII] Loading game world...\n");
            SYS_Report("[reVC-WII] InitialiseGame() START\n");
            InitialiseGame();
            SYS_Report("[reVC-WII] InitialiseGame() DONE\n");
            if (wantsInitialLoad) {
                SYS_Report("[reVC-WII] First transition requested save load. Running synthetic restart/load path.\n");
                CPad::ResetCheats();
                CPad::StopPadsShaking();
                DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);
                CGame::ShutDownForRestart();
                CTimer::Stop();
                CGame::InitialiseWhenRestarting();
                FrontEndMenuManager.m_bWantToRestart = false;
                SYS_Report("[reVC-WII] Synthetic first restart/load complete.\n");
            }
            WiiRestoreAudioFadeAfterLoad();
            DMAudio.ChangeMusicMode(MUSICMODE_GAME);
            // Splash ownership remains with the script/cutscene lifecycle.
            // Destroying it here made DoFade() lose the intro picture before
            // the black-to-picture transition could be rendered.
            FrontEndMenuManager.m_bGameNotLoaded = false;
            FrontEndMenuManager.m_bWantToRestart = false;
            if (!gGcDidSyntheticFirstRestart) {
                SYS_Report("[reVC-WII] Frontend transition: direct gameplay handoff enabled, synthetic first restart disabled\n");
                gGcDidSyntheticFirstRestart = true;
            }
            GcResetPadStateForGameplay();
            // Let the shared game lifecycle consume the first script frame
            // before the Wii loop submits a visible gameplay frame.  The
            // script creates cutscene objects and assigns their animations
            // after START_CUTSCENE; rendering immediately here exposes their
            // temporary bind-pose and bypasses the initial fade.
            Idle(NULL);
            SYS_Report("[reVC-WII] Game world loaded. Entering game loop.\n");
            break;
        }

        g_diagFrame++;

        // Check guard arrays on first frame
        if (g_diagFrame == 1) {
            int errs = 0;
            for (int i = 0; i < 64; i++) {
                if (_guard_before[i] != 0xDEADBEEF) errs++;
                if (_guard_after[i]  != 0xBEEFDEAD) errs++;
            }
            if (errs > 0)
                DIAG_ERROR("GUARD CORRUPTED: %d words damaged!\n", errs);
            else
                SYS_Report("[reVC-WII] GUARD: OK, no memory corruption detected\n");
        }
		/*        if (g_diagFrame % 60 == 0)*/
    }

    SYS_Report("[reVC-WII] Frontend loop exited. Total frames: %d\n", g_diagFrame);

    // ââ Game Loop ââ
    SYS_Report("[reVC-WII] Entering Game Loop...\n");
    WiiResetLoopTimingMarks();
    int gameFrames = 0;
    while (!RsGlobal.quit) {
        WiiWaitForVideoSyncAndMeasure(false);

        PAD_ScanPads();
        if (gWiiExitRequested)
            break;

        gWiiPresentedThisFrame = false;

        Idle((void*)0x00000001);
        if (gWiiExitRequested)
            break;

        if (FrontEndMenuManager.m_bWantToRestart || b_FoundRecentSavedGameWantToLoad) {
            SYS_Report("[reVC-WII] Restart/load requested: restart=%d recentLoad=%d wantLoad=%d demo=%d time=%d cut=%s cutStatus=%u cutRun=%d. Reinitialising game.\n",
                FrontEndMenuManager.m_bWantToRestart,
                b_FoundRecentSavedGameWantToLoad,
                FrontEndMenuManager.m_bWantToLoad,
                CGame::bDemoMode ? 1 : 0,
                CTimer::GetTimeInMilliseconds(),
                CCutsceneMgr::GetCutsceneName(),
                CCutsceneMgr::ms_cutsceneLoadStatus,
                CCutsceneMgr::IsRunning());

            CPad::ResetCheats();
            CPad::StopPadsShaking();
            DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);
            CGame::ShutDownForRestart();
            CTimer::Stop();

            if (b_FoundRecentSavedGameWantToLoad) {
                FrontEndMenuManager.m_bWantToRestart = true;
                FrontEndMenuManager.m_bWantToLoad = true;
            }

            CGame::InitialiseWhenRestarting();
            WiiRestoreAudioFadeAfterLoad();
            DMAudio.ChangeMusicMode(MUSICMODE_GAME);
            FrontEndMenuManager.m_bWantToRestart = false;
            // Keep script-owned splash textures alive across restart/load;
            // LoadSplash/ShutdownRenderWare release them at their boundary.
            GcResetPadStateForGameplay();
            Idle(NULL);

            gameFrames = 0;
            WiiResetLoopTimingMarks();
            SYS_Report("[reVC-WII] Restart/load complete. Continuing game loop.\n");
            continue;
        }
        gameFrames++;
    }

    SYS_Report("[reVC-WII] Game loop exited. Total frames: %d\n", gameFrames);

    if (gWiiExitRequested) {
        WiiFinishExitRequest();
        return 0;
    }

    RsEventHandler(rsRWTERMINATE, NULL);
    RsEventHandler(rsTERMINATE,   NULL);
    WiiFinishExitRequest();

    return 0;
}
