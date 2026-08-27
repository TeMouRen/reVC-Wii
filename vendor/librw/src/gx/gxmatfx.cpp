// gxmatfx.cpp -- GX MatFX pipeline boundary
//
// The GX MatFX path currently implements the environment-map effect as a
// separate additive pass. The object pipe owns the base material draw and
// state restoration; this file only owns the effect resource lookup, normal
// reflection texgen, and effect TEV setup.

#ifdef GAMECUBE

#include <stdio.h>

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

namespace rw {
namespace gx {

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

bool
gxMatFXEnvReady(Material *mat, bool hasNormals)
{
    if(mat == nil || !hasNormals)
        return false;

    const MatFX::Env *env = getEnvMap(mat);
    if(env == nil || env->tex == nil || env->coefficient <= 0.0f)
        return false;

    GxRaster *raster = getNativeRaster(env->tex);
    return raster != nil && raster->texObjValid;
}

bool
gxMatFXEnvUsesAlpha(Material *mat)
{
    const MatFX::Env *env = getEnvMap(mat);
    if(env == nil)
        return false;
    if(env->fbAlpha != 0)
        return true;

    if(mat == nil || mat->texture == nil || mat->texture->raster == nil)
        return false;
    GxRaster *baseRaster = getNativeRaster(mat->texture);
    return baseRaster != nil && baseRaster->hasAlpha != 0;
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
loadEnvTexMatrix(const MatFX::Env *env)
{
    Mtx mapMtx;

    guMtxIdentity(mapMtx);
    mapMtx[0][0] = MatFX::envMapFlipU ? -0.5f : 0.5f;
    mapMtx[0][3] = 0.5f;
    mapMtx[1][1] = -0.5f;
    mapMtx[1][3] = 0.5f;

    if(env->frame == nil){
        // A nil frame means the camera frame in the reference backends.
        // GX_NRM is already in that frame, so only apply the reflection map.
        GX_LoadTexMtxImm(mapMtx, GX_TEXMTX0, GX_MTX3x4);
        return;
    }

    Mtx frameMtx;
    Mtx invFrame;
    Mtx invView;
    Mtx normalMtx;
    Mtx envMtx;
    rwMatToGxMtx(frameMtx, env->frame->getLTM());
    if(!guMtxInverse(frameMtx, invFrame)){
        // GX already receives normals in view space. A singular effect
        // frame falls back to the plain normal-to-UV map.
        GX_LoadTexMtxImm(mapMtx, GX_TEXMTX0, GX_MTX3x4);
        return;
    }

    if(!guMtxInverse(gxInvCamLTM, invView))
        guMtxIdentity(invView);

    // Normals are vectors, not positions. Match the reference backends by
    // discarding the frame translation before projection.
    invFrame[0][3] = 0.0f;
    invFrame[1][3] = 0.0f;
    invFrame[2][3] = 0.0f;
    guMtxConcat(invFrame, invView, normalMtx);
    guMtxConcat(mapMtx, normalMtx, envMtx);
    GX_LoadTexMtxImm(envMtx, GX_TEXMTX0, GX_MTX3x4);
}

static GXColor
getEnvColor(Material *mat, const MatFX::Env *env)
{
    const RGBA &color = MatFX::envMapUseMatColor ?
                        mat->color : MatFX::envMapColor;
    float coefficient = env->coefficient;
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
gxMatFXSetupEnv(Material *mat, bool baseTextured, bool vertexAlpha)
{
    const MatFX::Env *env = getEnvMap(mat);
    if(env == nil || env->tex == nil)
        return false;

    GxRaster *envRaster = getNativeRaster(env->tex);
    if(envRaster == nil || !envRaster->texObjValid)
        return false;

    GXColor envColor = getEnvColor(mat, env);
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
    loadEnvTexMatrix(env);
    GX_SetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4,
                       GX_TG_NRM, GX_TEXMTX0,
                       GX_FALSE, GX_DTTIDENTITY);

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
    if(gxMatFXEnvUsesAlpha(mat) && baseTextured){
        // Match RenderWare's framebuffer-alpha mode and ordinary textured
        // alpha by masking the effect alpha with the base texture alpha. The
        // color result remains the environment texture.
        GX_SetTevOrder(stageCount, GX_TEXCOORD0,
                       GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevColorIn(stageCount,
                         GX_CC_ZERO, GX_CC_ZERO,
                         GX_CC_ZERO, GX_CC_CPREV);
        GX_SetTevColorOp(stageCount,
                         GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                         GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(stageCount,
                         GX_CA_ZERO, GX_CA_TEXA,
                         GX_CA_APREV, GX_CA_ZERO);
        GX_SetTevAlphaOp(stageCount,
                         GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                         GX_TRUE, GX_TEVPREV);
        stageCount++;
    }
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

    printf("[GX-MATFX] env tex=%s coef=%.3f applyLight=%d matColor=%d flipU=%d fbAlpha=%d stages=%u\n",
           env->tex->name ? env->tex->name : "<unnamed>",
           (double)env->coefficient,
           MatFX::envMapApplyLight ? 1 : 0,
           MatFX::envMapUseMatColor ? 1 : 0,
           MatFX::envMapFlipU ? 1 : 0,
           env->fbAlpha ? 1 : 0,
           (unsigned)stageCount);
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
