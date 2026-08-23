// gxpipe.cpp �?GX Default ObjPipeline (opaque 3D geometry)
//
// Handles non-skinned, non-MatFX atomic rendering:
// buildings, terrain, props, weapons.
//
// 2026-06-09: Switched from GX_VTXFMT1 indexed to GX_VTXFMT0 GX_DIRECT.
// GX_VTXFMT1 + GX_INDEX16 + GX_SetArray produces zero pixels on EFB.
// GX_DIRECT (same as gxskin.cpp + gxim2d.cpp) is proven working.
//
// References: Pikmin dgxGraphics.cpp, Octave BindMaterial/ConfigTev, GL3 gl3pipe.cpp

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <gccore.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "rwgx.h"
#ifdef GX_PIPELINE_DIAGNOSTICS
#include "gxmemory.h"
#endif

// Define GX_PIPELINE_DIAGNOSTICS when targeted pipeline tracing is needed.
#ifndef GX_PIPELINE_DIAGNOSTICS
#define printf(...) ((void)sizeof((::printf)(__VA_ARGS__)))
#endif

namespace rw {
namespace gx {

struct GxAtomicLights
{
    RGBAf ambient;
    Light *directionals[8];
    Light *locals[8];
    int32 numDirectionals;
    int32 numLocals;
    bool hasNormals;
};

static uint8
lightColorByte(float value)
{
    if(value <= 0.0f)
        return 0;
    if(value >= 1.0f)
        return 255;
    return (uint8)(value * 255.0f + 0.5f);
}

static void
gatherAtomicLights(Atomic *atomic, GxAtomicLights &lights)
{
    memset(&lights, 0, sizeof(lights));
    lights.ambient.alpha = 1.0f;

    if(atomic == nil || atomic->geometry == nil ||
       engine->currentWorld == nil)
        return;

    lights.hasNormals =
        (atomic->geometry->flags & Geometry::NORMALS) != 0;
    const bool dynamicLighting =
        (atomic->geometry->flags & Geometry::LIGHT) != 0;
    if(!dynamicLighting)
        return;

    WorldLights worldLights;
    worldLights.directionals = lights.directionals;
    worldLights.numDirectionals = 8;
    worldLights.locals = lights.locals;
    worldLights.numLocals = 8;
    ((World*)engine->currentWorld)->enumerateLights(atomic, &worldLights);

    // Diffuse lights require normals. LIGHT geometries without normals keep
    // only the ambient term, matching the GX2/GL3 reference pipelines.
    if(!lights.hasNormals){
        worldLights.numDirectionals = 0;
        worldLights.numLocals = 0;
    }

    lights.ambient = worldLights.ambient;
    lights.numDirectionals = worldLights.numDirectionals;
    lights.numLocals = worldLights.numLocals;

    static int s_lightDiagCount = 0;
    if(s_lightDiagCount < 24){
        printf("[GX-LIGHT] flags=0x%X ambient=%.3f,%.3f,%.3f dirs=%d locals=%d normals=%d\n",
               (unsigned)atomic->geometry->flags,
               (double)lights.ambient.red,
               (double)lights.ambient.green,
               (double)lights.ambient.blue,
               lights.numDirectionals,
               lights.numLocals,
               lights.hasNormals ? 1 : 0);
        s_lightDiagCount++;
    }
}

static GXColor
gxLightColor(const Light *light, float diffuseScale)
{
    GXColor color = {
        lightColorByte(light->color.red * diffuseScale),
        lightColorByte(light->color.green * diffuseScale),
        lightColorByte(light->color.blue * diffuseScale),
        255
    };
    return color;
}

static bool
normalizeViewVector(guVector &vector)
{
    float lenSq = vector.x * vector.x +
                  vector.y * vector.y +
                  vector.z * vector.z;
    if(lenSq <= 0.000001f)
        return false;
    float invLen = 1.0f / sqrtf(lenSq);
    vector.x *= invLen;
    vector.y *= invLen;
    vector.z *= invLen;
    return true;
}

static u32
loadGxLights(const GxAtomicLights &lights, float diffuseScale)
{
    u32 lightMask = GX_LIGHTNULL;
    int32 loaded = 0;

    for(int32 i = 0; i < lights.numDirectionals && loaded < 8; i++){
        Light *light = lights.directionals[i];
        if(light == nil || light->getFrame() == nil)
            continue;

        const Matrix *lightLtm = light->getFrame()->getLTM();
        guVector viewToLight = {
            -lightLtm->at.x,
            -lightLtm->at.y,
            -lightLtm->at.z
        };
        guVector transformed;
        guVecMultiplySR(gxInvCamLTM, &viewToLight, &transformed);
        if(!normalizeViewVector(transformed))
            continue;

        GXLightObj lightObj;
        memset(&lightObj, 0, sizeof(lightObj));
        GX_InitLightPos(&lightObj,
                        transformed.x * 100000.0f,
                        transformed.y * 100000.0f,
                        transformed.z * 100000.0f);
        GX_InitLightSpot(&lightObj, 0.0f, GX_SP_OFF);
        GX_InitLightDistAttn(&lightObj, 0.0f, 0.0f, GX_DA_OFF);
        GX_InitLightColor(&lightObj, gxLightColor(light, diffuseScale));

        u8 lightId = (u8)(GX_LIGHT0 << loaded);
        GX_LoadLightObj(&lightObj, lightId);
        lightMask |= lightId;
        loaded++;
    }

    for(int32 i = 0; i < lights.numLocals && loaded < 8; i++){
        Light *light = lights.locals[i];
        if(light == nil || light->getFrame() == nil)
            continue;

        const Matrix *lightLtm = light->getFrame()->getLTM();
        guVector worldPosition = {
            lightLtm->pos.x,
            lightLtm->pos.y,
            lightLtm->pos.z
        };
        guVector viewPosition;
        guVecMultiply(gxInvCamLTM, &worldPosition, &viewPosition);

        GXLightObj lightObj;
        memset(&lightObj, 0, sizeof(lightObj));
        GX_InitLightPos(&lightObj,
                        viewPosition.x,
                        viewPosition.y,
                        viewPosition.z);
        GX_InitLightColor(&lightObj, gxLightColor(light, diffuseScale));

        float radius = light->radius > 0.001f ? light->radius : 0.001f;
        GX_InitLightDistAttn(&lightObj, radius, 0.01f, GX_DA_MEDIUM);

        if(light->getType() == Light::SPOT ||
           light->getType() == Light::SOFTSPOT){
            guVector worldDirection = {
                lightLtm->at.x,
                lightLtm->at.y,
                lightLtm->at.z
            };
            guVector viewDirection;
            guVecMultiplySR(gxInvCamLTM, &worldDirection, &viewDirection);
            if(normalizeViewVector(viewDirection)){
                GX_InitLightDir(&lightObj,
                                viewDirection.x,
                                viewDirection.y,
                                viewDirection.z);
                float cutoff = light->getAngle() * (180.0f / 3.14159265358979323846f);
                if(cutoff < 0.1f)
                    cutoff = 0.1f;
                if(cutoff > 90.0f)
                    cutoff = 90.0f;
                GX_InitLightSpot(&lightObj, cutoff,
                                 light->getType() == Light::SOFTSPOT ?
                                     GX_SP_COS2 : GX_SP_FLAT);
            }else
                GX_InitLightSpot(&lightObj, 0.0f, GX_SP_OFF);
        }else{
            GX_InitLightSpot(&lightObj, 0.0f, GX_SP_OFF);
        }

        u8 lightId = (u8)(GX_LIGHT0 << loaded);
        GX_LoadLightObj(&lightObj, lightId);
        lightMask |= lightId;
        loaded++;
    }

    return lightMask;
}

static void
setupLightingChannels(Material *mat, const GxAtomicLights &lights)
{
    float ambientScale = mat ? mat->surfaceProps.ambient : 1.0f;
    float diffuseScale = mat ? mat->surfaceProps.diffuse : 1.0f;
    GXColor ambientColor = {
        lightColorByte(lights.ambient.red * ambientScale),
        lightColorByte(lights.ambient.green * ambientScale),
        lightColorByte(lights.ambient.blue * ambientScale),
        255
    };
    GXColor white = {255, 255, 255, 255};

    // Channel 0 carries the PRELIT vertex color. Channel 1 is only used for
    // dynamic diffuse lighting; ambient is added explicitly by the TEV so
    // LIGHT meshes without normals do not depend on a disabled GX channel.
    GX_SetChanMatColor(GX_COLOR0A0, white);
    GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE,
                   GX_SRC_REG, GX_SRC_VTX,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

    GX_SetChanAmbColor(GX_COLOR1A1, ambientColor);
    u32 lightMask = loadGxLights(lights, diffuseScale);
    if(lights.hasNormals){
        GX_SetChanMatColor(GX_COLOR1A1, white);
        GX_SetChanCtrl(GX_COLOR1, GX_TRUE,
                       GX_SRC_REG, GX_SRC_REG,
                       lightMask, GX_DF_CLAMP,
                       lights.numLocals > 0 ? GX_AF_SPOT : GX_AF_NONE);
    }else{
        // With lighting disabled GX passes the channel material color through.
        // Feed ambient through channel 1 so no-normal meshes use the same TEV
        // prelight + lighting path as meshes with normals.
        GX_SetChanMatColor(GX_COLOR1A1, ambientColor);
        GX_SetChanCtrl(GX_COLOR1A1, GX_FALSE,
                       GX_SRC_REG, GX_SRC_REG,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    }
    GX_SetChanCtrl(GX_ALPHA1, GX_FALSE,
                   GX_SRC_REG, GX_SRC_REG,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumChans(2);
}

static void
setupDefaultLitTev(GXColor matColor, bool textured, bool saveForMatFX)
{
    GX_SetTevColor(GX_TEVREG1, matColor);
    GX_SetNumTevStages(textured ? 4 : 3);

    // GX2: prelight + ambient/diffuse, then material color, then texture.
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL,
                   GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

    GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL,
                   GX_TEXMAP_NULL, GX_COLOR1A1);
    GX_SetTevColorIn(GX_TEVSTAGE1,
                     GX_CC_ZERO, GX_CC_RASC,
                     GX_CC_ONE, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE1,
                     GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                     GX_TRUE, saveForMatFX ? GX_TEVREG0 : GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE1,
                     GX_CA_ZERO, GX_CA_ZERO,
                     GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE1,
                     GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                     GX_TRUE, saveForMatFX ? GX_TEVREG0 : GX_TEVPREV);

    GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL,
                   GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE2,
                     GX_CC_ZERO, saveForMatFX ? GX_CC_C0 : GX_CC_CPREV,
                     GX_CC_C1, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE2,
                     GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                     GX_TRUE, saveForMatFX ? GX_TEVREG2 : GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE2,
                     GX_CA_ZERO, saveForMatFX ? GX_CA_A0 : GX_CA_APREV,
                     GX_CA_A1, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE2,
                     GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                     GX_TRUE, saveForMatFX ? GX_TEVREG2 : GX_TEVPREV);

    if(textured){
        GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD0,
                       GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE3,
                         GX_CC_ZERO, saveForMatFX ? GX_CC_C2 : GX_CC_CPREV,
                         GX_CC_TEXC, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE3,
                         GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                         GX_TRUE, saveForMatFX ? GX_TEVREG2 : GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE3,
                         GX_CA_ZERO, saveForMatFX ? GX_CA_A2 : GX_CA_APREV,
                         GX_CA_TEXA, GX_CA_ZERO);
        GX_SetTevAlphaOp(GX_TEVSTAGE3,
                         GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
                         GX_TRUE, saveForMatFX ? GX_TEVREG2 : GX_TEVPREV);
    }
}

static bool
pipeNeedsTrace(Geometry *geo)
{
    (void)geo;
    return false;
}

#ifdef GX_PIPELINE_DIAGNOSTICS

struct GxSolidDiagState
{
    const char *reason;
    Texture *texture;
    Raster *raster;
    GxRaster *natras;
    void *texObjData;
    uint16 texObjWidth;
    uint16 texObjHeight;
    uint8 texObjFmt;
    bool passClr;
};

static GxSolidDiagState
classifySolidFallback(Material *mat)
{
    GxSolidDiagState state;
    state.reason = "no-texture";
    state.texture = nil;
    state.raster = nil;
    state.natras = nil;
    state.texObjData = nil;
    state.texObjWidth = 0;
    state.texObjHeight = 0;
    state.texObjFmt = 0xFF;
    state.passClr = true;

    if(mat == nil || mat->texture == nil)
        return state;

    state.texture = mat->texture;
    state.raster = mat->texture->raster;
    if(state.raster == nil) {
        state.reason = "no-raster";
        return state;
    }

    state.natras = PLUGINOFFSET(GxRaster, state.raster, nativeRasterOffset);
    if(state.natras == nil) {
        state.reason = "no-natras";
        return state;
    }

    if(!state.natras->texObjValid) {
        state.reason = "texobj-invalid";
        return state;
    }

    state.passClr = false;

    state.texObjData = GX_GetTexObjData(&state.natras->texObj);
    state.texObjWidth = GX_GetTexObjWidth(&state.natras->texObj);
    state.texObjHeight = GX_GetTexObjHeight(&state.natras->texObj);
    state.texObjFmt = (uint8)GX_GetTexObjFmt(&state.natras->texObj);
    if(state.natras->gxData == nil || state.natras->dataSize == 0) {
        state.reason = "no-gxdata";
        return state;
    }
    if((uintptr_t)state.texObjData !=
       (uintptr_t)MEM_VIRTUAL_TO_PHYSICAL(state.natras->gxData)) {
        state.reason = "texobj-data-mismatch";
        return state;
    }
    if(state.texObjWidth != state.natras->w ||
       state.texObjHeight != state.natras->h) {
        state.reason = "texobj-size-mismatch";
        return state;
    }
    if(state.texObjFmt != state.natras->gxFmt) {
        state.reason = "texobj-format-mismatch";
        return state;
    }

    state.reason = nil;
    return state;
}

static void
logSolidFallback(const char *pipeline, Atomic *atomic, Geometry *geo,
                 uint32 meshIndex, uint32 passIndex, Material *mat,
                 const GxSolidDiagState &diag, GXColor matColor,
                 bool vertexAlpha, bool hasVertexColors,
                 bool modulateMaterialColor, uint32 numIndices)
{
    // Missing-material meshes are handled by gxShouldSkipUnresolvedTexturedMesh.
    // Keep this diagnostic reserved for actual GX texture-object faults.
    if(mat != nil && gxGetMissingMaterialDiag(mat, nil))
        return;
    const bool ordinaryUntextured = mat == nil || mat->texture == nil;
    if(ordinaryUntextured)
        return;

    struct SeenKey {
        const void *geo;
        const void *mat;
        const void *tex;
        const char *reason;
        uint32 shrink;
        uint32 compaction;
    };
    static SeenKey seen[96];
    static uint32 seenCount = 0;
    static uint32 seenNext = 0;
    static uint32 windowStartFrame = 0;
    static uint32 windowPassClrCount = 0;
    static uint32 windowFaultCount = 0;

    const void *texKey = diag.texture;
    const uint32 shrink = gxMemGetShrinkTotalCount();
    const uint32 compaction = gxMemGetCompactionGeneration();
    for(uint32 i = 0; i < seenCount; i++) {
        if(seen[i].geo == geo &&
           seen[i].mat == mat &&
           seen[i].tex == texKey &&
           seen[i].reason == diag.reason &&
           seen[i].shrink == shrink &&
           seen[i].compaction == compaction)
            return;
    }

    if(windowStartFrame == 0 ||
       (uint32)(gxFrameNum - windowStartFrame) >= 150u) {
        windowStartFrame = gxFrameNum;
        windowPassClrCount = 0;
        windowFaultCount = 0;
    }
    uint32 *windowCount = diag.passClr ? &windowPassClrCount : &windowFaultCount;
    const uint32 windowLimit = diag.passClr ? 6u : 16u;
    if(*windowCount >= windowLimit)
        return;
    (*windowCount)++;

    uint32 seenIndex;
    if(seenCount < (sizeof(seen) / sizeof(seen[0]))) {
        seenIndex = seenCount++;
    } else {
        seenIndex = seenNext++ % (sizeof(seen) / sizeof(seen[0]));
    }
    seen[seenIndex].geo = geo;
    seen[seenIndex].mat = mat;
    seen[seenIndex].tex = texKey;
    seen[seenIndex].reason = diag.reason;
    seen[seenIndex].shrink = shrink;
    seen[seenIndex].compaction = compaction;

    const Matrix *ltm = (atomic && atomic->getFrame()) ? atomic->getFrame()->getLTM() : nil;
    const char *texName = diag.texture ? diag.texture->name : nil;
    const char *maskName = diag.texture ? diag.texture->mask : nil;
    const uintptr_t expectedTexObjData = diag.natras && diag.natras->gxData ?
        (uintptr_t)MEM_VIRTUAL_TO_PHYSICAL(diag.natras->gxData) : 0u;
    fprintf(stdout,
           "%s pipe=%s path=%s reason=%s serial=%u modelId=%d model=%s txdSlot=%d txd=%s reqTex='%s' reqMask='%s' aliases=%u frame=%u gen=%u shrink=%u atomic=%p geo=%p mesh=%u pass=%u mat=%p tex=%p raster=%p natras=%p gx=%p objGx=%p objMatch=%d texObj=%d texRef=%d texName=%s mask=%s idx=%u rgba=%u,%u,%u,%u vtxA=%d prelit=%d mod=%d pos=%.3f,%.3f,%.3f fmt=0x%02X objFmt=0x%02X size=%u wh=%ux%u objWh=%ux%u hasA=%u aKind=%u\n",
           "[GX-TEXOBJ-FAULT]",
           pipeline,
           diag.passClr ? "passclr" : "textured",
           diag.reason,
           0u, -1, "<unknown>", -1, "<unknown>", "", "", 0u,
           gxFrameNum,
           compaction,
           shrink,
           (void*)atomic,
           (void*)geo,
           (unsigned)meshIndex,
           (unsigned)passIndex,
           (void*)mat,
           (void*)diag.texture,
           (void*)diag.raster,
           (void*)diag.natras,
           diag.natras ? diag.natras->gxData : nil,
           diag.texObjData,
           ((uintptr_t)diag.texObjData == expectedTexObjData) ? 1 : 0,
           (diag.natras && diag.natras->texObjValid) ? 1 : 0,
           diag.texture ? diag.texture->refCount : 0,
           texName ? texName : "none",
           maskName ? maskName : "none",
           (unsigned)numIndices,
           (unsigned)matColor.r, (unsigned)matColor.g,
           (unsigned)matColor.b, (unsigned)matColor.a,
           vertexAlpha ? 1 : 0,
           hasVertexColors ? 1 : 0,
           modulateMaterialColor ? 1 : 0,
           ltm ? (double)ltm->pos.x : 0.0,
           ltm ? (double)ltm->pos.y : 0.0,
           ltm ? (double)ltm->pos.z : 0.0,
           diag.natras ? (unsigned)diag.natras->gxFmt : 0xFFu,
           (unsigned)diag.texObjFmt,
           diag.natras ? (unsigned)diag.natras->dataSize : 0u,
           diag.natras ? (unsigned)diag.natras->w : 0u,
           diag.natras ? (unsigned)diag.natras->h : 0u,
           (unsigned)diag.texObjWidth,
           (unsigned)diag.texObjHeight,
           diag.natras ? (unsigned)diag.natras->hasAlpha : 0u,
           diag.natras ? (unsigned)diag.natras->alphaKind : 0u);
}

#endif // GX_PIPELINE_DIAGNOSTICS

// ── Per-geometry instance data ──────────────────────────────

enum {
    GX_INST_DEFAULT = 0x47584446, // GXDF
    GX_INST_SKIN    = 0x4758534B  // GXSK
};

static bool
startsWith(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static char
lowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool
nameContainsNoCase(const char *name, const char *needle)
{
    if(name == nil || needle == nil || needle[0] == '\0')
        return false;

    for(const char *p = name; *p; p++){
        const char *a = p;
        const char *b = needle;
        while(*a && *b && lowerAscii(*a) == lowerAscii(*b)){
            a++;
            b++;
        }
        if(*b == '\0')
            return true;
    }
    return false;
}

static bool
alphaTraceTexture(const char *name)
{
    return nameContainsNoCase(name, "window") ||
           nameContainsNoCase(name, "glass") ||
           nameContainsNoCase(name, "mesh") ||
           nameContainsNoCase(name, "fence") ||
           nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "plant") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "shadow") ||
           nameContainsNoCase(name, "wire") ||
           nameContainsNoCase(name, "rotor") ||
           nameContainsNoCase(name, "propell") ||
           nameContainsNoCase(name, "blade") ||
           nameContainsNoCase(name, "fan") ||
           nameContainsNoCase(name, "sign");
}

static bool
isKnownHardAlphaVegetationTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return strcmp(name, "kbtree4_test") == 0 ||
           strcmp(name, "foliage256") == 0 ||
           strcmp(name, "newtreeleaves128") == 0 ||
           strcmp(name, "newtreeleavesb128") == 0 ||
           strcmp(name, "planta256") == 0 ||
           strcmp(name, "plantb256") == 0 ||
           strcmp(name, "plantc256") == 0 ||
           strcmp(name, "fuzzyplant256") == 0 ||
           strcmp(name, "kbplanter_plants1") == 0 ||
           strcmp(name, "weepalmshadow") == 0 ||
           strcmp(name, "bigpalmshadow") == 0;
}

static bool
isVegetationShadowTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return strcmp(name, "weepalmshadow") == 0 ||
           strcmp(name, "bigpalmshadow") == 0;
}

static bool
isKnownSoftBlendVegetationTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    // NOTE: these foliage atlases (kbtree4_test, foliage256, planta/b/c256,
    // fuzzyplant256, newtreeleaves128/b128, kbplanter_plants1) are RGBA8 with a
    // real 8-bit alpha mask (verified in generic_gx.txd). Routing them through
    // alpha BLEND disabled the alpha test AND z-write, which stacked the crossed
    // frond cards into translucent gray/black canopy blobs and, at distance,
    // collapsed them into thin vertical strips. They belong on the hard-alpha
    // CUTOUT path instead (see isKnownHardAlphaVegetationTexture), so this soft
    // -blend classifier now matches nothing. Kept as a hook for genuinely soft
    // /translucent vegetation should any ever be needed.
    (void)name;
    return false;
}

static bool
isKnownEdgeBlendCutoutVegetationTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return strcmp(name, "kbtree4_test") == 0 ||
           strcmp(name, "foliage256") == 0 ||
           strcmp(name, "newtreeleaves128") == 0 ||
           strcmp(name, "newtreeleavesb128") == 0 ||
           strcmp(name, "planta256") == 0 ||
           strcmp(name, "plantb256") == 0 ||
           strcmp(name, "plantc256") == 0 ||
           strcmp(name, "fuzzyplant256") == 0 ||
           strcmp(name, "kbplanter_plants1") == 0;
}

static bool isRoomShellTexture(const char *name);
static bool isRoomShellFloorTexture(const char *name);

static bool
isHotelDebugTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return startsWith(name, "htl_") ||
           startsWith(name, "ht_") ||
           startsWith(name, "hot_") ||
           startsWith(name, "mob_") ||
           startsWith(name, "nt_wall") ||
           startsWith(name, "nt_floor") ||
           startsWith(name, "nt_woodwall") ||
           nameContainsNoCase(name, "hotel") ||
           nameContainsNoCase(name, "lobby");
}

static bool
isHotelRoomShadowTexture(const char *name)
{
    // hotshad1/2/3 are translucent, pre-baked window-light overlays. Treating
    // their alpha as a cutout leaves the black shadow texels fully opaque.
    return nameContainsNoCase(name, "hotshad") ||
           nameContainsNoCase(name, "hotelshad");
}

static bool
pipeFocusTexture(const char *name)
{
    return isKnownHardAlphaVegetationTexture(name) ||
           isKnownEdgeBlendCutoutVegetationTexture(name) ||
           isKnownSoftBlendVegetationTexture(name) ||
           isHotelRoomShadowTexture(name) ||
           isHotelDebugTexture(name) ||
           isRoomShellTexture(name) ||
           isRoomShellFloorTexture(name) ||
           alphaTraceTexture(name) ||
           (name && (
                strcmp(name, "black") == 0 ||
               strcmp(name, "black64") == 0 ||
                strcmp(name, "Grass_128HV") == 0 ||
               strcmp(name, "Grass_64HV") == 0 ||
               strcmp(name, "hedgewall_64") == 0 ||
               strcmp(name, "lodhedgewall_64") == 0 ||
               strcmp(name, "hedge2_128") == 0 ||
               strcmp(name, "Bow_grass_gryard") == 0 ||
               strcmp(name, "Bow_church_grass_gen") == 0));
}

static bool
isLikelyThinTwoSidedTexture(const char *name)
{
    return nameContainsNoCase(name, "rotor") ||
           nameContainsNoCase(name, "propell") ||
           nameContainsNoCase(name, "blade") ||
           nameContainsNoCase(name, "fan");
}

static bool
isLikelyVegetationTwoSidedTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return isKnownHardAlphaVegetationTexture(name) ||
           strcmp(name, "kbtree3_test") == 0 ||
           strcmp(name, "palmbark128") == 0 ||
           strcmp(name, "kbbark_test1") == 0 ||
           strcmp(name, "newtreeleaves128") == 0 ||
           nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "frond") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "bush") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "plant") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "bark") ||
           nameContainsNoCase(name, "trunk") ||
           nameContainsNoCase(name, "branch");
}

static u8
gxCullFromState(void)
{
    switch(gxState.cullMode){
    case CULLNONE:  return GX_CULL_NONE;
    case CULLFRONT: return GX_CULL_FRONT;
    default:        return GX_CULL_BACK;
    }
}

static u8
flipCullMode(u8 cullMode)
{
    if(cullMode == GX_CULL_BACK)
        return GX_CULL_FRONT;
    if(cullMode == GX_CULL_FRONT)
        return GX_CULL_BACK;
    return GX_CULL_NONE;
}

static bool
isRoomShellTexture(const char *name)
{
    if(name == nil)
        return false;

    return startsWith(name, "nt_wall") ||
           startsWith(name, "nt_woodwall") ||
           startsWith(name, "roof") ||
           startsWith(name, "LODroof") ||
           startsWith(name, "corrRoof") ||
           startsWith(name, "dt_LODroof") ||
           startsWith(name, "lod_roof") ||
           startsWith(name, "lod_corr_roof") ||
           startsWith(name, "lod_odroof") ||
           startsWith(name, "lodsjmtexroof") ||
           startsWith(name, "sjmroof") ||
           strcmp(name, "mob_metal1") == 0 ||
           strcmp(name, "concretemanky") == 0 ||
           strcmp(name, "shutter_64") == 0 ||
           strcmp(name, "kbwood_panel4_256") == 0;
}

static bool
isRoomShellFloorTexture(const char *name)
{
    if(name == nil)
        return false;

    return startsWith(name, "nt_floor") ||
           nameContainsNoCase(name, "floor") ||
           nameContainsNoCase(name, "ground");
}

static bool
isLikelyRoomShellDoubleSided(const char *name)
{
    return isRoomShellTexture(name) || isRoomShellFloorTexture(name);
}

static bool
preferCutoutTextureAlpha(const char *name)
{
    if(name == nil)
        return false;

    if(isKnownSoftBlendVegetationTexture(name))
        return false;

    return nameContainsNoCase(name, "fence") ||
           nameContainsNoCase(name, "mesh") ||
           nameContainsNoCase(name, "wire") ||
           nameContainsNoCase(name, "grate") ||
           nameContainsNoCase(name, "grill") ||
           nameContainsNoCase(name, "gate") ||
           nameContainsNoCase(name, "chain") ||
           nameContainsNoCase(name, "ivy") ||
           nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "bush") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "plant") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "fern") ||
           nameContainsNoCase(name, "frond") ||
           nameContainsNoCase(name, "weed") ||
           nameContainsNoCase(name, "sign") ||
           isLikelyThinTwoSidedTexture(name);
}

static u8
gxAlphaFuncFromState(int32 f)
{
    switch(f){
    case ALPHAALWAYS:       return GX_ALWAYS;
    case ALPHAGREATEREQUAL: return GX_GEQUAL;
    case ALPHALESS:         return GX_LESS;
    default:                return GX_ALWAYS;
    }
}

struct GxInstanceData
{
    uint32  platform;
    uint32  pipeType;
    uint32  serialNum;
    uint32  numMeshes;
    uint32  totalVertices;
    uint32  totalIndices;

    uint8  *vertexBuffer;  // interleaved: pos+nrm+col+tex
    uint16 *indexBuffer;
    uint32  vertexStride;

    struct MeshData {
        uint32    numIndices;
        uint32    minVert;
        uint32    numVertices;
        uint32    indexOffset;
        bool32    vertexAlpha;
        Material *material;
    } *meshes;

    bool hasNormals;
    bool hasColors;
    uint32 numTexCoords;
};

static bool
meshIndicesValid(Geometry *geo, const uint16 *meshIdx, uint32 numIdx,
                 uint32 *badAt, uint16 *badVi)
{
    if(geo == nil || meshIdx == nil)
        return false;

    for(uint32 i = 0; i < numIdx; i++){
        uint16 vi = meshIdx[i];
        if(vi >= geo->numVertices){
            if(badAt)
                *badAt = i;
            if(badVi)
                *badVi = vi;
            return false;
        }
    }
    return true;
}

static bool32
meshHasVertexAlpha(Geometry *geo, const uint16 *meshIdx, uint32 numIndices)
{
    if(geo == nil || geo->colors == nil || (geo->flags & Geometry::PRELIT) == 0)
        return 0;

    for(uint32 i = 0; i < numIndices; i++){
        uint16 vi = meshIdx[i];
        if(vi >= geo->numVertices)
            continue;
        if(geo->colors[vi].alpha != 255)
            return 1;
    }
    return 0;
}


// ── Build interleaved vertex buffer from Morph target ───────

static void __attribute__((unused))
buildVertexBuffer(GxInstanceData *inst, Geometry *geo)
{
    MorphTarget  *morph = &geo->morphTargets[0];
    uint32  numV  = inst->totalVertices;
    uint32  str   = inst->vertexStride;
    uint8  *buf   = inst->vertexBuffer;

    for (uint32 i = 0; i < numV; i++) {
        uint8 *vtx = buf + i * str;
        uint32 off = 0;

        // Pos: F32×3
        memcpy(vtx + off, &morph->vertices[i].x, 12);
        off += 12;

        // Nrm: F32×3
        if (inst->hasNormals) {
            memcpy(vtx + off, &morph->normals[i].x, 12);
            off += 12;
        }

        // Color: RGBA8
        if (inst->hasColors) {
            vtx[off + 0] = geo->colors[i].red;
            vtx[off + 1] = geo->colors[i].green;
            vtx[off + 2] = geo->colors[i].blue;
            vtx[off + 3] = geo->colors[i].alpha;
            off += 4;
        }

        // Tex0: F32×2
        for (uint32 t = 0; t < inst->numTexCoords; t++) {
            memcpy(vtx + off, &geo->texCoords[t][i].u, 8);
            off += 8;
        }
    }
}


// ── Cleanup ─────────────────────────────────────────────────

static void
freeInstanceData(Geometry *geo)
{
    if (geo->instData == nil) return;
    GxInstanceData *inst = (GxInstanceData*)geo->instData;
    if (inst->platform != PLATFORM_GX) return;
    geo->instData = nil;
    rwFree(inst->vertexBuffer);
    rwFree(inst->indexBuffer);
    rwFree(inst->meshes);
    rwFree(inst);
}

void*
destroyNativeData(void *object, int32, int32)
{
    freeInstanceData((Geometry*)object);
    return object;
}


// ══════════════════════════════════════════════════════════════�?// instance() �?upload vertex/index data
// ══════════════════════════════════════════════════════════════�?
static void
instance(rw::ObjPipeline * /*rwpipe*/, Atomic *atomic)
{
    if(atomic == nil || atomic->geometry == nil){
        printf("[PIPE-SKIP] instance atomic/geo NULL atomic=%p\n", (void*)atomic);
        return;
    }
    Geometry *geo = atomic->geometry;
    if (geo->flags & Geometry::NATIVE) return;
    if(geo->meshHeader == nil || geo->morphTargets == nil ||
       geo->morphTargets[0].vertices == nil){
        printf("[PIPE-SKIP] instance bad geo=%p meshHeader=%p morph=%p verts=%p flags=0x%x numVerts=%u\n",
               (void*)geo, (void*)geo->meshHeader, (void*)geo->morphTargets,
               geo->morphTargets ? (void*)geo->morphTargets[0].vertices : nil,
               (unsigned)geo->flags, (unsigned)geo->numVertices);
        return;
    }

    GxInstanceData *inst = (GxInstanceData*)geo->instData;
    if (inst) {
        if (inst->platform != PLATFORM_GX || inst->pipeType != GX_INST_DEFAULT) {
            freeInstanceData(geo);
        } else if (inst->serialNum != geo->meshHeader->serialNum) {
            freeInstanceData(geo);
        } else {
            return;
        }
    }

    inst = rwNewT(GxInstanceData, 1, MEMDUR_EVENT | ID_GEOMETRY);
    geo->instData = (InstanceDataHeader*)inst;
    inst->platform  = PLATFORM_GX;
    inst->pipeType  = GX_INST_DEFAULT;
    inst->serialNum = geo->meshHeader->serialNum;

    MeshHeader *meshh = geo->meshHeader;
    inst->numMeshes     = meshh->numMeshes;
    inst->totalVertices = geo->numVertices;
    inst->totalIndices  = meshh->totalIndices;
    inst->hasNormals    = !!(geo->flags & Geometry::NORMALS) &&
                           geo->morphTargets != nil &&
                           geo->morphTargets[0].normals != nil;
    inst->hasColors     = !!(geo->flags & Geometry::PRELIT);
    inst->numTexCoords  = (geo->numTexCoordSets > 0 && geo->texCoords[0] != nil) ? 1u : 0u;

    // Vertex stride: pos(12) + nrm(12) + col(4) + tex(8)×N
    uint32 stride = 12;
    if (inst->hasNormals) stride += 12;
    if (inst->hasColors)  stride += 4;
    stride += inst->numTexCoords * 8;
    inst->vertexStride = stride;

    inst->vertexBuffer = nil;
    inst->indexBuffer = nil;
    inst->meshes = rwNewT(GxInstanceData::MeshData, inst->numMeshes,
                          MEMDUR_EVENT | ID_GEOMETRY);

    Mesh *mesh = meshh->getMeshes();
    for(uint32 m = 0; m < inst->numMeshes; m++, mesh++){
        findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
                                  &inst->meshes[m].minVert,
                                  (int32*)&inst->meshes[m].numVertices);
        inst->meshes[m].numIndices = mesh->numIndices;
        inst->meshes[m].indexOffset = 0;
        inst->meshes[m].vertexAlpha = meshHasVertexAlpha(
            geo,
            mesh->indices,
            mesh->numIndices);
        inst->meshes[m].material = mesh->material;
    }

    ((void)0);
}


// ══════════════════════════════════════════════════════════════�?// uninstance()
// ══════════════════════════════════════════════════════════════�?
static void
uninstance(rw::ObjPipeline * /*rwpipe*/, Atomic * /*atomic*/)
{
    // instance data persists for reuse
}


// ── Material �?GX TEV (GX_FALSE �?same as gxskin.cpp) ──────

// GX keeps this state across draws. Keep the shadow local to one atomic
// render, so repeated material state does not emit redundant FIFO commands
// without making assumptions about state owned by another pipeline.
struct GxRenderStateCache
{
    bool cullValid;
    u8 cullMode;
    bool zCompValid;
    u8 zCompLoc;
    bool zModeValid;
    u8 zTest;
    u8 zFunc;
    u8 zWrite;
    bool blendValid;
    u8 blendMode;
    u8 blendSrc;
    u8 blendDst;
    u8 blendLogic;
    bool alphaValid;
    u8 alphaFunc;
    u8 alphaRef;
    u8 alphaOp;
    u8 alphaComp1;
    u8 alphaRef1;
};

static inline void
setCullModeCached(GxRenderStateCache *cache, u8 mode)
{
    if(cache == nil || !cache->cullValid || cache->cullMode != mode){
        GX_SetCullMode(mode);
        if(cache){
            cache->cullValid = true;
            cache->cullMode = mode;
        }
    }
}

static inline void
setZCompLocCached(GxRenderStateCache *cache, u8 loc)
{
    if(cache == nil || !cache->zCompValid || cache->zCompLoc != loc){
        GX_SetZCompLoc(loc);
        if(cache){
            cache->zCompValid = true;
            cache->zCompLoc = loc;
        }
    }
}

static inline void
setZModeCached(GxRenderStateCache *cache, u8 test, u8 func, u8 write)
{
    if(cache == nil || !cache->zModeValid || cache->zTest != test ||
       cache->zFunc != func || cache->zWrite != write){
        GX_SetZMode(test, func, write);
        if(cache){
            cache->zModeValid = true;
            cache->zTest = test;
            cache->zFunc = func;
            cache->zWrite = write;
        }
    }
}

static inline void
setBlendModeCached(GxRenderStateCache *cache, u8 mode, u8 src, u8 dst,
                   u8 logic, bool force = false)
{
    if(force || cache == nil || !cache->blendValid || cache->blendMode != mode ||
       cache->blendSrc != src || cache->blendDst != dst ||
       cache->blendLogic != logic){
        GX_SetBlendMode(mode, src, dst, logic);
        if(cache){
            cache->blendValid = true;
            cache->blendMode = mode;
            cache->blendSrc = src;
            cache->blendDst = dst;
            cache->blendLogic = logic;
        }
    }
}

static inline void
setAlphaCompareCached(GxRenderStateCache *cache, u8 func0, u8 ref0,
                      u8 op, u8 func1, u8 ref1)
{
    if(cache == nil || !cache->alphaValid || cache->alphaFunc != func0 ||
       cache->alphaRef != ref0 || cache->alphaOp != op ||
       cache->alphaComp1 != func1 || cache->alphaRef1 != ref1){
        GX_SetAlphaCompare(func0, ref0, op, func1, ref1);
        if(cache){
            cache->alphaValid = true;
            cache->alphaFunc = func0;
            cache->alphaRef = ref0;
            cache->alphaOp = op;
            cache->alphaComp1 = func1;
            cache->alphaRef1 = ref1;
        }
    }
}

static uint32
setMaterial(Material *mat, bool32 vertexAlpha, bool hasVertexColors,
            bool modulateMaterialColor, const GxAtomicLights &lights,
            uint32 passIndex, GxRenderStateCache *stateCache)
{
    static int s_alphaDiagCount = 0;
    static int s_cullDiagCount = 0;
    static int s_texStateDiagCount = 0;
    static int s_roomAlphaDiagCount = 0;
    const char *texName = (mat && mat->texture) ? mat->texture->name : nil;
    bool hasMatAlpha = false;
    bool hasTexAlpha = false;
    uint8 texAlphaKind = GX_RASTER_ALPHA_NONE;
    bool disableVegetationShadow = isVegetationShadowTexture(texName);
    // PRELIT RGB is independent from vertex alpha. Other backends always use
    // PRELIT vertex color when present, while mesh->vertexAlpha only decides
    // whether blending is needed.
    uint8 texFmt = 0xFF;
    bool texObjValid = false;
    GXColor matColor = {255, 255, 255, 255};
    if(mat){
        if(modulateMaterialColor){
            matColor.r = mat->color.red;
            matColor.g = mat->color.green;
            matColor.b = mat->color.blue;
        }
        matColor.a = mat->color.alpha;
        hasMatAlpha = mat->color.alpha != 255;
        if(disableVegetationShadow){
            // These palm "shadow" helper atlases are not real shadows. They
            // are extra crown-thickening cards, and on Wii they are the
            // primary source of the dark canopy blobs and the long black
            // vertical strips visible at distance. Keep the geometry alive so
            // we can still toggle this experiment cheaply, but make the pass
            // fully transparent for now and let the real frond atlas carry
            // the visible leaves.
            matColor.a = 0;
            hasMatAlpha = true;
        }
        // NOTE: hard-alpha vegetation (kbtree4_test, foliage256, …) no longer
        // forces matColor.a=255. VC drives per-object material alpha for LOD
        // fade; forcing it opaque both defeated the fade and, before the
        // coexisting test+blend model below, was the only thing keeping these
        // atlases off the translucent path. The alpha test still removes fully
        // transparent foliage texels while blending preserves authored edges,
        // surviving opaque leaf texels smoothly instead of collapsing into
        // dark cards -- matching the GX2 reference default pipe.
    }

    if(mat && mat->texture && mat->texture->raster){
        Raster *ras = mat->texture->raster;
        GxRaster *natras = PLUGINOFFSET(GxRaster, ras, nativeRasterOffset);
        if(natras){
            hasTexAlpha = natras->hasAlpha != 0;
            texAlphaKind = natras->alphaKind;
            texFmt = natras->gxFmt;
            texObjValid = natras->texObjValid;
        }else
            hasTexAlpha = Raster::formatHasAlpha(ras->format);
    }
    (void)hasVertexColors;

    if(pipeFocusTexture(texName) && s_texStateDiagCount < 160){
        /* noisy focused alpha/mesh trace disabled in favor of missing-texture diagnostics */
        s_texStateDiagCount++;
    }

    bool preferTexCutout = hasTexAlpha && preferCutoutTextureAlpha(texName);
    bool preferEdgeBlendCutout = hasTexAlpha &&
                                 isKnownEdgeBlendCutoutVegetationTexture(texName);
    bool usesAlpha = hasTexAlpha || hasMatAlpha || vertexAlpha;
    bool textureCutout = hasTexAlpha &&
                         texAlphaKind == GX_RASTER_ALPHA_CUTOUT;
    bool doBlend = usesAlpha;
    bool doAlphaTest = usesAlpha;
    bool dualPass = gxState.gsAlpha && usesAlpha && gxState.zWrite;
    bool zWriteEnable = dualPass ? (passIndex == 0) : gxState.zWrite;
    bool zAfterTexturing = usesAlpha;
    bool twoSided = isLikelyRoomShellDoubleSided(texName) ||
                    isLikelyThinTwoSidedTexture(texName) ||
                    isLikelyVegetationTwoSidedTexture(texName) ||
                    (doAlphaTest && preferTexCutout);
    // RW world/building meshes arrive with the opposite winding from GX's
    // cull convention in this pipe. Thin alpha planes stay double-sided.
    u8 cullMode = twoSided ? GX_CULL_NONE : flipCullMode(gxCullFromState());
    if(twoSided && s_cullDiagCount < 24){
        /* noisy focused alpha/mesh trace disabled in favor of missing-texture diagnostics */
        s_cullDiagCount++;
    }
    if(s_roomAlphaDiagCount < 96 &&
       isLikelyRoomShellDoubleSided(texName) &&
       (hasTexAlpha || hasMatAlpha || vertexAlpha || doBlend || doAlphaTest)){
        /* noisy focused alpha/mesh trace disabled in favor of missing-texture diagnostics */
        s_roomAlphaDiagCount++;
    }
    setCullModeCached(stateCache, cullMode);

    setZCompLocCached(stateCache, zAfterTexturing ? GX_FALSE : GX_TRUE);
    setZModeCached(stateCache, gxState.zTest ? GX_TRUE : GX_FALSE, GX_LEQUAL,
                   zWriteEnable ? GX_TRUE : GX_FALSE);
    setBlendModeCached(stateCache, doBlend ? GX_BM_BLEND : GX_BM_NONE,
                       (u8)gxState.srcBlend,
                       (u8)gxState.dstBlend,
                       GX_LO_CLEAR);

    // �?GX_FALSE + GX_SRC_REG: bypass lighting, pass register color directly to TEV
    // GX_TRUE would trigger MatColor × AmbColor where AmbColor defaults to black!
    setupLightingChannels(mat, lights);
    u8 effectiveAlphaRef = 0;
    if(doAlphaTest) {
        u8 alphaFunc = GX_ALWAYS;
        u8 alphaRef = 0;
        if(dualPass) {
            alphaFunc = (passIndex == 0) ? GX_GEQUAL : GX_LESS;
            alphaRef = (u8)gxState.gsAlphaRef;
        } else {
            alphaRef = (u8)gxState.alphaTestRef;
            alphaFunc = gxAlphaFuncFromState(gxState.alphaTestFunc);
            if(textureCutout){
                // Source alpha is 0/255. Test at the bilinear midpoint so filtered
                // edge samples do not turn into a wide translucent fringe. Scale
                // the cutoff with material alpha so LOD fades remain visible.
                uint32 scaledCutoff = (128u * (uint32)matColor.a + 254u) / 255u;
                if(scaledCutoff < 1u)
                    scaledCutoff = 1u;
                if(alphaRef < scaledCutoff)
                    alphaRef = (u8)scaledCutoff;
                alphaFunc = GX_GEQUAL;
            }
        }
        effectiveAlphaRef = alphaRef;
        if(gxState.gsAlpha && usesAlpha && !gxState.zWrite) {
            setAlphaCompareCached(stateCache, GX_ALWAYS, 0, GX_AOP_AND,
                                  GX_ALWAYS, 0);
        } else {
            setAlphaCompareCached(stateCache, alphaFunc, alphaRef,
                                  GX_AOP_AND, GX_ALWAYS, 0);
        }
    } else
        setAlphaCompareCached(stateCache, GX_ALWAYS, 0, GX_AOP_AND,
                              GX_ALWAYS, 0);
    if((doBlend || doAlphaTest || vertexAlpha) &&
       (s_alphaDiagCount < 20 ||
        (alphaTraceTexture(texName) && s_alphaDiagCount < 80))){
        /* noisy focused alpha/mesh trace disabled in favor of missing-texture diagnostics */
        s_alphaDiagCount++;
    }
    (void)texFmt;
    (void)texObjValid;
    (void)preferEdgeBlendCutout;
    (void)effectiveAlphaRef;

    // Try to use material texture
    if (mat && mat->texture && mat->texture->raster) {
        GxRaster *natras = PLUGINOFFSET(GxRaster, mat->texture->raster,
                                        nativeRasterOffset);
        if (natras && natras->texObjValid) {
            GX_LoadTexObj(&natras->texObj, GX_TEXMAP0);
            GX_SetNumTexGens(1);
            GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4,
                              GX_TG_TEX0, GX_IDENTITY);
            setupDefaultLitTev(matColor, true, false);
            return dualPass ? 2u : 1u;
        }
    }

    // No valid texture �?solid color via PASSCLR
    GX_SetNumTexGens(0);
    setupDefaultLitTev(matColor, false, false);
    return dualPass ? 2u : 1u;
}


// ── GX_DIRECT vertex descriptor (same as gxskin.cpp + Im2D) ─

static void
setup3dVtxDesc(bool hasNormals, bool hasColors, uint32 numTex)
{
    (void)hasNormals;
    (void)hasColors;
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    // GX2's default instance buffer always contains a normal attribute. Use
    // the canonical (0, 0, 1) value when the source geometry has no normals.
    GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    // Always declare CLR0 �?GX_PASSCLR needs vertex color
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    for (uint32 t = 0; t < numTex; t++) {
        GX_SetVtxDesc(GX_VA_TEX0 + t, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0 + t, GX_TEX_ST, GX_F32, 0);
    }
}


// ── Submit one vertex by index, expanding from interleaved buffer ──

static inline void __attribute__((unused))
submitPipeVertex(const uint8 *vtx, bool hasNrm, bool hasCol, uint32 numTex)
{
    GX_Position3f32(
        *(const float*)(vtx),
        *(const float*)(vtx + 4),
        *(const float*)(vtx + 8));
    uint32 off = 12;
    if (hasNrm) {
        GX_Normal3f32(
            *(const float*)(vtx + off),
            *(const float*)(vtx + off + 4),
            *(const float*)(vtx + off + 8));
        off += 12;
    }
    if (hasCol) {
        GX_Color4u8(vtx[off], vtx[off+1], vtx[off+2], vtx[off+3]);
        off += 4;
    } else {
        // GX2 initializes missing PRELIT data to black and adds lighting later.
        GX_Color4u8(0, 0, 0, 255);
    }
    for (uint32 t = 0; t < numTex; t++) {
        GX_TexCoord2f32(
            *(const float*)(vtx + off),
            *(const float*)(vtx + off + 4));
        off += 8;
    }
}

static inline void
submitPipeVertexRaw(Geometry *geo, uint16 vi, bool hasNrm, bool hasCol, uint32 numTex)
{
    MorphTarget *morph = &geo->morphTargets[0];
    const V3d *pos = &morph->vertices[vi];
    GX_Position3f32(pos->x, pos->y, pos->z);

    if (hasNrm && morph->normals) {
        const V3d *nrm = &morph->normals[vi];
        GX_Normal3f32(nrm->x, nrm->y, nrm->z);
    } else
        GX_Normal3f32(0.0f, 0.0f, 1.0f);

    if (hasCol && geo->colors) {
        RGBA *c = &geo->colors[vi];
        GX_Color4u8(c->red, c->green, c->blue, c->alpha);
    } else {
        GX_Color4u8(0, 0, 0, 255);
    }

    if (numTex > 0 && geo->texCoords[0]) {
        TexCoords *tc = &geo->texCoords[0][vi];
        GX_TexCoord2f32(tc->u, tc->v);
    }
}

static inline void
drawPipeMesh(Geometry *geo, const uint16 *meshIdx, uint32 numIdx,
             bool hasNrm, bool hasCol, uint32 numTex, uint8 prim,
             bool trace, uint32 meshIndex, uint32 passIndex)
{
    if(trace)
        printf("[PIPE-TRACE] gx-begin geo=%p mesh=%u pass=%u prim=%u numIdx=%u\n",
               (void*)geo, (unsigned)meshIndex, (unsigned)passIndex,
               (unsigned)prim, (unsigned)numIdx);
    GX_Begin(prim, GX_VTXFMT0, (u16)numIdx);
    for (uint32 i = 0; i < numIdx; i++)
        submitPipeVertexRaw(geo, meshIdx[i], hasNrm, hasCol, numTex);
    GX_End();
    if(trace)
        printf("[PIPE-TRACE] gx-end geo=%p mesh=%u pass=%u\n",
               (void*)geo, (unsigned)meshIndex, (unsigned)passIndex);
}


// ══════════════════════════════════════════════════════════════�?// render() �?draw the atomic mesh-by-mesh (GX_DIRECT mode)
// ══════════════════════════════════════════════════════════════�?
static void
render(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
    if(atomic == nil || atomic->geometry == nil){
        printf("[PIPE-SKIP] render atomic/geo NULL atomic=%p\n", (void*)atomic);
        return;
    }
    Geometry *geo = atomic->geometry;
    const bool trace = pipeNeedsTrace(geo);

    // ── DIAG ──
    {
        static int _rCount = 0;
        if (_rCount < 5)
            printf("[PIPE-RENDER] #%d geo=%p flags=0x%x numVerts=%u\n",
                   _rCount, (void*)geo, (unsigned)geo->flags,
                   (unsigned)geo->numVertices);
        _rCount++;
    }

    // Upload vertex/index data on first use (like GL3 render path)
    if(trace)
        printf("[PIPE-TRACE] enter geo=%p verts=%u idx=%u meshes=%u flags=0x%x\n",
               (void*)geo, (unsigned)geo->numVertices,
               geo->meshHeader ? (unsigned)geo->meshHeader->totalIndices : 0,
               geo->meshHeader ? (unsigned)geo->meshHeader->numMeshes : 0,
               (unsigned)geo->flags);
    rwpipe->instance(atomic);

    GxInstanceData *inst = (GxInstanceData*)geo->instData;
    if (inst == nil || inst->platform != PLATFORM_GX || inst->pipeType != GX_INST_DEFAULT) {
        if (inst == nil) {
            static int _nilC = 0;
            if (_nilC < 5) { printf("[PIPE-SKIP] instData=NULL geo=%p\n", (void*)geo); _nilC++; }
        } else if (inst->platform != PLATFORM_GX || inst->pipeType != GX_INST_DEFAULT) {
            static int _platC = 0;
            if (_platC < 5) { printf("[PIPE-SKIP] platform=%u pipeType=0x%x (not GX default)\n",
                                      (unsigned)inst->platform, (unsigned)inst->pipeType); _platC++; }
        }
        return;
    }

    // Need a frame for the model matrix
    if (!atomic->getFrame()) return;

    if(geo->meshHeader == nil || geo->morphTargets == nil ||
       geo->morphTargets[0].vertices == nil){
        printf("[PIPE-SKIP] render bad geo=%p meshHeader=%p morph=%p verts=%p flags=0x%x numVerts=%u\n",
               (void*)geo, (void*)geo->meshHeader, (void*)geo->morphTargets,
               geo->morphTargets ? (void*)geo->morphTargets[0].vertices : nil,
               (unsigned)geo->flags, (unsigned)geo->numVertices);
        return;
    }

    // �?Reset viewport & scissor �?Im2D alters these
    GX_SetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GX_SetScissor(0, 0, 640, 480);

    // Model-View matrix: concat View (inverse camera) × Model
    Mtx modelMtx, modelView;
    rwMatToGxMtx(modelMtx, atomic->getFrame()->getLTM());
    guMtxConcat(gxInvCamLTM, modelMtx, modelView);
    GX_LoadPosMtxImm(modelView, GX_PNMTX0);

    // Normal matrix (same as modelView for now)
    GX_LoadNrmMtxImm(modelView, GX_PNMTX0);

    // �?Reload camera projection �?Im2D overwrites it with ortho
    GX_LoadProjectionMtx(gxProjMtx, gxProjType);

    GxAtomicLights lights;
    gatherAtomicLights(atomic, lights);

    // ── GX_DIRECT vertex format (proven working, same as gxskin.cpp + Im2D) ──

    // Draw meshes
    uint8 prim = (geo->meshHeader->flags & MeshHeader::TRISTRIP)
                 ? GX_TRIANGLESTRIP : GX_TRIANGLES;
    Mesh *mesh = geo->meshHeader->getMeshes();

    GX_InvVtxCache();
    setup3dVtxDesc(inst->hasNormals, inst->hasColors, inst->numTexCoords);
    GxRenderStateCache stateCache;
    memset(&stateCache, 0, sizeof(stateCache));

    for (uint32 m = 0; m < inst->numMeshes; m++, mesh++) {
        GxInstanceData::MeshData *md = &inst->meshes[m];
        const char *meshTexName = (md->material && md->material->texture) ?
                                  md->material->texture->name : nil;
        bool forceVertexAlphaForSoftVegetation =
            isKnownSoftBlendVegetationTexture(meshTexName);
        bool effectiveVertexAlpha =
            md->vertexAlpha || forceVertexAlphaForSoftVegetation;
        bool meshNeedsBlendAlphaState =
            effectiveVertexAlpha ||
            (md->material && md->material->color.alpha != 255) ||
            forceVertexAlphaForSoftVegetation;
        if(gxShouldSkipUnresolvedTexturedMesh(md->material, "default", m,
                                              mesh->numIndices))
            continue;
        static int s_meshFocusDiagCount = 0;
        if(pipeFocusTexture(meshTexName) && s_meshFocusDiagCount < 160){
            printf("[PIPE-MESH] mesh=%u tex=%s vtxA=%d effVtxA=%d matA=%u numIdx=%u "
                   "geoFlags=0x%X hasClr=%d mod=%d\n",
                   (unsigned)m,
                   meshTexName ? meshTexName : "none",
                   md->vertexAlpha ? 1 : 0,
                   effectiveVertexAlpha ? 1 : 0,
                   md->material ? (unsigned)md->material->color.alpha : 255u,
                   (unsigned)mesh->numIndices,
                   (unsigned)geo->flags,
                   inst->hasColors ? 1 : 0,
                   (geo->flags & Geometry::MODULATE) ? 1 : 0);
            s_meshFocusDiagCount++;
        }
        if(trace)
            printf("[PIPE-TRACE] mesh-begin geo=%p mesh=%u numIdx=%u mat=%p tex=%s\n",
                   (void*)geo, (unsigned)m, (unsigned)mesh->numIndices,
                   (void*)md->material,
                   (md->material && md->material->texture) ? md->material->texture->name : "none");
        SetRenderState(VERTEXALPHA, meshNeedsBlendAlphaState);
        uint32 passCount = setMaterial(md->material, effectiveVertexAlpha, inst->hasColors,
                    (geo->flags & Geometry::MODULATE) != 0, lights, 0,
                    &stateCache);
#ifdef GX_PIPELINE_DIAGNOSTICS
        GxSolidDiagState solidDiag = classifySolidFallback(md->material);
        if(solidDiag.reason != nil) {
            GXColor solidMatColor = {255, 255, 255, 255};
            if(md->material) {
                if((geo->flags & Geometry::MODULATE) != 0) {
                    solidMatColor.r = md->material->color.red;
                    solidMatColor.g = md->material->color.green;
                    solidMatColor.b = md->material->color.blue;
                }
                solidMatColor.a = md->material->color.alpha;
            }
            logSolidFallback("default", atomic, geo, m, 0, md->material,
                             solidDiag, solidMatColor, effectiveVertexAlpha,
                             inst->hasColors,
                             (geo->flags & Geometry::MODULATE) != 0,
                             mesh->numIndices);
        }
#endif

        uint16 *meshIdx = mesh->indices;
        uint32  numIdx  = mesh->numIndices;
        if (numIdx == 0) {
            continue;
        }
        if (meshIdx == nil) {
            printf("[PIPE-SKIP] geo=%p mesh=%u indices=NULL numIdx=%u\n",
                   (void*)geo, (unsigned)m, (unsigned)numIdx);
            continue;
        }
        if (numIdx > 65535) {
            printf("[PIPE-SKIP] geo=%p mesh=%u numIdx=%u exceeds GX_Begin u16 count\n",
                   (void*)geo, (unsigned)m, (unsigned)numIdx);
            continue;
        }
        if(md->vertexAlpha && md->material && md->material->color.alpha == 255){
            static int s_vtxAlphaDiagCount = 0;
            if(s_vtxAlphaDiagCount < 16){
                const char *name = (md->material->texture != nil) ? md->material->texture->name : "none";
                printf("[PIPE-VTXA] mesh=%u tex=%s numIdx=%u minVert=%u numVert=%u\n",
                       (unsigned)m, name ? name : "none", (unsigned)numIdx,
                       (unsigned)md->minVert, (unsigned)md->numVertices);
                s_vtxAlphaDiagCount++;
            }
        }
        uint32 badAt = 0;
        uint16 badVi = 0;
        if (!meshIndicesValid(geo, meshIdx, numIdx, &badAt, &badVi)) {
            printf("[PIPE-SKIP] geo=%p mesh=%u badIndexAt=%u vi=%u totalVerts=%u numIdx=%u\n",
                   (void*)geo, (unsigned)m, (unsigned)badAt, (unsigned)badVi,
                   (unsigned)geo->numVertices, (unsigned)numIdx);
            continue;
        }

        drawPipeMesh(geo, meshIdx, numIdx,
                     inst->hasNormals, inst->hasColors,
                     inst->numTexCoords, prim, trace, m, 0);
        if(passCount > 1) {
            setMaterial(md->material, effectiveVertexAlpha, inst->hasColors,
                        (geo->flags & Geometry::MODULATE) != 0, lights, 1,
                        &stateCache);
            drawPipeMesh(geo, meshIdx, numIdx,
                         inst->hasNormals, inst->hasColors,
                         inst->numTexCoords, prim, trace, m, 1);
        }
    }
    // GS alpha emulation changes the hardware compare/depth state for the
    // second pass. Restore the RenderWare state before another pipeline (for
    // example Im2D/HUD) can run without drawing through the last mesh.
    GX_SetAlphaCompare(gxAlphaFuncFromState(gxState.alphaTestFunc),
                       (u8)gxState.alphaTestRef,
                       GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(gxState.zTest ? GX_TRUE : GX_FALSE, GX_LEQUAL,
                gxState.zWrite ? GX_TRUE : GX_FALSE);
    GX_SetZCompLoc(GX_TRUE);
    if(trace)
        printf("[PIPE-TRACE] render-end geo=%p\n", (void*)geo);
}


} // namespace gx

// ══════════════════════════════════════════════════════════════�?// makeDefaultPipeline �?create a GX ObjPipeline
// ══════════════════════════════════════════════════════════════�?
ObjPipeline*
makeDefaultPipeline(void)
{
    ObjPipeline *pipe = ObjPipeline::create();
    pipe->impl.instance   = gx::instance;
    pipe->impl.uninstance = gx::uninstance;
    pipe->impl.render     = gx::render;
    pipe->platform        = PLATFORM_GX;
    printf("[GX-PIPEDEF] pipe=%p inst=%p render=%p uninst=%p platform=%u\n",
           (void*)pipe,
           (void*)pipe->impl.instance,
           (void*)pipe->impl.render,
           (void*)pipe->impl.uninstance,
           (unsigned)pipe->platform);
    return pipe;
}

} // namespace rw
