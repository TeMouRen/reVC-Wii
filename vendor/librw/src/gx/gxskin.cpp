// gxskin.cpp — GX Skin Pipeline (minimal T-pose render)
//
// Handles skinned mesh rendering (player, peds, vehicles).
// Initial version: T-pose only — no bone deformation.
// Full GPU skinning with matrix palette to follow.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <gccore.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "../rwrender.h"
#include "rwgx.h"
#include "gxmemory.h"

// Define GX_PIPELINE_DIAGNOSTICS when targeted pipeline tracing is needed.
#ifndef GX_PIPELINE_DIAGNOSTICS
#define printf(...) ((void)sizeof((::printf)(__VA_ARGS__)))
#endif

namespace rw {
namespace gx {

enum {
    GX_INST_DEFAULT = 0x47584446, // GXDF
    GX_INST_SKIN    = 0x4758534B  // GXSK
};

struct GxSkinData
{
    uint32  platform;
    uint32  pipeType;
    uint32  serialNum;
    uint32  numMeshes;
    uint32  totalVertices;
    uint32  totalIndices;

    uint8  *vertexBuffer;
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

static const char*
gxFmtName(uint8 fmt)
{
    switch(fmt){
    case GX_TF_CMPR:  return "CMPR";
    case GX_TF_RGBA8: return "RGBA8";
    default:          return "OTHER";
    }
}

static u8
gxCullFromState(void)
{
    // Handedness is already compensated in the GX camera matrices/projection.
    // Keep culling consistent with the default object pipe and device state.
    switch(gxState.cullMode){
    case CULLNONE:  return GX_CULL_NONE;
    case CULLFRONT: return GX_CULL_FRONT;
    default:        return GX_CULL_BACK;
    }
}

static bool
nameContainsNoCase(const char *name, const char *needle)
{
    if(name == nil || needle == nil || needle[0] == '\0')
        return false;

    for(const char *p = name; *p; p++){
        const char *a = p;
        const char *b = needle;
        while(*a && *b){
            char ca = *a;
            char cb = *b;
            if(ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if(cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if(ca != cb)
                break;
            a++;
            b++;
        }
        if(*b == '\0')
            return true;
    }
    return false;
}

static bool
skinFocusTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "plant") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "bark") ||
           nameContainsNoCase(name, "trunk") ||
           nameContainsNoCase(name, "branch") ||
           nameContainsNoCase(name, "frond") ||
           nameContainsNoCase(name, "shadow");
}

struct GxSkinSolidDiagState
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

static GxSkinSolidDiagState
classifySkinSolidFallback(Material *mat)
{
    GxSkinSolidDiagState state;
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
logSkinSolidFallback(Atomic *atomic, Geometry *geo, uint32 meshIndex,
                     uint32 passIndex, GxSkinData::MeshData *md,
                     const GxSkinSolidDiagState &diag)
{
    Material *material = md ? md->material : nil;
    // Missing-material meshes are handled by gxShouldSkipUnresolvedTexturedMesh.
    // Keep this diagnostic reserved for actual GX texture-object faults.
    if(material != nil && gxGetMissingMaterialDiag(material, nil))
        return;
    const bool ordinaryUntextured = material == nil || material->texture == nil;
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

    const void *matKey = md ? md->material : nil;
    const void *texKey = diag.texture;
    const uint32 shrink = gxMemGetShrinkTotalCount();
    const uint32 compaction = gxMemGetCompactionGeneration();
    for(uint32 i = 0; i < seenCount; i++) {
        if(seen[i].geo == geo &&
           seen[i].mat == matKey &&
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
    seen[seenIndex].mat = matKey;
    seen[seenIndex].tex = texKey;
    seen[seenIndex].reason = diag.reason;
    seen[seenIndex].shrink = shrink;
    seen[seenIndex].compaction = compaction;

    const Matrix *ltm = (atomic && atomic->getFrame()) ? atomic->getFrame()->getLTM() : nil;
    const char *texName = diag.texture ? diag.texture->name : nil;
    const char *maskName = diag.texture ? diag.texture->mask : nil;
    RGBA matColor = {255, 255, 255, 255};
    if(md && md->material)
        matColor = md->material->color;
    const uintptr_t expectedTexObjData = diag.natras && diag.natras->gxData ?
        (uintptr_t)MEM_VIRTUAL_TO_PHYSICAL(diag.natras->gxData) : 0u;
    fprintf(stdout,
           "%s pipe=skin path=%s reason=%s serial=%u modelId=%d model=%s txdSlot=%d txd=%s reqTex='%s' reqMask='%s' aliases=%u frame=%u gen=%u shrink=%u atomic=%p geo=%p mesh=%u pass=%u mat=%p tex=%p raster=%p natras=%p gx=%p objGx=%p objMatch=%d texObj=%d texRef=%d texName=%s mask=%s idx=%u rgba=%u,%u,%u,%u vtxA=%d prelit=%d mod=0 pos=%.3f,%.3f,%.3f fmt=0x%02X objFmt=0x%02X size=%u wh=%ux%u objWh=%ux%u hasA=%u aKind=%u\n",
           "[GX-TEXOBJ-FAULT]",
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
           matKey,
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
           md ? (unsigned)md->numIndices : 0u,
           (unsigned)matColor.red, (unsigned)matColor.green,
           (unsigned)matColor.blue, (unsigned)matColor.alpha,
           (md && md->vertexAlpha) ? 1 : 0,
           geo && (geo->flags & Geometry::PRELIT) ? 1 : 0,
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

static int32
skinWeightCount(Skin *skin)
{
    if(skin == nil || skin->numWeights <= 0)
        return 0;
    // RW skin weights are stored as four entries per vertex.
    // Guarding here prevents corrupt/native data from walking past vi*4.
    return skin->numWeights > 4 ? 4 : skin->numWeights;
}

static int32
skinBoneCount(Skin *skin, HAnimHierarchy *hier)
{
    if(skin == nil || hier == nil || skin->numBones <= 0 || hier->numNodes <= 0)
        return 0;

    int32 count = skin->numBones;
    if(hier->numNodes < count)
        count = hier->numNodes;
    if(count > 64)
        count = 64;
    return count;
}

static bool
meshIndicesValid(GxSkinData *inst, GxSkinData::MeshData *md,
                 const uint16 *meshIdx, uint32 *badAt, uint16 *badVi)
{
    if(inst == nil || md == nil || meshIdx == nil)
        return false;
    if(md->indexOffset > inst->totalIndices ||
       md->numIndices > inst->totalIndices - md->indexOffset){
        if(badAt)
            *badAt = md->numIndices;
        if(badVi)
            *badVi = 0;
        return false;
    }

    for(uint32 i = 0; i < md->numIndices; i++){
        uint16 vi = meshIdx[i];
        if(vi >= inst->totalVertices){
            if(badAt)
                *badAt = i;
            if(badVi)
                *badVi = vi;
            return false;
        }
    }
    return true;
}

static void
logSkinMeshDiag(Atomic *atomic, GxSkinData *inst, GxSkinData::MeshData *md,
                const uint16 *meshIdx, uint8 prim, bool doSkin, Skin *skin,
                HAnimHierarchy *hier, Matrix *boneMats, int32 boneCount,
                int32 weightCount)
{
    static int s_skinDiagCount = 0;
    if(md == nil || meshIdx == nil || md->numIndices == 0)
        return;

    uint16 vi = meshIdx[0];
    if(vi >= inst->totalVertices){
        printf("[SKIN-MESH] BAD-INDEX vi=%u totalVerts=%u prim=%s meshIdxOff=%u numIdx=%u\n",
               (unsigned)vi, (unsigned)inst->totalVertices,
               prim == GX_TRIANGLESTRIP ? "STRIP" : "TRIS",
               (unsigned)md->indexOffset, (unsigned)md->numIndices);
        s_skinDiagCount++;
        return;
    }

    const uint8 *vtx = inst->vertexBuffer + vi * inst->vertexStride;
    float px = *(const float*)(vtx);
    float py = *(const float*)(vtx + 4);
    float pz = *(const float*)(vtx + 8);
    uint32 off = 12;
    if(inst->hasNormals) off += 12;
    if(inst->hasColors)  off += 4;

    float tu = 0.0f, tv = 0.0f;
    if(inst->numTexCoords > 0){
        tu = *(const float*)(vtx + off);
        tv = *(const float*)(vtx + off + 4);
    }

    float spx = px, spy = py, spz = pz;
    float weightSum = 0.0f;
    int32 validWeights = 0;
    if(doSkin && skin && hier){
        spx = spy = spz = 0.0f;
        for(int32 w = 0; w < weightCount; w++){
            uint8 bi = skin->indices[vi * 4 + w];
            float bw = skin->weights[vi * 4 + w];
            if(bw <= 0.0f || bi >= (uint8)boneCount)
                continue;
            weightSum += bw;
            validWeights++;
            Matrix *bm = &boneMats[bi];
            spx += bw * (px * bm->right.x + py * bm->up.x + pz * bm->at.x + bm->pos.x);
            spy += bw * (px * bm->right.y + py * bm->up.y + pz * bm->at.y + bm->pos.y);
            spz += bw * (px * bm->right.z + py * bm->up.z + pz * bm->at.z + bm->pos.z);
        }
        if(validWeights == 0 || weightSum <= 0.000001f)
            spx = px, spy = py, spz = pz;
    }

    const Matrix *ltm = atomic->getFrame() ? atomic->getFrame()->getLTM() : nil;
    Texture *tex = (md->material != nil) ? md->material->texture : nil;
    const char *texName = tex ? tex->name : "none";
    bool focus = skinFocusTexture(texName);
    if((!focus && s_skinDiagCount >= 16) || (focus && s_skinDiagCount >= 96))
        return;
    GxRaster *natras = (tex && tex->raster)
        ? PLUGINOFFSET(GxRaster, tex->raster, nativeRasterOffset) : nil;

    (void)px; (void)py; (void)pz;
    (void)spx; (void)spy; (void)spz;
    (void)tu; (void)tv;
    (void)ltm; (void)natras;
    (void)weightSum; (void)validWeights;
    (void)doSkin; (void)skin; (void)weightCount; (void)prim;
    /* noisy focused skin mesh trace disabled in favor of missing-texture diagnostics */
    s_skinDiagCount++;
}


static void
buildVertexBuffer(GxSkinData *inst, Geometry *geo)
{
    MorphTarget *morph = &geo->morphTargets[0];
    uint32 numV = inst->totalVertices;
    uint32 str  = inst->vertexStride;
    uint8 *buf  = inst->vertexBuffer;

    for (uint32 i = 0; i < numV; i++) {
        uint8 *vtx = buf + i * str;
        uint32 off = 0;

        memcpy(vtx + off, &morph->vertices[i].x, 12);
        off += 12;

        if (inst->hasNormals) {
            memcpy(vtx + off, &morph->normals[i].x, 12);
            off += 12;
        }

        if (inst->hasColors) {
            // RGBA is uint8 (0-255) — copy directly
            vtx[off + 0] = geo->colors[i].red;
            vtx[off + 1] = geo->colors[i].green;
            vtx[off + 2] = geo->colors[i].blue;
            vtx[off + 3] = geo->colors[i].alpha;
            off += 4;
        }

        for (uint32 t = 0; t < inst->numTexCoords; t++) {
            memcpy(vtx + off, &geo->texCoords[t][i].u, 8);
            off += 8;
        }
    }
}


// freeSkinData removed — unified destroyNativeData in gxpipe.cpp handles both layouts

static void
skinInstance(rw::ObjPipeline * /*rwpipe*/, Atomic *atomic)
{
    Geometry *geo = atomic->geometry;
    if (geo->flags & Geometry::NATIVE) return;

    GxSkinData *inst = (GxSkinData*)geo->instData;
    if (inst) {
        if (inst->platform != PLATFORM_GX || inst->pipeType != GX_INST_SKIN)
            destroyNativeData(geo, 0, 0);
        else if (inst->serialNum != geo->meshHeader->serialNum)
            destroyNativeData(geo, 0, 0);
        else
            return;
    }

    inst = rwNewT(GxSkinData, 1, MEMDUR_EVENT | ID_GEOMETRY);
    geo->instData = (InstanceDataHeader*)inst;
    inst->platform  = PLATFORM_GX;
    inst->pipeType  = GX_INST_SKIN;
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

    uint32 stride = 12;
    if (inst->hasNormals) stride += 12;
    if (inst->hasColors)  stride += 4;
    stride += inst->numTexCoords * 8;
    inst->vertexStride = stride;

    inst->vertexBuffer = rwNewT(uint8, inst->totalVertices * stride,
                                MEMDUR_EVENT | ID_GEOMETRY);
    inst->indexBuffer  = rwNewT(uint16, inst->totalIndices,
                                MEMDUR_EVENT | ID_GEOMETRY);
    inst->meshes       = rwNewT(GxSkinData::MeshData, inst->numMeshes,
                                MEMDUR_EVENT | ID_GEOMETRY);

    buildVertexBuffer(inst, geo);

    Mesh  *mesh   = meshh->getMeshes();
    uint32 idxOff = 0;
    for (uint32 m = 0; m < inst->numMeshes; m++) {
        findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
                                  &inst->meshes[m].minVert,
                                  (int32*)&inst->meshes[m].numVertices);
        inst->meshes[m].numIndices  = mesh->numIndices;
        inst->meshes[m].indexOffset = idxOff;
        inst->meshes[m].vertexAlpha = 0;
        inst->meshes[m].material    = mesh->material;
        memcpy(inst->indexBuffer + idxOff, mesh->indices,
               mesh->numIndices * 2);
        if(inst->hasColors && geo->colors != nil){
            uint32 minv = inst->meshes[m].minVert;
            uint32 numv = (uint32)inst->meshes[m].numVertices;
            inst->meshes[m].vertexAlpha = instColor(VERT_RGBA,
                inst->vertexBuffer + minv * stride + (inst->hasNormals ? 24 : 12),
                geo->colors + minv, numv, stride);
        }
        idxOff += mesh->numIndices;
        mesh++;
    }

    DCFlushRange(inst->vertexBuffer, inst->totalVertices * stride);
    DCFlushRange(inst->indexBuffer,  inst->totalIndices * 2);
    ((void)0);
}


static void
skinUninstance(rw::ObjPipeline * /*rwpipe*/, Atomic * /*atomic*/)
{
}


static uint32
setMaterialSkin(Material *mat, bool32 vertexAlpha, uint32 passIndex)
{
    // ★ Reload projection every mesh — Im2D/clearCamera overwrite it with ortho
    GX_LoadProjectionMtx(gxProjMtx, gxProjType);
    const bool fullbrightDebug = gxFullbrightDebugActive();
    const char *texName = (mat && mat->texture) ? mat->texture->name : nil;

    bool hasTexAlpha = false;
    uint8 texAlphaKind = GX_RASTER_ALPHA_NONE;
    uint8 texFmt = 0xFF;
    bool texObjValid = false;
    if(mat && mat->texture && mat->texture->raster){
        Raster *ras = mat->texture->raster;
        GxRaster *natras = PLUGINOFFSET(GxRaster, ras, nativeRasterOffset);
        if(natras){
            hasTexAlpha = natras->hasAlpha != 0;
            texAlphaKind = natras->alphaKind;
            texFmt = natras->gxFmt;
            texObjValid = natras->texObjValid != 0;
        }else
            hasTexAlpha = Raster::formatHasAlpha(ras->format);
    }
    (void)texAlphaKind;
    (void)texFmt;
    (void)texObjValid;

    // Keep skinned meshes on the same RenderWare alpha contract as the
    // default pipeline and the GX2/GL3 backends.
    bool hasMatAlpha = mat && mat->color.alpha < 255;
    bool usesAlpha = hasTexAlpha || vertexAlpha || hasMatAlpha;
    bool doBlend = !fullbrightDebug && usesAlpha;
    bool doAlphaTest = !fullbrightDebug && usesAlpha;
    bool dualPass = gxState.gsAlpha && usesAlpha &&
                    !fullbrightDebug && gxState.zWrite;
    bool zWriteEnable = dualPass ? (passIndex == 0) : gxState.zWrite;
    bool zAfterTexturing = usesAlpha;
    GX_SetZCompLoc(zAfterTexturing ? GX_FALSE : GX_TRUE);

    GX_SetZMode(gxState.zTest ? GX_TRUE : GX_FALSE,
                GX_LEQUAL,
                zWriteEnable ? GX_TRUE : GX_FALSE);
    GX_SetBlendMode(doBlend ? GX_BM_BLEND : GX_BM_NONE,
                    (u8)gxState.srcBlend,
                    (u8)gxState.dstBlend,
                    GX_LO_CLEAR);
    GX_SetCullMode(fullbrightDebug ? GX_CULL_NONE : gxCullFromState());

    // GX_FALSE bypasses dynamic lighting; GX_SRC_VTX keeps VC's prelit colors.
    // The material register is still populated for state fallback/debug paths.
    GXColor matCol = {255, 255, 255, 255};
    if (mat && !fullbrightDebug) {
        matCol.r = mat->color.red;
        matCol.g = mat->color.green;
        matCol.b = mat->color.blue;
        matCol.a = mat->color.alpha;
    }
    GX_SetChanMatColor(GX_COLOR0A0, matCol);
    GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE,
                   GX_SRC_REG,
                   fullbrightDebug ? GX_SRC_REG : GX_SRC_VTX,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumChans(1);
    u8 effectiveAlphaRef = 0;
    if(doAlphaTest){
        uint8 materialAlpha = mat ? mat->color.alpha : 255;
        uint32 scaledCutoff = (128u * (uint32)materialAlpha + 254u) / 255u;
        if(scaledCutoff < 1u)
            scaledCutoff = 1u;
        u8 stateAlphaRef = (u8)gxState.alphaTestRef;
        if(dualPass) {
            effectiveAlphaRef = (u8)gxState.gsAlphaRef;
            GX_SetAlphaCompare(passIndex == 0 ? GX_GEQUAL : GX_LESS,
                               effectiveAlphaRef,
                               GX_AOP_AND,
                               GX_ALWAYS, 0);
        } else if(gxState.gsAlpha && usesAlpha && !gxState.zWrite) {
            GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        } else {
            effectiveAlphaRef = stateAlphaRef < scaledCutoff ?
                                (u8)scaledCutoff : stateAlphaRef;
            GX_SetAlphaCompare(GX_GEQUAL,
                               effectiveAlphaRef,
                               GX_AOP_AND,
                               GX_ALWAYS, 0);
        }
    }else
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

    static int s_skinTexStateLogCount = 0;
    if(skinFocusTexture(texName) && s_skinTexStateLogCount < 160) {
        /* noisy focused skin tex trace disabled in favor of missing-texture diagnostics */
        s_skinTexStateLogCount++;
    }

    // Try to use material texture
    if (mat && mat->texture && mat->texture->raster) {
        GxRaster *natras = PLUGINOFFSET(GxRaster, mat->texture->raster,
                                        nativeRasterOffset);
        if (natras && natras->texObjValid) {
            GX_LoadTexObj(&natras->texObj, GX_TEXMAP0);
            GX_SetNumTexGens(1);
            GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4,
                              GX_TG_TEX0, GX_IDENTITY);
            GX_SetNumTevStages(1);
            GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0,
                           GX_TEXMAP0, GX_COLOR0A0);
            GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
            return dualPass ? 2u : 1u;
        }
    }

    // No valid texture — solid color via PASSCLR
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL,
                   GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    return dualPass ? 2u : 1u;
}

static u8
skinAlphaFuncFromState(int32 f)
{
    switch(f){
    case ALPHAGREATEREQUAL: return GX_GEQUAL;
    case ALPHALESS:         return GX_LESS;
    default:                return GX_ALWAYS;
    }
}


// ── GX_DIRECT vertex descriptor (matching proven Im2D path) ──
static void
setupSkinVtxDesc(bool hasNormals, bool hasColors, uint32 numTex)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    if (hasNormals) {
        GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    }
    // Always declare CLR0 — GX_PASSCLR needs vertex color
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    for (uint32 t = 0; t < numTex; t++) {
        GX_SetVtxDesc(GX_VA_TEX0 + t, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0 + t, GX_TEX_ST, GX_F32, 0);
    }
}

// ── Submit one vertex by index, expanding from interleaved buffer ──
static inline void
submitSkinVertex(const uint8 *vtx, bool hasNrm, bool hasCol, uint32 numTex)
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
    if (gxFullbrightDebugActive()) {
        GX_Color4u8(255, 255, 255, 255);
        if(hasCol)
            off += 4;
    } else if (hasCol) {
        GX_Color4u8(vtx[off], vtx[off+1], vtx[off+2], vtx[off+3]);
        off += 4;
    } else {
        // Always send white — GX_PASSCLR needs vertex color
        GX_Color4u8(255, 255, 255, 255);
    }
    for (uint32 t = 0; t < numTex; t++) {
        GX_TexCoord2f32(
            *(const float*)(vtx + off),
            *(const float*)(vtx + off + 4));
        off += 8;
    }
}


static void
skinRender(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
    Geometry *geo = atomic->geometry;
    rwpipe->instance(atomic);

    GxSkinData *inst = (GxSkinData*)geo->instData;
    if (inst == nil || inst->platform != PLATFORM_GX || inst->pipeType != GX_INST_SKIN) return;

    Frame *frame = atomic->getFrame();
    if (!frame) return;

    // ── Reset viewport & scissor after Im2D ──
    GX_SetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GX_SetScissor(0, 0, 640, 480);

    // ── Camera transform ──
    Mtx modelMtx, modelView;
    rwMatToGxMtx(modelMtx, frame->getLTM());
    guMtxConcat(gxInvCamLTM, modelMtx, modelView);
    GX_LoadPosMtxImm(modelView, GX_PNMTX0);
    GX_LoadNrmMtxImm(modelView, GX_PNMTX0);
    GX_LoadProjectionMtx(gxProjMtx, gxProjType);

    // ── Compute bone skinning matrices (inverseBind × hierarchy) ──
    Skin           *skin = Skin::get(geo);
    HAnimHierarchy *hier = Skin::getHierarchy(atomic);
    bool            doSkin = false;
    int32           weightCount = skinWeightCount(skin);
    int32           boneCount = skinBoneCount(skin, hier);
    Matrix          boneMats[64];

    if (boneCount > 0 && weightCount > 0 &&
        skin->inverseMatrices != nil && skin->indices != nil &&
        skin->weights != nil && hier->matrices != nil) {
        doSkin = true;
        Matrix *invMats = (Matrix*)skin->inverseMatrices;
        if (hier->flags & HAnimHierarchy::LOCALSPACEMATRICES) {
            for (int32 bn = 0; bn < boneCount; bn++) {
                invMats[bn].flags = 0;
                Matrix::mult(&boneMats[bn], &invMats[bn], &hier->matrices[bn]);
            }
        } else {
            // Non-local-space matrices: compose inverseAtomic × hierarchy
            // matching GL3 skin pipeline (gl3skin.cpp:233-237)
            Matrix invAtmMat;
            Matrix::invert(&invAtmMat, frame->getLTM());
            for (int32 bn = 0; bn < boneCount; bn++) {
                Matrix tmp;
                invMats[bn].flags = 0;
                Matrix::mult(&tmp, &hier->matrices[bn], &invAtmMat);
                Matrix::mult(&boneMats[bn], &invMats[bn], &tmp);
            }
        }
    }

    // ── GX_DIRECT vertex format ──
    setupSkinVtxDesc(inst->hasNormals, inst->hasColors, inst->numTexCoords);

#ifndef WII
    static int _flgOnce = 0;
    if (_flgOnce < 3) {
        _flgOnce++;
        printf("[SKIN] mesh flags=%u -> prim=%s skin=%s bones=%d wgt=%d\n",
            (unsigned)geo->meshHeader->flags,
            (geo->meshHeader->flags & 1) ? "STRIP" : "TRIS",
            doSkin ? "YES" : "NO",
            boneCount,
            weightCount);
        if (skin && (skin->numWeights > 4 || skin->numBones > 64 ||
                     (hier && hier->numNodes > 64))) {
            printf("[SKIN-CLAMP] rawBones=%d rawNodes=%d rawWeights=%d usedBones=%d usedWeights=%d\n",
                skin->numBones, hier ? hier->numNodes : 0, skin->numWeights,
                boneCount, weightCount);
        }
        if (doSkin && hier && boneCount > 0) {
            Matrix *bm0 = &boneMats[0];
            printf("[SKIN-DIAG] bone[0] right=(%.3f,%.3f,%.3f)\n",
                bm0->right.x, bm0->right.y, bm0->right.z);
            printf("[SKIN-DIAG] bone[0] up   =(%.3f,%.3f,%.3f)\n",
                bm0->up.x, bm0->up.y, bm0->up.z);
            printf("[SKIN-DIAG] bone[0] at   =(%.3f,%.3f,%.3f)\n",
                bm0->at.x, bm0->at.y, bm0->at.z);
            printf("[SKIN-DIAG] bone[0] pos  =(%.3f,%.3f,%.3f)\n",
                bm0->pos.x, bm0->pos.y, bm0->pos.z);
        }
        if (!doSkin && skin)
            printf("[SKIN-DIAG] NO SKIN: hier=%p numNodes=%d\n",
                (void*)hier, hier ? hier->numNodes : -1);
    }
#endif

    uint8  prim = (geo->meshHeader->flags & MeshHeader::TRISTRIP)
                  ? GX_TRIANGLESTRIP : GX_TRIANGLES;
    uint32 str  = inst->vertexStride;
    uint8 *base = inst->vertexBuffer;
    uint16 *idx = inst->indexBuffer;

    static int s_skinRenderCount = 0;
#ifdef WII
    bool logRender = false;
#else
    bool logRender = s_skinRenderCount < 16;
#endif
    if(logRender) {
        printf("[SKIN-RENDER-BEGIN] #%d geo=%p meshes=%u verts=%u idx=%u prim=%s skin=%d bones=%d weights=%d\n",
               s_skinRenderCount, (void*)geo,
               (unsigned)inst->numMeshes, (unsigned)inst->totalVertices,
               (unsigned)inst->totalIndices,
               prim == GX_TRIANGLESTRIP ? "STRIP" : "TRIS",
               doSkin ? 1 : 0, boneCount, weightCount);
    }
    s_skinRenderCount++;

    for (uint32 m = 0; m < inst->numMeshes; m++) {
        GxSkinData::MeshData *md = &inst->meshes[m];
        // ★ Reload model-view each mesh — clearCamera overwrites PNMTX0
        GX_LoadPosMtxImm(modelView, GX_PNMTX0);
        Texture *meshTex = (md->material != nil) ? md->material->texture : nil;
        const char *meshTexName = meshTex ? meshTex->name : nil;
        static int s_skinFocusMeshLogCount = 0;
        if(skinFocusTexture(meshTexName) && s_skinFocusMeshLogCount < 160) {
            /* noisy focused skin mesh trace disabled in favor of missing-texture diagnostics */
            s_skinFocusMeshLogCount++;
        }

        uint32  numIdx  = md->numIndices;
        if (numIdx == 0) continue;
        if (md->indexOffset > inst->totalIndices ||
            numIdx > inst->totalIndices - md->indexOffset) {
            printf("[SKIN-SKIP] mesh=%u indexRange off=%u numIdx=%u totalIdx=%u\n",
                   (unsigned)m, (unsigned)md->indexOffset,
                   (unsigned)numIdx, (unsigned)inst->totalIndices);
            continue;
        }
        uint16 *meshIdx = idx + md->indexOffset;
        if (numIdx > 65535) {
            printf("[SKIN-SKIP] mesh=%u numIdx=%u exceeds GX_Begin u16 count\n",
                   (unsigned)m, (unsigned)numIdx);
            continue;
        }
        uint32 badAt = 0;
        uint16 badVi = 0;
        if (!meshIndicesValid(inst, md, meshIdx, &badAt, &badVi)) {
            printf("[SKIN-SKIP] mesh=%u badIndexAt=%u vi=%u totalVerts=%u numIdx=%u\n",
                   (unsigned)m, (unsigned)badAt, (unsigned)badVi,
                   (unsigned)inst->totalVertices, (unsigned)numIdx);
            continue;
        }
        if(gxShouldSkipUnresolvedTexturedMesh(md->material, "skin", m,
                                              numIdx))
            continue;
        logSkinMeshDiag(atomic, inst, md, meshIdx, prim, doSkin, skin, hier,
                        boneMats, boneCount, weightCount);
        uint32 passCount = setMaterialSkin(md->material, md->vertexAlpha, 0);
        GxSkinSolidDiagState solidDiag = classifySkinSolidFallback(md->material);
        if(solidDiag.reason != nil)
            logSkinSolidFallback(atomic, geo, m, 0, md, solidDiag);
        for(uint32 pass = 0; pass < passCount; pass++) {
            GX_LoadPosMtxImm(modelView, GX_PNMTX0);
            if(pass > 0)
                setMaterialSkin(md->material, md->vertexAlpha, pass);

        if(logRender)
            printf("[SKIN-MESH-BEGIN] render=%d mesh=%u pass=%u numIdx=%u tex=%s\n",
                   s_skinRenderCount - 1, (unsigned)m, (unsigned)pass, (unsigned)numIdx,
                   (md->material && md->material->texture) ?
                       md->material->texture->name : "none");

        GX_Begin(prim, GX_VTXFMT0, (u16)numIdx);

        if (doSkin) {
            int32 nw = weightCount;
            static int s_zeroWeightLogs = 0;
            for (uint32 i = 0; i < numIdx; i++) {
                uint16 vi  = meshIdx[i];
                uint8 *vtx = base + vi * str;
                float px = *(const float*)(vtx);
                float py = *(const float*)(vtx + 4);
                float pz = *(const float*)(vtx + 8);
                uint32 off = 12;

                float nx = 0, ny = 0, nz = 0;
                bool hasNrm = inst->hasNormals;
                if (hasNrm) {
                    nx = *(const float*)(vtx + off);
                    ny = *(const float*)(vtx + off + 4);
                    nz = *(const float*)(vtx + off + 8);
                    off += 12;
                }

                float spx = 0, spy = 0, spz = 0;
                float snx = 0, sny = 0, snz = 0;
                float weightSum = 0.0f;
                int32 validWeights = 0;
                for (int32 w = 0; w < nw; w++) {
                    uint8 bi = skin->indices[vi * 4 + w];
                    float bw = skin->weights[vi * 4 + w];
                    if (bw <= 0.0f) continue;
                    if (bi >= (uint8)boneCount) continue;
                    weightSum += bw;
                    validWeights++;
                    Matrix *bm = &boneMats[bi];
                    // Row-vector × row-major matrix: v' = v × M
                    spx += bw * (px * bm->right.x + py * bm->up.x + pz * bm->at.x + bm->pos.x);
                    spy += bw * (px * bm->right.y + py * bm->up.y + pz * bm->at.y + bm->pos.y);
                    spz += bw * (px * bm->right.z + py * bm->up.z + pz * bm->at.z + bm->pos.z);
                    if (hasNrm) {
                        // Normal: direction only, no translation
                        snx += bw * (nx * bm->right.x + ny * bm->up.x + nz * bm->at.x);
                        sny += bw * (nx * bm->right.y + ny * bm->up.y + nz * bm->at.y);
                        snz += bw * (nx * bm->right.z + ny * bm->up.z + nz * bm->at.z);
                    }
                }

                if (validWeights == 0 || weightSum <= 0.000001f) {
                    spx = px; spy = py; spz = pz;
                    if (hasNrm) {
                        snx = nx; sny = ny; snz = nz;
                    }
                    if (s_zeroWeightLogs < 24) {
                        Texture *tex = md->material ? md->material->texture : nil;
                        printf("[SKIN-ZERO] tex=%s vi=%u nw=%d prim=%s wsum=%.3f valid=%d raw=",
                               tex ? tex->name : "none",
                               (unsigned)vi, (int)nw,
                               prim == GX_TRIANGLESTRIP ? "STRIP" : "TRIS",
                               weightSum, (int)validWeights);
                        for (int32 w = 0; w < nw; w++) {
                            uint8 bi = skin->indices[vi * 4 + w];
                            float bw = skin->weights[vi * 4 + w];
                            printf("%s%u:%.3f", w == 0 ? "" : ",", (unsigned)bi, bw);
                        }
                        printf("\n");
                        s_zeroWeightLogs++;
                    }
                }

                GX_Position3f32(spx, spy, spz);
                if (hasNrm) {
                    float len = snx*snx + sny*sny + snz*snz;
                    if (len > 0.000001f) {
                        len = 1.0f / sqrtf(len);
                        snx *= len; sny *= len; snz *= len;
                    }
                    GX_Normal3f32(snx, sny, snz);
                }

                if (inst->hasColors) {
                    GX_Color4u8(vtx[off], vtx[off+1], vtx[off+2], vtx[off+3]);
                    off += 4;
                } else {
                    GX_Color4u8(255, 255, 255, 255);
                }
                for (uint32 t = 0; t < inst->numTexCoords; t++) {
                    GX_TexCoord2f32(
                        *(const float*)(vtx + off),
                        *(const float*)(vtx + off + 4));
                    off += 8;
                }
            }
        } else {
            for (uint32 i = 0; i < numIdx; i++)
                submitSkinVertex(base + meshIdx[i] * str,
                                 inst->hasNormals, inst->hasColors,
                                 inst->numTexCoords);
        }

        GX_End();
        if(logRender)
            printf("[SKIN-MESH-END] render=%d mesh=%u pass=%u\n",
                   s_skinRenderCount - 1, (unsigned)m, (unsigned)pass);
        }
    }
    GX_SetAlphaCompare(skinAlphaFuncFromState(gxState.alphaTestFunc),
                       (u8)gxState.alphaTestRef,
                       GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(gxState.zTest ? GX_TRUE : GX_FALSE, GX_LEQUAL,
                gxState.zWrite ? GX_TRUE : GX_FALSE);
    GX_SetZCompLoc(GX_TRUE);
    if(logRender)
        printf("[SKIN-RENDER-END] #%d geo=%p\n",
               s_skinRenderCount - 1, (void*)geo);
}


} // namespace gx

// ═══════════════════════════════════════════════════════════════
// Skin plugin registration (platform-specific, like D3D8/GL3/PS2)
// ═══════════════════════════════════════════════════════════════

ObjPipeline*
makeSkinPipeline(void)
{
    ObjPipeline *pipe = ObjPipeline::create();
    pipe->impl.instance   = gx::skinInstance;
    pipe->impl.uninstance = gx::skinUninstance;
    pipe->impl.render     = gx::skinRender;
    pipe->platform        = PLATFORM_GX;
    pipe->pluginID        = ID_SKIN;
    pipe->pluginData      = 1;
    return pipe;
}

static void*
skinOpen(void *o, int32, int32)
{
    skinGlobals.pipelines[PLATFORM_GX] = makeSkinPipeline();
    return o;
}

static void*
skinClose(void *o, int32, int32)
{
    ((ObjPipeline*)skinGlobals.pipelines[PLATFORM_GX])->destroy();
    skinGlobals.pipelines[PLATFORM_GX] = nil;
    return o;
}

void
initSkin(void)
{
    Driver::registerPlugin(PLATFORM_GX, 0, ID_SKIN,
                           skinOpen, skinClose);
}

} // namespace rw
