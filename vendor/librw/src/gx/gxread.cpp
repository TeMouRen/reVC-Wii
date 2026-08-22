/**
 * GX native texture reader for the offline-tiled v2 asset contract.
 *
 * Header fields are little-endian RenderWare values. The payload is already in
 * the exact tiled byte order consumed by GX_InitTexObj, so this reader performs
 * one final allocation and one direct stream read with no format inference or
 * runtime conversion.
 */

#ifdef GAMECUBE
#define PLUGIN_ID ID_RASTERGX

#include <gccore.h>
#include <stdio.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"

#include "gxmemory.h"
#include "rwgx.h"

namespace rw {
namespace gx {

namespace {

enum {
    GX_NATIVE_V2_HEADER_SIZE = 88,
    GX_NATIVE_V2_FLAG_ALPHA_MASK = 0x3,
    GX_NATIVE_V2_FLAG_OWN_SAMPLER = 0x4,
    GX_NATIVE_V2_FLAG_MASK = 0x7
};

static_assert(GX_RASTER_ALPHA_NONE == 0, "v2 alpha contract mismatch");
static_assert(GX_RASTER_ALPHA_CUTOUT == 1, "v2 alpha contract mismatch");
static_assert(GX_RASTER_ALPHA_SMOOTH == 2, "v2 alpha contract mismatch");

struct NativeV2Header
{
    uint32 filterAddressing;
    char name[32];
    char mask[32];
    uint32 gxFormat;
    uint32 flags;
    uint16 width;
    uint16 height;
    uint32 dataSize;
};

static bool
readU32(Stream *stream, uint32 *value)
{
    return stream->read32(value, sizeof(*value)) == sizeof(*value);
}

static bool
readU16(Stream *stream, uint16 *value)
{
    return stream->read16(value, sizeof(*value)) == sizeof(*value);
}

static bool
readHeader(Stream *stream, NativeV2Header *header)
{
    return readU32(stream, &header->filterAddressing) &&
           stream->read8(header->name, sizeof(header->name)) == sizeof(header->name) &&
           stream->read8(header->mask, sizeof(header->mask)) == sizeof(header->mask) &&
           readU32(stream, &header->gxFormat) &&
           readU32(stream, &header->flags) &&
           readU16(stream, &header->width) &&
           readU16(stream, &header->height) &&
           readU32(stream, &header->dataSize);
}

static bool
expectedTiledSize(uint32 gxFormat, uint16 width, uint16 height, uint32 *result)
{
    if(width == 0 || height == 0 || result == nil)
        return false;

    const uint64 w = width;
    const uint64 h = height;
    uint64 size = 0;
    switch(gxFormat) {
    case GX_TF_I8:
        size = ((w + 7u) / 8u) * ((h + 3u) / 4u) * 32u;
        break;
    case GX_TF_IA8:
    case GX_TF_RGB5A3:
        size = ((w + 3u) / 4u) * ((h + 3u) / 4u) * 32u;
        break;
    case GX_TF_RGBA8:
        size = ((w + 3u) / 4u) * ((h + 3u) / 4u) * 64u;
        break;
    case GX_TF_CMPR:
        size = (((w + 3u) / 4u + 1u) / 2u) *
               (((h + 3u) / 4u + 1u) / 2u) * 32u;
        break;
    default:
        return false;
    }

    if(size == 0 || size > 0xFFFFFFFFu)
        return false;
    *result = (uint32)size;
    return true;
}

static bool
validFlagsForFormat(uint32 gxFormat, uint32 flags)
{
    if((flags & ~GX_NATIVE_V2_FLAG_MASK) != 0)
        return false;

    const uint32 alphaKind = flags & GX_NATIVE_V2_FLAG_ALPHA_MASK;
    if(alphaKind > GX_RASTER_ALPHA_SMOOTH)
        return false;
    if(gxFormat == GX_TF_I8 && alphaKind != GX_RASTER_ALPHA_NONE)
        return false;
    if(gxFormat == GX_TF_CMPR && alphaKind == GX_RASTER_ALPHA_SMOOTH)
        return false;
    return true;
}

static Texture*
rejectTexture(Texture *texture, Raster *raster)
{
    if(raster)
        raster->destroy();
    if(texture)
        texture->destroy();
    return nil;
}

} // namespace

Texture*
readNativeTexture(Stream *stream)
{
    uint32 structureLength = 0;
    if(!findChunk(stream, ID_STRUCT, &structureLength, nil)) {
        RWERROR((ERR_CHUNK, "STRUCT"));
        return nil;
    }

    uint32 platform = 0;
    if(!readU32(stream, &platform))
        return nil;
    if(platform != PLATFORM_GX_TILED_V2) {
        RWERROR((ERR_PLATFORM, platform));
        return nil;
    }

    NativeV2Header header;
    if(!readHeader(stream, &header)) {
        SYS_Report("[GX-V2] truncated native texture header\n");
        return nil;
    }
    header.name[sizeof(header.name) - 1] = '\0';
    header.mask[sizeof(header.mask) - 1] = '\0';

    uint32 expectedSize = 0;
    const uint64 expectedStructureLength =
        (uint64)GX_NATIVE_V2_HEADER_SIZE + (uint64)header.dataSize;
    if(!expectedTiledSize(header.gxFormat, header.width, header.height, &expectedSize) ||
       !validFlagsForFormat(header.gxFormat, header.flags) ||
       header.dataSize != expectedSize ||
       expectedStructureLength != structureLength) {
        SYS_Report("[GX-V2] reject tex='%s' fmt=%u flags=%u wh=%ux%u data=%u struct=%u\n",
                   header.name, (unsigned)header.gxFormat, (unsigned)header.flags,
                   (unsigned)header.width, (unsigned)header.height,
                   (unsigned)header.dataSize, (unsigned)structureLength);
        return nil;
    }

    Texture *texture = Texture::create(nil);
    if(texture == nil)
        return nil;
    texture->filterAddressing = header.filterAddressing;
    if((texture->filterAddressing & 0xF000) == 0)
        texture->filterAddressing |= (texture->filterAddressing & 0x0F00) << 4;
    memcpy(texture->name, header.name, sizeof(texture->name));
    memcpy(texture->mask, header.mask, sizeof(texture->mask));
    texture->name[sizeof(texture->name) - 1] = '\0';
    texture->mask[sizeof(texture->mask) - 1] = '\0';

    Raster *raster = Raster::create(
        header.width, header.height, 32, Raster::TEXTURE, PLATFORM_GX);
    if(raster == nil)
        return rejectTexture(texture, nil);

    GxRaster *gxRaster = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
    gxRaster->gxFmt = (uint8)header.gxFormat;
    gxRaster->alphaKind = (uint8)(header.flags & GX_NATIVE_V2_FLAG_ALPHA_MASK);
    gxRaster->hasAlpha = gxRaster->alphaKind != GX_RASTER_ALPHA_NONE ? 1 : 0;
    gxRaster->preferOwnSampler =
        (header.flags & GX_NATIVE_V2_FLAG_OWN_SAMPLER) != 0 ? 1 : 0;
    gxRaster->dataSize = header.dataSize;
    gxRaster->w = header.width;
    gxRaster->h = header.height;
    gxRaster->gxData = safeGxAlloc(header.dataSize, 32, texture->name);
    if(gxRaster->gxData == nil)
        return rejectTexture(texture, raster);

    if(stream->read8(gxRaster->gxData, header.dataSize) != header.dataSize) {
        SYS_Report("[GX-V2] truncated payload tex='%s' expected=%u\n",
                   texture->name, (unsigned)header.dataSize);
        return rejectTexture(texture, raster);
    }

    if(!texPoolRegister(raster, gxRaster->gxData, header.dataSize,
                        header.width, header.height, (uint8)header.gxFormat,
                        texture->name)) {
        SYS_Report("[GX-V2] texture pool registration failed tex='%s' size=%u\n",
                   texture->name, (unsigned)header.dataSize);
        return rejectTexture(texture, raster);
    }

    syncNativeSamplerFromTexture(texture, raster);
    DCFlushRange(gxRaster->gxData, header.dataSize);
    GX_InitTexObj(&gxRaster->texObj,
                  gxRaster->gxData,
                  header.width, header.height,
                  (uint8)header.gxFormat,
                  gxRaster->wrapS, gxRaster->wrapT,
                  GX_FALSE);
    GX_InitTexObjFilterMode(
        &gxRaster->texObj, gxRaster->minFilter, gxRaster->magFilter);
    GX_InvalidateTexAll();
    gxRaster->texObjValid = true;
    texPoolEnforceBudget(texture->name);

    texture->raster = raster;
    return texture;
}

} // namespace gx
} // namespace rw

#endif // GAMECUBE
