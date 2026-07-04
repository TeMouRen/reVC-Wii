// vendor/librw/src/gx/gxim2d_fixed.cpp
// Wii/GameCube GX immediate-mode 2D rendering.
#ifdef GAMECUBE

#include <gccore.h>
#include <string.h>
#include <stdio.h>

#include "../rwbase.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "rwgx.h"

namespace rw {
namespace gx {

static uint8 primToGX[] = {
    0,
    GX_LINES,
    GX_LINESTRIP,
    GX_TRIANGLES,
    GX_TRIANGLESTRIP,
    GX_TRIANGLEFAN,
    GX_POINTS,
};

static bool s_im2dProjReady = false;
static Mtx44 s_im2dProj;
static Mtx s_im2dView;
static Im2DVertex s_tmpVerts[3];
static int s_radarIm2dLogBudget = 120;

static bool
isRadarIm2DCall(const Im2DVertex *v, int32 n)
{
    if(v == nil || n < 3)
        return false;

    float minX = v[0].x, maxX = v[0].x;
    float minY = v[0].y, maxY = v[0].y;
    for(int32 i = 1; i < n; i++){
        if(v[i].x < minX) minX = v[i].x;
        if(v[i].x > maxX) maxX = v[i].x;
        if(v[i].y < minY) minY = v[i].y;
        if(v[i].y > maxY) maxY = v[i].y;
    }

    return minX >= 20.0f && maxX <= 160.0f && minY >= 330.0f && maxY <= 470.0f;
}

static bool
isDepthOnlyIm2DPass(void)
{
    return gxState.textures[0] == nil &&
           gxState.zWrite &&
           !gxState.zTest &&
           gxState.srcBlend == GX_BL_ZERO &&
           gxState.dstBlend == GX_BL_ONE;
}

static void
logRadarIm2D(const char *kind, PrimitiveType primType, const Im2DVertex *v, int32 n)
{
    if(s_radarIm2dLogBudget <= 0 || !isRadarIm2DCall(v, n))
        return;

    float minX = v[0].x, maxX = v[0].x;
    float minY = v[0].y, maxY = v[0].y;
    float minZ = v[0].z, maxZ = v[0].z;
    uint32 color0 = v[0].color;
    for(int32 i = 1; i < n; i++){
        if(v[i].x < minX) minX = v[i].x;
        if(v[i].x > maxX) maxX = v[i].x;
        if(v[i].y < minY) minY = v[i].y;
        if(v[i].y > maxY) maxY = v[i].y;
        if(v[i].z < minZ) minZ = v[i].z;
        if(v[i].z > maxZ) maxZ = v[i].z;
    }

    printf("[GX-IM2D-RADAR] f=%u %s prim=%d n=%d bbox=(%.1f,%.1f)-(%.1f,%.1f) z=(%.6f,%.6f) color0=0x%08X tex=%p zT=%d zW=%d src=%u dst=%u depthOnly=%d\n",
           gxFrameNum, kind, (int)primType, (int)n, minX, minY, maxX, maxY,
           minZ, maxZ, color0, gxState.textures[0],
           gxState.zTest ? 1 : 0, gxState.zWrite ? 1 : 0,
           (unsigned)gxState.srcBlend, (unsigned)gxState.dstBlend,
           isDepthOnlyIm2DPass() ? 1 : 0);
    s_radarIm2dLogBudget--;
}

static void
im2dSetup(void)
{
    if(!s_im2dProjReady){
        // RenderWare Im2D uses a top-left origin in screen pixels.
        guOrtho(s_im2dProj, 0.0f, 480.0f, 0.0f, 640.0f, -1000.0f, 1000.0f);
        guMtxIdentity(s_im2dView);
        s_im2dProjReady = true;
    }

    GX_LoadProjectionMtx(s_im2dProj, GX_ORTHOGRAPHIC);
    GX_LoadPosMtxImm(s_im2dView, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_SetCullMode(GX_CULL_NONE);

    // TEX0 is always declared because submitVertex always emits GX_TexCoord2f32.
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    bool hasTexture = gxState.textures[0] != nil;

    // VC's radar mask depends on RW render states: ZERO/ONE blend writes only
    // depth for the outside-circle mask, then later map tiles use Z testing.
    GX_SetZMode((gxState.zTest || gxState.zWrite) ? GX_TRUE : GX_FALSE,
                gxState.zTest ? GX_LEQUAL : GX_ALWAYS,
                gxState.zWrite ? GX_TRUE : GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND,
                    (u8)gxState.srcBlend,
                    (u8)gxState.dstBlend,
                    GX_LO_CLEAR);

    GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE,
                   GX_SRC_VTX, GX_SRC_VTX,
                   GX_LIGHT0, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumChans(1);

    if(hasTexture){
        GX_SetNumTexGens(1);
        GX_SetNumTevStages(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    }else{
        GX_SetNumTexGens(0);
        GX_SetNumTevStages(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }

    GX_InvVtxCache();
}

static inline void
submitVertex(const Im2DVertex *v)
{
    uint32 c = v->color;
    GX_Position3f32(v->x, v->y, v->z);
    GX_Color4u8((u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c);
    GX_TexCoord2f32(v->u, v->v);
}

void im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices);

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
    (void)numVertices;
    Im2DVertex *v = (Im2DVertex*)vertices;
    s_tmpVerts[0] = v[vert1];
    s_tmpVerts[1] = v[vert2];
    im2DRenderPrimitive(PRIMTYPELINELIST, s_tmpVerts, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
    (void)numVertices;
    Im2DVertex *v = (Im2DVertex*)vertices;
    s_tmpVerts[0] = v[vert1];
    s_tmpVerts[1] = v[vert2];
    s_tmpVerts[2] = v[vert3];
    im2DRenderPrimitive(PRIMTYPETRILIST, s_tmpVerts, 3);
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
    if(numVertices < 1 || primType >= sizeof(primToGX))
        return;

    Im2DVertex *v = (Im2DVertex*)vertices;
    logRadarIm2D("direct-before", primType, v, numVertices);
    im2dSetup();

    bool depthOnly = isDepthOnlyIm2DPass();
    if(depthOnly){
        GX_SetColorUpdate(GX_FALSE);
        GX_SetAlphaUpdate(GX_FALSE);
    }

    GX_Begin(primToGX[primType], GX_VTXFMT0, (u16)numVertices);
    for(int32 i = 0; i < numVertices; i++)
        submitVertex(&v[i]);
    GX_End();

    if(depthOnly){
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_TRUE);
    }
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
                           void *vertices, int32 numVertices,
                           void *indices, int32 numIndices)
{
    (void)numVertices;
    if(numIndices < 1 || primType >= sizeof(primToGX))
        return;

    Im2DVertex *v = (Im2DVertex*)vertices;
    logRadarIm2D("indexed-before", primType, v, numVertices);
    im2dSetup();

    bool depthOnly = isDepthOnlyIm2DPass();
    if(depthOnly){
        GX_SetColorUpdate(GX_FALSE);
        GX_SetAlphaUpdate(GX_FALSE);
    }

    uint16 *idx = (uint16*)indices;

    GX_Begin(primToGX[primType], GX_VTXFMT0, (u16)numIndices);
    for(int32 i = 0; i < numIndices; i++)
        submitVertex(&v[idx[i]]);
    GX_End();

    if(depthOnly){
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_TRUE);
    }
}

} // namespace gx
} // namespace rw

#endif // GAMECUBE
