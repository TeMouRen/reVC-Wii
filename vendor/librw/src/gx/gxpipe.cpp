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
#include <gccore.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "rwgx.h"

namespace rw {
namespace gx {

static bool
pipeNeedsTrace(Geometry *geo)
{
    (void)geo;
    return false;
}

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

static bool
needsTighterVegetationCutoutRef(const char *name)
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

static bool
pipeFocusTexture(const char *name)
{
    return isKnownHardAlphaVegetationTexture(name) ||
           isKnownEdgeBlendCutoutVegetationTexture(name) ||
           isKnownSoftBlendVegetationTexture(name) ||
           alphaTraceTexture(name) ||
           (name && (
                strcmp(name, "Grass_128HV") == 0 ||
               strcmp(name, "Grass_64HV") == 0 ||
               strcmp(name, "hedgewall_64") == 0 ||
               strcmp(name, "lodhedgewall_64") == 0 ||
               strcmp(name, "hedge2_128") == 0 ||
               strcmp(name, "Bow_grass_gryard") == 0 ||
               strcmp(name, "Bow_church_grass_gen") == 0));
}

static bool
preferBlendTextureAlpha(const char *name)
{
    if(name == nil)
        return false;

    return isKnownSoftBlendVegetationTexture(name) ||
           nameContainsNoCase(name, "glass") ||
           nameContainsNoCase(name, "window") ||
           nameContainsNoCase(name, "shadow") ||
           nameContainsNoCase(name, "beam") ||
           nameContainsNoCase(name, "light") ||
           nameContainsNoCase(name, "flare") ||
           nameContainsNoCase(name, "water");
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

static bool
isLikelyFoliageCutoutTexture(const char *name)
{
    if(isKnownSoftBlendVegetationTexture(name))
        return false;

    return nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "bush") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "ivy") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "plant");
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

static void
setMaterial(Material *mat, bool32 vertexAlpha, bool hasVertexColors)
{
    static int s_alphaDiagCount = 0;
    static int s_cullDiagCount = 0;
    static int s_texStateDiagCount = 0;
    const char *texName = (mat && mat->texture) ? mat->texture->name : nil;
    bool hasMatAlpha = false;
    bool hasTexAlpha = false;
    bool forcedKnownAlpha = isKnownHardAlphaVegetationTexture(texName);
    bool disableVegetationShadow = isVegetationShadowTexture(texName);
    bool useVertexMaterialColor = vertexAlpha && hasVertexColors;
    uint8 texFmt = 0xFF;
    bool texObjValid = false;
    GXColor matColor = {255, 255, 255, 255};
    if(mat){
        matColor.r = mat->color.red;
        matColor.g = mat->color.green;
        matColor.b = mat->color.blue;
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
        // atlases off the translucent path. Now the alpha TEST always runs for
        // masked foliage (see maskedCutout), so a fading tree blends its
        // surviving opaque leaf texels smoothly instead of collapsing into
        // dark cards -- matching the GX2 reference default pipe.
    }

    if(mat && mat->texture && mat->texture->raster){
        Raster *ras = mat->texture->raster;
        GxRaster *natras = PLUGINOFFSET(GxRaster, ras, nativeRasterOffset);
        if(natras){
            hasTexAlpha = natras->hasAlpha != 0;
            texFmt = natras->gxFmt;
            texObjValid = natras->texObjValid;
        }else
            hasTexAlpha = Raster::formatHasAlpha(ras->format);
    }

    if(forcedKnownAlpha && !hasTexAlpha)
        hasTexAlpha = true;

    if((forcedKnownAlpha || pipeFocusTexture(texName)) &&
       (forcedKnownAlpha || s_texStateDiagCount < 160)){
        printf("[PIPE-TEXSTATE] tex=%s fmt=0x%02X texObj=%d matA=%u texA=%d vtxA=%d prelit=%d forceA=%d colSrc=%s\n",
               texName ? texName : "none",
               (unsigned)texFmt,
               texObjValid ? 1 : 0,
               (unsigned)matColor.a,
               hasTexAlpha ? 1 : 0,
               vertexAlpha ? 1 : 0,
               hasVertexColors ? 1 : 0,
               forcedKnownAlpha ? 1 : 0,
               useVertexMaterialColor ? "VTX" : "REG");
        if(!forcedKnownAlpha)
            s_texStateDiagCount++;
    }

    bool preferTexBlend = hasTexAlpha && preferBlendTextureAlpha(texName);
    bool preferTexCutout = hasTexAlpha && preferCutoutTextureAlpha(texName);
    bool preferEdgeBlendCutout = hasTexAlpha &&
                                 isKnownEdgeBlendCutoutVegetationTexture(texName);
    // ── Coexisting alpha-test + blend, mirroring the GX2 reference default
    // pipe (gx2device.cpp setVertexAlpha/setRasterStage: blend and alpha test
    // are enabled by the SAME "has alpha" condition, never mutually exclusive).
    //
    // - doAlphaTest (cutout): run whenever the texture carries a real mask
    //   (foliage/fence/hedge), so fully-transparent texels are discarded and
    //   z-write stays meaningful. Genuinely translucent surfaces (glass/water/
    //   shadow, via preferTexBlend) are excluded -- they should blend, not test.
    // - doBlend: ADDITIVE, not exclusive. Enabled by per-object material alpha
    //   (LOD fade), vertex alpha, or explicit blend textures. A fading masked
    //   tree therefore does BOTH: the test kills transparent leaf texels while
    //   blend smooths the surviving opaque pixels -> no dark cards, no collapse.
    // - zWriteEnable: follow the game's z-write state. Only drop it for purely
    //   translucent blends that are NOT alpha-tested (glass/water/shadow),
    //   exactly as stock RW keeps masked foliage writing depth.
    bool maskedCutout = (hasTexAlpha || forcedKnownAlpha ||
                         isLikelyFoliageCutoutTexture(texName)) &&
                        !preferTexBlend;
    bool doBlend = hasMatAlpha || vertexAlpha || preferTexBlend;
    bool doAlphaTest = maskedCutout;
    bool zWriteEnable = gxState.zWrite && !(doBlend && !doAlphaTest);
    bool twoSided = isLikelyRoomShellDoubleSided(texName) ||
                    isLikelyThinTwoSidedTexture(texName) ||
                    isLikelyVegetationTwoSidedTexture(texName) ||
                    (doAlphaTest && preferTexCutout);
    // RW world/building meshes arrive with the opposite winding from GX's
    // cull convention in this pipe. Thin alpha planes stay double-sided.
    u8 cullMode = twoSided ? GX_CULL_NONE : flipCullMode(gxCullFromState());
    if(twoSided && s_cullDiagCount < 24){
        printf("[PIPE-CULL] tex=%s two-sided (rw=%u gx=%u texA=%d blend=%d cutout=%d)\n",
               texName ? texName : "none",
               (unsigned)gxState.cullMode,
               (unsigned)cullMode,
               hasTexAlpha ? 1 : 0,
               preferTexBlend ? 1 : 0,
               preferTexCutout ? 1 : 0);
        s_cullDiagCount++;
    }
    GX_SetCullMode(cullMode);

    bool zAfterTexturing = doAlphaTest;
    GX_SetZCompLoc(zAfterTexturing ? GX_FALSE : GX_TRUE);
    GX_SetZMode(gxState.zTest ? GX_TRUE : GX_FALSE, GX_LEQUAL,
                zWriteEnable ? GX_TRUE : GX_FALSE);
    GX_SetBlendMode(doBlend ? GX_BM_BLEND : GX_BM_NONE,
                    (u8)gxState.srcBlend,
                    (u8)gxState.dstBlend,
                    GX_LO_CLEAR);

    // �?GX_FALSE + GX_SRC_REG: bypass lighting, pass register color directly to TEV
    // GX_TRUE would trigger MatColor × AmbColor where AmbColor defaults to black!
    GX_SetChanMatColor(GX_COLOR0A0, matColor);
    GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE,
                   GX_SRC_REG,
                   useVertexMaterialColor ? GX_SRC_VTX : GX_SRC_REG,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumChans(1);

    if(doAlphaTest) {
        u8 alphaRef = (u8)gxState.alphaTestRef;
        u8 alphaFunc = gxAlphaFuncFromState(gxState.alphaTestFunc);
        if(isLikelyFoliageCutoutTexture(texName)) {
            // Runtime-converted RGBA8 leaf atlases contain a lot of mid-alpha
            // coverage. A near-zero cutoff keeps the whole billboard card
            // alive and shows the transparent rectangle around palms. Tighten
            // the authored foliage atlases more aggressively, but keep a lower
            // fallback for the generic foliage family so we don't collapse the
            // mask back into thin strips.
            if(needsTighterVegetationCutoutRef(texName))
                alphaRef = texFmt == GX_TF_CMPR ? 18 : 40;
            else
                alphaRef = texFmt == GX_TF_CMPR ? 10 : 16;
            alphaFunc = GX_GEQUAL;
        } else if(alphaRef < 10)
            alphaRef = 10;
        GX_SetAlphaCompare(alphaFunc,
                           alphaRef,
                           GX_AOP_AND, GX_ALWAYS, 0);
    } else
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    if((doBlend || doAlphaTest || vertexAlpha || forcedKnownAlpha) &&
       (forcedKnownAlpha ||
        s_alphaDiagCount < 20 ||
        (alphaTraceTexture(texName) && s_alphaDiagCount < 80))){
        printf("[PIPE-ALPHA] tex=%s fmt=0x%02X texObj=%d matA=%u texA=%d vtxA=%d blend=%d cutout=%d prefBlend=%d edgeBlend=%d "
               "aFn=%d aRef=%d srcB=%d dstB=%d zWrite=%d zAfterTex=%d cull=%u prelit=%d cutoutHint=%d masked=%d forceA=%d\n",
               texName ? texName : "none",
               (unsigned)texFmt,
               texObjValid ? 1 : 0,
               (unsigned)matColor.a,
               hasTexAlpha ? 1 : 0,
               vertexAlpha ? 1 : 0,
               doBlend ? 1 : 0,
               doAlphaTest ? 1 : 0,
               preferTexBlend ? 1 : 0,
               preferEdgeBlendCutout ? 1 : 0,
               (int)gxState.alphaTestFunc,
               (int)gxState.alphaTestRef,
               (int)gxState.srcBlend,
               (int)gxState.dstBlend,
               zWriteEnable ? 1 : 0,
               zAfterTexturing ? 1 : 0,
               (unsigned)cullMode,
               hasVertexColors ? 1 : 0,
               preferTexCutout ? 1 : 0,
               maskedCutout ? 1 : 0,
               forcedKnownAlpha ? 1 : 0);
        if(!forcedKnownAlpha)
            s_alphaDiagCount++;
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
            return;
        }
    }

    // No valid texture �?solid color via PASSCLR
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL,
                   GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}


// ── GX_DIRECT vertex descriptor (same as gxskin.cpp + Im2D) ─

static void
setup3dVtxDesc(bool hasNormals, bool hasColors, uint32 numTex)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    if (hasNormals) {
        GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    }
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
        // Always send white �?GX_PASSCLR/GX_MODULATE needs vertex color
        GX_Color4u8(255, 255, 255, 255);
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

    if (hasNrm) {
        const V3d *nrm = &morph->normals[vi];
        GX_Normal3f32(nrm->x, nrm->y, nrm->z);
    }

    if (hasCol && geo->colors) {
        RGBA *c = &geo->colors[vi];
        GX_Color4u8(c->red, c->green, c->blue, c->alpha);
    } else {
        GX_Color4u8(255, 255, 255, 255);
    }

    if (numTex > 0 && geo->texCoords[0]) {
        TexCoords *tc = &geo->texCoords[0][vi];
        GX_TexCoord2f32(tc->u, tc->v);
    }
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

    // ── GX_DIRECT vertex format (proven working, same as gxskin.cpp + Im2D) ──
    setup3dVtxDesc(inst->hasNormals, inst->hasColors, inst->numTexCoords);

    // Draw meshes
    uint8 prim = (geo->meshHeader->flags & MeshHeader::TRISTRIP)
                 ? GX_TRIANGLESTRIP : GX_TRIANGLES;
    Mesh *mesh = geo->meshHeader->getMeshes();

    GX_InvVtxCache();

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
        static int s_meshFocusDiagCount = 0;
        if(pipeFocusTexture(meshTexName) && s_meshFocusDiagCount < 160){
            printf("[PIPE-MESH] mesh=%u tex=%s vtxA=%d effVtxA=%d matA=%u numIdx=%u\n",
                   (unsigned)m,
                   meshTexName ? meshTexName : "none",
                   md->vertexAlpha ? 1 : 0,
                   effectiveVertexAlpha ? 1 : 0,
                   md->material ? (unsigned)md->material->color.alpha : 255u,
                   (unsigned)mesh->numIndices);
            s_meshFocusDiagCount++;
        }
        if(trace)
            printf("[PIPE-TRACE] mesh-begin geo=%p mesh=%u numIdx=%u mat=%p tex=%s\n",
                   (void*)geo, (unsigned)m, (unsigned)mesh->numIndices,
                   (void*)md->material,
                   (md->material && md->material->texture) ? md->material->texture->name : "none");
        SetRenderState(VERTEXALPHA, meshNeedsBlendAlphaState);
        setMaterial(md->material, effectiveVertexAlpha, inst->hasColors);

        uint16 *meshIdx = mesh->indices;
        uint32  numIdx  = mesh->numIndices;
        if (numIdx == 0) continue;
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

        if(trace)
            printf("[PIPE-TRACE] gx-begin geo=%p mesh=%u prim=%u numIdx=%u\n",
                   (void*)geo, (unsigned)m, (unsigned)prim, (unsigned)numIdx);
        GX_Begin(prim, GX_VTXFMT0, (u16)numIdx);
        for (uint32 i = 0; i < numIdx; i++)
            submitPipeVertexRaw(geo, meshIdx[i],
                                inst->hasNormals, inst->hasColors,
                                inst->numTexCoords);
        GX_End();
        if(trace)
            printf("[PIPE-TRACE] gx-end geo=%p mesh=%u\n", (void*)geo, (unsigned)m);
    }
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
