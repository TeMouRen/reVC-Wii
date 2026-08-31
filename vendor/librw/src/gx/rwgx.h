// vendor/librw/src/gx/rwgx.h
#pragma once

#ifdef RW_GX

#include <stdint.h>
#include <gccore.h>

// ▼▼▼ 关键：C 文件（如 glad.c）只能看到 gccore.h，不能看到 namespace ▼▼▼
#ifdef __cplusplus

namespace rw {
struct Texture;  // forward declare rw::Texture (defined in rwobjects.h)
struct Raster;   // forward declare rw::Raster (defined in rwobjects.h)
struct Material; // forward declare rw::Material (defined in rwobjects.h)
struct Geometry;  // forward declare rw::Geometry (defined in rwobjects.h)
struct Stream;  // forward declare rw::Stream (defined in rwbase.h)
namespace gx {

static const uint32_t PLATFORM_GX_TILED_V2 = 0x8000000Du;

struct Im2DVertex
{
    float    x, y, z, w;
    uint32_t color;
    float    u, v;

    void setScreenX(float x)           { this->x = x; }
    void setScreenY(float y)           { this->y = y; }
    void setScreenZ(float z)           { this->z = z; }
    void setCameraZ(float z)           { this->w = z; }
    void setRecipCameraZ(float rz)     { this->w = rz; }
    void setU(float u, float /*rz*/)   { this->u = u; }
    void setV(float v, float /*rz*/)   { this->v = v; }
    void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        this->color = ((uint32_t)r << 24) |
                      ((uint32_t)g << 16) |
                      ((uint32_t)b << 8)  |
                       (uint32_t)a;
    }
    float getScreenX()       { return x; }
    float getScreenY()       { return y; }
    float getScreenZ()       { return z; }
    float getCameraZ()       { return w; }
    float getRecipCameraZ()  { return w; }
    float getU()             { return u; }
    float getV()             { return v; }
};

struct Im3DVertex
{
    float    x, y, z;
    float    nx, ny, nz;
    uint32_t color;
    float    u, v;

    void setX(float x) { this->x = x; }
    void setY(float y) { this->y = y; }
    void setZ(float z) { this->z = z; }
    void setNormal(float x, float y, float z)
        { nx = x; ny = y; nz = z; }
    void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        this->color = ((uint32_t)r << 24) |
                      ((uint32_t)g << 16) |
                      ((uint32_t)b << 8)  |
                       (uint32_t)a;
    }
    void setU(float u) { this->u = u; }
    void setV(float v) { this->v = v; }
    float getX() { return x; }
    float getY() { return y; }
    float getZ() { return z; }
    float getU() { return u; }
    float getV() { return v; }
};

enum GxRasterAlphaKind
{
    GX_RASTER_ALPHA_NONE = 0,
    GX_RASTER_ALPHA_CUTOUT,
    GX_RASTER_ALPHA_SMOOTH
};

enum GxRasterCpuStorage
{
    GX_RASTER_CPU_NONE = 0,
    GX_RASTER_CPU_GENERIC_MEM2,
    GX_RASTER_CPU_GX
};

struct GxRaster
{
    GXTexObj  texObj;
    void     *gxData;
    void     *cpuData;
    uint32_t  dataSize;
    uint32_t  cpuDataSize;
    uint16_t  w, h;
    uint8_t   gxFmt;
    uint8_t   hasAlpha;
    uint8_t   alphaKind;
    uint8_t   wrapS, wrapT;
    uint8_t   minFilter, magFilter;
    uint8_t   preferOwnSampler;
    uint8_t   usageClass;
    uint8_t   cpuDataStorage;
    bool      dirty;
    bool      texObjValid;
};

extern int32_t  nativeRasterOffset;
extern Mtx      gxInvCamLTM;
extern Mtx44    gxProjMtx;         // Saved camera projection (reloaded by 3D pipes)
extern uint8_t  gxProjType;       // GX_PERSPECTIVE or GX_ORTHOGRAPHIC
extern uint32_t gxFrameNum;        // ▲ 诊断用全局帧计数器, 由 beginUpdate 自增

struct GxState {
    void    *textures[8];
    uint8_t  texMinFilter[8];
    uint8_t  texMagFilter[8];
    uint8_t  texWrapS[8];
    uint8_t  texWrapT[8];
    bool     zWrite;
    bool     zTest;
    uint32_t srcBlend;
    uint32_t dstBlend;
    
    // ▼ 补充以下缺失的渲染状态变量 ▼
    int32_t  uAddr;
    int32_t  vAddr;
    int32_t  texFilter;
    bool     vertexAlpha;
    int32_t  alphaTestFunc;
    int32_t  alphaTestRef;
    bool     gsAlpha;
    int32_t  gsAlphaRef;
    bool     fog;
    int32_t  cullMode;
};
extern GxState gxState;

void gxSetTexture(void *tex, int32_t unit);
void gxGetFramebufferSize(uint16_t *fbWidth, uint16_t *efbHeight);
// MatFX's PS2 effect pass uses a black fog colour while retaining the
// existing fog range and enable state. These helpers bracket that pass.
void gxMatFXBeginFog(void);
void gxMatFXEndFog(void);
bool gxFullbrightDebugActive(void);
void rwMatToGxMtx(Mtx dst, const void *rwMat);
void convertRGBA8_to_GX(void *dst, const void *src, int w, int h, int srcStride = 0);
uint32_t rgba8TiledSize(int w, int h);
void invalidateTextureBinding(void *raster);
void syncNativeSamplerFromTexture(Texture *tex, Raster *raster);
void *destroyNativeData(void *object, int, int);  // geometry buffer cleanup

// MatFX owns effect-specific GX state. The default object pipe owns the base
// material pass and calls these only for the MatFX pipeline.
bool gxMatFXEnvReady(Material *mat, bool hasNormals);
bool gxMatFXSetupEnv(Material *mat, bool baseTextured, bool vertexAlpha,
                     bool modulateMaterialColor, const Mtx modelView);
bool gxMatFXEnvUsesAlpha(Material *mat);
bool gxMatFXEnvOnlyDebugActive(void);
void gxMatFXRecordEnvUVStats(Material *mat, Geometry *geo,
                             const uint16_t *meshIdx, uint32_t numIdx,
                             const Mtx modelView,
                             uint32_t meshIndex, uint32_t passIndex);

// ── Device lifecycle ────────────────────────────────────────
struct EngineOpenParams;
int32_t gxOpen(EngineOpenParams *params);
int32_t gxClose(void);
void    gxSetVideoMode(const GXRModeObj *mode);
void    gxSetXFBs(void *xfb0, void *xfb1);

// ── GX Native Texture Reader ─────────────────────────────────
Texture* readNativeTexture(Stream *stream);

} // namespace gx

struct ObjPipeline;
ObjPipeline *makeDefaultPipeline(void);
ObjPipeline *makeSkinPipeline(void);
ObjPipeline *makeMatFXPipeline(void);
void initSkin(void);
void initMatFX(void);

} // namespace rw

#endif // __cplusplus
// ▲▲▲ C 文件保护区结束 ▲▲▲

#endif // RW_GX
