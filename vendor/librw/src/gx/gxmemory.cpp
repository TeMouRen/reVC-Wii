// vendor/librw/src/gx/gxmemory.cpp
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
// Wii GX Texture Memory Pool
//
// Strategy (GC-specific, differs from 3DS mipmap approach):
//   1. Maintain linked list of all GX raster allocations sorted by size
//   2. When safeGxAlloc fails â?pop largest texture â?halve resolution â?retry
//   3. Halving res = 4Ã memory reduction (half width Ã half height)
//   4. Minimum texture size: 4Ã4 (can't shrink below this)
//
// Shrink procedure for a single texture:
//   a) Untile gxData â?linear RGBA8 buffer
//   b) Downsample 2Ã box filter â?half-res linear buffer
//   c) free old gxData
//   d) Re-tile half-res â?new gxData
//   e) Update GxRaster + GXTexObj
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
#ifdef GAMECUBE

#include <gccore.h>
#include <ogc/system.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwgx.h"
#include "gxmemory.h"
#ifdef WII
void *MemoryMgrMallocMem2(size_t size, size_t align);
void *MemoryMgrMallocMem2Strict(size_t size, size_t align);
void MemoryMgrFreeMem2(void *mem);
void *MemoryMgrMallocAlignMem2(size_t size, size_t align);
void *MemoryMgrMallocAlignMem2Strict(size_t size, size_t align);
void MemoryMgrFreeAlignMem2(void *mem);
#endif

namespace rw {
namespace gx {

// ââ Global texture pool (sorted by size, descending) ââââââââââ
static GxTexPoolEntry *gTexPool = nullptr;
static int             gTexPoolCount = 0;
static uint32          gTexPoolTotalSize = 0;
static int             gShrinkTotalCount = 0;  // lifetime counter
static bool            gBudgetShrinkPass = false;
static bool            gPreferHostMemoryShrink = false;
#ifdef WII
static const uint32    GX_TEXP_DEFAULT_SOFT_BUDGET_BYTES = 26u * 1024u * 1024u;
static const uint32    GX_TEXP_MIN_SOFT_BUDGET_BYTES     = 22u * 1024u * 1024u;
static const uint32    GX_TEXP_MAX_SOFT_BUDGET_BYTES     = 28u * 1024u * 1024u;
#else
static const uint32    GX_TEXP_DEFAULT_SOFT_BUDGET_BYTES = 28u * 1024u * 1024u;
static const uint32    GX_TEXP_MIN_SOFT_BUDGET_BYTES     = 16u * 1024u * 1024u;
static const uint32    GX_TEXP_MAX_SOFT_BUDGET_BYTES     = 32u * 1024u * 1024u;
#endif
static uint32          gSoftBudgetBytes = GX_TEXP_DEFAULT_SOFT_BUDGET_BYTES;
static const uint32    gSoftBudgetSlackBytes = 512u * 1024u;
static void           *gShrinkEmergencyReserve = nullptr;
static size_t          gShrinkEmergencyReserveSize = 1024u * 1024u;
static int             gCriticalUiUploadDepth = 0;

// Maximum shrink attempts per safeGxAlloc call before giving up
#ifdef WII
#define MAX_SHRINK_ATTEMPTS  12
#define MAX_TEXPOOL_SCAN_STEPS  8192
#define MAX_ARENA2_FREE_STEPS   8192
#else
#define MAX_SHRINK_ATTEMPTS  32
#define MAX_TEXPOOL_SCAN_STEPS  16384
#define MAX_ARENA2_FREE_STEPS   16384
#endif
// Minimum texture dimension (won't shrink below this)
#define MIN_TEX_DIM           4
// Soft texture budget used while loading. Real GC target will need to be lower;
// this keeps enough headroom for geometry before we optimize streaming further.
#define GX_TEX_BUDGET_STEPS   256
#ifdef WII
#define GX_GXPOOL_MAX_BYTES        (64u * 1024u * 1024u)
#define GX_GXPOOL_MIN_BYTES        (24u * 1024u * 1024u)
#define GX_GXPOOL_KEEP_FREE_BYTES  (8u * 1024u * 1024u)
#else
#define GX_GXPOOL_MAX_BYTES        (32u * 1024u * 1024u)
#define GX_GXPOOL_MIN_BYTES        (12u * 1024u * 1024u)
#define GX_GXPOOL_KEEP_FREE_BYTES  (8u * 1024u * 1024u)
#endif
#define GX_GXPOOL_ALLOC_MAGIC      0x4758504Fu

// ââ Forward declarations ââââââââââââââââââââââââââââââââââââââ
static void downsample2x(const uint8 *src, int sw, int sh,
                         uint8 *dst, int dw, int dh);
static bool isShrinkableFormat(uint8 fmt);
static bool isBudgetProtectedTexture(const GxTexPoolEntry *entry);
static bool isUnnamedBudgetProtectedTexture(const GxTexPoolEntry *entry);
static bool allowLargeMemFallbackForTag(const char *tag);
static void logTextureShrink(const char *kind, const GxTexPoolEntry *entry,
                             int oldW, int oldH, uint32 oldSize,
                             int newW, int newH, uint32 newSize);
static void insertPoolEntrySorted(GxTexPoolEntry *entry);
static uint32 cmprTiledSizePadded(int w, int h);
static const uint8 *cmprBlockPtr(const uint8 *data, int w,
                                 int blockX, int blockY);
static void shrinkCMPRByBlockDrop(uint8 *dst, const uint8 *src,
                                  int oldW, int newW, int newH);
static void ensureShrinkEmergencyReserve(void);
static void releaseShrinkEmergencyReserve(const char *reason);
static void tightenBudgetForSafeAlloc(size_t size);
static uint32 clampSoftBudgetToRuntimePool(uint32 bytes);
static uint32 getPoolAwareBudgetWithHeadroom(uint32 headroomBytes);
static uintptr_t alignUpPow2(uintptr_t value, size_t alignment);
static uintptr_t alignDownPow2(uintptr_t value, size_t alignment);
static void* allocCpuTemp(size_t size, size_t alignment);
static void freeCpuTemp(void *ptr);
static void initArena2Pool(void);
static void* arena2Alloc(size_t size, size_t alignment);
static void arena2Free(void *ptr);

struct Arena2FreeBlock
{
    Arena2FreeBlock *prev;
    Arena2FreeBlock *next;
    size_t size;
};

struct Arena2AllocHeader
{
    uint32 magic;
    uint32 reserved;
    size_t totalSize;
};

static bool             gArena2InitAttempted = false;
static bool             gArena2Ready = false;
static uint8           *gArena2Base = nullptr;
static uint8           *gArena2End = nullptr;
static size_t           gArena2Size = 0;
static Arena2FreeBlock *gArena2FreeList = nullptr;
static uint32           gArena2FallbackCount = 0;

static uintptr_t
alignUpPow2(uintptr_t value, size_t alignment)
{
    if(alignment <= 1)
        return value;
    uintptr_t mask = (uintptr_t)alignment - 1u;
    return (value + mask) & ~mask;
}

static uintptr_t
alignDownPow2(uintptr_t value, size_t alignment)
{
    if(alignment <= 1)
        return value;
    uintptr_t mask = (uintptr_t)alignment - 1u;
    return value & ~mask;
}

static void*
allocCpuTemp(size_t size, size_t alignment)
{
    if(size == 0)
        size = 1;

#ifdef WII
    return MemoryMgrMallocAlignMem2Strict(size, alignment > 1 ? alignment : 32);
#else
    void *ptr = nil;
    if(alignment > 1)
        ptr = memalign(alignment, size);
    if(ptr == nil)
        ptr = malloc(size);
    return ptr;
#endif
}

static void
freeCpuTemp(void *ptr)
{
    if(ptr)
#ifdef WII
        MemoryMgrFreeAlignMem2(ptr);
#else
        free(ptr);
#endif
}

static void
arena2MergeForward(Arena2FreeBlock *block)
{
    if(block == nullptr || block->next == nullptr)
        return;

    Arena2FreeBlock *next = block->next;
    if((uint8*)block + block->size != (uint8*)next)
        return;

    block->size += next->size;
    block->next = next->next;
    if(block->next)
        block->next->prev = block;
}

static void
arena2InsertFreeBlock(Arena2FreeBlock *block)
{
    block->prev = nullptr;
    block->next = nullptr;

    if(gArena2FreeList == nullptr) {
        gArena2FreeList = block;
        return;
    }

    Arena2FreeBlock *cur = gArena2FreeList;
    Arena2FreeBlock *prev = nullptr;
    while(cur && (uintptr_t)cur < (uintptr_t)block) {
        prev = cur;
        cur = cur->next;
    }

    block->prev = prev;
    block->next = cur;
    if(prev)
        prev->next = block;
    else
        gArena2FreeList = block;
    if(cur)
        cur->prev = block;

    arena2MergeForward(block);
    if(block->prev)
        arena2MergeForward(block->prev);
}

static void
initArena2Pool(void)
{
    if(gArena2InitAttempted)
        return;

    gArena2InitAttempted = true;

    uintptr_t rawLo = (uintptr_t)SYS_GetArena2Lo();
    uintptr_t rawHi = (uintptr_t)SYS_GetArena2Hi();
    if(rawHi <= rawLo) {
        printf("[GX-POOL] Arena2 unavailable (lo=%p hi=%p)\n",
               (void*)rawLo, (void*)rawHi);
        return;
    }

    size_t available = (size_t)(rawHi - rawLo);
    // Reserve the dedicated GX pool straight out of Arena2/MEM2 instead of
    // routing it through the normal malloc heap. This keeps texture traffic
    // away from the general-purpose heap and lets us size the pool from the
    // real MEM2 budget rather than the remaining MEM1 window.
#ifdef WII
    // Wii has enough MEM2 to keep the GX texture pool near its intended cap.
    // The old half-arena rule came from the GC pressure path and caused early
    // texture shrink even while plenty of MEM2 was still available.
    size_t poolSize = available;
    if(poolSize > GX_GXPOOL_KEEP_FREE_BYTES)
        poolSize -= GX_GXPOOL_KEEP_FREE_BYTES;
    else
        poolSize = available / 2u;
#else
    size_t poolSize = available / 2u;
#endif

    if(poolSize > GX_GXPOOL_MAX_BYTES)
        poolSize = GX_GXPOOL_MAX_BYTES;
    if(poolSize < GX_GXPOOL_MIN_BYTES)
        poolSize = GX_GXPOOL_MIN_BYTES;

    if(available > GX_GXPOOL_KEEP_FREE_BYTES &&
       poolSize > available - GX_GXPOOL_KEEP_FREE_BYTES)
        poolSize = available - GX_GXPOOL_KEEP_FREE_BYTES;

    poolSize &= ~(size_t)31u;

    if(poolSize < sizeof(Arena2FreeBlock) + 256u) {
        printf("[GX-POOL] not enough Arena2 for dedicated GX pool (%u KB available)\n",
               (unsigned)(available / 1024u));
        return;
    }

    uintptr_t poolEnd = alignDownPow2(rawHi, 32);
    uintptr_t poolBase = poolEnd - poolSize;
    if(poolBase < rawLo) {
        printf("[GX-POOL] Arena2 reservation underflow (%u KB requested, %u KB available)\n",
               (unsigned)(poolSize / 1024u),
               (unsigned)(available / 1024u));
        return;
    }

    SYS_SetArena2Hi((void*)poolBase);

    gArena2Base = (uint8*)poolBase;
    gArena2End = (uint8*)poolEnd;
    gArena2Size = poolSize;
    gArena2FreeList = (Arena2FreeBlock*)gArena2Base;
    gArena2FreeList->prev = nullptr;
    gArena2FreeList->next = nullptr;
    gArena2FreeList->size = gArena2Size;
    gArena2Ready = true;

    printf("[GX-POOL] reserved %u KB dedicated GX pool from Arena2/MEM2 (arena=%u KB, base=%p end=%p, hi=%p)\n",
           (unsigned)(gArena2Size / 1024u),
           (unsigned)(available / 1024u),
           (void*)gArena2Base, (void*)gArena2End, (void*)SYS_GetArena2Hi());

    uint32 oldBudget = gSoftBudgetBytes;
    gSoftBudgetBytes = clampSoftBudgetToRuntimePool(gSoftBudgetBytes);
    if(gSoftBudgetBytes != oldBudget) {
        printf("[GX-TEXP] runtime soft budget clamp %uKB -> %uKB (pool=%uKB)\n",
               oldBudget / 1024u,
               gSoftBudgetBytes / 1024u,
               (unsigned)(gArena2Size / 1024u));
    }
}

static uint32
clampSoftBudgetToRuntimePool(uint32 bytes)
{
    uint32 minBudget = GX_TEXP_MIN_SOFT_BUDGET_BYTES;
    uint32 maxBudget = GX_TEXP_MAX_SOFT_BUDGET_BYTES;
#ifdef WII
    initArena2Pool();
    if(gArena2Ready && gArena2Size > 0) {
        uint32 runtimeCap = (uint32)gArena2Size;
        const uint32 runtimeHeadroom = 2u * 1024u * 1024u;
        if(runtimeCap > runtimeHeadroom)
            runtimeCap -= runtimeHeadroom;
        else
            runtimeCap /= 2u;
        if(runtimeCap < maxBudget)
            maxBudget = runtimeCap;
    }
#endif
    if(minBudget > maxBudget)
        minBudget = maxBudget;
    if(bytes < minBudget)
        bytes = minBudget;
    if(bytes > maxBudget)
        bytes = maxBudget;
    return bytes;
}

static uint32
getPoolAwareBudgetWithHeadroom(uint32 headroomBytes)
{
#ifdef WII
    initArena2Pool();
    if(gArena2Ready && gArena2Size > 0) {
        uint32 poolBudget = (uint32)gArena2Size;
        if(poolBudget > headroomBytes)
            poolBudget -= headroomBytes;
        else
            poolBudget /= 2u;
        return clampSoftBudgetToRuntimePool(poolBudget);
    }
#endif
    return clampSoftBudgetToRuntimePool(GX_TEXP_DEFAULT_SOFT_BUDGET_BYTES);
}

bool
gxMemOwns(const void *ptr)
{
    initArena2Pool();
    if(!gArena2Ready || ptr == nullptr)
        return false;
    return (const uint8*)ptr >= gArena2Base && (const uint8*)ptr < gArena2End;
}

static void*
allocFallbackHostMemory(size_t size, size_t alignment)
{
#ifdef WII
    if(alignment < 32)
        alignment = 32;
    return MemoryMgrMallocMem2(size, alignment);
#else
    return memalign(alignment, size);
#endif
}

static void*
allocFallbackHostMemoryStrict(size_t size, size_t alignment)
{
#ifdef WII
    if(alignment < 32)
        alignment = 32;
    return MemoryMgrMallocMem2Strict(size, alignment);
#else
    return memalign(alignment, size);
#endif
}

static void*
arena2Alloc(size_t size, size_t alignment)
{
    initArena2Pool();
    if(!gArena2Ready)
        return nullptr;

    if(size == 0)
        size = 1;
    if(alignment < 4)
        alignment = 4;

    int stepCount = 0;
    for(Arena2FreeBlock *block = gArena2FreeList; block; block = block->next) {
        if(++stepCount > MAX_ARENA2_FREE_STEPS) {
            printf("[GX-POOL] arena2 free-list runaway size=%u align=%u head=%p steps=%d\n",
                   (unsigned)size, (unsigned)alignment,
                   (void*)gArena2FreeList, stepCount);
            return nullptr;
        }
        if(block->next == block) {
            printf("[GX-POOL] arena2 free-list self-loop block=%p size=%u\n",
                   (void*)block, (unsigned)block->size);
            return nullptr;
        }

        uintptr_t blockStart = (uintptr_t)block;
        uintptr_t userPtr = alignUpPow2(blockStart + sizeof(Arena2AllocHeader) +
                                        sizeof(Arena2AllocHeader*), alignment);
        size_t totalUsed = (size_t)(userPtr + size - blockStart);
        if(totalUsed < sizeof(Arena2FreeBlock))
            totalUsed = sizeof(Arena2FreeBlock);
        totalUsed = (size_t)alignUpPow2((uintptr_t)totalUsed, 32);
        if(totalUsed > block->size)
            continue;

        Arena2FreeBlock *prev = block->prev;
        Arena2FreeBlock *next = block->next;
        size_t remainder = block->size - totalUsed;

        if(remainder >= sizeof(Arena2FreeBlock)) {
            Arena2FreeBlock *tail = (Arena2FreeBlock*)(blockStart + totalUsed);
            tail->prev = prev;
            tail->next = next;
            tail->size = remainder;
            if(prev)
                prev->next = tail;
            else
                gArena2FreeList = tail;
            if(next)
                next->prev = tail;
        } else {
            totalUsed = block->size;
            if(prev)
                prev->next = next;
            else
                gArena2FreeList = next;
            if(next)
                next->prev = prev;
        }

        Arena2AllocHeader *header = (Arena2AllocHeader*)blockStart;
        header->magic = GX_GXPOOL_ALLOC_MAGIC;
        header->reserved = 0;
        header->totalSize = totalUsed;
        ((Arena2AllocHeader**)userPtr)[-1] = header;
        return (void*)userPtr;
    }

    return nullptr;
}

static void
arena2Free(void *ptr)
{
    if(ptr == nullptr || !gxMemOwns(ptr))
        return;

    Arena2AllocHeader *header = ((Arena2AllocHeader**)ptr)[-1];
    if(header == nullptr ||
       (uint8*)header < gArena2Base ||
       (uint8*)header + sizeof(Arena2AllocHeader) > gArena2End ||
       header->magic != GX_GXPOOL_ALLOC_MAGIC ||
       header->totalSize < sizeof(Arena2FreeBlock)) {
        printf("[GX-POOL] invalid free ptr=%p header=%p\n", ptr, (void*)header);
        return;
    }

    header->magic = 0;

    Arena2FreeBlock *block = (Arena2FreeBlock*)header;
    block->size = header->totalSize;
    arena2InsertFreeBlock(block);
}

// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
// Pool Management
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ

void
texPoolRegister(Raster *raster, void *gxData, uint32 size,
                uint16 w, uint16 h, uint8 fmt, const char *name)
{
    if(!gxData || size == 0) return;

    // Allocate entry
    GxTexPoolEntry *entry = (GxTexPoolEntry*)malloc(sizeof(GxTexPoolEntry));
    if(!entry) return;  // pool bookkeeping OOM â?skip tracking (texture still works)

    entry->raster      = raster;
    entry->gxData      = gxData;
    entry->size        = size;
    entry->width       = w;
    entry->height      = h;
    entry->gxFmt       = fmt;
    entry->shrinkCount = 0;
    entry->next        = nullptr;
    if(name && name[0]) {
        strncpy(entry->name, name, sizeof(entry->name)-1);
        entry->name[sizeof(entry->name)-1] = '\0';
    } else {
        entry->name[0] = '\0';
        // Diagnostics for the long-lived anonymous textures that tend to
        // starve the pool later if they never receive a proper Texture name.
        static int s_unnamedLargeRegisterLogCount = 0;
        if(s_unnamedLargeRegisterLogCount < 48 && size >= 256u * 1024u) {
            printf("[GX-REGUNNAMED] %dx%d fmt=%u size=%u raster=%p gx=%p pool=%uKB tex=%d\n",
                   (int)w, (int)h, (unsigned)fmt, (unsigned)size,
                   (void*)raster, gxData,
                   gTexPoolTotalSize / 1024, gTexPoolCount);
            s_unnamedLargeRegisterLogCount++;
        }
    }

    // Insert sorted by size (descending â?largest first = easy shrink target)
    insertPoolEntrySorted(entry);

    gTexPoolCount++;
    gTexPoolTotalSize += size;

    if(entry->name[0] != '\0') {
        int sameNameCount = 0;
        uint32 sameNameTotal = 0;
        for(GxTexPoolEntry *scan = gTexPool; scan; scan = scan->next) {
            if(strcasecmp(scan->name, entry->name) != 0)
                continue;
            sameNameCount++;
            sameNameTotal += scan->size;
        }
        if(sameNameCount >= 4 && size >= 32u * 1024u) {
            (void)sameNameTotal;
        }
    }
}

void
texPoolUnregister(void *gxData)
{
    if(!gxData) return;

    GxTexPoolEntry **prev = &gTexPool;
    GxTexPoolEntry *cur  = gTexPool;

    while(cur) {
        if(cur->gxData == gxData) {
            *prev = cur->next;
            gTexPoolCount--;
            gTexPoolTotalSize -= cur->size;
            free(cur);
            return;
        }
        prev = &cur->next;
        cur  = cur->next;
    }
}

void
texPoolRename(Raster *raster, const char *name)
{
    if(!raster)
        return;

    GxTexPoolEntry *cur = gTexPool;
    while(cur) {
        if(cur->raster == raster) {
            bool wasUnnamed = cur->name[0] == '\0';
            char oldName[sizeof(cur->name)];
            strncpy(oldName, cur->name, sizeof(oldName) - 1);
            oldName[sizeof(oldName) - 1] = '\0';
            if(name && name[0]) {
                strncpy(cur->name, name, sizeof(cur->name) - 1);
                cur->name[sizeof(cur->name) - 1] = '\0';
            } else {
                cur->name[0] = '\0';
            }
            if((wasUnnamed && cur->size >= 256u * 1024u) ||
               (cur->width == 512 && cur->height == 512 && cur->size >= 512u * 1024u)) {
                static int s_renameLogCount = 0;
                if(s_renameLogCount < 96) {
                    printf("[GX-RENAME] raster=%p %s -> %s %dx%d fmt=%u size=%u pool=%uKB tex=%d\n",
                           (void*)raster,
                           oldName[0] ? oldName : "<unknown>",
                           cur->name[0] ? cur->name : "<unknown>",
                           cur->width, cur->height,
                           (unsigned)cur->gxFmt,
                           (unsigned)cur->size,
                           gTexPoolTotalSize / 1024,
                           gTexPoolCount);
                    s_renameLogCount++;
                }
            }
            return;
        }
        cur = cur->next;
    }
}

void
texPoolUpdateSize(void *gxData, uint32 newSize, uint16 newW, uint16 newH)
{
    if(!gxData) return;

    GxTexPoolEntry *cur = gTexPool;
    while(cur) {
        if(cur->gxData == gxData) {
            uint32 oldSize = cur->size;
            cur->size  = newSize;
            cur->width = newW;
            cur->height = newH;
            // Re-sort: remove and re-insert
            // Simple approach for now: just leave in place (size changed but still sorted-ish)
            // Full re-sort would require removing and re-inserting in order
            gTexPoolTotalSize = gTexPoolTotalSize - oldSize + newSize;
            return;
        }
        cur = cur->next;
    }
}

// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
// Untile Functions (GX tiled â?linear RGBA8)
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ

// GX RGBA8 tile: 4Ã4 blocks, 64 bytes each
//   bytes  0-31: AR pairs (alpha, red per pixel)
//   bytes 32-63: GB pairs (green, blue per pixel)
void
convertGX_RGBA8_toLinear(void *dst, const void *src, int w, int h)
{
    uint8       *d = (uint8*)dst;
    const uint8 *s = (const uint8*)src;

    for(int by = 0; by < h; by += 4) {
        for(int bx = 0; bx < w; bx += 4) {
            // AR sub-block (32 bytes) â?Alpha, Red for 4Ã4
            const uint8 *ar = s;
            // GB sub-block (32 bytes)
            const uint8 *gb = s + 32;

            for(int iy = 0; iy < 4; iy++) {
                int py = by + iy;
                if(py >= h) py = h - 1;
                for(int ix = 0; ix < 4; ix++) {
                    int px = bx + ix;
                    if(px >= w) px = w - 1;

                    uint8 *pixel = d + py * w * 4 + px * 4;
                    pixel[0] = ar[ix*2 + 1];  // R
                    pixel[1] = gb[ix*2 + 0];  // G
                    pixel[2] = gb[ix*2 + 1];  // B
                    pixel[3] = ar[ix*2 + 0];  // A
                }
                ar += 8;  // next row in AR sub-block (4 pixels Ã 2 bytes)
                gb += 8;
            }
            s += 64; // next tile
        }
    }
}

// GX IA4 tile: 8Ã4 blocks, 32 bytes each
// Each byte = [A3..A0][I3..I0]
void
convertGX_IA4_toLinear(void *dst, const void *src, int w, int h)
{
    uint8       *d = (uint8*)dst;
    const uint8 *s = (const uint8*)src;
    int pw = (w + 7) & ~7;
    int ph = (h + 3) & ~3;

    for(int by = 0; by < ph; by += 4) {
        for(int bx = 0; bx < pw; bx += 8) {
            for(int iy = 0; iy < 4; iy++) {
                int py = by + iy;
                if(py >= h) py = h - 1;
                for(int ix = 0; ix < 8; ix++) {
                    int px = bx + ix;
                    if(px >= w) px = w - 1;

                    uint8 byte = *s++;
                    uint8 alpha4 = byte >> 4;
                    uint8 intensity4 = byte & 0x0F;

                    uint8 *pixel = d + py * w * 4 + px * 4;
                    uint8 i8 = intensity4 | (intensity4 << 4);  // expand 4â? bits
                    pixel[0] = i8;   // R
                    pixel[1] = i8;   // G
                    pixel[2] = i8;   // B
                    pixel[3] = alpha4 | (alpha4 << 4);  // A (expand 4â?)
                }
            }
        }
    }
}

// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
// Downsample: 2Ã2 box filter â?1 pixel
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
static void
downsample2x(const uint8 *src, int sw, int sh,
             uint8 *dst, int dw, int dh)
{
    for(int dy = 0; dy < dh; dy++) {
        for(int dx = 0; dx < dw; dx++) {
            int sx = dx * 2;
            int sy = dy * 2;
            if(sx >= sw) sx = sw - 2;
            if(sy >= sh) sy = sh - 2;

            int a = 0;
            int weightedR = 0, weightedG = 0, weightedB = 0;
            for(int oy = 0; oy < 2; oy++) {
                for(int ox = 0; ox < 2; ox++) {
                    const uint8 *p = src + (sy + oy) * sw * 4 + (sx + ox) * 4;
                    a += p[3];
                    weightedR += (int)p[0] * (int)p[3];
                    weightedG += (int)p[1] * (int)p[3];
                    weightedB += (int)p[2] * (int)p[3];
                }
            }
            uint8 *d = dst + dy * dw * 4 + dx * 4;
            d[3] = (uint8)(a / 4);
            if(a > 0) {
                d[0] = (uint8)((weightedR + a / 2) / a);
                d[1] = (uint8)((weightedG + a / 2) / a);
                d[2] = (uint8)((weightedB + a / 2) / a);
            } else {
                d[0] = 0;
                d[1] = 0;
                d[2] = 0;
            }
        }
    }
}

static void
downsampleRGBA8Rows2x(const uint8 *srcRows, int srcRowBytes,
                      uint8 *dstRow, int dstWidth)
{
    for(int dx = 0; dx < dstWidth; dx++) {
        const uint8 *p00 = srcRows + (dx * 2) * 4;
        const uint8 *p01 = p00 + 4;
        const uint8 *p10 = srcRows + srcRowBytes + (dx * 2) * 4;
        const uint8 *p11 = p10 + 4;
        uint8 *d = dstRow + dx * 4;
        int a = (int)p00[3] + (int)p01[3] + (int)p10[3] + (int)p11[3];
        d[3] = (uint8)(a / 4);
        if(a > 0) {
            d[0] = (uint8)((((int)p00[0] * (int)p00[3]) +
                             ((int)p01[0] * (int)p01[3]) +
                             ((int)p10[0] * (int)p10[3]) +
                             ((int)p11[0] * (int)p11[3]) +
                             a / 2) / a);
            d[1] = (uint8)((((int)p00[1] * (int)p00[3]) +
                             ((int)p01[1] * (int)p01[3]) +
                             ((int)p10[1] * (int)p10[3]) +
                             ((int)p11[1] * (int)p11[3]) +
                             a / 2) / a);
            d[2] = (uint8)((((int)p00[2] * (int)p00[3]) +
                             ((int)p01[2] * (int)p01[3]) +
                             ((int)p10[2] * (int)p10[3]) +
                             ((int)p11[2] * (int)p11[3]) +
                             a / 2) / a);
        } else {
            d[0] = 0;
            d[1] = 0;
            d[2] = 0;
        }
    }
}

static bool
shrinkRGBA8TextureRowStreaming(GxTexPoolEntry *scan, Raster *raster,
                               int oldW, int oldH, int newW, int newH,
                               GxRaster *natras)
{
    uint32 newSize = rgba8TiledSize(newW, newH);
    void *newData = gxMemAlloc(newSize, 32);
    if(!newData) {
        releaseShrinkEmergencyReserve("RGBA8 shrink OOM");
        newData = gxMemAlloc(newSize, 32);
    }
    if(!newData)
        return false;

    const int srcRowBytes = oldW * 4;
    const int dstRowBytes = newW * 4;
    uint8 *srcRows = (uint8*)allocCpuTemp((size_t)srcRowBytes * 2u, 32);
    uint8 *halfLinear = (uint8*)allocCpuTemp((size_t)newW * (size_t)newH * 4u, 32);
    if(!srcRows || !halfLinear) {
        if(srcRows) freeCpuTemp(srcRows);
        if(halfLinear) freeCpuTemp(halfLinear);
        gxMemFree(newData);
        return false;
    }

    void *oldData = scan->gxData;
    uint32 oldSize = scan->size;
    int tilesPerRow = (oldW + 3) / 4;

    for(int dy = 0; dy < newH; dy++) {
        int srcY0 = dy * 2;
        int srcY1 = srcY0 + 1;
        if(srcY0 >= oldH)
            srcY0 = oldH - 1;
        if(srcY1 >= oldH)
            srcY1 = oldH - 1;

        memset(srcRows, 0, (size_t)srcRowBytes * 2u);
        int rowInTile0 = srcY0 & 3;
        int rowInTile1 = srcY1 & 3;
        int tileBase0 = (srcY0 / 4) * tilesPerRow * 64;
        int tileBase1 = (srcY1 / 4) * tilesPerRow * 64;

        for(int bx = 0; bx < oldW; bx += 4) {
            int tileX = bx / 4;
            const uint8 *tile0 = (const uint8*)oldData + tileBase0 + tileX * 64;
            const uint8 *tile1 = (const uint8*)oldData + tileBase1 + tileX * 64;
            uint8 *row0 = srcRows + bx * 4;
            uint8 *row1 = srcRows + srcRowBytes + bx * 4;

            for(int ix = 0; ix < 4; ix++) {
                int px = bx + ix;
                if(px >= oldW)
                    break;
                uint8 *p0 = row0 + ix * 4;
                uint8 *p1 = row1 + ix * 4;

                p0[0] = tile0[rowInTile0 * 8 + ix * 2 + 1];
                p0[1] = tile0[32 + rowInTile0 * 8 + ix * 2 + 0];
                p0[2] = tile0[32 + rowInTile0 * 8 + ix * 2 + 1];
                p0[3] = tile0[rowInTile0 * 8 + ix * 2 + 0];

                p1[0] = tile1[rowInTile1 * 8 + ix * 2 + 1];
                p1[1] = tile1[32 + rowInTile1 * 8 + ix * 2 + 0];
                p1[2] = tile1[32 + rowInTile1 * 8 + ix * 2 + 1];
                p1[3] = tile1[rowInTile1 * 8 + ix * 2 + 0];
            }
        }

        downsampleRGBA8Rows2x(srcRows, srcRowBytes, halfLinear + dy * dstRowBytes, newW);
    }

    freeCpuTemp(srcRows);
    convertRGBA8_to_GX(newData, halfLinear, newW, newH, newW * 4);
    freeCpuTemp(halfLinear);

    gxMemFree(oldData);

    natras->gxData   = newData;
    natras->dataSize = newSize;
    natras->w        = (uint16)newW;
    natras->h        = (uint16)newH;
    natras->gxFmt    = GX_TF_RGBA8;

    raster->width  = (uint16)newW;
    raster->height = (uint16)newH;

    invalidateTextureBinding(raster);
    DCFlushRange(newData, newSize);
    GX_InvalidateTexAll();

    GX_InitTexObj(&natras->texObj, newData,
                  (u16)newW, (u16)newH,
                  GX_TF_RGBA8,
                  natras->wrapS, natras->wrapT,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&natras->texObj,
                            natras->minFilter, natras->magFilter);
    natras->texObjValid = true;

    gTexPoolTotalSize -= oldSize;
    scan->gxData      = newData;
    scan->size        = newSize;
    scan->width       = (uint16)newW;
    scan->height      = (uint16)newH;
    scan->gxFmt       = GX_TF_RGBA8;
    scan->shrinkCount++;

    insertPoolEntrySorted(scan);

    gTexPoolTotalSize += newSize;
    gShrinkTotalCount++;
    logTextureShrink("RGBA8", scan, oldW, oldH, oldSize,
                     newW, newH, newSize);
    return true;
}

static bool
isShrinkableFormat(uint8 fmt)
{
    return fmt == GX_TF_RGBA8 || fmt == GX_TF_CMPR;
}

static char
lowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool
nameContainsNoCase(const char *name, const char *needle)
{
    if(!name || !needle || !needle[0])
        return false;

    for(const char *p = name; *p; p++) {
        const char *a = p;
        const char *b = needle;
        while(*a && *b && lowerAscii(*a) == lowerAscii(*b)) {
            a++;
            b++;
        }
        if(!*b)
            return true;
    }
    return false;
}

static bool
nameStartsWithNoCase(const char *name, const char *prefix)
{
    if(!name || !prefix)
        return false;

    while(*prefix) {
        if(*name == '\0' || lowerAscii(*name) != lowerAscii(*prefix))
            return false;
        name++;
        prefix++;
    }
    return true;
}

static bool
nameEqualsNoCase(const char *a, const char *b)
{
    if(!a || !b)
        return false;

    while(*a && *b) {
        if(lowerAscii(*a) != lowerAscii(*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool
hasWorldTextureNameHint(const char *name)
{
    if(!name || !name[0])
        return false;

    return nameContainsNoCase(name, "wall") ||
           nameContainsNoCase(name, "door") ||
           nameContainsNoCase(name, "roof") ||
           nameContainsNoCase(name, "floor") ||
           nameContainsNoCase(name, "road") ||
           nameContainsNoCase(name, "sign") ||
           nameContainsNoCase(name, "window") ||
           nameContainsNoCase(name, "glass") ||
           nameContainsNoCase(name, "fence") ||
           nameContainsNoCase(name, "shadow") ||
           nameContainsNoCase(name, "light") ||
           nameContainsNoCase(name, "metal") ||
           nameContainsNoCase(name, "tile") ||
           nameContainsNoCase(name, "shop") ||
           nameContainsNoCase(name, "concrete") ||
           nameContainsNoCase(name, "tarmac") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "lod");
}

static bool
hasCriticalUiTextureNameHint(const char *name)
{
    if(!name || !name[0])
        return false;

    return nameContainsNoCase(name, "font") ||
           nameContainsNoCase(name, "fonts") ||
           nameContainsNoCase(name, "hud") ||
           nameContainsNoCase(name, "radar") ||
           nameContainsNoCase(name, "radardisc") ||
           nameContainsNoCase(name, "menu") ||
           nameContainsNoCase(name, "frontend") ||
           nameContainsNoCase(name, "button") ||
           nameContainsNoCase(name, "btn") ||
           nameContainsNoCase(name, "subtitle") ||
           nameContainsNoCase(name, "pager") ||
           nameContainsNoCase(name, "brief") ||
           nameContainsNoCase(name, "viewfinder") ||
           nameContainsNoCase(name, "debugcharset") ||
           nameEqualsNoCase(name, "arrow") ||
           nameEqualsNoCase(name, "fist") ||
           nameStartsWithNoCase(name, "site") ||
           nameContainsNoCase(name, "loadsc") ||
           nameContainsNoCase(name, "intro") ||
           nameContainsNoCase(name, "splash");
}

static bool
hasTransientUiTextureNameHint(const char *name)
{
    if(!name || !name[0])
        return false;

    return nameContainsNoCase(name, "loadsc") ||
           nameContainsNoCase(name, "intro") ||
           nameContainsNoCase(name, "splash");
}

static bool
hasCriticalWaterTextureNameHint(const char *name)
{
    if(!name || !name[0])
        return false;

    return nameContainsNoCase(name, "waterclear") ||
           nameContainsNoCase(name, "waterwake") ||
           nameContainsNoCase(name, "waterreflection") ||
           nameEqualsNoCase(name, "sandywater");
}

static bool
hasCriticalRoadTextureNameHint(const char *name)
{
    if(!name || !name[0])
        return false;

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
hasSensitiveFoliageTextureNameHint(const char *name)
{
    if(!name || !name[0])
        return false;

    return nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "leaves") ||
           nameContainsNoCase(name, "tree") ||
           nameContainsNoCase(name, "bark") ||
           nameContainsNoCase(name, "trunk") ||
           nameContainsNoCase(name, "branch") ||
           nameContainsNoCase(name, "bush") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "ivy") ||
           nameContainsNoCase(name, "plant") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "fern") ||
           nameContainsNoCase(name, "frond") ||
           nameContainsNoCase(name, "weed");
}

static bool
hasDelicateFoliageTextureNameHint(const char *name)
{
    if(!name || !name[0])
        return false;

    return nameContainsNoCase(name, "leaf") ||
           nameContainsNoCase(name, "leaves") ||
           nameContainsNoCase(name, "bush") ||
           nameContainsNoCase(name, "palm") ||
           nameContainsNoCase(name, "ivy") ||
           nameContainsNoCase(name, "plant") ||
           nameContainsNoCase(name, "foliage") ||
           nameContainsNoCase(name, "grass") ||
           nameContainsNoCase(name, "hedge") ||
           nameContainsNoCase(name, "fern") ||
           nameContainsNoCase(name, "frond") ||
           nameContainsNoCase(name, "weed");
}

static int
getFoliageShrinkFloorDim(const GxTexPoolEntry *entry)
{
    if(!entry || !hasSensitiveFoliageTextureNameHint(entry->name))
        return 0;

    if(nameEqualsNoCase(entry->name, "kbtree4_test") ||
       nameEqualsNoCase(entry->name, "newtreeleaves128") ||
       nameEqualsNoCase(entry->name, "newtreeleavesb128") ||
       nameEqualsNoCase(entry->name, "foliage256") ||
       nameEqualsNoCase(entry->name, "weepalmshadow") ||
       nameEqualsNoCase(entry->name, "bigpalmshadow"))
        return 256;

    return hasDelicateFoliageTextureNameHint(entry->name) ? 256 : 128;
}

static bool
shouldLogTextureShrink(const GxTexPoolEntry *entry)
{
    if(!entry)
        return false;

    if(entry->shrinkCount == 0)
        return true;

    if(entry->name[0] == '\0')
        return entry->width >= 128 || entry->height >= 128;

    return hasCriticalUiTextureNameHint(entry->name) ||
           hasCriticalRoadTextureNameHint(entry->name) ||
           hasSensitiveFoliageTextureNameHint(entry->name) ||
           nameContainsNoCase(entry->name, "map") ||
           nameContainsNoCase(entry->name, "intro") ||
           nameContainsNoCase(entry->name, "hud") ||
           nameContainsNoCase(entry->name, "radar") ||
           nameContainsNoCase(entry->name, "prop") ||
           nameContainsNoCase(entry->name, "heli") ||
           nameContainsNoCase(entry->name, "door") ||
           nameContainsNoCase(entry->name, "floor") ||
           nameContainsNoCase(entry->name, "wall");
}

static void
logTextureShrink(const char *kind, const GxTexPoolEntry *entry,
                 int oldW, int oldH, uint32 oldSize,
                 int newW, int newH, uint32 newSize)
{
    static int s_texShrinkLogCount = 0;
    if(s_texShrinkLogCount >= 160 && !shouldLogTextureShrink(entry))
        return;

    printf("[GX-TEXSHRINK] %s #%d name=%s %dx%d->%dx%d %u->%u fmt=%u count=%d pool=%uKB tex=%d\n",
           kind,
           gShrinkTotalCount,
           entry && entry->name[0] ? entry->name : "<unknown>",
           oldW, oldH, newW, newH,
           oldSize, newSize,
           entry ? (unsigned)entry->gxFmt : 0u,
           entry ? entry->shrinkCount : 0,
           gTexPoolTotalSize / 1024,
           gTexPoolCount);
    s_texShrinkLogCount++;
}

static bool
isLikelyPedModelCodeName(const char *name)
{
    if(!name || !name[0] || hasWorldTextureNameHint(name))
        return false;

    size_t len = strlen(name);
    if(len != 5)
        return false;

    for(size_t i = 0; i < len; i++) {
        char c = name[i];
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            continue;
        return false;
    }
    return true;
}

static bool
isLikelyPedTexture(const GxTexPoolEntry *entry)
{
    if(!entry || !entry->name[0])
        return false;

    size_t len = strlen(entry->name);
    if(len < 4 || len > 6)
        return false;

    if(entry->width < 64 || entry->height < 64 ||
       entry->width > 256 || entry->height > 256)
        return false;

    bool shapeOK = (entry->width == entry->height) ||
                   (entry->width * 2 == entry->height) ||
                   (entry->height * 2 == entry->width);
    if(!shapeOK)
        return false;

    if(nameStartsWithNoCase(entry->name, "cs"))
        return true;

    if(nameEqualsNoCase(entry->name, "ken") ||
       nameEqualsNoCase(entry->name, "sonny") ||
       nameEqualsNoCase(entry->name, "cabbie") ||
       nameEqualsNoCase(entry->name, "cop") ||
       nameEqualsNoCase(entry->name, "player") ||
       nameContainsNoCase(entry->name, "male") ||
       nameContainsNoCase(entry->name, "woman") ||
       nameContainsNoCase(entry->name, "female") ||
       nameContainsNoCase(entry->name, "goon") ||
       nameContainsNoCase(entry->name, "security") ||
       nameContainsNoCase(entry->name, "guard"))
        return true;

    if(isLikelyPedModelCodeName(entry->name))
        return true;

    int upperCount = 0;
    for(size_t i = 0; i < len; i++) {
        char c = entry->name[i];
        if(c >= 'A' && c <= 'Z') {
            upperCount++;
            continue;
        }
        if(c >= '0' && c <= '9')
            continue;
        return false;
    }

    return upperCount >= 3;
}

static bool
isUnnamedBudgetProtectedTexture(const GxTexPoolEntry *entry)
{
    if(!entry || entry->name[0] != '\0')
        return false;

    // UI helper rasters are often registered before a texture name exists, but
    // broad-protecting every anonymous 256/512 RGBA8 world texture starves the
    // pause menu. Only protect the small helpers unless a critical UI load is
    // currently in progress.
#ifdef WII
    if(entry->gxFmt == GX_TF_RGBA8 &&
       gCriticalUiUploadDepth > 0 &&
       entry->width <= 512 &&
       entry->height <= 512 &&
       entry->size <= 1024u * 1024u)
        return true;
#endif

    // Keep tiny anonymous helper rasters stable on all GX targets.
    return entry->width <= 128 &&
           entry->height <= 128 &&
           entry->size <= 64u * 1024u;
}

static bool
isBudgetProtectedTexture(const GxTexPoolEntry *entry)
{
    if(!entry)
        return true;

    // Many fallback/Image-created rasters are registered before a Texture name
    // is attached. Keep known helper/UI-sized rasters stable; larger anonymous
    // world captures can still be shrunk.
    if(entry->name[0] == '\0')
        return isUnnamedBudgetProtectedTexture(entry);

    // Wii used to blanket-protect every 128x128+ CMPR world texture here.
    // In practice that starves safeGxAlloc(): when the pool gets tight it can
    // only chew through tiny helper rasters, so even a 64KB world upload keeps
    // failing after dozens of shrink attempts. Keep protection name-driven so
    // we still preserve HUD/radar/roads/glass, but allow ordinary building and
    // scenery CMPR textures to participate in emergency shrink.

    bool transientUi = hasTransientUiTextureNameHint(entry->name);
    bool persistentUi = hasCriticalUiTextureNameHint(entry->name) && !transientUi;

    return persistentUi ||
           (gCriticalUiUploadDepth > 0 && transientUi) ||
           hasCriticalWaterTextureNameHint(entry->name) ||
           hasCriticalRoadTextureNameHint(entry->name) ||
           nameContainsNoCase(entry->name, "thumb") ||
           strcmp(entry->name, "cross") == 0 ||
           strcmp(entry->name, "circle") == 0 ||
           strcmp(entry->name, "square") == 0 ||
           strcmp(entry->name, "triangle") == 0 ||
           strcmp(entry->name, "player") == 0 ||
           strcmp(entry->name, "cop") == 0 ||
           isLikelyPedTexture(entry);
}

static bool
allowLargeMemFallbackForTag(const char *tag)
{
    if(gCriticalUiUploadDepth > 0) {
        if(tag == nullptr || tag[0] == '\0')
            return false;

        // During intro/loadscreen critical sections, a missing scene texture is
        // worse than letting a few larger rasters spill into plain MEM1.
        return true;
    }

    if(tag == nullptr || tag[0] == '\0')
        return false;

    return hasCriticalUiTextureNameHint(tag) ||
           hasCriticalWaterTextureNameHint(tag) ||
           hasCriticalRoadTextureNameHint(tag);
}

static uint32
cmprTiledSizePadded(int w, int h)
{
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    int mbw = (bw + 1) / 2;
    int mbh = (bh + 1) / 2;
    return (uint32)(mbw * mbh * 32);
}

static const uint8*
cmprBlockPtr(const uint8 *data, int w, int blockX, int blockY)
{
    int bw = (w + 3) / 4;
    int mbw = (bw + 1) / 2;
    int mbx = blockX / 2;
    int mby = blockY / 2;
    int local = (blockY & 1) * 2 + (blockX & 1);
    return data + ((mby * mbw + mbx) * 4 + local) * 8;
}

static void
shrinkCMPRByBlockDrop(uint8 *dst, const uint8 *src,
                      int oldW, int newW, int newH)
{
    int newBW = (newW + 3) / 4;
    int newBH = (newH + 3) / 4;
    int newMBW = (newBW + 1) / 2;
    int newMBH = (newBH + 1) / 2;

    for(int mby = 0; mby < newMBH; mby++) {
        for(int mbx = 0; mbx < newMBW; mbx++) {
            for(int by = 0; by < 2; by++) {
                for(int bx = 0; bx < 2; bx++) {
                    int dstBlockX = mbx * 2 + bx;
                    int dstBlockY = mby * 2 + by;
                    if(dstBlockX < newBW && dstBlockY < newBH) {
                        const uint8 *block = cmprBlockPtr(src, oldW,
                                                          dstBlockX * 2,
                                                          dstBlockY * 2);
                        memcpy(dst, block, 8);
                    } else {
                        memset(dst, 0, 8);
                    }
                    dst += 8;
                }
            }
        }
    }
}

static void
ensureShrinkEmergencyReserve(void)
{
    if(gShrinkEmergencyReserve == nullptr && gShrinkEmergencyReserveSize > 0) {
        gShrinkEmergencyReserve = arena2Alloc(gShrinkEmergencyReserveSize, 32);
        if(gShrinkEmergencyReserve)
            printf("[GX-TEXP] reserved shrink emergency block (%u bytes)\n",
                   (unsigned)gShrinkEmergencyReserveSize);
    }
}

static void
releaseShrinkEmergencyReserve(const char *reason)
{
    if(gShrinkEmergencyReserve) {
        arena2Free(gShrinkEmergencyReserve);
        gShrinkEmergencyReserve = nullptr;
        printf("[GX-TEXP] released shrink emergency block (%u bytes) due to %s\n",
               (unsigned)gShrinkEmergencyReserveSize,
               reason ? reason : "<unknown>");
    }
}

static void
tightenBudgetForSafeAlloc(size_t size)
{
    if(gCriticalUiUploadDepth > 0) {
        uint32 oldBudget = gSoftBudgetBytes;
        uint32 newBudget = oldBudget;
#ifdef WII
        uint32 targetBudget = getPoolAwareBudgetWithHeadroom(
            size >= 256u * 1024u ?
                7u * 1024u * 1024u :
                6u * 1024u * 1024u);
#else
        uint32 targetBudget = size >= 256u * 1024u ?
            18u * 1024u * 1024u :
            19u * 1024u * 1024u;
#endif

        if(newBudget > targetBudget)
            newBudget = targetBudget;

        if(newBudget != oldBudget) {
#ifndef WII
            printf("[GX-TEXP] critical UI budget tighten for %u bytes: %uKB -> %uKB (pool=%uKB tex=%d)\n",
                   (unsigned)size,
                   oldBudget / 1024,
                   newBudget / 1024,
                   gTexPoolTotalSize / 1024,
                   gTexPoolCount);
#endif
            gSoftBudgetBytes = newBudget;
        }

        texPoolEnforceBudget("critical-ui-upload");
        return;
    }

    uint32 oldBudget = gSoftBudgetBytes;
#ifdef WII
    uint32 floorBytes = getPoolAwareBudgetWithHeadroom(
        size >= 256u * 1024u ?
            6u * 1024u * 1024u :
            5u * 1024u * 1024u);
#else
    uint32 floorBytes = size >= 256u * 1024u ?
        24u * 1024u * 1024u :
        25u * 1024u * 1024u;
#endif
    uint32 newBudget = oldBudget;

    if(newBudget > floorBytes)
        newBudget = floorBytes;

    if(newBudget != oldBudget) {
#ifndef WII
        printf("[GX-TEXP] safe alloc budget tighten for %u bytes: %uKB -> %uKB (pool=%uKB tex=%d)\n",
               (unsigned)size,
               oldBudget / 1024,
               newBudget / 1024,
               gTexPoolTotalSize / 1024,
               gTexPoolCount);
#endif
        gSoftBudgetBytes = newBudget;
    }

    texPoolEnforceBudget("safeGxAlloc");
}

static void
insertPoolEntrySorted(GxTexPoolEntry *entry)
{
    GxTexPoolEntry **prev = &gTexPool;
    GxTexPoolEntry *cur  = gTexPool;

    while(cur && cur->size > entry->size) {
        prev = &cur->next;
        cur  = cur->next;
    }
    entry->next = cur;
    *prev       = entry;
}

// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
// Shrink largest texture â?halves resolution
// Returns true if something was shrunk
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ

// Shrink largest texture â?halves resolution
// Returns true if something was shrunk
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ

bool
shrinkLargestTexture(void)
{
    if(!gTexPool) return false;

    ensureShrinkEmergencyReserve();

    bool allowProtected = false;
    bool preferHostMemory = gPreferHostMemoryShrink;
    bool disableRGBA8ShrinkForPass = false;
retry_scan:
    // New path: scan forward until we find a candidate that can actually
    // allocate the temporary buffers needed for shrinking. This avoids
    // aborting the whole shrink pass just because the single largest texture
    // itself can't be processed under low memory.
    {
        int scanSteps = 0;
        GxTexPoolEntry **scanPrev = &gTexPool;
        GxTexPoolEntry *scan = gTexPool;
        while(scan) {
            if(++scanSteps > MAX_TEXPOOL_SCAN_STEPS) {
                printf("[GX-TEXP] shrink scan runaway pass=%s%s rgba8=%d head=%p pool=%uKB tex=%d steps=%d\n",
                       preferHostMemory ? "hostmem" : "normal",
                       allowProtected ? "+protected" : "",
                       disableRGBA8ShrinkForPass ? 0 : 1,
                       (void*)gTexPool,
                       gTexPoolTotalSize / 1024,
                       gTexPoolCount,
                       scanSteps);
                return false;
            }
            if(scan->next == scan) {
                printf("[GX-TEXP] shrink list self-loop entry=%p name=%s size=%u\n",
                       (void*)scan,
                       scan->name[0] ? scan->name : "<unknown>",
                       (unsigned)scan->size);
                return false;
            }

            if(preferHostMemory && scan->gxData != nullptr &&
               gxMemOwns(scan->gxData)) {
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }

            if(!allowProtected && isBudgetProtectedTexture(scan)) {
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }

            if(!isShrinkableFormat(scan->gxFmt)) {
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }

            if(disableRGBA8ShrinkForPass && scan->gxFmt == GX_TF_RGBA8) {
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }

            Raster *raster = scan->raster;
            if(!raster) {
                GxTexPoolEntry *dead = scan;
                *scanPrev = scan->next;
                scan = scan->next;
                gTexPoolCount--;
                gTexPoolTotalSize -= dead->size;
                free(dead);
                continue;
            }

            int oldW = scan->width;
            int oldH = scan->height;
            int newW = oldW / 2;
            int newH = oldH / 2;
            int foliageShrinkFloor = getFoliageShrinkFloorDim(scan);
            int oldMaxDim = oldW > oldH ? oldW : oldH;

            if(newW < MIN_TEX_DIM) newW = MIN_TEX_DIM;
            if(newH < MIN_TEX_DIM) newH = MIN_TEX_DIM;
            if(newW == oldW && newH == oldH) {
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }

            if((scan->gxFmt == GX_TF_RGBA8 || scan->gxFmt == GX_TF_CMPR) &&
               foliageShrinkFloor > 0 &&
               oldMaxDim <= foliageShrinkFloor) {
                static int s_foliageShrinkSkipLogCount = 0;
                if(s_foliageShrinkSkipLogCount < 48) {
                    printf("[GX-TEXP] preserve foliage %s %s %dx%d -> %dx%d skip floor=%d\n",
                           scan->gxFmt == GX_TF_CMPR ? "cmpr" : "rgba8",
                           scan->name[0] ? scan->name : "<unknown>",
                           oldW, oldH, newW, newH,
                           foliageShrinkFloor);
                    s_foliageShrinkSkipLogCount++;
                }
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }

            if(scan->gxFmt == GX_TF_CMPR) {
                if(newW < 8) newW = 8;
                if(newH < 8) newH = 8;
                if(newW == oldW && newH == oldH) {
                    scanPrev = &scan->next;
                    scan = scan->next;
                    continue;
                }

                uint32 newSize = cmprTiledSizePadded(newW, newH);
                void *newData = gxMemAlloc(newSize, 32);
                if(!newData) {
                    releaseShrinkEmergencyReserve("CMPR shrink OOM");
                    newData = gxMemAlloc(newSize, 32);
                }
                if(!newData) {
                    static int s_skipCmprAllocOomLogCount = 0;
                    if(s_skipCmprAllocOomLogCount < 24) {
                        printf("[GX-TEXP] skip CMPR %dx%d -> %dx%d size=%u: alloc OOM\n",
                               oldW, oldH, newW, newH, newSize);
                        s_skipCmprAllocOomLogCount++;
                    }
                    scanPrev = &scan->next;
                    scan = scan->next;
                    continue;
                }

                GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
                void *oldData = scan->gxData;
                uint32 oldSize = scan->size;

                shrinkCMPRByBlockDrop((uint8*)newData, (const uint8*)oldData,
                                      oldW, newW, newH);

                *scanPrev = scan->next;
                gxMemFree(oldData);

                natras->gxData   = newData;
                natras->dataSize = newSize;
                natras->w        = (uint16)newW;
                natras->h        = (uint16)newH;
                natras->gxFmt    = GX_TF_CMPR;

                raster->width  = (uint16)newW;
                raster->height = (uint16)newH;

                invalidateTextureBinding(raster);
                DCFlushRange(newData, newSize);
                GX_InvalidateTexAll();

                GX_InitTexObj(&natras->texObj, newData,
                              (u16)newW, (u16)newH,
                              GX_TF_CMPR,
                              natras->wrapS, natras->wrapT,
                              GX_FALSE);
                GX_InitTexObjFilterMode(&natras->texObj,
                                        natras->minFilter, natras->magFilter);
                natras->texObjValid = true;

                gTexPoolTotalSize -= oldSize;
                scan->gxData      = newData;
                scan->size        = newSize;
                scan->width       = (uint16)newW;
                scan->height      = (uint16)newH;
                scan->gxFmt       = GX_TF_CMPR;
                scan->shrinkCount++;

                insertPoolEntrySorted(scan);

                gTexPoolTotalSize += newSize;
                gShrinkTotalCount++;
                logTextureShrink("CMPR", scan, oldW, oldH, oldSize,
                                 newW, newH, newSize);

                if((gShrinkTotalCount & 63) == 0) {
#ifndef WII
                    printf("[GX-TEXP] shrink CMPR #%d latest=%s pool=%uKB/%d tex\n",
                           gShrinkTotalCount,
                           scan->name[0] ? scan->name : "<unknown>",
                           gTexPoolTotalSize / 1024,
                           gTexPoolCount);
#endif
                }
                return true;
            }

            uint8 *linear = nil;
            bool tryStreamShrink = scan->gxFmt == GX_TF_RGBA8 &&
                                   oldW >= 128 && oldH >= 128;
            if(!tryStreamShrink)
                linear = (uint8*)allocCpuTemp((size_t)oldW * oldH * 4, 32);
            if(!linear && !tryStreamShrink) {
                static int s_skipLinearCount = 0;
                if(s_skipLinearCount < 16) {
                    printf("[GX-TEXP] skip candidate %dx%d fmt=%u size=%u: linear temp OOM\n",
                           oldW, oldH, (unsigned)scan->gxFmt, scan->size);
                    s_skipLinearCount++;
                }
                if(!disableRGBA8ShrinkForPass) {
                    disableRGBA8ShrinkForPass = true;
                    printf("[GX-TEXP] disabling RGBA8 shrink for this pass after temp OOM (pool=%uKB tex=%d)\n",
                           gTexPoolTotalSize / 1024,
                           gTexPoolCount);
                }
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }

            uint8 *halfLinear = linear ? (uint8*)allocCpuTemp((size_t)newW * newH * 4, 32) : nil;
            if(linear && !halfLinear) {
                static int s_skipHalfCount = 0;
                if(s_skipHalfCount < 16) {
                    printf("[GX-TEXP] skip candidate %dx%d fmt=%u size=%u: half temp OOM\n",
                           oldW, oldH, (unsigned)scan->gxFmt, scan->size);
                    s_skipHalfCount++;
                }
                if(!disableRGBA8ShrinkForPass) {
                    disableRGBA8ShrinkForPass = true;
                    printf("[GX-TEXP] disabling RGBA8 shrink for this pass after half-temp OOM (pool=%uKB tex=%d)\n",
                           gTexPoolTotalSize / 1024,
                           gTexPoolCount);
                }
                freeCpuTemp(linear);
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }

            GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
            void *oldData = scan->gxData;
            uint32 oldSize = scan->size;

            if(!linear && tryStreamShrink) {
                *scanPrev = scan->next;
                bool ok = shrinkRGBA8TextureRowStreaming(scan, raster, oldW, oldH, newW, newH, natras);
                if(!ok) {
                    scan->next = *scanPrev;
                    insertPoolEntrySorted(scan);
                    static int s_streamShrinkFailCount = 0;
                    if(s_streamShrinkFailCount < 24) {
                        printf("[GX-TEXP] stream shrink failed %dx%d -> %dx%d size=%u\n",
                               oldW, oldH, newW, newH, scan->size);
                        s_streamShrinkFailCount++;
                    }
                    if(!disableRGBA8ShrinkForPass) {
                        disableRGBA8ShrinkForPass = true;
                        printf("[GX-TEXP] disabling RGBA8 shrink for this pass after stream shrink failure (pool=%uKB tex=%d)\n",
                               gTexPoolTotalSize / 1024,
                               gTexPoolCount);
                    }
                    scanPrev = &scan->next;
                    scan = scan->next;
                    continue;
                }
                return true;
            }

            convertGX_RGBA8_toLinear(linear, oldData, oldW, oldH);
            downsample2x(linear, oldW, oldH, halfLinear, newW, newH);
            freeCpuTemp(linear);

            uint32 newSize = rgba8TiledSize(newW, newH);
            void *newData = gxMemAlloc(newSize, 32);
            if(!newData) {
                releaseShrinkEmergencyReserve("RGBA8 shrink OOM");
                newData = gxMemAlloc(newSize, 32);
            }
            if(!newData) {
                freeCpuTemp(halfLinear);
                printf("[GX-TEXP] shrink candidate %dx%d -> %dx%d failed: re-tile alloc OOM\n",
                       oldW, oldH, newW, newH);
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }

            *scanPrev = scan->next;
            gxMemFree(oldData);

            convertRGBA8_to_GX(newData, halfLinear, newW, newH, newW * 4);
            freeCpuTemp(halfLinear);

            natras->gxData   = newData;
            natras->dataSize = newSize;
            natras->w        = (uint16)newW;
            natras->h        = (uint16)newH;
            natras->gxFmt    = GX_TF_RGBA8;

            raster->width  = (uint16)newW;
            raster->height = (uint16)newH;

            invalidateTextureBinding(raster);
            DCFlushRange(newData, newSize);
            GX_InvalidateTexAll();

            GX_InitTexObj(&natras->texObj, newData,
                          (u16)newW, (u16)newH,
                          GX_TF_RGBA8,
                          natras->wrapS, natras->wrapT,
                          GX_FALSE);
            GX_InitTexObjFilterMode(&natras->texObj,
                                    natras->minFilter, natras->magFilter);
            natras->texObjValid = true;

            gTexPoolTotalSize -= oldSize;
            scan->gxData      = newData;
            scan->size        = newSize;
            scan->width       = (uint16)newW;
            scan->height      = (uint16)newH;
            scan->gxFmt       = GX_TF_RGBA8;
            scan->shrinkCount++;

            insertPoolEntrySorted(scan);

            gTexPoolTotalSize += newSize;
            gShrinkTotalCount++;
            logTextureShrink("RGBA8", scan, oldW, oldH, oldSize,
                             newW, newH, newSize);

            if((gShrinkTotalCount & 31) == 0) {
#ifndef WII
                printf("[GX-TEXP] shrink RGBA8 #%d latest=%s pool=%uKB/%d tex\n",
                       gShrinkTotalCount,
                       scan->name[0] ? scan->name : "<unknown>",
                       gTexPoolTotalSize / 1024,
                       gTexPoolCount);
#endif
            }
            return true;
        }
    }

    if(preferHostMemory) {
        preferHostMemory = false;
        goto retry_scan;
    }

    if(!allowProtected && !gBudgetShrinkPass) {
        allowProtected = true;
        goto retry_scan;
    }

    return false;
}

bool
shrinkLargestTexturePreferHostMemory(void)
{
    bool oldPreferHost = gPreferHostMemoryShrink;
    gPreferHostMemoryShrink = true;
    bool shrunk = shrinkLargestTexture();
    gPreferHostMemoryShrink = oldPreferHost;
    return shrunk;
}

void
pushCriticalUiUploadContext(const char *reason)
{
    gCriticalUiUploadDepth++;
    if(gCriticalUiUploadDepth <= 4) {
        printf("[GX-TEXP] critical UI upload PUSH depth=%d reason=%s pool=%uKB tex=%d budget=%uKB\n",
               gCriticalUiUploadDepth,
               reason ? reason : "<none>",
               gTexPoolTotalSize / 1024,
               gTexPoolCount,
               gSoftBudgetBytes / 1024);
    }
}

void
popCriticalUiUploadContext(const char *reason)
{
    if(gCriticalUiUploadDepth <= 0) {
        printf("[GX-TEXP] critical UI upload POP underflow reason=%s\n",
               reason ? reason : "<none>");
        gCriticalUiUploadDepth = 0;
        return;
    }

    gCriticalUiUploadDepth--;
    if(gCriticalUiUploadDepth <= 3) {
        printf("[GX-TEXP] critical UI upload POP depth=%d reason=%s pool=%uKB tex=%d budget=%uKB\n",
               gCriticalUiUploadDepth,
               reason ? reason : "<none>",
               gTexPoolTotalSize / 1024,
               gTexPoolCount,
               gSoftBudgetBytes / 1024);
    }
}

bool
isCriticalUiUploadContextActive(void)
{
    return gCriticalUiUploadDepth > 0;
}

#if 0

    // Pop the largest texture we can actually shrink.
    GxTexPoolEntry **entryPrev = &gTexPool;
    GxTexPoolEntry *entry = gTexPool;
    while(entry && !isShrinkableFormat(entry->gxFmt)) {
        entryPrev = &entry->next;
        entry = entry->next;
    }
    if(!entry)
        return false;
    *entryPrev = entry->next;

    Raster *raster = entry->raster;
    if(!raster) {
        // Raster was destroyed but entry not cleaned up (shouldn't happen)
        free(entry);
        return false;
    }

    int oldW = entry->width;
    int oldH = entry->height;
    int newW = oldW / 2;
    int newH = oldH / 2;

    if(newW < MIN_TEX_DIM) newW = MIN_TEX_DIM;
    if(newH < MIN_TEX_DIM) newH = MIN_TEX_DIM;

    if(newW == oldW && newH == oldH) {
        // Can't shrink further â?put back and return false
        insertPoolEntrySorted(entry);
        return false;
    }

    GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
    void *oldData = entry->gxData;
    uint32 oldSize = entry->size;
    uint8 oldFmt = entry->gxFmt;

    // Step 1: Untile gxData â?linear RGBA8
    uint8 *linear = (uint8*)gxMemAlloc((size_t)oldW * oldH * 4, 32);
    if(!linear) {
        // Can't even allocate temp buffer â?put back, give up
        insertPoolEntrySorted(entry);
        return false;
    }

    if(oldFmt == GX_TF_RGBA8) {
        convertGX_RGBA8_toLinear(linear, oldData, oldW, oldH);
    } else if(oldFmt == GX_TF_IA4) {
        convertGX_IA4_toLinear(linear, oldData, oldW, oldH);
    } else {
        // Unknown format â?skip
        gxMemFree(linear);
        insertPoolEntrySorted(entry);
        return false;
    }

    // Step 2: Downsample
    uint8 *halfLinear = (uint8*)gxMemAlloc((size_t)newW * newH * 4, 32);
    if(!halfLinear) {
        gxMemFree(linear);
        insertPoolEntrySorted(entry);
        return false;
    }

    downsample2x(linear, oldW, oldH, halfLinear, newW, newH);
    gxMemFree(linear);

    // Step 3: Allocate replacement before touching the live texture.
    uint32 newSize = rgba8TiledSize(newW, newH);
    void *newData = gxMemAlloc(newSize, 32);
    if(!newData) {
        gxMemFree(halfLinear);
        insertPoolEntrySorted(entry);
        return false;
    }

    // Step 4: Replace old gxData only after the new allocation succeeded.
    gxMemFree(oldData);

    convertRGBA8_to_GX(newData, halfLinear, newW, newH, newW * 4);
    gxMemFree(halfLinear);

    // Step 5: Update GxRaster
    natras->gxData   = newData;
    natras->dataSize = newSize;
    natras->w        = (uint16)newW;
    natras->h        = (uint16)newH;
    natras->gxFmt    = GX_TF_RGBA8;  // always RGBA8 after re-tiling

    // Update raster dimensions (the public fields)
    raster->width  = (uint16)newW;
    raster->height = (uint16)newH;

    invalidateTextureBinding(raster);
    DCFlushRange(newData, newSize);
    GX_InvalidateTexAll();

    GX_InitTexObj(&natras->texObj, newData,
                  (u16)newW, (u16)newH,
                  GX_TF_RGBA8,
                  GX_CLAMP, GX_CLAMP,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&natras->texObj, GX_LINEAR, GX_LINEAR);
    natras->texObjValid = true;

    // Step 6: Update pool entry and re-insert
    gTexPoolTotalSize -= oldSize;
    entry->gxData      = newData;
    entry->size        = newSize;
    entry->width       = (uint16)newW;
    entry->height      = (uint16)newH;
    entry->gxFmt       = GX_TF_RGBA8;
    entry->shrinkCount++;

    // Re-insert sorted by size
    insertPoolEntrySorted(entry);

    gTexPoolTotalSize += newSize;
    gShrinkTotalCount++;

    printf("[GX-TEXP] shrink #%d: %dx%dâ?dx%d %uâ?u bytes (freed %u, pool=%uKB/%d tex)\n",
           gShrinkTotalCount,
           oldW, oldH, newW, newH,
           oldSize, newSize,
           oldSize - newSize,
           gTexPoolTotalSize / 1024,
           gTexPoolCount);

    return true;
}
#endif

uint32
shrinkSomeTexture(void)
{
    if(shrinkLargestTexture()) {
        // Return the bytes freed by the last shrink
        // (we don't track per-shrink, return a non-zero sentinel)
        return 1;
    }
    return 0;
}

void
texPoolEnforceBudget(const char *reason)
{
    uint32 budgetLimit = gSoftBudgetBytes + gSoftBudgetSlackBytes;
    if(gTexPoolTotalSize <= budgetLimit)
        return;

    static uint32 s_lastReportKB = 0;
    uint32 startKB = gTexPoolTotalSize / 1024;
    if(startKB != s_lastReportKB) {
#ifndef WII
        printf("[GX-TEXP] budget pressure%s%s: pool=%uKB target=%uKB tex=%d\n",
               reason ? " after " : "",
               reason ? reason : "",
               startKB, gSoftBudgetBytes / 1024, gTexPoolCount);
#endif
        s_lastReportKB = startKB;
    }

    for(int i = 0; i < GX_TEX_BUDGET_STEPS &&
                   gTexPoolTotalSize > gSoftBudgetBytes; i++) {
        gBudgetShrinkPass = true;
        bool shrunk = shrinkLargestTexture();
        gBudgetShrinkPass = false;
        if(!shrunk) {
            printf("[GX-TEXP] budget: no more shrinkable textures, pool=%uKB target=%uKB\n",
                   gTexPoolTotalSize / 1024, gSoftBudgetBytes / 1024);
            texPoolDebug();
            break;
        }
    }
}

// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
// safeGxAlloc â?memalign with auto-shrink fallback
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
void*
gxMemAlloc(size_t size, size_t alignment)
{
    void *ptr = arena2Alloc(size, alignment);
    if(ptr)
        return ptr;

    ptr = allocFallbackHostMemory(size, alignment);
    if(ptr && gArena2Ready) {
        static int s_fallbackLogCount = 0;
        if(s_fallbackLogCount < 32) {
            gArena2FallbackCount++;
#ifndef WII
            printf("[GX-POOL] fallback to plain MEM1 size=%u align=%u count=%u\n",
                   (unsigned)size, (unsigned)alignment,
                   (unsigned)gArena2FallbackCount);
#endif
            s_fallbackLogCount++;
        }
    }
    return ptr;
}

void
gxMemFree(void *ptr)
{
    if(ptr == nullptr)
        return;
    if(gxMemOwns(ptr)) {
        arena2Free(ptr);
        return;
    }
#ifdef WII
    MemoryMgrFreeMem2(ptr);
#else
    free(ptr);
#endif
}

void*
safeGxAlloc(size_t size, size_t alignment, const char *tag)
{
    void *ptr = arena2Alloc(size, alignment);
    if(ptr)
        return ptr;

#ifdef WII
    bool allowEarlyHostFallback = size <= 256u * 1024u ||
                                  allowLargeMemFallbackForTag(tag);
    if(allowEarlyHostFallback) {
        ptr = allocFallbackHostMemoryStrict(size, alignment);
        if(ptr) {
            static int s_earlyHostFallbackLogCount = 0;
            gArena2FallbackCount++;
            if(s_earlyHostFallbackLogCount < 64) {
                printf("[GX-TEXP] safeGxAlloc(%u) early MEM2 fallback SUCCESS after GX pool direct FAIL (count=%u tag='%s')\n",
                       (unsigned)size,
                       (unsigned)gArena2FallbackCount,
                       tag ? tag : "<none>");
                s_earlyHostFallbackLogCount++;
            }
            return ptr;
        }
    }
#endif

    printf("[GX-TEXP] safeGxAlloc(%u) GX pool direct FAIL - triggering texture shrink (tag='%s')\n",
           (unsigned)size, tag ? tag : "<none>");

    tightenBudgetForSafeAlloc(size);

    ptr = arena2Alloc(size, alignment);
    if(ptr) {
#ifndef WII
        printf("[GX-TEXP] safeGxAlloc(%u) SUCCESS after budget tighten, pool=%uKB/%d tex tag='%s'\n",
               (unsigned)size,
               gTexPoolTotalSize / 1024, gTexPoolCount,
               tag ? tag : "<none>");
#endif
        return ptr;
    }

    for(int attempt = 1; attempt <= MAX_SHRINK_ATTEMPTS; attempt++) {
        if(attempt == 1 || attempt == 2 || attempt == 4 || attempt == MAX_SHRINK_ATTEMPTS) {
            printf("[GX-TEXP] safeGxAlloc attempt=%d/%d tag='%s' pool=%uKB tex=%d fallback=%u\n",
                   attempt, MAX_SHRINK_ATTEMPTS,
                   tag ? tag : "<none>",
                   gTexPoolTotalSize / 1024,
                   gTexPoolCount,
                   (unsigned)gArena2FallbackCount);
        }
        if(!shrinkLargestTexture()) {
            printf("[GX-TEXP] safeGxAlloc: no more textures to shrink (attempt %d)\n",
                   attempt);
            break;
        }

        ptr = arena2Alloc(size, alignment);
        if(ptr) {
            printf("[GX-TEXP] safeGxAlloc(%u) SUCCESS after %d shrink(s), pool=%uKB/%d tex tag='%s'\n",
                   (unsigned)size, attempt,
                   gTexPoolTotalSize / 1024, gTexPoolCount,
                   tag ? tag : "<none>");
            return ptr;
        }
    }

    // Large texture uploads falling back into plain MEM1 can starve later
    // rwMalloc/rwRealloc calls. Prefer a visible texture downgrade over
    // letting gameplay/cutscene allocations die after the texture load.
    bool allowLargeFallback = allowLargeMemFallbackForTag(tag);
    if(size <= 128u * 1024u || allowLargeFallback) {
        ptr = allocFallbackHostMemory(size, alignment);
        if(ptr) {
            gArena2FallbackCount++;
            printf("[GX-TEXP] safeGxAlloc(%u) host fallback SUCCESS after GX pool exhaustion (count=%u tag='%s')\n",
                   (unsigned)size, (unsigned)gArena2FallbackCount,
                   tag ? tag : "<none>");
            return ptr;
        }
    } else {
        printf("[GX-TEXP] safeGxAlloc(%u) refusing large MEM1 fallback to protect rw heap (tag='%s')\n",
               (unsigned)size, tag ? tag : "<none>");
    }

    printf("[GX-TEXP] safeGxAlloc(%u) FAILED after %d attempts - truly OOM (tag='%s')\n",
           (unsigned)size, MAX_SHRINK_ATTEMPTS, tag ? tag : "<none>");
    texPoolDebug();
    return nullptr;
}

// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
// Diagnostics
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ

void
texPoolDebug(void)
{
    printf("[GX-TEXP] Pool: %d textures, %u KB total, %d lifetime shrinks\n",
           gTexPoolCount, gTexPoolTotalSize / 1024, gShrinkTotalCount);

    int i = 0;
    GxTexPoolEntry *cur = gTexPool;
    while(cur && i < 20) {
        printf("  #%d: %dx%d fmt=%d size=%u shrinks=%d raster=%p name='%s'\n",
               i, cur->width, cur->height, cur->gxFmt,
               cur->size, cur->shrinkCount, (void*)cur->raster,
               cur->name[0] ? cur->name : "<unknown>");
        cur = cur->next;
        i++;
    }
    if(cur) printf("  ... (%d more)\n", gTexPoolCount - i);

    struct DuplicateSummary
    {
        char name[32];
        int count;
        uint32 totalSize;
        uint32 maxSize;
    };

    DuplicateSummary topDuplicates[12];
    memset(topDuplicates, 0, sizeof(topDuplicates));
    int unnamedCount = 0;
    uint32 unnamedTotal = 0;

    for(GxTexPoolEntry *entry = gTexPool; entry; entry = entry->next) {
        if(entry->name[0] == '\0') {
            unnamedCount++;
            unnamedTotal += entry->size;
            continue;
        }

        bool seenEarlier = false;
        for(GxTexPoolEntry *prev = gTexPool; prev != entry; prev = prev->next) {
            if(nameEqualsNoCase(prev->name, entry->name)) {
                seenEarlier = true;
                break;
            }
        }
        if(seenEarlier)
            continue;

        int count = 0;
        uint32 totalSize = 0;
        uint32 maxSize = 0;
        for(GxTexPoolEntry *scan = entry; scan; scan = scan->next) {
            if(!nameEqualsNoCase(scan->name, entry->name))
                continue;
            count++;
            totalSize += scan->size;
            if(scan->size > maxSize)
                maxSize = scan->size;
        }

        if(count <= 1)
            continue;

        DuplicateSummary candidate;
        memset(&candidate, 0, sizeof(candidate));
        strncpy(candidate.name, entry->name, sizeof(candidate.name) - 1);
        candidate.count = count;
        candidate.totalSize = totalSize;
        candidate.maxSize = maxSize;

        for(int slot = 0; slot < (int)(sizeof(topDuplicates) / sizeof(topDuplicates[0])); slot++) {
            if(topDuplicates[slot].count == 0 ||
               candidate.totalSize > topDuplicates[slot].totalSize ||
               (candidate.totalSize == topDuplicates[slot].totalSize &&
                candidate.count > topDuplicates[slot].count)) {
                for(int move = (int)(sizeof(topDuplicates) / sizeof(topDuplicates[0])) - 1; move > slot; move--)
                    topDuplicates[move] = topDuplicates[move - 1];
                topDuplicates[slot] = candidate;
                break;
            }
        }
    }

    if(unnamedCount > 0) {
        printf("[GX-TEXP] Unnamed rasters: count=%d total=%uKB\n",
               unnamedCount, unnamedTotal / 1024);
    }

    bool printedDuplicateHeader = false;
    for(int slot = 0; slot < (int)(sizeof(topDuplicates) / sizeof(topDuplicates[0])); slot++) {
        if(topDuplicates[slot].count <= 1)
            continue;
        if(!printedDuplicateHeader) {
            printf("[GX-TEXP] Duplicate names summary (duplicates are normal sometimes; repeated large world textures are worth checking):\n");
            printedDuplicateHeader = true;
        }
        printf("  dup#%d: name='%s' count=%d total=%uKB max=%uKB\n",
               slot,
               topDuplicates[slot].name,
               topDuplicates[slot].count,
               topDuplicates[slot].totalSize / 1024,
               topDuplicates[slot].maxSize / 1024);
    }
}

uint32
texPoolTotalBytes(void)
{
    return gTexPoolTotalSize;
}

int
texPoolCount(void)
{
    return gTexPoolCount;
}

void
texPoolSetSoftBudget(uint32 bytes)
{
    bytes = clampSoftBudgetToRuntimePool(bytes);

    if(gSoftBudgetBytes == bytes)
        return;

    gSoftBudgetBytes = bytes;
    printf("[GX-TEXP] soft budget set to %uKB (pool=%uKB tex=%d)\n",
           gSoftBudgetBytes / 1024,
           gTexPoolTotalSize / 1024,
           gTexPoolCount);
}

uint32
texPoolGetSoftBudget(void)
{
    return gSoftBudgetBytes;
}

} // namespace gx
} // namespace rw

#endif // GAMECUBE

