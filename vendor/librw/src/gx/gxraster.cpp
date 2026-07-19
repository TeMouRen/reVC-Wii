// vendor/librw/src/gx/gxraster.cpp
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// Wii GX Native Raster
// 淇: "Failed to load TXD" 鈥?瀹炵幇瀹屾暣 rasterFromImage 鍙?Driver 鍥炶皟
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
#ifdef GAMECUBE

#include <gccore.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../d3d/rwd3d.h"
#include "rwgx.h"
#include "gxmemory.h"

namespace rw {
const char* debugGetCurrentConvertingTextureName(void);
namespace gx {

// 鈹€鈹€ 鍏ㄥ眬鍙橀噺 (rwgx.h 涓?extern 澹版槑) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
int32   nativeRasterOffset;
GxState gxState;

static uint8_t
gxWrapFromRwState(int32 addr)
{
    switch(addr) {
    case Texture::WRAP:   return GX_REPEAT;
    case Texture::MIRROR: return GX_MIRROR;
    case Texture::CLAMP:  return GX_CLAMP;
    default:              return GX_CLAMP;
    }
}

static void
gxFilterFromRwState(int32 filter, uint8_t *minFilter, uint8_t *magFilter)
{
    uint8_t minf = GX_LINEAR;
    uint8_t magf = GX_LINEAR;

    switch(filter) {
    case Texture::NEAREST:
    case Texture::MIPNEAREST:
        minf = GX_NEAR;
        magf = GX_NEAR;
        break;
    default:
        break;
    }

    if(minFilter) *minFilter = minf;
    if(magFilter) *magFilter = magf;
}

static const char*
gxRasterTypeName(int32 type)
{
    switch(type) {
    case Raster::NORMAL:        return "NORMAL";
    case Raster::ZBUFFER:       return "ZBUFFER";
    case Raster::CAMERA:        return "CAMERA";
    case Raster::TEXTURE:       return "TEXTURE";
    case Raster::CAMERATEXTURE: return "CAMERATEXTURE";
    default:                    return "?";
    }
}

static uint8
classifyRGBAAlpha(const uint8 *pixels, int w, int h, int stride)
{
    if(pixels == nil || w <= 0 || h <= 0 || stride < w*4)
        return GX_RASTER_ALPHA_NONE;

    uint32 zeroAlpha = 0;
    uint32 fullAlpha = 0;
    uint32 smoothAlpha = 0;
    for(int y = 0; y < h; y++){
        const uint8 *row = pixels + y*stride;
        for(int x = 0; x < w; x++){
            uint8 alpha = row[x*4 + 3];
            if(alpha == 0)
                zeroAlpha++;
            else if(alpha == 255)
                fullAlpha++;
            else
                smoothAlpha++;
        }
    }

    if(zeroAlpha == 0 && smoothAlpha == 0)
        return GX_RASTER_ALPHA_NONE;
    if(smoothAlpha == 0)
        return GX_RASTER_ALPHA_CUTOUT;

    // Antialiased masks contain intermediate alpha at their contours, but
    // transparent and opaque endpoints still dominate the atlas. Classify
    // that distribution as CUTOUT without relying on a texture name. Smooth
    // glass, shadows and fades remain SMOOTH because intermediate alpha owns
    // most of their coverage.
    uint32 count = zeroAlpha + fullAlpha + smoothAlpha;
    if(zeroAlpha * 10u >= count &&
       fullAlpha * 20u >= count &&
       smoothAlpha * 5u <= count * 2u)
        return GX_RASTER_ALPHA_CUTOUT;
    return GX_RASTER_ALPHA_SMOOTH;
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

    for(const char *p = name; *p; p++) {
        const char *a = p;
        const char *b = needle;
        while(*a && *b && lowerAscii(*a) == lowerAscii(*b)) {
            a++;
            b++;
        }
        if(*b == '\0')
            return true;
    }
    return false;
}

static bool
isFocusedFoliageTexture(const char *name)
{
    if(!name || !name[0])
        return false;

    return nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "bark") ||
           nameContainsNoCase(name, "trunk") ||
           nameContainsNoCase(name, "branch") ||
           nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "frond") ||
           nameContainsNoCase(name, "plant");
}

static bool
shouldLogD3DTextureDecision(const char *name);

static bool
shouldPreferOwnSamplerState(const char *name)
{
    if(!name || !name[0])
        return false;

    return isFocusedFoliageTexture(name) ||
           strcmp(name, "kbtree4_test") == 0 ||
           strcmp(name, "kbtree3_test") == 0 ||
           strcmp(name, "newtreeleaves128") == 0 ||
           strcmp(name, "newtreeleavesb128") == 0 ||
           strcmp(name, "foliage256") == 0 ||
           strcmp(name, "planta256") == 0 ||
           strcmp(name, "plantb256") == 0 ||
           strcmp(name, "plantc256") == 0 ||
           strcmp(name, "fuzzyplant256") == 0 ||
           strcmp(name, "kbplanter_plants1") == 0 ||
           strcmp(name, "palmbark128") == 0;
}

static void
initNativeSamplerFromTexture(Texture *tex, GxRaster *natras)
{
    if(natras == nil)
        return;

    uint8_t wrapS = GX_CLAMP;
    uint8_t wrapT = GX_CLAMP;
    uint8_t minFilter = GX_LINEAR;
    uint8_t magFilter = GX_LINEAR;
    uint8_t preferOwnSampler = 0;

    if(tex) {
        wrapS = gxWrapFromRwState(tex->getAddressU());
        wrapT = gxWrapFromRwState(tex->getAddressV());
        gxFilterFromRwState(tex->getFilter(), &minFilter, &magFilter);
        preferOwnSampler = shouldPreferOwnSamplerState(tex->name) ? 1 : 0;
    }

    natras->wrapS = wrapS;
    natras->wrapT = wrapT;
    natras->minFilter = minFilter;
    natras->magFilter = magFilter;
    natras->preferOwnSampler = preferOwnSampler;

    static int s_samplerInitLogCount = 0;
    if(tex &&
       s_samplerInitLogCount < 96 &&
       isFocusedFoliageTexture(tex->name)) {
        printf("[GX-D3D-SAMPLER] %s: wrap=%u/%u filter=%u/%u rwAddr=0x%08X rwFilter=%d own=%d\n",
               tex->name,
               (unsigned)wrapS, (unsigned)wrapT,
               (unsigned)minFilter, (unsigned)magFilter,
               (unsigned)tex->filterAddressing,
               (int)tex->getFilter(),
               preferOwnSampler ? 1 : 0);
        s_samplerInitLogCount++;
    }
}

void
syncNativeSamplerFromTexture(Texture *tex, Raster *raster)
{
    if(tex == nil || raster == nil || raster->platform != PLATFORM_GX)
        return;

    GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
    if(natras == nil)
        return;

    initNativeSamplerFromTexture(tex, natras);
    invalidateTextureBinding(raster);

    if(natras->texObjValid){
        GX_InitTexObjWrapMode(&natras->texObj, natras->wrapS, natras->wrapT);
        GX_InitTexObjFilterMode(&natras->texObj, natras->minFilter, natras->magFilter);
    }

    static int s_samplerSyncLogCount = 0;
    if((shouldLogD3DTextureDecision(tex->name) || isFocusedFoliageTexture(tex->name)) &&
       s_samplerSyncLogCount < 160) {
        printf("[GX-SYNC-SAMPLER] %s: wrap=%u/%u filter=%u/%u own=%u valid=%u raster=%p\n",
               tex->name,
               (unsigned)natras->wrapS,
               (unsigned)natras->wrapT,
               (unsigned)natras->minFilter,
               (unsigned)natras->magFilter,
               (unsigned)natras->preferOwnSampler,
               natras->texObjValid ? 1u : 0u,
               (void*)raster);
        s_samplerSyncLogCount++;
    }
}

static bool
isKeyVegetationDebugTexture(const char *name)
{
    if(!name || !name[0])
        return false;

    return strcmp(name, "kbtree4_test") == 0 ||
           strcmp(name, "newtreeleaves128") == 0 ||
           strcmp(name, "foliage256") == 0 ||
           strcmp(name, "planta256") == 0 ||
           strcmp(name, "plantb256") == 0 ||
           strcmp(name, "plantc256") == 0 ||
           strcmp(name, "fuzzyplant256") == 0 ||
           strcmp(name, "kbtree3_test") == 0 ||
           strcmp(name, "palmbark128") == 0 ||
           strcmp(name, "weepalmshadow") == 0 ||
           strcmp(name, "bigpalmshadow") == 0;
}

static bool
isFocusedUiTexture(const char *name)
{
    if(!name || !name[0])
        return false;

    return nameContainsNoCase(name, "hud") ||
           nameContainsNoCase(name, "font") ||
           nameContainsNoCase(name, "radar") ||
           nameContainsNoCase(name, "map") ||
           nameContainsNoCase(name, "menu") ||
           nameContainsNoCase(name, "intro") ||
           nameContainsNoCase(name, "load") ||
           nameContainsNoCase(name, "splash");
}

static bool
shouldLogD3DTextureDecision(const char *name)
{
    return isKeyVegetationDebugTexture(name) ||
           isFocusedFoliageTexture(name) ||
           isFocusedUiTexture(name) ||
           nameContainsNoCase(name, "shadow");
}

static int
bleedTransparentRGBAEdges(uint8 *pixels, int w, int h, int stride,
                          uint8 alphaThreshold, int passes)
{
    if(pixels == nil || w <= 0 || h <= 0 || stride < w*4 || passes <= 0)
        return 0;

    int totalTouched = 0;
    uint8 *scratch = (uint8*)rwMalloc((size_t)stride * (size_t)h,
                                      MEMDUR_FUNCTION | ID_DRIVER);
    if(scratch == nil)
        return 0;

    for(int pass = 0; pass < passes; pass++){
        memcpy(scratch, pixels, (size_t)stride * (size_t)h);
        int touchedThisPass = 0;

        for(int y = 0; y < h; y++){
            for(int x = 0; x < w; x++){
                uint8 *dst = pixels + y*stride + x*4;
                if(dst[3] > alphaThreshold)
                    continue;

                int r = 0, g = 0, b = 0, weight = 0;
                for(int oy = -1; oy <= 1; oy++){
                    int ny = y + oy;
                    if(ny < 0 || ny >= h)
                        continue;
                    for(int ox = -1; ox <= 1; ox++){
                        int nx = x + ox;
                        if((ox == 0 && oy == 0) || nx < 0 || nx >= w)
                            continue;

                        const uint8 *src = scratch + ny*stride + nx*4;
                        if(src[3] <= alphaThreshold)
                            continue;

                        int sampleWeight = src[3];
                        r += src[0] * sampleWeight;
                        g += src[1] * sampleWeight;
                        b += src[2] * sampleWeight;
                        weight += sampleWeight;
                    }
                }

                if(weight == 0)
                    continue;

                dst[0] = (uint8)(r / weight);
                dst[1] = (uint8)(g / weight);
                dst[2] = (uint8)(b / weight);
                touchedThisPass++;
            }
        }

        totalTouched += touchedThisPass;
        if(touchedThisPass == 0)
            break;
    }

    rwFree(scratch);
    return totalTouched;
}

static void
traceUnnamed512RGBA8(const char *source, Raster *raster, uint32 dataSize)
{
#ifdef WII
    if(raster &&
       raster->width == 512 &&
       raster->height == 512 &&
       dataSize == 1024u * 1024u) {
        static int s_traceLogCount = 0;
        if(s_traceLogCount < 32) {
            printf("[GX-TRACE512] source=%s raster=%p type=%d flags=0x%X fmt=0x%X %dx%d size=%u\n",
                   source ? source : "<unknown>",
                   (void*)raster,
                   raster->type,
                   raster->flags,
                   raster->format,
                   raster->width,
                   raster->height,
                   (unsigned)dataSize);
            s_traceLogCount++;
        }
    }
#endif
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// 鍐呴儴宸ュ叿
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲

// GX RGBA8 tiled 鎵€闇€瀛楄妭鏁? 姣忎釜 4脳4 tile = 64 bytes
uint32
rgba8TiledSize(int w, int h)
{
    int tw = (w + 3) & ~3;
    int th = (h + 3) & ~3;
    return (uint32)(tw * th * 4);
}

uint32
ia4TiledSize(int w, int h)
{
    int tw = (w + 7) & ~7;
    int th = (h + 3) & ~3;
    return (uint32)(tw * th);
}

static uint32
cmprTiledSize(int w, int h)
{
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    int mbw = (bw + 1) / 2;
    int mbh = (bh + 1) / 2;
    return (uint32)(mbw * mbh * 32);
}

static uint8
dxt1SelectorByteToGX(uint8 b)
{
    return (uint8)(((b & 0x03u) << 6) |
                   ((b & 0x0Cu) << 2) |
                   ((b & 0x30u) >> 2) |
                   ((b & 0xC0u) >> 6));
}

static void
tileDXT1ToCMPR(uint8 *dst, const uint8 *src, int w, int h)
{
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    int mbw = (bw + 1) / 2;
    int mbh = (bh + 1) / 2;

    for(int mby = 0; mby < mbh; mby++) {
        for(int mbx = 0; mbx < mbw; mbx++) {
            for(int by = 0; by < 2; by++) {
                for(int bx = 0; bx < 2; bx++) {
                    int srcbx = mbx * 2 + bx;
                    int srcby = mby * 2 + by;
                    if(srcbx < bw && srcby < bh) {
                        int srcOff = (srcby * bw + srcbx) * 8;
                        dst[0] = src[srcOff + 0];
                        dst[1] = src[srcOff + 1];
                        dst[2] = src[srcOff + 2];
                        dst[3] = src[srcOff + 3];
                        dst[4] = dxt1SelectorByteToGX(src[srcOff + 4]);
                        dst[5] = dxt1SelectorByteToGX(src[srcOff + 5]);
                        dst[6] = dxt1SelectorByteToGX(src[srcOff + 6]);
                        dst[7] = dxt1SelectorByteToGX(src[srcOff + 7]);
                        dst += 8;
                    } else {
                        for(int j = 0; j < 8; j++)
                            *dst++ = 0;
                    }
                }
            }
        }
    }
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
// convertRGBA8_to_GX
// 绾挎€?RGBA8888 鈫?GX RGBA8 Tiled (4脳4 tile, AR/GB 鍒嗙)
//
// GX RGBA8 tile 鍐呭瓨甯冨眬 (姣?tile 64 bytes):
//   瀛楄妭  0-31: 4脳4 鍍忕礌鐨?AR 閫氶亾 (A,R,A,R,... 姣忓儚绱?2 bytes)
//   瀛楄妭 32-63: 4脳4 鍍忕礌鐨?GB 閫氶亾 (G,B,G,B,... 姣忓儚绱?2 bytes)
// 鍙傝€? opengx gc_gl.c scramble 瀹炵幇
// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void
convertRGBA8_to_GX(void *dst, const void *src, int w, int h, int srcStride)
{
    uint8       *d = (uint8*)dst;
    const uint8 *s = (const uint8*)src;
    int          rowBytes = (srcStride > 0) ? srcStride : w * 4;

    for(int by = 0; by < h; by += 4) {
        for(int bx = 0; bx < w; bx += 4) {

            // 鈹€鈹€ AR 瀛愬潡 (32 bytes) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
            for(int iy = 0; iy < 4; iy++) {
                int py = (by + iy < h) ? by + iy : h - 1;
                for(int ix = 0; ix < 4; ix++) {
                    int         px = (bx + ix < w) ? bx + ix : w - 1;
                    const uint8 *p = s + py * rowBytes + px * 4;
                    *d++ = p[3]; // A
                    *d++ = p[0]; // R
                }
            }

            // 鈹€鈹€ GB 瀛愬潡 (32 bytes) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
            for(int iy = 0; iy < 4; iy++) {
                int py = (by + iy < h) ? by + iy : h - 1;
                for(int ix = 0; ix < 4; ix++) {
                    int         px = (bx + ix < w) ? bx + ix : w - 1;
                    const uint8 *p = s + py * rowBytes + px * 4;
                    *d++ = p[1]; // G
                    *d++ = p[2]; // B
                }
            }
        }
    }
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// rasterCreate 鈥?鍒濆鍖?GX raster 鍏冩暟鎹?
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
Raster* // <--- 淇敼1锛氳繑鍥炲€兼敼涓?Raster*
rasterCreate(Raster *raster)
{
    GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);

    // 闆跺垵濮嬪寲,淇濊瘉瀹夊叏
    natras->gxData      = nullptr;
    natras->cpuData     = nullptr;
    natras->texObjValid = false;
    natras->dirty       = false;
    natras->dataSize    = 0;
    natras->w           = 0;
    natras->h           = 0;
    natras->hasAlpha    = 0;
    natras->alphaKind   = GX_RASTER_ALPHA_NONE;
    natras->wrapS       = GX_CLAMP;
    natras->wrapT       = GX_CLAMP;
    natras->minFilter   = GX_LINEAR;
    natras->magFilter   = GX_LINEAR;
    natras->preferOwnSampler = 0;

    if(raster->width <= 0 || raster->height <= 0) return raster;

    int w = raster->width;
    int h = raster->height;

    natras->w        = (uint16)w;
    natras->h        = (uint16)h;
    natras->gxFmt    = GX_TF_RGBA8;
    natras->hasAlpha = 0;
    natras->alphaKind = GX_RASTER_ALPHA_NONE;
    natras->dataSize = rgba8TiledSize(w, h);

    return raster;
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// rasterDestroy
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
void
rasterDestroy(Raster *raster)
{
    invalidateTextureBinding(raster);
    GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
    texPoolUnregister(natras->gxData);
    gx::gxMemFree(natras->gxData);  natras->gxData  = nullptr;
    gx::gxMemFree(natras->cpuData); natras->cpuData = nullptr;
    natras->texObjValid = false;
    natras->dataSize    = 0;
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// rasterLock 鈥?杩斿洖 CPU 渚у儚绱犳寚閽?(鎳掑垎閰?cpuData)
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
uint8*
rasterLock(Raster *raster, int32 /*level*/, int32 lockMode)
{
    GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);

    // 鎳掑垎閰?cpuData锛堜粎鍦?Lock 鏃舵墠闇€瑕?CPU 绾挎€х紦鍐诧級
    if(!natras->cpuData && raster->width > 0 && raster->height > 0) {
        natras->cpuData = gx::safeGxAlloc((size_t)raster->width * raster->height * 4, 32, "raster-cpu");
        if(natras->cpuData)
            memset(natras->cpuData, 0, (size_t)raster->width * raster->height * 4);
    }

    if(!natras->cpuData) return nullptr;
    if(lockMode & Raster::LOCKWRITE)
        natras->dirty = true;
    return (uint8*)natras->cpuData;
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// rasterUnlock 鈥?CPU 鈫?GPU 鍚屾
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
void
rasterUnlock(Raster *raster, int32 /*level*/)
{
    GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
    if(!natras->dirty || !natras->cpuData) return;

    if(!natras->gxData) {
        natras->dataSize = rgba8TiledSize(raster->width, raster->height);
        traceUnnamed512RGBA8("rasterUnlock", raster, natras->dataSize);
        natras->gxData = gx::safeGxAlloc(natras->dataSize, 32, "raster-unlock");
        if(!natras->gxData) {
            printf("[GX] rasterUnlock: alloc fail %dx%d dataSize=%u\n",
                   raster->width, raster->height, natras->dataSize);
            natras->dataSize = 0;
            return;
        }
        gx::texPoolRegister(raster, natras->gxData, natras->dataSize,
                        (uint16)raster->width, (uint16)raster->height,
                        natras->gxFmt);
    }

    natras->alphaKind = classifyRGBAAlpha((const uint8*)natras->cpuData,
                                          raster->width, raster->height,
                                          raster->width * 4);
    natras->hasAlpha = natras->alphaKind != GX_RASTER_ALPHA_NONE ? 1 : 0;

    convertRGBA8_to_GX(natras->gxData, natras->cpuData,
                       raster->width, raster->height, 0);
    DCFlushRange(natras->gxData, natras->dataSize);
    GX_InvalidateTexAll();

    GX_InitTexObj(&natras->texObj,
                  natras->gxData,
                  (u16)raster->width, (u16)raster->height,
                  GX_TF_RGBA8,
                  natras->wrapS, natras->wrapT,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&natras->texObj, natras->minFilter, natras->magFilter);

    natras->texObjValid = true;
    natras->dirty       = false;
    gx::texPoolEnforceBudget("rasterUnlock");
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// rasterNumLevels
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
int32
rasterNumLevels(Raster * /*r*/)
{
    return 1;
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// rasterFromImage  鈼勨梽鈼?淇 "Failed to load TXD" 鐨勬牳蹇冨嚱鏁?
//
// 璋冪敤璺緞:
//   RwTexDictionaryStreamRead
//     鈫?RwTextureStreamRead
//       鈫?RasterStreamRead (librw 鍐呴儴)
//         鈫?engine->driver[platform].rasterFromImage(raster, image)
//                                    鈫?姝ゅ鑻ヤ负 null 鎴栬繑鍥?0
//                                      鈫?"Failed to load TXD"
//
// 杩斿洖鍊? 1 = 鎴愬姛 (蹇呴』闈為浂!)
//         0 = 澶辫触 鈫?librw 杈撳嚭 "Failed to load TXD" 骞跺洖閫€鏁ｅ浘鎼滅储
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
bool32
rasterFromImage(Raster *raster, Image *image)
{
    if(!raster || !image) {
        printf("[GX] rasterFromImage: null arg (raster=%p image=%p)\n",
               (void*)raster, (void*)image);
        return 0;
    }

    int w = raster->width;
    int h = raster->height;

    if(w <= 0 || h <= 0) {
        printf("[GX] rasterFromImage: invalid size %dx%d\n", w, h);
        return 0;
    }

    if(image->width != w || image->height != h) {
        printf("[GX] rasterFromImage: size mismatch raster=%dx%d image=%dx%d\n",
               w, h, image->width, image->height);
        return 0;
    }

    // 鈹€鈹€ 寮哄埗杞崲涓?RGBA8888 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    if(image->depth != 32) {
        image->convertTo32();
        if(image->depth != 32 || !image->pixels) {
            printf("[GX] rasterFromImage: convertTo32 failed "
                   "(depth=%d pixels=%p)\n",
                   image->depth, (void*)image->pixels);
            return 0;
        }
    }

    if(!image->pixels) {
        printf("[GX] rasterFromImage: no pixel data\n");
        return 0;
    }

    GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
    const char *debugName = rw::debugGetCurrentConvertingTextureName();
    uint8 imageAlphaKind = classifyRGBAAlpha(image->pixels, w, h, image->stride);
    if(imageAlphaKind == GX_RASTER_ALPHA_CUTOUT) {
        int bled = bleedTransparentRGBAEdges(image->pixels, w, h,
                                             image->stride, 127, 4);
        static int s_imageBleedLogCount = 0;
        if((isKeyVegetationDebugTexture(debugName) || bled > 0) &&
           s_imageBleedLogCount < 160) {
            printf("[GX-CUTOUT-BLEED] %s: touched=%d %dx%d stride=%d\n",
                   debugName ? debugName : "<null>",
                   bled, w, h, image->stride);
            s_imageBleedLogCount++;
        }
    }
    bool imageHasAlpha = imageAlphaKind != GX_RASTER_ALPHA_NONE;

    // 鈹€鈹€ 閲婃斁鏃?GPU 缂撳啿锛沜puData 涓嶅湪姝よ矾寰勫垎閰?鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    texPoolUnregister(natras->gxData);
    gx::gxMemFree(natras->gxData);  natras->gxData  = nullptr;
    // cpuData 鐢?rasterLock 鎳掑垎閰嶏紝姝ゅ涓嶈Е纰颁互鑺傜渷鍐呭瓨
    natras->texObjValid = false;

    natras->w        = (uint16)w;
    natras->h        = (uint16)h;
    natras->gxFmt    = GX_TF_RGBA8;
    natras->hasAlpha = imageHasAlpha ? 1 : 0;
    natras->alphaKind = imageAlphaKind;
    natras->dataSize = rgba8TiledSize(w, h);

    traceUnnamed512RGBA8("rasterFromImage", raster, natras->dataSize);

    natras->gxData  = gx::safeGxAlloc(natras->dataSize, 32, "image-raster");

    if(!natras->gxData) {
        printf("[GX] rasterFromImage: alloc fail %dx%d dataSize=%u\n",
               w, h, natras->dataSize);
        natras->dataSize = 0;
        return 0;
    }

    gx::texPoolRegister(raster, natras->gxData, natras->dataSize,
                    (uint16)w, (uint16)h, natras->gxFmt);

    // 鈹€鈹€ 鈽?librw Image::pixels 鏍囧噯鏍煎紡鏄?RGBA8888
    //     (pixels[0]=R, [1]=G, [2]=B, [3]=A, 鐢?convertTo32 / toImage 淇濊瘉),
    //     鐩存帴浼犵粰 convertRGBA8_to_GX, 涓嶈鍐?swap. 鍘嗗彶涓婄殑 swap 鎶?R鈫擝
    //     閿欎綅, 瀵艰嚧鐏拌壊鑳屾櫙鍋忕传銆乀XD 棰滆壊寮傚父銆?
    // 鈹€鈹€ DIAG: 璁板綍鍍忕礌鍊肩敤浜庨鑹茶皟璇?(disabled) 鈹€鈹€
#if 0
    {
        static int diagCnt = 0;
        if (diagCnt < 10) {
            int cx = w/2, cy = h/2;
            uint8 *sp = image->pixels + cy * image->stride + cx * 4;
            /*//printf("[TXD] #%d fmt=0x%03x %dx%d centerRGBA=(%d,%d,%d,%d)\n",
                   diagCnt, raster->format, w, h, sp[0], sp[1], sp[2], sp[3]);*/

            // 鎵弿绗竴涓潪閫忔槑鍍忕礌 (alpha>128)锛岀敤浜庤瘖鏂瓧浣撶汗鐞?
            uint8 *p = image->pixels;
            bool found = false;
            for (int i = 0; i < w*h && !found; i++, p += 4) {
                if (p[3] > 128) {
            /*//printf("[TXD] #%d firstSolid (idx=%d): RGBA=(%d,%d,%d,%d)\n",
                           diagCnt, i, p[0], p[1], p[2], p[3]);*/
                    found = true;
                }
            }
            if (!found)
                //printf("[TXD] #%d: NO solid pixel found (all alpha<=128)\n", diagCnt);
        }
        diagCnt++;
    }
#endif

    // 鈹€鈹€ 鐩存帴浠?image->pixels 杞崲鍒?GX tiled (浣跨敤 stride) 鈹€
    convertRGBA8_to_GX(natras->gxData, image->pixels, w, h, image->stride);
    DCFlushRange(natras->gxData, natras->dataSize);
    GX_InvalidateTexAll();

    // 鈹€鈹€ 鍒濆鍖?GXTexObj 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    GX_InitTexObj(&natras->texObj,
                  natras->gxData,
                  (u16)w, (u16)h,
                  GX_TF_RGBA8,
                  natras->wrapS, natras->wrapT,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&natras->texObj, natras->minFilter, natras->magFilter);

    natras->texObjValid = true;
    natras->dirty       = false;

    // Diagnostic: log first 8 successful texture uploads (disabled)
#if 0
    {
        static int txdCnt = 0;
        if (txdCnt < 8) {
            // Sample center pixel to verify color data
            uint8 *p = (uint8*)image->pixels + (h/2) * image->stride + (w/2) * 4;
            /*//printf("[TXD] upload#%d %dx%d gxData=%p centerPx RGBA=(%d,%d,%d,%d)\n",
                   txdCnt, w, h, natras->gxData, p[0], p[1], p[2], p[3]);*/
            txdCnt++;
        }
    }
#endif

    return 1;
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// rasterToImage 鈥?TXD 鍐欏洖 / 鎴浘 (鍙€?
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
Image*
rasterToImage(Raster *raster)
{
    GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
    if(!natras->cpuData) return nullptr;

    int w = raster->width;
    int h = raster->height;

    Image *img = Image::create(w, h, 32);
    if(!img) return nullptr;
    img->allocate();

    const uint8 *src = (const uint8*)natras->cpuData;
    for(int y = 0; y < h; y++)
        memcpy(img->pixels + y * img->stride,
               src + y * w * 4,
               (size_t)w * 4);
    return img;
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// gxSetTexture 鈥?缁戝畾 raster 鍒?GX 閲囨牱鍣ㄥ崟鍏?
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
void
gxSetTexture(void *rasterPtr, int32 unit)
{
    if(unit < 0 || unit >= GX_MAX_TEXMAP) return;

    Raster *raster = (Raster*)rasterPtr;
    void *effectivePtr = nullptr;
    if(raster) {
        GxRaster *natras = PLUGINOFFSET(GxRaster,
                                        raster,
                                        nativeRasterOffset);

        // 鈹€鈹€ DIAG: 鑺傛祦 dump 绾圭悊缁戝畾鍒囨崲 (disabled) 鈹€鈹€
#if 0
        {
            static int seen = 0;
            if (seen < 64) {
                printf("[TEX] f%u bind#%d unit=%d ras=%p sz=%dx%d valid=%d gxData=%p\n",
                       gxFrameNum, seen, (int)unit, rasterPtr,
                       raster->width, raster->height,
                       natras->texObjValid ? 1 : 0,
                       natras->gxData);
                seen++;
            }
        }
#endif

        if(natras->texObjValid) {
            effectivePtr = rasterPtr;
            uint8_t stateWrapS = gxWrapFromRwState(gxState.uAddr);
            uint8_t stateWrapT = gxWrapFromRwState(gxState.vAddr);
            uint8_t stateMinFilter, stateMagFilter;
            gxFilterFromRwState(gxState.texFilter, &stateMinFilter, &stateMagFilter);

            bool useOwnSampler = natras->preferOwnSampler != 0;
            uint8_t wrapS = useOwnSampler ? natras->wrapS : stateWrapS;
            uint8_t wrapT = useOwnSampler ? natras->wrapT : stateWrapT;
            uint8_t minFilter = useOwnSampler ? natras->minFilter : stateMinFilter;
            uint8_t magFilter = useOwnSampler ? natras->magFilter : stateMagFilter;

            static int s_samplerOverrideLogs = 0;
            if(useOwnSampler &&
               s_samplerOverrideLogs < 96 &&
               (wrapS != stateWrapS ||
                wrapT != stateWrapT ||
                minFilter != stateMinFilter ||
                magFilter != stateMagFilter)) {
                printf("[GX-SAMPLER] f%u unit=%d ras=%p %dx%d fmt=%u alpha=%u "
                       "tex=%u/%u/%u/%u state=%u/%u/%u/%u\n",
                       gxFrameNum, unit, rasterPtr,
                       raster->width, raster->height,
                       (unsigned)natras->gxFmt,
                       (unsigned)natras->hasAlpha,
                       (unsigned)wrapS, (unsigned)wrapT,
                       (unsigned)minFilter, (unsigned)magFilter,
                       (unsigned)stateWrapS, (unsigned)stateWrapT,
                       (unsigned)stateMinFilter, (unsigned)stateMagFilter);
                s_samplerOverrideLogs++;
            }

            if(gxState.textures[unit] == effectivePtr &&
               gxState.texMinFilter[unit] == minFilter &&
               gxState.texMagFilter[unit] == magFilter &&
               gxState.texWrapS[unit] == wrapS &&
               gxState.texWrapT[unit] == wrapT)
                return;

            gxState.textures[unit] = effectivePtr;
            gxState.texMinFilter[unit] = minFilter;
            gxState.texMagFilter[unit] = magFilter;
            gxState.texWrapS[unit] = wrapS;
            gxState.texWrapT[unit] = wrapT;

            GXTexObj texObj = natras->texObj;
            GX_InitTexObjFilterMode(&texObj, minFilter, magFilter);
            GX_InitTexObjWrapMode(&texObj, wrapS, wrapT);
            GX_LoadTexObj(&texObj, (u8)(GX_TEXMAP0 + unit));
            return;
        }

        static int s_invalidBindLogs = 0;
        if(s_invalidBindLogs < 24) {
            printf("[GX-TEXMISS] f%u unit=%d type=%s %dx%d valid=0 gxData=%p cpuData=%p\n",
                   gxFrameNum, unit, gxRasterTypeName(raster->type),
                   raster->width, raster->height,
                   natras->gxData, natras->cpuData);
            s_invalidBindLogs++;
        }
    }

    if(gxState.textures[unit] == effectivePtr) return;
    gxState.textures[unit] = effectivePtr;
    gxState.texMinFilter[unit] = 0;
    gxState.texMagFilter[unit] = 0;
    gxState.texWrapS[unit] = 0;
    gxState.texWrapT[unit] = 0;
}


void
invalidateTextureBinding(void *raster)
{
    for(int i = 0; i < GX_MAX_TEXMAP; i++) {
        if(gxState.textures[i] == raster) {
            gxState.textures[i] = nullptr;
            gxState.texMinFilter[i] = 0;
            gxState.texMagFilter[i] = 0;
            gxState.texWrapS[i] = 0;
            gxState.texWrapT[i] = 0;
        }
    }
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// Plugin 鐢熷懡鍛ㄦ湡鍥炶皟 (姣忎釜 Raster 瀵硅薄鍒嗛厤/閲婃斁鏃惰Е鍙?
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
static void*
createNativeRaster(void *object, int32 offset, int32 /*size*/)
{
    GxRaster *natras = PLUGINOFFSET(GxRaster, object, offset);
    memset(natras, 0, sizeof(GxRaster));
    natras->wrapS = GX_CLAMP;
    natras->wrapT = GX_CLAMP;
    natras->minFilter = GX_LINEAR;
    natras->magFilter = GX_LINEAR;
    natras->preferOwnSampler = 0;
    return object;
}

static void*
destroyNativeRaster(void *object, int32 /*offset*/, int32 /*size*/)
{
    Raster *raster = (Raster*)object;
    rasterDestroy(raster);
    return object;
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// registerNativeRaster
// 鍚?librw 娉ㄥ唽 GX 鍘熺敓 Raster 鎻掍欢
// 鐢?gxdevice.cpp DEVICEOPEN 璋冪敤
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
void
registerNativeRaster(void)
{
    nativeRasterOffset = Raster::registerPlugin(
        sizeof(GxRaster),
        ID_RASTERGX,
        createNativeRaster,
        destroyNativeRaster,
        nullptr
    );
    printf("[GX] Native raster registered (offset=%d)\n",
           nativeRasterOffset);
}

} // namespace gx

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// 鈹€鈹€ IA4 tiling helper 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
// Converts linear RGBA8 to GX IA4 tiled (8脳4 macroblocks, 32 bytes each).
// GX_TF_IA4 uses 8脳4 blocks, NOT 4脳4 like RGBA8.
// Each output byte = [A3..A0][I3..I0].
static void
convertRGBA8_to_IA4(void *dst, const uint8 *src, int w, int h, int srcStride)
{
    uint8 *d = (uint8*)dst;
    int rowBytes = (srcStride > 0) ? srcStride : w * 4;
    int pw = (w + 7) & ~7;  // pad width to multiple of 8 for IA4
    int ph = (h + 3) & ~3;  // pad height to multiple of 4

    for(int by = 0; by < ph; by += 4) {
        for(int bx = 0; bx < pw; bx += 8) {
            for(int iy = 0; iy < 4; iy++) {
                int py = (by + iy < h) ? by + iy : h - 1;
                for(int ix = 0; ix < 8; ix++) {
                    int px = (bx + ix < w) ? bx + ix : w - 1;
                    const uint8 *p = src + py * rowBytes + px * 4;
                    uint8 r = p[0], g = p[1], b = p[2], a = p[3];
                    uint8 intensity = (uint8)((r + g + b) / 3) >> 4;
                    uint8 alpha4   = a >> 4;
                    *d++ = (uint8)((alpha4 << 4) | intensity);
                }
            }
        }
    }
}

// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
// gxConvertRasterToNative
// Called from texture.cpp after streamReadNative.
// On GC with tight memory, auto-detects grayscale textures and uses
// IA4 (8bpp, 1/4 size of RGBA8) to avoid alloc failures.
// 鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲鈺愨晲
void gxConvertRasterToNative(Texture *tex)
{
    if(!tex || !tex->raster) return;
    Raster *src = tex->raster;
    if(src->platform == PLATFORM_GX) return;

    uint32 srcD3DFormat = 0;
    bool srcD3DCustomFormat = false;
    bool srcDeclaredAlpha = false;

    if(src->platform == PLATFORM_D3D8 || src->platform == PLATFORM_D3D9) {
        d3d::D3dRaster *d3dRas = GETD3DRASTEREXT(src);
        srcD3DFormat = d3dRas->format;
        srcD3DCustomFormat = d3dRas->customFormat != 0;
        srcDeclaredAlpha = d3dRas->hasAlpha != 0;
        if(d3dRas->customFormat && d3dRas->format == d3d::D3DFMT_DXT1) {
            uint8 *blocks = src->lock(0, Raster::LOCKREAD);
            if(blocks) {
                int w = src->width;
                int h = src->height;
                int32 fmt = (d3dRas->hasAlpha ? Raster::C1555 : Raster::C565) | Raster::TEXTURE;
                Raster *gxRas = Raster::create(w, h, 32, fmt, PLATFORM_GX);
                if(gxRas) {
                    gx::GxRaster *natras = PLUGINOFFSET(gx::GxRaster, gxRas, gx::nativeRasterOffset);
                    gx::initNativeSamplerFromTexture(tex, natras);
                    natras->w = (uint16)w;
                    natras->h = (uint16)h;
                    natras->gxFmt = GX_TF_CMPR;
                    natras->hasAlpha = d3dRas->hasAlpha ? 1 : 0;
                    natras->alphaKind = d3dRas->hasAlpha ?
                        gx::GX_RASTER_ALPHA_CUTOUT : gx::GX_RASTER_ALPHA_NONE;
                    natras->dataSize = gx::cmprTiledSize(w, h);
                    natras->gxData = gx::safeGxAlloc(natras->dataSize, 32, tex->name);
                    if(natras->gxData) {
                        if(gx::isFocusedFoliageTexture(tex->name)) {
                            static int s_d3dCmprLogCount = 0;
                            if(s_d3dCmprLogCount < 96) {
                                printf("[GX-D3D-CMPR] %s: D3D DXT1 -> GX CMPR selector swizzle (%dx%d alpha=%d)\n",
                                       tex->name, w, h, d3dRas->hasAlpha ? 1 : 0);
                                s_d3dCmprLogCount++;
                            }
                        }
                        gx::tileDXT1ToCMPR((uint8*)natras->gxData, blocks, w, h);
                        gx::texPoolRegister(gxRas, natras->gxData, natras->dataSize,
                                            (uint16)w, (uint16)h, natras->gxFmt,
                                            tex->name);
                        DCFlushRange(natras->gxData, natras->dataSize);
                        GX_InvalidateTexAll();
                        GX_InitTexObj(&natras->texObj, natras->gxData, (u16)w, (u16)h,
                                      natras->gxFmt, natras->wrapS, natras->wrapT, GX_FALSE);
                        GX_InitTexObjFilterMode(&natras->texObj, natras->minFilter, natras->magFilter);
                        natras->texObjValid = true;
                        natras->dirty = false;
                        src->unlock(0);
                        src->destroy();
                        tex->raster = gxRas;
                        gx::texPoolEnforceBudget(tex->name);
                        printf("[GX] gxConvertRasterToNative: using CMPR %dx%d size=%u '%s'\n",
                               w, h, natras->dataSize, tex->name);
                        return;
                    }
                    gxRas->destroy();
                }
                src->unlock(0);
            }
        }
    }

    Image *img = src->toImage();
    if(!img){
        printf("[GX] gxConvertRasterToNative: toImage failed '%s' plat=%d fmt=0x%x\n",
               tex->name, src->platform, src->format);
        return;
    }
    if(img->depth != 32)
        img->convertTo32();
    if(img->depth != 32 || !img->pixels){
        printf("[GX] gxConvertRasterToNative: convertTo32 failed '%s' depth=%d pixels=%p\n",
               tex->name, img->depth, (void*)img->pixels);
        img->destroy();
        return;
    }

    uint8 *pixels = img->pixels;
    int stride = img->stride;
    int w = img->width;
    int h = img->height;

    // ── DECODE PROBE ── dump decoded-image stats for key foliage atlases so we
    // can tell a bad texture decode (black/banded RGBA) apart from a downstream
    // geometry/render problem. Purely diagnostic; safe to remove.
    if(gx::isKeyVegetationDebugTexture(tex->name)){
        unsigned aLo = 0, aMid = 0, aHi = 0;           // alpha <8 / 8..250 / >250
        unsigned rMin = 255, gMin = 255, bMin = 255;
        unsigned rMax = 0, gMax = 0, bMax = 0;
        unsigned long rSum = 0, gSum = 0, bSum = 0, opaque = 0;
        for(int yy = 0; yy < h; yy++){
            const uint8 *row = pixels + yy * stride;
            for(int xx = 0; xx < w; xx++){
                const uint8 *p = row + xx * 4;
                uint8 r = p[0], g = p[1], b = p[2], a = p[3];
                if(a < 8) aLo++; else if(a > 250) aHi++; else aMid++;
                if(a >= 8){                            // only count visible texels for RGB
                    if(r < rMin) rMin = r; if(r > rMax) rMax = r;
                    if(g < gMin) gMin = g; if(g > gMax) gMax = g;
                    if(b < bMin) bMin = b; if(b > bMax) bMax = b;
                    rSum += r; gSum += g; bSum += b; opaque++;
                }
            }
        }
        unsigned rAvg = opaque ? (unsigned)(rSum / opaque) : 0;
        unsigned gAvg = opaque ? (unsigned)(gSum / opaque) : 0;
        unsigned bAvg = opaque ? (unsigned)(bSum / opaque) : 0;
        const uint8 *c = pixels + (h/2) * stride + (w/2) * 4;   // center texel
        printf("[GX-DECODEPROBE] %s: %dx%d aHist(lo/mid/hi)=%u/%u/%u vis=%lu "
               "rgbMin=%u,%u,%u rgbMax=%u,%u,%u rgbAvg=%u,%u,%u center=%u,%u,%u,%u\n",
               tex->name, w, h, aLo, aMid, aHi, opaque,
               rMin, gMin, bMin, rMax, gMax, bMax, rAvg, gAvg, bAvg,
               (unsigned)c[0], (unsigned)c[1], (unsigned)c[2], (unsigned)c[3]);
    }

    uint8 imageAlphaKind = gx::classifyRGBAAlpha(pixels, w, h, stride);
    if(imageAlphaKind == gx::GX_RASTER_ALPHA_CUTOUT){
        int bled = gx::bleedTransparentRGBAEdges(pixels, w, h, stride, 127, 4);
        if(bled > 0){
            static int s_alphaBleedLogCount = 0;
            if(gx::isKeyVegetationDebugTexture(tex->name) || s_alphaBleedLogCount < 96){
                printf("[GX-CUTOUT-BLEED] %s: touched=%d %dx%d\n",
                       tex->name, bled, w, h);
                if(!gx::isKeyVegetationDebugTexture(tex->name))
                    s_alphaBleedLogCount++;
            }
        }
    }

    bool imageHasAlpha = imageAlphaKind != gx::GX_RASTER_ALPHA_NONE;
    bool preserveAlphaDetail = srcDeclaredAlpha || imageHasAlpha;

    // Detect grayscale: sample center + 4 corners (skip if all-transparent)
    bool isGrayscale = true;
    int samples = 0;
    for(int sy = 0; sy < h && samples < 20; sy += h/5 + 1) {
        for(int sx = 0; sx < w && samples < 20; sx += w/5 + 1) {
            const uint8 *p = pixels + sy * stride + sx * 4;
            if(p[3] < 32) continue; // skip transparent pixels
            int rg = (int)p[0] - (int)p[1];
            int gb = (int)p[1] - (int)p[2];
            if(rg < -16 || rg > 16 || gb < -16 || gb > 16) {
                isGrayscale = false;
                break;
            }
            samples++;
        }
        if(!isGrayscale) break;
    }
    if(samples < 4)
        isGrayscale = false;

    bool allowIA4 = isGrayscale && !preserveAlphaDetail;
    bool forceDecisionLog = gx::isKeyVegetationDebugTexture(tex->name);
    if(gx::shouldLogD3DTextureDecision(tex->name)) {
        static int s_d3dDecisionLogCount = 0;
        if(forceDecisionLog || s_d3dDecisionLogCount < 192) {
            printf("[GX-D3D-TEX] %s: srcFmt=0x%08X custom=%d declA=%d imgA=%d gray=%d ia4=%d %dx%d\n",
                   tex->name,
                   (unsigned)srcD3DFormat,
                   srcD3DCustomFormat ? 1 : 0,
                   srcDeclaredAlpha ? 1 : 0,
                   imageHasAlpha ? 1 : 0,
                   isGrayscale ? 1 : 0,
                   allowIA4 ? 1 : 0,
                   w, h);
            if(!forceDecisionLog)
                s_d3dDecisionLogCount++;
        }
    }

    int32 fmt = Raster::C8888 | Raster::TEXTURE;
    Raster *gxRas = Raster::create(w, h, 32, fmt, PLATFORM_GX);
    if(!gxRas){
        printf("[GX] gxConvertRasterToNative: create failed\n");
        img->destroy();
        return;
    }

    gx::GxRaster *natras = PLUGINOFFSET(gx::GxRaster, gxRas, gx::nativeRasterOffset);
    gx::initNativeSamplerFromTexture(tex, natras);
    natras->w = (uint16)w;
    natras->h = (uint16)h;

    if(allowIA4) {
        // IA4 is only safe for truly opaque grayscale helpers. Alpha-bearing
        // D3D textures such as foliage/HUD masks visibly break when we crush
        // them into 4-bit intensity + 4-bit alpha, so keep those on RGBA8.
        natras->gxFmt    = GX_TF_IA4;
        natras->hasAlpha = imageHasAlpha ? 1 : 0;
        natras->alphaKind = imageAlphaKind;
        natras->dataSize = gx::ia4TiledSize(w, h);
        natras->gxData   = gx::safeGxAlloc(natras->dataSize, 32, tex->name);
        if(natras->gxData) {
            convertRGBA8_to_IA4(natras->gxData, pixels, w, h, stride);
            gx::texPoolRegister(gxRas, natras->gxData, natras->dataSize,
                            (uint16)w, (uint16)h, natras->gxFmt,
                            tex->name);
            printf("[GX] gxConvertRasterToNative: using IA4 %dx%d size=%u\n",
                   w, h, natras->dataSize);
        }
    }

    if(!natras->gxData) {
        // Fallback: RGBA8
        natras->gxFmt    = GX_TF_RGBA8;
        natras->hasAlpha = imageHasAlpha ? 1 : 0;
        natras->alphaKind = imageAlphaKind;
        natras->dataSize = gx::rgba8TiledSize(w, h);
        natras->gxData   = gx::safeGxAlloc(natras->dataSize, 32, tex->name);
        if(!natras->gxData){
            printf("[GX] gxConvertRasterToNative: alloc fail %dx%d dataSize=%u\n",
                   w, h, natras->dataSize);
            img->destroy();
            gxRas->destroy();
            return;
        }
        gx::texPoolRegister(gxRas, natras->gxData, natras->dataSize,
                        (uint16)w, (uint16)h, natras->gxFmt,
                        tex->name);
        gx::convertRGBA8_to_GX(natras->gxData, pixels, w, h, stride);
    }

    DCFlushRange(natras->gxData, natras->dataSize);
    GX_InvalidateTexAll();

    img->destroy();
    src->destroy();

    GX_InitTexObj(&natras->texObj, natras->gxData, (u16)w, (u16)h,
                  natras->gxFmt, natras->wrapS, natras->wrapT, GX_FALSE);
    GX_InitTexObjFilterMode(&natras->texObj, natras->minFilter, natras->magFilter);
    natras->texObjValid = true;
    natras->dirty       = false;

    tex->raster = gxRas;
    gx::texPoolEnforceBudget(tex->name);
}
} // namespace rw

#endif // GAMECUBE
