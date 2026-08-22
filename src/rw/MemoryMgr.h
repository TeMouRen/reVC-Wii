#ifndef __GTA_MEMORYMGR_H__
#define __GTA_MEMORYMGR_H__

extern RwMemoryFunctions memFuncs;
void InitMemoryMgr(void);

void *MemoryMgrMalloc(size_t size);
void *MemoryMgrRealloc(void *ptr, size_t size);
void *MemoryMgrCalloc(size_t num, size_t size);
void MemoryMgrFree(void *ptr);

void *RwMallocAlign(RwUInt32 size, RwUInt32 align);
void RwFreeAlign(void *mem);

#ifdef WII
enum WiiStreamingPressure
{
	WII_STREAM_PRESSURE_GENERIC = 1u,
	WII_STREAM_PRESSURE_NEWLIB  = 2u,
	WII_STREAM_PRESSURE_GX      = 4u
};

struct WiiMemoryPoolSnapshot
{
	RwUInt32 genericUsed;
	RwUInt32 genericFree;
	RwUInt32 genericLargest;
	RwUInt32 audioUsed;
	RwUInt32 audioFree;
	RwUInt32 audioLargest;
	RwUInt32 newlibUsed;
	RwUInt32 newlibFree;
	RwUInt32 rawArena2Remaining;
	RwUInt32 gxUsed;
	RwUInt32 gxFree;
	RwUInt32 gxLargest;
};

struct WiiMemoryPoolUsage
{
	RwUInt32 genericUsed;
	RwUInt32 audioUsed;
	RwUInt32 newlibUsed;
	RwUInt32 rawArena2Remaining;
	RwUInt32 gxUsed;
};

struct WiiMemoryArenaStats
{
	RwUInt32 base;
	RwUInt32 end;
	RwUInt32 capacity;
	RwUInt32 used;
	RwUInt32 free;
	RwUInt32 largest;
	RwUInt32 peak;
	RwUInt32 allocCount;
	RwUInt32 failCount;
};

struct WiiMemoryPhase0Counters
{
	RwUInt32 requestPending;
	RwUInt32 requestRetryCount;
	RwUInt32 hardFallbackCount;
	RwInt32 txdFailureCount;
};

struct WiiMemoryResourceAttribution
{
	RwUInt32 mask;
	RwInt32 ownerGenericBytes;
	RwInt32 ownerNewlibBytes;
	RwInt32 ownerGxBytes;
	RwInt32 ownerGenericKiB;
	RwInt32 ownerNewlibKiB;
	RwInt32 ownerGxKiB;
	RwInt32 externalGenericBytes;
	RwInt32 externalNewlibBytes;
	RwInt32 externalGxBytes;
};

typedef void (*WiiMemoryResidentDeltaCallback)(RwUInt16 ownerStreamId,
	RwUInt32 poolBit, RwInt32 deltaBytes);

static const RwUInt16 WII_MEMORY_RESOURCE_OWNER_UNKNOWN = 0xffffu;

void *MemoryMgrMallocMem2(size_t size, size_t align);
void MemoryMgrFreeMem2(void *ptr);
void *MemoryMgrMallocMem2Strict(size_t size, size_t align);
void *MemoryMgrReallocMem2Strict(void *ptr, size_t size, size_t align);
void *MemoryMgrMallocAlignMem2(size_t size, size_t align);
void MemoryMgrFreeAlignMem2(void *mem);
void *MemoryMgrMallocAlignMem2Strict(size_t size, size_t align);
void *MemoryMgrMallocAudioMem2(size_t size, size_t align);
void MemoryMgrFreeAudioMem2(void *ptr);
void *MemoryMgrMallocAudioMem2Strict(size_t size, size_t align);
void *MemoryMgrReallocAudioMem2Strict(void *ptr, size_t size, size_t align);
void *MemoryMgrMallocAlignAudioMem2(size_t size, size_t align);
void MemoryMgrFreeAlignAudioMem2(void *mem);
void *MemoryMgrMallocAlignAudioMem2Strict(size_t size, size_t align);
extern "C" int WiiMemoryOwnsGenericMem2(const void *ptr);
extern "C" int WiiMemoryOwnsAudioMem2(const void *ptr);
extern "C" void WiiMemoryGetGenericMem2Stats(RwUInt32 *capacity, RwUInt32 *used,
	RwUInt32 *freeBytes, RwUInt32 *largestBytes, RwUInt32 *peakBytes,
	RwUInt32 *allocCount, RwUInt32 *failCount);
extern "C" void WiiMemoryGetAudioMem2Stats(RwUInt32 *capacity, RwUInt32 *used,
	RwUInt32 *freeBytes, RwUInt32 *largestBytes, RwUInt32 *peakBytes,
	RwUInt32 *allocCount, RwUInt32 *failCount);
extern "C" void WiiMemoryGetGenericMem2ArenaStats(WiiMemoryArenaStats *stats);
extern "C" void WiiMemoryGetAudioMem2ArenaStats(WiiMemoryArenaStats *stats);
extern "C" void WiiMemorySetResidentDeltaCallback(WiiMemoryResidentDeltaCallback callback);
extern "C" void WiiMemoryBeginResourceAttribution(RwUInt16 ownerStreamId);
extern "C" RwUInt16 WiiMemoryGetResourceAttributionOwner(void);
extern "C" void WiiMemoryRecordResidentPool(RwUInt32 poolBit);
extern "C" void WiiMemoryRecordResidentDelta(RwUInt16 ownerStreamId,
	RwUInt32 poolBit, RwInt32 deltaBytes);
extern "C" void WiiMemoryEndResourceAttribution(WiiMemoryResourceAttribution *result);
extern "C" void WiiMemoryGetPoolUsage(WiiMemoryPoolUsage *usage);
extern "C" void WiiMemoryGetPoolSnapshot(WiiMemoryPoolSnapshot *snapshot);
extern "C" void WiiMemoryPhase0ReportRunStart(void);
extern "C" void WiiMemoryPhase0ReportSnapshot(const WiiMemoryPhase0Counters *counters);
extern "C" RwUInt32 WiiMemoryGetStreamingPressure(void);
extern "C" RwUInt32 WiiMemoryGetStreamingPressureForSnapshot(const WiiMemoryPoolSnapshot *snapshot);
extern "C" RwUInt32 WiiMemoryGetStreamingPressureDeficit(const WiiMemoryPoolSnapshot *snapshot,
	RwUInt32 poolBit, size_t requestedBytes);
extern "C" void WiiMemoryDumpStats(const char *reason);
extern "C" void WiiMemoryEnsureFileOpenReserve(void);
extern "C" int WiiMemoryReleaseFileOpenReserve(const char *reason);
#endif

#endif // __GTA_MEMORYMGR_H__
