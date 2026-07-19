/**
 * gxread.cpp é¥?GX Native Texture Reader
 *
 * Reads PLATFORM_GX textures from TXD streams.
 * Pixel data is LINEAR big-endian in the stream; we tile it for GX hardware.
 *
 * Binary format (header fields little-endian; pixel data GX-native big-endian):
 *   uint32 platform          = PLATFORM_GX (13)
 *   uint32 filterAddressing
 *   char   name[32]
 *   char   mask[32]
 *   uint32 gxFmt             // GX_TF_CMPR=0xE, GX_TF_RGBA8=0x6
 *   uint32 hasAlpha
 *   uint16 width
 *   uint16 height
 *   uint32 dataSize          // linear pixel data size in bytes
 *   uint8  pixels[dataSize]  // raw GX-native linear pixel data (big-endian)
 */

#ifdef GAMECUBE
// Force include via -include, not normal include
  #define PLUGIN_ID ID_RASTERGX
  
#include <gccore.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"

#include "gxmemory.h"
#ifdef WII
void *MemoryMgrMallocAlignMem2(size_t size, size_t align);
void *MemoryMgrMallocAlignMem2Strict(size_t size, size_t align);
void MemoryMgrFreeAlignMem2(void *mem);
#endif

namespace rw {
namespace gx {

#ifndef GX_TEX_VERBOSE_DIAG
#define GX_TEX_VERBOSE_DIAG 0
#endif

#ifndef GX_RADAR_CMPR_TO_RGBA8
#define GX_RADAR_CMPR_TO_RGBA8 0
#endif

#ifndef GX_CMPR_FIX_DXT1_SELECTOR_ORDER
#define GX_CMPR_FIX_DXT1_SELECTOR_ORDER 1
#endif

#if GX_TEX_VERBOSE_DIAG
#define GX_TEX_DIAG_PRINTF(...) printf(__VA_ARGS__)
#else
#define GX_TEX_DIAG_PRINTF(...) ((void)0)
#endif

static const char*
gxFmtName(uint32 fmt)
{
    switch(fmt){
    case GX_TF_CMPR:  return "CMPR";
    case GX_TF_RGB5A3:return "RGB5A3";
    case GX_TF_RGB565:return "RGB565";
    case GX_TF_RGBA8: return "RGBA8";
    default:          return "OTHER";
    }
}

static uint8
gxWrapFromRw(int32 addr)
{
    switch(addr){
    case Texture::WRAP:   return GX_REPEAT;
    case Texture::MIRROR: return GX_MIRROR;
    case Texture::CLAMP:  return GX_CLAMP;
    default:              return GX_CLAMP;
    }
}

static void
gxFilterFromRw(Texture *tex, uint8 *minf, uint8 *magf)
{
    uint8 minFilter = GX_LINEAR;
    uint8 magFilter = GX_LINEAR;
    if(tex){
        switch(tex->getFilter()){
        case Texture::NEAREST:
        case Texture::MIPNEAREST:
            minFilter = GX_NEAR;
            magFilter = GX_NEAR;
            break;
        default:
            minFilter = GX_LINEAR;
            magFilter = GX_LINEAR;
            break;
        }
    }
    if(minf) *minf = minFilter;
    if(magf) *magf = magFilter;
}

static uint8* allocTempPixels(Texture *tex, uint32 size);
static void freeTempPixels(void *ptr);

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
startsWith(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool
gxTexNeedsFocusLog(const char *name)
{
    return nameContainsNoCase(name, "intro") ||
           nameContainsNoCase(name, "splash") ||
           nameContainsNoCase(name, "hud") ||
           nameContainsNoCase(name, "radar") ||
           nameContainsNoCase(name, "map") ||
           nameContainsNoCase(name, "window") ||
           nameContainsNoCase(name, "glass") ||
           nameContainsNoCase(name, "mesh") ||
           nameContainsNoCase(name, "fence") ||
           nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "bark") ||
           nameContainsNoCase(name, "trunk") ||
           nameContainsNoCase(name, "branch") ||
           nameContainsNoCase(name, "plant") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "road") ||
           nameContainsNoCase(name, "asphalt") ||
           nameContainsNoCase(name, "tarmac") ||
           nameContainsNoCase(name, "floor") ||
           nameContainsNoCase(name, "door") ||
           nameContainsNoCase(name, "dock") ||
           nameContainsNoCase(name, "wall") ||
           nameContainsNoCase(name, "rack") ||
           nameContainsNoCase(name, "shelf") ||
           nameContainsNoCase(name, "cash") ||
           nameContainsNoCase(name, "money") ||
           nameContainsNoCase(name, "drug") ||
           nameContainsNoCase(name, "heli") ||
           nameContainsNoCase(name, "rotor") ||
           nameContainsNoCase(name, "prop") ||
           nameContainsNoCase(name, "shadow") ||
           nameContainsNoCase(name, "sign");
}

static bool
shouldPreferOwnSamplerState(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "bark") ||
           nameContainsNoCase(name, "trunk") ||
           nameContainsNoCase(name, "branch") ||
           nameContainsNoCase(name, "plant") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "frond");
}

static bool
shouldFixTransparentFringeRGBA8(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return strcmp(name, "foliage256") == 0 ||
           strcmp(name, "newtreeleaves128") == 0 ||
           strcmp(name, "newtreeleavesb128") == 0 ||
           strcmp(name, "kbtree4_test") == 0 ||
           strcmp(name, "planta256") == 0 ||
           strcmp(name, "plantb256") == 0 ||
           strcmp(name, "plantc256") == 0 ||
           strcmp(name, "fuzzyplant256") == 0 ||
           strcmp(name, "kbplanter_plants1") == 0 ||
           strcmp(name, "dt_grasstrans1_texture") == 0;
}

static bool
isRadarMapTexture(const char *name)
{
    if(name == nil)
        return false;
    if(!nameContainsNoCase(name, "radar"))
        return false;

    const char *p = name;
    while(*p && !((*p >= '0') && (*p <= '9')))
        p++;
    return p[0] >= '0' && p[0] <= '9' &&
           p[1] >= '0' && p[1] <= '9' &&
           p[2] == '\0';
}

static uint32
bleedTransparentEdgeRGB(Texture *tex, uint8 *pixels, int32 width, int32 height)
{
    if(pixels == nil || width <= 0 || height <= 0)
        return 0;

    uint32 size = (uint32)width * (uint32)height * 4u;
    uint8 *copy = allocTempPixels(tex, size);
    if(copy == nil)
        return 0;

    memcpy(copy, pixels, size);

    uint32 fixed = 0;
    for(int32 y = 0; y < height; y++) {
        for(int32 x = 0; x < width; x++) {
            uint32 idx = (uint32)(y * width + x) * 4u;
            const uint8 *src = pixels + idx;
            uint8 *dst = copy + idx;
            if(src[3] > 8)
                continue;

            int bestAlpha = -1;
            const uint8 *best = nil;
            for(int32 dy = -1; dy <= 1; dy++) {
                int32 ny = y + dy;
                if(ny < 0 || ny >= height)
                    continue;
                for(int32 dx = -1; dx <= 1; dx++) {
                    int32 nx = x + dx;
                    if((dx == 0 && dy == 0) || nx < 0 || nx >= width)
                        continue;
                    const uint8 *neighbor = pixels + (uint32)(ny * width + nx) * 4u;
                    if((int)neighbor[3] > bestAlpha) {
                        bestAlpha = neighbor[3];
                        best = neighbor;
                    }
                }
            }

            if(best != nil && bestAlpha > 16) {
                dst[0] = best[0];
                dst[1] = best[1];
                dst[2] = best[2];
                fixed++;
            }
        }
    }

    memcpy(pixels, copy, size);
    freeTempPixels(copy);
    return fixed;
}

static void
gxLogFocusedTexture(Texture *tex, uint32 srcFmt, uint32 outFmt, uint32 hasAlpha,
                    int32 width, int32 height, uint32 dataSize,
                    uint8 minf, uint8 magf, uint8 wrapS, uint8 wrapT,
                    bool useRGB5A3)
{
    static int s_focusTexLogCount = 0;
    if(tex == nil)
        return;

    bool focus = gxTexNeedsFocusLog(tex->name);
    if(!focus && !useRGB5A3)
        return;

    if((focus && s_focusTexLogCount >= 256) ||
       (!focus && s_focusTexLogCount >= 96))
        return;

    printf("[GX-TEXFOCUS] tex=%s srcFmt=%s outFmt=%s alpha=%u %dx%d data=%u "
           "rwFilter=%d min=%u mag=%u wrap=%u/%u addr=0x%08X rgb5a3=%d\n",
           tex->name,
           gxFmtName(srcFmt),
           gxFmtName(outFmt),
           (unsigned)hasAlpha,
           width, height,
           dataSize,
           (int)tex->getFilter(),
           (unsigned)minf,
           (unsigned)magf,
           (unsigned)wrapS,
           (unsigned)wrapT,
           (unsigned)tex->filterAddressing,
           useRGB5A3 ? 1 : 0);
    s_focusTexLogCount++;
}

// éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éº?// GX hardware tiling: convert LINEAR pixel data to Morton-order
// tiled layout required by GX texture cache.
// éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éº?
/**
 * RGBA8 linear é«?GX tiled (AR/GB byte separation per 4è³4 tile).
 * GX hardware expects per tile (64 bytes):
 *   Bytes 0-31:  AR pairs é¥?all 16 pixels (A,R,A,R,...)
 *   Bytes 32-63: GB pairs é¥?all 16 pixels (G,B,G,B,...)
 */
static void
tileRGBA8(uint8 *dst, const uint8 *src, int w, int h)
{
    for (int by = 0; by < h; by += 4) {
        for (int bx = 0; bx < w; bx += 4) {
            for (int iy = 0; iy < 4; iy++) {
                int py = (by + iy < h) ? by + iy : h - 1;
                for (int ix = 0; ix < 4; ix++) {
                    int px = (bx + ix < w) ? bx + ix : w - 1;
                    const uint8 *p = src + (py * w + px) * 4;
                    *dst++ = p[3]; // A
                    *dst++ = p[0]; // R
                }
            }
            for (int iy = 0; iy < 4; iy++) {
                int py = (by + iy < h) ? by + iy : h - 1;
                for (int ix = 0; ix < 4; ix++) {
                    int px = (bx + ix < w) ? bx + ix : w - 1;
                    const uint8 *p = src + (py * w + px) * 4;
                    *dst++ = p[1]; // G
                    *dst++ = p[2]; // B
                }
            }
        }
    }
}

static void
tileRGB5A3(uint8 *dstBytes, const uint8 *src, int w, int h)
{
    uint16 *dst = (uint16*)dstBytes;
    for (int by = 0; by < h; by += 4) {
        for (int bx = 0; bx < w; bx += 4) {
            for (int iy = 0; iy < 4; iy++) {
                int py = (by + iy < h) ? by + iy : h - 1;
                for (int ix = 0; ix < 4; ix++) {
                    int px = (bx + ix < w) ? bx + ix : w - 1;
                    const uint8 *p = src + (py * w + px) * 4;
                    uint8 r = p[0], g = p[1], b = p[2], a = p[3];
                    uint16 out;
                    if (a >= 224) {
                        out = (uint16)(0x8000 |
                              ((r >> 3) << 10) |
                              ((g >> 3) << 5) |
                              (b >> 3));
                    } else {
                        out = (uint16)(((a >> 5) << 12) |
                              ((r >> 4) << 8) |
                              ((g >> 4) << 4) |
                              (b >> 4));
                    }
                    *dst++ = out;
                }
            }
        }
    }
}

static uint16
packRGB5A3Pixel(uint8 r, uint8 g, uint8 b, uint8 a)
{
    if(a >= 224) {
        return (uint16)(0x8000 |
                        ((r >> 3) << 10) |
                        ((g >> 3) << 5) |
                        (b >> 3));
    }
    return (uint16)(((a >> 5) << 12) |
                    ((r >> 4) << 8) |
                    ((g >> 4) << 4) |
                    (b >> 4));
}

static void
tileRGBA8BlockFromRows(uint8 *dst, const uint8 *rows, int rowBytes, int w, int bx)
{
    for (int iy = 0; iy < 4; iy++) {
        const uint8 *row = rows + iy * rowBytes;
        for (int ix = 0; ix < 4; ix++) {
            int px = bx + ix;
            if (px >= w) px = w - 1;
            const uint8 *p = row + px * 4;
            *dst++ = p[3];
            *dst++ = p[0];
        }
    }
    for (int iy = 0; iy < 4; iy++) {
        const uint8 *row = rows + iy * rowBytes;
        for (int ix = 0; ix < 4; ix++) {
            int px = bx + ix;
            if (px >= w) px = w - 1;
            const uint8 *p = row + px * 4;
            *dst++ = p[1];
            *dst++ = p[2];
        }
    }
}

static void
tileRGB5A3BlockFromRows(uint8 *dstBytes, const uint8 *rows, int rowBytes, int w, int bx)
{
    uint16 *dst = (uint16*)dstBytes;
    for (int iy = 0; iy < 4; iy++) {
        const uint8 *row = rows + iy * rowBytes;
        for (int ix = 0; ix < 4; ix++) {
            int px = bx + ix;
            if (px >= w) px = w - 1;
            const uint8 *p = row + px * 4;
            *dst++ = packRGB5A3Pixel(p[0], p[1], p[2], p[3]);
        }
    }
}

static bool
tileStreamRGBA8(Stream *stream, Texture *tex, uint8 *dst,
                int32 width, int32 height, bool useRGB5A3,
                uint8 firstSrc[8], uint8 *alphaKind)
{
    uint32 rowBytes = (uint32)width * 4u;
    uint8 *rows = allocTempPixels(tex, rowBytes * 4u);
    if(rows == nil)
        return false;

    bool capturedFirstSrc = false;
    bool hasTransparent = false;
    bool hasSmooth = false;
    for(int32 by = 0; by < height; by += 4){
        int32 rowsThisBlock = height - by;
        if(rowsThisBlock > 4)
            rowsThisBlock = 4;

        for(int32 iy = 0; iy < rowsThisBlock; iy++){
            stream->read8(rows + iy * rowBytes, rowBytes);
            const uint8 *row = rows + iy * rowBytes;
            for(int32 x = 0; x < width; x++){
                uint8 alpha = row[x*4 + 3];
                if(alpha == 255)
                    continue;
                hasTransparent = true;
                if(alpha != 0)
                    hasSmooth = true;
            }
        }

        for(int32 iy = rowsThisBlock; iy < 4; iy++)
            memcpy(rows + iy * rowBytes,
                   rows + (rowsThisBlock - 1) * rowBytes,
                   rowBytes);

        if(!capturedFirstSrc){
            for(int i = 0; i < 8; i++)
                firstSrc[i] = rowBytes > (uint32)i ? rows[i] : 0;
            capturedFirstSrc = true;
        }

        for(int32 bx = 0; bx < width; bx += 4){
            if(useRGB5A3){
                tileRGB5A3BlockFromRows(dst, rows, rowBytes, width, bx);
                dst += 32;
            }else{
                tileRGBA8BlockFromRows(dst, rows, rowBytes, width, bx);
                dst += 64;
            }
        }
    }

    if(alphaKind){
        *alphaKind = hasSmooth ? GX_RASTER_ALPHA_SMOOTH :
                     (hasTransparent ? GX_RASTER_ALPHA_CUTOUT :
                                       GX_RASTER_ALPHA_NONE);
    }
    freeTempPixels(rows);
    return true;
}

static uint8
classifyLinearRGBA8Alpha(const uint8 *pixels, int32 width, int32 height)
{
    if(pixels == nil || width <= 0 || height <= 0)
        return GX_RASTER_ALPHA_NONE;

    bool hasTransparent = false;
    uint32 count = (uint32)width * (uint32)height;
    for(uint32 i = 0; i < count; i++){
        uint8 alpha = pixels[i*4 + 3];
        if(alpha == 255)
            continue;
        hasTransparent = true;
        if(alpha != 0)
            return GX_RASTER_ALPHA_SMOOTH;
    }
    return hasTransparent ? GX_RASTER_ALPHA_CUTOUT : GX_RASTER_ALPHA_NONE;
}

static bool
isLikelySoftAlphaTexture(const char *name)
{
    return nameContainsNoCase(name, "glass") ||
           nameContainsNoCase(name, "window") ||
           nameContainsNoCase(name, "shadow") ||
           nameContainsNoCase(name, "light") ||
           nameContainsNoCase(name, "flare") ||
           nameContainsNoCase(name, "water");
}

static bool
isLikelyCutoutTexture(const char *name)
{
    return nameContainsNoCase(name, "fence") ||
           nameContainsNoCase(name, "mesh") ||
           nameContainsNoCase(name, "wire") ||
           nameContainsNoCase(name, "grate") ||
           nameContainsNoCase(name, "grill") ||
           nameContainsNoCase(name, "gate") ||
           nameContainsNoCase(name, "chain") ||
           nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "bush") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "ivy") ||
           nameContainsNoCase(name, "plant") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "fern") ||
           nameContainsNoCase(name, "frond") ||
           nameContainsNoCase(name, "weed") ||
           nameContainsNoCase(name, "sign");
}

static bool
isLikelyCompactAlphaHelperTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    // Keep smooth helper fades in RGBA8 on Wii. The earlier RGBA8 -> RGB5A3
    // demotion saved a little pool space, but it also quantized white128a /
    // black128 / tempalpha-style gradients into visibly harsh masks. That
    // showed up as blown-out white facades, over-bright pickups, and whole
    // dark room shells. With the larger MEM2-backed GX pool, correctness wins.
    (void)name;
    return false;
}

static bool
isLikelyVegetationShadowTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;
    if(!nameContainsNoCase(name, "shadow"))
        return false;

    return strcmp(name, "weepalmshadow") == 0 ||
           strcmp(name, "bigpalmshadow") == 0 ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "frond") ||
           nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "foliage");
}

static bool
shouldPreserveTextureAlphaSemantics(const char *name)
{
    return isLikelySoftAlphaTexture(name) ||
           isLikelyCutoutTexture(name);
}

static bool
isLikelyThinTwoSidedAlphaTexture(const char *name)
{
    return nameContainsNoCase(name, "rotor") ||
           nameContainsNoCase(name, "propell") ||
           nameContainsNoCase(name, "blade") ||
           nameContainsNoCase(name, "fan");
}

static bool
shouldRecoverTextureAlphaByName(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    // Name-only CMPR alpha recovery turned out to be too broad for Wii.
    // Many fully opaque world textures (grass, hedge, signs, walls) were
    // getting forced into the alpha path simply because of generic keywords.
    // Real DXT1/CMPR cutouts are still detected from selector 3 in the block
    // data, so keep name-based recovery only for the thinnest helper surfaces
    // where a false opaque classification is especially obvious.
    return isLikelyThinTwoSidedAlphaTexture(name);
}

static bool
isLikelyOpaqueWorldTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;
    if(shouldPreserveTextureAlphaSemantics(name))
        return false;

    // Keep this intentionally narrow. A broad "world texture is opaque"
    // rule breaks props, HUD masks, propellers, and cutscene set pieces that
    // rely on texture alpha even when their names look like walls/floors.
    return nameContainsNoCase(name, "road") ||
           nameContainsNoCase(name, "asphalt") ||
           nameContainsNoCase(name, "pave") ||
           nameContainsNoCase(name, "pavement") ||
           nameContainsNoCase(name, "tarmac") ||
           nameContainsNoCase(name, "street") ||
           nameContainsNoCase(name, "freeway") ||
           nameContainsNoCase(name, "highway") ||
           nameContainsNoCase(name, "sidewalk") ||
           nameContainsNoCase(name, "kerb") ||
           nameContainsNoCase(name, "curb");
}

static bool
shouldExpandCMPRRoadToRGB5A3(Texture *tex, int32 width, int32 height, uint32 hasAlpha)
{
    if(tex == nil || tex->name[0] == '\0')
        return false;
    if(hasAlpha != 0)
        return false;
    if(width != 256 || height != 256)
        return false;

    // Re-enable the road fallback very narrowly. Radar-style decode/upload is
    // too expensive for every opaque road-like CMPR texture, but the current
    // logs keep circling back to a couple of close-range road surfaces whose
    // native CMPR path still looks wrong while the small LOD roads look fine.
    // Keep this on a tight exact-name allowlist until we verify the visual win.
    return strcmp(tex->name, "road2_256") == 0 ||
           strcmp(tex->name, "road1256") == 0;
}

static bool
isKnownOpaqueVegetationCMPRTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return strcmp(name, "kbbark_test1") == 0 ||
           strcmp(name, "kbtree3_test") == 0 ||
           strcmp(name, "palmbark128") == 0 ||
           strcmp(name, "bark04S64") == 0 ||
           nameContainsNoCase(name, "bark") ||
           nameContainsNoCase(name, "trunk") ||
           nameContainsNoCase(name, "branch");
}

static bool
isKnownCutoutVegetationCMPRTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    return strcmp(name, "kbtree4_test") == 0 ||
           strcmp(name, "newtreeleaves128") == 0 ||
           strcmp(name, "newtreeleavesb128") == 0 ||
           strcmp(name, "planta256") == 0 ||
           strcmp(name, "plantb256") == 0 ||
           strcmp(name, "plantc256") == 0 ||
           strcmp(name, "fuzzyplant256") == 0 ||
           strcmp(name, "foliage256") == 0 ||
           strcmp(name, "kbplanter_plants1") == 0;
}

static bool
isLikelyLostMaskVegetationCMPRTexture(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    // Keep this deliberately tiny. The broad keyword-driven recovery grew into
    // a second texture conversion pipeline of its own and started damaging
    // normal CMPR assets more often than it helped. Only keep the handful of
    // proven-bad masks on the fallback path for now.
    return strcmp(name, "kbtree4_test") == 0 ||
           strcmp(name, "newtreeleaves128") == 0 ||
           strcmp(name, "hedgewall_64") == 0 ||
           strcmp(name, "lodhedgewall_64") == 0 ||
           strcmp(name, "hedge2_128") == 0;
}

static bool
shouldExpandOpaqueCMPRVegetationToRGB5A3(Texture *tex, int32 width, int32 height, uint32 hasAlpha)
{
    if(tex == nil || tex->name[0] == '\0')
        return false;
    if(hasAlpha != 0)
        return false;
    if(width > 128 || height > 128)
        return false;

    return isKnownOpaqueVegetationCMPRTexture(tex->name);
}

static bool
shouldExpandCutoutCMPRVegetationToRGB5A3(Texture *tex, int32 width, int32 height, uint32 hasAlpha)
{
    if(tex == nil || tex->name[0] == '\0')
        return false;
    if(hasAlpha == 0)
        return false;
    if(width > 256 || height > 256)
        return false;

    return isKnownCutoutVegetationCMPRTexture(tex->name);
}

static bool
shouldExpandColorKeyVegetationCMPRToRGB5A3(Texture *tex, int32 width, int32 height, uint32 hasAlpha)
{
    if(tex == nil || tex->name[0] == '\0')
        return false;
    if(hasAlpha != 0)
        return false;
    if(width > 256 || height > 256)
        return false;

    return isLikelyLostMaskVegetationCMPRTexture(tex->name);
}

static bool
shouldSkipCMPRSelectorReorder(const char *name)
{
    if(name == nil || name[0] == '\0')
        return false;

    // Our offline TXD conversion only byte-swaps DXT1 endpoints; most world
    // textures still need a runtime selector swizzle for GX CMPR. However,
    // the recent Wii logs consistently show static vegetation atlases looking
    // worse after that swizzle: grass/hedge/palm masks turn into broken paper
    // cutouts and foliage coverage collapses. Keep the selector remap for the
    // rest of the world, but preserve the authored selector layout for the
    // vegetation family until we have per-asset proof otherwise.
    return strcmp(name, "kbtree4_test") == 0 ||
           strcmp(name, "newtreeleaves128") == 0 ||
           strcmp(name, "newtreeleavesb128") == 0 ||
           strcmp(name, "foliage256") == 0 ||
           strcmp(name, "planta256") == 0 ||
           strcmp(name, "plantb256") == 0 ||
           strcmp(name, "plantc256") == 0 ||
           strcmp(name, "fuzzyplant256") == 0 ||
           strcmp(name, "hedgewall_64") == 0 ||
           strcmp(name, "lodhedgewall_64") == 0 ||
           strcmp(name, "hedge2_128") == 0;
}

static bool
shouldPreferRGB5A3(Texture *tex, int32 width, int32 height, uint32 hasAlpha)
{
    if (hasAlpha == 0)
        return false;
    if (width < 128 || height < 128)
        return false;
    if (tex == nil || tex->name[0] == '\0')
        return false;

    const char *name = tex->name;
    if(isLikelyVegetationShadowTexture(name))
        return false;

    if(isLikelyCompactAlphaHelperTexture(name))
        return true;

    // Keep this optimization conservative: visible signage/windows lose alpha
    // quality too easily when we auto-demote from RGBA8.
    return nameContainsNoCase(name, "shadow") &&
           !nameContainsNoCase(name, "window") &&
           !nameContainsNoCase(name, "glass") &&
           !nameContainsNoCase(name, "sign");
}

/**
 * CMPR linear é«?GX tiled (Morton Z-order within 8è³8 pixel macroblocks).
 *
 * GX hardware groups CMPR blocks into 8è³8 pixel (2è³2 block) macro-tiles.
 * Within each macro-tile, the 4 blocks must be in Z-order:
 *   dst order: block(0,0), block(1,0), block(0,1), block(1,1)
 * Reference: opengx convert_rgb_image_to_DXT1 (image_DXT.c:130-178)
 */
static void
tileCMPR(uint8 *dst, const uint8 *src, int w, int h)
{
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    int mbw = (bw + 1) / 2;
    int mbh = (bh + 1) / 2;

    for (int mby = 0; mby < mbh; mby++) {
        for (int mbx = 0; mbx < mbw; mbx++) {
            for (int by = 0; by < 2; by++) {
                for (int bx = 0; bx < 2; bx++) {
                    int src_bx = mbx * 2 + bx;
                    int src_by = mby * 2 + by;
                    if (src_bx < bw && src_by < bh) {
                        int src_off = (src_by * bw + src_bx) * 8;
                        for (int j = 0; j < 8; j++)
                            *dst++ = src[src_off + j];
                    } else {
                        for (int j = 0; j < 8; j++)
                            *dst++ = 0;
                    }
                }
            }
        }
    }
}

static uint32
cmprLinearSize(int w, int h)
{
    return (uint32)(((w + 3) / 4) * ((h + 3) / 4) * 8);
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

static uint16
readBlockGX16(const uint8 *p)
{
    // PLATFORM_GX native CMPR stores RGB565 endpoints in GX byte order.
    return (uint16)(((uint16)p[0] << 8) | (uint16)p[1]);
}

static uint32
readBlockLE32(const uint8 *p)
{
    return (uint32)p[0] |
           ((uint32)p[1] << 8) |
           ((uint32)p[2] << 16) |
           ((uint32)p[3] << 24);
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
convertDXT1SelectorOrderToGXCMPR(uint8 *blocks, int32 width, int32 height)
{
    if(blocks == nil || width <= 0 || height <= 0)
        return;

    uint32 numBlocks = cmprLinearSize(width, height) / 8u;
    for(uint32 i = 0; i < numBlocks; i++) {
        uint8 *sel = blocks + i * 8u + 4u;
        sel[0] = dxt1SelectorByteToGX(sel[0]);
        sel[1] = dxt1SelectorByteToGX(sel[1]);
        sel[2] = dxt1SelectorByteToGX(sel[2]);
        sel[3] = dxt1SelectorByteToGX(sel[3]);
    }
}

static void
decodeRGB565(uint16 c, uint8 *rgba)
{
    rgba[0] = (uint8)((((c >> 11) & 0x1Fu) * 255u) / 31u);
    rgba[1] = (uint8)((((c >> 5) & 0x3Fu) * 255u) / 63u);
    rgba[2] = (uint8)(((c & 0x1Fu) * 255u) / 31u);
    rgba[3] = 255;
}

static void
decodeCMPRBlockPalette(uint16 c0, uint16 c1, uint8 colors[4][4], bool forceOpaque)
{
    decodeRGB565(c0, colors[0]);
    decodeRGB565(c1, colors[1]);
    if(c0 > c1 || forceOpaque) {
        for(int32 k = 0; k < 3; k++) {
            colors[2][k] = (uint8)((2u * colors[0][k] + colors[1][k]) / 3u);
            colors[3][k] = (uint8)((colors[0][k] + 2u * colors[1][k]) / 3u);
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        for(int32 k = 0; k < 3; k++) {
            colors[2][k] = (uint8)((colors[0][k] + colors[1][k]) / 2u);
            colors[3][k] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }
}

static void
decodeCMPRToRGBA8(uint8 *dst, const uint8 *src, int32 width, int32 height)
{
    if(dst == nil || src == nil || width <= 0 || height <= 0)
        return;

    int32 bw = (width + 3) / 4;
    int32 bh = (height + 3) / 4;
    for(int32 by = 0; by < bh; by++) {
        for(int32 bx = 0; bx < bw; bx++) {
            const uint8 *block = src + (by * bw + bx) * 8;
            uint16 c0 = readBlockGX16(block + 0);
            uint16 c1 = readBlockGX16(block + 2);
            uint32 selectors = readBlockLE32(block + 4);
            uint8 colors[4][4];

            decodeCMPRBlockPalette(c0, c1, colors, false);

            for(int32 py = 0; py < 4; py++) {
                int32 y = by * 4 + py;
                if(y >= height)
                    continue;
                for(int32 px = 0; px < 4; px++) {
                    int32 x = bx * 4 + px;
                    if(x >= width)
                        continue;
                    uint32 idx = (selectors >> ((py * 4 + px) * 2)) & 0x3u;
                    memcpy(dst + (y * width + x) * 4, colors[idx], 4);
                }
            }
        }
    }
}

static void
tileCMPRToRGB5A3(uint8 *dstBytes, const uint8 *src, int32 width, int32 height,
                 bool forceOpaque, bool colorKeyBlack)
{
    if(dstBytes == nil || src == nil || width <= 0 || height <= 0)
        return;

    uint16 *dst = (uint16*)dstBytes;
    int32 bw = (width + 3) / 4;
    int32 bh = (height + 3) / 4;
    for(int32 by = 0; by < bh; by++) {
        for(int32 bx = 0; bx < bw; bx++) {
            const uint8 *block = src + (by * bw + bx) * 8;
            uint16 c0 = readBlockGX16(block + 0);
            uint16 c1 = readBlockGX16(block + 2);
            uint32 selectors = readBlockLE32(block + 4);
            uint8 colors[4][4];

            decodeCMPRBlockPalette(c0, c1, colors, forceOpaque);

            for(int32 py = 0; py < 4; py++) {
                int32 sy = py;
                if(by * 4 + sy >= height)
                    sy = height - by * 4 - 1;
                if(sy < 0)
                    sy = 0;

                for(int32 px = 0; px < 4; px++) {
                    int32 sx = px;
                    if(bx * 4 + sx >= width)
                        sx = width - bx * 4 - 1;
                    if(sx < 0)
                        sx = 0;

                    uint32 idx = (selectors >> ((sy * 4 + sx) * 2)) & 0x3u;
                    uint8 rgba[4];
                    memcpy(rgba, colors[idx], sizeof(rgba));
                    if(colorKeyBlack && rgba[3] != 0 &&
                       rgba[0] <= 20 && rgba[1] <= 20 && rgba[2] <= 20)
                        rgba[3] = 0;
                    *dst++ = packRGB5A3Pixel(rgba[0], rgba[1], rgba[2], rgba[3]);
                }
            }
        }
    }
}

static void
writeBlockLE32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFF);
    p[1] = (uint8)((v >> 8) & 0xFF);
    p[2] = (uint8)((v >> 16) & 0xFF);
    p[3] = (uint8)((v >> 24) & 0xFF);
}

static bool
cmprHasTransparentPixels(const uint8 *blocks, int32 width, int32 height)
{
    if(blocks == nil || width <= 0 || height <= 0)
        return false;

    uint32 numBlocks = cmprLinearSize(width, height) / 8u;
    for(uint32 i = 0; i < numBlocks; i++) {
        const uint8 *block = blocks + i * 8u;
        uint16 col0 = readBlockGX16(block + 0);
        uint16 col1 = readBlockGX16(block + 2);
        if(col0 > col1)
            continue;

        uint32 selectors = readBlockLE32(block + 4);
        for(int bit = 0; bit < 32; bit += 2) {
            if(((selectors >> bit) & 0x3u) == 3u)
                return true;
        }
    }

    return false;
}

static uint32
sanitizeOpaqueCMPR(uint8 *blocks, int32 width, int32 height)
{
    if(blocks == nil || width <= 0 || height <= 0)
        return 0;

    uint32 numBlocks = cmprLinearSize(width, height) / 8u;
    uint32 fixedBlocks = 0;
    for(uint32 i = 0; i < numBlocks; i++) {
        uint8 *block = blocks + i * 8u;
        uint16 col0 = readBlockGX16(block + 0);
        uint16 col1 = readBlockGX16(block + 2);
        if(col0 < col1) {
            uint8 t0 = block[0];
            uint8 t1 = block[1];
            block[0] = block[2];
            block[1] = block[3];
            block[2] = t0;
            block[3] = t1;
            // 0<->1, 2<->3. This preserves opaque DXT1 RGB blocks while
            // forcing GX CMPR into four-color mode instead of transparent mode.
            uint32 selectors = readBlockLE32(block + 4) ^ 0x55555555u;
            writeBlockLE32(block + 4, selectors);
            fixedBlocks++;
        } else if(col0 == col1) {
            // Equal endpoints still decode as 3-color + transparent on CMPR.
            // Strip selector 3 to keep the block opaque.
            uint32 selectors = readBlockLE32(block + 4);
            bool changed = false;
            uint32 out = 0;
            for(int bit = 0; bit < 32; bit += 2) {
                uint32 idx = (selectors >> bit) & 0x3u;
                if(idx == 3u) {
                    idx = 2u;
                    changed = true;
                }
                out |= idx << bit;
            }
            if(changed) {
                writeBlockLE32(block + 4, out);
                fixedBlocks++;
            }
        }
    }
    return fixedBlocks;
}

static bool
shouldSanitizeOpaqueCMPR(Texture *tex, int32 width, int32 height,
                         uint32 hasAlpha, bool detectedTransparentPixels)
{
    if(tex == nil || tex->name[0] == '\0')
        return false;

    (void)width;
    (void)height;

    if(hasAlpha != 0 || detectedTransparentPixels)
        return false;

    if(shouldRecoverTextureAlphaByName(tex->name))
        return false;

    // Earlier we sanitized almost every CMPR texture whose native header said
    // "no alpha". That prevented some false transparent holes, but it also
    // rewrote legitimate 3-color DXT1 blocks and visibly damaged detailed road
    // and prop textures. With selector-order conversion in place, prefer the
    // authored CMPR data unless we build a narrow allowlist for proven-bad
    // assets.
    return false;
}

#ifdef WII
static const uint32 GX_TEMP_SCRATCH_BYTES = 64u * 1024u;
static uint8 s_tempScratchA[GX_TEMP_SCRATCH_BYTES] ATTRIBUTE_ALIGN(32);
static uint8 s_tempScratchB[GX_TEMP_SCRATCH_BYTES] ATTRIBUTE_ALIGN(32);
static bool  s_tempScratchAInUse = false;
static bool  s_tempScratchBInUse = false;

static uint8*
tryAllocTempScratch(uint32 size, int *slotOut)
{
    if(slotOut)
        *slotOut = -1;
    if(size > GX_TEMP_SCRATCH_BYTES)
        return nil;

    if(!s_tempScratchAInUse) {
        s_tempScratchAInUse = true;
        if(slotOut)
            *slotOut = 0;
        return s_tempScratchA;
    }
    if(!s_tempScratchBInUse) {
        s_tempScratchBInUse = true;
        if(slotOut)
            *slotOut = 1;
        return s_tempScratchB;
    }
    return nil;
}

static bool
freeTempScratch(void *ptr)
{
    if(ptr == s_tempScratchA) {
        s_tempScratchAInUse = false;
        return true;
    }
    if(ptr == s_tempScratchB) {
        s_tempScratchBInUse = false;
        return true;
    }
    return false;
}
#endif

static uint8*
allocTempPixels(Texture *tex, uint32 size)
{
    if(size == 0)
        size = 1;

    uint8 *ptr = nil;
#ifdef WII
    int scratchSlot = -1;
    ptr = tryAllocTempScratch(size, &scratchSlot);
    if(ptr) {
        static int s_tempScratchLogCount = 0;
        if((size >= 8u * 1024u || (tex && gxTexNeedsFocusLog(tex->name))) &&
           s_tempScratchLogCount < 96) {
            printf("[GX-DIAG] %s: temp scratch(%u) slot=%d\n",
                   tex ? tex->name : "<null>", size, scratchSlot);
            s_tempScratchLogCount++;
        }
        return ptr;
    }

    ptr = (uint8*)MemoryMgrMallocAlignMem2Strict(size, 32);
    if(ptr)
        return ptr;

    if(size <= GX_TEMP_SCRATCH_BYTES) {
        texPoolEnforceBudget("temp-pixel-alloc");
        ptr = (uint8*)MemoryMgrMallocAlignMem2Strict(size, 32);
        if(ptr) {
            static int s_tempRetryLogCount = 0;
            if(s_tempRetryLogCount < 64) {
                printf("[GX-DIAG] %s: temp alloc(%u) recovered after budget enforce\n",
                       tex ? tex->name : "<null>", size);
                s_tempRetryLogCount++;
            }
            return ptr;
        }

        ptr = tryAllocTempScratch(size, &scratchSlot);
        if(ptr) {
            static int s_tempEmergencyScratchLogCount = 0;
            if(s_tempEmergencyScratchLogCount < 96) {
                printf("[GX-DIAG] %s: temp emergency scratch(%u) slot=%d after heap fail\n",
                       tex ? tex->name : "<null>", size, scratchSlot);
                s_tempEmergencyScratchLogCount++;
            }
            return ptr;
        }
    }
#else
    ptr = (uint8*)memalign(32, size);
    if(ptr == nil)
        ptr = (uint8*)malloc(size);
#endif
    if(ptr)
        return ptr;

    printf("[GX-DIAG] %s: temp memalign/malloc(%u) FAIL\n",
           tex ? tex->name : "<null>", size);
    return nil;
}

static void
freeTempPixels(void *ptr)
{
    if(ptr)
#ifdef WII
        if(!freeTempScratch(ptr))
            MemoryMgrFreeAlignMem2(ptr);
#else
        free(ptr);
#endif
}


// éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éº?// GX Native Texture Reader
// éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éºæ¨æ²éº?
Texture*
readNativeTexture(rw::Stream *stream)
{
    // é¹â¬é¹â¬ Parse STRUCT header é¹â¬é¹â¬
    if (!findChunk(stream, ID_STRUCT, nil, nil)) {
        RWERROR((ERR_CHUNK, "STRUCT"));
        return nil;
    }
    uint32 platform = stream->readU32();
    if (platform != PLATFORM_GX) {
        RWERROR((ERR_PLATFORM, platform));
        return nil;
    }

    Texture *tex = Texture::create(nil);
    if (tex == nil)
        return nil;

    // é¹â¬é¹â¬ Texture fields é¹â¬é¹â¬
    tex->filterAddressing = stream->readU32();
    if((tex->filterAddressing & 0xF000) == 0)
        tex->filterAddressing |= (tex->filterAddressing & 0x0F00) << 4;
    stream->read8(tex->name, 32);
    stream->read8(tex->mask, 32);

    // é¹â¬é¹â¬ Raster fields é¹â¬é¹â¬
    uint32  gxFmt    = stream->readU32();
    uint32  hasAlpha = stream->readU32();
    int32   width    = (int32)stream->readU16();
    int32   height   = (int32)stream->readU16();
    uint32  dataSize = stream->readU32();

    GX_TEX_DIAG_PRINTF("[GX-READ] %s: %dx%d fmt=0x%02X(%s) hasAlpha=%u dataSize=%u\n",
                       tex->name, width, height, gxFmt, gxFmtName(gxFmt), hasAlpha, dataSize);

    // é¹â¬é¹â¬ Heap probe: test malloc before rwNewT é¹â¬é¹â¬
#if GX_TEX_VERBOSE_DIAG
    printf("[GX-DIAG] %s: about to probe malloc(512)...\n", tex->name);
    {
        void *probe = malloc(512);
        printf("[GX-DIAG] %s: probe malloc returned %p\n", tex->name, probe);
        if (probe) {
            free(probe);
            printf("[GX-DIAG] %s: probe free OK\n", tex->name);
        } else {
            printf("[GX-DIAG] %s: HEAP PROBE FAILED! malloc(512)=NULL\n", tex->name);
        }
    }
#endif

    // é¹â¬é¹â¬ Create GX raster é¹â¬é¹â¬
    Raster *raster = Raster::create(width, height, 32, Raster::TEXTURE, PLATFORM_GX);
    if (raster == nil) {
        printf("[GX-DIAG] %s: Raster::create FAIL\n", tex->name);
        tex->destroy();
        return nil;
    }
    GX_TEX_DIAG_PRINTF("[GX-DIAG] %s: Raster::create OK\n", tex->name);

    GxRaster *gxras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);

    // Determine tiled size
    uint32 tiledSize;
    bool   isCMPR = (gxFmt == GX_TF_CMPR);
    bool   isRGBA8 = (gxFmt == GX_TF_RGBA8);
    bool   fixTransparentFringeRGBA8 = isRGBA8 &&
                                       shouldFixTransparentFringeRGBA8(tex->name);
    bool   useRGB5A3 = false;
    bool   expandCMPRToRGBA8 = GX_RADAR_CMPR_TO_RGBA8 && isCMPR && isRadarMapTexture(tex->name);
    bool   expandCMPRRoadToRGB5A3 = isCMPR && shouldExpandCMPRRoadToRGB5A3(tex, width, height, hasAlpha);
    bool   expandCMPROpaqueVegetationToRGB5A3 = isCMPR && shouldExpandOpaqueCMPRVegetationToRGB5A3(tex, width, height, hasAlpha);
    bool   expandCMPRCutoutVegetationToRGB5A3 = isCMPR && shouldExpandCutoutCMPRVegetationToRGB5A3(tex, width, height, hasAlpha);
    bool   expandCMPRColorKeyVegetationToRGB5A3 = isCMPR && shouldExpandColorKeyVegetationCMPRToRGB5A3(tex, width, height, hasAlpha);
    bool   expandCMPRVegetationToRGB5A3 = expandCMPROpaqueVegetationToRGB5A3 ||
                                          expandCMPRCutoutVegetationToRGB5A3 ||
                                          expandCMPRColorKeyVegetationToRGB5A3;
    bool   expandCMPRToRGB5A3 = expandCMPRRoadToRGB5A3 || expandCMPRVegetationToRGB5A3;
    bool   forceOpaqueExpandedCMPR = expandCMPRRoadToRGB5A3 ||
                                     expandCMPROpaqueVegetationToRGB5A3;
    uint32 outFmt = gxFmt;

    if(expandCMPRToRGBA8) {
        outFmt = GX_TF_RGBA8;
        printf("[GX-RADAR] %s: CMPR -> RGBA8 runtime upload (%dx%d)\n",
               tex->name, width, height);
    } else if(expandCMPRRoadToRGB5A3) {
        outFmt = GX_TF_RGB5A3;
        printf("[GX-ROAD] %s: CMPR -> RGB5A3 direct opaque upload (%dx%d)\n",
               tex->name, width, height);
    } else if(expandCMPROpaqueVegetationToRGB5A3) {
        outFmt = GX_TF_RGB5A3;
        printf("[GX-TREE] %s: CMPR -> RGB5A3 opaque bark upload (%dx%d)\n",
               tex->name, width, height);
    } else if(expandCMPRCutoutVegetationToRGB5A3) {
        outFmt = GX_TF_RGB5A3;
        printf("[GX-TREE] %s: CMPR -> RGB5A3 cutout foliage upload (%dx%d)\n",
               tex->name, width, height);
    } else if(expandCMPRColorKeyVegetationToRGB5A3) {
        outFmt = GX_TF_RGB5A3;
        printf("[GX-TREEKEY] %s: CMPR -> RGB5A3 color-key foliage recovery (%dx%d)\n",
               tex->name, width, height);
    }

    if (isRGBA8 && shouldPreferRGB5A3(tex, width, height, hasAlpha)) {
        useRGB5A3 = true;
        outFmt = GX_TF_RGB5A3;
        printf("[GX-FMT] %s: RGBA8 -> RGB5A3 (%dx%d alpha=%u)\n",
               tex->name, width, height, hasAlpha);
    } else if(isRGBA8 && isLikelyVegetationShadowTexture(tex->name)) {
        printf("[GX-FMT] %s: keep RGBA8 soft vegetation shadow (%dx%d alpha=%u)\n",
               tex->name, width, height, hasAlpha);
    }

    if (!isCMPR && !isRGBA8) {
        printf("[GX-WARN] %s: untested native fmt=0x%02X(%s); current reader will tile it as RGBA8\n",
               tex->name, gxFmt, gxFmtName(gxFmt));
    }

    if (expandCMPRToRGBA8) {
        tiledSize = rgba8TiledSize(width, height);
    } else if (expandCMPRToRGB5A3) {
        tiledSize = (uint32)width * (uint32)height * 2u;
    } else if (isCMPR) {
        tiledSize = cmprTiledSize(width, height);
    } else if (useRGB5A3) {
        tiledSize = (uint32)width * (uint32)height * 2u;
    } else {
        tiledSize = rgba8TiledSize(width, height);
    }

    if (isCMPR) {
        uint32 expect = cmprLinearSize(width, height);
        if (dataSize != expect)
            printf("[GX-WARN] %s: CMPR dataSize=%u expect=%u\n",
                   tex->name, dataSize, expect);
    } else if (isRGBA8) {
        uint32 expect = (uint32)width * (uint32)height * 4u;
        if (dataSize != expect)
            printf("[GX-WARN] %s: RGBA8 linear dataSize=%u expect=%u\n",
                   tex->name, dataSize, expect);
    }

    GX_TEX_DIAG_PRINTF("[GX-DIAG] %s: tiledSize=%u, calling safeGxAlloc...\n", tex->name, tiledSize);

    gxras->gxFmt    = (uint8)outFmt;
    gxras->hasAlpha = hasAlpha ? 1 : 0;
    gxras->alphaKind = hasAlpha ?
        (isCMPR ? GX_RASTER_ALPHA_CUTOUT : GX_RASTER_ALPHA_SMOOTH) :
        GX_RASTER_ALPHA_NONE;
    gxras->dataSize = tiledSize;
    gxras->w        = (uint16)width;
    gxras->h        = (uint16)height;
    gxras->gxData   = safeGxAlloc(tiledSize, 32, tex->name);

    if (gxras->gxData == nil) {
        printf("[GX-READ] %s: alloc FAIL (tiledSize=%u)\n", tex->name, tiledSize);
        raster->destroy();
        tex->destroy();
        return nil;
    }
    GX_TEX_DIAG_PRINTF("[GX-DIAG] %s: safeGxAlloc OK, tiling...\n", tex->name);

    uint8 *linearPixels = nil;
    uint8 firstSrcBytes[8] = {0};
    bool usedStreamTiler = false;
    uint8 streamedAlphaKind = GX_RASTER_ALPHA_NONE;
    bool cmprDetectedTransparentPixels = false;
    bool alphaHintByName = shouldRecoverTextureAlphaByName(tex->name);

    if (isRGBA8 && !fixTransparentFringeRGBA8) {
        usedStreamTiler = true;
        GX_TEX_DIAG_PRINTF("[GX-DIAG] %s: streaming RGBA8 rows directly into %s tiles\n",
                           tex->name, useRGB5A3 ? "RGB5A3" : "RGBA8");
        if (!tileStreamRGBA8(stream, tex, (uint8 *)gxras->gxData,
                             width, height, useRGB5A3, firstSrcBytes,
                             &streamedAlphaKind)) {
            printf("[GX-DIAG] %s: tileStreamRGBA8 FAIL\n", tex->name);
            gxMemFree(gxras->gxData);
            gxras->gxData = nil;
            raster->destroy();
            tex->destroy();
            return nil;
        }
    } else {
        // [FIX] rwNewT crashes on some textures (heap corruption in RenderWare allocator).
        // Use plain malloc/free for the temporary linear pixel buffer instead.
        linearPixels = allocTempPixels(tex, dataSize);
        if (linearPixels == nil) {
            printf("[GX-DIAG] %s: temp alloc(%u) FAIL\n", tex->name, dataSize);
            gxMemFree(gxras->gxData);
            gxras->gxData = nil;
            raster->destroy();
            tex->destroy();
            return nil;
        }
        GX_TEX_DIAG_PRINTF("[GX-DIAG] %s: malloc OK, reading %u bytes...\n", tex->name, dataSize);
        stream->read8(linearPixels, dataSize);
        GX_TEX_DIAG_PRINTF("[GX-DIAG] %s: read8 OK\n", tex->name);
        if(fixTransparentFringeRGBA8) {
            uint32 fixed = bleedTransparentEdgeRGB(tex, linearPixels, width, height);
            static int s_rgba8FringeLogCount = 0;
            if((fixed > 0 || gxTexNeedsFocusLog(tex->name)) &&
               s_rgba8FringeLogCount < 128) {
                printf("[GX-FRINGE] %s: rgba8 transparent-edge bleed fixed=%u (%dx%d)\n",
                       tex->name, fixed, width, height);
                s_rgba8FringeLogCount++;
            }
        }
        bool skipCMPRSelectorReorder = isCMPR &&
                                       shouldSkipCMPRSelectorReorder(tex->name);
        if(isCMPR && !expandCMPRToRGBA8 && !expandCMPRToRGB5A3 &&
           !skipCMPRSelectorReorder && GX_CMPR_FIX_DXT1_SELECTOR_ORDER) {
            convertDXT1SelectorOrderToGXCMPR(linearPixels, width, height);
        }

        if(isCMPR && !forceOpaqueExpandedCMPR)
            cmprDetectedTransparentPixels = cmprHasTransparentPixels(linearPixels,
                                                                     width,
                                                                     height);

        if(isCMPR && shouldSanitizeOpaqueCMPR(tex, width, height, hasAlpha,
                                              cmprDetectedTransparentPixels)) {
            uint32 fixedBlocks = sanitizeOpaqueCMPR(linearPixels, width, height);
            if(fixedBlocks > 0) {
                static int s_cmprFixLogCount = 0;
                if(s_cmprFixLogCount < 64 || gxTexNeedsFocusLog(tex->name)) {
                    printf("[GX-CMPRFIX] %s: forced opaque cmpr blocks=%u alphaMeta=%u (%dx%d)\n",
                           tex->name, fixedBlocks, (unsigned)hasAlpha, width, height);
                    s_cmprFixLogCount++;
                }
            }
        } else if(isCMPR && hasAlpha == 0 && gxTexNeedsFocusLog(tex->name)) {
            static int s_cmprSkipLogCount = 0;
            if(s_cmprSkipLogCount < 32) {
                printf("[GX-CMPRFIX] %s: skip opaque sanitize (hint=%d detected=%d)\n",
                       tex->name,
                       alphaHintByName ? 1 : 0,
                       cmprDetectedTransparentPixels ? 1 : 0);
                s_cmprSkipLogCount++;
            }
        }
    }

    uint32 effectiveHasAlpha = hasAlpha;
    if(isCMPR && effectiveHasAlpha == 0 &&
       (cmprDetectedTransparentPixels || alphaHintByName))
        effectiveHasAlpha = 1;
    if(expandCMPRColorKeyVegetationToRGB5A3)
        effectiveHasAlpha = 1;

    bool forceOpaqueAlpha = isCMPR &&
                            isLikelyOpaqueWorldTexture(tex->name) &&
                            !cmprDetectedTransparentPixels &&
                            !alphaHintByName;

    gxras->hasAlpha = (effectiveHasAlpha && !forceOpaqueAlpha) ? 1 : 0;
    if(!gxras->hasAlpha)
        gxras->alphaKind = GX_RASTER_ALPHA_NONE;
    else if(isCMPR)
        gxras->alphaKind = GX_RASTER_ALPHA_CUTOUT;
    else if(isRGBA8){
        gxras->alphaKind = usedStreamTiler ? streamedAlphaKind :
            classifyLinearRGBA8Alpha(linearPixels, width, height);
        gxras->hasAlpha = gxras->alphaKind != GX_RASTER_ALPHA_NONE ? 1 : 0;
    }else
        gxras->alphaKind = GX_RASTER_ALPHA_SMOOTH;
    if(isCMPR && effectiveHasAlpha != hasAlpha && gxTexNeedsFocusLog(tex->name)) {
        printf("[GX-ALPHAFIX] %s: alphaMeta=%u -> effective=%u detect=%d hint=%d\n",
               tex->name,
               (unsigned)hasAlpha,
               (unsigned)effectiveHasAlpha,
               cmprDetectedTransparentPixels ? 1 : 0,
               alphaHintByName ? 1 : 0);
    }

    // é¹â¬é¹â¬ Tile the pixel data é¹â¬é¹â¬
    if (expandCMPRToRGBA8) {
        uint32 rgbaSize = (uint32)width * (uint32)height * 4u;
        uint8 *rgbaPixels = allocTempPixels(tex, rgbaSize);
        if(rgbaPixels == nil) {
            printf("[GX-RADAR] %s: RGBA8 temp alloc FAIL (%u)\n",
                   tex->name, rgbaSize);
            gxMemFree(gxras->gxData);
            gxras->gxData = nil;
            raster->destroy();
            tex->destroy();
            return nil;
        }
        decodeCMPRToRGBA8(rgbaPixels, linearPixels, width, height);
        tileRGBA8((uint8 *)gxras->gxData, rgbaPixels, width, height);
        freeTempPixels(rgbaPixels);
    } else if (expandCMPRToRGB5A3) {
        tileCMPRToRGB5A3((uint8 *)gxras->gxData,
                         linearPixels,
                         width,
                         height,
                         forceOpaqueExpandedCMPR,
                         expandCMPRColorKeyVegetationToRGB5A3);
    } else if (isCMPR) {
        tileCMPR((uint8 *)gxras->gxData, linearPixels, width, height);
    } else if (!usedStreamTiler && useRGB5A3) {
        tileRGB5A3((uint8 *)gxras->gxData, linearPixels, width, height);
    } else if (!usedStreamTiler) {
        tileRGBA8((uint8 *)gxras->gxData, linearPixels, width, height);
    }

#if GX_TEX_VERBOSE_DIAG
    {
        static int s_texByteDiagCount = 0;
        if (s_texByteDiagCount < 10) {
            uint8 *dst = (uint8 *)gxras->gxData;
            printf("[GX-BYTES] %s lin=%02X %02X %02X %02X %02X %02X %02X %02X "
                   "tiled=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                   tex->name,
                   usedStreamTiler ? firstSrcBytes[0] : (dataSize > 0 ? linearPixels[0] : 0),
                   usedStreamTiler ? firstSrcBytes[1] : (dataSize > 1 ? linearPixels[1] : 0),
                   usedStreamTiler ? firstSrcBytes[2] : (dataSize > 2 ? linearPixels[2] : 0),
                   usedStreamTiler ? firstSrcBytes[3] : (dataSize > 3 ? linearPixels[3] : 0),
                   usedStreamTiler ? firstSrcBytes[4] : (dataSize > 4 ? linearPixels[4] : 0),
                   usedStreamTiler ? firstSrcBytes[5] : (dataSize > 5 ? linearPixels[5] : 0),
                   usedStreamTiler ? firstSrcBytes[6] : (dataSize > 6 ? linearPixels[6] : 0),
                   usedStreamTiler ? firstSrcBytes[7] : (dataSize > 7 ? linearPixels[7] : 0),
                   tiledSize > 0 ? dst[0] : 0,
                   tiledSize > 1 ? dst[1] : 0,
                   tiledSize > 2 ? dst[2] : 0,
                   tiledSize > 3 ? dst[3] : 0,
                   tiledSize > 4 ? dst[4] : 0,
                   tiledSize > 5 ? dst[5] : 0,
                   tiledSize > 6 ? dst[6] : 0,
                   tiledSize > 7 ? dst[7] : 0);
            s_texByteDiagCount++;
        }
    }
#endif

    if(linearPixels)
        freeTempPixels(linearPixels);
    GX_TEX_DIAG_PRINTF("[GX-DIAG] %s: tile+free OK, setting up GXTexObj...\n", tex->name);

    // Register in texture pool for shrink management
    texPoolRegister(raster, gxras->gxData, tiledSize,
                    (uint16)width, (uint16)height, (uint8)outFmt,
                    tex->name);

    // é¹â¬é¹â¬ Set up GXTexObj é¹â¬é¹â¬
    DCFlushRange(gxras->gxData, tiledSize);

    uint8 wrapS = gxWrapFromRw(tex->getAddressU());
    uint8 wrapT = gxWrapFromRw(tex->getAddressV());
    uint8 minf, magf;
    gxFilterFromRw(tex, &minf, &magf);
    gxras->wrapS = wrapS;
    gxras->wrapT = wrapT;
    gxras->minFilter = minf;
    gxras->magFilter = magf;
    gxras->preferOwnSampler = shouldPreferOwnSamplerState(tex->name) ? 1 : 0;

    gxLogFocusedTexture(tex, gxFmt, outFmt, gxras->hasAlpha, width, height,
                        dataSize, minf, magf, wrapS, wrapT, useRGB5A3);

    GX_InitTexObj(&gxras->texObj,
                  gxras->gxData,
                  (u16)width, (u16)height,
                  (u8)outFmt,
                  wrapS, wrapT,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&gxras->texObj, minf, magf);

    GX_InvalidateTexAll();
    gxras->texObjValid = true;
    texPoolEnforceBudget(tex->name);

    tex->raster = raster;
    GX_TEX_DIAG_PRINTF("[GX-DONE] %s returned OK\n", tex->name);
    return tex;
}

} // namespace gx
} // namespace rw

#endif // GAMECUBE
