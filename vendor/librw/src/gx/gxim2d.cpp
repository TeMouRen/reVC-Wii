// vendor/librw/src/gx/gxim2d.cpp
// Wii GX Immediate-Mode 2D Rendering
// Replaces the empty stubs in gxdevice.cpp
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

// ── Primitive type mapping: rw::PrimitiveType �?GX ────────────
static uint8 primToGX[] = {
    0,                  // 0 = PRIMTYPENONE (invalid)
    GX_LINES,           // 1 = PRIMTYPELINELIST
    GX_LINESTRIP,       // 2 = PRIMTYPEPOLYLINE
    GX_TRIANGLES,       // 3 = PRIMTYPETRILIST
    GX_TRIANGLESTRIP,   // 4 = PRIMTYPETRISTRIP
    GX_TRIANGLEFAN,     // 5 = PRIMTYPETRIFAN
    GX_POINTS,          // 6 = PRIMTYPEPOINTLIST
};

// ── 2D orthographic projection ────────────────────────────────
static bool  s_im2dProjReady = false;
static Mtx44 s_im2dProj;
static Mtx   s_im2dView;

// ── Scratch buffer for RenderLine / RenderTriangle ────────────
static Im2DVertex s_tmpVerts[3];

// ────────────────────────────────────────────────────────────────
// im2dSetup: load 2D ortho projection + configure TEV for Im2D
// ────────────────────────────────────────────────────────────────
static void
im2dSetup(void)
{
    if (!s_im2dProjReady) {
        // Top-left origin: Y=0 �?top, Y=480 �?bottom
        guOrtho(s_im2dProj, 0.0f, 480.0f, 0.0f, 640.0f, -1000.0f, 1000.0f);
        guMtxIdentity(s_im2dView);
        s_im2dProjReady = true;
    }

    GX_LoadProjectionMtx(s_im2dProj, GX_ORTHOGRAPHIC);
    GX_LoadPosMtxImm(s_im2dView, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
		GX_SetCullMode(GX_CULL_NONE);  // 2D never needs backface culling

    // Re-assert vertex descriptor (may have been altered by 3D pipeline)
    // TEX0 is always declared because submitVertex always emits GX_TexCoord2f32.
    // Omitting it when hasTexture==false would corrupt the FIFO.
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);

    // TEV path still needs to know whether a texture is bound
    bool hasTexture = (gxState.textures[0] != nullptr);

    // �?2D rendering: Z test OFF
    GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    // �?2D: pikmin exact �?GX_FALSE channel + light_mask=1
    GX_SetChanCtrl(GX_COLOR0A0, GX_FALSE,
                   GX_SRC_VTX, GX_SRC_VTX,
                   GX_LIGHT0, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumChans(1);

    if (hasTexture) {
        GX_SetNumTexGens(1);
        GX_SetNumTevStages(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    } else {
        GX_SetNumTexGens(0);
        GX_SetNumTevStages(1);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }

#if 0
    static int _diagCnt = 0;
    if (_diagCnt < 3) {
        printf("[DIAG] im2dSetup #%d: tex=%p zMode=OFF blend=ON matColor=WHITE\n",
               _diagCnt, (void*)gxState.textures[0]);
        _diagCnt++;
    }
#endif

    GX_InvVtxCache();
}

// ────────────────────────────────────────────────────────────────
// submitVertex: packed RGBA (color = R<<24|G<<16|B<<8|A) �?GX
// 诊断输出已上移到 im2DRenderPrimitive (统一 dump 整个 quad)
// ────────────────────────────────────────────────────────────────
static inline void
submitVertex(const Im2DVertex *v)
{
    uint32 c = v->color;
    GX_Position3f32(v->x, v->y, v->z);
    GX_Color4u8((u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c);
    GX_TexCoord2f32(v->u, v->v);
}

// ════════════════════════════════════════════════════════════════
// Public Im2D API
// ════════════════════════════════════════════════════════════════

void im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices);

// ────────────────────────────────────────────────────────────────
// dumpDraw: 集中输出 quad / sprite / 字符的详细诊�?//   - 识别 quad (4 verts trifan/tristrip �?6 verts trilist)
//   - 计算 bounding box / 纯色判定 / 半透明判定
//   - dump 所有顶�?RGBA + UV + 屏幕坐标
//   - dump 当前 GxState 关键字段
// ────────────────────────────────────────────────────────────────
static void
dumpDraw(PrimitiveType primType, const Im2DVertex *v, int32 n, const char *tag)
{
#if 1
    static uint32 lastFrame = 0xFFFFFFFFu;
    static int    callsThisFrame = 0;
    if (gxFrameNum != lastFrame) { lastFrame = gxFrameNum; callsThisFrame = 0; }
    if (callsThisFrame >= 50) return;

    bool hasTex = (gxState.textures[0] != nullptr);

    // Bounding box & solid color check
    float minX = v[0].x, maxX = v[0].x, minY = v[0].y, maxY = v[0].y;
    uint32 c0 = v[0].color;
    bool solid = true;
    int  uniqColors = 1;
    for (int32 i = 1; i < n; i++) {
        if (v[i].x < minX) minX = v[i].x;
        if (v[i].x > maxX) maxX = v[i].x;
        if (v[i].y < minY) minY = v[i].y;
        if (v[i].y > maxY) maxY = v[i].y;
        if (v[i].color != c0) { solid = false; uniqColors++; }
    }

    u8 r0 = (u8)(c0>>24), g0 = (u8)(c0>>16), b0 = (u8)(c0>>8), a0 = (u8)c0;
    int w = (int)(maxX - minX);
    int h = (int)(maxY - minY);
    bool isSemi  = (a0 < 255);
    bool isLarge = (w > 200 && h > 100);
    bool isQuad  = (n == 4 || n == 6);

    // 过滤掉无信息量的情况
    bool boring = (!hasTex && solid && r0 == 0  && g0 == 0  && b0 == 0)         // 纯黑实心矩形
               || (!hasTex && solid && r0 == 30 && g0 == 30 && b0 == 30);       // 阴影文字底色
    if (boring && callsThisFrame > 5) return;

    const char *kind = "MISC";
    if (!hasTex && solid && isLarge && isSemi)             kind = "BG_SEMI";
    else if (!hasTex && solid && isLarge)                  kind = "BG_OPAQ";
    else if (!hasTex && solid)                             kind = "RECT";
    else if (hasTex && isQuad)                             kind = "SPRITE";
    else if (hasTex)                                       kind = "TEX_STRIP";

    printf("[DRAW] f%u #%d %s %s prim=%d n=%d wh=%dx%d tex=%p uniq=%d\n",
           gxFrameNum, callsThisFrame, tag, kind, (int)primType, (int)n,
           w, h, gxState.textures[0], uniqColors);

    printf("       state: srcB=%d dstB=%d  zT=%d zW=%d  aF=%d aR=%d  vA=%d fog=%d cull=%d\n",
           gxState.srcBlend, gxState.dstBlend,
           gxState.zTest ? 1 : 0, gxState.zWrite ? 1 : 0,
           gxState.alphaTestFunc, gxState.alphaTestRef,
           gxState.vertexAlpha ? 1 : 0,
           gxState.fog ? 1 : 0,
           gxState.cullMode);

    // dump vertices
    int dumpN = (n <= 8) ? n : 4;
    for (int i = 0; i < dumpN; i++) {
        uint32 c = v[i].color;
        printf("       v%d (%6.1f,%6.1f,%6.2f) rgba=0x%08X (%3d,%3d,%3d,%3d) uv=(%6.3f,%6.3f)\n",
               i, v[i].x, v[i].y, v[i].z,
               c,
               (int)(u8)(c>>24), (int)(u8)(c>>16),
               (int)(u8)(c>>8),  (int)(u8)c,
               v[i].u, v[i].v);
    }

    callsThisFrame++;
#endif
}

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
    Im2DVertex *v = (Im2DVertex*)vertices;
    s_tmpVerts[0] = v[vert1];
    s_tmpVerts[1] = v[vert2];
    im2DRenderPrimitive(PRIMTYPELINELIST, s_tmpVerts, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices,
                   int32 vert1, int32 vert2, int32 vert3)
{
    Im2DVertex *v = (Im2DVertex*)vertices;
    s_tmpVerts[0] = v[vert1];
    s_tmpVerts[1] = v[vert2];
    s_tmpVerts[2] = v[vert3];
    im2DRenderPrimitive(PRIMTYPETRILIST, s_tmpVerts, 3);
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
    if (numVertices < 1 || primType >= sizeof(primToGX)) return;

    //dumpDraw(primType, (Im2DVertex*)vertices, numVertices, "DIRECT");

    im2dSetup();

    Im2DVertex *v = (Im2DVertex*)vertices;
    GX_Begin(primToGX[primType], GX_VTXFMT0, (u16)numVertices);
    for (int32 i = 0; i < numVertices; i++)
        submitVertex(&v[i]);
    GX_End();
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
                           void *vertices, int32 numVertices,
                           void *indices, int32 numIndices)
{
    if (numIndices < 1 || primType >= sizeof(primToGX)) return;

    //dumpDraw(primType, (Im2DVertex*)vertices, numVertices, "INDEXED");

    im2dSetup();

    Im2DVertex *v = (Im2DVertex*)vertices;
    uint16     *idx = (uint16*)indices;

    GX_Begin(primToGX[primType], GX_VTXFMT0, (u16)numIndices);
    for (int32 i = 0; i < numIndices; i++)
        submitVertex(&v[idx[i]]);
    GX_End();
}

} // namespace gx
} // namespace rw

#endif // GAMECUBE
