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

    printf("[SKIN-MESH] tex=%s fmt=%s texObj=%d prim=%s skin=%d idx0=%u numIdx=%u "
           "v0=(%.3f,%.3f,%.3f) sv0=(%.3f,%.3f,%.3f) uv0=(%.3f,%.3f) "
           "ltmPos=(%.3f,%.3f,%.3f) wsum=%.3f valid=%d weights=",
           texName,
           natras ? gxFmtName(natras->gxFmt) : "none",
           natras ? natras->texObjValid : 0,
           prim == GX_TRIANGLESTRIP ? "STRIP" : "TRIS",
           doSkin ? 1 : 0,
           (unsigned)vi, (unsigned)md->numIndices,
           px, py, pz, spx, spy, spz, tu, tv,
           ltm ? ltm->pos.x : 0.0f, ltm ? ltm->pos.y : 0.0f, ltm ? ltm->pos.z : 0.0f,
           weightSum, (int)validWeights);

    if(doSkin && skin){
        for(int32 w = 0; w < weightCount; w++){
            uint8 bi = skin->indices[vi * 4 + w];
            float bw = skin->weights[vi * 4 + w];
            if(bw <= 0.0f)
                continue;
            printf("%s%u:%.3f", w == 0 ? "" : ",", (unsigned)bi, bw);
        }
    }else{
        printf("none");
    }
    printf("\n");
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
    const bool freeCamDebug = gxFreeCamDebugActive();
    const bool fullbrightDebug = gxFullbrightDebugActive();
    const bool freeCamXray = gxFreeCamCpuSceneActive();
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

    // Keep skinned meshes on the same RenderWare alpha contract as the
    // default pipeline and the GX2/GL3 backends.
    bool hasMatAlpha = mat && mat->color.alpha < 255;
    bool usesAlpha = hasTexAlpha || vertexAlpha || hasMatAlpha;
    bool doBlend = !fullbrightDebug && usesAlpha;
    bool doAlphaTest = !fullbrightDebug && usesAlpha;
    bool dualPass = !freeCamXray && gxState.gsAlpha && usesAlpha &&
                    !fullbrightDebug && gxState.zWrite;
    bool zWriteEnable = dualPass ? (passIndex == 0) : gxState.zWrite;
    bool zAfterTexturing = usesAlpha;
    GX_SetZCompLoc(zAfterTexturing ? GX_FALSE : GX_TRUE);

    if(freeCamXray)
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    else
        GX_SetZMode(gxState.zTest ? GX_TRUE : GX_FALSE,
                    GX_LEQUAL,
                    zWriteEnable ? GX_TRUE : GX_FALSE);
    GX_SetBlendMode(doBlend ? GX_BM_BLEND : GX_BM_NONE,
                    (u8)gxState.srcBlend,
                    (u8)gxState.dstBlend,
                    GX_LO_CLEAR);
    GX_SetCullMode((freeCamDebug || fullbrightDebug) ? GX_CULL_NONE : gxCullFromState());

    // GX_FALSE bypasses dynamic lighting; GX_SRC_VTX keeps VC's prelit colors.
    // The material register is still populated for state fallback/debug paths.
    GXColor matCol = {255, 255, 255, 255};
    if (mat && !freeCamDebug && !fullbrightDebug) {
        matCol.r = mat->color.red;
        matCol.g = mat->color.green;
        matCol.b = mat->color.blue;
        matCol.a = mat->color.alpha;
    }
    GX_SetChanMatColor(GX_COLOR0A0, matCol);
    GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE,
                   GX_SRC_REG,
                   (freeCamDebug || fullbrightDebug) ? GX_SRC_REG : GX_SRC_VTX,
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
        printf("[SKIN-TEXSTATE] tex=%s fmt=0x%02X texObj=%d texA=%d aKind=%u vtxA=%d blend=%d cutout=%d "
               "matA=%u aFn=%d aRef=%d effARef=%u zWrite=%d zAfterTex=%d cull=%u\n",
               texName,
               (unsigned)texFmt,
               texObjValid ? 1 : 0,
               hasTexAlpha ? 1 : 0,
               (unsigned)texAlphaKind,
               vertexAlpha ? 1 : 0,
               doBlend ? 1 : 0,
               doAlphaTest ? 1 : 0,
               mat ? (unsigned)mat->color.alpha : 255u,
               (int)gxState.alphaTestFunc,
               (int)gxState.alphaTestRef,
               (unsigned)effectiveAlphaRef,
               zWriteEnable ? 1 : 0,
               zAfterTexturing ? 1 : 0,
               (unsigned)((freeCamDebug || fullbrightDebug) ? GX_CULL_NONE : gxCullFromState()));
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
    if (gxFreeCamDebugActive() || gxFullbrightDebugActive()) {
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
            printf("[SKIN-FOCUS] tex=%s mesh=%u prim=%s doSkin=%d bones=%d weights=%d "
                   "vAlpha=%d hasNrm=%d hasCol=%d texCoords=%u numIdx=%u\n",
                   meshTexName,
                   (unsigned)m,
                   prim == GX_TRIANGLESTRIP ? "STRIP" : "TRIS",
                   doSkin ? 1 : 0,
                   (int)boneCount,
                   (int)weightCount,
                   md->vertexAlpha ? 1 : 0,
                   inst->hasNormals ? 1 : 0,
                   inst->hasColors ? 1 : 0,
                   (unsigned)inst->numTexCoords,
                   (unsigned)md->numIndices);
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
        logSkinMeshDiag(atomic, inst, md, meshIdx, prim, doSkin, skin, hier,
                        boneMats, boneCount, weightCount);
        uint32 passCount = setMaterialSkin(md->material, md->vertexAlpha, 0);
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
