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
#include <stdarg.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwgx.h"
#include "gxmemory.h"
#ifdef WII
void MemoryMgrFreeMem2(void *mem);
void *MemoryMgrMallocAlignMem2Strict(size_t size, size_t align);
void MemoryMgrFreeAlignMem2(void *mem);
extern "C" int WiiMemoryOwnsGenericMem2(const void *ptr);
extern "C" unsigned short WiiMemoryGetResourceAttributionOwner(void);
extern "C" void WiiMemoryRecordResidentPool(unsigned int poolBit);
extern "C" void WiiMemoryRecordResidentDelta(unsigned short ownerStreamId,
                                               unsigned int poolBit,
                                               int deltaBytes);
extern "C" unsigned int WiiMemoryGetStreamingPressure(void);
extern "C" void WiiMemoryDumpStats(const char *reason);
#endif

// Define GX_PIPELINE_DIAGNOSTICS when targeted texture-pool tracing is required.
#ifndef GX_PIPELINE_DIAGNOSTICS
#define printf(...) ((void)sizeof((::printf)(__VA_ARGS__)))
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
static Raster         *gActiveHudWeaponRaster = nullptr;
#ifdef WII
#ifndef WII_GX_ARENA2_EXPANSION_BYTES
#define WII_GX_ARENA2_EXPANSION_BYTES 0u
#endif
#endif
#ifdef WII
#if WII_GX_ARENA2_EXPANSION_BYTES == 8388608
static const uint32    GX_TEXP_DEFAULT_SOFT_BUDGET_BYTES = 30u * 1024u * 1024u;
static const uint32    GX_TEXP_MIN_SOFT_BUDGET_BYTES     = 18u * 1024u * 1024u;
static const uint32    GX_TEXP_MAX_SOFT_BUDGET_BYTES     = 30u * 1024u * 1024u;
#elif WII_GX_ARENA2_EXPANSION_BYTES == 6291456
static const uint32    GX_TEXP_DEFAULT_SOFT_BUDGET_BYTES = 28u * 1024u * 1024u;
static const uint32    GX_TEXP_MIN_SOFT_BUDGET_BYTES     = 18u * 1024u * 1024u;
static const uint32    GX_TEXP_MAX_SOFT_BUDGET_BYTES     = 28u * 1024u * 1024u;
#elif WII_GX_ARENA2_EXPANSION_BYTES == 2097152
static const uint32    GX_TEXP_DEFAULT_SOFT_BUDGET_BYTES = 24u * 1024u * 1024u;
static const uint32    GX_TEXP_MIN_SOFT_BUDGET_BYTES     = 18u * 1024u * 1024u;
static const uint32    GX_TEXP_MAX_SOFT_BUDGET_BYTES     = 24u * 1024u * 1024u;
#else
static const uint32    GX_TEXP_DEFAULT_SOFT_BUDGET_BYTES = 22u * 1024u * 1024u;
static const uint32    GX_TEXP_MIN_SOFT_BUDGET_BYTES     = 18u * 1024u * 1024u;
static const uint32    GX_TEXP_MAX_SOFT_BUDGET_BYTES     = 22u * 1024u * 1024u;
#endif
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
static int             gPersistentUiTextureUploadDepth = 0;
static bool            gTexPoolResidencyReporting = false;

#ifdef WII
static const unsigned short WII_TEXPOOL_OWNER_UNKNOWN = 0xffffu;

static unsigned int
texPoolStoragePoolBit(const void *gxData)
{
    if(gxMemOwns(gxData))
        return 4u;
    if(WiiMemoryOwnsGenericMem2(gxData))
        return 1u;
    return 0u;
}

static void
texPoolRecordOwnerDelta(unsigned short ownerStreamId, const void *gxData,
                        int deltaBytes)
{
    unsigned int poolBit = texPoolStoragePoolBit(gxData);
    if(poolBit != 0 && deltaBytes != 0)
        WiiMemoryRecordResidentDelta(ownerStreamId, poolBit, deltaBytes);
}
#endif

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
#define MAX_SAFE_ALLOC_SHRINKS 32
// Minimum texture dimension (won't shrink below this)
#define MIN_TEX_DIM           4
// Soft texture budget used while loading. Real GC target will need to be lower;
// this keeps enough headroom for geometry before we optimize streaming further.
#define GX_TEX_BUDGET_STEPS   256
#ifdef WII
#if WII_GX_ARENA2_EXPANSION_BYTES != 0 && WII_GX_ARENA2_EXPANSION_BYTES != 2097152 && WII_GX_ARENA2_EXPANSION_BYTES != 6291456 && WII_GX_ARENA2_EXPANSION_BYTES != 8388608
#error "WII_GX_ARENA2_EXPANSION_BYTES must be 0, 2097152, 6291456, or 8388608"
#endif
#define GX_GXPOOL_BASE_BYTES       (24u * 1024u * 1024u)
#define GX_GXPOOL_MAX_BYTES        (GX_GXPOOL_BASE_BYTES + WII_GX_ARENA2_EXPANSION_BYTES)
#define GX_GXPOOL_MIN_BYTES        (GX_GXPOOL_BASE_BYTES + WII_GX_ARENA2_EXPANSION_BYTES)
#define GX_GXPOOL_KEEP_FREE_BYTES  (8u * 1024u * 1024u)
#else
#define GX_GXPOOL_MAX_BYTES        (32u * 1024u * 1024u)
#define GX_GXPOOL_MIN_BYTES        (12u * 1024u * 1024u)
#define GX_GXPOOL_KEEP_FREE_BYTES  (8u * 1024u * 1024u)
#endif
#define GX_GXPOOL_ALLOC_MAGIC      0x4758504Fu

// ââ Forward declarations ââââââââââââââââââââââââââââââââââââââ
struct Arena2AllocHeader;

static void downsample2x(const uint8 *src, int sw, int sh,
                         uint8 *dst, int dw, int dh);
static bool isShrinkableFormat(uint8 fmt);
static bool isBudgetProtectedTexture(const GxTexPoolEntry *entry);
static bool isUnnamedBudgetProtectedTexture(const GxTexPoolEntry *entry);
static bool nameEqualsNoCase(const char *a, const char *b);
#ifndef WII
static bool allowLargeMemFallbackForTag(const char *tag);
#endif
static void logTextureShrink(const char *kind, const GxTexPoolEntry *entry,
                             int oldW, int oldH, uint32 oldSize,
                             int newW, int newH, uint32 newSize);
static void logShrinkCheck(const char *kind, const GxTexPoolEntry *entry,
                           Raster *raster, GxRaster *natras,
                           void *oldGxData, uint32 oldSize,
                           int oldW, int oldH, uint8 oldFmt,
                           const uint8 *samplePixels, int sampleW, int sampleH);
static void insertPoolEntrySorted(GxTexPoolEntry *entry);
static uint32 cmprTiledSizePadded(int w, int h);
static const uint8 *cmprBlockPtr(const uint8 *data, int w,
                                 int blockX, int blockY);
static void shrinkCMPRByBlockDrop(uint8 *dst, const uint8 *src,
                                  int oldW, int newW, int newH);
static uint32 rgb565TiledSizePadded(int w, int h);
static void convertGX_RGBA8ToOpaqueRGB565(void *dst, const void *src,
                                          int w, int h);
static void ensureShrinkEmergencyReserve(void);
static void releaseShrinkEmergencyReserve(const char *reason);
static int tightenBudgetForSafeAlloc(size_t size);
static int texPoolEnforceBudgetImmediateSteps(const char *reason, int maxSteps);
void texPoolEnforceBudgetImmediate(const char *reason, int maxSteps);
static uint32 clampSoftBudgetToRuntimePool(uint32 bytes);
static uintptr_t alignUpPow2(uintptr_t value, size_t alignment);
static uintptr_t alignDownPow2(uintptr_t value, size_t alignment);
static size_t arena2RequiredBlockSize(size_t size, size_t alignment);
static size_t arena2EmergencyReserveTotalSize(void);
static void* allocCpuTemp(size_t size, size_t alignment);
static void freeCpuTemp(void *ptr);
static void initArena2Pool(void);
static void* arena2Alloc(size_t size, size_t alignment);
static void arena2Free(void *ptr);
static bool arena2ShouldDeferCompaction(size_t requiredBlockBytes,
                                        size_t totalFree, size_t largestFree);
static void markArena2CompactionPending(size_t requiredBlockBytes,
                                        size_t totalFree, size_t largestFree,
                                        const char *reason);
static bool arena2ValidateTrackedEntries(void);
static GxTexPoolEntry* arena2FindTrackedEntryForHeader(const Arena2AllocHeader *header,
                                                       int *matchCount);
static bool rebuildRasterTexObjForEntry(GxTexPoolEntry *entry, bool flushPayload);
static bool validateRasterTexObjForEntry(const GxTexPoolEntry *entry);
static bool arena2CompactInternal(const char *reason, bool force,
                                  bool gpuAlreadyIdle,
                                  size_t requiredBlockBytes);
static void texPoolReportLine(bool sysReport, const char *fmt, ...);
static void texPoolFormatBytes(uint32 bytes, char *buffer, size_t bufferSize);
static const char* texPoolFormatName(uint8 fmt);
static void texPoolCollectDuplicateSummaries(struct DuplicateSummary *topDuplicates,
                                             int *unnamedCount, uint32 *unnamedTotal);
static void texPoolEmitSummaryLines(bool sysReport, const char *reason);

struct DuplicateSummary
{
    char name[32];
    int count;
    uint32 totalSize;
    uint32 maxSize;
};

struct Arena2FreeBlock
{
    Arena2FreeBlock *prev;
    Arena2FreeBlock *next;
    size_t size;
};

struct Arena2AllocHeader
{
    uint32 magic;
    GxTexPoolEntry *trackedEntry;
    size_t totalSize;
};

static bool             gArena2InitAttempted = false;
static bool             gArena2Ready = false;
static uint8           *gArena2Base = nullptr;
static uint8           *gArena2End = nullptr;
static size_t           gArena2Size = 0;
static Arena2FreeBlock *gArena2FreeList = nullptr;
static bool             gArena2CompactionPending = false;
static size_t           gArena2PendingCompactBytes = 0;
static uint32           gArena2FallbackCount = 0;
static uint32           gArena2AllocFailCount = 0;
static size_t           gArena2BytesUsed = 0;
static size_t           gArena2PeakBytesUsed = 0;
static uint32           gArena2CompactionRequestCount = 0;
static uint32           gArena2CompactionSuccessCount = 0;
static uint32           gArena2CompactionRejectCount = 0;
static uint32           gArena2CompactionSkipCount = 0;
static uint32           gArena2CompactionMoveCount = 0;
static size_t           gArena2CompactionBytesMoved = 0;
static uint32           gArena2CompactionGeneration = 0;
static size_t           gArena2CompactionLastTotalFree = 0;
static size_t           gArena2CompactionLastLargestBefore = 0;
static size_t           gArena2CompactionLastLargestAfter = 0;
#ifdef WII
static uint32           gBudgetShrinkFrame = UINT32_MAX;
static int              gBudgetShrinksThisFrame = 0;
static uint8            gArena2FreesSinceFragmentCheck = 0;
#endif

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

static size_t
arena2RequiredBlockSize(size_t size, size_t alignment)
{
    if(size == 0)
        size = 1;
    if(alignment < 4)
        alignment = 4;

    size_t total = sizeof(Arena2AllocHeader) +
                   sizeof(Arena2AllocHeader*) +
                   (alignment - 1u) +
                   size;
    if(total < sizeof(Arena2FreeBlock))
        total = sizeof(Arena2FreeBlock);
    return (size_t)alignUpPow2((uintptr_t)total, 32);
}

static size_t
arena2EmergencyReserveTotalSize(void)
{
    if(gShrinkEmergencyReserve == nil ||
       !gxMemOwns(gShrinkEmergencyReserve) ||
       (uint8*)gShrinkEmergencyReserve < gArena2Base + sizeof(Arena2AllocHeader*))
        return 0;

    Arena2AllocHeader *header = ((Arena2AllocHeader**)gShrinkEmergencyReserve)[-1];
    if(header == nil ||
       (uint8*)header < gArena2Base ||
       (uint8*)header + sizeof(Arena2AllocHeader) > gArena2End ||
       header->magic != GX_GXPOOL_ALLOC_MAGIC ||
       (uint8*)header + header->totalSize > gArena2End)
        return 0;

    return header->totalSize;
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
arena2GetFreeStats(size_t *totalFree, size_t *largestFree)
{
    size_t total = 0;
    size_t largest = 0;
    int steps = 0;
    for(Arena2FreeBlock *block = gArena2FreeList;
        block && steps < MAX_ARENA2_FREE_STEPS;
        block = block->next, steps++) {
        total += block->size;
        if(block->size > largest)
            largest = block->size;
    }
    if(totalFree)
        *totalFree = total;
    if(largestFree)
        *largestFree = largest;
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
    // Keep a deterministic high-end GX partition and leave the middle of
    // Arena2 available to newlib rather than consuming every spare byte.
    size_t maxPoolBytes = GX_GXPOOL_MAX_BYTES;
    size_t minPoolBytes = GX_GXPOOL_MIN_BYTES;
    if(WII_GX_ARENA2_EXPANSION_BYTES != 0 &&
       available < GX_GXPOOL_MAX_BYTES + GX_GXPOOL_KEEP_FREE_BYTES) {
        // An expanded profile must preserve the established raw/newlib startup
        // headroom. If it cannot, retain the base GX boundary instead.
        SYS_Report("[GX-POOL] GX expansion rejected (need=%u KB available=%u KB); using base boundary\n",
                   (unsigned)((GX_GXPOOL_MAX_BYTES + GX_GXPOOL_KEEP_FREE_BYTES) / 1024u),
                   (unsigned)(available / 1024u));
        maxPoolBytes = GX_GXPOOL_BASE_BYTES;
        minPoolBytes = GX_GXPOOL_BASE_BYTES;
    }
    size_t poolSize = available;
    if(poolSize > GX_GXPOOL_KEEP_FREE_BYTES)
        poolSize -= GX_GXPOOL_KEEP_FREE_BYTES;
    else
        poolSize = available / 2u;
#else
    size_t poolSize = available / 2u;
#endif

    if(poolSize > maxPoolBytes)
        poolSize = maxPoolBytes;
    if(poolSize < minPoolBytes)
        poolSize = minPoolBytes;

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

void
gxMemInitArena2Pool(void)
{
    initArena2Pool();
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

bool
gxMemOwns(const void *ptr)
{
    initArena2Pool();
    if(!gArena2Ready || ptr == nullptr)
        return false;
    return (const uint8*)ptr >= gArena2Base && (const uint8*)ptr < gArena2End;
}

static bool
arena2BindTrackedEntry(void *gxData, GxTexPoolEntry *entry)
{
    if(!gxMemOwns(gxData))
        return true;
    if(gxData == nullptr || entry == nullptr ||
       (uint8*)gxData < gArena2Base + sizeof(Arena2AllocHeader*))
        return false;

    Arena2AllocHeader *header = ((Arena2AllocHeader**)gxData)[-1];
    if(header == nullptr ||
       (uint8*)header < gArena2Base ||
       (uint8*)header + sizeof(Arena2AllocHeader) > gArena2End ||
       header->magic != GX_GXPOOL_ALLOC_MAGIC ||
       (uint8*)header + header->totalSize > gArena2End)
        return false;

    header->trackedEntry = entry;
    return true;
}

void*
gxMemGetPoolBase(void)
{
    initArena2Pool();
    return gArena2Base;
}

void*
gxMemGetPoolEnd(void)
{
    initArena2Pool();
    return gArena2End;
}

void
gxMemGetPoolStats(uint32 *capacityBytes, uint32 *usedBytes,
                  uint32 *peakBytes, uint32 *largestFreeBytes,
                  uint32 *allocFailCount, uint32 *fallbackCount)
{
    initArena2Pool();
    size_t largest = 0;
    if(largestFreeBytes)
        arena2GetFreeStats(nullptr, &largest);
    if(capacityBytes)
        *capacityBytes = (uint32)gArena2Size;
    if(usedBytes)
        *usedBytes = (uint32)gArena2BytesUsed;
    if(peakBytes)
        *peakBytes = (uint32)gArena2PeakBytesUsed;
    if(largestFreeBytes)
        *largestFreeBytes = (uint32)largest;
    if(allocFailCount)
        *allocFailCount = gArena2AllocFailCount;
    if(fallbackCount)
        *fallbackCount = gArena2FallbackCount;
}

static void*
allocFallbackHostMemory(size_t size, size_t alignment)
{
#ifdef WII
    return MemoryMgrMallocAlignMem2Strict(size, alignment > 1 ? alignment : 32);
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
        header->trackedEntry = nil;
        header->totalSize = totalUsed;
        ((Arena2AllocHeader**)userPtr)[-1] = header;
        gArena2BytesUsed += totalUsed;
        if(gArena2BytesUsed > gArena2PeakBytesUsed)
            gArena2PeakBytesUsed = gArena2BytesUsed;
        return (void*)userPtr;
    }

    gArena2AllocFailCount++;
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

    size_t totalSize = header->totalSize;
    header->trackedEntry = nil;
    header->magic = 0;

    Arena2FreeBlock *block = (Arena2FreeBlock*)header;
    block->size = totalSize;
    if(totalSize <= gArena2BytesUsed)
        gArena2BytesUsed -= totalSize;
    else
        gArena2BytesUsed = 0;
    arena2InsertFreeBlock(block);
#ifdef WII
    // Detect fragmentation while freeing, before the next large allocation
    // is forced to block on GX_DrawDone. The pending request is serviced once
    // by endUpdate(), where the GPU is already idle after the display copy.
    if(!gArena2CompactionPending &&
       (++gArena2FreesSinceFragmentCheck >= 8 || totalSize >= 256u * 1024u)) {
        gArena2FreesSinceFragmentCheck = 0;
        const size_t proactiveBlockBytes = 512u * 1024u;
        size_t totalFree = 0;
        size_t largestFree = 0;
        arena2GetFreeStats(&totalFree, &largestFree);
        if(totalFree >= 1u * 1024u * 1024u &&
           arena2ShouldDeferCompaction(proactiveBlockBytes,
                                       totalFree, largestFree))
            markArena2CompactionPending(proactiveBlockBytes,
                                        totalFree, largestFree,
                                        "free-fragmentation");
    }
#endif
}

static bool
arena2ShouldDeferCompaction(size_t requiredBlockBytes,
                            size_t totalFree, size_t largestFree)
{
    if(requiredBlockBytes == 0 ||
       totalFree < requiredBlockBytes ||
       largestFree >= requiredBlockBytes)
        return false;

    size_t fragmentedBytes = totalFree - largestFree;
    size_t threshold = 128u * 1024u;
    if(threshold < requiredBlockBytes / 4u)
        threshold = requiredBlockBytes / 4u;
    return fragmentedBytes >= threshold;
}

static void
markArena2CompactionPending(size_t requiredBlockBytes,
                            size_t totalFree, size_t largestFree,
                            const char *reason)
{
    bool firstPending = !gArena2CompactionPending;
    size_t oldBytes = gArena2PendingCompactBytes;

    gArena2CompactionPending = true;
    if(requiredBlockBytes > gArena2PendingCompactBytes)
        gArena2PendingCompactBytes = requiredBlockBytes;

    if(firstPending || gArena2PendingCompactBytes != oldBytes) {
        texPoolReportLine(true,
                          "[GX-POOL] pending compaction reason=%s need=%uKB free=%uKB largest=%uKB pool=%uKB tex=%d\n",
                          reason ? reason : "<none>",
                          (unsigned)(gArena2PendingCompactBytes / 1024u),
                          (unsigned)(totalFree / 1024u),
                          (unsigned)(largestFree / 1024u),
                          gTexPoolTotalSize / 1024u,
                          gTexPoolCount);
    }
}

static GxTexPoolEntry*
arena2FindTrackedEntryForHeader(const Arena2AllocHeader *header, int *matchCount)
{
    if(matchCount)
        *matchCount = 0;
    if(header == nullptr || header->trackedEntry == nullptr)
        return nil;

    GxTexPoolEntry *entry = header->trackedEntry;
    if(entry->gxData == nullptr || !gxMemOwns(entry->gxData) ||
       (uint8*)entry->gxData < gArena2Base + sizeof(Arena2AllocHeader*) ||
       ((Arena2AllocHeader**)entry->gxData)[-1] != header)
        return nil;

    if(matchCount)
        *matchCount = 1;
    return entry;
}

static bool
arena2ValidateTrackedEntries(void)
{
    int steps = 0;
    for(GxTexPoolEntry *entry = gTexPool; entry; entry = entry->next) {
        if(++steps > MAX_TEXPOOL_SCAN_STEPS) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: texPool scan limit exceeded head=%p limit=%d\n",
                              (void*)gTexPool, MAX_TEXPOOL_SCAN_STEPS);
            return false;
        }
        if(entry->gxData == nullptr || !gxMemOwns(entry->gxData))
            continue;
        if(entry->raster == nil) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: tracked Arena2 entry missing raster gx=%p name='%s'\n",
                              entry->gxData,
                              entry->name[0] ? entry->name : "<unnamed>");
            return false;
        }

        GxRaster *natras = PLUGINOFFSET(GxRaster, entry->raster, nativeRasterOffset);
        if(natras == nil || natras->gxData != entry->gxData) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: raster mismatch gx=%p raster=%p natras=%p natras->gx=%p\n",
                              entry->gxData,
                              (void*)entry->raster,
                              (void*)natras,
                              natras ? natras->gxData : nil);
            return false;
        }
        if((uint8*)entry->gxData < gArena2Base + sizeof(Arena2AllocHeader*)) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: gxData backlink underrun gx=%p base=%p\n",
                              entry->gxData, (void*)gArena2Base);
            return false;
        }

        Arena2AllocHeader *header = ((Arena2AllocHeader**)entry->gxData)[-1];
        if(header == nil ||
           (uint8*)header < gArena2Base ||
           (uint8*)header + sizeof(Arena2AllocHeader) > gArena2End ||
           header->magic != GX_GXPOOL_ALLOC_MAGIC ||
           header->totalSize < sizeof(Arena2FreeBlock) ||
           (uint8*)header + header->totalSize > gArena2End) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: invalid tracked header gx=%p header=%p size=%u\n",
                              entry->gxData, (void*)header, entry->size);
            return false;
        }
        if(header->trackedEntry != entry) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: header owner mismatch gx=%p header=%p tracked=%p expected=%p\n",
                              entry->gxData, (void*)header,
                              (void*)header->trackedEntry, (void*)entry);
            return false;
        }

        size_t userOffset = (size_t)((uint8*)entry->gxData - (uint8*)header);
        size_t requiredOffset = sizeof(Arena2AllocHeader) + sizeof(Arena2AllocHeader*);
        if(userOffset < requiredOffset || userOffset >= header->totalSize) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: corrupt user offset gx=%p header=%p ofs=%u total=%u\n",
                              entry->gxData, (void*)header,
                              (unsigned)userOffset,
                              (unsigned)header->totalSize);
            return false;
        }

        if(entry->size == 0 || natras->dataSize != entry->size) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: size mismatch gx=%p entry=%u raster=%u\n",
                              entry->gxData,
                              entry->size,
                              natras->dataSize);
            return false;
        }
        if(userOffset + entry->size > header->totalSize) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: payload overrun gx=%p payload=%u total=%u ofs=%u\n",
                              entry->gxData,
                              entry->size,
                              (unsigned)header->totalSize,
                              (unsigned)userOffset);
            return false;
        }
    }
    return true;
}

static bool
rebuildRasterTexObjForEntry(GxTexPoolEntry *entry, bool flushPayload)
{
    if(entry == nil || entry->raster == nil || entry->gxData == nil)
        return false;

    GxRaster *natras = PLUGINOFFSET(GxRaster, entry->raster, nativeRasterOffset);
    if(natras == nil || natras->gxData != entry->gxData)
        return false;

    natras->dataSize = entry->size;
    natras->w = entry->width;
    natras->h = entry->height;
    natras->gxFmt = entry->gxFmt;

    invalidateTextureBinding(entry->raster);
    if(flushPayload && entry->size > 0)
        DCFlushRange(entry->gxData, entry->size);
    GX_InitTexObj(&natras->texObj, natras->gxData,
                  (u16)natras->w, (u16)natras->h,
                  natras->gxFmt,
                  natras->wrapS, natras->wrapT,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&natras->texObj, natras->minFilter, natras->magFilter);
    natras->texObjValid = true;
    return true;
}

static bool
validateRasterTexObjForEntry(const GxTexPoolEntry *entry)
{
    if(entry == nil || entry->raster == nil || entry->gxData == nil)
        return false;

    GxRaster *natras = PLUGINOFFSET(GxRaster, entry->raster, nativeRasterOffset);
    if(natras == nil || !natras->texObjValid ||
       natras->gxData != entry->gxData || natras->dataSize != entry->size ||
       natras->w != entry->width || natras->h != entry->height ||
       natras->gxFmt != entry->gxFmt)
        return false;

    return (uintptr_t)GX_GetTexObjData(&natras->texObj) ==
               (uintptr_t)MEM_VIRTUAL_TO_PHYSICAL(entry->gxData) &&
           GX_GetTexObjWidth(&natras->texObj) == entry->width &&
           GX_GetTexObjHeight(&natras->texObj) == entry->height &&
           (uint8)GX_GetTexObjFmt(&natras->texObj) == entry->gxFmt;
}

static bool
arena2CompactInternal(const char *reason, bool force,
                      bool gpuAlreadyIdle, size_t requiredBlockBytes)
{
    initArena2Pool();
    if(!gArena2Ready)
        return false;

    size_t totalFree = 0;
    size_t largestFree = 0;
    arena2GetFreeStats(&totalFree, &largestFree);

    bool runBecausePending = gArena2CompactionPending;
    bool runBecauseFragmented = arena2ShouldDeferCompaction(
        requiredBlockBytes, totalFree, largestFree);
    if(requiredBlockBytes == 0 && !runBecauseFragmented)
        runBecauseFragmented = totalFree >= 256u * 1024u && largestFree * 2u < totalFree;

    if(!force && !runBecausePending && !runBecauseFragmented) {
        gArena2CompactionSkipCount++;
        return false;
    }

    releaseShrinkEmergencyReserve("Arena2 compaction");
    arena2GetFreeStats(&totalFree, &largestFree);
    if(requiredBlockBytes > 0 && largestFree >= requiredBlockBytes && !force) {
        gArena2CompactionPending = false;
        gArena2PendingCompactBytes = 0;
        return false;
    }

    gArena2CompactionRequestCount++;
    gArena2CompactionLastTotalFree = totalFree;
    gArena2CompactionLastLargestBefore = largestFree;

    if(!arena2ValidateTrackedEntries()) {
        gArena2CompactionRejectCount++;
        gArena2CompactionPending = false;
        gArena2PendingCompactBytes = 0;
        return false;
    }

    uint8 *cursor = gArena2Base;
    Arena2FreeBlock *freeBlock = gArena2FreeList;
    int freeSteps = 0;
    size_t usedBytes = 0;
    size_t freeBytes = 0;
    if(freeBlock && freeBlock->prev != nil) {
        texPoolReportLine(true,
                          "[GX-POOL] reject compaction: free-list head prev=%p expected=nil\n",
                          (void*)freeBlock->prev);
        gArena2CompactionRejectCount++;
        gArena2CompactionPending = false;
        gArena2PendingCompactBytes = 0;
        return false;
    }
    while(cursor < gArena2End) {
        if(freeBlock && cursor == (uint8*)freeBlock) {
            if(++freeSteps > MAX_ARENA2_FREE_STEPS ||
               freeBlock->size < sizeof(Arena2FreeBlock) ||
               (freeBlock->size & 31u) != 0 ||
               (uint8*)freeBlock < gArena2Base ||
               (uint8*)freeBlock + freeBlock->size > gArena2End ||
               (freeBlock->next && (uint8*)freeBlock->next <= (uint8*)freeBlock) ||
               (freeBlock->next && freeBlock->next->prev != freeBlock)) {
                texPoolReportLine(true,
                                  "[GX-POOL] reject compaction: corrupt free list block=%p size=%u next=%p prev=%p\n",
                                  (void*)freeBlock, (unsigned)freeBlock->size,
                                  (void*)freeBlock->next, (void*)freeBlock->prev);
                gArena2CompactionRejectCount++;
                gArena2CompactionPending = false;
                gArena2PendingCompactBytes = 0;
                return false;
            }
            freeBytes += freeBlock->size;
            cursor += freeBlock->size;
            freeBlock = freeBlock->next;
            continue;
        }

        Arena2AllocHeader *header = (Arena2AllocHeader*)cursor;
        if(header->magic != GX_GXPOOL_ALLOC_MAGIC ||
           header->totalSize < sizeof(Arena2FreeBlock) ||
           (header->totalSize & 31u) != 0 ||
           cursor + header->totalSize > gArena2End) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: corrupt alloc header=%p magic=%08X size=%u\n",
                              (void*)header,
                              (unsigned)header->magic,
                              (unsigned)header->totalSize);
            gArena2CompactionRejectCount++;
            gArena2CompactionPending = false;
            gArena2PendingCompactBytes = 0;
            return false;
        }

        int matchCount = 0;
        GxTexPoolEntry *entry = arena2FindTrackedEntryForHeader(header, &matchCount);
        if(entry == nil || matchCount != 1) {
            texPoolReportLine(true,
                              "[GX-POOL] reject compaction: block=%p size=%u trackedMatches=%d\n",
                              (void*)header,
                              (unsigned)header->totalSize,
                              matchCount);
            gArena2CompactionRejectCount++;
            gArena2CompactionPending = false;
            gArena2PendingCompactBytes = 0;
            return false;
        }

        usedBytes += header->totalSize;
        cursor += header->totalSize;
    }

    if(freeBlock != nil ||
       usedBytes != gArena2BytesUsed ||
       usedBytes + freeBytes != gArena2Size) {
        texPoolReportLine(true,
                          "[GX-POOL] reject compaction: layout accounting mismatch used=%u tracked=%u free=%u pool=%u freeTail=%p\n",
                          (unsigned)usedBytes,
                          (unsigned)gArena2BytesUsed,
                          (unsigned)freeBytes,
                          (unsigned)gArena2Size,
                          (void*)freeBlock);
        gArena2CompactionRejectCount++;
        gArena2CompactionPending = false;
        gArena2PendingCompactBytes = 0;
        return false;
    }

    if(!gpuAlreadyIdle)
        GX_DrawDone();

    cursor = gArena2Base;
    freeBlock = gArena2FreeList;
    uint8 *writeCursor = gArena2Base;
    uint32 movedBlocks = 0;
    size_t movedPayloadBytes = 0;
    while(cursor < gArena2End) {
        if(freeBlock && cursor == (uint8*)freeBlock) {
            cursor += freeBlock->size;
            freeBlock = freeBlock->next;
            continue;
        }

        Arena2AllocHeader *oldHeader = (Arena2AllocHeader*)cursor;
        int matchCount = 0;
        GxTexPoolEntry *entry = arena2FindTrackedEntryForHeader(oldHeader, &matchCount);
        GxRaster *natras = PLUGINOFFSET(GxRaster, entry->raster, nativeRasterOffset);
        size_t oldTotalSize = oldHeader->totalSize;
        size_t userOffset = (size_t)((uint8*)entry->gxData - (uint8*)oldHeader);
        Arena2AllocHeader *newHeader = (Arena2AllocHeader*)writeCursor;
        void *newGxData = writeCursor + userOffset;

        if(writeCursor != cursor) {
            memmove(writeCursor, cursor, oldTotalSize);
            movedBlocks++;
            movedPayloadBytes += entry->size;
        }

        ((Arena2AllocHeader**)newGxData)[-1] = newHeader;
        entry->gxData = newGxData;
        natras->gxData = newGxData;
        if(writeCursor != cursor)
            rebuildRasterTexObjForEntry(entry, true);

        writeCursor += oldTotalSize;
        cursor += oldTotalSize;
    }

    size_t tailFree = (size_t)(gArena2End - writeCursor);
    if(tailFree > 0) {
        Arena2FreeBlock *tail = (Arena2FreeBlock*)writeCursor;
        tail->prev = nil;
        tail->next = nil;
        tail->size = tailFree;
        gArena2FreeList = tail;
    } else
        gArena2FreeList = nil;

    gArena2BytesUsed = usedBytes;
    gArena2CompactionPending = false;
    gArena2PendingCompactBytes = 0;
    gArena2CompactionSuccessCount++;
    gArena2CompactionGeneration++;
    gArena2CompactionMoveCount += movedBlocks;
    gArena2CompactionBytesMoved += movedPayloadBytes;

    uint32 texObjMismatchCount = 0;
    const GxTexPoolEntry *firstTexObjMismatch = nil;
    for(GxTexPoolEntry *entry = gTexPool; entry; entry = entry->next) {
        if(!gxMemOwns(entry->gxData))
            continue;
        if(!validateRasterTexObjForEntry(entry)) {
            texObjMismatchCount++;
            if(firstTexObjMismatch == nil)
                firstTexObjMismatch = entry;
        }
    }

    arena2GetFreeStats(&totalFree, &largestFree);
    gArena2CompactionLastLargestAfter = largestFree;

    GX_InvalidateTexAll();
    texPoolReportLine(true,
                      "[GX-POOL] compacted gen=%u reason=%s force=%d moved=%u payload=%uKB free=%uKB largest=%uKB->%uKB pending=%d texObjMismatch=%u firstMismatch=%s\n",
                      gArena2CompactionGeneration,
                      reason ? reason : "<none>",
                      force ? 1 : 0,
                      movedBlocks,
                      (unsigned)(movedPayloadBytes / 1024u),
                      (unsigned)(totalFree / 1024u),
                      (unsigned)(gArena2CompactionLastLargestBefore / 1024u),
                      (unsigned)(gArena2CompactionLastLargestAfter / 1024u),
                      gArena2CompactionPending ? 1 : 0,
                      texObjMismatchCount,
                      firstTexObjMismatch && firstTexObjMismatch->name[0] ?
                          firstTexObjMismatch->name : "<none>");
    return true;
}

static void
texPoolReportLine(bool sysReport, const char *fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
#ifdef WII
    if(sysReport)
        SYS_Report("%s", buffer);
    else
#endif
        printf("%s", buffer);
}

static void
texPoolFormatBytes(uint32 bytes, char *buffer, size_t bufferSize)
{
    if(buffer == nil || bufferSize == 0)
        return;

    if(bytes >= 1024u * 1024u)
        snprintf(buffer, bufferSize, "%uB (%.2f MiB)",
                 (unsigned)bytes,
                 (double)bytes / (1024.0 * 1024.0));
    else if(bytes >= 1024u)
        snprintf(buffer, bufferSize, "%uB (%.2f KiB)",
                 (unsigned)bytes,
                 (double)bytes / 1024.0);
    else
        snprintf(buffer, bufferSize, "%uB", (unsigned)bytes);

    buffer[bufferSize - 1] = '\0';
}

static const char*
texPoolFormatName(uint8 fmt)
{
    switch(fmt)
    {
    case GX_TF_I4:     return "I4";
    case GX_TF_I8:     return "I8";
    case GX_TF_IA4:    return "IA4";
    case GX_TF_IA8:    return "IA8";
    case GX_TF_RGB565: return "RGB565";
    case GX_TF_RGB5A3: return "RGB5A3";
    case GX_TF_RGBA8:  return "RGBA8";
    case GX_TF_CI4:    return "CI4";
    case GX_TF_CI8:    return "CI8";
    case GX_TF_CI14:   return "CI14";
    case GX_TF_CMPR:   return "CMPR";
    default:           return "UNKNOWN";
    }
}

static void
texPoolCollectDuplicateSummaries(DuplicateSummary *topDuplicates,
                                 int *unnamedCount, uint32 *unnamedTotal)
{
    if(topDuplicates == nil)
        return;

    memset(topDuplicates, 0, sizeof(DuplicateSummary) * 12);
    if(unnamedCount)
        *unnamedCount = 0;
    if(unnamedTotal)
        *unnamedTotal = 0;

    for(GxTexPoolEntry *entry = gTexPool; entry; entry = entry->next) {
        if(entry->name[0] == '\0') {
            if(unnamedCount)
                (*unnamedCount)++;
            if(unnamedTotal)
                *unnamedTotal += entry->size;
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
        memcpy(candidate.name, entry->name, sizeof(candidate.name) - 1);
        candidate.name[sizeof(candidate.name) - 1] = '\0';
        candidate.count = count;
        candidate.totalSize = totalSize;
        candidate.maxSize = maxSize;

        for(int slot = 0; slot < 12; slot++) {
            if(topDuplicates[slot].count == 0 ||
               candidate.totalSize > topDuplicates[slot].totalSize ||
               (candidate.totalSize == topDuplicates[slot].totalSize &&
                candidate.count > topDuplicates[slot].count)) {
                for(int move = 11; move > slot; move--)
                    topDuplicates[move] = topDuplicates[move - 1];
                topDuplicates[slot] = candidate;
                break;
            }
        }
    }
}

static void
texPoolEmitSummaryLines(bool sysReport, const char *reason)
{
    struct FormatSummary
    {
        uint8 fmt;
        int count;
        uint32 bytes;
    };

    enum { MAX_FORMAT_SUMMARIES = 11 };
    FormatSummary formatTotals[MAX_FORMAT_SUMMARIES];
    memset(formatTotals, 0, sizeof(formatTotals));

    DuplicateSummary topDuplicates[12];
    int unnamedCount = 0;
    uint32 unnamedTotal = 0;
    texPoolCollectDuplicateSummaries(topDuplicates, &unnamedCount, &unnamedTotal);

    for(GxTexPoolEntry *entry = gTexPool; entry; entry = entry->next) {
        int slot = -1;
        for(int i = 0; i < MAX_FORMAT_SUMMARIES; i++) {
            if(formatTotals[i].count > 0 && formatTotals[i].fmt == entry->gxFmt) {
                slot = i;
                break;
            }
            if(slot < 0 && formatTotals[i].count == 0)
                slot = i;
        }
        if(slot >= 0) {
            formatTotals[slot].fmt = entry->gxFmt;
            formatTotals[slot].count++;
            formatTotals[slot].bytes += entry->size;
        }
    }

    for(int i = 1; i < MAX_FORMAT_SUMMARIES; i++) {
        FormatSummary current = formatTotals[i];
        int j = i - 1;
        while(j >= 0 && current.count > 0 &&
              (formatTotals[j].count == 0 || current.bytes > formatTotals[j].bytes)) {
            formatTotals[j + 1] = formatTotals[j];
            j--;
        }
        formatTotals[j + 1] = current;
    }

    char totalBytes[32];
    char softBudget[32];
    texPoolFormatBytes(gTexPoolTotalSize, totalBytes, sizeof(totalBytes));
    texPoolFormatBytes(gSoftBudgetBytes, softBudget, sizeof(softBudget));
    texPoolReportLine(sysReport,
                      "[GX-TEXP] residency %s: actual=%s count=%d lifetimeShrinks=%d softBudget=%s\n",
                      reason ? reason : "snapshot",
                      totalBytes, gTexPoolCount, gShrinkTotalCount, softBudget);
    if(gArena2Ready) {
        size_t totalFree = 0;
        size_t largestFree = 0;
        arena2GetFreeStats(&totalFree, &largestFree);
        texPoolReportLine(sysReport,
                          "[GX-POOL] arena2 used=%uKB free=%uKB largest=%uKB pending=%u compact=%u/%u reject=%u moved=%u payload=%uKB\n",
                          (unsigned)(gArena2BytesUsed / 1024u),
                          (unsigned)(totalFree / 1024u),
                          (unsigned)(largestFree / 1024u),
                          gArena2CompactionPending ? 1u : 0u,
                          gArena2CompactionSuccessCount,
                          gArena2CompactionRequestCount,
                          gArena2CompactionRejectCount,
                          gArena2CompactionMoveCount,
                          (unsigned)(gArena2CompactionBytesMoved / 1024u));
    }

    bool printedFormatHeader = false;
    for(int i = 0; i < MAX_FORMAT_SUMMARIES; i++) {
        if(formatTotals[i].count <= 0)
            continue;
        if(!printedFormatHeader) {
            texPoolReportLine(sysReport, "[GX-TEXP] format totals:\n");
            printedFormatHeader = true;
        }
        char bytes[32];
        texPoolFormatBytes(formatTotals[i].bytes, bytes, sizeof(bytes));
        texPoolReportLine(sysReport, "  fmt=%s count=%d actual=%s\n",
                          texPoolFormatName(formatTotals[i].fmt),
                          formatTotals[i].count, bytes);
    }

    texPoolReportLine(sysReport, "[GX-TEXP] top 20 largest entries:\n");
    int index = 0;
    for(GxTexPoolEntry *cur = gTexPool; cur && index < 20; cur = cur->next, index++) {
        char entryBytes[32];
        texPoolFormatBytes(cur->size, entryBytes, sizeof(entryBytes));
        texPoolReportLine(sysReport,
                          "  #%02d name='%s' %ux%u fmt=%s actual=%s shrinks=%u\n",
                          index + 1,
                          cur->name[0] ? cur->name : "<unnamed>",
                          (unsigned)cur->width,
                          (unsigned)cur->height,
                          texPoolFormatName(cur->gxFmt),
                          entryBytes,
                          (unsigned)cur->shrinkCount);
    }
    if(gTexPoolCount > 20)
        texPoolReportLine(sysReport, "  ... %d more entries\n", gTexPoolCount - 20);

    if(unnamedCount > 0) {
        char unnamedBytes[32];
        texPoolFormatBytes(unnamedTotal, unnamedBytes, sizeof(unnamedBytes));
        texPoolReportLine(sysReport,
                          "[GX-TEXP] unnamed entries: count=%d actual=%s\n",
                          unnamedCount, unnamedBytes);
    }

    bool printedDuplicateHeader = false;
    for(int slot = 0; slot < 12; slot++) {
        if(topDuplicates[slot].count <= 1)
            continue;
        if(!printedDuplicateHeader) {
            texPoolReportLine(sysReport, "[GX-TEXP] top duplicate-name groups:\n");
            printedDuplicateHeader = true;
        }
        char totalBytesDup[32];
        char maxBytesDup[32];
        texPoolFormatBytes(topDuplicates[slot].totalSize, totalBytesDup, sizeof(totalBytesDup));
        texPoolFormatBytes(topDuplicates[slot].maxSize, maxBytesDup, sizeof(maxBytesDup));
        texPoolReportLine(sysReport,
                          "  dup#%d name='%s' count=%d actual=%s largest=%s\n",
                          slot + 1,
                          topDuplicates[slot].name,
                          topDuplicates[slot].count,
                          totalBytesDup,
                          maxBytesDup);
    }
}

// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
// Pool Management
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
bool
texPoolRegister(Raster *raster, void *gxData, uint32 size,
                uint16 w, uint16 h, uint8 fmt, const char *name)
{
    if(raster == nil || gxData == nil || size == 0) {
        static int s_invalidRegisterLogCount = 0;
        if(s_invalidRegisterLogCount < 32) {
            fprintf(stdout,
                    "[GX-POOL-FAULT] texPoolRegister reject invalid args raster=%p gx=%p size=%u\n",
                    (void*)raster, gxData, (unsigned)size);
            s_invalidRegisterLogCount++;
        }
        return false;
    }

    int scanSteps = 0;
    for(GxTexPoolEntry *scan = gTexPool; scan; scan = scan->next) {
        if(++scanSteps > MAX_TEXPOOL_SCAN_STEPS) {
            fprintf(stdout,
                    "[GX-POOL-FAULT] texPoolRegister reject corrupt pool chain head=%p\n",
                    (void*)gTexPool);
            return false;
        }
        if(scan->gxData == gxData || scan->raster == raster) {
            static int s_duplicateRegisterLogCount = 0;
            if(s_duplicateRegisterLogCount < 32) {
                fprintf(stdout,
                        "[GX-POOL-FAULT] texPoolRegister reject duplicate raster=%p gx=%p existingRaster=%p existingGx=%p name='%s'\n",
                        (void*)raster, gxData,
                        (void*)scan->raster, scan->gxData,
                        scan->name[0] ? scan->name : "<unnamed>");
                s_duplicateRegisterLogCount++;
            }
            return false;
        }
    }
    GxTexPoolEntry *entry = (GxTexPoolEntry*)malloc(sizeof(GxTexPoolEntry));
    if(entry == nil) {
        fprintf(stdout,
                "[GX-POOL-FAULT] texPoolRegister bookkeeping OOM raster=%p gx=%p size=%u\n",
                (void*)raster, gxData, (unsigned)size);
        return false;
    }

    entry->raster      = raster;
    entry->gxData      = gxData;
    entry->size        = size;
    entry->width       = w;
    entry->height      = h;
    entry->gxFmt       = fmt;
    entry->shrinkCount = 0;
#ifdef WII
    entry->ownerStreamId = WiiMemoryGetResourceAttributionOwner();
#else
    entry->ownerStreamId = 0xffffu;
#endif
    entry->next        = nil;
    if(name && name[0]) {
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
    } else {
        entry->name[0] = '\0';
        static int s_unnamedLargeRegisterLogCount = 0;
        if(s_unnamedLargeRegisterLogCount < 48 && size >= 256u * 1024u) {
            printf("[GX-REGUNNAMED] %dx%d fmt=%u size=%u raster=%p gx=%p pool=%uKB tex=%d\n",
                   (int)w, (int)h, (unsigned)fmt, (unsigned)size,
                   (void*)raster, gxData,
                   gTexPoolTotalSize / 1024, gTexPoolCount);
            s_unnamedLargeRegisterLogCount++;
        }
    }

    if(!arena2BindTrackedEntry(gxData, entry)) {
        fprintf(stdout,
                "[GX-POOL-FAULT] texPoolRegister cannot bind Arena2 header raster=%p gx=%p size=%u\n",
                (void*)raster, gxData, (unsigned)size);
        free(entry);
        return false;
    }

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
        if(sameNameCount >= 4 && size >= 32u * 1024u)
            (void)sameNameTotal;
    }

#ifdef WII
    if(gxMemOwns(gxData))
        WiiMemoryRecordResidentPool(4u);
    else if(WiiMemoryOwnsGenericMem2(gxData))
        WiiMemoryRecordResidentPool(1u);
    texPoolRecordOwnerDelta(entry->ownerStreamId, gxData, (int)size);
#endif

    return true;
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
            if(gxMemOwns(cur->gxData)) {
                Arena2AllocHeader *header = ((Arena2AllocHeader**)cur->gxData)[-1];
                if(header && header->trackedEntry == cur)
                    header->trackedEntry = nil;
            }
#ifdef WII
            texPoolRecordOwnerDelta(cur->ownerStreamId, cur->gxData,
                                    -(int)cur->size);
#endif
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
#ifdef WII
            texPoolRecordOwnerDelta(cur->ownerStreamId, cur->gxData,
                                    (int)newSize - (int)oldSize);
#endif
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

void
setActiveHudWeaponRaster(Raster *raster)
{
    if(gActiveHudWeaponRaster == raster)
        return;

    gActiveHudWeaponRaster = raster;
#ifdef WII
    if(raster) {
        for(GxTexPoolEntry *entry = gTexPool; entry; entry = entry->next) {
            if(entry->raster == raster) {
                SYS_Report("[WII-HUD-TEX] pin raster=%p name='%s' wh=%ux%u fmt=%s size=%u shrink=%u\n",
                           (void*)raster,
                           entry->name[0] ? entry->name : "<unnamed>",
                           (unsigned)entry->width, (unsigned)entry->height,
                           texPoolFormatName(entry->gxFmt),
                           (unsigned)entry->size,
                           (unsigned)entry->shrinkCount);
                return;
            }
        }
        SYS_Report("[WII-HUD-TEX] pin raster=%p pool-entry=missing\n",
                   (void*)raster);
    } else {
        SYS_Report("[WII-HUD-TEX] clear\n");
    }
#endif
}

void
clearActiveHudWeaponRaster(Raster *raster)
{
    if(raster && gActiveHudWeaponRaster != raster)
        return;
    setActiveHudWeaponRaster(nullptr);
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

    gxMemFree(oldData);
#ifdef WII
    texPoolRecordOwnerDelta(scan->ownerStreamId, oldData, -(int)oldSize);
    texPoolRecordOwnerDelta(scan->ownerStreamId, newData, (int)newSize);
#endif

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
    if(!arena2BindTrackedEntry(newData, scan))
        texPoolReportLine(true, "[GX-POOL-FAULT] cannot bind streamed shrink replacement gx=%p entry=%p\n",
                          newData, (void*)scan);
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
    logShrinkCheck("RGBA8-stream", scan, raster, natras,
                   oldData, oldSize, oldW, oldH, GX_TF_RGBA8,
                   halfLinear, newW, newH);
    freeCpuTemp(halfLinear);
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
hasCriticalWeaponTextureNameHint(const char *name)
{
    if(!name || !name[0])
        return false;

    // These are the HUD weapon-sprite names from Hud.cpp. Keep their masks and
    // icons at native resolution; shrinking them fixes a world upload at the
    // cost of a visibly degraded weapon selector.
    char base[32];
    const char *candidate = name;
    size_t len = strlen(name);
    if(len > 1 && len < sizeof(base) &&
       (name[len - 1] == 'A' || name[len - 1] == 'a' ||
        name[len - 1] == 'M' || name[len - 1] == 'm')) {
        memcpy(base, name, len - 1);
        base[len - 1] = '\0';
        candidate = base;
    }

    return nameEqualsNoCase(candidate, "fist") ||
           nameEqualsNoCase(candidate, "brassk") ||
           nameEqualsNoCase(candidate, "screw") ||
           nameEqualsNoCase(candidate, "golf") ||
           nameEqualsNoCase(candidate, "nightstick") ||
           nameEqualsNoCase(candidate, "knife") ||
           nameEqualsNoCase(candidate, "bat") ||
           nameEqualsNoCase(candidate, "hammer") ||
           nameEqualsNoCase(candidate, "cleaver") ||
           nameEqualsNoCase(candidate, "machete") ||
           nameEqualsNoCase(candidate, "sword") ||
           nameEqualsNoCase(candidate, "chainsaw") ||
           nameEqualsNoCase(candidate, "grenade") ||
           nameEqualsNoCase(candidate, "teargas") ||
           nameEqualsNoCase(candidate, "molotov") ||
           nameEqualsNoCase(candidate, "rocket") ||
           nameEqualsNoCase(candidate, "handgun1") ||
           nameEqualsNoCase(candidate, "python") ||
           nameEqualsNoCase(candidate, "chromegun") ||
           nameEqualsNoCase(candidate, "spasshotgun") ||
           nameEqualsNoCase(candidate, "stubshotgun") ||
           nameEqualsNoCase(candidate, "tec9") ||
           nameEqualsNoCase(candidate, "uzi1") ||
           nameEqualsNoCase(candidate, "uzi2") ||
           nameEqualsNoCase(candidate, "mp5") ||
           nameEqualsNoCase(candidate, "m4") ||
           nameEqualsNoCase(candidate, "ruger") ||
           nameEqualsNoCase(candidate, "sniper") ||
           nameEqualsNoCase(candidate, "laserscope") ||
           nameEqualsNoCase(candidate, "flamer") ||
           nameEqualsNoCase(candidate, "m60") ||
           nameEqualsNoCase(candidate, "minigun") ||
           nameEqualsNoCase(candidate, "bomb") ||
           nameEqualsNoCase(candidate, "camera");
}

static bool
hasCriticalUiTextureNameHint(const char *name)
{
    if(!name || !name[0])
        return false;

    // PLAYER.TXD through PLAYER9.TXD contain player, player2, ... player9.
    // Treat each playable-character texture as persistent so emergency world
    // reclamation never turns the character into the visibly degraded fallback.
    return nameStartsWithNoCase(name, "player") ||
           hasCriticalWeaponTextureNameHint(name) ||
           nameContainsNoCase(name, "font") ||
           nameContainsNoCase(name, "fonts") ||
           nameContainsNoCase(name, "vcchs") ||
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

#ifndef WII
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
#endif

static void
logTextureShrink(const char *kind, const GxTexPoolEntry *entry,
                 int oldW, int oldH, uint32 oldSize,
                 int newW, int newH, uint32 newSize)
{
#ifdef WII
    (void)kind;
    (void)entry;
    (void)oldW;
    (void)oldH;
    (void)oldSize;
    (void)newW;
    (void)newH;
    (void)newSize;
    // The fault-only shrink validation below carries both mutation and validation state.
#else
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
#endif
}

struct GxShrinkPayloadStats
{
    uint32 payloadHash;
    uint32 texelCount;
    uint8 minR, minG, minB, minA;
    uint8 maxR, maxG, maxB, maxA;
    uint32 alphaZeroCount;
    uint32 alphaMidCount;
    uint32 alphaFullCount;
};

static GxShrinkPayloadStats
sampleLinearRgbaStats(const uint8 *pixels, int width, int height)
{
    GxShrinkPayloadStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.minR = stats.minG = stats.minB = stats.minA = 255;

    if(pixels == nil || width <= 0 || height <= 0)
        return stats;

    const uint32 totalTexels = (uint32)width * (uint32)height;
    const uint32 maxSamples = 4096u;
    uint32 step = totalTexels > maxSamples ? (totalTexels / maxSamples) : 1u;
    if(step == 0)
        step = 1u;

    uint32 hash = 2166136261u;
    for(uint32 i = 0; i < totalTexels; i += step) {
        const uint8 *p = pixels + (size_t)i * 4u;
        uint8 r = p[0];
        uint8 g = p[1];
        uint8 b = p[2];
        uint8 a = p[3];

        if(r < stats.minR) stats.minR = r;
        if(g < stats.minG) stats.minG = g;
        if(b < stats.minB) stats.minB = b;
        if(a < stats.minA) stats.minA = a;
        if(r > stats.maxR) stats.maxR = r;
        if(g > stats.maxG) stats.maxG = g;
        if(b > stats.maxB) stats.maxB = b;
        if(a > stats.maxA) stats.maxA = a;

        if(a == 0)
            stats.alphaZeroCount++;
        else if(a == 255)
            stats.alphaFullCount++;
        else
            stats.alphaMidCount++;

        hash ^= r; hash *= 16777619u;
        hash ^= g; hash *= 16777619u;
        hash ^= b; hash *= 16777619u;
        hash ^= a; hash *= 16777619u;
        stats.texelCount++;
    }
    stats.payloadHash = hash;
    return stats;
}

static void
logShrinkCheck(const char *kind, const GxTexPoolEntry *entry,
               Raster *raster, GxRaster *natras,
               void *oldGxData, uint32 oldSize, int oldW, int oldH, uint8 oldFmt,
               const uint8 *samplePixels, int sampleW, int sampleH)
{
    static int s_shrinkCheckCount = 0;
    if(entry == nil || raster == nil || natras == nil)
        return;
    if(s_shrinkCheckCount >= 128)
        return;

    const void *texObjData = natras->texObjValid ?
        GX_GetTexObjData(&natras->texObj) : nil;
    const uintptr_t expectedTexObjData = natras->gxData ?
        (uintptr_t)MEM_VIRTUAL_TO_PHYSICAL(natras->gxData) : 0u;
    const bool valid = natras->texObjValid &&
        (uintptr_t)texObjData == expectedTexObjData &&
        natras->gxData == entry->gxData && natras->dataSize == entry->size &&
        (unsigned)GX_GetTexObjWidth(&natras->texObj) == (unsigned)natras->w &&
        (unsigned)GX_GetTexObjHeight(&natras->texObj) == (unsigned)natras->h &&
        (unsigned)GX_GetTexObjFmt(&natras->texObj) == (unsigned)natras->gxFmt;
    if(valid)
        return;

    const GxShrinkPayloadStats stats =
        sampleLinearRgbaStats(samplePixels, sampleW, sampleH);
    fprintf(stdout,
           "[GX-SHRINK-FAULT] kind=%s gen=%u shrink=%u name=%s raster=%p entry=%p oldGx=%p newGx=%p objGx=%p objMatch=%d old=%dx%d/%u/%u new=%ux%u/%u/%u obj=%ux%u/%u hasA=%u aKind=%u texObj=%u link=%d sizeMatch=%d hash=%08X sample=%u rgba=[%u,%u,%u,%u]-[%u,%u,%u,%u] alpha(z/m/f)=%u/%u/%u\n",
           kind ? kind : "<none>",
           gArena2CompactionGeneration,
           (unsigned)gShrinkTotalCount,
           entry->name[0] ? entry->name : "<unknown>",
           (void*)raster,
           (void*)entry,
           oldGxData,
           natras->gxData,
           texObjData,
           ((uintptr_t)texObjData == expectedTexObjData) ? 1 : 0,
           oldW, oldH, (unsigned)oldFmt, oldSize,
           (unsigned)natras->w, (unsigned)natras->h,
           (unsigned)natras->gxFmt, (unsigned)natras->dataSize,
           natras->texObjValid ? (unsigned)GX_GetTexObjWidth(&natras->texObj) : 0u,
           natras->texObjValid ? (unsigned)GX_GetTexObjHeight(&natras->texObj) : 0u,
           natras->texObjValid ? (unsigned)GX_GetTexObjFmt(&natras->texObj) : 0xFFu,
           (unsigned)natras->hasAlpha,
           (unsigned)natras->alphaKind,
           natras->texObjValid ? 1u : 0u,
           (natras->gxData == entry->gxData) ? 1 : 0,
           (natras->dataSize == entry->size) ? 1 : 0,
           stats.payloadHash,
           stats.texelCount,
           (unsigned)stats.minR, (unsigned)stats.minG,
           (unsigned)stats.minB, (unsigned)stats.minA,
           (unsigned)stats.maxR, (unsigned)stats.maxG,
           (unsigned)stats.maxB, (unsigned)stats.maxA,
           stats.alphaZeroCount,
           stats.alphaMidCount,
           stats.alphaFullCount);
    s_shrinkCheckCount++;
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
hasPersistentUiTextureUsage(const GxTexPoolEntry *entry)
{
    if(!entry || !entry->raster)
        return false;

    const GxRaster *natras = PLUGINOFFSET(GxRaster, entry->raster,
                                           nativeRasterOffset);
    return natras != nil &&
           natras->usageClass == GX_TEXTURE_USAGE_PERSISTENT_UI;
}

static bool
isBudgetProtectedTexture(const GxTexPoolEntry *entry)
{
    if(!entry)
        return true;

    // This comes from the owning subsystem's TXD-load scope, not from a
    // texture-name heuristic. A weapon/HUD atlas must never become the
    // allocator's last-resort quality sacrifice.
    if(hasPersistentUiTextureUsage(entry))
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
	bool fixedIslandSplash = nameEqualsNoCase(entry->name, "splash1") ||
	                         nameEqualsNoCase(entry->name, "splash2");

	return fixedIslandSplash ||
	       persistentUi ||
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
#ifdef WII
    if(tag == nullptr || tag[0] == '\0')
        return false;

    // Generic MEM2 is shared with geometry, audio, and streaming. Only let
    // explicitly critical texture classes spill there, and only while the
    // corresponding protected upload window is active.
    if(gCriticalUiUploadDepth <= 0)
        return false;

    return hasCriticalUiTextureNameHint(tag) ||
           hasCriticalWaterTextureNameHint(tag) ||
           hasCriticalRoadTextureNameHint(tag);
#else
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
#endif
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

static uint32
rgb565TiledSizePadded(int w, int h)
{
    int tilesWide = (w + 3) / 4;
    int tilesHigh = (h + 3) / 4;
    return (uint32)(tilesWide * tilesHigh * 32);
}

static void
convertGX_RGBA8ToOpaqueRGB565(void *dst, const void *src, int w, int h)
{
    uint8 *out = (uint8*)dst;
    const uint8 *in = (const uint8*)src;

    for(int by = 0; by < h; by += 4) {
        for(int bx = 0; bx < w; bx += 4) {
            const uint8 *ar = in;
            const uint8 *gb = in + 32;
            for(int iy = 0; iy < 4; iy++) {
                for(int ix = 0; ix < 4; ix++) {
                    uint8 r = ar[iy * 8 + ix * 2 + 1];
                    uint8 g = gb[iy * 8 + ix * 2 + 0];
                    uint8 b = gb[iy * 8 + ix * 2 + 1];
                    uint16 packed = (uint16)(((uint16)(r >> 3) << 11) |
                                             ((uint16)(g >> 2) << 5) |
                                             (uint16)(b >> 3));
                    *out++ = (uint8)(packed >> 8);
                    *out++ = (uint8)packed;
                }
            }
            in += 64;
        }
    }
}

static uint32
getSafeAllocTargetBudget(size_t size, uint32 safetyBytes)
{
    uint64 reclaimBytes = (uint64)size + (uint64)safetyBytes;
    uint32 targetBudget = gTexPoolTotalSize;

    // A direct allocation failure is a request-sized pressure event. Reclaim
    // enough for this block plus a small fragmentation cushion; do not reuse
    // the old multi-megabyte load headroom here, because that permanently
    // downsamples unrelated resident textures.
    if(reclaimBytes < (uint64)targetBudget)
        targetBudget -= (uint32)reclaimBytes;
    else
        targetBudget = GX_TEXP_MIN_SOFT_BUDGET_BYTES;

    return clampSoftBudgetToRuntimePool(targetBudget);
}

static int
tightenBudgetForSafeAlloc(size_t size)
{
    if(gCriticalUiUploadDepth > 0) {
        uint32 oldBudget = gSoftBudgetBytes;
        uint32 newBudget;
#ifdef WII
        uint32 targetBudget = getSafeAllocTargetBudget(
            size, 512u * 1024u);
#else
        uint32 targetBudget = size >= 256u * 1024u ?
            18u * 1024u * 1024u :
            19u * 1024u * 1024u;
#endif

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

#ifdef WII
        if(gTexPoolTotalSize > gSoftBudgetBytes)
            GX_DrawDone();
#endif
        int shrinkCount = texPoolEnforceBudgetImmediateSteps(
            "critical-ui-upload", MAX_SAFE_ALLOC_SHRINKS);
        gSoftBudgetBytes = oldBudget;
        return shrinkCount;
    }

    uint32 oldBudget = gSoftBudgetBytes;
#ifdef WII
    uint32 floorBytes = getSafeAllocTargetBudget(
        size, 256u * 1024u);
#else
    uint32 floorBytes = size >= 256u * 1024u ?
        24u * 1024u * 1024u :
        25u * 1024u * 1024u;
#endif
    uint32 newBudget = floorBytes;

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

    // A direct GX allocation failure is already a hard allocation boundary.
    // The normal Wii budget path permits only one shrink per frame, which can
    // leave a 256 KiB upload retrying while the pool remains fragmented.
#ifdef WII
    // Shrinking replaces live GX payloads; fence before the emergency pass so
    // the allocator never frees storage still referenced by the command FIFO.
    if(gTexPoolTotalSize > gSoftBudgetBytes)
        GX_DrawDone();
#endif
    int shrinkCount = texPoolEnforceBudgetImmediateSteps(
        "safeGxAlloc", MAX_SAFE_ALLOC_SHRINKS);
    gSoftBudgetBytes = oldBudget;
    return shrinkCount;
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

static bool
demoteOpaqueRGBA8ToRGB565(GxTexPoolEntry **entryLink,
                          GxTexPoolEntry *entry,
                          Raster *raster,
                          GxRaster *natras)
{
#if defined(WII_GX_OPAQUE_RGBA8_RGB565_TIER) && WII_GX_OPAQUE_RGBA8_RGB565_TIER
    if(entryLink == nil || entry == nil || raster == nil || natras == nil ||
       raster->type != Raster::TEXTURE || natras->cpuData != nil ||
       entry->gxFmt != GX_TF_RGBA8 || natras->gxFmt != GX_TF_RGBA8 ||
       natras->hasAlpha != 0 || natras->alphaKind != GX_RASTER_ALPHA_NONE ||
       natras->gxData != entry->gxData)
        return false;

    uint32 newSize = rgb565TiledSizePadded(entry->width, entry->height);
    uint32 oldSize = entry->size;
    if(newSize == 0 || newSize >= oldSize)
        return false;

#ifdef WII
    static uint32 s_qualityTierFenceFrame = UINT32_MAX;
    if(s_qualityTierFenceFrame != gxFrameNum) {
        GX_DrawDone();
        s_qualityTierFenceFrame = gxFrameNum;
    }
#endif

    void *oldData = entry->gxData;
#ifdef WII
    unsigned int oldPoolBit = texPoolStoragePoolBit(oldData);
#endif
    void *newData = gxMemAlloc(newSize, 32);
    bool oldDataReleased = false;
    if(newData) {
        convertGX_RGBA8ToOpaqueRGB565(newData, oldData,
                                      entry->width, entry->height);
    } else {
        // A fragmented GX arena may have enough total free space but no
        // contiguous block for the replacement. Stage the already-demoted
        // payload in generic MEM2, release the old GX block, then retry. The
        // staged block remains a valid GPU-visible fallback if the retry ever
        // fails, so this replacement is transactional and cannot leave a
        // dangling raster.
        void *stagedData = allocCpuTemp(newSize, 32);
        if(!stagedData)
            return false;
        convertGX_RGBA8ToOpaqueRGB565(stagedData, oldData,
                                      entry->width, entry->height);
        gxMemFree(oldData);
        oldDataReleased = true;
        newData = gxMemAlloc(newSize, 32);
        if(newData) {
            memcpy(newData, stagedData, newSize);
            freeCpuTemp(stagedData);
        } else {
            newData = stagedData;
        }
    }

    *entryLink = entry->next;
    if(!oldDataReleased)
        gxMemFree(oldData);
#ifdef WII
    if(oldPoolBit != 0)
        WiiMemoryRecordResidentDelta(entry->ownerStreamId, oldPoolBit,
                                     -(int)oldSize);
    texPoolRecordOwnerDelta(entry->ownerStreamId, newData, (int)newSize);
#endif

    natras->gxData = newData;
    natras->dataSize = newSize;
    natras->gxFmt = GX_TF_RGB565;

    invalidateTextureBinding(raster);
    DCFlushRange(newData, newSize);
    GX_InvalidateTexAll();
    GX_InitTexObj(&natras->texObj, newData,
                  (u16)entry->width, (u16)entry->height,
                  GX_TF_RGB565,
                  natras->wrapS, natras->wrapT,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&natras->texObj,
                            natras->minFilter, natras->magFilter);
    natras->texObjValid = true;

    gTexPoolTotalSize -= oldSize;
    entry->gxData = newData;
    if(!arena2BindTrackedEntry(newData, entry))
        texPoolReportLine(true,
                          "[GX-POOL-FAULT] cannot bind RGB565 quality-tier replacement gx=%p entry=%p\n",
                          newData, (void*)entry);
    entry->size = newSize;
    entry->gxFmt = GX_TF_RGB565;
    entry->shrinkCount++;
    insertPoolEntrySorted(entry);
    gTexPoolTotalSize += newSize;
    gShrinkTotalCount++;

    logTextureShrink("RGBA8->RGB565", entry,
                     entry->width, entry->height, oldSize,
                     entry->width, entry->height, newSize);
    logShrinkCheck("RGBA8->RGB565", entry, raster, natras,
                   oldData, oldSize,
                   entry->width, entry->height, GX_TF_RGBA8,
                   nil, 0, 0);
#ifdef WII
    static int s_qualityTierLogCount = 0;
    if(s_qualityTierLogCount < 96) {
        SYS_Report("[WII-GX-QUALITY] opaque-rgb565 name='%s' wh=%ux%u bytes=%u->%u freed=%u storage=%s\n",
                   entry->name[0] ? entry->name : "<unnamed>",
                   (unsigned)entry->width, (unsigned)entry->height,
                   (unsigned)oldSize, (unsigned)newSize,
                   (unsigned)(oldSize - newSize),
                   gxMemOwns(newData) ? "gx" : "generic");
        s_qualityTierLogCount++;
    }
#endif
    return true;
#else
    (void)entryLink;
    (void)entry;
    (void)raster;
    (void)natras;
    return false;
#endif
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

            // Durable UI protection survives the final fallback scan. Name
            // heuristics below remain soft compatibility protection, but a
            // texture uploaded by a persistent UI owner is never shrunk.
            if(scan->raster == gActiveHudWeaponRaster ||
               hasPersistentUiTextureUsage(scan) ||
               (!allowProtected && isBudgetProtectedTexture(scan))) {
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

            GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
#if defined(WII_GX_OPAQUE_RGBA8_RGB565_TIER) && WII_GX_OPAQUE_RGBA8_RGB565_TIER
            if(scan->gxFmt == GX_TF_RGBA8 && natras != nil &&
               natras->hasAlpha == 0 &&
               natras->alphaKind == GX_RASTER_ALPHA_NONE) {
                if(demoteOpaqueRGBA8ToRGB565(scanPrev, scan, raster, natras))
                    return true;

                // This profile treats an opaque RGBA8 texture's full spatial
                // resolution as the quality floor. If the same-size format
                // tier cannot be allocated, try another resource instead of
                // immediately dropping 75 percent of its texels.
                scanPrev = &scan->next;
                scan = scan->next;
                continue;
            }
#endif

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

                void *oldData = scan->gxData;
                uint32 oldSize = scan->size;

                shrinkCMPRByBlockDrop((uint8*)newData, (const uint8*)oldData,
                                      oldW, newW, newH);

                *scanPrev = scan->next;
                gxMemFree(oldData);
#ifdef WII
                texPoolRecordOwnerDelta(scan->ownerStreamId, oldData, -(int)oldSize);
                texPoolRecordOwnerDelta(scan->ownerStreamId, newData, (int)newSize);
#endif

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
                if(!arena2BindTrackedEntry(newData, scan))
                    texPoolReportLine(true, "[GX-POOL-FAULT] cannot bind CMPR shrink replacement gx=%p entry=%p\n",
                                      newData, (void*)scan);
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
                logShrinkCheck("CMPR", scan, raster, natras,
                               oldData, oldSize, oldW, oldH, GX_TF_CMPR,
                               nil, 0, 0);

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
#ifdef WII
            texPoolRecordOwnerDelta(scan->ownerStreamId, oldData, -(int)oldSize);
            texPoolRecordOwnerDelta(scan->ownerStreamId, newData, (int)newSize);
#endif

            convertRGBA8_to_GX(newData, halfLinear, newW, newH, newW * 4);

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
            if(!arena2BindTrackedEntry(newData, scan))
                texPoolReportLine(true, "[GX-POOL-FAULT] cannot bind RGBA8 shrink replacement gx=%p entry=%p\n",
                                  newData, (void*)scan);
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
            logShrinkCheck("RGBA8", scan, raster, natras,
                           oldData, oldSize, oldW, oldH, GX_TF_RGBA8,
                           halfLinear, newW, newH);
            freeCpuTemp(halfLinear);

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
pushPersistentUiTextureUploadContext(const char *reason)
{
    (void)reason;
    gPersistentUiTextureUploadDepth++;
}

void
popPersistentUiTextureUploadContext(const char *reason)
{
    (void)reason;
    if(gPersistentUiTextureUploadDepth > 0)
        gPersistentUiTextureUploadDepth--;
}

uint8
currentTextureUsageClass(void)
{
    return gPersistentUiTextureUploadDepth > 0 ?
        GX_TEXTURE_USAGE_PERSISTENT_UI : GX_TEXTURE_USAGE_DEFAULT;
}

void
markPersistentUiTexture(Raster *raster)
{
    if(!raster || raster->platform != PLATFORM_GX)
        return;

    GxRaster *natras = PLUGINOFFSET(GxRaster, raster, nativeRasterOffset);
    if(natras)
        natras->usageClass = GX_TEXTURE_USAGE_PERSISTENT_UI;
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
    uint8 *linear = (uint8*)allocCpuTemp((size_t)oldW * oldH * 4, 32);
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
        freeCpuTemp(linear);
        insertPoolEntrySorted(entry);
        return false;
    }

    // Step 2: Downsample
    uint8 *halfLinear = (uint8*)allocCpuTemp((size_t)newW * newH * 4, 32);
    if(!halfLinear) {
        freeCpuTemp(linear);
        insertPoolEntrySorted(entry);
        return false;
    }

    downsample2x(linear, oldW, oldH, halfLinear, newW, newH);
    freeCpuTemp(linear);

    // Step 3: Allocate replacement before touching the live texture.
    uint32 newSize = rgba8TiledSize(newW, newH);
    void *newData = gxMemAlloc(newSize, 32);
    if(!newData) {
        freeCpuTemp(halfLinear);
        insertPoolEntrySorted(entry);
        return false;
    }

    // Step 4: Replace old gxData only after the new allocation succeeded.
    gxMemFree(oldData);

    convertRGBA8_to_GX(newData, halfLinear, newW, newH, newW * 4);
    freeCpuTemp(halfLinear);

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
    if(!arena2BindTrackedEntry(newData, entry))
        texPoolReportLine(true, "[GX-POOL-FAULT] cannot bind host shrink replacement gx=%p entry=%p\n",
                          newData, (void*)entry);
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

static int
shrinkTexturePoolToBudget(int maxSteps)
{
    int steps = 0;
    while(steps < maxSteps && gTexPoolTotalSize > gSoftBudgetBytes) {
        gBudgetShrinkPass = true;
        bool shrunk = shrinkLargestTexture();
        gBudgetShrinkPass = false;
        if(!shrunk) {
            printf("[GX-TEXP] budget: no more shrinkable textures, pool=%uKB target=%uKB\n",
                   gTexPoolTotalSize / 1024, gSoftBudgetBytes / 1024);
            texPoolDebug();
            break;
        }
        steps++;
    }
    return steps;
}

void
texPoolEnforceBudget(const char *reason)
{
#ifdef WII
    (void)reason;
#endif
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

#ifdef WII
    if(gxFrameNum == 0) {
        shrinkTexturePoolToBudget(1);
        return;
    }
    if(gBudgetShrinkFrame != gxFrameNum) {
        gBudgetShrinkFrame = gxFrameNum;
        gBudgetShrinksThisFrame = 0;
    }
    if(gBudgetShrinksThisFrame >= 1)
        return;
    gBudgetShrinksThisFrame = 1;
    shrinkTexturePoolToBudget(1);
#else
    shrinkTexturePoolToBudget(GX_TEX_BUDGET_STEPS);
#endif
}

static int
texPoolEnforceBudgetImmediateSteps(const char *reason, int maxSteps)
{
    (void)reason;
    if(gTexPoolTotalSize <= gSoftBudgetBytes || maxSteps <= 0)
        return 0;
    if(maxSteps > GX_TEX_BUDGET_STEPS)
        maxSteps = GX_TEX_BUDGET_STEPS;
    int steps = shrinkTexturePoolToBudget(maxSteps);
#ifdef WII
    gBudgetShrinkFrame = gxFrameNum;
    gBudgetShrinksThisFrame = 1;
#endif
    return steps;
}

void
texPoolEnforceBudgetImmediate(const char *reason, int maxSteps)
{
    (void)texPoolEnforceBudgetImmediateSteps(reason, maxSteps);
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

#ifdef WII
    return nullptr;
#else
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
#endif
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
    // safeGxAlloc's MEM2 fallback uses the aligned wrapper, not the raw
    // allocator. Recover its original pointer before returning the block.
    MemoryMgrFreeAlignMem2(ptr);
#else
    free(ptr);
#endif
}

bool
gxMemHasPendingCompaction(void)
{
    return gArena2CompactionPending;
}

bool
gxMemCompact(const char *reason, bool force)
{
    return arena2CompactInternal(reason, force, false, 0);
}

bool
gxMemRunPendingCompactionAtGpuIdle(const char *reason)
{
    if(!gArena2CompactionPending)
        return false;
    return arena2CompactInternal(reason, false, true, gArena2PendingCompactBytes);
}

void*
safeGxAlloc(size_t size, size_t alignment, const char *tag)
{
    void *ptr = arena2Alloc(size, alignment);
    if(ptr)
        return ptr;

    printf("[GX-TEXP] safeGxAlloc(%u) GX pool direct FAIL - evaluating compaction/shrink (tag='%s')\n",
           (unsigned)size, tag ? tag : "<none>");

    size_t requiredBlockBytes = arena2RequiredBlockSize(size, alignment);
    size_t totalFree = 0;
    size_t largestFree = 0;
    arena2GetFreeStats(&totalFree, &largestFree);
    size_t potentialTotalFree = totalFree + arena2EmergencyReserveTotalSize();
    if(arena2ShouldDeferCompaction(requiredBlockBytes, potentialTotalFree, largestFree)) {
        markArena2CompactionPending(requiredBlockBytes, potentialTotalFree, largestFree,
                                    tag ? tag : "safeGxAlloc");
#ifdef WII
        // Fragmentation is recoverable without destroying texture quality.
        // Draining GX here turns this allocation boundary into a safe move point.
        arena2CompactInternal(tag ? tag : "safeGxAlloc", false, false,
                              requiredBlockBytes);
        ptr = arena2Alloc(size, alignment);
        if(ptr) {
            texPoolReportLine(true,
                              "[GX-POOL] allocation recovered by compaction size=%uKB tag='%s'\n",
                              (unsigned)(size / 1024u), tag ? tag : "<none>");
            return ptr;
        }
#endif
    }

    // Only sacrifice texture resolution after proving that compaction cannot
    // recover the request from existing free space plus the releasable shrink
    // reserve. Fragmentation is a layout problem, not a capacity problem, and
    // must not create permanent quality loss by itself.
    int budgetShrinkCount = tightenBudgetForSafeAlloc(size);

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

    int remainingShrinkAttempts = MAX_SHRINK_ATTEMPTS;
    if(budgetShrinkCount < MAX_SAFE_ALLOC_SHRINKS) {
        int budgetRemaining = MAX_SAFE_ALLOC_SHRINKS - budgetShrinkCount;
        if(remainingShrinkAttempts > budgetRemaining)
            remainingShrinkAttempts = budgetRemaining;
    } else {
        remainingShrinkAttempts = 0;
    }
    for(int attempt = 1; attempt <= remainingShrinkAttempts; attempt++) {
        if(attempt == 1 || attempt == 2 || attempt == 4 ||
           attempt == remainingShrinkAttempts) {
            printf("[GX-TEXP] safeGxAlloc attempt=%d/%d tag='%s' pool=%uKB tex=%d fallback=%u\n",
                   attempt, remainingShrinkAttempts,
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

    bool allowLargeFallback = allowLargeMemFallbackForTag(tag);
#ifdef WII
    // Preserve a reserve for non-texture users of the generic MEM2 pool.
    bool genericMem2Healthy = (WiiMemoryGetStreamingPressure() & 1u) == 0;
#else
    bool genericMem2Healthy = true;
#endif
    if(genericMem2Healthy && (size <= 320u * 1024u ||
#ifdef WII
       (allowLargeFallback && size <= 512u * 1024u)
#else
       allowLargeFallback
#endif
    )) {
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
#ifdef WII
    SYS_Report("[WII-MEM] GX allocation OOM size=%u tag='%s' pool=%uKB/%d tex\n",
               (unsigned)size, tag ? tag : "<none>",
               gTexPoolTotalSize / 1024u, gTexPoolCount);
    WiiMemoryDumpStats("gx allocation OOM");
#else
    texPoolDebug();
#endif
    return nullptr;
}

// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ
// Diagnostics
// ââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââââ

void
texPoolDebug(void)
{
    texPoolEmitSummaryLines(false, "debug");
}

void
texPoolResidencyReport(const char *reason)
{
    if(gTexPoolResidencyReporting)
        return;

    gTexPoolResidencyReporting = true;
#ifdef WII
    texPoolEmitSummaryLines(true, reason ? reason : "snapshot");
#else
    texPoolEmitSummaryLines(false, reason ? reason : "snapshot");
#endif
    gTexPoolResidencyReporting = false;
}

void
texPoolGetOwnerStats(uint32 *ownedGenericBytes, uint32 *unknownGenericBytes,
                     uint32 *ownedGxBytes, uint32 *unknownGxBytes)
{
    if(ownedGenericBytes) *ownedGenericBytes = 0;
    if(unknownGenericBytes) *unknownGenericBytes = 0;
    if(ownedGxBytes) *ownedGxBytes = 0;
    if(unknownGxBytes) *unknownGxBytes = 0;

#ifdef WII
    for(const GxTexPoolEntry *entry = gTexPool; entry; entry = entry->next) {
        unsigned int poolBit = texPoolStoragePoolBit(entry->gxData);
        bool unknownOwner = entry->ownerStreamId == WII_TEXPOOL_OWNER_UNKNOWN;
        if(poolBit == 1u) {
            if(unknownOwner && unknownGenericBytes)
                *unknownGenericBytes += entry->size;
            else if(!unknownOwner && ownedGenericBytes)
                *ownedGenericBytes += entry->size;
        } else if(poolBit == 4u) {
            if(unknownOwner && unknownGxBytes)
                *unknownGxBytes += entry->size;
            else if(!unknownOwner && ownedGxBytes)
                *ownedGxBytes += entry->size;
        }
    }
#endif
}

void
texPoolVisitOwnerResidency(TexPoolOwnerResidencyCallback callback)
{
    if(!callback)
        return;

#ifdef WII
    for(const GxTexPoolEntry *entry = gTexPool; entry; entry = entry->next) {
        if(entry->ownerStreamId == WII_TEXPOOL_OWNER_UNKNOWN)
            continue;
        unsigned int poolBit = texPoolStoragePoolBit(entry->gxData);
        if(poolBit != 0)
            callback(entry->ownerStreamId, poolBit, entry->size);
    }
#else
    (void)callback;
#endif
}

uint32
texPoolTotalBytes(void)
{
    return gTexPoolTotalSize;
}

uint32
gxMemGetShrinkTotalCount(void)
{
    return (uint32)gShrinkTotalCount;
}

uint32
gxMemGetCompactionGeneration(void)
{
    return gArena2CompactionGeneration;
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

void
texPoolResetSoftBudget(void)
{
    texPoolSetSoftBudget(GX_TEXP_DEFAULT_SOFT_BUDGET_BYTES);
}

uint32
texPoolGetSoftBudget(void)
{
    return gSoftBudgetBytes;
}

} // namespace gx
} // namespace rw

#endif // GAMECUBE

