#include "common.h"
#include "MemoryHeap.h"
#include "MemoryMgr.h"

#ifdef GAMECUBE
#include <gccore.h>
#include "gxmemory.h"
#endif

#ifdef WII
extern "C" u32 MALLOC_MEM2;

namespace
{
// Wii has enough MEM2 headroom for a larger generic resource pool. The
// original 6MB pool gets exhausted by streaming buffers and the temporary
// decode/shrink scratch blocks we now route through MEM2, which pushes small
// rw allocations back onto MEM1/newlib and recreates the old OOM chain.
static const u32 MEM2_RESOURCE_POOL_TARGET_BYTES = 16u * 1024u * 1024u;
static const u32 MEM2_RESOURCE_POOL_MIN_BYTES    = 12u * 1024u * 1024u;
static const u32 MEM2_RESOURCE_ALLOC_MAGIC       = 0x4D32474Eu; // M2GN
static const size_t WII_RW_MEM2_MALLOC_THRESHOLD = 8u * 1024u;

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

static bool sMem2PoolInitAttempted = false;
static bool sMem2PoolReady = false;
static uint8 *sMem2PoolBase = nullptr;
static uint8 *sMem2PoolEnd = nullptr;
static size_t sMem2PoolSize = 0;
static Mem2FreeBlock *sMem2FreeList = nullptr;
static uint32 sMem2FallbackCount = 0;
static uint32 sMem2RwAllocCount = 0;
static uint32 sMem2RwFallbackCount = 0;

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
WiiMem2MergeForward(Mem2FreeBlock *block)
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
WiiMem2InsertFreeBlock(Mem2FreeBlock *block)
{
	block->prev = nil;
	block->next = nil;

	if(sMem2FreeList == nil) {
		sMem2FreeList = block;
		return;
	}

	Mem2FreeBlock *cur = sMem2FreeList;
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
		sMem2FreeList = block;
	if(cur)
		cur->prev = block;

	WiiMem2MergeForward(block);
	if(block->prev)
		WiiMem2MergeForward(block->prev);
}

static void
InitMem2ResourcePool(void)
{
	if(sMem2PoolInitAttempted)
		return;

	sMem2PoolInitAttempted = true;

	uintptr rawLo = (uintptr)SYS_GetArena2Lo();
	uintptr rawHi = (uintptr)SYS_GetArena2Hi();
	if(rawHi <= rawLo) {
		printf("[MEM2-GEN] Arena2 unavailable (lo=%p hi=%p)\n",
		       (void*)rawLo, (void*)rawHi);
		return;
	}

	uintptr poolBase = WiiMem2AlignUpPow2(rawLo, 32);
	size_t available = (size_t)(rawHi - poolBase);
	size_t poolSize = MEM2_RESOURCE_POOL_TARGET_BYTES;
	size_t keepFree = 8u * 1024u * 1024u;

	if(available > keepFree && poolSize > available - keepFree)
		poolSize = available - keepFree;
	if(poolSize < MEM2_RESOURCE_POOL_MIN_BYTES) {
		printf("[MEM2-GEN] not enough Arena2 for generic MEM2 pool (%u KB available)\n",
		       (unsigned)(available / 1024u));
		return;
	}

	poolSize = (size_t)WiiMem2AlignDownPow2((uintptr)poolSize, 32);
	uintptr poolEnd = poolBase + poolSize;
	if(poolEnd > rawHi || poolEnd <= poolBase) {
		printf("[MEM2-GEN] reservation failed (%u KB requested, arena=%u KB)\n",
		       (unsigned)(poolSize / 1024u),
		       (unsigned)((rawHi - rawLo) / 1024u));
		return;
	}

	SYS_SetArena2Lo((void*)poolEnd);

	sMem2PoolBase = (uint8*)poolBase;
	sMem2PoolEnd = (uint8*)poolEnd;
	sMem2PoolSize = poolSize;
	sMem2FreeList = (Mem2FreeBlock*)sMem2PoolBase;
	sMem2FreeList->prev = nil;
	sMem2FreeList->next = nil;
	sMem2FreeList->size = sMem2PoolSize;
	sMem2PoolReady = true;

	printf("[MEM2-GEN] reserved %u KB generic MEM2 pool from Arena2 (arena=%u KB, base=%p end=%p, lo=%p)\n",
	       (unsigned)(sMem2PoolSize / 1024u),
	       (unsigned)((rawHi - rawLo) / 1024u),
	       (void*)sMem2PoolBase, (void*)sMem2PoolEnd, (void*)SYS_GetArena2Lo());
}

static bool
WiiMem2Owns(const void *ptr)
{
	InitMem2ResourcePool();
	if(!sMem2PoolReady || ptr == nil)
		return false;
	return (const uint8*)ptr >= sMem2PoolBase &&
	       (const uint8*)ptr < sMem2PoolEnd;
}

static void *
WiiMem2Alloc(size_t size, size_t alignment)
{
	InitMem2ResourcePool();
	if(!sMem2PoolReady)
		return nil;

	if(size == 0)
		size = 1;
	if(alignment < 4)
		alignment = 4;

	for(Mem2FreeBlock *block = sMem2FreeList; block; block = block->next) {
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
				sMem2FreeList = tail;
			if(next)
				next->prev = tail;
		} else {
			totalUsed = block->size;
			if(prev)
				prev->next = next;
			else
				sMem2FreeList = next;
			if(next)
				next->prev = prev;
		}

		Mem2AllocHeader *header = (Mem2AllocHeader*)blockStart;
		header->magic = MEM2_RESOURCE_ALLOC_MAGIC;
		header->reserved = 0;
		header->totalSize = totalUsed;
		header->userSize = size;
		((Mem2AllocHeader**)userPtr)[-1] = header;
		return (void*)userPtr;
	}

	return nil;
}

static void
WiiMem2Free(void *ptr)
{
	if(ptr == nil || !WiiMem2Owns(ptr))
		return;

	Mem2AllocHeader *header = ((Mem2AllocHeader**)ptr)[-1];
	if(header == nil ||
	   (uint8*)header < sMem2PoolBase ||
	   (uint8*)header + sizeof(Mem2AllocHeader) > sMem2PoolEnd ||
	   header->magic != MEM2_RESOURCE_ALLOC_MAGIC ||
	   header->totalSize < sizeof(Mem2FreeBlock)) {
		printf("[MEM2-GEN] invalid free ptr=%p header=%p\n", ptr, (void*)header);
		return;
	}

	header->magic = 0;

	Mem2FreeBlock *block = (Mem2FreeBlock*)header;
	block->size = header->totalSize;
	WiiMem2InsertFreeBlock(block);
}

static size_t
WiiMem2UserSize(const void *ptr)
{
	if(ptr == nil || !WiiMem2Owns(ptr))
		return 0;

	Mem2AllocHeader *header = ((Mem2AllocHeader* const*)ptr)[-1];
	if(header == nil ||
	   (uint8*)header < sMem2PoolBase ||
	   (uint8*)header + sizeof(Mem2AllocHeader) > sMem2PoolEnd ||
	   header->magic != MEM2_RESOURCE_ALLOC_MAGIC)
		return 0;

	return header->userSize;
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
	MALLOC_MEM2 = 1;
	InitMem2ResourcePool();
	printf("[MEM2-GEN] newlib malloc MEM2 enabled; rw large threshold=%uKB\n",
	       (unsigned)(WII_RW_MEM2_MALLOC_THRESHOLD / 1024u));
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
#ifdef WII
	if(size >= WII_RW_MEM2_MALLOC_THRESHOLD) {
		void *mem2 = WiiMem2Alloc(size, 32);
		if(mem2) {
			sMem2RwAllocCount++;
			if(sMem2RwAllocCount <= 24 || (sMem2RwAllocCount % 256u) == 0) {
				printf("[MEM2-RW] malloc size=%u ptr=%p count=%u\n",
				       (unsigned)size, mem2, (unsigned)sMem2RwAllocCount);
			}
			if((uint8*)mem2 + size > pMemoryTop)
				pMemoryTop = (uint8*)mem2 + size;
			return mem2;
		}

		sMem2RwFallbackCount++;
		if(sMem2RwFallbackCount <= 24 || (sMem2RwFallbackCount % 128u) == 0) {
			printf("[MEM2-RW] fallback malloc to newlib size=%u count=%u\n",
			       (unsigned)size, (unsigned)sMem2RwFallbackCount);
		}
	}
#endif
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
	if(WiiMem2Owns(ptr)) {
		size_t oldSize = WiiMem2UserSize(ptr);
		void *mem = nil;
		if(size >= WII_RW_MEM2_MALLOC_THRESHOLD)
			mem = WiiMem2Alloc(size, 32);
		if(mem == nil)
			mem = malloc(size);
		if(mem) {
			memcpy(mem, ptr, oldSize < size ? oldSize : size);
			WiiMem2Free(ptr);
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
#ifdef WII
	size_t total = num * size;
	if(total >= WII_RW_MEM2_MALLOC_THRESHOLD) {
		void *mem2 = WiiMem2Alloc(total, 32);
		if(mem2) {
			memset(mem2, 0, total);
			sMem2RwAllocCount++;
			if(sMem2RwAllocCount <= 24 || (sMem2RwAllocCount % 256u) == 0) {
				printf("[MEM2-RW] calloc size=%u ptr=%p count=%u\n",
				       (unsigned)total, mem2, (unsigned)sMem2RwAllocCount);
			}
			if((uint8*)mem2 + total > pMemoryTop)
				pMemoryTop = (uint8*)mem2 + total;
			return mem2;
		}
	}
#endif
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
	if(WiiMem2Owns(ptr)) {
		WiiMem2Free(ptr);
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
	if(size == 0)
		size = 1;
	if(align < 32)
		align = 32;

	void *mem = WiiMem2Alloc(size, align);
	if(mem)
		return mem;

	mem = RwMallocAlign((RwUInt32)size, (RwUInt32)align);
	if(mem) {
		sMem2FallbackCount++;
		if(sMem2FallbackCount <= 16) {
			printf("[MEM2-GEN] fallback to MEM1 size=%u align=%u count=%u\n",
			       (unsigned)size, (unsigned)align,
			       (unsigned)sMem2FallbackCount);
		}
	}
	return mem;
}

void *
MemoryMgrMallocMem2Strict(size_t size, size_t align)
{
	if(size == 0)
		size = 1;
	if(align < 32)
		align = 32;
	return WiiMem2Alloc(size, align);
}

void
MemoryMgrFreeMem2(void *ptr)
{
	if(ptr == nil)
		return;
	if(WiiMem2Owns(ptr)) {
		WiiMem2Free(ptr);
		return;
	}
	RwFreeAlign(ptr);
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

void
MemoryMgrFreeAlignMem2(void *mem)
{
	ASSERT(mem != nil);

	void *addr = *(((void **)mem) - 1);

	ASSERT(addr != nil);

	MemoryMgrFreeMem2(addr);
}
#endif
