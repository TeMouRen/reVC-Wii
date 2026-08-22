// vendor/librw/src/gx/gxmemory.h
// ========================================================================
// Wii GX Texture Memory Pool - Dynamic Degradation System
//
// Inspired by re3-3ds librw/src/3ds/memory.cpp safeLinearAlloc pattern.
// Maintains a sorted linked list of all loaded GX textures.
// When heap memory is exhausted, shrinks the largest texture to
// make room instead of returning NULL.
// ========================================================================
#pragma once

#ifdef RW_GX

#include <stdint.h>
#include <stddef.h>

namespace rw {

// fwd
struct Raster;

namespace gx {

typedef void (*TexPoolOwnerResidencyCallback)(uint16 ownerStreamId,
                                               uint32 poolBit,
                                               uint32 bytes);

enum GxRasterStorageMask
{
    GX_RASTER_STORAGE_NONE         = 0,
    GX_RASTER_STORAGE_GENERIC_MEM2 = 1u,
    GX_RASTER_STORAGE_DEDICATED    = 2u
};

enum GxTextureUsageClass
{
    GX_TEXTURE_USAGE_DEFAULT = 0,
    GX_TEXTURE_USAGE_PERSISTENT_UI = 1
};

// -- Texture Pool Entry ----------------------------------------------------
struct GxTexPoolEntry
{
    GxTexPoolEntry *next;
    Raster *raster;      // back-pointer for format/size access (never null while in pool)
    void   *gxData;      // the allocated GX buffer
    uint32  size;        // allocated size in bytes
    uint16  width;
    uint16  height;
    uint8   gxFmt;       // GX_TF_RGBA8, GX_TF_IA4, etc.
    uint8   shrinkCount; // number of times this texture has been shrunk
    uint16  ownerStreamId;
    char    name[32];
};

#ifdef WII
static_assert(sizeof(GxTexPoolEntry) == 56,
              "Wii texture pool entry must remain 56 bytes");
#endif

// -- Pool Management -------------------------------------------------------
bool    texPoolRegister(Raster *raster, void *gxData, uint32 size,
                        uint16 w, uint16 h, uint8 fmt,
                        const char *name = nullptr);
void    texPoolRename(Raster *raster, const char *name);
void    texPoolUnregister(void *gxData);
void    texPoolUpdateSize(void *gxData, uint32 newSize,
                          uint16 newW, uint16 newH);
void    texPoolEnforceBudget(const char *reason = nullptr);
void    setActiveHudWeaponRaster(Raster *raster);
void    clearActiveHudWeaponRaster(Raster *raster = nullptr);

// -- Shrink Operations -----------------------------------------------------
// Returns true if at least one texture was shrunk
bool    shrinkLargestTexture(void);
// Prefer shrinking textures that currently live outside the dedicated GX pool,
// because that returns memory directly to the normal rw heap.
bool    shrinkLargestTexturePreferHostMemory(void);
// Returns bytes freed (0 if nothing could be shrunk)
uint32  shrinkSomeTexture(void);

// -- Safe Allocation -------------------------------------------------------
// Tries memalign; on failure, triggers texture shrink and retries.
// Returns NULL only after exhausting all shrink options.
void*   safeGxAlloc(size_t size, size_t alignment, const char *tag = nullptr);
void*   gxMemAlloc(size_t size, size_t alignment);
void    gxMemFree(void *ptr);
bool    gxMemOwns(const void *ptr);
bool    gxMemCompact(const char *reason = nullptr, bool force = false);
bool    gxMemRunPendingCompactionAtGpuIdle(const char *reason = nullptr);
bool    gxMemHasPendingCompaction(void);
void    pushPersistentUiTextureUploadContext(const char *reason = nullptr);
void    popPersistentUiTextureUploadContext(const char *reason = nullptr);
uint8   currentTextureUsageClass(void);
void    markPersistentUiTexture(Raster *raster);
void    pushCriticalUiUploadContext(const char *reason = nullptr);
void    popCriticalUiUploadContext(const char *reason = nullptr);
bool    isCriticalUiUploadContextActive(void);
uint32  rasterStorageMask(const Raster *raster);

// -- Diagnostics -----------------------------------------------------------
void    texPoolDebug(void);
void    texPoolResidencyReport(const char *reason = nullptr);
uint32  texPoolTotalBytes(void);
int     texPoolCount(void);
void    texPoolSetSoftBudget(uint32 bytes);
void    texPoolResetSoftBudget(void);
uint32  texPoolGetSoftBudget(void);
uint32  gxMemGetShrinkTotalCount(void);
uint32  gxMemGetCompactionGeneration(void);
void    texPoolGetOwnerStats(uint32 *ownedGenericBytes,
                             uint32 *unknownGenericBytes,
                             uint32 *ownedGxBytes,
                             uint32 *unknownGxBytes);
void    texPoolVisitOwnerResidency(TexPoolOwnerResidencyCallback callback);

// -- Pixel Conversion Helpers ---------------------------------------------
// Untile GX RGBA8 -> linear RGBA8
void    convertGX_RGBA8_toLinear(void *dst, const void *src, int w, int h);
// Untile GX IA4 -> linear RGBA8
void    convertGX_IA4_toLinear(void *dst, const void *src, int w, int h);

} // namespace gx
} // namespace rw

#endif // RW_GX
