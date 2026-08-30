// gxmatfx.cpp -- GX MatFX pipeline boundary
//
// The GX MatFX path currently implements the environment-map effect as a
// separate additive pass. The object pipe owns the base material draw and
// state restoration; this file only owns the effect resource lookup, normal
// reflection texgen, and effect TEV setup.

#ifdef GAMECUBE

#include <stdio.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwgx.h"

// Define GX_PIPELINE_DIAGNOSTICS when targeted MatFX tracing is needed.
#ifndef GX_PIPELINE_DIAGNOSTICS
#define printf(...) ((void)sizeof((::printf)(__VA_ARGS__)))
#endif

// MatFX diagnostics stay enabled for the Wii build, but are capped so a
// material rendered every frame does not flood the serial/debug console.
#ifndef GX_MATFX_TRACE_LIMIT
#define GX_MATFX_TRACE_LIMIT 96u
#endif

namespace rw {
namespace gx {

struct MatFxTraceKey
{
    const Material *mat;
    const Texture *envTex;
};

static MatFxTraceKey s_matFxReadyTrace[GX_MATFX_TRACE_LIMIT];
static uint32 s_matFxReadyTraceCount = 0;
static uint32 s_matFxSetupTraceCount = 0;
static uint32 s_matFxMatrixTraceCount = 0;
static uint32 s_envUvTraceCount = 0;

#ifndef GX_MATFX_ENV_ONLY_DEBUG
#define GX_MATFX_ENV_ONLY_DEBUG 0
#endif

static const MatFX::Env *getEnvMap(Material *mat);

struct EnvUvStats
{
    bool active;
    uint32 meshIndex;
    uint32 passIndex;
    uint32 count;
    float minU;
    float minV;
    float maxU;
    float maxV;
    double sumU;
    double sumV;
};

static EnvUvStats s_envUvStats = { false, 0u, 0u, 0u, 0.0f, 0.0f, 0.0f, 0.0f, 0.0, 0.0 };

bool
gxMatFXEnvOnlyDebugActive(void)
{
    return GX_MATFX_ENV_ONLY_DEBUG != 0;
}

static bool
envUvStatsStart(uint32 meshIndex, uint32 passIndex)
{
    if(!gxMatFXEnvOnlyDebugActive())
        return false;

    s_envUvStats.active = true;
    s_envUvStats.meshIndex = meshIndex;
    s_envUvStats.passIndex = passIndex;
    s_envUvStats.count = 0u;
    s_envUvStats.minU = 1.0e30f;
    s_envUvStats.minV = 1.0e30f;
    s_envUvStats.maxU = -1.0e30f;
    s_envUvStats.maxV = -1.0e30f;
    s_envUvStats.sumU = 0.0;
    s_envUvStats.sumV = 0.0;
    return true;
}

static void
envUvStatsAdd(float u, float v)
{
    if(!s_envUvStats.active)
        return;

    if(u < s_envUvStats.minU) s_envUvStats.minU = u;
    if(v < s_envUvStats.minV) s_envUvStats.minV = v;
    if(u > s_envUvStats.maxU) s_envUvStats.maxU = u;
    if(v > s_envUvStats.maxV) s_envUvStats.maxV = v;
    s_envUvStats.sumU += u;
    s_envUvStats.sumV += v;
    s_envUvStats.count++;
}

static void
envUvStatsFinish(const Material *mat, const Texture *envTex)
{
    if(!s_envUvStats.active)
        return;

    double avgU = s_envUvStats.count ? s_envUvStats.sumU / s_envUvStats.count : 0.0;
    double avgV = s_envUvStats.count ? s_envUvStats.sumV / s_envUvStats.count : 0.0;
    if(s_envUvTraceCount < GX_MATFX_TRACE_LIMIT){
        s_envUvTraceCount++;
        fprintf(stdout,
                "[GX-MATFX-UV] mat=%p envTex=%p name=%s mesh=%u pass=%u n=%u "
                "u=[%.5f,%.5f] v=[%.5f,%.5f] span=(%.5f,%.5f) avg=(%.5f,%.5f)\n",
                (void*)mat,
                (void*)envTex,
                envTex && envTex->name[0] ? envTex->name : "<none>",
                (unsigned)s_envUvStats.meshIndex,
                (unsigned)s_envUvStats.passIndex,
                (unsigned)s_envUvStats.count,
                (double)s_envUvStats.minU,
                (double)s_envUvStats.maxU,
                (double)s_envUvStats.minV,
                (double)s_envUvStats.maxV,
                (double)(s_envUvStats.maxU - s_envUvStats.minU),
                (double)(s_envUvStats.maxV - s_envUvStats.minV),
                avgU,
                avgV);
    }
    s_envUvStats.active = false;
}

static bool
matFxTraceReadyOnce(const Material *mat, const Texture *envTex)
{
    for(uint32 i = 0; i < s_matFxReadyTraceCount; i++)
        if(s_matFxReadyTrace[i].mat == mat &&
           s_matFxReadyTrace[i].envTex == envTex)
            return false;

    if(s_matFxReadyTraceCount >= GX_MATFX_TRACE_LIMIT)
        return false;

    s_matFxReadyTrace[s_matFxReadyTraceCount].mat = mat;
    s_matFxReadyTrace[s_matFxReadyTraceCount].envTex = envTex;
    s_matFxReadyTraceCount++;
    return true;
}

static const char*
matFxTypeName(uint32 type)
{
    switch(type){
    case MatFX::NOTHING:      return "nothing";
    case MatFX::BUMPMAP:      return "bump";
    case MatFX::ENVMAP:       return "envmap";
    case MatFX::BUMPENVMAP:   return "bump+env";
    case MatFX::DUAL:         return "dual";
    case MatFX::UVTRANSFORM:  return "uv-transform";
    case MatFX::DUALUVTRANSFORM: return "dual-uv-transform";
    default:                  return "unknown";
    }
}

static void
matFxTraceRaster(const char *slot, Texture *texture, GxRaster *raster)
{
    fprintf(stdout,
            "[GX-MATFX-TRACE] raster slot=%s tex=%p name=%s raster=%p "
            "valid=%d gxData=%p data=%u wh=%ux%u fmt=0x%02X "
            "alpha=%u kind=%u wrap=%u/%u filter=%u/%u ownSampler=%u\n",
            slot,
            (void*)texture,
            texture && texture->name[0] ? texture->name : "<unnamed>",
            (void*)raster,
            raster && raster->texObjValid ? 1 : 0,
            raster ? raster->gxData : nil,
            raster ? (unsigned)raster->dataSize : 0u,
            raster ? (unsigned)raster->w : 0u,
            raster ? (unsigned)raster->h : 0u,
            raster ? (unsigned)raster->gxFmt : 0xFFu,
            raster ? (unsigned)raster->hasAlpha : 0u,
            raster ? (unsigned)raster->alphaKind : 0u,
            raster ? (unsigned)raster->wrapS : 0u,
            raster ? (unsigned)raster->wrapT : 0u,
            raster ? (unsigned)raster->minFilter : 0u,
            raster ? (unsigned)raster->magFilter : 0u,
            raster ? (unsigned)raster->preferOwnSampler : 0u);
}

static bool
matFxTraceMatrixOnce(const MatFX::Env *env)
{
    // Matrix setup is called once per mesh. A small global cap is enough to
    // capture the first effect-frame variants without logging every draw.
    return env != nil && s_matFxMatrixTraceCount++ < GX_MATFX_TRACE_LIMIT;
}

static void
matFxTraceMatrix(const char *mode, const MatFX::Env *env, const Mtx matrix)
{
    fprintf(stdout,
            "[GX-MATFX-TRACE] texmtx mode=%s env=%p frame=%p "
            "m=[%.5f %.5f %.5f %.5f; %.5f %.5f %.5f %.5f; "
            "%.5f %.5f %.5f %.5f]\n",
            mode,
            (void*)env,
            env ? (void*)env->frame : nil,
            (double)matrix[0][0], (double)matrix[0][1],
            (double)matrix[0][2], (double)matrix[0][3],
            (double)matrix[1][0], (double)matrix[1][1],
            (double)matrix[1][2], (double)matrix[1][3],
            (double)matrix[2][0], (double)matrix[2][1],
            (double)matrix[2][2], (double)matrix[2][3]);
}

static bool
buildEnvTexMatrix(const MatFX::Env *env, Mtx envMtx)
{
    Mtx mapMtx;
    guMtxIdentity(mapMtx);
    // Match the D3D path: flip U only for framed env maps.
    mapMtx[0][0] = (env != nil && env->frame != nil && MatFX::envMapFlipU) ? -0.5f : 0.5f;
    mapMtx[0][3] = 0.5f;
    mapMtx[1][1] = -0.5f;
    mapMtx[1][3] = 0.5f;

    if(env == nil)
        return false;

    if(env->frame == nil){
        memcpy(envMtx, mapMtx, sizeof(Mtx));
        return true;
    }

    Mtx frameMtx;
    Mtx invFrame;
    Mtx invView;
    Mtx normalMtx;
    rwMatToGxMtx(frameMtx, env->frame->getLTM());
    if(!guMtxInverse(frameMtx, invFrame)){
        memcpy(envMtx, mapMtx, sizeof(Mtx));
        return true;
    }

    bool haveView = guMtxInverse(gxInvCamLTM, invView);
    if(!haveView)
        guMtxIdentity(invView);

    // The D3D reference builds the framed map from the camera frame with its
    // right axis negated. gxInvCamLTM already contains the GX handedness
    // correction on Z, so undo that correction before applying the D3D
    // camera-frame convention here.
    if(haveView){
        for(uint32 row = 0; row < 3; row++){
            invView[row][2] = -invView[row][2];
            invView[row][0] = -invView[row][0];
            invView[row][3] = 0.0f;
        }
    }

    invFrame[0][3] = 0.0f;
    invFrame[1][3] = 0.0f;
    invFrame[2][3] = 0.0f;
    // RwMatrixMultiply(out, a, b) and guMtxConcat(a, b, out) both use the
    // row-major a*b order. The D3D reference is camera-frame * inverse-env,
    // followed by the sphere-map scale/offset matrix.
    guMtxConcat(invView, invFrame, normalMtx);
    guMtxConcat(normalMtx, mapMtx, envMtx);
    return true;
}

static bool
buildEnvNormalMatrix(const Mtx modelView, Mtx normalMtx)
{
    Mtx invModelView;
    if(!guMtxInverse(modelView, invModelView)){
        memcpy(normalMtx, modelView, sizeof(Mtx));
        normalMtx[0][3] = 0.0f;
        normalMtx[1][3] = 0.0f;
        normalMtx[2][3] = 0.0f;
        return false;
    }

    normalMtx[0][0] = invModelView[0][0];
    normalMtx[0][1] = invModelView[1][0];
    normalMtx[0][2] = invModelView[2][0];
    normalMtx[1][0] = invModelView[0][1];
    normalMtx[1][1] = invModelView[1][1];
    normalMtx[1][2] = invModelView[2][1];
    normalMtx[2][0] = invModelView[0][2];
    normalMtx[2][1] = invModelView[1][2];
    normalMtx[2][2] = invModelView[2][2];
    normalMtx[0][3] = 0.0f;
    normalMtx[1][3] = 0.0f;
    normalMtx[2][3] = 0.0f;
    return true;
}

void
gxMatFXRecordEnvUVStats(Material *mat, Geometry *geo, const uint16_t *meshIdx,
                        uint32_t numIdx, const Mtx modelView,
                        uint32_t meshIndex, uint32_t passIndex)
{
    if(!gxMatFXEnvOnlyDebugActive() || mat == nil || geo == nil || meshIdx == nil || numIdx == 0)
        return;

    const MatFX::Env *env = getEnvMap(mat);
    V3d *normals = geo->morphTargets && geo->morphTargets[0].normals ?
                   geo->morphTargets[0].normals : nil;
    if(env == nil || normals == nil)
        return;

    Mtx envMtx;
    Mtx normalMtx;
    if(!buildEnvTexMatrix(env, envMtx))
        return;
    buildEnvNormalMatrix(modelView, normalMtx);

    if(!s_envUvStats.active || s_envUvStats.meshIndex != meshIndex ||
       s_envUvStats.passIndex != passIndex){
        envUvStatsStart(meshIndex, passIndex);
    }

    for(uint32 i = 0; i < numIdx; i++){
        uint16 vi = meshIdx[i];
        const V3d &n = normals[vi];
        float x = normalMtx[0][0] * n.x + normalMtx[0][1] * n.y + normalMtx[0][2] * n.z;
        float y = normalMtx[1][0] * n.x + normalMtx[1][1] * n.y + normalMtx[1][2] * n.z;
        float z = normalMtx[2][0] * n.x + normalMtx[2][1] * n.y + normalMtx[2][2] * n.z;
        float tu = envMtx[0][0] * x + envMtx[0][1] * y + envMtx[0][2] * z + envMtx[0][3];
        float tv = envMtx[1][0] * x + envMtx[1][1] * y + envMtx[1][2] * z + envMtx[1][3];
        envUvStatsAdd(tu, tv);
    }
    envUvStatsFinish(mat, env->tex);
}

static GxRaster*
getNativeRaster(Texture *texture)
{
    if(texture == nil || texture->raster == nil)
        return nil;
    return PLUGINOFFSET(GxRaster, texture->raster, nativeRasterOffset);
}

static const MatFX::Env*
getEnvMap(Material *mat)
{
    if(mat == nil)
        return nil;
    MatFX *matfx = MatFX::get(mat);
    if(matfx == nil || matfx->type != MatFX::ENVMAP)
        return nil;

    int32 index = matfx->getEffectIndex(MatFX::ENVMAP);
    if(index < 0)
        return nil;
    return &matfx->fx[index].env;
}

static float
clamp01(float value)
{
    if(value < 0.0f)
        return 0.0f;
    if(value > 1.0f)
        return 1.0f;
    return value;
}

bool
gxMatFXEnvReady(Material *mat, bool hasNormals)
{
    if(mat == nil)
        return false;

    MatFX *matfx = MatFX::get(mat);
    if(matfx == nil || matfx->type != MatFX::ENVMAP)
        return false;

    const MatFX::Env *env = getEnvMap(mat);
    Texture *envTex = env ? env->tex : nil;
    GxRaster *raster = envTex ? getNativeRaster(envTex) : nil;
    float intensity = clamp01(matFXEnvMapIntensity);
    float effective = env != nil ? env->coefficient * intensity : 0.0f;
    bool ready = hasNormals && env != nil && envTex != nil &&
                 effective > 0.0f && raster != nil &&
                 raster->texObjValid;

    if(matFxTraceReadyOnce(mat, envTex)){
        const char *reason = ready ? "ready" :
            !hasNormals ? "no-normals" :
            env == nil ? "no-env-record" :
            envTex == nil ? "no-env-texture" :
            effective <= 0.0f ? "nonpositive-effective" :
            raster == nil ? "no-native-raster" : "invalid-texobj";
        fprintf(stdout,
                "[GX-MATFX-TRACE] ready=%d reason=%s mat=%p matfx=%p "
                "type=%s hasNormals=%d env=%p envTex=%p envName=%s "
                "coefRaw=%.5f intensity=%.5f coefEff=%.5f fbAlpha=%d frame=%p matRGBA=%u,%u,%u,%u "
                "globals=light:%d matColor:%d flipU:%d\n",
                ready ? 1 : 0,
                reason,
                (void*)mat,
                (void*)matfx,
                matFxTypeName(matfx->type),
                hasNormals ? 1 : 0,
                (void*)env,
                (void*)envTex,
                envTex && envTex->name[0] ? envTex->name : "<none>",
                env ? (double)env->coefficient : 0.0,
                (double)intensity,
                (double)effective,
                env && env->fbAlpha ? 1 : 0,
                env ? (void*)env->frame : nil,
                (unsigned)mat->color.red,
                (unsigned)mat->color.green,
                (unsigned)mat->color.blue,
                (unsigned)mat->color.alpha,
                MatFX::envMapApplyLight ? 1 : 0,
                MatFX::envMapUseMatColor ? 1 : 0,
                MatFX::envMapFlipU ? 1 : 0);
        matFxTraceRaster("env", envTex, raster);
        if(mat->texture)
            matFxTraceRaster("base", mat->texture,
                             getNativeRaster(mat->texture));
    }

    return ready;
}

bool
gxMatFXEnvUsesAlpha(Material *mat)
{
    const MatFX::Env *env = getEnvMap(mat);
    // Match the D3D MatFX pass: source-alpha blending is selected only for
    // the explicit framebuffer-alpha material flag. A diffuse texture's
    // alpha does not switch the environment pass away from additive blending.
    return env != nil && env->fbAlpha != 0;
}

static uint8
colorByte(float value)
{
    if(value <= 0.0f)
        return 0;
    if(value >= 1.0f)
        return 255;
    return (uint8)(value * 255.0f + 0.5f);
}

static void
loadEnvTexMatrix(const MatFX::Env *env, const Mtx modelView)
{
    Mtx envMtx;
    Mtx normalMtx;
    bool trace = matFxTraceMatrixOnce(env);

    bool normalExact = buildEnvNormalMatrix(modelView, normalMtx);
    GX_LoadTexMtxImm(normalMtx, GX_TEXMTX0, GX_MTX3x4);

    if(!buildEnvTexMatrix(env, envMtx))
        return;
    GX_LoadTexMtxImm(envMtx, GX_DTTMTX0, GX_MTX3x4);

    if(trace){
        matFxTraceMatrix(normalExact ? "env-normal-inv-transpose" :
                         "env-normal-fallback", env, normalMtx);
        matFxTraceMatrix("env-post", env, envMtx);
    }
}

static GXColor
getEnvColor(Material *mat, const MatFX::Env *env,
            bool modulateMaterialColor)
{
    // D3D's setMaterial(flags, ...) uses white when the geometry does not
    // request material-colour modulation. Keep the explicit envMapColor
    // override only for the compatibility mode that disables that mapping.
    static const RGBA white = { 255, 255, 255, 255 };
    const RGBA &color = MatFX::envMapUseMatColor ?
                        (modulateMaterialColor ? mat->color : white) :
                        MatFX::envMapColor;
    float coefficient = env->coefficient * clamp01(matFXEnvMapIntensity);
    GXColor result = {
        colorByte((float)color.red / 255.0f * coefficient),
        colorByte((float)color.green / 255.0f * coefficient),
        colorByte((float)color.blue / 255.0f * coefficient),
        (uint8)(mat != nil ? mat->color.alpha : 255)
    };
    return result;
}

static void
setEnvTextureStage(uint8 stage)
{
    GX_SetTevOrder(stage, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
    GX_SetTevKColorSel(stage, GX_TEV_KCSEL_K0);
    GX_SetTevColorIn(stage,
                     GX_CC_ZERO, GX_CC_KONST,
                     GX_CC_TEXC, GX_CC_ZERO);
    GX_SetTevColorOp(stage,
                     GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                     GX_TRUE, GX_TEVPREV);
    GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_K0_A);
    GX_SetTevAlphaIn(stage,
                     GX_CA_ZERO, GX_CA_TEXA,
                     GX_CA_KONST, GX_CA_ZERO);
    GX_SetTevAlphaOp(stage,
                     GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                     GX_TRUE, GX_TEVPREV);
}

bool
gxMatFXSetupEnv(Material *mat, bool baseTextured, bool vertexAlpha,
                bool modulateMaterialColor, const Mtx modelView)
{
    const MatFX::Env *env = getEnvMap(mat);
    if(env == nil || env->tex == nil)
        return false;

    GxRaster *envRaster = getNativeRaster(env->tex);
    if(envRaster == nil || !envRaster->texObjValid)
        return false;

    GXColor envColor = getEnvColor(mat, env, modulateMaterialColor);
    GX_SetTevKColor(GX_KCOLOR0, envColor);
    gxSetTexture(env->tex->raster, 1);

    GX_SetNumTexGens(2);
    if(baseTextured){
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4,
                          GX_TG_TEX0, GX_IDENTITY);
    }else{
        // TEXCOORD0 is unused by the solid base pass, but GX requires all
        // generated coordinates below GX_SetNumTexGens(2) to be defined.
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4,
                          GX_TG_POS, GX_IDENTITY);
    }
    loadEnvTexMatrix(env, modelView);
    GX_SetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4,
                       GX_TG_NRM, GX_TEXMTX0,
                       GX_TRUE, GX_DTTMTX0);

    // The effect is drawn after the base material. Keep this pass deliberately
    // small: env texture * K0, with blending/depth policy owned by gxpipe.
    // This avoids carrying the base pass's TEV registers across a primitive.
    setEnvTextureStage(GX_TEVSTAGE0);
    uint8 stageCount = 1;
    if(MatFX::envMapApplyLight){
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL,
                       GX_TEXMAP_NULL, GX_COLOR1A1);
        GX_SetTevColorIn(GX_TEVSTAGE1,
                         GX_CC_ZERO, GX_CC_CPREV,
                         GX_CC_RASC, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE1,
                         GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                         GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1,
                         GX_CA_ZERO, GX_CA_ZERO,
                         GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1,
                         GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                         GX_TRUE, GX_TEVPREV);
        stageCount++;
    }
    // The D3D framebuffer-alpha path samples the environment texture after
    // replacing texture unit 0; diffuse/base alpha is not part of this pass.
    if(vertexAlpha){
        GX_SetTevOrder(stageCount, GX_TEXCOORDNULL,
                       GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevColorIn(stageCount,
                         GX_CC_ZERO, GX_CC_ZERO,
                         GX_CC_ZERO, GX_CC_CPREV);
        GX_SetTevColorOp(stageCount,
                         GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                         GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(stageCount,
                         GX_CA_ZERO, GX_CA_APREV,
                         GX_CA_RASA, GX_CA_ZERO);
        GX_SetTevAlphaOp(stageCount,
                         GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                         GX_TRUE, GX_TEVPREV);
        stageCount++;
    }
    GX_SetNumTevStages(stageCount);

    if(s_matFxSetupTraceCount < GX_MATFX_TRACE_LIMIT){
        s_matFxSetupTraceCount++;
        fprintf(stdout,
                "[GX-MATFX-TRACE] setup mat=%p envTex=%p name=%s "
                "baseTextured=%d vertexAlpha=%d intensity=%.3f "
                "envColor=%u,%u,%u,%u "
                "applyLight=%d matColor=%d flipU=%d fbAlpha=%d usesAlpha=%d "
                "stages=%u\n",
                (void*)mat,
                (void*)env->tex,
                env->tex->name[0] ? env->tex->name : "<unnamed>",
                baseTextured ? 1 : 0,
                vertexAlpha ? 1 : 0,
                (double)clamp01(matFXEnvMapIntensity),
                (unsigned)envColor.r,
                (unsigned)envColor.g,
                (unsigned)envColor.b,
                (unsigned)envColor.a,
                MatFX::envMapApplyLight ? 1 : 0,
                MatFX::envMapUseMatColor ? 1 : 0,
                MatFX::envMapFlipU ? 1 : 0,
                env->fbAlpha ? 1 : 0,
                gxMatFXEnvUsesAlpha(mat) ? 1 : 0,
                (unsigned)stageCount);
    }
    return true;
}

} // namespace gx

static void*
matfxOpen(void *o, int32, int32)
{
    matFXGlobals.pipelines[PLATFORM_GX] = makeMatFXPipeline();
    printf("[GX-MATFX] open: fallback pipeline=%p\n",
           (void*)matFXGlobals.pipelines[PLATFORM_GX]);
    return o;
}

static void*
matfxClose(void *o, int32, int32)
{
    if(matFXGlobals.pipelines[PLATFORM_GX]){
        ((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_GX])->destroy();
        matFXGlobals.pipelines[PLATFORM_GX] = nil;
    }
    return o;
}

void
initMatFX(void)
{
    Driver::registerPlugin(PLATFORM_GX, 0, ID_MATFX,
                            matfxOpen, matfxClose);
}

ObjPipeline*
makeMatFXPipeline(void)
{
    ObjPipeline *pipe = makeDefaultPipeline();
    pipe->pluginID = ID_MATFX;
    pipe->pluginData = 0;
    return pipe;
}

} // namespace rw

#endif // GAMECUBE
