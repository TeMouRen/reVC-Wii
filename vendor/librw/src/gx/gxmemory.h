// vendor/librw/src/gx/gxmemory.h
// ════════════════════════════════════════════════════════════════
// Wii GX Texture Memory Pool �?Dynamic Degradation System
//
// Inspired by re3-3ds librw/src/3ds/memory.cpp safeLinearAlloc pattern.
// Maintains a sorted linked list of all loaded GX textures.
// When heap memory is exhausted, shrinks the largest texture to
// make room instead of returning NULL.
// ════════════════════════════════════════════════════════════════
#pragma once

#ifdef RW_GX

#include <stdint.h>
#include <stddef.h>

namespace rw {

// fwd
struct Raster;

namespace gx {

// ── Texture Pool Entry ────────────────────────────────────────
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
    char    name[32];
};

// ── Pool Management ───────────────────────────────────────────
void    texPoolRegister(Raster *raster, void *gxData, uint32 size,
                        uint16 w, uint16 h, uint8 fmt,
                        const char *name = nullptr);
void    texPoolRename(Raster *raster, const char *name);
void    texPoolUnregister(void *gxData);
void    texPoolUpdateSize(void *gxData, uint32 newSize,
                          uint16 newW, uint16 newH);
void    texPoolEnforceBudget(const char *reason = nullptr);

// ── Shrink Operations ─────────────────────────────────────────
// Returns true if at least one texture was shrunk
bool    shrinkLargestTexture(void);
// Prefer shrinking textures that currently live outside the dedicated GX pool,
// because that returns memory directly to the normal rw heap.
bool    shrinkLargestTexturePreferHostMemory(void);
// Returns bytes freed (0 if nothing could be shrunk)
uint32  shrinkSomeTexture(void);

// ── Safe Allocation ───────────────────────────────────────────
// Tries memalign; on failure, triggers texture shrink and retries.
// Returns NULL only after exhausting all shrink options.
void*   safeGxAlloc(size_t size, size_t alignment, const char *tag = nullptr);
void*   gxMemAlloc(size_t size, size_t alignment);
void    gxMemFree(void *ptr);
bool    gxMemOwns(const void *ptr);
void    pushCriticalUiUploadContext(const char *reason = nullptr);
void    popCriticalUiUploadContext(const char *reason = nullptr);
bool    isCriticalUiUploadContextActive(void);

// ── Diagnostics ────────────────────────────────────────────────
void    texPoolDebug(void);
uint32  texPoolTotalBytes(void);
int     texPoolCount(void);
void    texPoolSetSoftBudget(uint32 bytes);
uint32  texPoolGetSoftBudget(void);

// ── Pixel Conversion Helpers ──────────────────────────────────
// Untile GX RGBA8 �?linear RGBA8
void    convertGX_RGBA8_toLinear(void *dst, const void *src, int w, int h);
// Untile GX IA4 �?linear RGBA8
void    convertGX_IA4_toLinear(void *dst, const void *src, int w, int h);

} // namespace gx
} // namespace rw

#endif // RW_GX
