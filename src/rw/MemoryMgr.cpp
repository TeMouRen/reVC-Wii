#include "common.h"
#include "MemoryHeap.h"
#include "MemoryMgr.h"

#ifdef GAMECUBE
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include "gxmemory.h"
#endif

#ifdef WII
#include <malloc.h>

namespace rw { namespace gx {
void gxMemInitArena2Pool(void);
void *gxMemGetPoolBase(void);
void *gxMemGetPoolEnd(void);
void gxMemGetPoolStats(uint32 *capacityBytes, uint32 *usedBytes,
                       uint32 *peakBytes, uint32 *largestFreeBytes,
                       uint32 *allocFailCount, uint32 *fallbackCount);
} }

extern uint8 __Arena2Lo[];
extern uint8 __Arena2Hi[];

#ifndef WII_STREAM_MEMORY_DIAGNOSTICS
#define WII_STREAM_MEMORY_DIAGNOSTICS 0
#endif
#ifndef WII_MEMORY_PROFILE_ID
#define WII_MEMORY_PROFILE_ID "unknown"
#endif
#ifndef WII_BUILD_ID
#define WII_BUILD_ID "unknown"
#endif
#ifndef WII_MEM2_RESOURCE_POOL_BYTES
#define WII_MEM2_RESOURCE_POOL_BYTES (18u * 1024u * 1024u)
#endif
#ifndef WII_MEM2_RECLAIMED_BYTES
#define WII_MEM2_RECLAIMED_BYTES 0u
#endif
#ifndef WII_AUDIO_DECODE_ENABLE
#define WII_AUDIO_DECODE_ENABLE 1
#endif
#ifndef WII_AUDIO_BASELINE_REQUESTED_BYTES
#define WII_AUDIO_BASELINE_REQUESTED_BYTES 14177248u
#endif
#ifndef WII_AUDIO_MEM2_POOL_BYTES
#define WII_AUDIO_MEM2_POOL_BYTES 0u
#endif

// Define WII_MEMORY_DIAGNOSTICS when allocator tracing is required.
#ifndef WII_MEMORY_DIAGNOSTICS
#define printf(...) ((void)sizeof((::printf)(__VA_ARGS__)))
#endif
#endif

#ifdef WII
extern "C" {
u32 MALLOC_MEM2 = 0;
}

namespace
{
// Keep audio and short-lived conversion scratch isolated from newlib and GX.
#if WII_MEM2_RESOURCE_POOL_BYTES != 6291456 && WII_MEM2_RESOURCE_POOL_BYTES != 8388608 && WII_MEM2_RESOURCE_POOL_BYTES != 18874368
#error "WII_MEM2_RESOURCE_POOL_BYTES must be 6291456, 8388608, or 18874368"
#endif
static const u32 MEM2_RESOURCE_POOL_TARGET_BYTES = WII_MEM2_RESOURCE_POOL_BYTES;
static const u32 MEM2_RESOURCE_POOL_MIN_BYTES    =
	WII_MEM2_RESOURCE_POOL_BYTES < 12u * 1024u * 1024u ?
	WII_MEM2_RESOURCE_POOL_BYTES : 12u * 1024u * 1024u;
static const u32 MEM2_RESOURCE_ALLOC_MAGIC       = 0x4D32474Eu; // M2GN
static const u32 MEM2_AUDIO_ALLOC_MAGIC          = 0x4D324155u; // M2AU
static const u32 MEM2_AUDIO_POOL_TARGET_BYTES    = WII_AUDIO_MEM2_POOL_BYTES;
static const size_t WII_STREAM_GENERIC_RESERVE_BYTES = 2u * 1024u * 1024u;
static const size_t WII_STREAM_GENERIC_LARGEST_BYTES = 512u * 1024u;
static const size_t WII_STREAM_NEWLIB_RECLAIM_BYTES  = 512u * 1024u;
static const size_t WII_STREAM_GX_RESERVE_BYTES      = 2u * 1024u * 1024u;
static const size_t WII_STREAM_GX_LARGEST_BYTES      = 512u * 1024u;
static const size_t WII_STREAM_REQUEST_FIT_CAP_BYTES = 1u * 1024u * 1024u;
static const size_t WII_FILE_OPEN_RESERVE_BYTES      = 128u * 1024u;

struct Mem2FreeBlock
{
	Mem2FreeBlock *prev;
	Mem2FreeBlock *next;
	size_t size;
};

struct Mem2AllocHeader
{
	u32 magic;
	u32 reserved;
	size_t totalSize;
	size_t userSize;
};

struct Mem2Pool
{
	const char *logTag;
	const char *reportName;
	u32 allocMagic;
	bool initAttempted;
	bool ready;
	uint8 *base;
	uint8 *end;
	size_t size;
	Mem2FreeBlock *freeList;
	uint32 allocCount;
	uint32 allocFailCount;
	size_t bytesUsed;
	size_t peakBytesUsed;
};

static Mem2Pool sGenericMem2Pool = {
	"MEM2-GEN", "generic", MEM2_RESOURCE_ALLOC_MAGIC,
	false, false, nullptr, nullptr, 0, nullptr, 0, 0, 0, 0
};
static Mem2Pool sAudioMem2Pool = {
	"MEM2-AUDIO", "audio", MEM2_AUDIO_ALLOC_MAGIC,
	false, false, nullptr, nullptr, 0, nullptr, 0, 0, 0, 0
};
static void *sFileOpenReserve = nil;
static bool sResourceAttributionActive = false;
static uint32 sResourceAttributionMask = 0;
static RwUInt16 sResourceAttributionOwner = WII_MEMORY_RESOURCE_OWNER_UNKNOWN;
static RwInt32 sResourceAttributionOwnerDelta[3];
static RwInt32 sResourceAttributionOwnerKiB[3];
static RwInt32 sResourceAttributionExternalDelta[3];
static WiiMemoryResidentDeltaCallback sResidentDeltaCallback = nil;
static uintptr sRuntimeArena2Base = 0;
static uintptr sRuntimeArena2End = 0;
#if WII_STREAM_MEMORY_DIAGNOSTICS
static uint64 sPhase0EpochTicks = 0;
static uint32 sPhase0Sequence = 0;
static bool sPhase0RunStarted = false;
#endif

static int
WiiMemoryResourcePoolIndex(RwUInt32 poolBit)
{
	switch(poolBit){
	case WII_STREAM_PRESSURE_GENERIC: return 0;
	case WII_STREAM_PRESSURE_NEWLIB: return 1;
	case WII_STREAM_PRESSURE_GX: return 2;
	default: return -1;
	}
}

static RwInt32
WiiMemoryAddResidentDelta(RwInt32 total, RwInt32 delta)
{
	int64 sum = (int64)total + delta;
	if(sum > INT32_MAX)
		return INT32_MAX;
	if(sum < INT32_MIN)
		return INT32_MIN;
	return (RwInt32)sum;
}

static RwInt32
WiiMemoryResidentKiBDelta(RwInt32 deltaBytes)
{
	if(deltaBytes == 0)
		return 0;

	int64 magnitude = deltaBytes > 0 ? deltaBytes : -(int64)deltaBytes;
	int64 kib = (magnitude + 1023) / 1024;
	if(kib > INT32_MAX)
		return deltaBytes > 0 ? INT32_MAX : INT32_MIN;
	return deltaBytes > 0 ? (RwInt32)kib : -(RwInt32)kib;
}

static uintptr
WiiMem2AlignUpPow2(uintptr value, size_t alignment)
{
	if(alignment <= 1)
		return value;
	uintptr mask = (uintptr)alignment - 1u;
	return (value + mask) & ~mask;
}

static uintptr
WiiMem2AlignDownPow2(uintptr value, size_t alignment)
{
	if(alignment <= 1)
		return value;
	uintptr mask = (uintptr)alignment - 1u;
	return value & ~mask;
}

static void
WiiMem2MergeForward(Mem2Pool *pool, Mem2FreeBlock *block)
{
	if(block == nil || block->next == nil)
		return;

	Mem2FreeBlock *next = block->next;
	if((uint8*)block + block->size != (uint8*)next)
		return;

	block->size += next->size;
	block->next = next->next;
	if(block->next)
		block->next->prev = block;
}

static void
WiiMem2InsertFreeBlock(Mem2Pool *pool, Mem2FreeBlock *block)
{
	block->prev = nil;
	block->next = nil;

	if(pool->freeList == nil) {
		pool->freeList = block;
		return;
	}

	Mem2FreeBlock *cur = pool->freeList;
	Mem2FreeBlock *prev = nil;
	while(cur && (uintptr)cur < (uintptr)block) {
		prev = cur;
		cur = cur->next;
	}

	block->prev = prev;
	block->next = cur;
	if(prev)
		prev->next = block;
	else
		pool->freeList = block;
	if(cur)
		cur->prev = block;

	WiiMem2MergeForward(pool, block);
	if(block->prev)
		WiiMem2MergeForward(pool, block->prev);
}

static void
WiiMem2GetFreeStats(const Mem2Pool *pool, size_t *totalFree, size_t *largestFree)
{
	size_t total = 0;
	size_t largest = 0;
	int steps = 0;
	for(Mem2FreeBlock *block = pool->freeList;
	    block && steps < 8192;
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
WiiMem2InitPool(Mem2Pool *pool, size_t targetBytes, size_t minBytes, size_t keepFreeBytes)
{
	if(pool->initAttempted)
		return;

	pool->initAttempted = true;
	if(targetBytes == 0)
		return;

	uintptr rawLo = (uintptr)SYS_GetArena2Lo();
	uintptr rawHi = (uintptr)SYS_GetArena2Hi();
	if(rawHi <= rawLo) {
		printf("[%s] Arena2 unavailable (lo=%p hi=%p)\n", pool->logTag,
		       (void*)rawLo, (void*)rawHi);
		return;
	}

	uintptr poolBase = WiiMem2AlignUpPow2(rawLo, 32);
	size_t available = (size_t)(rawHi - poolBase);
	size_t poolSize = targetBytes;

	if(keepFreeBytes > 0 && available > keepFreeBytes && poolSize > available - keepFreeBytes)
		poolSize = available - keepFreeBytes;
	if(poolSize < minBytes) {
		printf("[%s] not enough Arena2 for %s MEM2 pool (%u KB available)\n",
		       pool->logTag, pool->reportName,
		       (unsigned)(available / 1024u));
		return;
	}

	poolSize = (size_t)WiiMem2AlignDownPow2((uintptr)poolSize, 32);
	uintptr poolEnd = poolBase + poolSize;
	if(poolEnd > rawHi || poolEnd <= poolBase) {
		printf("[%s] reservation failed (%u KB requested, arena=%u KB)\n",
		       pool->logTag,
		       (unsigned)(poolSize / 1024u),
		       (unsigned)((rawHi - rawLo) / 1024u));
		return;
	}

	SYS_SetArena2Lo((void*)poolEnd);

	pool->base = (uint8*)poolBase;
	pool->end = (uint8*)poolEnd;
	pool->size = poolSize;
	pool->freeList = (Mem2FreeBlock*)pool->base;
	pool->freeList->prev = nil;
	pool->freeList->next = nil;
	pool->freeList->size = pool->size;
	pool->ready = true;

	printf("[%s] reserved %u KB %s MEM2 pool from Arena2 (arena=%u KB, base=%p end=%p, lo=%p)\n",
	       pool->logTag,
	       (unsigned)(pool->size / 1024u),
	       pool->reportName,
	       (unsigned)((rawHi - rawLo) / 1024u),
	       (void*)pool->base, (void*)pool->end, (void*)SYS_GetArena2Lo());
}

static void
InitMem2ResourcePool(void)
{
	WiiMem2InitPool(&sGenericMem2Pool, MEM2_RESOURCE_POOL_TARGET_BYTES,
	                MEM2_RESOURCE_POOL_MIN_BYTES, 8u * 1024u * 1024u);
}

static void
InitAudioMem2Pool(void)
{
	if(sAudioMem2Pool.initAttempted)
		return;
	WiiMem2InitPool(&sAudioMem2Pool, MEM2_AUDIO_POOL_TARGET_BYTES,
	                MEM2_AUDIO_POOL_TARGET_BYTES, 0);
	SYS_Report("[WII-MEM-PROFILE] profile=%s audio_decode=%u "
	           "audio_baseline_requested=%u generic_reclaimed=%u "
	           "generic_capacity=%u audio_capacity=%u\n",
	           WII_MEMORY_PROFILE_ID, (unsigned)WII_AUDIO_DECODE_ENABLE,
	           (unsigned)WII_AUDIO_BASELINE_REQUESTED_BYTES,
	           (unsigned)WII_MEM2_RECLAIMED_BYTES,
	           (unsigned)sGenericMem2Pool.size,
	           (unsigned)sAudioMem2Pool.size);
}

static bool
WiiMem2Owns(const Mem2Pool *pool, const void *ptr)
{
	if(!pool->ready || ptr == nil)
		return false;
	return (const uint8*)ptr >= pool->base &&
	       (const uint8*)ptr < pool->end;
}

static void *
WiiMem2AllocFromPool(Mem2Pool *pool, size_t size, size_t alignment)
{
	if(!pool->ready)
		return nil;

	if(size == 0)
		size = 1;
	if(alignment < 4)
		alignment = 4;

	for(Mem2FreeBlock *block = pool->freeList; block; block = block->next) {
		uintptr blockStart = (uintptr)block;
		uintptr userPtr = WiiMem2AlignUpPow2(blockStart + sizeof(Mem2AllocHeader) +
		                                     sizeof(Mem2AllocHeader*), alignment);
		size_t totalUsed = (size_t)(userPtr + size - blockStart);
		if(totalUsed < sizeof(Mem2FreeBlock))
			totalUsed = sizeof(Mem2FreeBlock);
		totalUsed = (size_t)WiiMem2AlignUpPow2((uintptr)totalUsed, 32);
		if(totalUsed > block->size)
			continue;

		Mem2FreeBlock *prev = block->prev;
		Mem2FreeBlock *next = block->next;
		size_t remainder = block->size - totalUsed;

		if(remainder >= sizeof(Mem2FreeBlock)) {
			Mem2FreeBlock *tail = (Mem2FreeBlock*)(blockStart + totalUsed);
			tail->prev = prev;
			tail->next = next;
			tail->size = remainder;
			if(prev)
				prev->next = tail;
			else
				pool->freeList = tail;
			if(next)
				next->prev = tail;
		} else {
			totalUsed = block->size;
			if(prev)
				prev->next = next;
			else
				pool->freeList = next;
			if(next)
				next->prev = prev;
		}

		Mem2AllocHeader *header = (Mem2AllocHeader*)blockStart;
		header->magic = pool->allocMagic;
		header->reserved = 0;
		header->totalSize = totalUsed;
		header->userSize = size;
		((Mem2AllocHeader**)userPtr)[-1] = header;
		pool->allocCount++;
		pool->bytesUsed += totalUsed;
		if(pool->bytesUsed > pool->peakBytesUsed)
			pool->peakBytesUsed = pool->bytesUsed;
		return (void*)userPtr;
	}

	pool->allocFailCount++;
	return nil;
}

static void
WiiMem2FreeFromPool(Mem2Pool *pool, void *ptr)
{
	if(ptr == nil || !WiiMem2Owns(pool, ptr))
		return;
	if((uint8*)ptr < pool->base + sizeof(Mem2AllocHeader*)) {
		printf("[%s] invalid free ptr=%p below header boundary\n",
		       pool->logTag, ptr);
		return;
	}

	Mem2AllocHeader *header = ((Mem2AllocHeader**)ptr)[-1];
	if(header == nil ||
	   (uint8*)header < pool->base ||
	   (uint8*)header + sizeof(Mem2AllocHeader) > pool->end ||
	   header->magic != pool->allocMagic ||
	   header->totalSize < sizeof(Mem2FreeBlock)) {
		printf("[%s] invalid free ptr=%p header=%p\n", pool->logTag, ptr, (void*)header);
		return;
	}

	size_t totalSize = header->totalSize;
	header->magic = 0;

	Mem2FreeBlock *block = (Mem2FreeBlock*)header;
	block->size = totalSize;
	if(totalSize <= pool->bytesUsed)
		pool->bytesUsed -= totalSize;
	else
		pool->bytesUsed = 0;
	WiiMem2InsertFreeBlock(pool, block);
}

static size_t
WiiMem2UserSize(const Mem2Pool *pool, const void *ptr)
{
	if(ptr == nil || !WiiMem2Owns(pool, ptr))
		return 0;
	if((const uint8*)ptr < pool->base + sizeof(Mem2AllocHeader*))
		return 0;

	Mem2AllocHeader *header = ((Mem2AllocHeader* const*)ptr)[-1];
	if(header == nil ||
	   (uint8*)header < pool->base ||
	   (uint8*)header + sizeof(Mem2AllocHeader) > pool->end ||
	   header->magic != pool->allocMagic)
		return 0;

	return header->userSize;
}

static void
WiiMem2GetArenaStats(const Mem2Pool *pool, WiiMemoryArenaStats *stats)
{
	if(stats == nil)
		return;

	memset(stats, 0, sizeof(*stats));
	if(!pool->ready)
		return;

	size_t freeBytes = 0;
	size_t largestBytes = 0;
	WiiMem2GetFreeStats(pool, &freeBytes, &largestBytes);
	stats->base = (RwUInt32)(uintptr)pool->base;
	stats->end = (RwUInt32)(uintptr)pool->end;
	stats->capacity = (RwUInt32)pool->size;
	stats->used = (RwUInt32)pool->bytesUsed;
	stats->free = (RwUInt32)freeBytes;
	stats->largest = (RwUInt32)largestBytes;
	stats->peak = (RwUInt32)pool->peakBytesUsed;
	stats->allocCount = pool->allocCount;
	stats->failCount = pool->allocFailCount;
}
}
#endif


uint8 *pMemoryTop;

extern uint8 _end[];
extern uint8 _stack_size[];

void
InitMemoryMgr(void)
{
#ifdef WII
	sRuntimeArena2Base = (uintptr)SYS_GetArena2Lo();
	sRuntimeArena2End = (uintptr)SYS_GetArena2Hi();
	InitMem2ResourcePool();
	InitAudioMem2Pool();
	rw::gx::gxMemInitArena2Pool();
	if((uintptr)SYS_GetArena2Lo() > (uintptr)SYS_GetArena2Hi()) {
		SYS_Report("[WII-MEM] FATAL: MEM2 partitions overlap (lo=%p hi=%p)\n",
		           SYS_GetArena2Lo(), SYS_GetArena2Hi());
		exit(1);
	}
	MALLOC_MEM2 = 1;
	WiiMemoryDumpStats("boot");
#endif
#ifdef USE_CUSTOM_ALLOCATOR
#ifdef GTA_PS2
	// not quite clear what the 0x1000s and 0x10 are exactly
	uint32 memUsed = (uint32)_end + (uint32)_stack_size + 0x1000 + 0x1000;
	uint32 heapSize = 32*1024*1024 - memUsed - 0x10;
printf("Heap size: %d\n", heapSize);
	gMainHeap.Init(heapSize);

#elif defined(GAMECUBE)
#ifdef WII
	InitMem2ResourcePool();
#endif
	// GC keeps most MEM1 for system malloc. Wii moves large streaming/GX resources
	// to MEM2, so RenderWare's custom heap needs a much larger MEM1 slice.
	void *arenaLo = SYS_GetArena1Lo();
	void *arenaHi = SYS_GetArena1Hi();
	u32   arenaSz = (u32)arenaHi - (u32)arenaLo;
#ifdef WII
	const u32 heapPercent = 65u;
#else
	const u32 heapPercent = 30u;
#endif
	u32   heapSz  = (u32)(((u64)arenaSz * heapPercent) / 100u);
	printf("[GC-MEM] CMemoryHeap: arena=%uKB, heap=%uKB (%u%%), system=%uKB\n",
	       arenaSz / 1024, heapSz / 1024, heapPercent, (arenaSz - heapSz) / 1024);
	gMainHeap.Init(heapSz);
#else
	// randomly allocate 128mb
	gMainHeap.Init(128*1024*1024);
#endif
#endif

#ifdef WII
	WiiMemoryEnsureFileOpenReserve();
#if WII_STREAM_MEMORY_DIAGNOSTICS
	WiiMemoryPhase0ReportRunStart();
#endif
#endif
}


RwMemoryFunctions memFuncs = {
	MemoryMgrMalloc,
	MemoryMgrFree,
	MemoryMgrRealloc,
	MemoryMgrCalloc
};

#ifdef USE_CUSTOM_ALLOCATOR
// game seems to be using heap directly here, but this is nicer
void *operator new(size_t sz) throw() { return MemoryMgrMalloc(sz); }
void *operator new[](size_t sz) throw() { return MemoryMgrMalloc(sz); }
void operator delete(void *ptr) throw() { MemoryMgrFree(ptr); }
void operator delete[](void *ptr) throw() { MemoryMgrFree(ptr); }
#endif

void*
MemoryMgrMalloc(size_t size)
{
#ifdef USE_CUSTOM_ALLOCATOR
	void *mem;
	if (gMainHeap.m_start)
		mem = gMainHeap.Malloc(size);
	else
		mem = malloc(size);	// fallback: heap not initialized yet
#else
	void *mem = malloc(size);
#endif
	if((uint8*)mem + size > pMemoryTop)
		pMemoryTop = (uint8*)mem + size ;
	return mem;
}

void*
MemoryMgrRealloc(void *ptr, size_t size)
{
#ifdef WII
	if(ptr == nil)
		return MemoryMgrMalloc(size);
	if(size == 0) {
		MemoryMgrFree(ptr);
		return nil;
	}
	if(WiiMem2Owns(&sGenericMem2Pool, ptr)) {
		size_t oldSize = WiiMem2UserSize(&sGenericMem2Pool, ptr);
		void *mem = WiiMem2AllocFromPool(&sGenericMem2Pool, size, 32);
		if(mem) {
			memcpy(mem, ptr, oldSize < size ? oldSize : size);
			WiiMem2FreeFromPool(&sGenericMem2Pool, ptr);
			if((uint8*)mem + size  > pMemoryTop)
				pMemoryTop = (uint8*)mem + size;
		}
		return mem;
	}
	if(WiiMem2Owns(&sAudioMem2Pool, ptr)) {
		size_t oldSize = WiiMem2UserSize(&sAudioMem2Pool, ptr);
		void *mem = WiiMem2AllocFromPool(&sAudioMem2Pool, size, 32);
		if(mem) {
			memcpy(mem, ptr, oldSize < size ? oldSize : size);
			WiiMem2FreeFromPool(&sAudioMem2Pool, ptr);
			if((uint8*)mem + size  > pMemoryTop)
				pMemoryTop = (uint8*)mem + size;
		}
		return mem;
	}
#endif
#ifdef USE_CUSTOM_ALLOCATOR
	void *mem;
	if (gMainHeap.m_start)
		mem = gMainHeap.Realloc(ptr, size);
	else
		mem = realloc(ptr, size);
#else
	void *mem = realloc(ptr, size);
#endif
	if((uint8*)mem + size  > pMemoryTop)
		pMemoryTop = (uint8*)mem + size ;
	return mem;
}

void*
MemoryMgrCalloc(size_t num, size_t size)
{
#ifdef USE_CUSTOM_ALLOCATOR
	void *mem;
	if (gMainHeap.m_start)
		mem = gMainHeap.Malloc(num*size);
	else
		mem = calloc(num, size);
#else
	void *mem = calloc(num, size);
#endif
	if((uint8*)mem + size  > pMemoryTop)
		pMemoryTop = (uint8*)mem + size ;
#ifdef FIX_BUGS
	memset(mem, 0, num*size);
#endif
	return mem;
}

void
MemoryMgrFree(void *ptr)
{
#ifdef WII
	if(ptr == nil)
		return;
	if(WiiMem2Owns(&sGenericMem2Pool, ptr)) {
		WiiMem2FreeFromPool(&sGenericMem2Pool, ptr);
		return;
	}
	if(WiiMem2Owns(&sAudioMem2Pool, ptr)) {
		WiiMem2FreeFromPool(&sAudioMem2Pool, ptr);
		return;
	}
#endif
#ifdef USE_CUSTOM_ALLOCATOR
	if(ptr == nil) return;
	if (gMainHeap.m_start)
		gMainHeap.Free(ptr);
	else
		free(ptr);
#else
	free(ptr);
#endif
}

void *
RwMallocAlign(RwUInt32 size, RwUInt32 align)
{
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	uintptr ptralign = align-1;
	void *mem = (void *)MemoryMgrMalloc(size + sizeof(uintptr) + ptralign);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + sizeof(uintptr) + ptralign) & ~ptralign);
#else
	void *mem = (void *)MemoryMgrMalloc(size + align);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + align) & ~(align - 1));
#endif

	*(((void **)addr) - 1) = mem;

	return addr;
}

void
RwFreeAlign(void *mem)
{
	ASSERT(mem != nil);

	void *addr = *(((void **)mem) - 1);

	ASSERT(addr != nil);

	MemoryMgrFree(addr);
}

#ifdef WII
void *
MemoryMgrMallocMem2(size_t size, size_t align)
{
	InitMem2ResourcePool();
	if(size == 0)
		size = 1;
	if(align < 32)
		align = 32;

	return WiiMem2AllocFromPool(&sGenericMem2Pool, size, align);
}

void *
MemoryMgrMallocMem2Strict(size_t size, size_t align)
{
	InitMem2ResourcePool();
	if(size == 0)
		size = 1;
	if(align < 32)
		align = 32;
	return WiiMem2AllocFromPool(&sGenericMem2Pool, size, align);
}

void *
MemoryMgrReallocMem2Strict(void *ptr, size_t size, size_t align)
{
	if(ptr == nil)
		return MemoryMgrMallocMem2Strict(size, align);
	if(size == 0) {
		MemoryMgrFree(ptr);
		return nil;
	}
	if(WiiMem2Owns(&sGenericMem2Pool, ptr))
		return MemoryMgrRealloc(ptr, size);

	if(align < 32)
		align = 32;
	size_t oldSize;
#ifdef USE_CUSTOM_ALLOCATOR
	if(gMainHeap.m_start)
		oldSize = gMainHeap.GetDescFromHeapPointer(ptr)->m_size;
	else
		oldSize = malloc_usable_size(ptr);
#else
	oldSize = malloc_usable_size(ptr);
#endif

	InitMem2ResourcePool();
	void *mem = WiiMem2AllocFromPool(&sGenericMem2Pool, size, align);
	if(mem == nil)
		return nil;
	memcpy(mem, ptr, Min(oldSize, size));
	MemoryMgrFree(ptr);
	return mem;
}

void
MemoryMgrFreeMem2(void *ptr)
{
	if(ptr == nil)
		return;
	if(WiiMem2Owns(&sGenericMem2Pool, ptr)) {
		WiiMem2FreeFromPool(&sGenericMem2Pool, ptr);
		return;
	}
	SYS_Report("[WII-MEM] rejected non-generic pointer in MemoryMgrFreeMem2: %p\n", ptr);
}

void *
MemoryMgrMallocAudioMem2(size_t size, size_t align)
{
	InitAudioMem2Pool();
	if(size == 0)
		size = 1;
	if(align < 32)
		align = 32;

	u32 level;
	_CPU_ISR_Disable(level);
	void *mem = WiiMem2AllocFromPool(&sAudioMem2Pool, size, align);
	_CPU_ISR_Restore(level);
	return mem;
}

void *
MemoryMgrMallocAudioMem2Strict(size_t size, size_t align)
{
	return MemoryMgrMallocAudioMem2(size, align);
}

void *
MemoryMgrReallocAudioMem2Strict(void *ptr, size_t size, size_t align)
{
	if(ptr == nil)
		return MemoryMgrMallocAudioMem2Strict(size, align);
	if(size == 0) {
		MemoryMgrFreeAudioMem2(ptr);
		return nil;
	}
	if(align < 32)
		align = 32;
	u32 level;
	_CPU_ISR_Disable(level);
	bool ownsAudio = WiiMem2Owns(&sAudioMem2Pool, ptr);
	size_t oldSize = ownsAudio ? WiiMem2UserSize(&sAudioMem2Pool, ptr) : 0;
	_CPU_ISR_Restore(level);
	if(!ownsAudio)
		return nil;

	void *mem = MemoryMgrMallocAudioMem2Strict(size, align);
	if(mem == nil)
		return nil;
	memcpy(mem, ptr, Min(oldSize, size));
	MemoryMgrFreeAudioMem2(ptr);
	return mem;
}

void
MemoryMgrFreeAudioMem2(void *ptr)
{
	if(ptr == nil)
		return;
	u32 level;
	_CPU_ISR_Disable(level);
	bool ownsAudio = WiiMem2Owns(&sAudioMem2Pool, ptr);
	if(ownsAudio)
		WiiMem2FreeFromPool(&sAudioMem2Pool, ptr);
	_CPU_ISR_Restore(level);
	if(ownsAudio)
		return;
	SYS_Report("[WII-MEM] rejected non-audio pointer in MemoryMgrFreeAudioMem2: %p\n", ptr);
}

void *
MemoryMgrMallocAlignMem2(size_t size, size_t align)
{
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	uintptr ptralign = align-1;
	void *mem = (void *)MemoryMgrMallocMem2(size + sizeof(uintptr) + ptralign, 32);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + sizeof(uintptr) + ptralign) & ~ptralign);
#else
	void *mem = (void *)MemoryMgrMallocMem2(size + align, 32);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + align) & ~(align - 1));
#endif

	*(((void **)addr) - 1) = mem;

	return addr;
}

void *
MemoryMgrMallocAlignMem2Strict(size_t size, size_t align)
{
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	uintptr ptralign = align-1;
	void *mem = (void *)MemoryMgrMallocMem2Strict(size + sizeof(uintptr) + ptralign, 32);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + sizeof(uintptr) + ptralign) & ~ptralign);
#else
	void *mem = (void *)MemoryMgrMallocMem2Strict(size + align, 32);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + align) & ~(align - 1));
#endif

	*(((void **)addr) - 1) = mem;

	return addr;
}

void *
MemoryMgrMallocAlignAudioMem2(size_t size, size_t align)
{
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	uintptr ptralign = align-1;
	void *mem = (void *)MemoryMgrMallocAudioMem2(size + sizeof(uintptr) + ptralign, 32);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + sizeof(uintptr) + ptralign) & ~ptralign);
#else
	void *mem = (void *)MemoryMgrMallocAudioMem2(size + align, 32);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + align) & ~(align - 1));
#endif

	*(((void **)addr) - 1) = mem;

	return addr;
}

void *
MemoryMgrMallocAlignAudioMem2Strict(size_t size, size_t align)
{
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	uintptr ptralign = align-1;
	void *mem = (void *)MemoryMgrMallocAudioMem2Strict(size + sizeof(uintptr) + ptralign, 32);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + sizeof(uintptr) + ptralign) & ~ptralign);
#else
	void *mem = (void *)MemoryMgrMallocAudioMem2Strict(size + align, 32);

	if (mem == nil) return nil;

	void *addr = (void *)((((uintptr)mem) + align) & ~(align - 1));
#endif

	*(((void **)addr) - 1) = mem;

	return addr;
}

void
MemoryMgrFreeAlignMem2(void *mem)
{
	ASSERT(mem != nil);

	void *addr = *(((void **)mem) - 1);

	ASSERT(addr != nil);

	MemoryMgrFreeMem2(addr);
}

void
MemoryMgrFreeAlignAudioMem2(void *mem)
{
	ASSERT(mem != nil);

	void *addr = *(((void **)mem) - 1);

	ASSERT(addr != nil);

	MemoryMgrFreeAudioMem2(addr);
}

extern "C" int
WiiMemoryOwnsGenericMem2(const void *ptr)
{
	return WiiMem2Owns(&sGenericMem2Pool, ptr) ? 1 : 0;
}

extern "C" int
WiiMemoryOwnsAudioMem2(const void *ptr)
{
	u32 level;
	_CPU_ISR_Disable(level);
	int owns = WiiMem2Owns(&sAudioMem2Pool, ptr) ? 1 : 0;
	_CPU_ISR_Restore(level);
	return owns;
}

extern "C" void
WiiMemoryGetGenericMem2Stats(RwUInt32 *capacity, RwUInt32 *used,
	RwUInt32 *freeBytes, RwUInt32 *largestBytes, RwUInt32 *peakBytes,
	RwUInt32 *allocCount, RwUInt32 *failCount)
{
	WiiMemoryArenaStats stats;
	WiiMem2GetArenaStats(&sGenericMem2Pool, &stats);
	if(capacity) *capacity = stats.capacity;
	if(used) *used = stats.used;
	if(freeBytes) *freeBytes = stats.free;
	if(largestBytes) *largestBytes = stats.largest;
	if(peakBytes) *peakBytes = stats.peak;
	if(allocCount) *allocCount = stats.allocCount;
	if(failCount) *failCount = stats.failCount;
}

extern "C" void
WiiMemoryGetAudioMem2Stats(RwUInt32 *capacity, RwUInt32 *used,
	RwUInt32 *freeBytes, RwUInt32 *largestBytes, RwUInt32 *peakBytes,
	RwUInt32 *allocCount, RwUInt32 *failCount)
{
	WiiMemoryArenaStats stats;
	u32 level;
	_CPU_ISR_Disable(level);
	WiiMem2GetArenaStats(&sAudioMem2Pool, &stats);
	_CPU_ISR_Restore(level);
	if(capacity) *capacity = stats.capacity;
	if(used) *used = stats.used;
	if(freeBytes) *freeBytes = stats.free;
	if(largestBytes) *largestBytes = stats.largest;
	if(peakBytes) *peakBytes = stats.peak;
	if(allocCount) *allocCount = stats.allocCount;
	if(failCount) *failCount = stats.failCount;
}

extern "C" void
WiiMemoryGetGenericMem2ArenaStats(WiiMemoryArenaStats *stats)
{
	WiiMem2GetArenaStats(&sGenericMem2Pool, stats);
}

extern "C" void
WiiMemoryGetAudioMem2ArenaStats(WiiMemoryArenaStats *stats)
{
	u32 level;
	_CPU_ISR_Disable(level);
	WiiMem2GetArenaStats(&sAudioMem2Pool, stats);
	_CPU_ISR_Restore(level);
}

extern "C" void
WiiMemoryEnsureFileOpenReserve(void)
{
	if(sFileOpenReserve != nil)
		return;

	void *candidate = malloc(WII_FILE_OPEN_RESERVE_BYTES);
	if(candidate == nil)
		return;

	u32 level;
	_CPU_ISR_Disable(level);
	if(sFileOpenReserve == nil) {
		sFileOpenReserve = candidate;
		candidate = nil;
	}
	_CPU_ISR_Restore(level);

	if(candidate)
		free(candidate);
}

extern "C" int
WiiMemoryReleaseFileOpenReserve(const char *reason)
{
	void *reserve;
	u32 level;
	_CPU_ISR_Disable(level);
	reserve = sFileOpenReserve;
	sFileOpenReserve = nil;
	_CPU_ISR_Restore(level);

	if(reserve == nil)
		return 0;

	free(reserve);
	SYS_Report("[WII-IO] released %uKB file-open reserve for %s\n",
	           (unsigned)(WII_FILE_OPEN_RESERVE_BYTES / 1024u),
	           reason ? reason : "critical open");
	return 1;
}

extern "C" RwUInt32
WiiMemoryGetStreamingPressureForSnapshot(const WiiMemoryPoolSnapshot *snapshot)
{
	if(snapshot == nil)
		return 0;

	uint32 pressure = 0;
	if(snapshot->genericFree < WII_STREAM_GENERIC_RESERVE_BYTES ||
	   snapshot->genericLargest < WII_STREAM_GENERIC_LARGEST_BYTES)
		pressure |= WII_STREAM_PRESSURE_GENERIC;
	if((size_t)snapshot->newlibFree + snapshot->rawArena2Remaining < WII_STREAM_NEWLIB_RECLAIM_BYTES)
		pressure |= WII_STREAM_PRESSURE_NEWLIB;
	if(snapshot->gxFree < WII_STREAM_GX_RESERVE_BYTES ||
	   snapshot->gxLargest < WII_STREAM_GX_LARGEST_BYTES)
		pressure |= WII_STREAM_PRESSURE_GX;
	return pressure;
}

static size_t
WiiMemoryShortfall(size_t target, size_t available)
{
	return available < target ? target - available : 0;
}

extern "C" RwUInt32
WiiMemoryGetStreamingPressureDeficit(const WiiMemoryPoolSnapshot *snapshot,
	RwUInt32 poolBit, size_t requestedBytes)
{
	if(snapshot == nil)
		return 0;

	size_t fitBytes = Min(requestedBytes, WII_STREAM_REQUEST_FIT_CAP_BYTES);
	size_t deficit = 0;
	switch(poolBit){
	case WII_STREAM_PRESSURE_GENERIC:
		deficit = WiiMemoryShortfall(WII_STREAM_GENERIC_RESERVE_BYTES,
		                             snapshot->genericFree) +
		          WiiMemoryShortfall(WII_STREAM_GENERIC_LARGEST_BYTES,
		                             snapshot->genericLargest) +
		          WiiMemoryShortfall(fitBytes, snapshot->genericLargest);
		break;
	case WII_STREAM_PRESSURE_NEWLIB:
		deficit = WiiMemoryShortfall(WII_STREAM_NEWLIB_RECLAIM_BYTES,
		                             (size_t)snapshot->newlibFree +
		                             snapshot->rawArena2Remaining) +
		          WiiMemoryShortfall(fitBytes,
		                             (size_t)snapshot->newlibFree +
		                             snapshot->rawArena2Remaining);
		break;
	case WII_STREAM_PRESSURE_GX:
		deficit = WiiMemoryShortfall(WII_STREAM_GX_RESERVE_BYTES,
		                             snapshot->gxFree) +
		          WiiMemoryShortfall(WII_STREAM_GX_LARGEST_BYTES,
		                             snapshot->gxLargest) +
		          WiiMemoryShortfall(fitBytes, snapshot->gxLargest);
		break;
	default:
		return 0;
	}
	return deficit > UINT32_MAX ? UINT32_MAX : (RwUInt32)deficit;
}

extern "C" RwUInt32
WiiMemoryGetStreamingPressure(void)
{
	WiiMemoryPoolSnapshot snapshot;
	WiiMemoryGetPoolSnapshot(&snapshot);
	return WiiMemoryGetStreamingPressureForSnapshot(&snapshot);
}

extern "C" void
WiiMemorySetResidentDeltaCallback(WiiMemoryResidentDeltaCallback callback)
{
	sResidentDeltaCallback = callback;
}

extern "C" void
WiiMemoryBeginResourceAttribution(RwUInt16 ownerStreamId)
{
	sResourceAttributionMask = 0;
	sResourceAttributionOwner = ownerStreamId;
	memset(sResourceAttributionOwnerDelta, 0,
	       sizeof(sResourceAttributionOwnerDelta));
	memset(sResourceAttributionOwnerKiB, 0,
	       sizeof(sResourceAttributionOwnerKiB));
	memset(sResourceAttributionExternalDelta, 0,
	       sizeof(sResourceAttributionExternalDelta));
	sResourceAttributionActive = true;
}

extern "C" RwUInt16
WiiMemoryGetResourceAttributionOwner(void)
{
	return sResourceAttributionActive ? sResourceAttributionOwner :
	       WII_MEMORY_RESOURCE_OWNER_UNKNOWN;
}

extern "C" void
WiiMemoryRecordResidentPool(RwUInt32 poolBit)
{
	if(sResourceAttributionActive)
		sResourceAttributionMask |= poolBit & (WII_STREAM_PRESSURE_GENERIC |
		                                      WII_STREAM_PRESSURE_NEWLIB |
		                                      WII_STREAM_PRESSURE_GX);
}

extern "C" void
WiiMemoryRecordResidentDelta(RwUInt16 ownerStreamId, RwUInt32 poolBit,
	RwInt32 deltaBytes)
{
	int poolIndex = WiiMemoryResourcePoolIndex(poolBit);
	if(poolIndex < 0 || deltaBytes == 0)
		return;

	if(sResourceAttributionActive){
		if(ownerStreamId == sResourceAttributionOwner){
			sResourceAttributionOwnerDelta[poolIndex] =
				WiiMemoryAddResidentDelta(
					sResourceAttributionOwnerDelta[poolIndex], deltaBytes);
			sResourceAttributionOwnerKiB[poolIndex] =
				WiiMemoryAddResidentDelta(
					sResourceAttributionOwnerKiB[poolIndex],
					WiiMemoryResidentKiBDelta(deltaBytes));
			if(deltaBytes > 0)
				sResourceAttributionMask |= poolBit;
			return;
		}

		sResourceAttributionExternalDelta[poolIndex] =
			WiiMemoryAddResidentDelta(
				sResourceAttributionExternalDelta[poolIndex], deltaBytes);
	}

	if(ownerStreamId != WII_MEMORY_RESOURCE_OWNER_UNKNOWN &&
	   sResidentDeltaCallback != nil)
		sResidentDeltaCallback(ownerStreamId, poolBit, deltaBytes);
}

extern "C" void
WiiMemoryEndResourceAttribution(WiiMemoryResourceAttribution *result)
{
	if(result){
		memset(result, 0, sizeof(*result));
		if(sResourceAttributionActive){
			result->mask = sResourceAttributionMask;
			result->ownerGenericBytes = sResourceAttributionOwnerDelta[0];
			result->ownerNewlibBytes = sResourceAttributionOwnerDelta[1];
			result->ownerGxBytes = sResourceAttributionOwnerDelta[2];
			result->ownerGenericKiB = sResourceAttributionOwnerKiB[0];
			result->ownerNewlibKiB = sResourceAttributionOwnerKiB[1];
			result->ownerGxKiB = sResourceAttributionOwnerKiB[2];
			result->externalGenericBytes = sResourceAttributionExternalDelta[0];
			result->externalNewlibBytes = sResourceAttributionExternalDelta[1];
			result->externalGxBytes = sResourceAttributionExternalDelta[2];
		}
	}
	sResourceAttributionMask = 0;
	sResourceAttributionOwner = WII_MEMORY_RESOURCE_OWNER_UNKNOWN;
	memset(sResourceAttributionOwnerDelta, 0,
	       sizeof(sResourceAttributionOwnerDelta));
	memset(sResourceAttributionOwnerKiB, 0,
	       sizeof(sResourceAttributionOwnerKiB));
	memset(sResourceAttributionExternalDelta, 0,
	       sizeof(sResourceAttributionExternalDelta));
	sResourceAttributionActive = false;
}

extern "C" void
WiiMemoryGetPoolUsage(WiiMemoryPoolUsage *usage)
{
	if(usage == nil)
		return;

	struct mallinfo heap = mallinfo();
	uintptr rawLo = (uintptr)SYS_GetArena2Lo();
	uintptr rawHi = (uintptr)SYS_GetArena2Hi();
	uint32 gxUsed = 0;
	rw::gx::gxMemGetPoolStats(nil, &gxUsed, nil, nil, nil, nil);

	usage->genericUsed = (RwUInt32)sGenericMem2Pool.bytesUsed;
	usage->audioUsed = (RwUInt32)sAudioMem2Pool.bytesUsed;
	usage->newlibUsed = heap.uordblks > 0 ? (RwUInt32)heap.uordblks : 0;
	usage->rawArena2Remaining = rawHi > rawLo ? (RwUInt32)(rawHi - rawLo) : 0;
	usage->gxUsed = gxUsed;
}

extern "C" void
WiiMemoryGetPoolSnapshot(WiiMemoryPoolSnapshot *snapshot)
{
	if(snapshot == nil)
		return;

	size_t genericFree = 0;
	size_t genericLargest = 0;
	RwUInt32 audioUsed = 0;
	RwUInt32 audioFree = 0;
	RwUInt32 audioLargest = 0;
	WiiMem2GetFreeStats(&sGenericMem2Pool, &genericFree, &genericLargest);
	WiiMemoryGetAudioMem2Stats(nil, &audioUsed, &audioFree, &audioLargest,
	                           nil, nil, nil);

	struct mallinfo heap = mallinfo();
	uintptr rawLo = (uintptr)SYS_GetArena2Lo();
	uintptr rawHi = (uintptr)SYS_GetArena2Hi();
	size_t rawRemaining = rawHi > rawLo ? (size_t)(rawHi - rawLo) : 0;

	uint32 gxCapacity = 0;
	uint32 gxUsed = 0;
	uint32 gxLargest = 0;
	rw::gx::gxMemGetPoolStats(&gxCapacity, &gxUsed, nil, &gxLargest, nil, nil);

	snapshot->genericUsed = (RwUInt32)sGenericMem2Pool.bytesUsed;
	snapshot->genericFree = (RwUInt32)genericFree;
	snapshot->genericLargest = (RwUInt32)genericLargest;
	snapshot->audioUsed = audioUsed;
	snapshot->audioFree = audioFree;
	snapshot->audioLargest = audioLargest;
	snapshot->newlibUsed = heap.uordblks > 0 ? (RwUInt32)heap.uordblks : 0;
	snapshot->newlibFree = heap.fordblks > 0 ? (RwUInt32)heap.fordblks : 0;
	snapshot->rawArena2Remaining = (RwUInt32)rawRemaining;
	snapshot->gxUsed = gxUsed;
	snapshot->gxFree = gxCapacity > gxUsed ? gxCapacity - gxUsed : 0;
	snapshot->gxLargest = gxLargest;
}

#if WII_STREAM_MEMORY_DIAGNOSTICS
static uint32
WiiMemoryPhase0ElapsedMs(void)
{
	uint64 elapsed = ticks_to_millisecs(gettime() - sPhase0EpochTicks);
	return elapsed > UINT32_MAX ? UINT32_MAX : (uint32)elapsed;
}

static void
WiiMemoryPhase0Report(const char *event, const WiiMemoryPhase0Counters *counters)
{
	WiiMemoryPoolSnapshot snapshot;
	WiiMemoryGetPoolSnapshot(&snapshot);

	struct mallinfo heap = mallinfo();
	WiiMemoryArenaStats genericStats;
	WiiMemoryArenaStats audioStats;
	WiiMemoryGetGenericMem2ArenaStats(&genericStats);
	WiiMemoryGetAudioMem2ArenaStats(&audioStats);
	uint32 gxCapacity = 0;
	uint32 gxAllocFails = 0;
	uint32 gxFallbacks = 0;
	rw::gx::gxMemGetPoolStats(&gxCapacity, nil, nil, nil,
	                          &gxAllocFails, &gxFallbacks);
	uint32 ownedGenericBytes = 0;
	uint32 unknownGenericBytes = 0;
	uint32 ownedGxBytes = 0;
	uint32 unknownGxBytes = 0;
	rw::gx::texPoolGetOwnerStats(&ownedGenericBytes, &unknownGenericBytes,
	                             &ownedGxBytes, &unknownGxBytes);

	const uint32 requestPending = counters ? counters->requestPending : 0;
	const uint32 requestRetryCount = counters ? counters->requestRetryCount : 0;
	const uint32 hardFallbackCount = counters ? counters->hardFallbackCount : 0;
	const int32 txdFailureCount = counters ? counters->txdFailureCount : -1;
	char txdFailureValue[16];
	if(txdFailureCount < 0)
		strcpy(txdFailureValue, "null");
	else
		snprintf(txdFailureValue, sizeof(txdFailureValue), "%d", txdFailureCount);

	SYS_Report("[WII-P0] {\"schema_version\":1,\"event\":\"%s\",\"sequence\":%u,\"elapsed_ms\":%u,\"profile_id\":\"%s\",\"build_id\":\"%s\",\"runtime_arena2_lo\":%u,\"runtime_arena2_hi\":%u,\"linker_arena2_lo\":%u,\"linker_arena2_hi\":%u,\"generic_base\":%u,\"generic_end\":%u,\"audio_base\":%u,\"audio_end\":%u,\"process_heap_claim_base\":%u,\"process_heap_claim_end\":%u,\"raw2_lo\":%u,\"raw2_hi\":%u,\"gx_base\":%u,\"gx_end\":%u,\"shared_reserve_base\":0,\"shared_reserve_end\":0,\"shared_reserve_state\":\"disabled\",\"allocator_routing_mode\":\"generic-fixed+audio-fixed+process-heap-arena2+gx-fixed\",\"texture_format_policy\":\"current-runtime\",\"texture_candidate_state\":\"blocked\",\"malloc_mem2\":%u,\"generic_capacity\":%u,\"generic_used\":%u,\"generic_free\":%u,\"generic_largest\":%u,\"generic_peak\":%u,\"audio_capacity\":%u,\"audio_used\":%u,\"audio_free\":%u,\"audio_largest\":%u,\"audio_peak\":%u,\"audio_alloc_fail_count\":%u,\"process_heap_arena\":%u,\"process_heap_used\":%u,\"process_heap_free\":%u,\"process_heap_top\":%u,\"raw2_remaining\":%u,\"gx_capacity\":%u,\"gx_used\":%u,\"gx_free\":%u,\"gx_largest\":%u,\"gx_texture_bytes\":%u,\"gx_texture_count\":%u,\"gx_compaction_generation\":%u,\"gx_shrink_count\":%u,\"generic_owner_bytes\":%u,\"generic_system_bytes\":null,\"generic_unknown_bytes\":%u,\"gx_owner_bytes\":%u,\"gx_system_bytes\":null,\"gx_unknown_bytes\":%u,\"gx_alloc_fail_count\":%u,\"gx_fallback_count\":%u,\"request_pending\":%u,\"request_retry_count\":%u,\"hard_fallback_count\":%u,\"txd_failure_count\":%s}\n",
	           event,
	           (unsigned)sPhase0Sequence++,
	           (unsigned)WiiMemoryPhase0ElapsedMs(),
	           WII_MEMORY_PROFILE_ID, WII_BUILD_ID,
	           (unsigned)sRuntimeArena2Base, (unsigned)sRuntimeArena2End,
	           (unsigned)(uintptr)__Arena2Lo, (unsigned)(uintptr)__Arena2Hi,
	           genericStats.base, genericStats.end,
	           audioStats.base, audioStats.end,
	           audioStats.end ? audioStats.end : genericStats.end,
	           (unsigned)(uintptr)SYS_GetArena2Lo(),
	           (unsigned)(uintptr)SYS_GetArena2Lo(), (unsigned)(uintptr)SYS_GetArena2Hi(),
	           (unsigned)(uintptr)rw::gx::gxMemGetPoolBase(),
	           (unsigned)(uintptr)rw::gx::gxMemGetPoolEnd(),
	           (unsigned)MALLOC_MEM2,
	           genericStats.capacity,
	           (unsigned)snapshot.genericUsed, (unsigned)snapshot.genericFree,
	           (unsigned)snapshot.genericLargest, genericStats.peak,
	           audioStats.capacity,
	           (unsigned)snapshot.audioUsed, (unsigned)snapshot.audioFree,
	           (unsigned)snapshot.audioLargest, audioStats.peak,
	           audioStats.failCount,
	           heap.arena > 0 ? (unsigned)heap.arena : 0u,
	           heap.uordblks > 0 ? (unsigned)heap.uordblks : 0u,
	           heap.fordblks > 0 ? (unsigned)heap.fordblks : 0u,
	           heap.keepcost > 0 ? (unsigned)heap.keepcost : 0u,
	           (unsigned)snapshot.rawArena2Remaining,
	           (unsigned)gxCapacity, (unsigned)snapshot.gxUsed,
	           (unsigned)snapshot.gxFree, (unsigned)snapshot.gxLargest,
	           (unsigned)rw::gx::texPoolTotalBytes(),
	           (unsigned)rw::gx::texPoolCount(),
	           (unsigned)rw::gx::gxMemGetCompactionGeneration(),
	           (unsigned)rw::gx::gxMemGetShrinkTotalCount(),
	           (unsigned)ownedGenericBytes, (unsigned)unknownGenericBytes,
	           (unsigned)ownedGxBytes, (unsigned)unknownGxBytes,
	           (unsigned)gxAllocFails, (unsigned)gxFallbacks,
	           (unsigned)requestPending, (unsigned)requestRetryCount,
	           (unsigned)hardFallbackCount,
	           txdFailureValue);
}
#endif

extern "C" void
WiiMemoryPhase0ReportRunStart(void)
{
#if WII_STREAM_MEMORY_DIAGNOSTICS
	if(sPhase0RunStarted)
		return;
	sPhase0EpochTicks = gettime();
	sPhase0Sequence = 0;
	sPhase0RunStarted = true;
	WiiMemoryPhase0Report("run_start", nil);
#endif
}

extern "C" void
WiiMemoryPhase0ReportSnapshot(const WiiMemoryPhase0Counters *counters)
{
#if WII_STREAM_MEMORY_DIAGNOSTICS
	if(!sPhase0RunStarted)
		return;
	WiiMemoryPhase0Report("snapshot", counters);
#else
	(void)counters;
#endif
}

extern "C" void
WiiMemoryDumpStats(const char *reason)
{
	const char *label = reason ? reason : "snapshot";
	size_t genericFree = 0;
	size_t genericLargest = 0;
	RwUInt32 audioCapacity = 0;
	RwUInt32 audioUsed = 0;
	RwUInt32 audioFree = 0;
	RwUInt32 audioLargest = 0;
	RwUInt32 audioPeak = 0;
	RwUInt32 audioAlloc = 0;
	RwUInt32 audioFail = 0;
	WiiMem2GetFreeStats(&sGenericMem2Pool, &genericFree, &genericLargest);
	WiiMemoryGetAudioMem2Stats(&audioCapacity, &audioUsed, &audioFree,
	                           &audioLargest, &audioPeak, &audioAlloc, &audioFail);

	uint32 gxCapacity = 0;
	uint32 gxUsed = 0;
	uint32 gxPeak = 0;
	uint32 gxLargest = 0;
	uint32 gxAllocFails = 0;
	uint32 gxFallbacks = 0;
	rw::gx::gxMemGetPoolStats(&gxCapacity, &gxUsed, &gxPeak,
	                          &gxLargest, &gxAllocFails, &gxFallbacks);

	struct mallinfo heap = mallinfo();
	void *arena1Lo = SYS_GetArena1Lo();
	void *arena1Hi = SYS_GetArena1Hi();
	void *arena2Lo = SYS_GetArena2Lo();
	void *arena2Hi = SYS_GetArena2Hi();
	uint32 gxFree = gxCapacity > gxUsed ? gxCapacity - gxUsed : 0;

	SYS_Report("[WII-MEM] %s layout generic=[%p,%p) audio=[%p,%p) raw2-unclaimed=[%p,%p) gx=[%p,%p)\n",
	           label, (void*)sGenericMem2Pool.base, (void*)sGenericMem2Pool.end,
	           (void*)sAudioMem2Pool.base, (void*)sAudioMem2Pool.end,
	           arena2Lo, arena2Hi,
	           rw::gx::gxMemGetPoolBase(), rw::gx::gxMemGetPoolEnd());
	SYS_Report("[WII-MEM] %s generic=%u/%uKB peak=%uKB free=%uKB largest=%uKB alloc=%u fail=%u\n",
	           label,
	           (unsigned)(sGenericMem2Pool.bytesUsed / 1024u),
	           (unsigned)(sGenericMem2Pool.size / 1024u),
	           (unsigned)(sGenericMem2Pool.peakBytesUsed / 1024u),
	           (unsigned)(genericFree / 1024u),
	           (unsigned)(genericLargest / 1024u),
	           (unsigned)sGenericMem2Pool.allocCount,
	           (unsigned)sGenericMem2Pool.allocFailCount);
	SYS_Report("[WII-MEM] %s audio=%u/%uKB peak=%uKB free=%uKB largest=%uKB alloc=%u fail=%u\n",
	           label,
	           (unsigned)(audioUsed / 1024u),
	           (unsigned)(audioCapacity / 1024u),
	           (unsigned)(audioPeak / 1024u),
	           (unsigned)(audioFree / 1024u),
	           (unsigned)(audioLargest / 1024u),
	           (unsigned)audioAlloc,
	           (unsigned)audioFail);
	SYS_Report("[WII-MEM] %s newlib arena=%uKB used=%uKB free=%uKB top=%uKB fileReserve=%uKB arena1=[%p,%p)\n",
	           label,
	           (unsigned)(heap.arena / 1024u),
	           (unsigned)(heap.uordblks / 1024u),
	           (unsigned)(heap.fordblks / 1024u),
	           (unsigned)(heap.keepcost / 1024u),
	           sFileOpenReserve ? (unsigned)(WII_FILE_OPEN_RESERVE_BYTES / 1024u) : 0u,
	           arena1Lo, arena1Hi);
	SYS_Report("[WII-MEM] %s gx=%u/%uKB peak=%uKB free=%uKB largest=%uKB fail=%u fallback=%u tex=%uKB/%d soft=%uKB\n",
	           label,
	           (unsigned)(gxUsed / 1024u),
	           (unsigned)(gxCapacity / 1024u),
	           (unsigned)(gxPeak / 1024u),
	           (unsigned)(gxFree / 1024u),
	           (unsigned)(gxLargest / 1024u),
	           (unsigned)gxAllocFails,
	           (unsigned)gxFallbacks,
	           (unsigned)(rw::gx::texPoolTotalBytes() / 1024u),
	           rw::gx::texPoolCount(),
	           (unsigned)(rw::gx::texPoolGetSoftBudget() / 1024u));
	if(rw::gx::texPoolCount() > 0)
		rw::gx::texPoolResidencyReport(label);
}
#endif
