#include "common.h"

#include "General.h"
#include "Pad.h"
#include "Hud.h"
#include "Text.h"
#include "Clock.h"
#include "Renderer.h"
#include "ModelInfo.h"
#include "TxdStore.h"
#include "ModelIndices.h"
#include "Pools.h"
#include "Wanted.h"
#include "Directory.h"
#include "RwHelper.h"
#include "World.h"
#include "Entity.h"
#include "Physical.h"
#include "FileMgr.h"
#include "FileLoader.h"
#include "Zones.h"
#include "Radar.h"
#include "Camera.h"
#include "Record.h"
#include "CarCtrl.h"
#include "Population.h"
#include "Gangs.h"
#include "CutsceneMgr.h"
#include "CdStream.h"
#include "Streaming.h"
#include "Replay.h"
#include "main.h"
#include "ColStore.h"
#include "DMAudio.h"
#include "Script.h"
#include "MemoryMgr.h"
#include "MemoryHeap.h"
#include "Font.h"
#include "Frontend.h"
#include "VarConsole.h"

#ifdef WII
#include <ogc/lwp_watchdog.h>
#include "gxmemory.h"

#ifndef WII_ISLAND_READ_LIVENESS
#define WII_ISLAND_READ_LIVENESS 0
#endif
#ifndef WII_STREAM_PS2_TRANSITION_PURGE
#define WII_STREAM_PS2_TRANSITION_PURGE 0
#endif
#ifndef WII_STREAM_PS2_WORLD_SCAN_RADIUS
#define WII_STREAM_PS2_WORLD_SCAN_RADIUS 0
#endif
#ifndef WII_STREAM_GX_HEADROOM_GUARD
#define WII_STREAM_GX_HEADROOM_GUARD 0
#endif
#ifndef WII_STREAM_ATOMIC_BIG_HANDOFF
#define WII_STREAM_ATOMIC_BIG_HANDOFF 0
#endif
#ifndef WII_STREAM_P7_BLOCKING_HANDOFF
#define WII_STREAM_P7_BLOCKING_HANDOFF 0
#endif
#ifndef WII_STREAM_SPLASH_VISUAL_GATE
#define WII_STREAM_SPLASH_VISUAL_GATE 0
#endif
#ifndef WII_STREAM_P7_VISIBLE_TXD_GUARD
#define WII_STREAM_P7_VISIBLE_TXD_GUARD 0
#endif

class WiiTextureStreamContextScope
{
public:
	WiiTextureStreamContextScope(int32 modelId, CBaseModelInfo *mi)
	{
		const int32 txdSlot = mi ? mi->GetTxdSlot() : -1;
		rw::gxSetTextureStreamContext(modelId,
			mi ? mi->GetModelName() : nil,
			txdSlot,
			txdSlot >= 0 ? CTxdStore::GetTxdName(txdSlot) : nil);
	}

	~WiiTextureStreamContextScope()
	{
		rw::gxClearTextureStreamContext();
	}
};

// The soft cache budget grows only while every allocator has comfortable
// headroom. Pool pressure is reclaimed at the owning pool first; the archive
// proxy is not an eviction target. A global fallback is reserved for pressure
// that does not include GX, whose victims must release GX-owned bytes.
#ifndef WII_STREAM_ADAPTIVE_ARCHIVE_CEILING
#define WII_STREAM_ADAPTIVE_ARCHIVE_CEILING 0
#endif
#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING != 0 && WII_STREAM_ADAPTIVE_ARCHIVE_CEILING != 1
#error "WII_STREAM_ADAPTIVE_ARCHIVE_CEILING must be 0 or 1"
#endif
#ifndef WII_STREAM_ARCHIVE_GLOBAL_LRU
#define WII_STREAM_ARCHIVE_GLOBAL_LRU 0
#endif
#if WII_STREAM_ARCHIVE_GLOBAL_LRU != 0 && WII_STREAM_ARCHIVE_GLOBAL_LRU != 1
#error "WII_STREAM_ARCHIVE_GLOBAL_LRU must be 0 or 1"
#endif
static const size_t WII_STREAMING_MEMORY_BUDGET_LOW  = 16u * 1024u * 1024u;
static const size_t WII_STREAMING_MEMORY_BUDGET_HIGH = 20u * 1024u * 1024u;
static const size_t WII_STREAMING_ARCHIVE_CEILING_BASE = 24u * 1024u * 1024u;
#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING
static const size_t WII_STREAMING_ARCHIVE_CEILING_HIGH = 28u * 1024u * 1024u;
static const size_t WII_STREAMING_ARCHIVE_CEILING_STEP = 512u * 1024u;
static const uint32 WII_STREAMING_ARCHIVE_GROW_MS = 1000u;
static const uint32 WII_STREAMING_ARCHIVE_HARD_PRESSURE_MS = 2000u;
static const uint32 WII_STREAMING_ARCHIVE_PRESSURE_SAMPLE_GAP_MS = 250u;
static const uint32 WII_STREAMING_ARCHIVE_RECOVERY_MS = 5000u;
static const size_t WII_STREAM_ARCHIVE_GROW_GENERIC_FREE_BYTES = 2560u * 1024u;
static const size_t WII_STREAM_ARCHIVE_GROW_GENERIC_LARGEST_BYTES = 1u * 1024u * 1024u;
static const size_t WII_STREAM_ARCHIVE_GROW_NEWLIB_RAW_BYTES = 8u * 1024u * 1024u;
static const size_t WII_STREAM_ARCHIVE_GROW_GX_FREE_BYTES = 6u * 1024u * 1024u;
static const size_t WII_STREAM_ARCHIVE_GROW_GX_LARGEST_BYTES = 2u * 1024u * 1024u;
#endif
static const size_t WII_STREAM_ARCHIVE_KEEP_GENERIC_FREE_BYTES = 2u * 1024u * 1024u;
static const size_t WII_STREAM_ARCHIVE_KEEP_GENERIC_LARGEST_BYTES = 512u * 1024u;
static const size_t WII_STREAM_ARCHIVE_KEEP_NEWLIB_RAW_BYTES = 4u * 1024u * 1024u;
static const size_t WII_STREAM_ARCHIVE_KEEP_GX_FREE_BYTES = 3u * 1024u * 1024u;
static const size_t WII_STREAM_ARCHIVE_KEEP_GX_LARGEST_BYTES = 1u * 1024u * 1024u;
#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING
static const size_t WII_STREAM_ARCHIVE_MAX_ELASTIC_DEBT_BYTES = 2u * 1024u * 1024u;
#endif
static const size_t WII_STREAMING_MEMORY_BUDGET_STEP = 512u * 1024u;
static const uint32 WII_STREAMING_MEMORY_BUDGET_GROW_MS = 1000u;
static const uint32 WII_STREAMING_MEMORY_BUDGET_PRESSURE_MS = 2000u;
static const size_t WII_STREAM_EXPAND_GENERIC_FREE_BYTES = 4u * 1024u * 1024u;
static const size_t WII_STREAM_EXPAND_GENERIC_LARGEST_BYTES = 1u * 1024u * 1024u;
static const size_t WII_STREAM_EXPAND_NEWLIB_FREE_BYTES = 2u * 1024u * 1024u;
static const size_t WII_STREAM_EXPAND_GX_FREE_BYTES = 3u * 1024u * 1024u;
static const size_t WII_STREAM_EXPAND_GX_LARGEST_BYTES = 512u * 1024u;

// Island changes are staged over several frames.  The target set is kept
// deliberately small (nearby world data, collision, radar and island LOD)
// so the old island does not have to coexist in full with the new one.
static const uint32 WII_ISLAND_PREFETCH_INTERVAL_MS = 250u;
static const uint32 WII_ISLAND_SOFT_TIMEOUT_MS = 6000u;
static const uint32 WII_ISLAND_HARD_TIMEOUT_MS = 12000u;
static const uint32 WII_ISLAND_ABORT_TIMEOUT_MS = 20000u;
static const uint32 WII_ISLAND_VISUAL_HANDOFF_TIMEOUT_MS = 8000u;
static const float WII_ISLAND_PREFETCH_DISTANCE = 220.0f;
static const float WII_ISLAND_PREFETCH_MIN_SPEED = 0.05f;
static const float WII_ISLAND_REQUIRED_BIG_RADIUS = 320.0f;
static const float WII_ISLAND_LANDING_CORE_RADIUS = 200.0f;
static const size_t WII_ISLAND_TRANSITION_RESERVE = 2u * 1024u * 1024u;
static const int32 WII_ISLAND_RETIRE_SCAN_PER_FRAME = 64;
static const int32 WII_ISLAND_RETIRE_REMOVE_PER_FRAME = 8;
static const uint32 WII_ISLAND_SPLASH_READ_DELAY_MS = 34u;
static const uint32 WII_ISLAND_SPLASH_MIN_DISPLAY_MS = 250u;

static const uint32 WII_STREAM_KEEP_FAIR_WAIT_MS = 2000u;
static const uint32 WII_STREAM_HARD_FAIR_WAIT_MS = 3000u;
static const uint32 WII_STREAM_RADAR_DEADLINE_MS = 500u;
static const uint8 WII_STREAM_FOREGROUND_BURST = 4u;
static const int32 WII_STREAM_PRESSURE_MAX_REMOVALS = 6;
static const size_t WII_STREAM_ARCHIVE_TRANSITION_GUARD = 512u * 1024u;
static const size_t WII_STREAM_MIN_POOL_PROGRESS_BYTES = 32u * 1024u;
static const uint32 WII_STREAM_PRESSURE_HARD_MASK =
	WII_STREAM_PRESSURE_GENERIC | WII_STREAM_PRESSURE_NEWLIB |
	WII_STREAM_PRESSURE_GX;
#if WII_STREAM_P7_VISIBLE_TXD_GUARD
static const uint32 WII_STREAM_P7_RECENT_VISIBLE_TXD_GRACE_MS = 1000u;
#endif
static const uint32 WII_STREAM_PRESSURE_GX_ADMISSION = 8u;
#if WII_STREAM_GX_HEADROOM_GUARD
static const size_t WII_STREAM_GX_HEADROOM_GUARD_FREE_BYTES =
	WII_STREAM_ARCHIVE_KEEP_GX_FREE_BYTES;
static const size_t WII_STREAM_GX_HEADROOM_GUARD_LARGEST_BYTES =
	WII_STREAM_ARCHIVE_KEEP_GX_LARGEST_BYTES;
#endif
enum eWiiStreamRequestClass {
	WII_STREAM_REQUEST_NORMAL = 0,
	WII_STREAM_REQUEST_TRANSITION_PREFETCH,
	WII_STREAM_REQUEST_WORLD_VISIBLE,
	WII_STREAM_REQUEST_RADAR
};

static const uint16 WII_STREAM_RESIDENT_UNKNOWN_KIB = 0xffffu;
static const uint16 WII_STREAM_RESIDENT_SATURATED_KIB = 0xfffeu;

struct WiiStreamResidentCost {
	uint16 genericKiB;
	uint16 newlibKiB;
	uint16 gxKiB;
};

static_assert(sizeof(WiiStreamResidentCost) == 6,
              "Wii stream resident cost must stay three uint16 KiB fields");

static uint8 gWiiStreamResourcePoolMask[NUMSTREAMINFO];
static uint32 gWiiStreamQueuedAtMs[NUMSTREAMINFO];
static uint8 gWiiStreamDispatched[(NUMSTREAMINFO + 7) / 8];
static uint8 gWiiStreamDispatchedFlags[NUMSTREAMINFO];
static uint8 gWiiStreamDispatchedRequestClass[NUMSTREAMINFO];
static uint8 gWiiStreamRequestClass[NUMSTREAMINFO];
static uint8 gWiiStreamWorldRequest[(NUMSTREAMINFO + 7) / 8];
static uint8 gWiiStreamDispatchedWorldRequest[(NUMSTREAMINFO + 7) / 8];
static uint8 gWiiStreamWorldLoadedThisFrame[(NUMSTREAMINFO + 7) / 8];
#if WII_STREAM_P7_VISIBLE_TXD_GUARD
static uint32 gWiiStreamRecentVisibleTxdMs[STREAM_OFFSET_COL - STREAM_OFFSET_TXD];
#endif
static uint32 gWiiStreamWorldLoadFrame = UINT32_MAX;
static uint32 gWiiStreamSameFrameVictimDeferrals;
static bool gWiiStreamSynchronousLoad;
static uint32 gWiiStreamFrameWorkFrame = UINT32_MAX;
static WiiStreamingFrameWork gWiiStreamFrameWork;
static uint32 gWiiStreamDependencyUnwindFrame = UINT32_MAX;
static size_t gWiiStreamMemoryBudget = WII_STREAMING_MEMORY_BUDGET_LOW;
static size_t gWiiStreamArchiveCeiling = WII_STREAMING_ARCHIVE_CEILING_BASE;
static uint32 gWiiStreamBudgetLastGrowthMs;
static uint32 gWiiStreamArchiveLastAdjustMs;
static uint32 gWiiStreamArchivePressureSinceMs;
static uint32 gWiiStreamArchivePressureLastSeenMs;
static uint32 gWiiStreamArchiveRecoverySinceMs;
static bool gWiiStreamArchiveRetreated;
static uint32 gWiiStreamArchiveRetireFrame = UINT32_MAX;
static uint32 gWiiStreamBudgetPressureSinceMs;
static uint8 gWiiStreamForegroundServicesSinceFair;
static uint32 gWiiStreamResidentSaturationCount;
static uint32 gWiiStreamResidentUnderflowCount;
#if WII_STREAM_MEMORY_DIAGNOSTICS
static uint32 gWiiPhase0RequestRetryCount;
static uint32 gWiiPhase0HardFallbackCount;
#endif
#if WII_STREAM_MEMORY_DIAGNOSTICS && WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
static uint32 gWiiStreamDiagLastRequestMs[NUMSTREAMINFO];
static uint32 gWiiStreamDiagDispatchMs[NUMSTREAMINFO];
static uint32 gWiiStreamDiagQueueWaitMs[NUMSTREAMINFO];
static uint32 gWiiStreamDiagLastLoadMs[NUMSTREAMINFO];
static uint32 gWiiStreamDiagLastWorldVisibleMs[NUMSTREAMINFO];
static uint8 gWiiStreamDiagLastRequestClass[NUMSTREAMINFO];
static uint8 gWiiStreamDiagLastDispatchClass[NUMSTREAMINFO];
static void WiiStreamDiagSetVictimCounterfactual(int32 selectedId,
	int32 oldestId, uint32 oldestAgeMs);
#endif
static RwUInt32 gWiiStreamResidentLastRawArena2Remaining;
static bool gWiiStreamResidentHasLastRawArena2;
enum eWiiStreamDispatchClass {
	WII_STREAM_DISPATCH_NORMAL = 0,
	WII_STREAM_DISPATCH_PRIORITY_CHAIN,
	WII_STREAM_DISPATCH_FAIR
};
static int32 gWiiStreamSelectedDispatchId = -1;
static uint8 gWiiStreamSelectedDispatchClass;
static uint32 gWiiStreamSelectedDispatchWaitMs;

enum eWiiIslandTransitionPhase {
	WII_ISLAND_IDLE = 0,
	WII_ISLAND_PREFETCH,
	WII_ISLAND_READ,
	WII_ISLAND_COMMIT,
	WII_ISLAND_RETIRE
};

static eWiiIslandTransitionPhase gWiiIslandPhase = WII_ISLAND_IDLE;
static eLevelName gWiiIslandSourceLevel = LEVEL_GENERIC;
static eLevelName gWiiIslandTargetLevel = LEVEL_GENERIC;
static CVector gWiiIslandWorkPosition;
static CVector gWiiIslandSourceReturnPosition;
static CVector gWiiIslandLastStablePosition;
static uint32 gWiiIslandStartedAtMs;
static uint32 gWiiIslandReadStartedAtMs;
static uint32 gWiiIslandCommittedAtMs;
static uint32 gWiiIslandLastRequestAtMs;
static uint32 gWiiIslandLastSeenAtMs;
static CVector gWiiIslandLastRequestPosition;
static bool gWiiIslandCaptureRequests;
static bool gWiiIslandCaptureTemporaryPin;
static bool gWiiIslandCapturePriority;
static bool gWiiIslandInternalRequeue;
static bool gWiiIslandSplashVisible;
static bool gWiiIslandSplashPrepared;
static bool gWiiIslandSplashPrepareAttempted;
static bool gWiiIslandPauseOwned;
static bool gWiiIslandHardFallbackUsed;
static bool gWiiIslandPrefetchReadyLogged;
static bool gWiiIslandNeedsReadCoreRebuild;
static bool gWiiIslandRetireProtectionActive;
static bool gWiiIslandVisualHandoffPending;
static bool gWiiIslandRetireComplete;
#if WII_STREAM_ATOMIC_BIG_HANDOFF
static bool gWiiIslandAtomicBigReady;
static CVector gWiiIslandAtomicHandoffPosition;
#endif
#if WII_STREAM_PS2_TRANSITION_PURGE
static bool gWiiIslandPs2PurgeDone;
#endif
static uint8 gWiiIslandFlags[NUMSTREAMINFO];
static WiiStreamResidentCost gWiiStreamResidentCost[NUMSTREAMINFO];
static uint8 gWiiIslandProtectedCols[COLSTORESIZE];
static uint8 gWiiIslandProtectedRadarTxds[TXDSTORESIZE];
// This is a boolean set indexed by model id. Keep it packed so the island
// transition bookkeeping does not consume one byte per model in BSS.
static uint8 gWiiIslandTargetBuildingModels[(MODELINFOSIZE + 7) / 8];
static_assert(ARRAY_SIZE(gWiiIslandFlags) == NUMSTREAMINFO,
              "packed island flag array size changed");
static_assert(ARRAY_SIZE(gWiiStreamResidentCost) == NUMSTREAMINFO,
              "Wii resident ledger size changed");
static eLevelName gWiiIslandLastStableLevel = LEVEL_GENERIC;
static bool gWiiIslandHasLastStablePosition;
static int32 gWiiIslandRetirePool;
static int32 gWiiIslandRetireIndex;

static void WiiIslandTransitionReset(void);
static bool WiiIslandTransitionProtectsStreamId(int32 streamId);
static bool WiiIslandTransitionConversionBudgetActive(void);
static void WiiIslandTransitionCaptureRequest(int32 streamId, int32 flags);
static void WiiIslandObserveExternalRequest(int32 streamId, int32 flags);
static void WiiStreamPromoteRequestClass(int32 streamId, uint8 requestClass);
static void WiiIslandBuildTargetBuildingModelSet(void);
static int32 WiiIslandRequestNearbyBigBuildings(eLevelName level,
	const CVector &position);
#if WII_STREAM_ATOMIC_BIG_HANDOFF || WII_STREAM_SPLASH_VISUAL_GATE
static int32 WiiIslandCountVisualBigBuildings(eLevelName level,
	const CVector &position, bool visibleOnly, int32 *missingOut);
#endif
static int32 WiiIslandRequestLandingCore(eLevelName level,
	const CVector &position, int32 *treadableRequestedOut);
static bool WiiStreamRetireOneHeadroomResource(
	const WiiMemoryPoolSnapshot &snapshot, uint32 pressure,
	uint32 *poolBitOut);

enum eWiiIslandFlag {
	WII_ISLAND_REQUIRED = 1u << 0,
	WII_ISLAND_CREATED_REQUEST = 1u << 1,
	WII_ISLAND_ADDED_DONT_REMOVE = 1u << 2,
	WII_ISLAND_ADDED_PRIORITY = 1u << 3,
	WII_ISLAND_EXTERNAL_REQUEST = 1u << 4,
	WII_ISLAND_EXTERNAL_DONT_REMOVE = 1u << 5,
	WII_ISLAND_EXTERNAL_PRIORITY = 1u << 6
};

static bool
WiiIslandHasFlag(int32 streamId, uint8 flag)
{
	return (gWiiIslandFlags[streamId] & flag) != 0;
}

static void
WiiIslandSetFlags(int32 streamId, uint8 flags)
{
	gWiiIslandFlags[streamId] |= flags;
}

static void
WiiIslandSetFlag(int32 streamId, uint8 flag)
{
	gWiiIslandFlags[streamId] |= flag;
}

static void
WiiIslandClearFlags(int32 streamId, uint8 flags)
{
	gWiiIslandFlags[streamId] &= ~flags;
}

static void
WiiIslandClearAllFlags(void)
{
	memset(gWiiIslandFlags, 0, sizeof(gWiiIslandFlags));
}

static bool
WiiIslandHasCapturedFlag(int32 streamId)
{
	return WiiIslandHasFlag(streamId, WII_ISLAND_REQUIRED);
}

static uint16
WiiStreamBytesToResidentKiB(size_t bytes)
{
	size_t kib = (bytes + 1023u) / 1024u;
	return kib >= WII_STREAM_RESIDENT_SATURATED_KIB ?
		WII_STREAM_RESIDENT_SATURATED_KIB : (uint16)kib;
}

static uint16
WiiStreamObservedResidentKiB(RwInt32 deltaBytes, uint8 mask, uint8 poolBit)
{
	if(deltaBytes > 0)
		return WiiStreamBytesToResidentKiB((size_t)deltaBytes);
	if(mask & poolBit)
		return WII_STREAM_RESIDENT_UNKNOWN_KIB;
	return 0;
}

static RwInt32
WiiStreamResidentUsageDelta(RwUInt32 beforeBytes, RwUInt32 afterBytes,
	RwInt32 externalDeltaBytes)
{
	int64 delta = (int64)afterBytes - beforeBytes - externalDeltaBytes;
	if(delta > INT32_MAX)
		return INT32_MAX;
	if(delta < INT32_MIN)
		return INT32_MIN;
	return (RwInt32)delta;
}

static void
WiiStreamClearResidentCost(int32 streamId)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
	memset(&gWiiStreamResidentCost[streamId], 0,
	       sizeof(gWiiStreamResidentCost[streamId]));
}

static void
WiiStreamApplyResidentKiBDelta(int32 streamId, uint8 poolBit, RwInt32 deltaKiB);

static void
WiiStreamApplyResidentDelta(int32 streamId, uint8 poolBit, RwInt32 deltaBytes)
{
	if(deltaBytes == 0)
		return;
	int64 magnitude = deltaBytes > 0 ? deltaBytes : -(int64)deltaBytes;
	int64 deltaKiB = (magnitude + 1023) / 1024;
	if(deltaKiB > INT32_MAX)
		deltaKiB = INT32_MAX;
	WiiStreamApplyResidentKiBDelta(streamId, poolBit,
		deltaBytes > 0 ? (RwInt32)deltaKiB : -(RwInt32)deltaKiB);
}

static void
WiiStreamApplyResidentKiBDelta(int32 streamId, uint8 poolBit, RwInt32 deltaKiB)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;

	WiiStreamResidentCost &cost = gWiiStreamResidentCost[streamId];
	uint16 *value;
	switch(poolBit){
	case WII_STREAM_PRESSURE_GENERIC: value = &cost.genericKiB; break;
	case WII_STREAM_PRESSURE_NEWLIB: value = &cost.newlibKiB; break;
	case WII_STREAM_PRESSURE_GX: value = &cost.gxKiB; break;
	default: return;
	}

	if(deltaKiB == 0 || *value == WII_STREAM_RESIDENT_UNKNOWN_KIB)
		return;

	if(deltaKiB > 0){
		uint32 amount = (uint32)deltaKiB;
		if(amount >= WII_STREAM_RESIDENT_SATURATED_KIB ||
		   *value >= WII_STREAM_RESIDENT_SATURATED_KIB - amount){
			if(*value != WII_STREAM_RESIDENT_SATURATED_KIB)
				gWiiStreamResidentSaturationCount++;
			*value = WII_STREAM_RESIDENT_SATURATED_KIB;
		}else
			*value += (uint16)amount;
	}else{
		uint32 amount = (uint32)(-(int64)deltaKiB);
		if(amount >= *value){
			if(amount > *value)
				gWiiStreamResidentUnderflowCount++;
			*value = 0;
		}else
			*value -= (uint16)amount;
	}
}

static void
WiiStreamApplyObservedResidentDelta(int32 streamId, uint8 poolBit,
	RwInt32 deltaBytes, uint8 mask)
{
	if(deltaBytes > 0){
		WiiStreamApplyResidentDelta(streamId, poolBit, deltaBytes);
		return;
	}
	if(deltaBytes < 0 || mask & poolBit){
		WiiStreamResidentCost &cost = gWiiStreamResidentCost[streamId];
		switch(poolBit){
		case WII_STREAM_PRESSURE_GENERIC: cost.genericKiB = WII_STREAM_RESIDENT_UNKNOWN_KIB; break;
		case WII_STREAM_PRESSURE_NEWLIB: cost.newlibKiB = WII_STREAM_RESIDENT_UNKNOWN_KIB; break;
		case WII_STREAM_PRESSURE_GX: cost.gxKiB = WII_STREAM_RESIDENT_UNKNOWN_KIB; break;
		}
	}
}

static void
WiiStreamCommitResidentCost(int32 streamId, bool replace, uint8 mask,
	const WiiMemoryPoolUsage &before, const WiiMemoryPoolUsage &after,
	const WiiMemoryResourceAttribution &attribution)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
	if(replace)
		WiiStreamClearResidentCost(streamId);

	RwInt32 genericDelta = WiiStreamResidentUsageDelta(before.genericUsed,
		after.genericUsed, attribution.externalGenericBytes);
	RwInt32 newlibDelta = WiiStreamResidentUsageDelta(before.newlibUsed,
		after.newlibUsed, attribution.externalNewlibBytes);
	RwInt32 gxDelta = WiiStreamResidentUsageDelta(before.gxUsed,
		after.gxUsed, attribution.externalGxBytes);

	if(replace){
		WiiStreamResidentCost &cost = gWiiStreamResidentCost[streamId];
		cost.genericKiB = WiiStreamObservedResidentKiB(genericDelta, mask,
			WII_STREAM_PRESSURE_GENERIC);
		cost.newlibKiB = WiiStreamObservedResidentKiB(newlibDelta, mask,
			WII_STREAM_PRESSURE_NEWLIB);
		cost.gxKiB = attribution.ownerGxKiB > 0 ?
			(attribution.ownerGxKiB >= WII_STREAM_RESIDENT_SATURATED_KIB ?
			 WII_STREAM_RESIDENT_SATURATED_KIB : (uint16)attribution.ownerGxKiB) :
			WiiStreamObservedResidentKiB(gxDelta, mask, WII_STREAM_PRESSURE_GX);
		return;
	}

	WiiStreamApplyObservedResidentDelta(streamId, WII_STREAM_PRESSURE_GENERIC,
		genericDelta, mask);
	WiiStreamApplyObservedResidentDelta(streamId, WII_STREAM_PRESSURE_NEWLIB,
		newlibDelta, mask);
	if(attribution.ownerGxKiB != 0)
		WiiStreamApplyResidentKiBDelta(streamId, WII_STREAM_PRESSURE_GX,
			attribution.ownerGxKiB);
	else
		WiiStreamApplyObservedResidentDelta(streamId, WII_STREAM_PRESSURE_GX,
			gxDelta, mask);
}

static void
WiiStreamRecordResidentDelta(RwUInt16 ownerStreamId, RwUInt32 poolBit,
	RwInt32 deltaBytes)
{
	if(ownerStreamId == WII_MEMORY_RESOURCE_OWNER_UNKNOWN ||
	   ownerStreamId >= NUMSTREAMINFO)
		return;
	uint8 state = CStreaming::ms_aInfoForModel[ownerStreamId].m_loadState;
	if(state != STREAMSTATE_LOADED && state != STREAMSTATE_STARTED)
		return;
	WiiStreamApplyResidentDelta((int32)ownerStreamId, (uint8)poolBit,
		deltaBytes);
}

static void
WiiStreamRebuildResidentCostEntry(RwUInt16 ownerStreamId, RwUInt32 poolBit,
	RwUInt32 bytes)
{
	if(ownerStreamId >= NUMSTREAMINFO || bytes == 0)
		return;
	uint8 state = CStreaming::ms_aInfoForModel[ownerStreamId].m_loadState;
	if(state != STREAMSTATE_LOADED && state != STREAMSTATE_STARTED)
		return;
	if(bytes > INT32_MAX)
		bytes = INT32_MAX;
	WiiStreamApplyResidentDelta((int32)ownerStreamId, (uint8)poolBit,
		(RwInt32)bytes);
}

static void
WiiStreamRebuildResidentCosts(void)
{
	for(int32 streamId = 0; streamId < NUMSTREAMINFO; streamId++){
		WiiStreamResidentCost &cost = gWiiStreamResidentCost[streamId];
		memset(&cost, 0, sizeof(cost));
		uint8 state = CStreaming::ms_aInfoForModel[streamId].m_loadState;
		if(state == STREAMSTATE_LOADED || state == STREAMSTATE_STARTED){
			// GX and generic can be rebuilt from texture entries. Newlib has no
			// owner walk yet, so keep it explicitly observationally unknown.
			cost.newlibKiB = WII_STREAM_RESIDENT_UNKNOWN_KIB;
		}
	}

	rw::gx::texPoolVisitOwnerResidency(WiiStreamRebuildResidentCostEntry);

	for(int32 streamId = 0; streamId < NUMSTREAMINFO; streamId++){
		uint8 state = CStreaming::ms_aInfoForModel[streamId].m_loadState;
		if(state != STREAMSTATE_LOADED && state != STREAMSTATE_STARTED)
			continue;
		WiiStreamResidentCost &cost = gWiiStreamResidentCost[streamId];
		if(cost.genericKiB == 0)
			cost.genericKiB = WII_STREAM_RESIDENT_UNKNOWN_KIB;
		if(cost.gxKiB == 0)
			cost.gxKiB = WII_STREAM_RESIDENT_UNKNOWN_KIB;
	}
}

static const char*
WiiIslandTransitionPhaseName(void)
{
	switch(gWiiIslandPhase){
	case WII_ISLAND_PREFETCH: return "prefetch";
	case WII_ISLAND_READ: return "read";
	case WII_ISLAND_COMMIT: return "commit";
	case WII_ISLAND_RETIRE: return "retire";
	default: return "idle";
	}
}

static uint32
WiiIslandWallClockMs(void)
{
	return (uint32)ticks_to_millisecs(gettime());
}

static void
WiiIslandTransitionReset(void)
{
	if(gWiiIslandPauseOwned && CTimer::GetIsCodePaused())
		CTimer::SetCodePause(false);
	if(gWiiIslandSplashVisible)
		WiiEndIslandTransitionSplash();
	gWiiIslandPhase = WII_ISLAND_IDLE;
	gWiiIslandSourceLevel = LEVEL_GENERIC;
	gWiiIslandTargetLevel = LEVEL_GENERIC;
	gWiiIslandWorkPosition = CVector(0.0f, 0.0f, 0.0f);
	gWiiIslandSourceReturnPosition = CVector(0.0f, 0.0f, 0.0f);
	gWiiIslandStartedAtMs = 0;
	gWiiIslandReadStartedAtMs = 0;
	gWiiIslandCommittedAtMs = 0;
	gWiiIslandLastRequestAtMs = 0;
	gWiiIslandLastSeenAtMs = 0;
	gWiiIslandLastRequestPosition = CVector(0.0f, 0.0f, 0.0f);
	gWiiIslandCaptureRequests = false;
	gWiiIslandCaptureTemporaryPin = false;
	gWiiIslandCapturePriority = false;
	gWiiIslandInternalRequeue = false;
	gWiiIslandSplashVisible = false;
	gWiiIslandSplashPrepared = false;
	gWiiIslandSplashPrepareAttempted = false;
	gWiiIslandPauseOwned = false;
	gWiiIslandHardFallbackUsed = false;
	gWiiIslandPrefetchReadyLogged = false;
	gWiiIslandNeedsReadCoreRebuild = false;
	gWiiIslandRetireProtectionActive = false;
	gWiiIslandVisualHandoffPending = false;
	gWiiIslandRetireComplete = false;
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	gWiiIslandAtomicBigReady = false;
	gWiiIslandAtomicHandoffPosition = CVector(0.0f, 0.0f, 0.0f);
#endif
#if WII_STREAM_PS2_TRANSITION_PURGE
	gWiiIslandPs2PurgeDone = false;
#endif
	gWiiIslandRetirePool = 0;
	gWiiIslandRetireIndex = -1;
	WiiIslandClearAllFlags();
	memset(gWiiIslandProtectedCols, 0, sizeof(gWiiIslandProtectedCols));
	memset(gWiiIslandProtectedRadarTxds, 0,
	       sizeof(gWiiIslandProtectedRadarTxds));
	memset(gWiiIslandTargetBuildingModels, 0,
	       sizeof(gWiiIslandTargetBuildingModels));
}

static bool
WiiIslandTransitionProtectsStreamId(int32 streamId)
{
	return streamId >= 0 && streamId < NUMSTREAMINFO &&
	       (gWiiIslandPhase == WII_ISLAND_PREFETCH ||
	        gWiiIslandPhase == WII_ISLAND_READ ||
	        gWiiIslandPhase == WII_ISLAND_COMMIT ||
	        (gWiiIslandPhase == WII_ISLAND_RETIRE &&
	         gWiiIslandRetireProtectionActive)) &&
	       WiiIslandHasCapturedFlag(streamId);
}

static bool
WiiIslandTransitionConversionBudgetActive(void)
{
	return gWiiIslandPhase == WII_ISLAND_PREFETCH ||
	       gWiiIslandPhase == WII_ISLAND_READ;
}

static eLevelName
WiiIslandPredictedLevel(const CVector &position)
{
	const CVector &speed = FindPlayerSpeed();
	if(speed.Magnitude2D() < WII_ISLAND_PREFETCH_MIN_SPEED)
		return LEVEL_GENERIC;

	CVector predicted = position + speed * WII_ISLAND_PREFETCH_DISTANCE;
	eLevelName level = CTheZones::GetLevelFromPosition(&predicted);
	if(level == LEVEL_GENERIC || level == CGame::currLevel)
		return LEVEL_GENERIC;
	return level;
}

static void
WiiIslandRequeueModel(int32 streamId, uint8 flags)
{
	bool oldInternalRequeue = gWiiIslandInternalRequeue;
	gWiiIslandInternalRequeue = true;
	CStreaming::RequestModel(streamId, flags);
	gWiiIslandInternalRequeue = oldInternalRequeue;
}

static void
WiiIslandReleaseTemporaryPin(int32 streamId, bool cancelQueued)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
	bool addedDontRemove = WiiIslandHasFlag(streamId, WII_ISLAND_ADDED_DONT_REMOVE);
	bool addedPriority = WiiIslandHasFlag(streamId, WII_ISLAND_ADDED_PRIORITY);
	bool createdRequest = WiiIslandHasFlag(streamId, WII_ISLAND_CREATED_REQUEST);
	bool externalRequest = WiiIslandHasFlag(streamId, WII_ISLAND_EXTERNAL_REQUEST);
	bool externalDontRemove = WiiIslandHasFlag(streamId, WII_ISLAND_EXTERNAL_DONT_REMOVE);
	bool externalPriority = WiiIslandHasFlag(streamId, WII_ISLAND_EXTERNAL_PRIORITY);
	if(!addedDontRemove && !addedPriority && !createdRequest){
		WiiIslandClearFlags(streamId,
			WII_ISLAND_EXTERNAL_REQUEST |
			WII_ISLAND_EXTERNAL_DONT_REMOVE |
			WII_ISLAND_EXTERNAL_PRIORITY);
		return;
	}

	CStreamingInfo *info = &CStreaming::ms_aInfoForModel[streamId];
	if(cancelQueued && createdRequest && !externalRequest &&
	   info->m_loadState == STREAMSTATE_INQUEUE &&
	   (info->m_flags & STREAMFLAGS_SCRIPTOWNED) == 0){
		CStreaming::RemoveModel(streamId);
		info->m_flags &= ~(STREAMFLAGS_DONT_REMOVE | STREAMFLAGS_PRIORITY);
	}else{
		if(addedDontRemove && !externalDontRemove)
			info->m_flags &= ~STREAMFLAGS_DONT_REMOVE;
		if(addedPriority && !externalPriority &&
		   (info->m_flags & STREAMFLAGS_PRIORITY)){
			info->m_flags &= ~STREAMFLAGS_PRIORITY;
			if(info->m_loadState == STREAMSTATE_INQUEUE &&
			   CStreaming::ms_numPriorityRequests > 0)
				CStreaming::ms_numPriorityRequests--;
		}
	}

	WiiIslandClearFlags(streamId,
		WII_ISLAND_ADDED_DONT_REMOVE |
		WII_ISLAND_ADDED_PRIORITY |
		WII_ISLAND_CREATED_REQUEST |
		WII_ISLAND_EXTERNAL_REQUEST |
		WII_ISLAND_EXTERNAL_DONT_REMOVE |
		WII_ISLAND_EXTERNAL_PRIORITY);
	if(!addedDontRemove || info->m_loadState != STREAMSTATE_LOADED ||
	   info->m_next != nil || (info->m_flags & STREAMFLAGS_DONT_REMOVE))
		return;
	if(streamId < STREAM_OFFSET_TXD){
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(streamId);
		if(mi && mi->GetModelType() != MITYPE_VEHICLE &&
		   (info->m_flags & STREAMFLAGS_SCRIPTOWNED) == 0)
			info->AddToList(&CStreaming::ms_startLoadedList);
	}else if(streamId < STREAM_OFFSET_COL || streamId >= STREAM_OFFSET_ANIM){
		if((info->m_flags & STREAMFLAGS_SCRIPTOWNED) == 0)
			info->AddToList(&CStreaming::ms_startLoadedList);
	}
}

static void
WiiIslandReleaseTemporaryPins(bool keepTargetCore, bool cancelQueued)
{
	for(int32 streamId = 0; streamId < NUMSTREAMINFO; streamId++)
		WiiIslandReleaseTemporaryPin(streamId, cancelQueued);
	if(keepTargetCore){
		bool oldInternalRequeue = gWiiIslandInternalRequeue;
		gWiiIslandInternalRequeue = true;
		WiiIslandRequestNearbyBigBuildings(gWiiIslandTargetLevel,
		                                   gWiiIslandWorkPosition);
		WiiIslandRequestLandingCore(gWiiIslandTargetLevel,
		                            gWiiIslandWorkPosition, nil);
		CStreaming::RequestIslands(gWiiIslandTargetLevel);
		gWiiIslandInternalRequeue = oldInternalRequeue;
	}
}

static int32
WiiIslandRequestNearbyBigBuildings(eLevelName level, const CVector &position)
{
	int32 requested = 0;
	float radiusSq = sq(WII_ISLAND_REQUIRED_BIG_RADIUS);
	for(int32 i = CPools::GetBuildingPool()->GetSize() - 1; i >= 0; i--){
		CBuilding *building = CPools::GetBuildingPool()->GetSlot(i);
		if(building == nil || !building->bIsBIGBuilding ||
		   building->m_level != level)
			continue;

		// This limits only the transition's blocking core. Normal renderer
		// requests resume after commit and retain their original draw distances.
		float reach = WII_ISLAND_REQUIRED_BIG_RADIUS + building->GetBoundRadius();
		bool needed = building->bStreamBIGBuilding ?
		              (building->GetBoundCentre() - position).MagnitudeSqr2D() <=
		                  sq(reach) &&
		              CRenderer::ShouldModelBeStreamed(building, position) :
		              (building->GetPosition() - position).MagnitudeSqr2D() <= radiusSq;
		if(!needed)
			continue;

		int32 model = building->GetModelIndex();
		bool firstRequest = model >= 0 && model < NUMSTREAMINFO &&
		                    !WiiIslandHasCapturedFlag(model);
		CStreaming::RequestModel(model,
		                         building->bStreamBIGBuilding ? 0 :
		                         STREAMFLAGS_DONT_REMOVE);
		if(firstRequest)
			requested++;
	}
	return requested;
}

static int32
WiiIslandRequestVisibleBigBuildings(eLevelName level, const CVector &position)
{
	int32 requested = 0;
	for(int32 i = CPools::GetBuildingPool()->GetSize() - 1; i >= 0; i--){
		CBuilding *building = CPools::GetBuildingPool()->GetSlot(i);
		if(building == nil || !building->bIsBIGBuilding ||
		   building->m_level != level || !building->bIsVisible ||
		   !CRenderer::ShouldModelBeStreamed(building, position))
			continue;

		int32 model = building->GetModelIndex();
		if(model < 0 || model >= NUMSTREAMINFO)
			continue;
		bool firstRequest = !WiiIslandHasCapturedFlag(model);
		CStreaming::RequestModel(model,
		                         building->bStreamBIGBuilding ? 0 :
		                         STREAMFLAGS_DONT_REMOVE);
		if(firstRequest)
			requested++;
	}
	return requested;
}

#if WII_STREAM_ATOMIC_BIG_HANDOFF || WII_STREAM_SPLASH_VISUAL_GATE
static int32
WiiIslandCountVisualBigBuildings(eLevelName level, const CVector &position,
	bool visibleOnly, int32 *missingOut)
{
	int32 total = 0;
	int32 missing = 0;
	for(int32 i = CPools::GetBuildingPool()->GetSize() - 1; i >= 0; i--){
		CBuilding *building = CPools::GetBuildingPool()->GetSlot(i);
		if(building == nil || !building->bIsBIGBuilding ||
		   building->m_level != level || !building->bStreamBIGBuilding ||
		   (visibleOnly && !building->bIsVisible) ||
		   !CRenderer::ShouldModelBeStreamed(building, position))
			continue;

		total++;
		int32 model = building->GetModelIndex();
		CBaseModelInfo *modelInfo =
			model >= 0 && model < MODELINFOSIZE ?
			CModelInfo::GetModelInfo(model) : nil;
		if(modelInfo == nil ||
		   CStreaming::ms_aInfoForModel[model].m_loadState != STREAMSTATE_LOADED){
			missing++;
			continue;
		}

		int32 txd = modelInfo->GetTxdSlot();
		int32 txdStreamId = txd + STREAM_OFFSET_TXD;
		if(txd >= 0 &&
		   (txdStreamId >= STREAM_OFFSET_COL ||
		    CStreaming::ms_aInfoForModel[txdStreamId].m_loadState != STREAMSTATE_LOADED)){
			missing++;
			continue;
		}
		if(building->m_rwObject == nil)
			missing++;
	}
	if(missingOut)
		*missingOut = missing;
	return total;
}
#endif

static bool
WiiIslandRequestLandingCoreEntity(CBuilding *building, eLevelName level,
	const CVector &position, float radius, bool allowGeneric)
{
	if(building == nil || building->bIsBIGBuilding ||
	   (building->m_level != level &&
	    (!allowGeneric || building->m_level != LEVEL_GENERIC)) ||
	   building->bStreamingDontDelete ||
	   building->bDontStream || !building->bIsVisible ||
	   !IsAreaVisible(building->m_area))
		return false;

	int32 model = building->GetModelIndex();
	if(model < 0 || model >= MODELINFOSIZE)
		return false;
	CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(model);
	if(modelInfo == nil || modelInfo->GetColModel() == nil)
		return false;
	if(modelInfo->GetModelType() == MITYPE_TIME){
		CTimeModelInfo *timeInfo = (CTimeModelInfo*)modelInfo;
		if(!CClock::GetIsTimeInRange(timeInfo->GetTimeOn(),
		                             timeInfo->GetTimeOff()))
			return false;
	}

	// Streamed COL payloads release their volumes but retain these bounds.
	float reach = radius + building->GetBoundRadius();
	if((building->GetBoundCentre() - position).MagnitudeSqr2D() > sq(reach))
		return false;

	bool firstRequest = !WiiIslandHasCapturedFlag(model);
	CStreaming::RequestModel(model, 0);
	return firstRequest;
}

static int32
WiiIslandRequestLandingCore(eLevelName level, const CVector &position,
	int32 *treadableRequestedOut)
{
	int32 requested = 0;
	int32 treadableRequested = 0;
	for(int32 i = CPools::GetBuildingPool()->GetSize() - 1; i >= 0; i--)
		if(WiiIslandRequestLandingCoreEntity(
		       CPools::GetBuildingPool()->GetSlot(i), level, position,
		       WII_ISLAND_LANDING_CORE_RADIUS, false))
			requested++;
	for(int32 i = CPools::GetTreadablePool()->GetSize() - 1; i >= 0; i--)
		if(WiiIslandRequestLandingCoreEntity(
		       CPools::GetTreadablePool()->GetSlot(i), level, position,
		       WII_ISLAND_LANDING_CORE_RADIUS,
		       false)){
			requested++;
			treadableRequested++;
		}
	if(treadableRequestedOut)
		*treadableRequestedOut = treadableRequested;
	return requested;
}

static void
WiiIslandClearCapturedWorkingSet(bool cancelQueued)
{
	for(int32 streamId = 0; streamId < NUMSTREAMINFO; streamId++){
		if(!WiiIslandHasCapturedFlag(streamId))
			continue;
		WiiIslandReleaseTemporaryPin(streamId, cancelQueued);
	}
	WiiIslandClearAllFlags();
	memset(gWiiIslandProtectedCols, 0, sizeof(gWiiIslandProtectedCols));
	memset(gWiiIslandProtectedRadarTxds, 0,
	       sizeof(gWiiIslandProtectedRadarTxds));
	gWiiIslandLastRequestAtMs = 0;
	gWiiIslandPrefetchReadyLogged = false;
}

static int32
WiiIslandCountRequired(void)
{
	int32 required = 0;
	for(int32 streamId = 0; streamId < NUMSTREAMINFO; streamId++)
		if(WiiIslandHasFlag(streamId, WII_ISLAND_REQUIRED))
			required++;
	return required;
}

static void
WiiIslandRequestWorkingSet(void)
{
	uint32 now = WiiIslandWallClockMs();
	if(gWiiIslandLastRequestAtMs != 0 &&
	   now - gWiiIslandLastRequestAtMs < WII_ISLAND_PREFETCH_INTERVAL_MS)
		return;
	gWiiIslandLastRequestAtMs = now;
	gWiiIslandLastRequestPosition = gWiiIslandWorkPosition;

	gWiiIslandCaptureRequests = true;
	gWiiIslandCaptureTemporaryPin = true;
	// Prediction runs in the background. Once the player has crossed the
	// boundary, the blocking read must finish the whole landing core before
	// the transition can commit.
	gWiiIslandCapturePriority = gWiiIslandPhase == WII_ISLAND_READ;
	int32 nearbyBig = WiiIslandRequestNearbyBigBuildings(
		gWiiIslandTargetLevel, gWiiIslandWorkPosition);
	int32 visualBig = 0;
#if WII_STREAM_P7_BLOCKING_HANDOFF
	// The P7 handoff must publish the target skyline with its visible BIG
	// buildings already resident. Capture them during READ so commit readiness
	// cannot succeed while the post-commit visual set is still queued.
	if(gWiiIslandPhase == WII_ISLAND_READ)
		WiiIslandRequestVisibleBigBuildings(gWiiIslandTargetLevel,
			                                   gWiiIslandWorkPosition);
#endif
#if WII_STREAM_SPLASH_VISUAL_GATE
	if(gWiiIslandPhase == WII_ISLAND_READ)
		visualBig = WiiIslandRequestVisibleBigBuildings(
			gWiiIslandTargetLevel, gWiiIslandWorkPosition);
#endif
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	if(gWiiIslandPhase == WII_ISLAND_READ)
		CStreaming::RequestBigBuildings(gWiiIslandTargetLevel,
		                                gWiiIslandAtomicHandoffPosition);
#endif
	int32 landingTreadable = 0;
	int32 landingCore = WiiIslandRequestLandingCore(
		gWiiIslandTargetLevel, gWiiIslandWorkPosition, &landingTreadable);
	CStreaming::RequestIslands(gWiiIslandTargetLevel);
	int32 corridorTreadable = 0;
	CStreaming::AddModelsToRequestList(gWiiIslandWorkPosition, 0);
	CColStore::RequestCollision(gWiiIslandWorkPosition);
	gWiiIslandCapturePriority = false;
	CColStore::RequestCollision(gWiiIslandSourceReturnPosition);
	CRadar::RequestRadarSections(gWiiIslandWorkPosition);
	gWiiIslandCaptureTemporaryPin = false;
	gWiiIslandCaptureRequests = false;
	int32 required = WiiIslandCountRequired();
	if(gWiiIslandLastRequestAtMs == now)
		SYS_Report("[WII-ISLAND] working-set target=%d nearbyBig=%d visualBig=%d landing=%d treadable=%d corridor=%d/%d required=%d queued=%d priority=%d\n",
		           (int)gWiiIslandTargetLevel, nearbyBig, visualBig, landingCore,
		           landingTreadable, corridorTreadable,
		           0,
		           required,
		           CStreaming::ms_numModelsRequested,
		           CStreaming::ms_numPriorityRequests);
}

static void
WiiIslandCaptureRetireProtection(void)
{
	WiiIslandReleaseTemporaryPins(false, false);
	WiiIslandClearAllFlags();
	memset(gWiiIslandProtectedCols, 0, sizeof(gWiiIslandProtectedCols));
	memset(gWiiIslandProtectedRadarTxds, 0,
	       sizeof(gWiiIslandProtectedRadarTxds));

	gWiiIslandCaptureRequests = true;
	gWiiIslandCaptureTemporaryPin = true;
	gWiiIslandCapturePriority = false;
	WiiIslandRequestNearbyBigBuildings(gWiiIslandTargetLevel,
	                                   gWiiIslandWorkPosition);
	WiiIslandRequestLandingCore(gWiiIslandTargetLevel,
	                            gWiiIslandWorkPosition, nil);
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	CStreaming::RequestBigBuildings(gWiiIslandTargetLevel,
	                                gWiiIslandAtomicHandoffPosition);
	int32 atomicMissing = 0;
	int32 visualBig = WiiIslandCountVisualBigBuildings(
		gWiiIslandTargetLevel, gWiiIslandAtomicHandoffPosition, false,
		&atomicMissing);
#else
	int32 visualBig = WiiIslandRequestVisibleBigBuildings(
		gWiiIslandTargetLevel, gWiiIslandWorkPosition);
#endif
	CStreaming::RequestIslands(gWiiIslandTargetLevel);
	CStreaming::AddModelsToRequestList(gWiiIslandWorkPosition, 0);
	CColStore::RequestCollision(gWiiIslandWorkPosition);
	CRadar::RequestRadarSections(gWiiIslandWorkPosition);
	gWiiIslandCaptureTemporaryPin = false;
	gWiiIslandCaptureRequests = false;

	gWiiIslandRetireProtectionActive = true;
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	SYS_Report("[WII-ISLAND] retire protection target=%d visualBig=%d atomicMissing=%d required=%d\n",
	           (int)gWiiIslandTargetLevel, visualBig, atomicMissing,
	           WiiIslandCountRequired());
#else
	SYS_Report("[WII-ISLAND] retire protection target=%d visualBig=%d required=%d\n",
	           (int)gWiiIslandTargetLevel, visualBig,
	           WiiIslandCountRequired());
#endif
}

static void
WiiIslandRefreshVisualHandoff(const CVector &position)
{
	if(!gWiiIslandRetireProtectionActive)
		return;
	uint32 now = WiiIslandWallClockMs();
	if(gWiiIslandLastRequestAtMs != 0 &&
	   now - gWiiIslandLastRequestAtMs < WII_ISLAND_PREFETCH_INTERVAL_MS)
		return;
	gWiiIslandLastRequestAtMs = now;

	gWiiIslandCaptureRequests = true;
	gWiiIslandCaptureTemporaryPin = true;
	gWiiIslandCapturePriority = false;
	int32 visualBig = WiiIslandRequestVisibleBigBuildings(
		gWiiIslandTargetLevel, position);
	gWiiIslandCaptureTemporaryPin = false;
	gWiiIslandCaptureRequests = false;

	if(visualBig > 0)
		SYS_Report("[WII-ISLAND] visual refresh target=%d visualBig=%d required=%d\n",
		           (int)gWiiIslandTargetLevel, visualBig,
		           WiiIslandCountRequired());
}

static void
WiiIslandReleaseRetireProtection(const char *reason)
{
	if(!gWiiIslandRetireProtectionActive)
		return;
	int32 required = WiiIslandCountRequired();
	WiiIslandReleaseTemporaryPins(false, false);
	WiiIslandClearAllFlags();
	memset(gWiiIslandProtectedCols, 0, sizeof(gWiiIslandProtectedCols));
	memset(gWiiIslandProtectedRadarTxds, 0,
	       sizeof(gWiiIslandProtectedRadarTxds));
	gWiiIslandRetireProtectionActive = false;
	SYS_Report("[WII-ISLAND] retire protection released reason=%s required=%d\n",
	           reason, required);
}

static bool
WiiIslandWorkingSetReady(int32 *missingRequiredOut, int32 *missingOptionalOut)
{
	int32 missingRequired = 0;
	int32 missingOptional = 0;
	for(int32 streamId = 0; streamId < NUMSTREAMINFO; streamId++){
		if(!WiiIslandHasFlag(streamId, WII_ISLAND_REQUIRED) ||
		   CStreaming::ms_aInfoForModel[streamId].m_loadState == STREAMSTATE_LOADED)
			continue;
		if(streamId >= STREAM_OFFSET_COL && streamId < STREAM_OFFSET_ANIM)
			continue;
		if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL &&
		   gWiiIslandProtectedRadarTxds[streamId - STREAM_OFFSET_TXD])
			missingOptional++;
		else
			missingRequired++;
	}
	if(!CColStore::HasCollisionLoaded(gWiiIslandWorkPosition))
		missingRequired++;
#if WII_STREAM_SPLASH_VISUAL_GATE
	if(gWiiIslandPhase == WII_ISLAND_READ){
		CStreaming::InstanceBigBuildings(gWiiIslandTargetLevel,
		                                 gWiiIslandWorkPosition);
		int32 visualMissing = 0;
		WiiIslandCountVisualBigBuildings(gWiiIslandTargetLevel,
		                                 gWiiIslandWorkPosition, true,
		                                 &visualMissing);
		missingRequired += visualMissing;
	}
#endif
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	if(gWiiIslandPhase == WII_ISLAND_READ){
		CStreaming::InstanceBigBuildings(gWiiIslandTargetLevel,
		                                 gWiiIslandAtomicHandoffPosition);
		int32 atomicMissing = 0;
		WiiIslandCountVisualBigBuildings(gWiiIslandTargetLevel,
		                                 gWiiIslandAtomicHandoffPosition, false,
		                                 &atomicMissing);
		missingRequired += atomicMissing;
	}
#endif
	if(missingRequiredOut)
		*missingRequiredOut = missingRequired;
	if(missingOptionalOut)
		*missingOptionalOut = missingOptional;
	return missingRequired == 0 && missingOptional == 0;
}

static void
WiiIslandBeginTransition(eLevelName target, const CVector &position,
	const CVector &sourceReturnPosition, bool crossedBoundary)
{
	WiiIslandTransitionReset();
	gWiiIslandSourceLevel = CGame::currLevel;
	gWiiIslandTargetLevel = target;
	WiiIslandBuildTargetBuildingModelSet();
	gWiiIslandWorkPosition = position;
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	// Freeze the original landing sample so the target skyline that should be
	// visible right after the splash stays protected through commit/retire.
	gWiiIslandAtomicHandoffPosition = position;
#endif
	gWiiIslandSourceReturnPosition = sourceReturnPosition;
	gWiiIslandStartedAtMs = WiiIslandWallClockMs();
	gWiiIslandLastSeenAtMs = gWiiIslandStartedAtMs;
	gWiiIslandNeedsReadCoreRebuild = !crossedBoundary;
	gWiiIslandPhase = WII_ISLAND_PREFETCH;
	WiiIslandRequestWorkingSet();
	SYS_Report("[WII-ISLAND] begin source=%d target=%d phase=%s crossed=%d pos=(%.1f,%.1f,%.1f) reserve=%uKB\n",
	           (int)gWiiIslandSourceLevel, (int)gWiiIslandTargetLevel,
	           WiiIslandTransitionPhaseName(), crossedBoundary ? 1 : 0,
	           position.x, position.y, position.z,
	           (unsigned)(WII_ISLAND_TRANSITION_RESERVE / 1024u));
}

static void
WiiIslandEnterReadPhase(void)
{
	if(gWiiIslandPhase == WII_ISLAND_READ)
		return;
	gWiiIslandPhase = WII_ISLAND_READ;
	gWiiIslandReadStartedAtMs = WiiIslandWallClockMs();
	if(!gWiiIslandSplashPrepared)
		gWiiIslandSplashPrepareAttempted = false;
	if(!gWiiIslandSplashVisible){
		WiiBeginIslandTransitionSplash(gWiiIslandTargetLevel);
		gWiiIslandSplashVisible = WiiIsIslandTransitionSplashActive();
	}
	if(!CTimer::GetIsCodePaused()){
		CTimer::SetCodePause(true);
		gWiiIslandPauseOwned = true;
	}
	DMAudio.SetEffectsFadeVol(0);
	CPad::StopPadsShaking();
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	// The splash hides source retirement. Releasing only the old island's big
	// buildings here avoids carrying both detailed skylines through the target
	// load while preserving the target island proxy until the atomic commit.
	CStreaming::RemoveBigBuildings(gWiiIslandSourceLevel);
#endif
	if(gWiiIslandNeedsReadCoreRebuild){
		CVector previousPosition = gWiiIslandLastRequestPosition;
		WiiIslandClearCapturedWorkingSet(true);
		gWiiIslandNeedsReadCoreRebuild = false;
		WiiIslandRequestWorkingSet();
		SYS_Report("[WII-ISLAND] read-core rebuilt target=%d from=(%.1f,%.1f) to=(%.1f,%.1f) required=%d\n",
		           (int)gWiiIslandTargetLevel, previousPosition.x,
		           previousPosition.y, gWiiIslandWorkPosition.x,
		           gWiiIslandWorkPosition.y, WiiIslandCountRequired());
	}
	CStreaming::RemoveUnusedModelsInLoadedList();
	SYS_Report("[WII-ISLAND] phase=read target=%d pause=%d splash=%d\n",
	           (int)gWiiIslandTargetLevel, gWiiIslandPauseOwned ? 1 : 0,
	           gWiiIslandSplashVisible ? 1 : 0);
}

static bool
WiiIslandCommitTransition(void)
{
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	// The captured model/TXD set is complete before this point. Build its RW
	// objects while the splash is still active and do not expose the target
	// frame until every streamed big building at the fixed handoff position is
	// present.
	CStreaming::InstanceBigBuildings(gWiiIslandTargetLevel,
	                                 gWiiIslandAtomicHandoffPosition);
	int32 atomicMissing = 0;
	int32 atomicTotal = WiiIslandCountVisualBigBuildings(
		gWiiIslandTargetLevel, gWiiIslandAtomicHandoffPosition, false,
		&atomicMissing);
	if(atomicMissing != 0){
		SYS_Report("[WII-ISLAND] atomic handoff waiting target=%d total=%d missing=%d pending=%d priority=%d\n",
		           (int)gWiiIslandTargetLevel, atomicTotal, atomicMissing,
		           CStreaming::ms_numModelsRequested,
		           CStreaming::ms_numPriorityRequests);
		return false;
	}
	gWiiIslandAtomicBigReady = true;
#endif
	gWiiIslandPhase = WII_ISLAND_COMMIT;
	gWiiIslandCommittedAtMs = WiiIslandWallClockMs();
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	gWiiIslandVisualHandoffPending = false;
#else
	gWiiIslandVisualHandoffPending = true;
#endif
	gWiiIslandRetireComplete = false;
	CReplay::EmptyReplayBuffer();
	CGame::currLevel = gWiiIslandTargetLevel;
	CCollision::ms_collisionInMemory = gWiiIslandTargetLevel;
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	CStreaming::InstanceBigBuildings(gWiiIslandTargetLevel,
	                                 gWiiIslandAtomicHandoffPosition);
#else
	CStreaming::InstanceBigBuildings(gWiiIslandTargetLevel,
	                                 gWiiIslandWorkPosition);
#endif
	CStreaming::InstanceLoadedModels(gWiiIslandWorkPosition);
	WiiIslandCaptureRetireProtection();
#if WII_STREAM_ATOMIC_BIG_HANDOFF
	// Remove the target-island proxy only after its detailed replacement is
	// loaded and instanced, and while the splash still covers the handoff.
	CStreaming::RemoveIslandsNotUsed(gWiiIslandTargetLevel);
	SYS_Report("[WII-ISLAND] atomic handoff target=%d ready=1 detailed=%d proxy-removed=1\n",
	           (int)gWiiIslandTargetLevel, atomicTotal);
#endif
	if(gWiiIslandPauseOwned && CTimer::GetIsCodePaused())
		CTimer::SetCodePause(false);
	gWiiIslandPauseOwned = false;
	DMAudio.SetEffectsFadeVol(127);
	if(gWiiIslandSplashVisible)
		WiiEndIslandTransitionSplash();
	gWiiIslandSplashVisible = false;
	gWiiIslandPhase = WII_ISLAND_RETIRE;
#if WII_STREAM_P7_BLOCKING_HANDOFF
	// Skip only the cross-island jump. Subsequent frames keep the stock
	// request-before-retire spatial lifetime active around the target island.
	const CVector &spatialRetireOrigin = TheCamera.GetPosition();
	CStreaming::ms_oldSectorX =
		CWorld::GetSectorIndexX(spatialRetireOrigin.x);
	CStreaming::ms_oldSectorY =
		CWorld::GetSectorIndexY(spatialRetireOrigin.y);
#endif
	gWiiIslandRetirePool = 0;
	gWiiIslandRetireIndex = CPools::GetBuildingPool()->GetSize() - 1;
	SYS_Report("[WII-ISLAND] commit source=%d target=%d dt=%ums pending=%d priority=%d\n",
	           (int)gWiiIslandSourceLevel, (int)gWiiIslandTargetLevel,
	           (unsigned)(WiiIslandWallClockMs() - gWiiIslandStartedAtMs),
	           CStreaming::ms_numModelsRequested,
	           CStreaming::ms_numPriorityRequests);
	return true;
}

static void
WiiIslandReturnPlayerToSource(const CVector &position)
{
	CEntity *player = FindPlayerEntity();
	if(player == nil)
		return;

	player->Teleport(position);
	const CVector &actual = player->GetPosition();
	if(Abs(actual.x - position.x) > 0.01f ||
	   Abs(actual.y - position.y) > 0.01f ||
	   Abs(actual.z - position.z) > 0.01f){
		// Heli, plane and train inherit CEntity's no-op Teleport.
		CWorld::Remove(player);
		player->SetPosition(position);
		CWorld::Add(player);
	}
	player->GetMatrix().UpdateRW();
	player->UpdateRwFrame();

	CPhysical *physical = (CPhysical*)player;
	physical->SetMoveSpeed(0.0f, 0.0f, 0.0f);
	physical->SetTurnSpeed(0.0f, 0.0f, 0.0f);
}

static void
WiiIslandAbortTransition(int32 missingRequired, int32 missingOptional)
{
	eLevelName source = gWiiIslandSourceLevel;
	eLevelName target = gWiiIslandTargetLevel;
	CVector returnPosition = gWiiIslandSourceReturnPosition;
	uint32 readElapsed = WiiIslandWallClockMs() - gWiiIslandReadStartedAtMs;
	WiiIslandReleaseTemporaryPins(false, true);
	WiiIslandReturnPlayerToSource(returnPosition);
	CGame::currLevel = source;
	CCollision::ms_collisionInMemory = source;
	CTheZones::m_CurrLevel = source;
	TheCamera.RestoreWithJumpCut();
	DMAudio.SetEffectsFadeVol(127);
	SYS_Report("[WII-ISLAND] abort source=%d target=%d required=%d optional=%d dt=%ums return=(%.1f,%.1f,%.1f)\n",
	           (int)source, (int)target, missingRequired, missingOptional,
	           (unsigned)readElapsed, returnPosition.x, returnPosition.y,
	           returnPosition.z);
	WiiIslandTransitionReset();
}

static bool
WiiIslandTargetUsesBuildingModel(int32 model)
{
	return model >= 0 && model < MODELINFOSIZE &&
	       (gWiiIslandTargetBuildingModels[model >> 3] & (1u << (model & 7))) != 0;
}

static void
WiiIslandAddTargetEntityModel(CEntity *entity)
{
	if(entity == nil ||
	   (entity->m_level != gWiiIslandTargetLevel &&
	    entity->m_level != LEVEL_GENERIC))
		return;
	int32 model = entity->GetModelIndex();
	if(model >= 0 && model < MODELINFOSIZE)
		gWiiIslandTargetBuildingModels[model >> 3] |= 1u << (model & 7);
}

static void
WiiIslandBuildTargetBuildingModelSet(void)
{
	memset(gWiiIslandTargetBuildingModels, 0,
	       sizeof(gWiiIslandTargetBuildingModels));
	for(int32 i = CPools::GetBuildingPool()->GetSize() - 1; i >= 0; i--)
		WiiIslandAddTargetEntityModel(CPools::GetBuildingPool()->GetSlot(i));
	for(int32 i = CPools::GetTreadablePool()->GetSize() - 1; i >= 0; i--)
		WiiIslandAddTargetEntityModel(CPools::GetTreadablePool()->GetSlot(i));
	for(int32 i = CPools::GetObjectPool()->GetSize() - 1; i >= 0; i--)
		WiiIslandAddTargetEntityModel(CPools::GetObjectPool()->GetSlot(i));
	for(int32 i = CPools::GetDummyPool()->GetSize() - 1; i >= 0; i--)
		WiiIslandAddTargetEntityModel(CPools::GetDummyPool()->GetSlot(i));
}

static bool
WiiIslandRetireEntity(CEntity *entity, int32 pool)
{
	if(entity == nil || entity->m_level != gWiiIslandSourceLevel ||
	   entity->bImBeingRendered || entity->bStreamingDontDelete ||
	   (FindPlayerPed() && FindPlayerPed()->m_pCurSurface == entity))
		return false;
	if(pool == 2 && ((CObject*)entity)->ObjectCreatedBy != GAME_OBJECT)
		return false;
	int32 model = entity->GetModelIndex();
	bool oldBigBuilding = entity->bIsBIGBuilding;
	if(entity->m_rwObject == nil)
		return false;
	entity->DeleteRwObject();
	if(oldBigBuilding && !WiiIslandTargetUsesBuildingModel(model))
		CStreaming::SetModelIsDeletable(model);
	return true;
}

static bool
WiiIslandRetireOldLevelStep(void)
{
	int32 scanned = 0;
	int32 removed = 0;
	while(scanned < WII_ISLAND_RETIRE_SCAN_PER_FRAME &&
	      removed < WII_ISLAND_RETIRE_REMOVE_PER_FRAME){
		CEntity *entity = nil;
		int32 poolSize = 0;
		switch(gWiiIslandRetirePool){
		case 0:
			poolSize = CPools::GetBuildingPool()->GetSize();
			if(gWiiIslandRetireIndex >= 0)
				entity = CPools::GetBuildingPool()->GetSlot(gWiiIslandRetireIndex);
			break;
		case 1:
			poolSize = CPools::GetTreadablePool()->GetSize();
			if(gWiiIslandRetireIndex >= 0)
				entity = CPools::GetTreadablePool()->GetSlot(gWiiIslandRetireIndex);
			break;
		case 2:
			poolSize = CPools::GetObjectPool()->GetSize();
			if(gWiiIslandRetireIndex >= 0)
				entity = CPools::GetObjectPool()->GetSlot(gWiiIslandRetireIndex);
			break;
		case 3:
			poolSize = CPools::GetDummyPool()->GetSize();
			if(gWiiIslandRetireIndex >= 0)
				entity = CPools::GetDummyPool()->GetSlot(gWiiIslandRetireIndex);
			break;
		default:
			CStreaming::RemoveIslandsNotUsed(gWiiIslandTargetLevel);
			return true;
		}

		if(gWiiIslandRetireIndex < 0){
			gWiiIslandRetirePool++;
			if(gWiiIslandRetirePool <= 3){
				switch(gWiiIslandRetirePool){
				case 1: gWiiIslandRetireIndex = CPools::GetTreadablePool()->GetSize() - 1; break;
				case 2: gWiiIslandRetireIndex = CPools::GetObjectPool()->GetSize() - 1; break;
				case 3: gWiiIslandRetireIndex = CPools::GetDummyPool()->GetSize() - 1; break;
				}
			}
			continue;
		}

		(void)poolSize;
		gWiiIslandRetireIndex--;
		scanned++;
		if(WiiIslandRetireEntity(entity, gWiiIslandRetirePool))
			removed++;
	}
	return false;
}

static void
WiiIslandTransitionCaptureRequest(int32 streamId, int32 flags)
{
	if(!gWiiIslandCaptureRequests || streamId < 0 || streamId >= NUMSTREAMINFO)
		return;

	WiiStreamPromoteRequestClass(streamId,
	                             WII_STREAM_REQUEST_TRANSITION_PREFETCH);
	if(gWiiIslandCaptureTemporaryPin){
		CStreamingInfo *info = &CStreaming::ms_aInfoForModel[streamId];
		if(info->m_loadState == STREAMSTATE_NOTLOADED){
			// RequestModel replaces flags for a fresh request, so stale flags on a
			// NOTLOADED entry do not represent ownership by another requester.
			WiiIslandSetFlags(streamId,
				WII_ISLAND_CREATED_REQUEST |
				WII_ISLAND_ADDED_DONT_REMOVE |
				(gWiiIslandCapturePriority ? WII_ISLAND_ADDED_PRIORITY : 0));
		}else{
			uint8 captureFlags = 0;
			if((info->m_flags & STREAMFLAGS_DONT_REMOVE) == 0)
				captureFlags |= WII_ISLAND_ADDED_DONT_REMOVE;
			if(gWiiIslandCapturePriority &&
			   (info->m_flags & STREAMFLAGS_PRIORITY) == 0)
				captureFlags |= WII_ISLAND_ADDED_PRIORITY;
			WiiIslandSetFlags(streamId, captureFlags);
		}
	}
	WiiIslandSetFlag(streamId, WII_ISLAND_REQUIRED);
	if(streamId >= STREAM_OFFSET_COL && streamId < STREAM_OFFSET_ANIM)
		gWiiIslandProtectedCols[streamId - STREAM_OFFSET_COL] = 1;
}

static void
WiiIslandObserveExternalRequest(int32 streamId, int32 flags)
{
	if(gWiiIslandCaptureRequests || gWiiIslandInternalRequeue ||
	   streamId < 0 || streamId >= NUMSTREAMINFO ||
	   !WiiIslandHasCapturedFlag(streamId))
		return;

	WiiIslandSetFlag(streamId, WII_ISLAND_EXTERNAL_REQUEST);
	if(flags & STREAMFLAGS_DONT_REMOVE)
		WiiIslandSetFlag(streamId, WII_ISLAND_EXTERNAL_DONT_REMOVE);
	CStreamingInfo *info = &CStreaming::ms_aInfoForModel[streamId];
	if((flags & STREAMFLAGS_PRIORITY) &&
	   (info->m_loadState == STREAMSTATE_NOTLOADED ||
	    info->m_loadState == STREAMSTATE_INQUEUE))
		WiiIslandSetFlag(streamId, WII_ISLAND_EXTERNAL_PRIORITY);

	if(streamId < STREAM_OFFSET_TXD){
		CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(streamId);
		if(modelInfo == nil)
			return;
		int32 txdStreamId = modelInfo->GetTxdSlot() + STREAM_OFFSET_TXD;
		if(txdStreamId >= STREAM_OFFSET_TXD && txdStreamId < STREAM_OFFSET_COL &&
		   WiiIslandHasCapturedFlag(txdStreamId)){
			WiiIslandSetFlag(txdStreamId, WII_ISLAND_EXTERNAL_REQUEST);
			if(flags & STREAMFLAGS_DONT_REMOVE)
				WiiIslandSetFlag(txdStreamId, WII_ISLAND_EXTERNAL_DONT_REMOVE);
			CStreamingInfo *txdInfo = &CStreaming::ms_aInfoForModel[txdStreamId];
			if((flags & STREAMFLAGS_PRIORITY) &&
			   (txdInfo->m_loadState == STREAMSTATE_NOTLOADED ||
			    txdInfo->m_loadState == STREAMSTATE_INQUEUE))
				WiiIslandSetFlag(txdStreamId, WII_ISLAND_EXTERNAL_PRIORITY);
		}
		int32 anim = modelInfo->GetAnimFileIndex();
		int32 animStreamId = anim == -1 ? -1 : anim + STREAM_OFFSET_ANIM;
		if(animStreamId >= STREAM_OFFSET_ANIM && animStreamId < NUMSTREAMINFO &&
		   WiiIslandHasCapturedFlag(animStreamId))
			WiiIslandSetFlag(animStreamId, WII_ISLAND_EXTERNAL_REQUEST);
	}
}

static uint32
WiiStreamNowMs(void)
{
	return CTimer::GetTimeInMilliseconds();
}

static uint32
WiiStreamSaturatingAdd(uint32 current, uint64 value)
{
	uint64 total = (uint64)current + value;
	return total > UINT32_MAX ? UINT32_MAX : (uint32)total;
}

static void
WiiStreamRecordFrameWork(uint64 makeSpaceTicks, uint64 removalTicks,
	int removals, int archiveRemovals, int pressureRemovals)
{
#if WII_STREAM_MEMORY_DIAGNOSTICS
	uint32 frame = CTimer::GetFrameCounter();
	if(gWiiStreamFrameWorkFrame != frame){
		gWiiStreamFrameWorkFrame = frame;
		memset(&gWiiStreamFrameWork, 0, sizeof(gWiiStreamFrameWork));
	}
	gWiiStreamFrameWork.makeSpaceCalls++;
	gWiiStreamFrameWork.removals += removals > 0 ? (uint32)removals : 0;
	gWiiStreamFrameWork.archiveRemovals +=
		archiveRemovals > 0 ? (uint32)archiveRemovals : 0;
	gWiiStreamFrameWork.pressureRemovals +=
		pressureRemovals > 0 ? (uint32)pressureRemovals : 0;
	gWiiStreamFrameWork.makeSpaceUs = WiiStreamSaturatingAdd(
		gWiiStreamFrameWork.makeSpaceUs, ticks_to_microsecs(makeSpaceTicks));
	gWiiStreamFrameWork.removalUs = WiiStreamSaturatingAdd(
		gWiiStreamFrameWork.removalUs, ticks_to_microsecs(removalTicks));
#else
	(void)makeSpaceTicks;
	(void)removalTicks;
	(void)removals;
	(void)archiveRemovals;
	(void)pressureRemovals;
#endif
}

void
CStreaming::GetFrameWork(WiiStreamingFrameWork *work)
{
	if(work == nil)
		return;
#if WII_STREAM_MEMORY_DIAGNOSTICS
	if(gWiiStreamFrameWorkFrame == CTimer::GetFrameCounter())
		*work = gWiiStreamFrameWork;
	else
#endif
		memset(work, 0, sizeof(*work));
}

static void
WiiStreamSetResourceBit(uint8 *bits, int32 streamId)
{
	bits[streamId >> 3] |= 1u << (streamId & 7);
}

static void
WiiStreamClearResourceBit(uint8 *bits, int32 streamId)
{
	bits[streamId >> 3] &= ~(1u << (streamId & 7));
}

static bool
WiiStreamGetResourceBit(const uint8 *bits, int32 streamId)
{
	return (bits[streamId >> 3] & (1u << (streamId & 7))) != 0;
}

static void
WiiStreamPromoteRequestClassResource(int32 streamId, uint8 requestClass)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
	if(requestClass > gWiiStreamRequestClass[streamId])
		gWiiStreamRequestClass[streamId] = requestClass;
}

static void
WiiStreamPromoteRequestClass(int32 streamId, uint8 requestClass)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
	WiiStreamPromoteRequestClassResource(streamId, requestClass);
	if(streamId >= STREAM_OFFSET_TXD)
		return;

	CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(streamId);
	if(modelInfo == nil)
		return;
	int32 txd = modelInfo->GetTxdSlot();
	int32 txdStreamId = txd + STREAM_OFFSET_TXD;
	if(txd >= 0 && txdStreamId < STREAM_OFFSET_COL)
		WiiStreamPromoteRequestClassResource(txdStreamId, requestClass);
	int32 anim = modelInfo->GetAnimFileIndex();
	int32 animStreamId = anim == -1 ? -1 : anim + STREAM_OFFSET_ANIM;
	if(animStreamId >= STREAM_OFFSET_ANIM && animStreamId < NUMSTREAMINFO)
		WiiStreamPromoteRequestClassResource(animStreamId, requestClass);
}

#if WII_STREAM_P7_VISIBLE_TXD_GUARD
static bool
WiiStreamIsTxdStreamId(int32 streamId)
{
	return streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL;
}

static void
WiiStreamMarkRecentVisibleTxd(int32 streamId)
{
	if(!WiiStreamIsTxdStreamId(streamId))
		return;
	uint32 now = WiiStreamNowMs();
	gWiiStreamRecentVisibleTxdMs[streamId - STREAM_OFFSET_TXD] =
		now != 0 ? now : 1u;
}

static bool
WiiStreamProtectsRecentVisibleTxd(int32 streamId, uint32 now)
{
	if(!WiiStreamIsTxdStreamId(streamId))
		return false;
	uint32 lastVisible = gWiiStreamRecentVisibleTxdMs[streamId - STREAM_OFFSET_TXD];
	return lastVisible != 0 &&
	       now - lastVisible < WII_STREAM_P7_RECENT_VISIBLE_TXD_GRACE_MS;
}
#endif

static void
WiiStreamBeginWorldLoadFrame(uint32 frame)
{
	if(gWiiStreamWorldLoadFrame == frame)
		return;
	memset(gWiiStreamWorldLoadedThisFrame, 0,
	       sizeof(gWiiStreamWorldLoadedThisFrame));
	gWiiStreamWorldLoadFrame = frame;
}

static void
WiiStreamMarkWorldRequestResource(int32 streamId)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
#if WII_STREAM_P7_VISIBLE_TXD_GUARD
	WiiStreamMarkRecentVisibleTxd(streamId);
#endif
#if WII_STREAM_MEMORY_DIAGNOSTICS && WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	uint32 now = WiiStreamNowMs();
	gWiiStreamDiagLastWorldVisibleMs[streamId] = now != 0 ? now : 1u;
#endif
	uint8 state = CStreaming::ms_aInfoForModel[streamId].m_loadState;
	if(state == STREAMSTATE_LOADED){
		WiiStreamBeginWorldLoadFrame(CTimer::GetFrameCounter());
		WiiStreamSetResourceBit(gWiiStreamWorldLoadedThisFrame, streamId);
	}else if(state == STREAMSTATE_READING || state == STREAMSTATE_STARTED)
		WiiStreamSetResourceBit(gWiiStreamDispatchedWorldRequest, streamId);
	else
		WiiStreamSetResourceBit(gWiiStreamWorldRequest, streamId);
}

static void
WiiStreamMarkWorldRequestBits(int32 streamId)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
	WiiStreamMarkWorldRequestResource(streamId);
	if(streamId >= STREAM_OFFSET_TXD)
		return;

	CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(streamId);
	if(modelInfo == nil)
		return;
	int32 txd = modelInfo->GetTxdSlot();
	int32 txdStreamId = txd + STREAM_OFFSET_TXD;
	if(txd >= 0 && txdStreamId < STREAM_OFFSET_COL)
		WiiStreamMarkWorldRequestResource(txdStreamId);
}

static void
WiiStreamMarkWorldRequest(int32 streamId)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
	WiiStreamMarkWorldRequestBits(streamId);
	WiiStreamPromoteRequestClass(streamId, WII_STREAM_REQUEST_WORLD_VISIBLE);
}

static void
WiiStreamMarkWorldLoaded(int32 streamId)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO ||
	   !WiiStreamGetResourceBit(gWiiStreamDispatchedWorldRequest, streamId))
		return;

	uint32 frame = CTimer::GetFrameCounter();
	WiiStreamBeginWorldLoadFrame(frame);
	WiiStreamSetResourceBit(gWiiStreamWorldLoadedThisFrame, streamId);
	WiiStreamClearResourceBit(gWiiStreamWorldRequest, streamId);
	WiiStreamClearResourceBit(gWiiStreamDispatchedWorldRequest, streamId);
	if(streamId >= STREAM_OFFSET_TXD)
		return;

	CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(streamId);
	if(modelInfo == nil)
		return;
	int32 txd = modelInfo->GetTxdSlot();
	int32 txdStreamId = txd + STREAM_OFFSET_TXD;
	if(txd >= 0 && txdStreamId < STREAM_OFFSET_COL &&
	   CStreaming::ms_aInfoForModel[txdStreamId].m_loadState == STREAMSTATE_LOADED){
		WiiStreamSetResourceBit(gWiiStreamWorldLoadedThisFrame, txdStreamId);
	}
}

static bool
WiiStreamProtectsSameFrameWorldLoad(int32 streamId)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO ||
	   gWiiStreamSynchronousLoad ||
	   gWiiStreamWorldLoadFrame != CTimer::GetFrameCounter())
		return false;
	return WiiStreamGetResourceBit(gWiiStreamWorldLoadedThisFrame, streamId);
}

static void
WiiStreamClearSameFrameWorldLoad(int32 streamId)
{
	if(streamId >= 0 && streamId < NUMSTREAMINFO &&
	   gWiiStreamWorldLoadFrame == CTimer::GetFrameCounter())
		WiiStreamClearResourceBit(gWiiStreamWorldLoadedThisFrame, streamId);
}

static void
WiiStreamReportSameFrameVictimDeferral(int32 streamId, uint32 poolBit)
{
	gWiiStreamSameFrameVictimDeferrals++;
	if(gWiiStreamSameFrameVictimDeferrals <= 16 ||
	   (gWiiStreamSameFrameVictimDeferrals &
	    (gWiiStreamSameFrameVictimDeferrals - 1)) == 0)
		SYS_Report("[WII-STREAM] same-frame world victim deferred id=%d pool=0x%x frame=%u count=%u\n",
		           streamId, (unsigned)poolBit,
		           (unsigned)CTimer::GetFrameCounter(),
		           (unsigned)gWiiStreamSameFrameVictimDeferrals);
}

static void
WiiStreamResetState(void)
{
	WiiIslandTransitionReset();
	gWiiIslandLastStablePosition = CVector(0.0f, 0.0f, 0.0f);
	gWiiIslandLastStableLevel = LEVEL_GENERIC;
	gWiiIslandHasLastStablePosition = false;
	memset(gWiiStreamResourcePoolMask, 0, sizeof(gWiiStreamResourcePoolMask));
	memset(gWiiStreamResidentCost, 0, sizeof(gWiiStreamResidentCost));
	memset(gWiiStreamQueuedAtMs, 0, sizeof(gWiiStreamQueuedAtMs));
	memset(gWiiStreamDispatched, 0, sizeof(gWiiStreamDispatched));
	memset(gWiiStreamDispatchedFlags, 0, sizeof(gWiiStreamDispatchedFlags));
	memset(gWiiStreamDispatchedRequestClass, 0, sizeof(gWiiStreamDispatchedRequestClass));
	memset(gWiiStreamRequestClass, 0, sizeof(gWiiStreamRequestClass));
	memset(gWiiStreamWorldRequest, 0, sizeof(gWiiStreamWorldRequest));
	memset(gWiiStreamDispatchedWorldRequest, 0,
	       sizeof(gWiiStreamDispatchedWorldRequest));
	memset(gWiiStreamWorldLoadedThisFrame, 0,
	       sizeof(gWiiStreamWorldLoadedThisFrame));
#if WII_STREAM_P7_VISIBLE_TXD_GUARD
	memset(gWiiStreamRecentVisibleTxdMs, 0,
	       sizeof(gWiiStreamRecentVisibleTxdMs));
#endif
	gWiiStreamWorldLoadFrame = UINT32_MAX;
	gWiiStreamSameFrameVictimDeferrals = 0;
	gWiiStreamSynchronousLoad = false;
	gWiiStreamFrameWorkFrame = UINT32_MAX;
	memset(&gWiiStreamFrameWork, 0, sizeof(gWiiStreamFrameWork));
	gWiiStreamDependencyUnwindFrame = UINT32_MAX;
	gWiiStreamMemoryBudget = WII_STREAMING_MEMORY_BUDGET_LOW;
	gWiiStreamArchiveCeiling = WII_STREAMING_ARCHIVE_CEILING_BASE;
	gWiiStreamBudgetLastGrowthMs = WiiStreamNowMs();
	gWiiStreamArchiveLastAdjustMs = gWiiStreamBudgetLastGrowthMs;
	gWiiStreamArchivePressureSinceMs = 0;
	gWiiStreamArchivePressureLastSeenMs = 0;
	gWiiStreamArchiveRecoverySinceMs = 0;
	gWiiStreamArchiveRetreated = false;
	gWiiStreamArchiveRetireFrame = UINT32_MAX;
	gWiiStreamBudgetPressureSinceMs = 0;
	gWiiStreamForegroundServicesSinceFair = 0;
	gWiiStreamResidentSaturationCount = 0;
	gWiiStreamResidentUnderflowCount = 0;
	gWiiStreamResidentLastRawArena2Remaining = 0;
	gWiiStreamResidentHasLastRawArena2 = false;
	gWiiStreamSelectedDispatchId = -1;
	gWiiStreamSelectedDispatchClass = WII_STREAM_DISPATCH_NORMAL;
	gWiiStreamSelectedDispatchWaitMs = 0;
	WiiMemorySetResidentDeltaCallback(WiiStreamRecordResidentDelta);
	WiiStreamRebuildResidentCosts();
	CStreaming::ms_memoryAvailable = gWiiStreamMemoryBudget;
}

static void
WiiStreamMarkQueued(int32 streamId)
{
	if(streamId >= 0 && streamId < NUMSTREAMINFO &&
	   gWiiStreamQueuedAtMs[streamId] == 0){
		uint32 now = WiiStreamNowMs();
		gWiiStreamQueuedAtMs[streamId] = now != 0 ? now : 1u;
	}
}

static void
WiiStreamClearQueued(int32 streamId)
{
	if(streamId >= 0 && streamId < NUMSTREAMINFO){
		if(!WiiStreamGetResourceBit(gWiiStreamDispatched, streamId))
			gWiiStreamQueuedAtMs[streamId] = 0;
		gWiiStreamRequestClass[streamId] = WII_STREAM_REQUEST_NORMAL;
		if(gWiiStreamSelectedDispatchId == streamId){
			gWiiStreamSelectedDispatchId = -1;
			gWiiStreamSelectedDispatchClass = WII_STREAM_DISPATCH_NORMAL;
			gWiiStreamSelectedDispatchWaitMs = 0;
		}
	}
}

static int32
WiiStreamSelectDispatch(int32 streamId, uint8 dispatchClass, uint32 effectiveWait)
{
	gWiiStreamSelectedDispatchId = streamId;
	gWiiStreamSelectedDispatchClass = dispatchClass;
	gWiiStreamSelectedDispatchWaitMs = effectiveWait;
	return streamId;
}

static void
WiiStreamClearDispatchedFlags(int32 streamId)
{
	if(streamId >= 0 && streamId < NUMSTREAMINFO){
		gWiiStreamQueuedAtMs[streamId] = 0;
		WiiStreamClearResourceBit(gWiiStreamDispatched, streamId);
		gWiiStreamDispatchedFlags[streamId] = 0;
		gWiiStreamDispatchedRequestClass[streamId] = WII_STREAM_REQUEST_NORMAL;
#if WII_STREAM_MEMORY_DIAGNOSTICS && WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
		gWiiStreamDiagDispatchMs[streamId] = 0;
		gWiiStreamDiagQueueWaitMs[streamId] = 0;
		gWiiStreamDiagLastDispatchClass[streamId] = WII_STREAM_DISPATCH_NORMAL;
#endif
		WiiStreamClearResourceBit(gWiiStreamWorldRequest, streamId);
		WiiStreamClearResourceBit(gWiiStreamDispatchedWorldRequest, streamId);
	}
}

static bool
WiiIslandDiscardOptionalRadarChannelError(void)
{
	int32 channelId = CStreaming::ms_channelError;
	if(channelId < 0 || channelId >= 2)
		return false;

	CStreamingChannel *channel = &CStreaming::ms_channel[channelId];
	bool hasResource = false;
	for(int32 i = 0; i < 4; i++){
		int32 streamId = channel->streamIds[i];
		if(streamId == -1)
			continue;
		hasResource = true;
		if(streamId < STREAM_OFFSET_TXD || streamId >= STREAM_OFFSET_COL ||
		   !gWiiIslandProtectedRadarTxds[streamId - STREAM_OFFSET_TXD])
			return false;
	}
	if(!hasResource)
		return false;

	SYS_Report("[WII-ISLAND] discard optional radar error ch=%d status=0x%02X ids=%d/%d/%d/%d\n",
	           channelId, channel->status & 0xFF,
	           channel->streamIds[0], channel->streamIds[1],
	           channel->streamIds[2], channel->streamIds[3]);
	for(int32 i = 0; i < 4; i++){
		int32 streamId = channel->streamIds[i];
		if(streamId == -1)
			continue;
		CStreaming::RemoveModel(streamId);
		WiiStreamClearDispatchedFlags(streamId);
	}
	for(int32 i = 0; i < 4; i++){
		channel->streamIds[i] = -1;
		channel->offsets[i] = -1;
	}
	channel->state = CHANNELSTATE_IDLE;
	channel->field24 = 0;
	channel->position = 0;
	channel->size = 0;
	channel->numTries = 0;
	channel->status = STREAM_NONE;
	CStreaming::ms_channelError = -1;
	CHud::SetMessage(nil);
	return true;
}

static void
WiiStreamRequeueDispatched(int32 streamId, const char *reason)
{
	uint8 flags = gWiiStreamDispatchedFlags[streamId];
	uint8 requestClass = gWiiStreamDispatchedRequestClass[streamId];
	uint32 queuedAt = gWiiStreamQueuedAtMs[streamId];
	bool worldRequest = WiiStreamGetResourceBit(
		gWiiStreamDispatchedWorldRequest, streamId);
	if(flags == 0)
		flags = CStreaming::ms_aInfoForModel[streamId].m_flags;
	CStreaming::RemoveModel(streamId);
	WiiStreamPromoteRequestClassResource(streamId, requestClass);
	WiiIslandRequeueModel(streamId, flags);
	if(queuedAt != 0)
		gWiiStreamQueuedAtMs[streamId] = queuedAt;
	if(worldRequest)
		WiiStreamMarkWorldRequestBits(streamId);
	WiiStreamPromoteRequestClass(streamId, requestClass);
	#if WII_STREAM_MEMORY_DIAGNOSTICS
	gWiiPhase0RequestRetryCount++;
	#endif
	if(flags & STREAMFLAGS_PRIORITY)
		SYS_Report("[WII-STREAM] restored priority request id=%d flags=0x%02X reason=%s\n",
		           streamId, (unsigned)flags, reason ? reason : "retry");
}

static bool
WiiStreamIsAgedFairRequest(int32 streamId, const CStreamingInfo *si, uint32 now)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO || si == nil ||
	   si->m_loadState != STREAMSTATE_INQUEUE ||
	   (si->m_flags & STREAMFLAGS_PRIORITY) != 0)
		return false;
	uint32 queuedAt = gWiiStreamQueuedAtMs[streamId];
	if(now == 0 || queuedAt == 0)
		return false;
	uint32 wait = now - queuedAt;
	uint32 deadline = (si->m_flags &
	                  (STREAMFLAGS_KEEP_IN_MEMORY | STREAMFLAGS_DEPENDENCY)) != 0 ?
	                  WII_STREAM_KEEP_FAIR_WAIT_MS : WII_STREAM_HARD_FAIR_WAIT_MS;
	return wait >= deadline;
}

static bool
WiiStreamIsRadarRequest(int32 streamId)
{
	return streamId >= 0 && streamId < NUMSTREAMINFO &&
	       gWiiStreamRequestClass[streamId] == WII_STREAM_REQUEST_RADAR;
}

static bool
WiiStreamHasExpansionHeadroom(const WiiMemoryPoolSnapshot &snapshot)
{
	return snapshot.genericFree >= WII_STREAM_EXPAND_GENERIC_FREE_BYTES &&
	       snapshot.genericLargest >= WII_STREAM_EXPAND_GENERIC_LARGEST_BYTES &&
	       (size_t)snapshot.newlibFree + snapshot.rawArena2Remaining >=
	           WII_STREAM_EXPAND_NEWLIB_FREE_BYTES &&
	       snapshot.gxFree >= WII_STREAM_EXPAND_GX_FREE_BYTES &&
	       snapshot.gxLargest >= WII_STREAM_EXPAND_GX_LARGEST_BYTES;
}

static uint32
WiiStreamHardPressureBits(uint32 pressure)
{
	return pressure & WII_STREAM_PRESSURE_HARD_MASK;
}

static uint32
WiiStreamAdmissionPressureBits(uint32 pressure)
{
#if WII_STREAM_GX_HEADROOM_GUARD
	return pressure & WII_STREAM_PRESSURE_GX_ADMISSION;
#else
	(void)pressure;
	return 0;
#endif
}

static bool
WiiStreamHasGxAdmissionHeadroom(const WiiMemoryPoolSnapshot &snapshot)
{
#if WII_STREAM_GX_HEADROOM_GUARD
	return snapshot.gxFree >= WII_STREAM_GX_HEADROOM_GUARD_FREE_BYTES &&
	       snapshot.gxLargest >= WII_STREAM_GX_HEADROOM_GUARD_LARGEST_BYTES;
#else
	(void)snapshot;
	return true;
#endif
}

static uint32
WiiStreamGetStreamingPressureForSnapshotWithAdmission(
	const WiiMemoryPoolSnapshot &snapshot)
{
	uint32 pressure = WiiMemoryGetStreamingPressureForSnapshot(&snapshot);
#if WII_STREAM_GX_HEADROOM_GUARD
	if((pressure & WII_STREAM_PRESSURE_GX) == 0 &&
	   !WiiStreamHasGxAdmissionHeadroom(snapshot))
		pressure |= WII_STREAM_PRESSURE_GX_ADMISSION;
#endif
	return pressure;
}

static uint32
WiiStreamPressureServiceBit(uint32 pressureBit)
{
#if WII_STREAM_GX_HEADROOM_GUARD
	if(pressureBit == WII_STREAM_PRESSURE_GX_ADMISSION)
		return WII_STREAM_PRESSURE_GX;
#endif
	return pressureBit;
}

static uint32
WiiStreamGxAdmissionPressureDeficit(const WiiMemoryPoolSnapshot &snapshot)
{
#if WII_STREAM_GX_HEADROOM_GUARD
	size_t freeDeficit = snapshot.gxFree < WII_STREAM_GX_HEADROOM_GUARD_FREE_BYTES ?
	                     WII_STREAM_GX_HEADROOM_GUARD_FREE_BYTES -
	                         snapshot.gxFree :
	                     0;
	size_t largestDeficit =
		snapshot.gxLargest < WII_STREAM_GX_HEADROOM_GUARD_LARGEST_BYTES ?
			WII_STREAM_GX_HEADROOM_GUARD_LARGEST_BYTES -
			    snapshot.gxLargest :
			0;
	return (uint32)(freeDeficit > largestDeficit ? freeDeficit : largestDeficit);
#else
	(void)snapshot;
	return 0;
#endif
}

#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING
static bool
WiiStreamHasArchiveGrowthHeadroom(
	const WiiMemoryPoolSnapshot &snapshot, uint32 pressure)
{
	size_t newlibRawFree =
		(size_t)snapshot.newlibFree + snapshot.rawArena2Remaining;
	return WiiStreamHardPressureBits(pressure) == 0 &&
	       WiiStreamAdmissionPressureBits(pressure) == 0 &&
	       snapshot.genericFree >= WII_STREAM_ARCHIVE_GROW_GENERIC_FREE_BYTES &&
	       snapshot.genericLargest >= WII_STREAM_ARCHIVE_GROW_GENERIC_LARGEST_BYTES &&
	       newlibRawFree >= WII_STREAM_ARCHIVE_GROW_NEWLIB_RAW_BYTES &&
	       snapshot.gxFree >= WII_STREAM_ARCHIVE_GROW_GX_FREE_BYTES &&
	       snapshot.gxLargest >= WII_STREAM_ARCHIVE_GROW_GX_LARGEST_BYTES;
}
#endif

#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING
static size_t
WiiStreamArchiveElasticAllowance(
	const WiiMemoryPoolSnapshot &snapshot, uint32 pressure)
{
	size_t newlibRawFree =
		(size_t)snapshot.newlibFree + snapshot.rawArena2Remaining;
	if(WiiStreamHardPressureBits(pressure) != 0 ||
	   WiiStreamAdmissionPressureBits(pressure) != 0 ||
	   snapshot.genericFree <= WII_STREAM_ARCHIVE_KEEP_GENERIC_FREE_BYTES ||
	   snapshot.genericLargest <= WII_STREAM_ARCHIVE_KEEP_GENERIC_LARGEST_BYTES ||
	   newlibRawFree <= WII_STREAM_ARCHIVE_KEEP_NEWLIB_RAW_BYTES ||
	   snapshot.gxFree <= WII_STREAM_ARCHIVE_KEEP_GX_FREE_BYTES ||
	   snapshot.gxLargest <= WII_STREAM_ARCHIVE_KEEP_GX_LARGEST_BYTES)
		return 0;

	size_t allowance = WII_STREAM_ARCHIVE_MAX_ELASTIC_DEBT_BYTES;
	allowance = Min(allowance, snapshot.genericFree -
	                         WII_STREAM_ARCHIVE_KEEP_GENERIC_FREE_BYTES);
	allowance = Min(allowance, snapshot.genericLargest -
	                         WII_STREAM_ARCHIVE_KEEP_GENERIC_LARGEST_BYTES);
	allowance = Min(allowance, newlibRawFree -
	                         WII_STREAM_ARCHIVE_KEEP_NEWLIB_RAW_BYTES);
	allowance = Min(allowance, snapshot.gxFree -
	                         WII_STREAM_ARCHIVE_KEEP_GX_FREE_BYTES);
	allowance = Min(allowance, snapshot.gxLargest -
	                         WII_STREAM_ARCHIVE_KEEP_GX_LARGEST_BYTES);
	return allowance;
}
#endif

static size_t
WiiStreamUpdateArchiveCeiling(const WiiMemoryPoolSnapshot &snapshot, uint32 pressure)
{
#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING
	size_t newlibRawFree = (size_t)snapshot.newlibFree + snapshot.rawArena2Remaining;
	uint32 hardPressure = WiiStreamHardPressureBits(pressure);
	uint32 admissionPressure = WiiStreamAdmissionPressureBits(pressure);
	bool growthHeadroom =
		WiiStreamHasArchiveGrowthHeadroom(snapshot, pressure);
	size_t elasticAllowance =
		WiiStreamArchiveElasticAllowance(snapshot, pressure);
	bool retentionHeadroom = elasticAllowance != 0;
	uint32 now = WiiStreamNowMs();
	if(now == 0)
		return gWiiStreamArchiveCeiling;

	if(hardPressure != 0){
		gWiiStreamArchiveRecoverySinceMs = 0;
		if(gWiiStreamArchivePressureSinceMs == 0 ||
		   gWiiStreamArchivePressureLastSeenMs == 0 ||
		   now - gWiiStreamArchivePressureLastSeenMs >
		       WII_STREAMING_ARCHIVE_PRESSURE_SAMPLE_GAP_MS)
			gWiiStreamArchivePressureSinceMs = now;
		gWiiStreamArchivePressureLastSeenMs = now;
	}else{
		gWiiStreamArchivePressureSinceMs = 0;
		gWiiStreamArchivePressureLastSeenMs = 0;
		bool recoveryEligible = gWiiStreamArchiveRetreated &&
		                        retentionHeadroom &&
		                        gWiiStreamArchiveCeiling <
		                            WII_STREAMING_ARCHIVE_CEILING_HIGH;
		if(!recoveryEligible)
			gWiiStreamArchiveRecoverySinceMs = 0;
		else if(gWiiStreamArchiveRecoverySinceMs == 0)
			gWiiStreamArchiveRecoverySinceMs = now;
	}
	uint32 hardPressureAgeMs = gWiiStreamArchivePressureSinceMs == 0 ? 0 :
	                           now - gWiiStreamArchivePressureSinceMs;
	uint32 recoveryAgeMs = gWiiStreamArchiveRecoverySinceMs == 0 ? 0 :
	                       now - gWiiStreamArchiveRecoverySinceMs;

	size_t oldCeiling = gWiiStreamArchiveCeiling;
	const char *reason = "none";
	if(hardPressure != 0 &&
	   hardPressureAgeMs >= WII_STREAMING_ARCHIVE_HARD_PRESSURE_MS &&
	   gWiiStreamArchiveCeiling > WII_STREAMING_ARCHIVE_CEILING_BASE){
		gWiiStreamArchiveCeiling = gWiiStreamArchiveCeiling >
			WII_STREAMING_ARCHIVE_CEILING_BASE + WII_STREAMING_ARCHIVE_CEILING_STEP ?
			gWiiStreamArchiveCeiling - WII_STREAMING_ARCHIVE_CEILING_STEP :
			WII_STREAMING_ARCHIVE_CEILING_BASE;
		gWiiStreamArchivePressureSinceMs = now;
		gWiiStreamArchivePressureLastSeenMs = now;
		gWiiStreamArchiveRetreated = true;
		reason = "hard";
	}else if(!gWiiStreamArchiveRetreated && growthHeadroom &&
	         now - gWiiStreamArchiveLastAdjustMs >= WII_STREAMING_ARCHIVE_GROW_MS &&
	         gWiiStreamArchiveCeiling < WII_STREAMING_ARCHIVE_CEILING_HIGH){
		gWiiStreamArchiveCeiling += WII_STREAMING_ARCHIVE_CEILING_STEP;
		if(gWiiStreamArchiveCeiling > WII_STREAMING_ARCHIVE_CEILING_HIGH)
			gWiiStreamArchiveCeiling = WII_STREAMING_ARCHIVE_CEILING_HIGH;
		reason = "grow";
	}else if(gWiiStreamArchiveRetreated &&
	         recoveryAgeMs >= WII_STREAMING_ARCHIVE_RECOVERY_MS &&
	         gWiiStreamArchiveCeiling < WII_STREAMING_ARCHIVE_CEILING_HIGH){
		gWiiStreamArchiveCeiling += WII_STREAMING_ARCHIVE_CEILING_STEP;
		gWiiStreamArchiveRecoverySinceMs = now;
		if(gWiiStreamArchiveCeiling >= WII_STREAMING_ARCHIVE_CEILING_HIGH){
			gWiiStreamArchiveCeiling = WII_STREAMING_ARCHIVE_CEILING_HIGH;
			gWiiStreamArchiveRetreated = false;
			gWiiStreamArchiveRecoverySinceMs = 0;
		}
		reason = "recover";
	}
	if(oldCeiling != gWiiStreamArchiveCeiling){
		gWiiStreamArchiveLastAdjustMs = now;
		SYS_Report("[WII-STREAM] archive ceiling=%uKB->%uKB pressure=0x%X hard=0x%X admission=0x%X grow=%d allow=%uKB "
		           "reason=%s hard_age=%ums recovery_age=%ums retreated=%d "
		           "generic=%u/%uKB rawnl=%uKB gx=%u/%uKB\n",
		           (unsigned)(oldCeiling / 1024u),
		           (unsigned)(gWiiStreamArchiveCeiling / 1024u),
		           (unsigned)pressure, (unsigned)hardPressure,
		           (unsigned)admissionPressure, growthHeadroom ? 1 : 0,
		           (unsigned)(elasticAllowance / 1024u),
		           reason, (unsigned)hardPressureAgeMs,
		           (unsigned)recoveryAgeMs,
		           gWiiStreamArchiveRetreated ? 1 : 0,
		           (unsigned)(snapshot.genericFree / 1024u),
		           (unsigned)(snapshot.genericLargest / 1024u),
		           (unsigned)(newlibRawFree / 1024u),
		           (unsigned)(snapshot.gxFree / 1024u),
		           (unsigned)(snapshot.gxLargest / 1024u));
	}
#else
	(void)snapshot;
	(void)pressure;
#endif
	return gWiiStreamArchiveCeiling;
}

static size_t
WiiStreamUpdateMemoryBudget(const WiiMemoryPoolSnapshot &snapshot, uint32 pressure)
{
	size_t oldBudget = gWiiStreamMemoryBudget;
	uint32 now = WiiStreamNowMs();
	uint32 hardPressure = WiiStreamHardPressureBits(pressure);
	uint32 admissionPressure = WiiStreamAdmissionPressureBits(pressure);
	if(hardPressure != 0){
		if(gWiiStreamBudgetPressureSinceMs == 0)
			gWiiStreamBudgetPressureSinceMs = now != 0 ? now : 1u;
		gWiiStreamBudgetLastGrowthMs = now;
	}else{
		gWiiStreamBudgetPressureSinceMs = 0;
		if(admissionPressure == 0 && WiiStreamHasExpansionHeadroom(snapshot) &&
		   gWiiStreamMemoryBudget < WII_STREAMING_MEMORY_BUDGET_HIGH &&
		   now - gWiiStreamBudgetLastGrowthMs >= WII_STREAMING_MEMORY_BUDGET_GROW_MS){
			gWiiStreamMemoryBudget += WII_STREAMING_MEMORY_BUDGET_STEP;
			if(gWiiStreamMemoryBudget > WII_STREAMING_MEMORY_BUDGET_HIGH)
				gWiiStreamMemoryBudget = WII_STREAMING_MEMORY_BUDGET_HIGH;
			gWiiStreamBudgetLastGrowthMs = now;
		}
	}

	CStreaming::ms_memoryAvailable = gWiiStreamMemoryBudget;
	if(oldBudget != gWiiStreamMemoryBudget)
		SYS_Report("[WII-STREAM] cache budget=%uKB->%uKB pressure=0x%X hard=0x%X admission=0x%X headroom=%d\n",
		           (unsigned)(oldBudget / 1024u),
		           (unsigned)(gWiiStreamMemoryBudget / 1024u),
		           (unsigned)pressure, (unsigned)hardPressure,
		           (unsigned)admissionPressure,
		           WiiStreamHasExpansionHeadroom(snapshot) ? 1 : 0);
	return gWiiStreamMemoryBudget;
}

static bool
WiiStreamApplyPersistentPressure(uint32 pressure)
{
	uint32 hardPressure = WiiStreamHardPressureBits(pressure);
	if(hardPressure == 0){
		gWiiStreamBudgetPressureSinceMs = 0;
		return false;
	}

	// Targeted reclamation above already tried the pool selected for pressure.
	// If only newlib remains blocked, lowering the cache cap just evicts scene
	// content and increases dependency reload churn without making progress.
	if((hardPressure & ~WII_STREAM_PRESSURE_NEWLIB) == 0)
		return false;

	uint32 now = WiiStreamNowMs();
	if(now == 0)
		return false;
	if(gWiiStreamBudgetPressureSinceMs == 0){
		gWiiStreamBudgetPressureSinceMs = now;
		return false;
	}
	if(now - gWiiStreamBudgetPressureSinceMs <
	   WII_STREAMING_MEMORY_BUDGET_PRESSURE_MS ||
	   gWiiStreamMemoryBudget <= WII_STREAMING_MEMORY_BUDGET_LOW)
		return false;

	size_t oldBudget = gWiiStreamMemoryBudget;
	gWiiStreamMemoryBudget -= WII_STREAMING_MEMORY_BUDGET_STEP;
	if(gWiiStreamMemoryBudget < WII_STREAMING_MEMORY_BUDGET_LOW)
		gWiiStreamMemoryBudget = WII_STREAMING_MEMORY_BUDGET_LOW;
	CStreaming::ms_memoryAvailable = gWiiStreamMemoryBudget;
	gWiiStreamBudgetPressureSinceMs = now;
	gWiiStreamBudgetLastGrowthMs = now;
	SYS_Report("[WII-STREAM] cache budget=%uKB->%uKB persistent=0x%X\n",
	           (unsigned)(oldBudget / 1024u),
	           (unsigned)(gWiiStreamMemoryBudget / 1024u),
	           (unsigned)pressure);
	return true;
}

static void
WiiStreamOnRequestDispatched(int32 streamId)
{
	CStreamingInfo *si = &CStreaming::ms_aInfoForModel[streamId];
	WiiStreamSetResourceBit(gWiiStreamDispatched, streamId);
	WiiStreamClearResourceBit(gWiiStreamDispatchedWorldRequest, streamId);
	if(WiiStreamGetResourceBit(gWiiStreamWorldRequest, streamId))
		WiiStreamSetResourceBit(gWiiStreamDispatchedWorldRequest, streamId);
	WiiStreamClearResourceBit(gWiiStreamWorldRequest, streamId);
	gWiiStreamDispatchedFlags[streamId] = si->m_flags;
	gWiiStreamDispatchedRequestClass[streamId] = gWiiStreamRequestClass[streamId];
	uint32 now = WiiStreamNowMs();
	uint32 queuedAt = gWiiStreamQueuedAtMs[streamId];
	bool priority = si->IsPriority();
	bool aged = WiiStreamIsAgedFairRequest(streamId, si, now);
	bool selected = gWiiStreamSelectedDispatchId == streamId;
	uint8 dispatchClass = selected ? gWiiStreamSelectedDispatchClass :
	                      WII_STREAM_DISPATCH_NORMAL;
#if WII_STREAM_MEMORY_DIAGNOSTICS && WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	gWiiStreamDiagDispatchMs[streamId] = now != 0 ? now : 1u;
	gWiiStreamDiagQueueWaitMs[streamId] =
		queuedAt != 0 && now >= queuedAt ? now - queuedAt : 0;
	gWiiStreamDiagLastDispatchClass[streamId] = dispatchClass;
#endif
	bool fair = dispatchClass == WII_STREAM_DISPATCH_FAIR || aged;
	bool priorityService = priority ||
	                       dispatchClass == WII_STREAM_DISPATCH_PRIORITY_CHAIN;
	if(fair){
		uint32 wait = selected && gWiiStreamSelectedDispatchWaitMs != 0 ?
		              gWiiStreamSelectedDispatchWaitMs :
		              (queuedAt != 0 ? now - queuedAt : 0);
		static uint32 sFairGrantReports = 0;
		sFairGrantReports++;
		if(sFairGrantReports <= 16 ||
		   (sFairGrantReports & (sFairGrantReports - 1)) == 0)
			SYS_Report("[WII-STREAM] fair id=%d wait=%ums priority=%d flags=0x%02X count=%u\n",
			           streamId, (unsigned)wait,
			           CStreaming::ms_numPriorityRequests, (unsigned)si->m_flags,
			           (unsigned)sFairGrantReports);
		gWiiStreamForegroundServicesSinceFair = 0;
	}else if(priorityService){
		if(gWiiStreamForegroundServicesSinceFair < WII_STREAM_FOREGROUND_BURST)
			gWiiStreamForegroundServicesSinceFair++;
	}else if(CStreaming::ms_numPriorityRequests != 0)
		gWiiStreamForegroundServicesSinceFair = 0;
	WiiStreamClearQueued(streamId);
}

static uint8
WiiStreamPoolNetGrowthMask(const WiiMemoryPoolUsage &before,
	const WiiMemoryPoolUsage &after)
{
	uint8 mask = 0;
	if(after.genericUsed > before.genericUsed)
		mask |= WII_STREAM_PRESSURE_GENERIC;
	if(after.newlibUsed > before.newlibUsed ||
	   after.rawArena2Remaining < before.rawArena2Remaining)
		mask |= WII_STREAM_PRESSURE_NEWLIB;
	if(after.gxUsed > before.gxUsed)
		mask |= WII_STREAM_PRESSURE_GX;
	return mask;
}

class WiiStreamPoolAttributionScope
{
public:
	enum CommitMode {
		REPLACE_RESIDENT_COST,
		ADD_RESIDENT_COST
	};

	explicit WiiStreamPoolAttributionScope(int32 streamId, CommitMode commitMode)
	: m_streamId(streamId), m_commitMode(commitMode), m_committed(false)
	{
		WiiMemoryGetPoolUsage(&m_before);
		RwUInt16 owner = streamId >= 0 && streamId < NUMSTREAMINFO ?
			(RwUInt16)streamId : WII_MEMORY_RESOURCE_OWNER_UNKNOWN;
		WiiMemoryBeginResourceAttribution(owner);
	}

	void Commit(void) { m_committed = true; }

	~WiiStreamPoolAttributionScope()
	{
		WiiMemoryResourceAttribution attribution;
		WiiMemoryEndResourceAttribution(&attribution);
		if(!m_committed || m_streamId < 0 || m_streamId >= NUMSTREAMINFO)
			return;
		uint8 state = CStreaming::ms_aInfoForModel[m_streamId].m_loadState;
		if(state != STREAMSTATE_LOADED && state != STREAMSTATE_STARTED)
			return;

		WiiMemoryPoolUsage after;
		WiiMemoryGetPoolUsage(&after);
		uint8 mask = (uint8)attribution.mask;
		mask |= WiiStreamPoolNetGrowthMask(m_before, after);
		// Models, TXDs and animations retain RenderWare metadata in newlib.
		if(m_streamId < STREAM_OFFSET_COL || m_streamId >= STREAM_OFFSET_ANIM)
			mask |= WII_STREAM_PRESSURE_NEWLIB;
		WiiStreamCommitResidentCost(m_streamId,
			m_commitMode == REPLACE_RESIDENT_COST, mask, m_before, after,
			attribution);
		gWiiStreamResourcePoolMask[m_streamId] |= mask;
	}

private:
	int32 m_streamId;
	CommitMode m_commitMode;
	bool m_committed;
	WiiMemoryPoolUsage m_before;
};

#if WII_STREAM_MEMORY_DIAGNOSTICS
enum WiiStreamDiagEventType
{
	WII_STREAM_DIAG_TRIM = 1,
	WII_STREAM_DIAG_LOAD,
	WII_STREAM_DIAG_RELOAD
};

enum WiiStreamDiagTrimReason
{
	WII_STREAM_TRIM_NONE = 0,
	WII_STREAM_TRIM_DEPENDENCY_UNWIND,
	WII_STREAM_TRIM_POOL_GENERIC,
	WII_STREAM_TRIM_POOL_NEWLIB,
	WII_STREAM_TRIM_POOL_GX,
	WII_STREAM_TRIM_PERSISTENT_BUDGET
};

struct WiiStreamDiagLatencyStats
{
	uint32 count;
	uint64 totalMs;
	uint32 maxMs;
	uint32 buckets[8];
};

struct WiiStreamDiagEvent
{
	uint8 type;
	uint8 flags;
	uint8 pressure;
	uint8 servicePressure;
	uint8 radarTile;
	uint8 requestClass;
	uint8 dispatchClass;
	uint8 trimReason;
	uint16 trimOrdinal;
	uint16 queueDepth;
	int16 area;
	int16 level;
	int32 streamId;
	int32 globalOldestId;
	uint32 trimEpisode;
	uint32 eventMs;
	uint32 elapsedMs;
	uint32 queueWaitMs;
	uint32 serviceMs;
	uint32 lastRequestAgeMs;
	uint32 lastLoadAgeMs;
	uint32 lastVisibleAgeMs;
	uint32 globalOldestAgeMs;
	uint32 streamUsedBefore;
	uint32 streamUsedAfter;
	uint32 streamTarget;
	WiiMemoryPoolSnapshot before;
	WiiMemoryPoolSnapshot after;
};

struct WiiStreamDiagTrimContext
{
	bool active;
	bool hadVictim;
	uint16 ordinal;
	uint32 episode;
	uint32 pressure;
	uint32 servicePressure;
	uint32 target;
	uint8 reason;
	int32 selectedId;
	int32 globalOldestId;
	uint32 globalOldestAgeMs;
};

#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
struct WiiStreamDiagChurnEntry
{
	int32 streamId;
	uint16 trims;
	uint16 reload5;
	uint16 reload10;
	uint16 loads;
	uint32 reloadTotalMs;
	uint32 reloadMaxMs;
	uint32 queueTotalMs;
	uint32 queueMaxMs;
	uint32 serviceTotalMs;
	uint32 serviceMaxMs;
	uint32 lastRequestAgeMs;
	uint32 lastLoadAgeMs;
	uint32 lastVisibleAgeMs;
	int32 globalOldestId;
	uint32 globalOldestAgeMs;
	uint16 classBiasBypasses;
	uint8 trimReason;
	uint8 requestClass;
	int16 area;
	int16 level;
};
#endif

static const uint32 WII_STREAM_DIAG_SUMMARY_MS = 5000u;
static const uint32 WII_STREAM_DIAG_FLUSH_MS = 100u;
static const uint32 WII_STREAM_DIAG_SLOW_LOAD_MS = 250u;
static const uint32 WII_STREAM_DIAG_RELOAD_5S_MS = 5000u;
static const uint32 WII_STREAM_DIAG_RELOAD_10S_MS = 10000u;
static const uint32 WII_STREAM_DIAG_LATENCY_BUCKETS[8] = {
	33u, 67u, 100u, 250u, 500u, 1000u, 5000u, 60000u
};
static const int32 WII_STREAM_DIAG_EVENT_CAPACITY =
	WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS ? 128 : 1;
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
static const int32 WII_STREAM_DIAG_CHURN_CAPACITY = 32;
static const int32 WII_STREAM_DIAG_CHURN_REPORT_COUNT = 8;
#endif

static uint32 gWiiStreamDiagRequestMs[NUMSTREAMINFO];
static uint32 gWiiStreamDiagLastTrimMs[NUMSTREAMINFO];
static uint8 gWiiStreamDiagRadarTile[TXDSTORESIZE];
static WiiStreamDiagEvent gWiiStreamDiagEvents[WII_STREAM_DIAG_EVENT_CAPACITY];
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
static WiiStreamDiagChurnEntry gWiiStreamDiagChurn[WII_STREAM_DIAG_CHURN_CAPACITY];
static uint32 gWiiStreamDiagChurnDropped;
#endif
static WiiStreamDiagLatencyStats gWiiStreamDiagLatency[3];
static WiiStreamDiagTrimContext gWiiStreamDiagTrim;
static uint16 gWiiStreamDiagEventHead;
static uint16 gWiiStreamDiagEventCount;
static uint32 gWiiStreamDiagTrimSequence;
static uint32 gWiiStreamDiagWindowStartMs;
static uint32 gWiiStreamDiagLastFlushMs;
static uint32 gWiiStreamDiagWindowTrimEpisodes;
static uint32 gWiiStreamDiagWindowVictims[4];
static uint32 gWiiStreamDiagWindowReload5;
static uint32 gWiiStreamDiagWindowReload10;
static uint32 gWiiStreamDiagWindowDropped;
static bool gWiiStreamDiagActive;

static uint32
WiiStreamDiagNowMs(void)
{
	uint32 now = WiiStreamNowMs();
	return now != 0 ? now : 1u;
}

static int32
WiiStreamDiagResourceKind(int32 streamId)
{
	if(streamId < STREAM_OFFSET_TXD)
		return 0;
	if(streamId < STREAM_OFFSET_COL)
		return 1;
	if(streamId < STREAM_OFFSET_ANIM)
		return 2;
	return 3;
}

static const char *
WiiStreamDiagResourceType(int32 streamId)
{
	static const char *types[] = { "model", "txd", "col", "anim" };
	return types[WiiStreamDiagResourceKind(streamId)];
}

static const char *
WiiStreamDiagResourceName(int32 streamId)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return "<invalid>";
	if(streamId < STREAM_OFFSET_TXD){
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(streamId);
		return mi ? mi->GetModelName() : "<no-model>";
	}
	if(streamId < STREAM_OFFSET_COL)
		return CTxdStore::GetTxdName(streamId - STREAM_OFFSET_TXD);
	if(streamId < STREAM_OFFSET_ANIM)
		return CColStore::GetColName(streamId - STREAM_OFFSET_COL);
	return CAnimManager::GetAnimationBlock(streamId - STREAM_OFFSET_ANIM)->name;
}

static uint8
WiiStreamDiagRadarTileForStream(int32 streamId)
{
	if(streamId < STREAM_OFFSET_TXD || streamId >= STREAM_OFFSET_COL)
		return 0xFFu;
	return gWiiStreamDiagRadarTile[streamId - STREAM_OFFSET_TXD];
}

#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
static const char *
WiiStreamDiagRequestClassName(uint8 requestClass)
{
	switch(requestClass){
	case WII_STREAM_REQUEST_TRANSITION_PREFETCH: return "transition";
	case WII_STREAM_REQUEST_WORLD_VISIBLE: return "visible";
	case WII_STREAM_REQUEST_RADAR: return "radar";
	default: return "normal";
	}
}

static const char *
WiiStreamDiagDispatchClassName(uint8 dispatchClass)
{
	switch(dispatchClass){
	case WII_STREAM_DISPATCH_PRIORITY_CHAIN: return "foreground";
	case WII_STREAM_DISPATCH_FAIR: return "fair";
	default: return "normal";
	}
}

static const char *
WiiStreamDiagTrimReasonName(uint8 reason)
{
	switch(reason){
	case WII_STREAM_TRIM_DEPENDENCY_UNWIND: return "dependency-unwind";
	case WII_STREAM_TRIM_POOL_GENERIC: return "pool-generic";
	case WII_STREAM_TRIM_POOL_NEWLIB: return "pool-newlib";
	case WII_STREAM_TRIM_POOL_GX: return "pool-gx";
	case WII_STREAM_TRIM_PERSISTENT_BUDGET: return "persistent-budget";
	default: return "none";
	}
}

static uint32
WiiStreamDiagAge(uint32 now, uint32 timestamp)
{
	return timestamp != 0 && now >= timestamp ? now - timestamp : UINT32_MAX;
}

static uint32
WiiStreamDiagChurnScore(const WiiStreamDiagChurnEntry &entry)
{
	return (uint32)entry.reload10 * 1024u + (uint32)entry.reload5 * 128u +
	       (uint32)entry.classBiasBypasses * 32u + (uint32)entry.trims * 2u +
	       entry.loads;
}

static WiiStreamDiagChurnEntry *
WiiStreamDiagFindChurn(int32 streamId, bool replaceWeakest)
{
	WiiStreamDiagChurnEntry *empty = nil;
	WiiStreamDiagChurnEntry *weakest = &gWiiStreamDiagChurn[0];
	for(int32 i = 0; i < WII_STREAM_DIAG_CHURN_CAPACITY; i++){
		WiiStreamDiagChurnEntry *entry = &gWiiStreamDiagChurn[i];
		if(entry->streamId == streamId)
			return entry;
		if(entry->streamId == -1 && empty == nil)
			empty = entry;
		if(WiiStreamDiagChurnScore(*entry) < WiiStreamDiagChurnScore(*weakest))
			weakest = entry;
	}
	WiiStreamDiagChurnEntry *entry = empty;
	if(entry == nil && replaceWeakest){
		entry = weakest;
		gWiiStreamDiagChurnDropped++;
	}else if(entry == nil){
		gWiiStreamDiagChurnDropped++;
		return nil;
	}
	memset(entry, 0, sizeof(*entry));
	entry->streamId = streamId;
	entry->globalOldestId = -1;
	return entry;
}

static void
WiiStreamDiagResetChurn(void)
{
	memset(gWiiStreamDiagChurn, 0, sizeof(gWiiStreamDiagChurn));
	for(int32 i = 0; i < WII_STREAM_DIAG_CHURN_CAPACITY; i++)
		gWiiStreamDiagChurn[i].streamId = -1;
	gWiiStreamDiagChurnDropped = 0;
}
#endif

static void
WiiStreamDiagResetWindow(uint32 now)
{
	memset(gWiiStreamDiagLatency, 0, sizeof(gWiiStreamDiagLatency));
	memset(gWiiStreamDiagWindowVictims, 0, sizeof(gWiiStreamDiagWindowVictims));
	gWiiStreamDiagWindowTrimEpisodes = 0;
	gWiiStreamDiagWindowReload5 = 0;
	gWiiStreamDiagWindowReload10 = 0;
	gWiiStreamDiagWindowDropped = 0;
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	WiiStreamDiagResetChurn();
#endif
	gWiiStreamDiagWindowStartMs = now;
}

static void
WiiStreamDiagReset(void)
{
	memset(gWiiStreamDiagRequestMs, 0, sizeof(gWiiStreamDiagRequestMs));
	memset(gWiiStreamDiagLastTrimMs, 0, sizeof(gWiiStreamDiagLastTrimMs));
	memset(gWiiStreamDiagRadarTile, 0xFF, sizeof(gWiiStreamDiagRadarTile));
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	memset(gWiiStreamDiagLastRequestMs, 0, sizeof(gWiiStreamDiagLastRequestMs));
	memset(gWiiStreamDiagDispatchMs, 0, sizeof(gWiiStreamDiagDispatchMs));
	memset(gWiiStreamDiagQueueWaitMs, 0, sizeof(gWiiStreamDiagQueueWaitMs));
	memset(gWiiStreamDiagLastLoadMs, 0, sizeof(gWiiStreamDiagLastLoadMs));
	memset(gWiiStreamDiagLastWorldVisibleMs, 0,
	       sizeof(gWiiStreamDiagLastWorldVisibleMs));
	memset(gWiiStreamDiagLastRequestClass, 0,
	       sizeof(gWiiStreamDiagLastRequestClass));
	memset(gWiiStreamDiagLastDispatchClass, 0,
	       sizeof(gWiiStreamDiagLastDispatchClass));
#endif
	memset(&gWiiStreamDiagTrim, 0, sizeof(gWiiStreamDiagTrim));
	gWiiStreamDiagEventHead = 0;
	gWiiStreamDiagEventCount = 0;
	gWiiStreamDiagTrimSequence = 0;
	uint32 now = WiiStreamDiagNowMs();
	gWiiStreamDiagLastFlushMs = now;
	gWiiStreamDiagActive = true;
	WiiStreamDiagResetWindow(now);
	size_t stateBytes = sizeof(gWiiStreamDiagRequestMs) +
	                    sizeof(gWiiStreamDiagLastTrimMs) +
	                    sizeof(gWiiStreamDiagRadarTile) +
	                    sizeof(gWiiStreamDiagEvents);
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	stateBytes += sizeof(gWiiStreamDiagLastRequestMs) +
	              sizeof(gWiiStreamDiagDispatchMs) +
	              sizeof(gWiiStreamDiagQueueWaitMs) +
	              sizeof(gWiiStreamDiagLastLoadMs) +
	              sizeof(gWiiStreamDiagLastWorldVisibleMs) +
	              sizeof(gWiiStreamDiagLastRequestClass) +
	              sizeof(gWiiStreamDiagLastDispatchClass) +
	              sizeof(gWiiStreamDiagChurn);
#endif
	SYS_Report("[WII-STREAM-DIAG] enabled state=%uKB events=%d detail=%s summary=%ums schema=lifecycle-v2 pools=used/free/largest bytes nl=used/free/raw\n",
	           (unsigned)(stateBytes / 1024u),
	           WII_STREAM_DIAG_EVENT_CAPACITY,
	           WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS ? "per-resource" : "summary-only",
	           (unsigned)WII_STREAM_DIAG_SUMMARY_MS);
}

static void WiiStreamDiagPrintEvent(const WiiStreamDiagEvent &event);

static WiiStreamDiagEvent *
WiiStreamDiagAllocEvent(uint8 type)
{
#if !WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	(void)type;
	return nil;
#else
	if(gWiiStreamDiagEventCount >= WII_STREAM_DIAG_EVENT_CAPACITY){
		gWiiStreamDiagEventHead = (gWiiStreamDiagEventHead + 1) % WII_STREAM_DIAG_EVENT_CAPACITY;
		gWiiStreamDiagEventCount--;
		gWiiStreamDiagWindowDropped++;
	}
	uint16 index = (gWiiStreamDiagEventHead + gWiiStreamDiagEventCount) % WII_STREAM_DIAG_EVENT_CAPACITY;
	WiiStreamDiagEvent *event = &gWiiStreamDiagEvents[index];
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->radarTile = 0xFFu;
	event->globalOldestId = -1;
	event->lastRequestAgeMs = UINT32_MAX;
	event->lastLoadAgeMs = UINT32_MAX;
	event->lastVisibleAgeMs = UINT32_MAX;
	event->globalOldestAgeMs = UINT32_MAX;
	event->area = (int16)CGame::currArea;
	event->level = (int16)CGame::currLevel;
	event->eventMs = WiiStreamDiagNowMs();
	gWiiStreamDiagEventCount++;
	return event;
#endif
}

static void
WiiStreamDiagRecordLatency(WiiStreamDiagLatencyStats &stats, uint32 elapsedMs)
{
	stats.count++;
	stats.totalMs += elapsedMs;
	if(elapsedMs > stats.maxMs)
		stats.maxMs = elapsedMs;
	for(int32 i = 0; i < ARRAY_SIZE(WII_STREAM_DIAG_LATENCY_BUCKETS); i++){
		if(elapsedMs <= WII_STREAM_DIAG_LATENCY_BUCKETS[i] || i == ARRAY_SIZE(WII_STREAM_DIAG_LATENCY_BUCKETS) - 1){
			stats.buckets[i]++;
			break;
		}
	}
}

static uint32
WiiStreamDiagLatencyP95Bucket(const WiiStreamDiagLatencyStats &stats)
{
	if(stats.count == 0)
		return 0;
	uint32 wanted = (stats.count * 95u + 99u) / 100u;
	uint32 seen = 0;
	for(int32 i = 0; i < ARRAY_SIZE(WII_STREAM_DIAG_LATENCY_BUCKETS); i++){
		seen += stats.buckets[i];
		if(seen >= wanted)
			return WII_STREAM_DIAG_LATENCY_BUCKETS[i];
	}
	return WII_STREAM_DIAG_LATENCY_BUCKETS[ARRAY_SIZE(WII_STREAM_DIAG_LATENCY_BUCKETS) - 1];
}

static uint32
WiiStreamDiagLatencyAverage(const WiiStreamDiagLatencyStats &stats)
{
	return stats.count ? (uint32)(stats.totalMs / stats.count) : 0;
}

static void
WiiStreamDiagPrintEvent(const WiiStreamDiagEvent &event)
{
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	const char *type = WiiStreamDiagResourceType(event.streamId);
	const char *name = WiiStreamDiagResourceName(event.streamId);
	if(event.type == WII_STREAM_DIAG_TRIM){
		SYS_Report("[WII-TRIM-VICTIM] at=%u ep=%u n=%u reason=%s p=0x%X svc=0x%X id=%d type=%s name='%s' flags=0x%02X req=%s age=req%u/load%u/vis%u cf=%d/%u bypass=%d loc=%d/%d stream=%u>%u target=%u g=%u/%u/%u>%u/%u/%u nl=%u/%u/%u>%u/%u/%u gx=%u/%u/%u>%u/%u/%u\n",
		           (unsigned)event.eventMs,
		           (unsigned)event.trimEpisode, (unsigned)event.trimOrdinal,
		           WiiStreamDiagTrimReasonName(event.trimReason),
		           (unsigned)event.pressure, (unsigned)event.servicePressure,
		           event.streamId, type, name,
		           (unsigned)event.flags,
		           WiiStreamDiagRequestClassName(event.requestClass),
		           (unsigned)event.lastRequestAgeMs,
		           (unsigned)event.lastLoadAgeMs,
		           (unsigned)event.lastVisibleAgeMs,
		           event.globalOldestId, (unsigned)event.globalOldestAgeMs,
		           event.globalOldestId >= 0 && event.globalOldestId != event.streamId,
		           (int)event.level, (int)event.area,
		           (unsigned)event.streamUsedBefore, (unsigned)event.streamUsedAfter,
		           (unsigned)event.streamTarget,
		           (unsigned)event.before.genericUsed, (unsigned)event.before.genericFree,
		           (unsigned)event.before.genericLargest, (unsigned)event.after.genericUsed,
		           (unsigned)event.after.genericFree, (unsigned)event.after.genericLargest,
		           (unsigned)event.before.newlibUsed, (unsigned)event.before.newlibFree,
		           (unsigned)event.before.rawArena2Remaining, (unsigned)event.after.newlibUsed,
		           (unsigned)event.after.newlibFree, (unsigned)event.after.rawArena2Remaining,
		           (unsigned)event.before.gxUsed, (unsigned)event.before.gxFree,
		           (unsigned)event.before.gxLargest, (unsigned)event.after.gxUsed,
		           (unsigned)event.after.gxFree, (unsigned)event.after.gxLargest);
	}else if(event.type == WII_STREAM_DIAG_RELOAD){
		SYS_Report("[WII-TRIM-RELOAD] at=%u id=%d type=%s name='%s' flags=0x%02X req=%s after=%ums bucket=%s age=load%u/vis%u loc=%d/%d radar=%d,%d\n",
		           (unsigned)event.eventMs, event.streamId, type, name, (unsigned)event.flags,
		           WiiStreamDiagRequestClassName(event.requestClass),
		           (unsigned)event.elapsedMs,
		           event.elapsedMs <= WII_STREAM_DIAG_RELOAD_5S_MS ? "<=5s" : "5-10s",
		           (unsigned)event.lastLoadAgeMs,
		           (unsigned)event.lastVisibleAgeMs,
		           (int)event.level, (int)event.area,
		           event.radarTile == 0xFFu ? -1 : event.radarTile & 0x0F,
		           event.radarTile == 0xFFu ? -1 : event.radarTile >> 4);
	}else{
		SYS_Report("[WII-LOAD-LAT] at=%u id=%d type=%s name='%s' flags=0x%02X req=%s dispatch=%s total=%u queue=%u service=%u q=%u loc=%d/%d radar=%d,%d\n",
		           (unsigned)event.eventMs, event.streamId, type, name, (unsigned)event.flags,
		           WiiStreamDiagRequestClassName(event.requestClass),
		           WiiStreamDiagDispatchClassName(event.dispatchClass),
		           (unsigned)event.elapsedMs, (unsigned)event.queueWaitMs,
		           (unsigned)event.serviceMs, (unsigned)event.queueDepth,
		           (int)event.level, (int)event.area,
		           event.radarTile == 0xFFu ? -1 : event.radarTile & 0x0F,
		           event.radarTile == 0xFFu ? -1 : event.radarTile >> 4);
	}
#else
	(void)event;
#endif
}

struct WiiStreamResidentTotals
{
	uint32 loadedCount;
	uint32 startedCount;
	uint32 genericKiB;
	uint32 newlibKiB;
	uint32 gxKiB;
	uint32 genericUnknown;
	uint32 newlibUnknown;
	uint32 gxUnknown;
};

static void
WiiStreamDiagAddResidentField(uint16 value, uint32 *knownKiB,
	uint32 *unknownCount)
{
	if(value == WII_STREAM_RESIDENT_UNKNOWN_KIB)
		(*unknownCount)++;
	else
		*knownKiB += value;
}

static int32
WiiStreamDiagResidentResidualKiB(RwUInt32 usedBytes, uint32 knownKiB)
{
	int64 residual = (int64)(usedBytes / 1024u) - knownKiB;
	if(residual > INT32_MAX)
		return INT32_MAX;
	if(residual < INT32_MIN)
		return INT32_MIN;
	return (int32)residual;
}

static void
WiiStreamDiagPrintResidentSummary(void)
{
	WiiStreamResidentTotals totals;
	memset(&totals, 0, sizeof(totals));
	for(int32 streamId = 0; streamId < NUMSTREAMINFO; streamId++){
		uint8 state = CStreaming::ms_aInfoForModel[streamId].m_loadState;
		if(state != STREAMSTATE_LOADED && state != STREAMSTATE_STARTED)
			continue;
		if(state == STREAMSTATE_LOADED)
			totals.loadedCount++;
		else
			totals.startedCount++;
		const WiiStreamResidentCost &cost = gWiiStreamResidentCost[streamId];
		WiiStreamDiagAddResidentField(cost.genericKiB, &totals.genericKiB,
			&totals.genericUnknown);
		WiiStreamDiagAddResidentField(cost.newlibKiB, &totals.newlibKiB,
			&totals.newlibUnknown);
		WiiStreamDiagAddResidentField(cost.gxKiB, &totals.gxKiB,
			&totals.gxUnknown);
	}

	WiiMemoryPoolUsage usage;
	WiiMemoryGetPoolUsage(&usage);
	uint32 ownedGenericBytes = 0;
	uint32 unknownGenericBytes = 0;
	uint32 ownedGxBytes = 0;
	uint32 unknownGxBytes = 0;
	rw::gx::texPoolGetOwnerStats(&ownedGenericBytes, &unknownGenericBytes,
		&ownedGxBytes, &unknownGxBytes);
	int32 rawArena2DeltaKiB = 0;
	if(gWiiStreamResidentHasLastRawArena2){
		int64 rawDelta = (int64)usage.rawArena2Remaining -
			gWiiStreamResidentLastRawArena2Remaining;
		if(rawDelta > INT32_MAX * 1024ll)
			rawDelta = INT32_MAX * 1024ll;
		if(rawDelta < INT32_MIN * 1024ll)
			rawDelta = INT32_MIN * 1024ll;
		rawArena2DeltaKiB = (int32)(rawDelta / 1024ll);
	}
	gWiiStreamResidentLastRawArena2Remaining = usage.rawArena2Remaining;
	gWiiStreamResidentHasLastRawArena2 = true;

	SYS_Report("[WII-RESIDENT] live=L%u/S%u ledger=g%u/u%u nl%u/u%u gx%u/u%u pool=g%u nl%u gx%u residual=g%+d nl%+d gx%+d raw2free=%u/d%+d owner=g%u/u%u gx%u/u%u sat=%u under=%u\n",
		(unsigned)totals.loadedCount, (unsigned)totals.startedCount,
		(unsigned)totals.genericKiB, (unsigned)totals.genericUnknown,
		(unsigned)totals.newlibKiB, (unsigned)totals.newlibUnknown,
		(unsigned)totals.gxKiB, (unsigned)totals.gxUnknown,
		(unsigned)(usage.genericUsed / 1024u),
		(unsigned)(usage.newlibUsed / 1024u),
		(unsigned)(usage.gxUsed / 1024u),
		WiiStreamDiagResidentResidualKiB(usage.genericUsed, totals.genericKiB),
		WiiStreamDiagResidentResidualKiB(usage.newlibUsed, totals.newlibKiB),
		WiiStreamDiagResidentResidualKiB(usage.gxUsed, totals.gxKiB),
		(unsigned)(usage.rawArena2Remaining / 1024u),
		rawArena2DeltaKiB,
		(unsigned)(ownedGenericBytes / 1024u),
		(unsigned)(unknownGenericBytes / 1024u),
		(unsigned)(ownedGxBytes / 1024u),
		(unsigned)(unknownGxBytes / 1024u),
		(unsigned)gWiiStreamResidentSaturationCount,
	           (unsigned)gWiiStreamResidentUnderflowCount);
}

#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
static void
WiiStreamDiagPrintChurnSummary(void)
{
	bool emitted[WII_STREAM_DIAG_CHURN_CAPACITY];
	memset(emitted, 0, sizeof(emitted));
	for(int32 rank = 0; rank < WII_STREAM_DIAG_CHURN_REPORT_COUNT; rank++){
		int32 best = -1;
		uint32 bestScore = 0;
		for(int32 i = 0; i < WII_STREAM_DIAG_CHURN_CAPACITY; i++){
			if(emitted[i] || gWiiStreamDiagChurn[i].streamId < 0)
				continue;
			uint32 score = WiiStreamDiagChurnScore(gWiiStreamDiagChurn[i]);
			if(best == -1 || score > bestScore){
				best = i;
				bestScore = score;
			}
		}
		if(best == -1 || bestScore == 0)
			break;
		emitted[best] = true;
		const WiiStreamDiagChurnEntry &entry = gWiiStreamDiagChurn[best];
		uint32 reloadAverage = entry.reload10 ?
		                       entry.reloadTotalMs / entry.reload10 : 0;
		uint32 queueAverage = entry.loads ? entry.queueTotalMs / entry.loads : 0;
		uint32 serviceAverage = entry.loads ? entry.serviceTotalMs / entry.loads : 0;
		SYS_Report("[WII-CHURN] rank=%u id=%d type=%s name='%s' trim=%u r5=%u r10=%u load=%u gap=%u/%u queue=%u/%u service=%u/%u reason=%s req=%s age=req%u/load%u/vis%u cf=%d/%u bypass=%u loc=%d/%d dropped=%u\n",
		           (unsigned)(rank + 1), entry.streamId,
		           WiiStreamDiagResourceType(entry.streamId),
		           WiiStreamDiagResourceName(entry.streamId),
		           (unsigned)entry.trims, (unsigned)entry.reload5,
		           (unsigned)entry.reload10, (unsigned)entry.loads,
		           (unsigned)reloadAverage, (unsigned)entry.reloadMaxMs,
		           (unsigned)queueAverage, (unsigned)entry.queueMaxMs,
		           (unsigned)serviceAverage, (unsigned)entry.serviceMaxMs,
		           WiiStreamDiagTrimReasonName(entry.trimReason),
		           WiiStreamDiagRequestClassName(entry.requestClass),
		           (unsigned)entry.lastRequestAgeMs,
		           (unsigned)entry.lastLoadAgeMs,
		           (unsigned)entry.lastVisibleAgeMs,
		           entry.globalOldestId, (unsigned)entry.globalOldestAgeMs,
		           (unsigned)entry.classBiasBypasses,
		           (int)entry.level, (int)entry.area,
		           (unsigned)gWiiStreamDiagChurnDropped);
	}
}
#endif

static void
WiiStreamDiagPrintSummary(uint32 now)
{
	uint32 windowMs = now - gWiiStreamDiagWindowStartMs;
	const WiiStreamDiagLatencyStats &models = gWiiStreamDiagLatency[0];
	const WiiStreamDiagLatencyStats &txds = gWiiStreamDiagLatency[1];
	const WiiStreamDiagLatencyStats &radar = gWiiStreamDiagLatency[2];
	SYS_Report("[WII-STREAM-DIAG] win=%ums trimEp=%u victims=%u m/t/c/a=%u/%u/%u/%u reload5=%u reload10=%u model=n%u/avg%u/p95b%u/max%u txd=n%u/avg%u/p95b%u/max%u radar=n%u/avg%u/p95b%u/max%u pending=%d evq=%u dropped=%u\n",
	           (unsigned)windowMs, (unsigned)gWiiStreamDiagWindowTrimEpisodes,
	           (unsigned)(gWiiStreamDiagWindowVictims[0] + gWiiStreamDiagWindowVictims[1] +
	                      gWiiStreamDiagWindowVictims[2] + gWiiStreamDiagWindowVictims[3]),
	           (unsigned)gWiiStreamDiagWindowVictims[0],
	           (unsigned)gWiiStreamDiagWindowVictims[1],
	           (unsigned)gWiiStreamDiagWindowVictims[2],
	           (unsigned)gWiiStreamDiagWindowVictims[3],
	           (unsigned)gWiiStreamDiagWindowReload5,
	           (unsigned)gWiiStreamDiagWindowReload10,
	           (unsigned)models.count, (unsigned)WiiStreamDiagLatencyAverage(models),
	           (unsigned)WiiStreamDiagLatencyP95Bucket(models), (unsigned)models.maxMs,
	           (unsigned)txds.count, (unsigned)WiiStreamDiagLatencyAverage(txds),
	           (unsigned)WiiStreamDiagLatencyP95Bucket(txds), (unsigned)txds.maxMs,
	           (unsigned)radar.count, (unsigned)WiiStreamDiagLatencyAverage(radar),
	           (unsigned)WiiStreamDiagLatencyP95Bucket(radar), (unsigned)radar.maxMs,
	           CStreaming::ms_numModelsRequested, (unsigned)gWiiStreamDiagEventCount,
	           (unsigned)gWiiStreamDiagWindowDropped);
	#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	WiiStreamDiagPrintChurnSummary();
	#endif
	WiiStreamDiagPrintResidentSummary();
	WiiMemoryPhase0Counters phase0;
	phase0.requestPending = CStreaming::ms_numModelsRequested > 0 ?
	                        (RwUInt32)CStreaming::ms_numModelsRequested : 0;
	phase0.requestRetryCount = gWiiPhase0RequestRetryCount;
	phase0.hardFallbackCount = gWiiPhase0HardFallbackCount;
	phase0.txdFailureCount = -1;
	WiiMemoryPhase0ReportSnapshot(&phase0);
	WiiStreamDiagResetWindow(now);
}

static void
WiiStreamDiagPump(void)
{
	uint32 now = WiiStreamDiagNowMs();
	if(!gWiiStreamDiagActive)
		return;

#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	if(now - gWiiStreamDiagLastFlushMs >= WII_STREAM_DIAG_FLUSH_MS){
		int32 limit = gWiiStreamDiagEventCount > 64 ? 2 : 1;
		while(gWiiStreamDiagEventCount > 0 && limit-- > 0){
			WiiStreamDiagPrintEvent(gWiiStreamDiagEvents[gWiiStreamDiagEventHead]);
			gWiiStreamDiagEventHead = (gWiiStreamDiagEventHead + 1) % WII_STREAM_DIAG_EVENT_CAPACITY;
			gWiiStreamDiagEventCount--;
		}
		gWiiStreamDiagLastFlushMs = now;
	}
#endif
	if(now - gWiiStreamDiagWindowStartMs >= WII_STREAM_DIAG_SUMMARY_MS)
		WiiStreamDiagPrintSummary(now);
}

static void
WiiStreamDiagBeginTrim(uint32 pressure, size_t target)
{
	if(!gWiiStreamDiagActive)
		return;
	gWiiStreamDiagTrim.active = true;
	gWiiStreamDiagTrim.hadVictim = false;
	gWiiStreamDiagTrim.ordinal = 0;
	gWiiStreamDiagTrim.episode = ++gWiiStreamDiagTrimSequence;
	gWiiStreamDiagTrim.pressure = pressure;
	gWiiStreamDiagTrim.servicePressure = 0;
	gWiiStreamDiagTrim.target = (uint32)target;
	gWiiStreamDiagTrim.reason = WII_STREAM_TRIM_NONE;
	gWiiStreamDiagTrim.selectedId = -1;
	gWiiStreamDiagTrim.globalOldestId = -1;
	gWiiStreamDiagTrim.globalOldestAgeMs = UINT32_MAX;
}

static void
WiiStreamDiagSetTrimPressure(uint32 pressure, uint32 servicePressure)
{
	if(gWiiStreamDiagTrim.active){
		gWiiStreamDiagTrim.pressure = pressure;
		gWiiStreamDiagTrim.servicePressure = servicePressure;
	}
}

static void
WiiStreamDiagSetTrimReason(uint8 reason)
{
	if(gWiiStreamDiagTrim.active)
		gWiiStreamDiagTrim.reason = reason;
}

#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
static void
WiiStreamDiagSetVictimCounterfactual(int32 selectedId, int32 oldestId,
	uint32 oldestAgeMs)
{
	gWiiStreamDiagTrim.selectedId = selectedId;
	gWiiStreamDiagTrim.globalOldestId = oldestId;
	gWiiStreamDiagTrim.globalOldestAgeMs = oldestAgeMs;
}
#endif

static void
WiiStreamDiagEndTrim(void)
{
	gWiiStreamDiagTrim.active = false;
}

static bool
WiiStreamDiagCapturingTrim(void)
{
	return gWiiStreamDiagActive && gWiiStreamDiagTrim.active;
}

static void
WiiStreamDiagRecordTrimVictim(int32 streamId)
{
	if(!WiiStreamDiagCapturingTrim())
		return;
	uint32 now = WiiStreamDiagNowMs();
	if(!gWiiStreamDiagTrim.hadVictim){
		gWiiStreamDiagTrim.hadVictim = true;
		gWiiStreamDiagWindowTrimEpisodes++;
	}
	gWiiStreamDiagTrim.ordinal++;
	gWiiStreamDiagLastTrimMs[streamId] = now ? now : 1u;
	gWiiStreamDiagWindowVictims[WiiStreamDiagResourceKind(streamId)]++;
}

#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
static void
WiiStreamDiagRecordTrimVictimEvent(int32 streamId, uint8 flags,
	const WiiMemoryPoolSnapshot &before, const WiiMemoryPoolSnapshot &after,
	size_t streamUsedBefore, size_t streamUsedAfter)
{
	WiiStreamDiagEvent *event = WiiStreamDiagAllocEvent(WII_STREAM_DIAG_TRIM);
	if(event == nil)
		return;
	event->streamId = streamId;
	event->flags = flags;
	event->pressure = (uint8)gWiiStreamDiagTrim.pressure;
	event->servicePressure = (uint8)gWiiStreamDiagTrim.servicePressure;
	event->trimReason = gWiiStreamDiagTrim.reason;
	event->requestClass = gWiiStreamDiagLastRequestClass[streamId];
	event->trimOrdinal = gWiiStreamDiagTrim.ordinal;
	event->trimEpisode = gWiiStreamDiagTrim.episode;
	uint32 now = event->eventMs;
	event->lastRequestAgeMs = WiiStreamDiagAge(
		now, gWiiStreamDiagLastRequestMs[streamId]);
	event->lastLoadAgeMs = WiiStreamDiagAge(
		now, gWiiStreamDiagLastLoadMs[streamId]);
	event->lastVisibleAgeMs = WiiStreamDiagAge(
		now, gWiiStreamDiagLastWorldVisibleMs[streamId]);
	event->globalOldestId = gWiiStreamDiagTrim.globalOldestId;
	event->globalOldestAgeMs = gWiiStreamDiagTrim.globalOldestAgeMs;
	event->streamUsedBefore = (uint32)streamUsedBefore;
	event->streamUsedAfter = (uint32)streamUsedAfter;
	event->streamTarget = gWiiStreamDiagTrim.target;
	event->before = before;
	event->after = after;
	WiiStreamDiagChurnEntry *churn = WiiStreamDiagFindChurn(streamId, false);
	if(churn){
		if(churn->trims != UINT16_MAX)
			churn->trims++;
		churn->lastRequestAgeMs = event->lastRequestAgeMs;
		churn->lastLoadAgeMs = event->lastLoadAgeMs;
		churn->lastVisibleAgeMs = event->lastVisibleAgeMs;
		churn->globalOldestId = event->globalOldestId;
		churn->globalOldestAgeMs = event->globalOldestAgeMs;
		if(event->globalOldestId >= 0 && event->globalOldestId != streamId &&
		   churn->classBiasBypasses != UINT16_MAX)
			churn->classBiasBypasses++;
		churn->trimReason = event->trimReason;
		churn->requestClass = event->requestClass;
		churn->area = event->area;
		churn->level = event->level;
	}
}
#endif

static void
WiiStreamDiagOnRequest(int32 streamId, uint8 flags)
{
	if(!gWiiStreamDiagActive || streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
	uint32 now = WiiStreamDiagNowMs();
	uint32 lastTrim = gWiiStreamDiagLastTrimMs[streamId];
	if(lastTrim != 0){
		uint32 elapsed = now - lastTrim;
		if(elapsed <= WII_STREAM_DIAG_RELOAD_10S_MS){
			gWiiStreamDiagWindowReload10++;
			if(elapsed <= WII_STREAM_DIAG_RELOAD_5S_MS)
				gWiiStreamDiagWindowReload5++;
			WiiStreamDiagEvent *event = WiiStreamDiagAllocEvent(WII_STREAM_DIAG_RELOAD);
			if(event){
				event->streamId = streamId;
				event->flags = flags;
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
				event->requestClass = gWiiStreamDiagLastRequestClass[streamId];
#endif
				event->elapsedMs = elapsed;
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
				event->lastLoadAgeMs = WiiStreamDiagAge(
					now, gWiiStreamDiagLastLoadMs[streamId]);
				event->lastVisibleAgeMs = WiiStreamDiagAge(
					now, gWiiStreamDiagLastWorldVisibleMs[streamId]);
#endif
				event->radarTile = WiiStreamDiagRadarTileForStream(streamId);
			}
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
			WiiStreamDiagChurnEntry *churn = WiiStreamDiagFindChurn(streamId, true);
			if(churn){
				if(churn->reload10 != UINT16_MAX)
					churn->reload10++;
				if(elapsed <= WII_STREAM_DIAG_RELOAD_5S_MS && churn->reload5 != UINT16_MAX)
					churn->reload5++;
				churn->reloadTotalMs = WiiStreamSaturatingAdd(
					churn->reloadTotalMs, elapsed);
				if(elapsed > churn->reloadMaxMs)
					churn->reloadMaxMs = elapsed;
				churn->requestClass = gWiiStreamDiagLastRequestClass[streamId];
				churn->lastLoadAgeMs = WiiStreamDiagAge(
					now, gWiiStreamDiagLastLoadMs[streamId]);
				churn->lastVisibleAgeMs = WiiStreamDiagAge(
					now, gWiiStreamDiagLastWorldVisibleMs[streamId]);
				churn->area = (int16)CGame::currArea;
				churn->level = (int16)CGame::currLevel;
			}
#endif
		}
		gWiiStreamDiagLastTrimMs[streamId] = 0;
	}
	if(streamId < STREAM_OFFSET_COL && gWiiStreamDiagRequestMs[streamId] == 0)
		gWiiStreamDiagRequestMs[streamId] = now ? now : 1u;
}

static void
WiiStreamDiagCancelRequest(int32 streamId, uint8 oldState)
{
	if(!gWiiStreamDiagActive || streamId < 0 || streamId >= STREAM_OFFSET_COL)
		return;
	if(oldState == STREAMSTATE_INQUEUE || oldState == STREAMSTATE_READING || oldState == STREAMSTATE_STARTED)
		gWiiStreamDiagRequestMs[streamId] = 0;
}

static void
WiiStreamDiagOnLoaded(int32 streamId)
{
	if(!gWiiStreamDiagActive || streamId < 0 || streamId >= STREAM_OFFSET_COL)
		return;
	uint32 now = WiiStreamDiagNowMs();
	uint32 requested = gWiiStreamDiagRequestMs[streamId];
	gWiiStreamDiagRequestMs[streamId] = 0;
	uint32 previousLoad = 0;
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	previousLoad = gWiiStreamDiagLastLoadMs[streamId];
	gWiiStreamDiagLastLoadMs[streamId] = now != 0 ? now : 1u;
#endif
	if(requested == 0)
		return;

	uint32 elapsed = now - requested;
	uint32 queueWaitMs = 0;
	uint32 serviceMs = elapsed;
	uint8 requestClass = WII_STREAM_REQUEST_NORMAL;
	uint8 dispatchClass = WII_STREAM_DISPATCH_NORMAL;
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	queueWaitMs = gWiiStreamDiagQueueWaitMs[streamId];
	uint32 dispatched = gWiiStreamDiagDispatchMs[streamId];
	serviceMs = dispatched != 0 && now >= dispatched ? now - dispatched : elapsed;
	requestClass = gWiiStreamDispatchedRequestClass[streamId];
	dispatchClass = gWiiStreamDiagLastDispatchClass[streamId];
	WiiStreamDiagChurnEntry *churn = WiiStreamDiagFindChurn(streamId, false);
	if(churn){
		if(churn->loads != UINT16_MAX)
			churn->loads++;
		churn->queueTotalMs = WiiStreamSaturatingAdd(churn->queueTotalMs, queueWaitMs);
		churn->serviceTotalMs = WiiStreamSaturatingAdd(churn->serviceTotalMs, serviceMs);
		if(queueWaitMs > churn->queueMaxMs)
			churn->queueMaxMs = queueWaitMs;
		if(serviceMs > churn->serviceMaxMs)
			churn->serviceMaxMs = serviceMs;
		churn->requestClass = requestClass;
	}
#endif
	uint8 radarTile = WiiStreamDiagRadarTileForStream(streamId);
	int32 category = streamId < STREAM_OFFSET_TXD ? 0 : (radarTile == 0xFFu ? 1 : 2);
	WiiStreamDiagRecordLatency(gWiiStreamDiagLatency[category], elapsed);
	if(radarTile == 0xFFu && elapsed < WII_STREAM_DIAG_SLOW_LOAD_MS)
		return;

	WiiStreamDiagEvent *event = WiiStreamDiagAllocEvent(WII_STREAM_DIAG_LOAD);
	if(event == nil)
		return;
	event->streamId = streamId;
	event->flags = CStreaming::ms_aInfoForModel[streamId].m_flags;
	event->requestClass = requestClass;
	event->dispatchClass = dispatchClass;
	event->elapsedMs = elapsed;
	event->queueWaitMs = queueWaitMs;
	event->serviceMs = serviceMs;
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	event->lastRequestAgeMs = WiiStreamDiagAge(
		now, gWiiStreamDiagLastRequestMs[streamId]);
	event->lastLoadAgeMs = WiiStreamDiagAge(now, previousLoad);
	event->lastVisibleAgeMs = WiiStreamDiagAge(
		now, gWiiStreamDiagLastWorldVisibleMs[streamId]);
#endif
	event->radarTile = radarTile;
	event->queueDepth = (uint16)Min(CStreaming::ms_numModelsRequested, 65535);
}
#endif

#if WII_SPECIAL_STREAM_DIAGNOSTICS
static const char *
WiiStreamStateName(uint8 state)
{
	switch (state) {
	case STREAMSTATE_NOTLOADED: return "notloaded";
	case STREAMSTATE_LOADED: return "loaded";
	case STREAMSTATE_INQUEUE: return "inqueue";
	case STREAMSTATE_READING: return "reading";
	case STREAMSTATE_STARTED: return "started";
	default: return "unknown";
	}
}
#endif
#endif

#ifdef WII
static bool
IsTxdRequiredByScriptModel(int32 txdId, int32 ignoredModel)
{
	for(int32 modelId = 0; modelId < MODELINFOSIZE; modelId++){
		if(modelId == ignoredModel)
			continue;
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(modelId);
		if(mi && mi->GetTxdSlot() == txdId &&
		   (CStreaming::ms_aInfoForModel[modelId].m_flags & STREAMFLAGS_SCRIPTOWNED))
			return true;
	}
	return false;
}

static void
ReleaseScriptOwnedTxdIfUnused(int32 txdId, int32 ignoredModel = -1)
{
	if(txdId < 0 || txdId >= TXDSTORESIZE)
		return;
	int32 streamId = txdId + STREAM_OFFSET_TXD;
	if((CStreaming::ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_SCRIPTOWNED) == 0 ||
	   IsTxdRequiredByScriptModel(txdId, ignoredModel))
		return;
	CStreaming::SetMissionDoesntRequireModel(streamId);
}
#endif

bool CStreaming::ms_disableStreaming;
bool CStreaming::ms_bLoadingBigModel;
int32 CStreaming::ms_numModelsRequested;
CStreamingInfo CStreaming::ms_aInfoForModel[NUMSTREAMINFO];
CStreamingInfo CStreaming::ms_startLoadedList;
CStreamingInfo CStreaming::ms_endLoadedList;
CStreamingInfo CStreaming::ms_startRequestedList;
CStreamingInfo CStreaming::ms_endRequestedList;
int32 CStreaming::ms_oldSectorX;
int32 CStreaming::ms_oldSectorY;
int32 CStreaming::ms_streamingBufferSize;
#ifndef ONE_THREAD_PER_CHANNEL
int8 *CStreaming::ms_pStreamingBuffer[2];
#else
int8 *CStreaming::ms_pStreamingBuffer[4];
#endif
size_t CStreaming::ms_memoryUsed;
CStreamingChannel CStreaming::ms_channel[2];
int32 CStreaming::ms_channelError;
int32 CStreaming::ms_numVehiclesLoaded;
int32 CStreaming::ms_numPedsLoaded;
int32 CStreaming::ms_vehiclesLoaded[MAXVEHICLESLOADED];
int32 CStreaming::ms_lastVehicleDeleted;
bool CStreaming::ms_bIsPedFromPedGroupLoaded[NUMMODELSPERPEDGROUP];
CDirectory *CStreaming::ms_pExtraObjectsDir;
int32 CStreaming::ms_numPriorityRequests;
int32 CStreaming::ms_currentPedGrp;
int32 CStreaming::ms_currentPedLoading;
int32 CStreaming::ms_lastCullZone;
uint16 CStreaming::ms_loadedGangs;
uint16 CStreaming::ms_loadedGangCars;
int32 CStreaming::ms_imageOffsets[NUMCDIMAGES];
int32 CStreaming::ms_lastImageRead;
int32 CStreaming::ms_imageSize;
size_t CStreaming::ms_memoryAvailable;

int32 desiredNumVehiclesLoaded = 12;

CEntity *pIslandLODmainlandEntity;
CEntity *pIslandLODbeachEntity;
int32 islandLODmainland;
int32 islandLODbeach;

#ifndef MASTER
bool gbPrintStats;
bool gbPrintVehiclesInMemory;  // TODO
bool gbPrintStreamingBuffer; // TODO
#endif

bool
CStreamingInfo::GetCdPosnAndSize(uint32 &posn, uint32 &size)
{
	if(m_size == 0)
		return false;
	posn = m_position;
	size = m_size;
	return true;
}

void
CStreamingInfo::SetCdPosnAndSize(uint32 posn, uint32 size)
{
	m_position = posn;
	m_size = size;
}

void
CStreamingInfo::AddToList(CStreamingInfo *link)
{
	// Insert this after link
	m_next = link->m_next;
	m_prev = link;
	link->m_next = this;
	m_next->m_prev = this;
}

void
CStreamingInfo::RemoveFromList(void)
{
	m_next->m_prev = m_prev;
	m_prev->m_next = m_next;
	m_next = nil;
	m_prev = nil;
}

void
CStreaming::Init2(void)
{
	int i;
		printf("[GC-INIT2] Entered Init2");
#ifdef WII
	WiiStreamResetState();
#endif
#if defined(WII) && WII_STREAM_MEMORY_DIAGNOSTICS
	WiiStreamDiagReset();
#endif

	for(i = 0; i < NUMSTREAMINFO; i++){
		ms_aInfoForModel[i].m_loadState = STREAMSTATE_NOTLOADED;
		ms_aInfoForModel[i].m_next = nil;
		ms_aInfoForModel[i].m_prev = nil;
		ms_aInfoForModel[i].m_nextID = -1;
		ms_aInfoForModel[i].m_size = 0;
		ms_aInfoForModel[i].m_position = 0;
	}

	ms_channelError = -1;

	// init lists

	ms_startLoadedList.m_next = &ms_endLoadedList;
	ms_startLoadedList.m_prev = nil;
	ms_endLoadedList.m_prev = &ms_startLoadedList;
	ms_endLoadedList.m_next = nil;

	ms_startRequestedList.m_next = &ms_endRequestedList;
	ms_startRequestedList.m_prev = nil;
	ms_endRequestedList.m_prev = &ms_startRequestedList;
	ms_endRequestedList.m_next = nil;

	// init misc

	ms_oldSectorX = 0;
	ms_oldSectorY = 0;
	ms_streamingBufferSize = 0;
	ms_disableStreaming = false;
	ms_memoryUsed = 0;
	ms_bLoadingBigModel = false;

	// init channels

	ms_channel[0].state = CHANNELSTATE_IDLE;
	ms_channel[1].state = CHANNELSTATE_IDLE;
	for(i = 0; i < 4; i++){
		ms_channel[0].streamIds[i] = -1;
		ms_channel[0].offsets[i] = -1;
		ms_channel[1].streamIds[i] = -1;
		ms_channel[1].offsets[i] = -1;
	}

	// init stream info, mark things that are already loaded

	for(i = 0; i < MODELINFOSIZE; i++){
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(i);
		if(mi && mi->GetRwObject()){
			ms_aInfoForModel[i].m_loadState = STREAMSTATE_LOADED;
			ms_aInfoForModel[i].m_flags = STREAMFLAGS_DONT_REMOVE;
			if(mi->IsSimple())
				((CSimpleModelInfo*)mi)->m_alpha = 255;
		}
	}

	for(i = 0; i < TXDSTORESIZE; i++)
		if(CTxdStore::GetSlot(i) && CTxdStore::GetSlot(i)->texDict)
			ms_aInfoForModel[i + STREAM_OFFSET_TXD].m_loadState = STREAMSTATE_LOADED;

#ifdef WII
	WiiStreamRebuildResidentCosts();
#endif


	for(i = 0; i < MAXVEHICLESLOADED; i++)
		ms_vehiclesLoaded[i] = -1;
	ms_numVehiclesLoaded = 0;
	ms_numPedsLoaded = 8;

	for(i = 0; i < ARRAY_SIZE(ms_bIsPedFromPedGroupLoaded); i++)
		ms_bIsPedFromPedGroupLoaded[i] = false;

	ms_pExtraObjectsDir = new CDirectory(EXTRADIRSIZE);
	ms_numPriorityRequests = 0;
	ms_currentPedGrp = -1;
	ms_lastCullZone = -1;		// unused because RemoveModelsNotVisibleFromCullzone is gone
	ms_loadedGangs = 0;
	ms_currentPedLoading = NUMMODELSPERPEDGROUP;	// unused, whatever it is

	printf("[GC-INIT2] About to call LoadCdDirectory...");
	LoadCdDirectory();

	// allocate streaming buffers
	if(ms_streamingBufferSize & 1) ms_streamingBufferSize++;
#ifndef ONE_THREAD_PER_CHANNEL
	ms_pStreamingBuffer[0] = (int8*)RwMallocAlign(ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE, CDSTREAM_SECTOR_SIZE);
	ms_streamingBufferSize /= 2;
	ms_pStreamingBuffer[1] = ms_pStreamingBuffer[0] + ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE;
#else
	ms_pStreamingBuffer[0] = (int8*)RwMallocAlign(ms_streamingBufferSize*2*CDSTREAM_SECTOR_SIZE, CDSTREAM_SECTOR_SIZE);
	ms_streamingBufferSize /= 2;
	ms_pStreamingBuffer[1] = ms_pStreamingBuffer[0] + ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE;
	ms_pStreamingBuffer[2] = ms_pStreamingBuffer[1] + ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE;
	ms_pStreamingBuffer[3] = ms_pStreamingBuffer[2] + ms_streamingBufferSize*CDSTREAM_SECTOR_SIZE;
#endif
	debug("Streaming buffer size is %d sectors", ms_streamingBufferSize);

	// The PC floor is larger than Wii's entire practical streaming headroom.
#ifdef WII
	ms_memoryAvailable = WII_STREAMING_MEMORY_BUDGET_LOW;
	desiredNumVehiclesLoaded = 16;
	SYS_Report("[WII-STREAM] cache budget=%uKB..%uKB archive=%uKB..%uKB vehicles=%d victim=%s\n",
	           (unsigned)(ms_memoryAvailable / 1024u),
	           (unsigned)(WII_STREAMING_MEMORY_BUDGET_HIGH / 1024u),
	           (unsigned)(WII_STREAMING_ARCHIVE_CEILING_BASE / 1024u),
#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING
	           (unsigned)(WII_STREAMING_ARCHIVE_CEILING_HIGH / 1024u),
#else
	           (unsigned)(WII_STREAMING_ARCHIVE_CEILING_BASE / 1024u),
#endif
	           desiredNumVehiclesLoaded,
#if WII_STREAM_ARCHIVE_GLOBAL_LRU
	           "global-lru");
#else
	           "type-priority");
#endif
#elif defined GTA_PC
#define MB (1024*1024)
#ifdef FIX_BUGS
	// do what gta3 does
	extern size_t _dwMemAvailPhys;
	ms_memoryAvailable = (_dwMemAvailPhys - 10*MB)/2;
	if(ms_memoryAvailable < 65*MB)
		ms_memoryAvailable = 65*MB;
	desiredNumVehiclesLoaded = (int32)((ms_memoryAvailable / MB - 65) / 3 + 12);
	if(desiredNumVehiclesLoaded > MAXVEHICLESLOADED)
		desiredNumVehiclesLoaded = MAXVEHICLESLOADED;
#else
	ms_memoryAvailable = 65 * MB;
	desiredNumVehiclesLoaded = 25;
	debug("Memory allocated to Streaming is %zuMB", ms_memoryAvailable/MB); // original modifier was %d
#endif
#undef MB
#endif

	// find island LODs

	pIslandLODmainlandEntity = nil;
	pIslandLODbeachEntity = nil;
	islandLODmainland = -1;
	islandLODbeach = -1;
	CModelInfo::GetModelInfo("IslandLODmainland", &islandLODmainland);
	CModelInfo::GetModelInfo("IslandLODbeach", &islandLODbeach);

#ifndef MASTER
	VarConsole.Add("Streaming Debug", &gbPrintStats, true);
	VarConsole.Add("Streaming Vehicle Debug", &gbPrintVehiclesInMemory, true);
	VarConsole.Add("Printf Streaming Buffer contents", &gbPrintStreamingBuffer, true);
#endif
}

void
CStreaming::Init(void)
{
	printf("[GC-INIT2] CStreaming::Init called");
#ifdef USE_TXD_CDIMAGE
	if(!CanVideoCardDoDXT()){
		int txdHandle = CFileMgr::OpenFile("MODELS\\TXD.IMG", "r");
		if (txdHandle)
			CFileMgr::CloseFile(txdHandle);
		if (!CheckVideoCardCaps() && txdHandle) {
			CdStreamAddImage("MODELS\\TXD.IMG");
			CStreaming::Init2();
		} else {
			CStreaming::Init2();
			if (CreateTxdImageForVideoCard()) {
				CStreaming::Shutdown();
				CdStreamAddImage("MODELS\\TXD.IMG");
				CStreaming::Init2();
			}
		}
	} else
		CStreaming::Init2();
#else
	CStreaming::Init2();
#endif
}

void
CStreaming::ReInit(void)
{
	int i;
#ifdef WII
	WiiIslandReleaseTemporaryPins(false, false);
	WiiIslandTransitionReset();
#endif
	CStreaming::FlushRequestList();
	CStreaming::DeleteAllRwObjects();
	CStreaming::RemoveAllUnusedModels();
	for(i = 0; i < MODELINFOSIZE; i++)
		if(CModelInfo::GetModelInfo(i) && ms_aInfoForModel[i].m_flags & STREAMFLAGS_SCRIPTOWNED)
			SetMissionDoesntRequireModel(i);
#ifdef WII
	WiiStreamResetState();
#endif
#if defined(WII) && WII_STREAM_MEMORY_DIAGNOSTICS
	WiiStreamDiagReset();
#endif
	CStreaming::ms_disableStreaming = false;
}

void
CStreaming::Shutdown(void)
{
	RwFreeAlign(ms_pStreamingBuffer[0]);
	ms_streamingBufferSize = 0;
	if(ms_pExtraObjectsDir) {
		delete ms_pExtraObjectsDir;
#ifdef FIX_BUGS
		ms_pExtraObjectsDir = nil;
#endif
	}
}

#ifndef MASTER
uint64 timeProcessingTXD;
uint64 timeProcessingDFF;
#endif

void
CStreaming::Update(void)
{
	CStreamingInfo *si, *prev;
	bool requestedSubway = false;
#ifdef WII
	uint64 wiiUpdateStartTicks = gettime();
	uint64 wiiAfterPrepTicks = wiiUpdateStartTicks;
	uint64 wiiAfterLoadTicks = wiiUpdateStartTicks;
#endif

#if defined(WII) && WII_STREAM_MEMORY_DIAGNOSTICS
	WiiStreamDiagPump();
#endif

#ifdef WII
	if(!IsIslandTransitionActive())
		RemoveIslandsNotUsed(CGame::currLevel);
#endif

#ifndef MASTER
	timeProcessingTXD = 0;
	timeProcessingDFF = 0;
#endif

	UpdateMemoryUsed();

	if(ms_channelError != -1){
	#ifdef WII
		if(IsIslandTransitionBlocking())
			LoadBigBuildingsWhenNeeded();
		if(ms_channelError == -1)
			return;
	#endif
		RetryLoadFile(ms_channelError);
		return;
	}

	#ifdef WII
	if(IsIslandTransitionBlocking()){
		LoadBigBuildingsWhenNeeded();
	#if WII_ISLAND_READ_LIVENESS
		if(gWiiIslandPauseOwned && CTimer::GetIsCodePaused() &&
		   !CTimer::GetIsUserPaused()){
			if(!ms_disableStreaming && TheCamera.GetPosition().z < 55.0f)
				AddModelsToRequestList(TheCamera.GetPosition(), 0);
			DeleteFarAwayRwObjects(TheCamera.GetPosition());
		}
	#endif
		LoadRequestedModels();
		wiiAfterPrepTicks = gettime();
		wiiAfterLoadTicks = wiiAfterPrepTicks;
		goto wii_stream_cleanup;
	}
	#endif
	if(CTimer::GetIsPaused())
		return;

	LoadBigBuildingsWhenNeeded();
	#ifdef WII
	if(IsIslandTransitionBlocking()){
	#if WII_ISLAND_READ_LIVENESS
		if(gWiiIslandPauseOwned && CTimer::GetIsCodePaused() &&
		   !CTimer::GetIsUserPaused()){
			if(!ms_disableStreaming && TheCamera.GetPosition().z < 55.0f)
				AddModelsToRequestList(TheCamera.GetPosition(), 0);
			DeleteFarAwayRwObjects(TheCamera.GetPosition());
		}
	#endif
		LoadRequestedModels();
		wiiAfterPrepTicks = gettime();
		wiiAfterLoadTicks = wiiAfterPrepTicks;
		goto wii_stream_cleanup;
	}
	#endif
	if(!ms_disableStreaming && TheCamera.GetPosition().z < 55.0f){
		AddModelsToRequestList(TheCamera.GetPosition(), 0);
	}

	DeleteFarAwayRwObjects(TheCamera.GetPosition());
#ifdef WII
	wiiAfterPrepTicks = gettime();
	if(!ms_disableStreaming){
		WiiMemoryPoolSnapshot headroomSnapshot;
		WiiMemoryGetPoolSnapshot(&headroomSnapshot);
		uint32 headroomPressure =
			WiiStreamGetStreamingPressureForSnapshotWithAdmission(
				headroomSnapshot);
		WiiStreamRetireOneHeadroomResource(
			headroomSnapshot, headroomPressure, nil);
	}
#endif

	if(!ms_disableStreaming &&
	   !CCutsceneMgr::IsCutsceneProcessing() &&
	   ms_numModelsRequested < 5 &&
	   !CRenderer::m_loadingPriority &&
	   CGame::currArea == AREA_MAIN_MAP &&
	   !CReplay::IsPlayingBack()){
		StreamVehiclesAndPeds();
		StreamZoneModels(FindPlayerCoors());
	}

	if(CWorld::Players[0].m_pRemoteVehicle){
		CColStore::AddCollisionNeededAtPosn(FindPlayerCoors());
		CColStore::LoadCollision(CWorld::Players[0].m_pRemoteVehicle->GetPosition());
		LoadRequestedModels();
		CColStore::EnsureCollisionIsInMemory(CWorld::Players[0].m_pRemoteVehicle->GetPosition());
	}else{
		CColStore::LoadCollision(FindPlayerCoors());
		LoadRequestedModels();
		CColStore::EnsureCollisionIsInMemory(FindPlayerCoors());
	}
#ifdef WII
	wiiAfterLoadTicks = gettime();
wii_stream_cleanup:
#endif

	// TODO: PrintRequestList
	//if (CPad::GetPad(1)->GetLeftShoulder2JustDown() && CPad::GetPad(1)->GetRightShoulder1() && CPad::GetPad(1)->GetRightShoulder2())
	//	PrintRequestList();

	for(si = ms_endRequestedList.m_prev; si != &ms_startRequestedList; si = prev){
		prev = si->m_prev;
		if((si->m_flags & (STREAMFLAGS_KEEP_IN_MEMORY|STREAMFLAGS_PRIORITY)) == 0)
			RemoveModel(si - ms_aInfoForModel);
	}
#ifdef WII
	uint64 wiiUpdateEndTicks = gettime();
	uint32 wiiUpdateMs = (uint32)ticks_to_millisecs(
		wiiUpdateEndTicks - wiiUpdateStartTicks);
	if(wiiUpdateMs >= 50u)
		SYS_Report("[WII-STREAM-STAGE] total=%ums prep=%ums load=%ums cleanup=%ums pending=%d priority=%d pressure=0x%X\n",
		           (unsigned)wiiUpdateMs,
		           (unsigned)ticks_to_millisecs(wiiAfterPrepTicks - wiiUpdateStartTicks),
		           (unsigned)ticks_to_millisecs(wiiAfterLoadTicks - wiiAfterPrepTicks),
		           (unsigned)ticks_to_millisecs(wiiUpdateEndTicks - wiiAfterLoadTicks),
		           ms_numModelsRequested, ms_numPriorityRequests,
		           (unsigned)WiiMemoryGetStreamingPressure());
#endif
}

void
CStreaming::LoadCdDirectory(void)
{
	char dirname[132];
	int i;
	printf("[GC-DIR] LoadCdDirectory: %d images\n", CdStreamGetNumImages());

#ifdef GTA_PC
	ms_imageOffsets[0] = 0;
	ms_imageOffsets[1] = -1;
	ms_imageOffsets[2] = -1;
	ms_imageOffsets[3] = -1;
	ms_imageOffsets[4] = -1;
	ms_imageOffsets[5] = -1;
	ms_imageSize = GetGTA3ImgSize();
	// PS2 uses CFileMgr::GetCdFile on all IMG files to fill the array
#endif

	i = CdStreamGetNumImages();
	while(i-- >= 1){
		strcpy(dirname, CdStreamGetImageName(i));
		strncpy(strrchr(dirname, '.') + 1, "DIR", 3);
		LoadCdDirectory(dirname, i);
	}

	ms_lastImageRead = 0;
	ms_imageSize /= CDSTREAM_SECTOR_SIZE;
}

void
CStreaming::LoadCdDirectory(const char *dirname, int n)
{
	int fd, lastID, imgSelector;
	int modelId;
	CDirectory::DirectoryInfo direntry;
	char *dot;

	lastID = -1;
	printf("[GC-DIR] Loading %s (image %d)...\n", dirname, n);
	fd = CFileMgr::OpenFile(dirname, "rb");
	assert(fd > 0);

	imgSelector = n<<24;
	assert(sizeof(direntry) == 32);
	int entryCount = 0;
	while(CFileMgr::Read(fd, (char*)&direntry, sizeof(direntry))){
		entryCount++;
#if defined(GAMECUBE) || defined(RW_BIG_ENDIAN)
		direntry.offset = ((direntry.offset & 0xFF000000) >> 24) |
		                  ((direntry.offset & 0x00FF0000) >> 8) |
		                  ((direntry.offset & 0x0000FF00) << 8) |
		                  ((direntry.offset & 0x000000FF) << 24);
		direntry.size   = ((direntry.size & 0xFF000000) >> 24) |
		                  ((direntry.size & 0x00FF0000) >> 8) |
		                  ((direntry.size & 0x0000FF00) << 8) |
		                  ((direntry.size & 0x000000FF) << 24);
#endif
		bool bAddToStreaming = false;

		if(direntry.size > (uint32)ms_streamingBufferSize)
			ms_streamingBufferSize = direntry.size;
		direntry.name[23] = '\0';
		dot = strchr(direntry.name, '.');
		if(dot == nil || dot-direntry.name > 20){
			debug("%s is too long\n", direntry.name);
			lastID = -1;
			continue;
		}

		*dot = '\0';

		if(strncasecmp(dot+1, "DFF", 3) == 0){
			if(CModelInfo::GetModelInfo(direntry.name, &modelId)){
				bAddToStreaming = true;
			}else{
#ifdef FIX_BUGS
				// remember which cdimage this came from
				ms_pExtraObjectsDir->AddItem(direntry, n);
#else
				ms_pExtraObjectsDir->AddItem(direntry);
#endif
				lastID = -1;
			}
		}else if(strncasecmp(dot+1, "TXD", 3) == 0){
			modelId = CTxdStore::FindTxdSlot(direntry.name);
			if(modelId == -1)
				modelId = CTxdStore::AddTxdSlot(direntry.name);
			modelId += STREAM_OFFSET_TXD;
			bAddToStreaming = true;
		}else if(strncasecmp(dot+1, "COL", 3) == 0){
			modelId = CColStore::FindColSlot(direntry.name);
			if(modelId == -1)
				modelId = CColStore::AddColSlot(direntry.name);
			modelId += STREAM_OFFSET_COL;
			bAddToStreaming = true;
		}else if(strncasecmp(dot+1, "IFP", 3) == 0){
			modelId = CAnimManager::RegisterAnimBlock(direntry.name);
			modelId += STREAM_OFFSET_ANIM;
			bAddToStreaming = true;
		}else{
			*dot = '.';
			lastID = -1;
		}

		if(bAddToStreaming){
			if(ms_aInfoForModel[modelId].GetCdSize()){
				debug("%s.%s appears more than once in %s\n", direntry.name, dot+1, dirname);
				lastID = -1;
			}else{
				direntry.offset |= imgSelector;
				ms_aInfoForModel[modelId].SetCdPosnAndSize(direntry.offset, direntry.size);
				if(lastID != -1)
					ms_aInfoForModel[lastID].m_nextID = modelId;
				lastID = modelId;
			}
		}
	}

	printf("[GC-DIR] Loaded %d entries from %s\n", entryCount, dirname);
	CFileMgr::CloseFile(fd);
}

static char*
GetObjectName(int streamId)
{
	static char objname[32];
	if(streamId < STREAM_OFFSET_TXD)
		sprintf(objname, "%s.dff", CModelInfo::GetModelInfo(streamId)->GetModelName());
	else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL)
		sprintf(objname, "%s.txd", CTxdStore::GetTxdName(streamId-STREAM_OFFSET_TXD));
	else if(streamId >= STREAM_OFFSET_COL && streamId < STREAM_OFFSET_ANIM)
		sprintf(objname, "%s.col", CColStore::GetColName(streamId-STREAM_OFFSET_COL));
	else{
		assert(streamId < NUMSTREAMINFO);
		sprintf(objname, "%s.ifp", CAnimManager::GetAnimationBlock(streamId-STREAM_OFFSET_ANIM)->name);
	}
	return objname;
}

#ifdef USE_CUSTOM_ALLOCATOR
RpAtomic*
RegisterAtomicMemPtrsCB(RpAtomic *atomic, void *data)
{
	// empty because we expect models to be pre-instanced
	return atomic;
}
#endif

bool
CStreaming::ConvertBufferToObject(int8 *buf, int32 streamId)
{
	RwMemory mem;
	RwStream *stream;
	int cdsize;
	uint32 startTime, endTime, timeDiff;
	CBaseModelInfo *mi;
	bool success;

#ifdef WII
	WiiStreamPoolAttributionScope poolAttribution(streamId,
		WiiStreamPoolAttributionScope::REPLACE_RESIDENT_COST);
#endif

	startTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();

	cdsize = ms_aInfoForModel[streamId].GetCdSize();
	mem.start = (uint8*)buf;
	mem.length = cdsize * CDSTREAM_SECTOR_SIZE;
	stream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &mem);

	if (streamId < STREAM_OFFSET_TXD)
		((void)0); // [GC-DEBUG-DISABLED]
	else if (streamId < STREAM_OFFSET_COL)
		((void)0); // [GC-DEBUG-DISABLED]

	if(streamId < STREAM_OFFSET_TXD){
                // Model
                mi = CModelInfo::GetModelInfo(streamId);
		#ifdef WII
		WiiTextureStreamContextScope textureStreamContext(streamId, mi);
		#endif

		// Txd and anim have to be loaded
		int animId = mi->GetAnimFileIndex();
#ifdef FIX_BUGS
		if(!HasTxdLoaded(mi->GetTxdSlot()) ||
#else
		// texDict will exist even if only first part has loaded
		if(CTxdStore::GetSlot(mi->GetTxdSlot())->texDict == nil ||
#endif
		   animId != -1 && !CAnimManager::GetAnimationBlock(animId)->isLoaded){
  #if REAL_GAMECUBE
			RemoveModel(streamId);
                        // Real GC: skip ReRequest - anim blocks in separate IFP,
                        // ReRequest would make LoadAllRequestedModels loop forever
  #else
#ifdef WII
                        WiiStreamRequeueDispatched(streamId, "dependency");
#else
			RemoveModel(streamId);
                        ReRequestModel(streamId);
#endif
  #endif
                        RwStreamClose(stream, &mem);
			return false;
		}

		// Set Txd and anims to use
		CTxdStore::AddRef(mi->GetTxdSlot());
#if GTA_VERSION > GTAVC_PS2
		if(animId != -1)
			CAnimManager::AddAnimBlockRef(animId);
#endif

		PUSH_MEMID(MEMID_STREAM_MODELS);
		CTxdStore::SetCurrentTxd(mi->GetTxdSlot());
		if(mi->IsSimple()){
                        ((void)0); // [GC-DEBUG-DISABLED]
			success = CFileLoader::LoadAtomicFile(stream, streamId);
                        ((void)0); // [GC-DEBUG-DISABLED]
			// TODO(MIAMI)? complain if file is not pre-instanced. we hardly are interested in that
		} else if (mi->GetModelType() == MITYPE_VEHICLE) {
			// load vehicles in two parts
                        ((void)0); // [GC-DEBUG-DISABLED]
			CModelInfo::GetModelInfo(streamId)->AddRef();
			success = CFileLoader::StartLoadClumpFile(stream, streamId);
                        ((void)0); // [GC-DEBUG-DISABLED]
			if(success)
				ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_STARTED;
		}else{
                        ((void)0); // [GC-DEBUG-DISABLED]
			success = CFileLoader::LoadClumpFile(stream, streamId);
                        ((void)0); // [GC-DEBUG-DISABLED]
#ifdef USE_CUSTOM_ALLOCATOR
			if(success)
				RpClumpForAllAtomics((RpClump*)mi->GetRwObject(), RegisterAtomicMemPtrsCB, nil);
#endif
		}
		POP_MEMID();
		UpdateMemoryUsed();

		// Txd and anims no longer needed unless we only read part of the file
		if(ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_STARTED){
			CTxdStore::RemoveRefWithoutDelete(mi->GetTxdSlot());
#if GTA_VERSION > GTAVC_PS2
			if(animId != -1)
				CAnimManager::RemoveAnimBlockRefWithoutDelete(animId);
#endif
		}

		if(!success){
                                ((void)0); // [GC-DEBUG-DISABLED]
#ifdef WII
                                WiiStreamRequeueDispatched(streamId, "model-convert");
#else
			                        RemoveModel(streamId);
                                ReRequestModel(streamId);
#endif
			RwStreamClose(stream, &mem);
			return false;
		}
	}else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
		// Txd
		if((ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_KEEP_IN_MEMORY) == 0 &&
		   !IsTxdUsedByRequestedModels(streamId - STREAM_OFFSET_TXD)){
			RemoveModel(streamId);
#ifdef WII
			WiiStreamClearDispatchedFlags(streamId);
#endif
			RwStreamClose(stream, &mem);
			return false;
		}

		PUSH_MEMID(MEMID_STREAM_TEXUTRES);
		if(ms_bLoadingBigModel || cdsize > 200){
			success = CTxdStore::StartLoadTxd(streamId - STREAM_OFFSET_TXD, stream);
			if(success)
				ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_STARTED;
		}else
		        success = CTxdStore::LoadTxd(streamId - STREAM_OFFSET_TXD, stream);
		POP_MEMID();
		UpdateMemoryUsed();

		if(!success){
			debug("Failed to load %s.txd\n", CTxdStore::GetTxdName(streamId - STREAM_OFFSET_TXD));
#ifdef WII
			WiiStreamRequeueDispatched(streamId, "txd-convert");
#else
			RemoveModel(streamId);
			ReRequestModel(streamId);
#endif
			RwStreamClose(stream, &mem);
			return false;
		}
	}else if(streamId >= STREAM_OFFSET_COL && streamId < STREAM_OFFSET_ANIM){
		PUSH_MEMID(MEMID_STREAM_COLLISION);
		bool success = CColStore::LoadCol(streamId-STREAM_OFFSET_COL, mem.start, mem.length);
		POP_MEMID();
		if(!success){
			debug("Failed to load %s.col\n", CColStore::GetColName(streamId - STREAM_OFFSET_COL));
#ifdef WII
			WiiStreamRequeueDispatched(streamId, "col-convert");
#else
			RemoveModel(streamId);
			ReRequestModel(streamId);
#endif
			RwStreamClose(stream, &mem);
			return false;
		}
	}else if(streamId >= STREAM_OFFSET_ANIM){
		assert(streamId < NUMSTREAMINFO);
		if((ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_KEEP_IN_MEMORY) == 0 &&
		   !AreAnimsUsedByRequestedModels(streamId - STREAM_OFFSET_ANIM)){
			RemoveModel(streamId);
#ifdef WII
			WiiStreamClearDispatchedFlags(streamId);
#endif
			RwStreamClose(stream, &mem);
			return false;
		}
		PUSH_MEMID(MEMID_STREAM_ANIMATION);
		CAnimManager::LoadAnimFile(stream, true, nil);
		CAnimManager::CreateAnimAssocGroups();
		POP_MEMID();
	}

	RwStreamClose(stream, &mem);

	if(streamId < STREAM_OFFSET_TXD){
		// Model
		// Vehicles and Peds not in loaded list
		if (mi->GetModelType() != MITYPE_VEHICLE && mi->GetModelType() != MITYPE_PED) {
			CSimpleModelInfo *smi = (CSimpleModelInfo*)mi;

			// Set fading for some objects
			if(mi->IsSimple() && !smi->m_isBigBuilding){
				if(ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_NOFADE)
					smi->m_alpha = 255;
				else
					smi->m_alpha = 0;
			}

			if(CanRemoveModel(streamId))
				ms_aInfoForModel[streamId].AddToList(&ms_startLoadedList);
		}
	}else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL ||
	         streamId >= STREAM_OFFSET_ANIM){
		assert(streamId < NUMSTREAMINFO);
		// Txd and anims
		if(CanRemoveModel(streamId))
			ms_aInfoForModel[streamId].AddToList(&ms_startLoadedList);
	}

	// Mark objects as loaded
	if(ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_STARTED){
		ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_LOADED;
#ifndef USE_CUSTOM_ALLOCATOR
		ms_memoryUsed += ms_aInfoForModel[streamId].GetCdSize() * CDSTREAM_SECTOR_SIZE;
#endif
	#ifdef WII
		WiiStreamMarkWorldLoaded(streamId);
	#if WII_STREAM_MEMORY_DIAGNOSTICS
		WiiStreamDiagOnLoaded(streamId);
	#endif
		WiiStreamClearDispatchedFlags(streamId);
	#endif
	}

	endTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();
	timeDiff = endTime - startTime;
	if(timeDiff > 5)
		debug("%s took %d ms\n", GetObjectName(streamId), timeDiff);
	#ifdef WII
	poolAttribution.Commit();
	#endif
    ((void)0); // [GC-DEBUG-DISABLED]
	return true;
}

bool
CStreaming::FinishLoadingLargeFile(int8 *buf, int32 streamId)
{
	RwMemory mem;
	RwStream *stream;
	uint32 startTime, endTime, timeDiff;
	CBaseModelInfo *mi;
	bool success;

	startTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();

	if(ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_STARTED){
		if(streamId < STREAM_OFFSET_TXD)
			CModelInfo::GetModelInfo(streamId)->RemoveRef();
	#ifdef WII
		WiiStreamClearDispatchedFlags(streamId);
	#endif
		return false;
	}

#ifdef WII
	WiiStreamPoolAttributionScope poolAttribution(streamId,
		WiiStreamPoolAttributionScope::ADD_RESIDENT_COST);
#endif

	mem.start = (uint8*)buf;
	mem.length = ms_aInfoForModel[streamId].GetCdSize() * CDSTREAM_SECTOR_SIZE;
	stream = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &mem);

	if(streamId < STREAM_OFFSET_TXD){
		// Model
		mi = CModelInfo::GetModelInfo(streamId);
		#ifdef WII
		WiiTextureStreamContextScope textureStreamContext(streamId, mi);
		#endif
		PUSH_MEMID(MEMID_STREAM_MODELS);
		CTxdStore::SetCurrentTxd(mi->GetTxdSlot());
		success = CFileLoader::FinishLoadClumpFile(stream, streamId);
		if(success){
#ifdef USE_CUSTOM_ALLOCATOR
			RpClumpForAllAtomics((RpClump*)mi->GetRwObject(), RegisterAtomicMemPtrsCB, nil);
#endif
			success = AddToLoadedVehiclesList(streamId);
		}
		POP_MEMID();
		mi->RemoveRef();
		CTxdStore::RemoveRefWithoutDelete(mi->GetTxdSlot());
#if GTA_VERSION > GTAVC_PS2
		if(mi->GetAnimFileIndex() != -1)
			CAnimManager::RemoveAnimBlockRefWithoutDelete(mi->GetAnimFileIndex());
#endif
	}else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
		// Txd
		CTxdStore::AddRef(streamId - STREAM_OFFSET_TXD);
		PUSH_MEMID(MEMID_STREAM_TEXUTRES);
		((void)0); // [GC-DEBUG-DISABLED]
			success = CTxdStore::FinishLoadTxd(streamId - STREAM_OFFSET_TXD, stream);
			((void)0); // [GC-DEBUG-DISABLED]
		POP_MEMID();
		CTxdStore::RemoveRefWithoutDelete(streamId - STREAM_OFFSET_TXD);
	}else{
		assert(0 && "invalid streamId");
	}

	RwStreamClose(stream, &mem);

	ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_LOADED;
#ifndef USE_CUSTOM_ALLOCATOR
	ms_memoryUsed += ms_aInfoForModel[streamId].GetCdSize() * CDSTREAM_SECTOR_SIZE;
#endif

	if(!success){
#ifdef WII
		WiiStreamRequeueDispatched(streamId, "large-convert");
#else
		RemoveModel(streamId);
		ReRequestModel(streamId);
#endif
		UpdateMemoryUsed();
		return false;
	}
	#ifdef WII
	WiiStreamMarkWorldLoaded(streamId);
	#if WII_STREAM_MEMORY_DIAGNOSTICS
	WiiStreamDiagOnLoaded(streamId);
	#endif
	WiiStreamClearDispatchedFlags(streamId);
	#endif

	UpdateMemoryUsed();

	endTime = CTimer::GetCurrentTimeInCycles() / CTimer::GetCyclesPerMillisecond();
	timeDiff = endTime - startTime;
	if(timeDiff > 5)
		debug("%s took %d ms\n", GetObjectName(streamId), timeDiff);
	#ifdef WII
	poolAttribution.Commit();
	#endif

	return true;
}

void
CStreaming::RequestModel(int32 id, int32 flags)
{
	CSimpleModelInfo *mi;

#ifdef WII
	WiiIslandObserveExternalRequest(id, flags);
	WiiIslandTransitionCaptureRequest(id, flags);
	if(gWiiIslandCaptureRequests && gWiiIslandCaptureTemporaryPin)
		flags |= STREAMFLAGS_DONT_REMOVE;
	if(gWiiIslandCaptureRequests && gWiiIslandCaptureTemporaryPin &&
	   gWiiIslandCapturePriority)
		flags |= STREAMFLAGS_PRIORITY;
#if WII_STREAM_MEMORY_DIAGNOSTICS && WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	uint32 diagRequestNow = WiiStreamNowMs();
	gWiiStreamDiagLastRequestMs[id] = diagRequestNow != 0 ? diagRequestNow : 1u;
	gWiiStreamDiagLastRequestClass[id] = gWiiStreamRequestClass[id];
#endif
#endif

	#ifdef WII
	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_NOTLOADED)
		WiiStreamMarkQueued(id);
	#endif
	#if defined(WII) && WII_STREAM_MEMORY_DIAGNOSTICS
	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_NOTLOADED)
		WiiStreamDiagOnRequest(id, (uint8)flags);
	#endif

	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_INQUEUE){
		// updgrade to priority
		if(flags & STREAMFLAGS_PRIORITY && !ms_aInfoForModel[id].IsPriority()){
			ms_numPriorityRequests++;
			ms_aInfoForModel[id].m_flags |= STREAMFLAGS_PRIORITY;
		}
	}else if(ms_aInfoForModel[id].m_loadState != STREAMSTATE_NOTLOADED){
		flags &= ~STREAMFLAGS_PRIORITY;
	}
	ms_aInfoForModel[id].m_flags |= flags;

	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED){
		// Already loaded, only check changed flags

		if(ms_aInfoForModel[id].m_flags & STREAMFLAGS_NOFADE && id < STREAM_OFFSET_TXD){
			mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(id);
			if(mi->IsSimple())
				mi->m_alpha = 255;
		}

		// reinsert into list
		if(ms_aInfoForModel[id].m_next){
			ms_aInfoForModel[id].RemoveFromList();
			if(CanRemoveModel(id))
				ms_aInfoForModel[id].AddToList(&ms_startLoadedList);
		}
	}else if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_NOTLOADED ||
	         ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED){	// how can this be true again?

		if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_NOTLOADED){
			if(id < STREAM_OFFSET_TXD){
				mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(id);
				RequestTxd(mi->GetTxdSlot(), flags);
				int anim = mi->GetAnimFileIndex();
				if(anim != -1)
					RequestAnim(anim, STREAMFLAGS_DEPENDENCY);
			}
			ms_aInfoForModel[id].AddToList(&ms_startRequestedList);
			ms_numModelsRequested++;
			if(flags & STREAMFLAGS_PRIORITY)
				ms_numPriorityRequests++;
		}

		ms_aInfoForModel[id].m_loadState = STREAMSTATE_INQUEUE;
		ms_aInfoForModel[id].m_flags = flags;
	}
}

void
CStreaming::RequestModelFromWorldScan(int32 id, int32 flags)
{
#ifdef WII
	if(gWiiIslandCaptureRequests)
		WiiStreamMarkWorldRequestBits(id);
	else
		WiiStreamMarkWorldRequest(id);
#endif
	RequestModel(id, flags);
}

#ifdef WII
void
CStreaming::RequestRadarTxd(int32 txd, int32 flags, int32 tileX, int32 tileY)
{
	int32 streamId = txd + STREAM_OFFSET_TXD;
	if(streamId >= 0 && streamId < NUMSTREAMINFO)
		WiiStreamPromoteRequestClassResource(streamId,
		                                     WII_STREAM_REQUEST_RADAR);
	if(gWiiIslandCaptureRequests && txd >= 0 && txd < TXDSTORESIZE)
		gWiiIslandProtectedRadarTxds[txd] = 1;
#if WII_STREAM_MEMORY_DIAGNOSTICS
	if(txd >= 0 && txd < TXDSTORESIZE &&
	   tileX >= 0 && tileX < 8 && tileY >= 0 && tileY < 8)
		gWiiStreamDiagRadarTile[txd] = (uint8)((tileY << 4) | tileX);
#endif
	RequestTxd(txd, flags);
}
#endif

#define BIGBUILDINGFLAGS STREAMFLAGS_DONT_REMOVE

void
CStreaming::RequestBigBuildings(eLevelName level)
{
	int i, n;
	CBuilding *b;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		b = CPools::GetBuildingPool()->GetSlot(i);
		if(b && b->bIsBIGBuilding
#ifdef NO_ISLAND_LOADING
		   && (((FrontEndMenuManager.m_PrefsIslandLoading != CMenuManager::ISLAND_LOADING_LOW) && (b != pIslandLODmainlandEntity) &&
		        (b != pIslandLODbeachEntity)) ||
		       (b->m_level == level))
#else
		   && b->m_level == level
#endif
			)
			if(!b->bStreamBIGBuilding)
				RequestModelFromWorldScan(b->GetModelIndex(), BIGBUILDINGFLAGS);
	}
	RequestIslands(level);
}

void
CStreaming::RequestBigBuildings(eLevelName level, const CVector &pos)
{
	int i, n;
	CBuilding *b;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		b = CPools::GetBuildingPool()->GetSlot(i);
		if(b && b->bIsBIGBuilding
#ifdef NO_ISLAND_LOADING
		    && (((FrontEndMenuManager.m_PrefsIslandLoading != CMenuManager::ISLAND_LOADING_LOW) && (b != pIslandLODmainlandEntity) && (b != pIslandLODbeachEntity)
				) || (b->m_level == level))
#else
		   && b->m_level == level
#endif
		)
			if(b->bStreamBIGBuilding){
				if(CRenderer::ShouldModelBeStreamed(b, pos))
					RequestModelFromWorldScan(b->GetModelIndex(), 0);
			}else
				RequestModelFromWorldScan(b->GetModelIndex(), BIGBUILDINGFLAGS);
	}
	RequestIslands(level);
}

void
CStreaming::InstanceBigBuildings(eLevelName level, const CVector &pos)
{
	int i, n;
	CBuilding *b;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		b = CPools::GetBuildingPool()->GetSlot(i);
		if(b && b->bIsBIGBuilding && b->m_level == level &&
		   b->bStreamBIGBuilding && b->m_rwObject == nil)
			if(CRenderer::ShouldModelBeStreamed(b, pos))
				b->CreateRwObject();
	}
}

void
CStreaming::InstanceLoadedModelsInSectorList(CPtrList &list)
{
	CPtrNode *node;
	CEntity *e;
	for(node = list.first; node; node = node->next) {
		e = (CEntity *)node->item;
		if(IsAreaVisible(e->m_area) && e->m_rwObject == nil)
			e->CreateRwObject();
	}
}

void
CStreaming::InstanceLoadedModels(const CVector &pos)
{
	int minX = CWorld::GetSectorIndexX(pos.x - 80.0f);
	if(minX <= 0) minX = 0;

	int minY = CWorld::GetSectorIndexY(pos.y - 80.0f);
	if(minY <= 0) minY = 0;

	int maxX = CWorld::GetSectorIndexX(pos.x + 80.0f);
	if(maxX >= NUMSECTORS_X) maxX = NUMSECTORS_X - 1;

	int maxY = CWorld::GetSectorIndexY(pos.y + 80.0f);
	if(maxY >= NUMSECTORS_Y) maxY = NUMSECTORS_Y - 1;

	int x, y;
	for(y = minY; y <= maxY; y++){
		for(x = minX; x <= maxX; x++){
			CSector *sector = CWorld::GetSector(x, y);
			InstanceLoadedModelsInSectorList(sector->m_lists[ENTITYLIST_BUILDINGS]);
			InstanceLoadedModelsInSectorList(sector->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]);
			InstanceLoadedModelsInSectorList(sector->m_lists[ENTITYLIST_OBJECTS]);
			InstanceLoadedModelsInSectorList(sector->m_lists[ENTITYLIST_DUMMIES]);
		}
	}
}

void
CStreaming::RequestIslands(eLevelName level)
{
	ISLAND_LOADING_ISNT(HIGH)
	switch(level){
	case LEVEL_MAINLAND:
		if(islandLODbeach != -1)
			RequestModelFromWorldScan(islandLODbeach, BIGBUILDINGFLAGS);
		break;
	case LEVEL_BEACH:
		if(islandLODmainland != -1)
			RequestModelFromWorldScan(islandLODmainland, BIGBUILDINGFLAGS);
		break;
	default: break;
	}
}

static char *IGnames[] = {
	"player",
	"player2",
	"player3",
	"player4",
	"player5",
	"player6",
	"player7",
	"player8",
	"player9",
	"play10",
	"play11",
	"igken",
	"igcandy",
	"igsonny",
	"igbuddy",
	"igjezz",
	"ighlary",
	"igphil",
	"igmerc",
	"igdick",
	"igdiaz",
	""
};

static char *CSnames[] = {
	"csplay",
	"csplay2",
	"csplay3",
	"csplay4",
	"csplay5",
	"csplay6",
	"csplay7",
	"csplay8",
	"csplay9",
	"csplay10",
	"csplay11",
	"csken",
	"cscandy",
	"cssonny",
	"csbuddy",
	"csjezz",
	"cshlary",
	"csphil",
	"csmerc",
	"csdick",
	"csdiaz",
	""
};

void
CStreaming::RequestSpecialModel(int32 modelId, const char *modelName, int32 flags)
{
	CBaseModelInfo *mi;
	int txdId;
#ifdef WII
	int oldTxdId;
#endif
	char oldName[48];
	uint32 pos, size;
	int i, n;
#if defined(WII) && WII_SPECIAL_STREAM_DIAGNOSTICS
	const char *requestedName = modelName;
#endif

	mi = CModelInfo::GetModelInfo(modelId);
#ifdef WII
	oldTxdId = mi->GetTxdSlot();
#endif
	if(strncasecmp("CSPlay", modelName, 6) == 0){
		char *curname = CModelInfo::GetModelInfo(MI_PLAYER)->GetModelName();
		for(int i = 0; CSnames[i][0]; i++){
			if(strcasecmp(curname, IGnames[i]) == 0){
				modelName = CSnames[i];
				break;
			}
		}
	}
#if defined(WII) && WII_SPECIAL_STREAM_DIAGNOSTICS
	printf("[SPEC-WII] request_special modelId=%d slot=%d req=%s resolved=%s current=%s flags=0x%02X refs=%d state=%s\n",
		modelId,
		modelId - MI_SPECIAL01 + 1,
		requestedName,
		modelName,
		mi->GetModelName(),
		flags,
		mi->GetNumRefs(),
		WiiStreamStateName(ms_aInfoForModel[modelId].m_loadState));
#endif
	if(!CGeneral::faststrcmp(mi->GetModelName(), modelName)){
		// Already have the correct name, just request it
#if defined(WII) && WII_SPECIAL_STREAM_DIAGNOSTICS
		printf("[SPEC-WII] request_special reuse modelId=%d slot=%d model=%s flags=0x%02X state=%s\n",
			modelId,
			modelId - MI_SPECIAL01 + 1,
			modelName,
			flags,
			WiiStreamStateName(ms_aInfoForModel[modelId].m_loadState));
#endif
		RequestModel(modelId, flags);
		return;
	}

	if(mi->GetNumRefs() > 0){
		n = CPools::GetPedPool()->GetSize()-1;
		for(i = n; i >= 0 && mi->GetNumRefs() > 0; i--){
			CPed *ped = CPools::GetPedPool()->GetSlot(i);
			if(ped && ped->GetModelIndex() == modelId &&
			   !ped->IsPlayer() && ped->CanBeDeletedEvenInVehicle())
				CTheScripts::RemoveThisPed(ped);
		}
		n = CPools::GetObjectPool()->GetSize()-1;
		for(i = n; i >= 0 && mi->GetNumRefs() > 0; i--){
			CObject *obj = CPools::GetObjectPool()->GetSlot(i);
			if(obj && obj->GetModelIndex() == modelId && obj->CanBeDeleted()){
				CWorld::Remove(obj);
				CWorld::RemoveReferencesToDeletedObject(obj);
				delete obj;
			}
		}
	}

	strcpy(oldName, mi->GetModelName());
	mi->SetModelName(modelName);

	// What exactly is going on here?
	if(CModelInfo::GetModelInfo(oldName, nil)){
		txdId = CTxdStore::FindTxdSlot(oldName);
		if(txdId != -1 && CTxdStore::GetSlot(txdId)->texDict){
			CTxdStore::AddRef(txdId);
			RemoveModel(modelId);
			CTxdStore::RemoveRefWithoutDelete(txdId);
		}else
			RemoveModel(modelId);
	}else
		RemoveModel(modelId);

	bool found = ms_pExtraObjectsDir->FindItem(modelName, pos, size);
#if defined(WII) && WII_SPECIAL_STREAM_DIAGNOSTICS
	if(!found){
		printf("[SPEC-WII] request_special missing_extra modelId=%d slot=%d model=%s\n",
			modelId,
			modelId - MI_SPECIAL01 + 1,
			modelName);
	}
#endif
	assert(found);
	mi->ClearTexDictionary();
	int specialTxdSlot = CTxdStore::FindTxdSlot(modelName);
	if(specialTxdSlot == -1)
		mi->SetTexDictionary("generic");
	else
		mi->SetTexDictionary(modelName);
#ifdef WII
	if(oldTxdId != mi->GetTxdSlot())
		ReleaseScriptOwnedTxdIfUnused(oldTxdId, modelId);
#endif
	ms_aInfoForModel[modelId].SetCdPosnAndSize(pos, size);
#if defined(WII) && WII_SPECIAL_STREAM_DIAGNOSTICS
	printf("[SPEC-WII] request_special queued modelId=%d slot=%d old=%s new=%s txd=%s txdSlot=%d cdPos=%u size=%u flags=0x%02X\n",
		modelId,
		modelId - MI_SPECIAL01 + 1,
		oldName,
		modelName,
		specialTxdSlot == -1 ? "generic" : modelName,
		specialTxdSlot,
		pos,
		size,
		flags);
#endif
	RequestModel(modelId, flags);
}

void
CStreaming::RequestSpecialChar(int32 charId, const char *modelName, int32 flags)
{
	RequestSpecialModel(charId + MI_SPECIAL01, modelName, flags);
}

bool
CStreaming::HasSpecialCharLoaded(int32 id)
{
	int32 modelId = id + MI_SPECIAL01;
	bool loaded = HasModelLoaded(modelId);
#if defined(WII) && WII_SPECIAL_STREAM_DIAGNOSTICS
	if(id >= 0 && id <= MI_SPECIAL21 - MI_SPECIAL01){
		static bool s_init = false;
		static uint8 s_lastModelState[MI_SPECIAL21 - MI_SPECIAL01 + 1];
		static uint8 s_lastTxdState[MI_SPECIAL21 - MI_SPECIAL01 + 1];
		static bool s_lastLoaded[MI_SPECIAL21 - MI_SPECIAL01 + 1];
		if(!s_init){
			for(int i = 0; i < ARRAY_SIZE(s_lastModelState); i++){
				s_lastModelState[i] = 0xFF;
				s_lastTxdState[i] = 0xFF;
				s_lastLoaded[i] = false;
			}
			s_init = true;
		}

		CBaseModelInfo *mi = CModelInfo::GetModelInfo(modelId);
		CStreamingInfo *modelInfo = &ms_aInfoForModel[modelId];
		int txdSlot = CTxdStore::FindTxdSlot(mi->GetModelName());
		uint8 txdState = txdSlot != -1 ? ms_aInfoForModel[txdSlot + STREAM_OFFSET_TXD].m_loadState : 0xFE;
		if(s_lastLoaded[id] != loaded ||
		   s_lastModelState[id] != modelInfo->m_loadState ||
		   s_lastTxdState[id] != txdState){
			uint32 pos = 0, size = 0;
			modelInfo->GetCdPosnAndSize(pos, size);
			printf("[SPEC-WII] has_special id=%d loaded=%d model=%s modelState=%s txd=%s txdSlot=%d txdState=%s flags=0x%02X refs=%d cdPos=%u size=%u\n",
				id + 1,
				loaded ? 1 : 0,
				mi->GetModelName(),
				WiiStreamStateName(modelInfo->m_loadState),
				txdSlot == -1 ? "generic" : CTxdStore::GetTxdName(txdSlot),
				txdSlot,
				txdState == 0xFE ? "n/a" : WiiStreamStateName(txdState),
				modelInfo->m_flags,
				mi->GetNumRefs(),
				pos,
				size);
			s_lastLoaded[id] = loaded;
			s_lastModelState[id] = modelInfo->m_loadState;
			s_lastTxdState[id] = txdState;
		}
	}
#endif
	return loaded;
}

void
CStreaming::SetMissionDoesntRequireSpecialChar(int32 id)
{
	return SetMissionDoesntRequireModel(id + MI_SPECIAL01);
}

void
CStreaming::DecrementRef(int32 id)
{
	ms_numModelsRequested--;
#ifdef WII
	WiiStreamClearQueued(id);
#endif
	if(ms_aInfoForModel[id].IsPriority()){
		ms_aInfoForModel[id].m_flags &= ~STREAMFLAGS_PRIORITY;
		ms_numPriorityRequests--;
	#ifdef WII
		if(ms_numPriorityRequests == 0)
			gWiiStreamForegroundServicesSinceFair = 0;
	#endif
	}
}

void
CStreaming::RemoveModel(int32 id)
{
	int i;
	uint8 oldState = ms_aInfoForModel[id].m_loadState;

#if defined(WII) && WII_STREAM_MEMORY_DIAGNOSTICS
	bool captureTrim = oldState == STREAMSTATE_LOADED && WiiStreamDiagCapturingTrim();
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	uint8 oldFlags = ms_aInfoForModel[id].m_flags;
	size_t streamUsedBefore = ms_memoryUsed;
	WiiMemoryPoolSnapshot before;
	if(captureTrim)
		WiiMemoryGetPoolSnapshot(&before);
#endif
#endif

	if(oldState == STREAMSTATE_NOTLOADED){
#ifdef WII
		gWiiStreamRequestClass[id] = WII_STREAM_REQUEST_NORMAL;
		WiiStreamClearDispatchedFlags(id);
		WiiStreamClearSameFrameWorldLoad(id);
		WiiStreamClearResidentCost(id);
#endif
		return;
	}

	// A GX alias can hold a texture from a donor TXD while the consumer model
	// is resident. Keep the stream entry loaded until that donor pin is gone.
	if(id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL &&
	   (oldState == STREAMSTATE_LOADED || oldState == STREAMSTATE_STARTED) &&
	   CTxdStore::IsTxdAliasPinned(id - STREAM_OFFSET_TXD))
		return;

	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED){
		if(id < STREAM_OFFSET_TXD)
			CModelInfo::GetModelInfo(id)->DeleteRwObject();
		else if(id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL)
			CTxdStore::RemoveTxd(id - STREAM_OFFSET_TXD);
		else if(id >= STREAM_OFFSET_COL && id < STREAM_OFFSET_ANIM)
			CColStore::RemoveCol(id - STREAM_OFFSET_COL);
		else if(id >= STREAM_OFFSET_ANIM){
			assert(id < NUMSTREAMINFO);
			CAnimManager::RemoveAnimBlock(id - STREAM_OFFSET_ANIM);
		}
		ms_memoryUsed -= ms_aInfoForModel[id].GetCdSize()*CDSTREAM_SECTOR_SIZE;
	}

	if(ms_aInfoForModel[id].m_next){
		// Remove from list, model is neither loaded nor requested
		if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_INQUEUE)
			DecrementRef(id);
		ms_aInfoForModel[id].RemoveFromList();
	}else if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_READING){
		for(i = 0; i < 4; i++){
			if(ms_channel[0].streamIds[i] == id)
				ms_channel[0].streamIds[i] = -1;
			if(ms_channel[1].streamIds[i] == id)
				ms_channel[1].streamIds[i] = -1;
		}
	}

	if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_STARTED){
		if(id < STREAM_OFFSET_TXD)
			RpClumpGtaCancelStream();
		else if(id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL)
			CTxdStore::RemoveTxd(id - STREAM_OFFSET_TXD);
		else if(id >= STREAM_OFFSET_COL && id < STREAM_OFFSET_ANIM)
			CColStore::RemoveCol(id - STREAM_OFFSET_COL);
		else if(id >= STREAM_OFFSET_ANIM){
			assert(id < NUMSTREAMINFO);
			CAnimManager::RemoveAnimBlock(id - STREAM_OFFSET_ANIM);
		}
	}

	ms_aInfoForModel[id].m_loadState = STREAMSTATE_NOTLOADED;

#if defined(WII) && WII_STREAM_MEMORY_DIAGNOSTICS
	WiiStreamDiagCancelRequest(id, oldState);
	if(captureTrim){
		WiiStreamDiagRecordTrimVictim(id);
#if WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
		WiiMemoryPoolSnapshot after;
		WiiMemoryGetPoolSnapshot(&after);
		WiiStreamDiagRecordTrimVictimEvent(id, oldFlags, before, after,
		                                   streamUsedBefore, ms_memoryUsed);
#endif
	}
#endif
#ifdef WII
	gWiiStreamRequestClass[id] = WII_STREAM_REQUEST_NORMAL;
	gWiiStreamResourcePoolMask[id] = 0;
	WiiStreamClearDispatchedFlags(id);
	WiiStreamClearSameFrameWorldLoad(id);
	WiiStreamClearResidentCost(id);
#endif
}

void
CStreaming::RemoveUnusedBuildings(eLevelName level)
{
	if(level != LEVEL_BEACH)
		RemoveBuildings(LEVEL_BEACH);
	if(level != LEVEL_MAINLAND)
		RemoveBuildings(LEVEL_MAINLAND);
}

void
CStreaming::RemoveBuildings(eLevelName level)
{
	int i, n;
	CEntity *e;
	CBaseModelInfo *mi;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetBuildingPool()->GetSlot(i);
		if(e && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}

	n = CPools::GetTreadablePool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetTreadablePool()->GetSlot(i);
		if(e && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}

	n = CPools::GetObjectPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetObjectPool()->GetSlot(i);
		if(e && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered && ((CObject*)e)->ObjectCreatedBy == GAME_OBJECT){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}

	n = CPools::GetDummyPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetDummyPool()->GetSlot(i);
		if(e && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}
}

void
CStreaming::RemoveBuildingsNotInArea(int32 area)
{
	int i, n;
	CEntity *e;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetBuildingPool()->GetSlot(i);
		if(e && e->m_rwObject && !IsAreaVisible(area) &&
		   (!e->bIsBIGBuilding || e->bStreamBIGBuilding)){
			if(e->bIsBIGBuilding)
				RequestModel(e->GetModelIndex(), 0);
			if(!e->bImBeingRendered)
				e->DeleteRwObject();
		}
	}

	n = CPools::GetTreadablePool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetTreadablePool()->GetSlot(i);
		if(e && e->m_rwObject && !IsAreaVisible(area) &&
		   (!e->bIsBIGBuilding || e->bStreamBIGBuilding)){
			if(e->bIsBIGBuilding)
				RequestModel(e->GetModelIndex(), 0);
			if(!e->bImBeingRendered)
				e->DeleteRwObject();
		}
	}

	n = CPools::GetObjectPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetObjectPool()->GetSlot(i);
		if(e && e->m_rwObject && !IsAreaVisible(area) &&
		   (!e->bIsBIGBuilding || e->bStreamBIGBuilding)){
			if(e->bIsBIGBuilding)
				RequestModel(e->GetModelIndex(), 0);
			if(!e->bImBeingRendered)
				e->DeleteRwObject();
		}
	}

	n = CPools::GetDummyPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetDummyPool()->GetSlot(i);
		if(e && e->m_rwObject && !IsAreaVisible(area) &&
		   (!e->bIsBIGBuilding || e->bStreamBIGBuilding)){
			if(e->bIsBIGBuilding)
				RequestModel(e->GetModelIndex(), 0);
			if(!e->bImBeingRendered)
				e->DeleteRwObject();
		}
	}
}

void
CStreaming::RemoveUnusedBigBuildings(eLevelName level)
{
	ISLAND_LOADING_IS(LOW)
	{
	if(level != LEVEL_BEACH)
		RemoveBigBuildings(LEVEL_BEACH);
	if(level != LEVEL_MAINLAND)
		RemoveBigBuildings(LEVEL_MAINLAND);
	}
	RemoveIslandsNotUsed(level);
}

void
DeleteIsland(CEntity *island)
{
	if(island == nil)
		return;
	int32 model = island->GetModelIndex();
	if(island->m_rwObject == nil &&
	   CStreaming::ms_aInfoForModel[model].m_loadState == STREAMSTATE_NOTLOADED)
		return;
	if(island->bImBeingRendered)
		return;
	island->DeleteRwObject();
	CStreaming::RemoveModel(model);
}

void
CStreaming::RemoveIslandsNotUsed(eLevelName level)
{
	int i;
#if defined(WII) && WII_STREAM_ATOMIC_BIG_HANDOFF
	if(gWiiIslandPhase != WII_ISLAND_IDLE && level == gWiiIslandTargetLevel &&
	   !gWiiIslandAtomicBigReady)
		return;
#endif
	if(pIslandLODmainlandEntity == nil || pIslandLODbeachEntity == nil)
	for(i = CPools::GetBuildingPool()->GetSize()-1; i >= 0; i--){
		CBuilding *building = CPools::GetBuildingPool()->GetSlot(i);
		if(building == nil)
			continue;
		if(building->GetModelIndex() == islandLODmainland)
			pIslandLODmainlandEntity = building;
		if(building->GetModelIndex() == islandLODbeach)
			pIslandLODbeachEntity = building;
	}
#ifdef NO_ISLAND_LOADING
	if(FrontEndMenuManager.m_PrefsIslandLoading == CMenuManager::ISLAND_LOADING_HIGH) {
		DeleteIsland(pIslandLODmainlandEntity);
		DeleteIsland(pIslandLODbeachEntity);
	} else
#endif
	switch(level){
	case LEVEL_MAINLAND:
		DeleteIsland(pIslandLODmainlandEntity);
		break;
	case LEVEL_BEACH:
		DeleteIsland(pIslandLODbeachEntity);

		break;
	}
}

void
CStreaming::RemoveBigBuildings(eLevelName level)
{
	int i, n;
	CEntity *e;
	CBaseModelInfo *mi;

	n = CPools::GetBuildingPool()->GetSize()-1;
	for(i = n; i >= 0; i--){
		e = CPools::GetBuildingPool()->GetSlot(i);
		if(e && e->bIsBIGBuilding && e->m_level == level){
			mi = CModelInfo::GetModelInfo(e->GetModelIndex());
			if(!e->bImBeingRendered){
				e->DeleteRwObject();
				if (mi->GetNumRefs() == 0)
					RemoveModel(e->GetModelIndex());
			}
		}
	}
}

bool
CStreaming::RemoveLoadedVehicle(uint32 poolBit)
{
	int i, id;

	for(i = 0; i < MAXVEHICLESLOADED; i++){
		ms_lastVehicleDeleted++;
		if(ms_lastVehicleDeleted == MAXVEHICLESLOADED)
			ms_lastVehicleDeleted = 0;
		id = ms_vehiclesLoaded[ms_lastVehicleDeleted];
	#ifdef WII
		if(id != -1 && poolBit != 0 &&
		   (gWiiStreamResourcePoolMask[id] & poolBit) == 0)
			continue;
	#else
		(void)poolBit;
	#endif
		if(id != -1 && CanRemoveModel(id) && CModelInfo::GetModelInfo(id)->GetNumRefs() == 0 &&
		   ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED)
			goto found;
	}
	return false;
found:
	RemoveModel(ms_vehiclesLoaded[ms_lastVehicleDeleted]);
	ms_numVehiclesLoaded--;
	ms_vehiclesLoaded[ms_lastVehicleDeleted] = -1;
	CVehicleModelInfo* pVehicleInfo = (CVehicleModelInfo*)CModelInfo::GetModelInfo(id);
	if (pVehicleInfo->m_vehicleClass != -1)
		CCarCtrl::RemoveFromLoadedVehicleArray(id, pVehicleInfo->m_vehicleClass);
	return true;
}

#ifdef WII
struct WiiStreamTxdPoolQuery
{
	uint8 mask;
};

static RwTexture*
WiiStreamCollectTxdPoolMask(RwTexture *texture, void *data)
{
	WiiStreamTxdPoolQuery *query = (WiiStreamTxdPoolQuery*)data;
	if(WiiMemoryOwnsGenericMem2(texture))
		query->mask |= WII_STREAM_PRESSURE_GENERIC;

	RwRaster *raster = RwTextureGetRaster(texture);
	if(raster == nil)
		return texture;
	if(WiiMemoryOwnsGenericMem2(raster))
		query->mask |= WII_STREAM_PRESSURE_GENERIC;

	uint32 storageMask = rw::gx::rasterStorageMask(raster);
	if(storageMask & rw::gx::GX_RASTER_STORAGE_GENERIC_MEM2)
		query->mask |= WII_STREAM_PRESSURE_GENERIC;
	if(storageMask & rw::gx::GX_RASTER_STORAGE_DEDICATED)
		query->mask |= WII_STREAM_PRESSURE_GX;
	return texture;
}

static uint8
WiiStreamCurrentTxdPoolMask(int32 streamId)
{
	WiiStreamTxdPoolQuery query = { WII_STREAM_PRESSURE_NEWLIB };
	TxdDef *txd = CTxdStore::GetSlot(streamId - STREAM_OFFSET_TXD);
	if(txd == nil || txd->texDict == nil)
		return query.mask;
	if(WiiMemoryOwnsGenericMem2(txd->texDict))
		query.mask |= WII_STREAM_PRESSURE_GENERIC;
	RwTexDictionaryForAllTextures(txd->texDict,
	                              WiiStreamCollectTxdPoolMask, &query);
	return query.mask;
}

static bool
WiiStreamResourceMatchesPool(int32 streamId, uint32 poolBit)
{
	if(poolBit == 0)
		return true;
	if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL)
		gWiiStreamResourcePoolMask[streamId] =
			WiiStreamCurrentTxdPoolMask(streamId);
	return (gWiiStreamResourcePoolMask[streamId] & poolBit) != 0;
}

static bool
WiiStreamHasRetireablePoolResidency(int32 streamId, uint32 poolBit)
{
	if(poolBit != WII_STREAM_PRESSURE_GX ||
	   streamId >= STREAM_OFFSET_TXD)
		return true;

	// A model can inherit the GX pool bit from a conversion without owning a
	// releasable GX allocation. Only use the measured ledger for model victims;
	// TXD candidates are checked from their live raster storage below.
	uint16 gxKiB = gWiiStreamResidentCost[streamId].gxKiB;
	return gxKiB != 0 && gxKiB != WII_STREAM_RESIDENT_UNKNOWN_KIB;
}

static bool
WiiStreamCanRemoveLoadedResource(int32 streamId)
{
	if(streamId < STREAM_OFFSET_TXD){
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(streamId);
		return mi != nil && mi->GetNumRefs() == 0;
	}
	if(streamId < STREAM_OFFSET_COL)
		return CTxdStore::GetNumRefs(streamId - STREAM_OFFSET_TXD) == 0 &&
		       !CStreaming::IsTxdUsedByRequestedModels(streamId - STREAM_OFFSET_TXD) &&
		       !CTxdStore::IsTxdAliasPinned(streamId - STREAM_OFFSET_TXD);
	if(streamId >= STREAM_OFFSET_ANIM)
		return CAnimManager::GetNumRefsToAnimBlock(streamId - STREAM_OFFSET_ANIM) == 0 &&
		       !CStreaming::AreAnimsUsedByRequestedModels(streamId - STREAM_OFFSET_ANIM);
	return false;
}

// A TXD can be unloaded before its model dependencies only on paper: the
// model's RenderWare objects may still own the texture rasters. Retire one
// eligible dependency first and leave the TXD in place while any blocked
// dependency remains.
static int32
WiiStreamFindTxdDependencyVictim(int32 txdId, uint32 excludeMask,
	bool *sameFrameDeferred)
{
	if(sameFrameDeferred)
		*sameFrameDeferred = false;
	bool blocked = false;
	for(int32 modelId = 0; modelId < STREAM_OFFSET_TXD; modelId++){
		CStreamingInfo *info = &CStreaming::ms_aInfoForModel[modelId];
		if(info->m_loadState != STREAMSTATE_LOADED)
			continue;
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(modelId);
		if(mi == nil || mi->GetTxdSlot() != txdId)
			continue;
		if(info->m_next == nil ||
		   (info->m_flags & (excludeMask | STREAMFLAGS_DONT_REMOVE |
		                     STREAMFLAGS_CANT_REMOVE)) != 0 ||
		   mi->GetNumRefs() != 0 ||
		   WiiIslandTransitionProtectsStreamId(modelId)){
			blocked = true;
			continue;
		}
		if(WiiStreamProtectsSameFrameWorldLoad(modelId)){
			blocked = true;
			if(sameFrameDeferred)
				*sameFrameDeferred = true;
		}
	}
	if(blocked)
		return -2;

	for(CStreamingInfo *si = CStreaming::ms_endLoadedList.m_prev;
	    si != &CStreaming::ms_startLoadedList; si = si->m_prev){
		int32 modelId = si - CStreaming::ms_aInfoForModel;
		if(modelId < STREAM_OFFSET_TXD &&
		   si->m_loadState == STREAMSTATE_LOADED){
			CBaseModelInfo *mi = CModelInfo::GetModelInfo(modelId);
			if(mi != nil && mi->GetTxdSlot() == txdId)
				return modelId;
		}
	}
	return -1;
}

static bool
WiiStreamRemoveLeastUsedForPool(uint32 excludeMask, uint32 poolBit,
	int32 *removedId, bool *sameFrameDeferred)
{
	if(sameFrameDeferred)
		*sameFrameDeferred = false;
	int32 deferredId = -1;
#if WII_STREAM_P7_VISIBLE_TXD_GUARD
	int32 recentVisibleFallbackId = -1;
	uint32 recentVisibleNowMs = 0;
#endif
#if WII_STREAM_MEMORY_DIAGNOSTICS && WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
	int32 globalOldestId = -1;
	uint32 globalOldestAgeMs = UINT32_MAX;
	if(poolBit == 0){
		for(CStreamingInfo *candidate = CStreaming::ms_endLoadedList.m_prev;
		    candidate != &CStreaming::ms_startLoadedList; candidate = candidate->m_prev){
			if(candidate->m_flags & excludeMask)
				continue;
			int32 candidateId = candidate - CStreaming::ms_aInfoForModel;
			if(WiiIslandTransitionProtectsStreamId(candidateId) ||
			   !WiiStreamCanRemoveLoadedResource(candidateId) ||
			   WiiStreamProtectsSameFrameWorldLoad(candidateId))
				continue;
			globalOldestId = candidateId;
			globalOldestAgeMs = WiiStreamDiagAge(
				WiiStreamDiagNowMs(), gWiiStreamDiagLastRequestMs[candidateId]);
			break;
		}
	}
#endif
	for(int32 pass = 0; pass < 3; pass++){
		for(CStreamingInfo *si = CStreaming::ms_endLoadedList.m_prev;
		    si != &CStreaming::ms_startLoadedList; si = si->m_prev){
			if(si->m_flags & excludeMask)
				continue;
			int32 streamId = si - CStreaming::ms_aInfoForModel;
			if(WiiIslandTransitionProtectsStreamId(streamId))
				continue;
			if(!WiiStreamResourceMatchesPool(streamId, poolBit))
				continue;
			if(!WiiStreamHasRetireablePoolResidency(streamId, poolBit))
				continue;

			int32 victimPass;
#if WII_STREAM_ARCHIVE_GLOBAL_LRU
			if(poolBit == 0)
				victimPass = 0;
			else
#endif
			if(poolBit == WII_STREAM_PRESSURE_GX)
				victimPass = streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL ? 0 :
				             streamId < STREAM_OFFSET_TXD ? 1 : 2;
			else if(poolBit == WII_STREAM_PRESSURE_GENERIC)
				victimPass = streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL ? 0 :
				             streamId >= STREAM_OFFSET_ANIM ? 1 : 2;
			else
				victimPass = streamId < STREAM_OFFSET_TXD ? 0 :
				             streamId >= STREAM_OFFSET_ANIM ? 1 : 2;
			if(victimPass != pass)
				continue;

			if(!WiiStreamCanRemoveLoadedResource(streamId))
				continue;
			if(WiiStreamProtectsSameFrameWorldLoad(streamId)){
				if(deferredId == -1)
					deferredId = streamId;
				continue;
			}
#if WII_STREAM_P7_VISIBLE_TXD_GUARD
			if((poolBit == WII_STREAM_PRESSURE_GX || poolBit == 0) &&
			   streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
				if(recentVisibleNowMs == 0)
					recentVisibleNowMs = WiiStreamNowMs();
				if(WiiStreamProtectsRecentVisibleTxd(streamId, recentVisibleNowMs)){
					if(recentVisibleFallbackId == -1)
						recentVisibleFallbackId = streamId;
					continue;
				}
			}
#endif
			if(poolBit == WII_STREAM_PRESSURE_GX &&
			   streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
				bool dependencyDeferred = false;
				int32 dependencyId = WiiStreamFindTxdDependencyVictim(
					streamId - STREAM_OFFSET_TXD, excludeMask,
					&dependencyDeferred);
				if(dependencyId == -2){
					if(dependencyDeferred && deferredId == -1)
						deferredId = streamId;
					continue;
				}
				if(dependencyId >= 0){
					CStreaming::RemoveModel(dependencyId);
					if(removedId)
						*removedId = dependencyId;
					return true;
				}
			}

#if WII_STREAM_MEMORY_DIAGNOSTICS && WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
			WiiStreamDiagSetVictimCounterfactual(
				streamId, globalOldestId, globalOldestAgeMs);
#endif
			CStreaming::RemoveModel(streamId);
			if(removedId)
				*removedId = streamId;
			return true;
		}
#if WII_STREAM_P7_VISIBLE_TXD_GUARD
		if(poolBit == WII_STREAM_PRESSURE_GX && pass == 0 &&
		   recentVisibleFallbackId != -1){
			bool dependencyDeferred = false;
			int32 dependencyId = WiiStreamFindTxdDependencyVictim(
				recentVisibleFallbackId - STREAM_OFFSET_TXD, excludeMask,
				&dependencyDeferred);
			if(dependencyId == -2){
				if(dependencyDeferred && deferredId == -1)
					deferredId = recentVisibleFallbackId;
				continue;
			}
			if(dependencyId >= 0){
				CStreaming::RemoveModel(dependencyId);
				if(removedId)
					*removedId = dependencyId;
				return true;
			}
#if WII_STREAM_MEMORY_DIAGNOSTICS && WII_STREAM_MEMORY_DIAGNOSTIC_EVENTS
			WiiStreamDiagSetVictimCounterfactual(
				recentVisibleFallbackId, globalOldestId, globalOldestAgeMs);
#endif
			CStreaming::RemoveModel(recentVisibleFallbackId);
			if(removedId)
				*removedId = recentVisibleFallbackId;
			return true;
		}
#endif
#if WII_STREAM_ARCHIVE_GLOBAL_LRU
		if(poolBit == 0)
			break;
#endif
	}
	if(poolBit != 0 &&
	   (CStreaming::ms_numVehiclesLoaded > 7 ||
	    CGame::currArea != AREA_MAIN_MAP && CStreaming::ms_numVehiclesLoaded > 4) &&
	   CStreaming::RemoveLoadedVehicle(poolBit))
		return true;
	if(deferredId != -1)
		WiiStreamReportSameFrameVictimDeferral(deferredId, poolBit);
	if(deferredId != -1 && sameFrameDeferred)
		*sameFrameDeferred = true;
	return false;
}

static uint32
WiiStreamSelectPressureBit(uint32 pressure,
	const WiiMemoryPoolSnapshot &snapshot, size_t requestedBytes)
{
	static const uint32 poolOrder[] = {
#if WII_STREAM_GX_HEADROOM_GUARD
		WII_STREAM_PRESSURE_GX_ADMISSION,
#endif
		WII_STREAM_PRESSURE_GX,
		WII_STREAM_PRESSURE_GENERIC,
		WII_STREAM_PRESSURE_NEWLIB
	};
	uint32 selected = 0;
	uint32 selectedDeficit = 0;
	for(int32 i = 0; i < ARRAY_SIZE(poolOrder); i++){
		uint32 poolBit = poolOrder[i];
		if((pressure & poolBit) == 0)
			continue;
		uint32 deficit = poolBit == WII_STREAM_PRESSURE_GX_ADMISSION ?
		                 WiiStreamGxAdmissionPressureDeficit(snapshot) :
		                 WiiMemoryGetStreamingPressureDeficit(
			                 &snapshot, poolBit, requestedBytes);
		if(selected == 0 || deficit > selectedDeficit){
			selected = poolBit;
			selectedDeficit = deficit;
		}
	}
	return selected;
}

static int32
WiiStreamPressureIndex(uint32 poolBit)
{
	poolBit = WiiStreamPressureServiceBit(poolBit);
	return poolBit == WII_STREAM_PRESSURE_GENERIC ? 0 :
	       poolBit == WII_STREAM_PRESSURE_NEWLIB ? 1 : 2;
}

static size_t
WiiStreamPoolReclaimedBytes(uint32 poolBit, const WiiMemoryPoolSnapshot &before,
	const WiiMemoryPoolSnapshot &after)
{
	poolBit = WiiStreamPressureServiceBit(poolBit);
	if(poolBit == WII_STREAM_PRESSURE_GENERIC)
		return before.genericUsed > after.genericUsed ?
		       before.genericUsed - after.genericUsed : 0;
	if(poolBit == WII_STREAM_PRESSURE_NEWLIB){
		size_t released = before.newlibUsed > after.newlibUsed ?
		                  before.newlibUsed - after.newlibUsed : 0;
		size_t rawReleased = after.rawArena2Remaining > before.rawArena2Remaining ?
		                     after.rawArena2Remaining - before.rawArena2Remaining : 0;
		return Max(released, rawReleased);
	}
	return before.gxUsed > after.gxUsed ? before.gxUsed - after.gxUsed : 0;
}

static bool
WiiStreamPoolMadeProgress(uint32 poolBit, uint32 nextPressure,
	const WiiMemoryPoolSnapshot &before, const WiiMemoryPoolSnapshot &after,
	size_t reclaimedBytes)
{
	uint32 serviceBit = WiiStreamPressureServiceBit(poolBit);
	if((nextPressure & poolBit) == 0 ||
	   reclaimedBytes >= WII_STREAM_MIN_POOL_PROGRESS_BYTES)
		return true;
	if(serviceBit == WII_STREAM_PRESSURE_GENERIC)
		return after.genericLargest >= before.genericLargest +
		       WII_STREAM_MIN_POOL_PROGRESS_BYTES;
	if(serviceBit == WII_STREAM_PRESSURE_GX)
		return after.gxLargest >= before.gxLargest +
		       WII_STREAM_MIN_POOL_PROGRESS_BYTES;
	return false;
}
#endif

bool
CStreaming::RemoveLeastUsedModel(uint32 excludeMask)
{
#ifdef WII
	if(WiiStreamRemoveLeastUsedForPool(excludeMask, 0, nil, nil))
		return true;
#else
	CStreamingInfo *si;
	int streamId;

	for(si = ms_endLoadedList.m_prev; si != &ms_startLoadedList; si = si->m_prev){
		if(si->m_flags & excludeMask)
			continue;
		streamId = si - ms_aInfoForModel;
		if(streamId < STREAM_OFFSET_TXD){
			if (CModelInfo::GetModelInfo(streamId)->GetNumRefs() == 0) {
				RemoveModel(streamId);
				return true;
			}
		}else if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
			if(CTxdStore::GetNumRefs(streamId - STREAM_OFFSET_TXD) == 0 &&
			   !IsTxdUsedByRequestedModels(streamId - STREAM_OFFSET_TXD)){
				RemoveModel(streamId);
				return true;
			}
		}else if(streamId >= STREAM_OFFSET_ANIM){
			assert(streamId < NUMSTREAMINFO);
			if(CAnimManager::GetNumRefsToAnimBlock(streamId - STREAM_OFFSET_ANIM) == 0 &&
			   !AreAnimsUsedByRequestedModels(streamId - STREAM_OFFSET_ANIM)){
				RemoveModel(streamId);
				return true;
			}
		}
	}
#endif
	return (ms_numVehiclesLoaded > 7 || CGame::currArea != AREA_MAIN_MAP && ms_numVehiclesLoaded > 4) && RemoveLoadedVehicle();
}

void
CStreaming::RemoveAllUnusedModels(void)
{
	int i;

	for(i = 0; i < MAXVEHICLESLOADED; i++)
		RemoveLoadedVehicle();

	for(i = NUM_DEFAULT_MODELS; i < MODELINFOSIZE; i++){
		if(ms_aInfoForModel[i].m_loadState == STREAMSTATE_LOADED &&
		    CModelInfo::GetModelInfo(i)->GetNumRefs() == 0) {
			RemoveModel(i);
			ms_aInfoForModel[i].m_loadState = STREAMSTATE_NOTLOADED;
		}
	}
}

void
CStreaming::RemoveUnusedModelsInLoadedList(void)
{
#if defined(WII) && WII_STREAM_PS2_TRANSITION_PURGE
	if(gWiiIslandPhase != WII_ISLAND_READ || gWiiIslandPs2PurgeDone)
		return;
	gWiiIslandPs2PurgeDone = true;

	uint64 purgeStartTicks = gettime();
	int32 vehicleAttempts = 0;
	int32 scanned = 0;
	int32 removed = 0;
	int32 vehiclesRemoved = 0;
	int32 modelsRemoved = 0;
	int32 txdsRemoved = 0;
	int32 skippedProtected = 0;
	int32 skippedFlags = 0;
	int32 skippedRefs = 0;
	int32 skippedDependencies = 0;
	int32 skippedAliasPins = 0;
	int32 skippedOther = 0;

	for(; vehicleAttempts < 20; ){
		vehicleAttempts++;
		if(!RemoveLoadedVehicle())
			break;
		removed++;
		vehiclesRemoved++;
	}

	for(CStreamingInfo *si = ms_endLoadedList.m_prev, *prev = nil;
	    si != &ms_startLoadedList; si = prev){
		prev = si->m_prev;
		scanned++;
		int32 streamId = si - ms_aInfoForModel;

		if(si->m_loadState != STREAMSTATE_LOADED){
			skippedOther++;
			continue;
		}
		if(WiiIslandTransitionProtectsStreamId(streamId)){
			skippedProtected++;
			continue;
		}
		if((si->m_flags & (STREAMFLAGS_DONT_REMOVE |
		                  STREAMFLAGS_SCRIPTOWNED |
		                  STREAMFLAGS_KEEP_IN_MEMORY)) != 0){
			skippedFlags++;
			continue;
		}

		if(streamId < STREAM_OFFSET_TXD){
			CBaseModelInfo *mi = CModelInfo::GetModelInfo(streamId);
			if(mi == nil){
				skippedOther++;
				continue;
			}
			if(mi->GetNumRefs() != 0){
				skippedRefs++;
				continue;
			}
			RemoveModel(streamId);
			removed++;
			modelsRemoved++;
			continue;
		}

		if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
			int32 txdId = streamId - STREAM_OFFSET_TXD;
			if(CTxdStore::IsTxdAliasPinned(txdId)){
				skippedAliasPins++;
				continue;
			}
			if(CTxdStore::GetNumRefs(txdId) != 0){
				skippedRefs++;
				continue;
			}
			if(IsTxdUsedByRequestedModels(txdId)){
				skippedDependencies++;
				continue;
			}
			RemoveModel(streamId);
			removed++;
			txdsRemoved++;
			continue;
		}

		skippedOther++;
	}

	SYS_Report("[WII-PS2-PURGE] vehicleAttempts=%d scanned=%d removed=%d vehicles=%d models=%d txds=%d skipProtected=%d skipFlags=%d skipRefs=%d skipDeps=%d skipAlias=%d skipOther=%d dt=%ums\n",
	           vehicleAttempts, scanned, removed, vehiclesRemoved,
	           modelsRemoved, txdsRemoved, skippedProtected, skippedFlags,
	           skippedRefs, skippedDependencies, skippedAliasPins, skippedOther,
	           (unsigned)ticks_to_millisecs(gettime() - purgeStartTicks));
#endif
}

bool
CStreaming::RemoveLoadedZoneModel(void)
{
	int i;

	if(ms_currentPedGrp == -1)
		return false;

	for(i = 0; i < NUMMODELSPERPEDGROUP; i++){
		int mi = CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i];
		if(mi != -1 && ms_bIsPedFromPedGroupLoaded[i] &&
		   HasModelLoaded(mi) &&  CanRemoveModel(mi) &&
		   CModelInfo::GetModelInfo(mi)->GetNumRefs() == 0){
			RemoveModel(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
			ms_numPedsLoaded--;
			ms_bIsPedFromPedGroupLoaded[i] = false;
			return true;
		}
	}

	return false;
}

bool
CStreaming::IsTxdUsedByRequestedModels(int32 txdId)
{
	CStreamingInfo *si;
	int streamId;
	int i;

	for(si = ms_startRequestedList.m_next; si != &ms_endRequestedList; si = si->m_next){
		streamId = si - ms_aInfoForModel;
		if(streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetTxdSlot() == txdId)
			return true;
	}

	for(i = 0; i < 4; i++){
		streamId = ms_channel[0].streamIds[i];
		if(streamId != -1 && streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetTxdSlot() == txdId)
			return true;
		streamId = ms_channel[1].streamIds[i];
		if(streamId != -1 && streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetTxdSlot() == txdId)
			return true;
	}

			// Check 3 (GC batch protection): scan READING/STARTED models
		for(i = 0; i < STREAM_OFFSET_TXD; i++){
			int state = ms_aInfoForModel[i].m_loadState;
			if((state == STREAMSTATE_READING || state == STREAMSTATE_STARTED) &&
			   CModelInfo::GetModelInfo(i) &&
			   CModelInfo::GetModelInfo(i)->GetTxdSlot() == txdId)
				return true;
		}

		return false;
}

bool
CStreaming::AreAnimsUsedByRequestedModels(int32 animId)
{
	CStreamingInfo *si;
	int streamId;
	int i;

	for(si = ms_startRequestedList.m_next; si != &ms_endRequestedList; si = si->m_next){
		streamId = si - ms_aInfoForModel;
		if(streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex() == animId)
			return true;
	}

	for(i = 0; i < 4; i++){
		streamId = ms_channel[0].streamIds[i];
		if(streamId != -1 && streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex() == animId)
			return true;
		streamId = ms_channel[1].streamIds[i];
		if(streamId != -1 && streamId < STREAM_OFFSET_TXD &&
		   CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex() == animId)
			return true;
	}

	return false;
}

int32
CStreaming::GetAvailableVehicleSlot(void)
{
	int i;
	for(i = 0; i < MAXVEHICLESLOADED; i++)
		if(ms_vehiclesLoaded[i] == -1)
			return i;
	return -1;
}

bool
CStreaming::AddToLoadedVehiclesList(int32 modelId)
{
	int i;
	int id;

	if(ms_numVehiclesLoaded < desiredNumVehiclesLoaded){
		// still room for vehicles
		for(i = 0; i < MAXVEHICLESLOADED; i++){
			if(ms_vehiclesLoaded[ms_lastVehicleDeleted] == -1)
				break;
			ms_lastVehicleDeleted++;
			if(ms_lastVehicleDeleted == MAXVEHICLESLOADED)
				ms_lastVehicleDeleted = 0;
		}
		assert(ms_vehiclesLoaded[ms_lastVehicleDeleted] == -1);
		ms_numVehiclesLoaded++;
	}else{
		// find vehicle we can remove
		for(i = 0; i < MAXVEHICLESLOADED; i++){
			id = ms_vehiclesLoaded[ms_lastVehicleDeleted];
			if(id != -1 && CanRemoveModel(id) &&
			   CModelInfo::GetModelInfo(id)->GetNumRefs() == 0)
				goto found;
			ms_lastVehicleDeleted++;
			if(ms_lastVehicleDeleted == MAXVEHICLESLOADED)
				ms_lastVehicleDeleted = 0;
		}
		id = -1;
found:
		if(id == -1){
			// didn't find anything, try a free slot
			id = GetAvailableVehicleSlot();
			if(id == -1)
				return false;	// still no luck
			ms_lastVehicleDeleted = id;
			// this is more than we wanted actually
			ms_numVehiclesLoaded++;
		}
		else{
			RemoveModel(id);
			CVehicleModelInfo* pVehicleInfo = (CVehicleModelInfo*)CModelInfo::GetModelInfo(id);
			if (pVehicleInfo->m_vehicleClass != -1)
				CCarCtrl::RemoveFromLoadedVehicleArray(id, pVehicleInfo->m_vehicleClass);
		}
	}

	ms_vehiclesLoaded[ms_lastVehicleDeleted++] = modelId;
	if(ms_lastVehicleDeleted == MAXVEHICLESLOADED)
		ms_lastVehicleDeleted = 0;
	CVehicleModelInfo* pVehicleInfo = (CVehicleModelInfo*)CModelInfo::GetModelInfo(modelId);
	if (pVehicleInfo->m_vehicleClass != -1)
		CCarCtrl::AddToLoadedVehicleArray(modelId, pVehicleInfo->m_vehicleClass, pVehicleInfo->m_frequency);
	return true;
}

bool
CStreaming::IsObjectInCdImage(int32 id)
{
	uint32 posn, size;
	return ms_aInfoForModel[id].GetCdPosnAndSize(posn, size);
}

void
CStreaming::SetModelIsDeletable(int32 id)
{
	ms_aInfoForModel[id].m_flags &= ~STREAMFLAGS_DONT_REMOVE;
	if ((id >= STREAM_OFFSET_TXD && id < STREAM_OFFSET_COL || CModelInfo::GetModelInfo(id)->GetModelType() != MITYPE_VEHICLE) &&
	   (ms_aInfoForModel[id].m_flags & STREAMFLAGS_SCRIPTOWNED) == 0){
		if(ms_aInfoForModel[id].m_loadState != STREAMSTATE_LOADED)
			RemoveModel(id);
		else if(ms_aInfoForModel[id].m_next == nil)
			ms_aInfoForModel[id].AddToList(&ms_startLoadedList);
	}
}

void
CStreaming::SetModelTxdIsDeletable(int32 id)
{
	SetModelIsDeletable(CModelInfo::GetModelInfo(id)->GetTxdSlot() + STREAM_OFFSET_TXD);
}

void
CStreaming::SetMissionDoesntRequireModel(int32 id)
{
#ifdef WII
	int32 txdId = -1;
	if(id >= 0 && id < STREAM_OFFSET_TXD){
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(id);
		if(mi)
			txdId = mi->GetTxdSlot();
	}
#endif
	ms_aInfoForModel[id].m_flags &= ~STREAMFLAGS_SCRIPTOWNED;
	if ((id >= STREAM_OFFSET_TXD || CModelInfo::GetModelInfo(id)->GetModelType() != MITYPE_VEHICLE) &&
	   (ms_aInfoForModel[id].m_flags & STREAMFLAGS_DONT_REMOVE) == 0){
		if(ms_aInfoForModel[id].m_loadState != STREAMSTATE_LOADED)
			RemoveModel(id);
		else if(ms_aInfoForModel[id].m_next == nil)
			ms_aInfoForModel[id].AddToList(&ms_startLoadedList);
	}
#ifdef WII
	ReleaseScriptOwnedTxdIfUnused(txdId);
#endif
}

void
CStreaming::LoadInitialPeds(void)
{
	RequestModel(MI_COP, STREAMFLAGS_DONT_REMOVE);
	RequestModel(MI_MALE01, STREAMFLAGS_DONT_REMOVE);
	RequestModel(MI_TAXI_D, STREAMFLAGS_DONT_REMOVE);
}

void
CStreaming::LoadInitialWeapons(void)
{
	CStreaming::RequestModel(MI_NIGHTSTICK, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_MISSILE, STREAMFLAGS_DONT_REMOVE);
}

void
CStreaming::LoadInitialVehicles(void)
{
	ms_numVehiclesLoaded = 0;
	ms_lastVehicleDeleted = 0;

	RequestModel(MI_POLICE, STREAMFLAGS_DONT_REMOVE);
}

void
CStreaming::StreamVehiclesAndPeds(void)
{
	int i, model;
	static int timeBeforeNextLoad = 0;
	static int modelQualityClass = 0;

	if(CRecordDataForGame::IsRecording() ||
	   CRecordDataForGame::IsPlayingBack()
#ifdef FIX_BUGS
	   || CReplay::IsPlayingBack()
#endif
		)
		return;

	if(FindPlayerPed()->m_pWanted->AreSwatRequired()){
		RequestModel(MI_ENFORCER, STREAMFLAGS_DONT_REMOVE);
		RequestModel(MI_SWAT, STREAMFLAGS_DONT_REMOVE);
	}else{
		SetModelIsDeletable(MI_ENFORCER);
		if(!HasModelLoaded(MI_ENFORCER))
			SetModelIsDeletable(MI_SWAT);
	}

	if(FindPlayerPed()->m_pWanted->AreFbiRequired()){
		RequestModel(MI_FBIRANCH, STREAMFLAGS_DONT_REMOVE);
		RequestModel(MI_FBI, STREAMFLAGS_DONT_REMOVE);
	}else{
		SetModelIsDeletable(MI_FBIRANCH);
		if(!HasModelLoaded(MI_FBIRANCH))
			SetModelIsDeletable(MI_FBI);
	}

	if(FindPlayerPed()->m_pWanted->AreArmyRequired()){
		RequestModel(MI_RHINO, STREAMFLAGS_DONT_REMOVE);
		RequestModel(MI_BARRACKS, STREAMFLAGS_DONT_REMOVE);
		RequestModel(MI_ARMY, STREAMFLAGS_DONT_REMOVE);
	}else{
		SetModelIsDeletable(MI_RHINO);
		SetModelIsDeletable(MI_BARRACKS);
		if(!HasModelLoaded(MI_RHINO) && !HasModelLoaded(MI_BARRACKS))
			SetModelIsDeletable(MI_ARMY);
	}

	if(FindPlayerPed()->m_pWanted->NumOfHelisRequired() > 0)
		RequestModel(MI_CHOPPER, STREAMFLAGS_DONT_REMOVE);
	else
		SetModelIsDeletable(MI_CHOPPER);

	if (FindPlayerPed()->m_pWanted->AreMiamiViceRequired()) {
		SetModelIsDeletable(MI_VICE1);
		SetModelIsDeletable(MI_VICE2);
		SetModelIsDeletable(MI_VICE3);
		SetModelIsDeletable(MI_VICE4);
		SetModelIsDeletable(MI_VICE5);
		SetModelIsDeletable(MI_VICE6);
		SetModelIsDeletable(MI_VICE7);
		SetModelIsDeletable(MI_VICE8);
		RequestModel(MI_VICECHEE, STREAMFLAGS_DONT_REMOVE);
		if(CPopulation::NumMiamiViceCops == 0)
			switch (CCarCtrl::MiamiViceCycle) {
			case 0:
				RequestModel(MI_VICE1, STREAMFLAGS_DONT_REMOVE);
				RequestModel(MI_VICE2, STREAMFLAGS_DONT_REMOVE);
				break;
			case 1:
				RequestModel(MI_VICE3, STREAMFLAGS_DONT_REMOVE);
				RequestModel(MI_VICE4, STREAMFLAGS_DONT_REMOVE);
				break;
			case 2:
				RequestModel(MI_VICE5, STREAMFLAGS_DONT_REMOVE);
				RequestModel(MI_VICE6, STREAMFLAGS_DONT_REMOVE);
				break;
			case 3:
				RequestModel(MI_VICE7, STREAMFLAGS_DONT_REMOVE);
				RequestModel(MI_VICE8, STREAMFLAGS_DONT_REMOVE);
				break;
			}
	}
	else {
		SetModelIsDeletable(MI_VICECHEE);
		SetModelIsDeletable(MI_VICE1);
		SetModelIsDeletable(MI_VICE2);
		SetModelIsDeletable(MI_VICE3);
		SetModelIsDeletable(MI_VICE4);
		SetModelIsDeletable(MI_VICE5);
		SetModelIsDeletable(MI_VICE6);
		SetModelIsDeletable(MI_VICE7);
		SetModelIsDeletable(MI_VICE8);
	}

	if(timeBeforeNextLoad >= 0)
		timeBeforeNextLoad--;
	else if(ms_numVehiclesLoaded <= desiredNumVehiclesLoaded){
		CZoneInfo zone;
		CVector coors = FindPlayerCoors();
		CTheZones::GetZoneInfoForTimeOfDay(&coors, &zone);
		int32 maxReq = -1;
		int32 mostRequestedRating = 0;
		for(i = 0; i < CCarCtrl::TOTAL_CUSTOM_CLASSES; i++){
			if(CCarCtrl::NumRequestsOfCarRating[i] > maxReq &&
				((i == 0 && zone.carThreshold[0] != 0) ||
#ifdef FIX_BUGS
				(i < CCarCtrl::NUM_CAR_CLASSES && zone.carThreshold[i] != zone.carThreshold[i-1]) ||
				(i == CCarCtrl::NUM_CAR_CLASSES && zone.boatThreshold[i - CCarCtrl::NUM_CAR_CLASSES] != 0) ||
				(i > CCarCtrl::NUM_CAR_CLASSES && i < CCarCtrl::TOTAL_CUSTOM_CLASSES && zone.boatThreshold[i - CCarCtrl::NUM_CAR_CLASSES] != zone.boatThreshold[i - CCarCtrl::NUM_CAR_CLASSES - 1]))) {
#else
				(i != 0 && zone.carThreshold[i] != zone.carThreshold[i-1]))) {
#endif
				maxReq = CCarCtrl::NumRequestsOfCarRating[i];
				mostRequestedRating = i;
			}
		}
		model = CCarCtrl::ChooseCarModelToLoad(mostRequestedRating);
		if(!HasModelLoaded(model)){
			RequestModel(model, STREAMFLAGS_DEPENDENCY);
			timeBeforeNextLoad = 350;
		}
		CCarCtrl::NumRequestsOfCarRating[mostRequestedRating] = 0;
	}
}

void
CStreaming::StreamZoneModels(const CVector &pos)
{
	int i, j;
	uint16 gangsToLoad, gangCarsToLoad, bit;
	CZoneInfo info;
	static int timeBeforeNextLoad = 0;

	CTheZones::GetZoneInfoForTimeOfDay(&pos, &info);

	if(info.pedGroup != ms_currentPedGrp){

		// unload pevious group
		if(ms_currentPedGrp != -1)
			for(i = 0; i < NUMMODELSPERPEDGROUP; i++){
				ms_bIsPedFromPedGroupLoaded[i] = false;
				if(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i] != -1){
					SetModelIsDeletable(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
					SetModelTxdIsDeletable(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
				}
			}

		ms_currentPedGrp = info.pedGroup;

		for(i = 0; i < MAXZONEPEDSLOADED; i++){
			do
				j = CGeneral::GetRandomNumberInRange(0, NUMMODELSPERPEDGROUP);
			while(ms_bIsPedFromPedGroupLoaded[j]);
			ms_bIsPedFromPedGroupLoaded[j] = true;
			if(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[j] != -1)
				RequestModel(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[j], STREAMFLAGS_DEPENDENCY);
		}
		ms_numPedsLoaded = MAXZONEPEDSLOADED;
		timeBeforeNextLoad = 300;
	}

	if(timeBeforeNextLoad >= 0)
		timeBeforeNextLoad--;
	else{
		// Switch a ped
		int oldMI;
		// Find a ped to unload
		for(i = 0; i < NUMMODELSPERPEDGROUP; i++)
			if(ms_bIsPedFromPedGroupLoaded[i]){
				oldMI = CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i];
				if(oldMI != -1 && CModelInfo::GetModelInfo(oldMI)->GetNumRefs() == 0)
					break;
			}
		// And load a new one
		if(i != NUMMODELSPERPEDGROUP || ms_numPedsLoaded < MAXZONEPEDSLOADED){
			do
				j = CGeneral::GetRandomNumberInRange(0, NUMMODELSPERPEDGROUP);
			while(ms_bIsPedFromPedGroupLoaded[j]);
			if(ms_numPedsLoaded == MAXZONEPEDSLOADED)
				ms_bIsPedFromPedGroupLoaded[i] = false;
			ms_bIsPedFromPedGroupLoaded[j] = true;
			int newMI = CPopulation::ms_pPedGroups[ms_currentPedGrp].models[j];
			if(newMI != oldMI){
				RequestModel(newMI, STREAMFLAGS_DEPENDENCY);
				debug("Request Ped %s\n", CModelInfo::GetModelInfo(newMI)->GetModelName());
				if(ms_numPedsLoaded == MAXZONEPEDSLOADED){
					SetModelIsDeletable(oldMI);
					SetModelTxdIsDeletable(oldMI);
					debug("Remove Ped %s\n", CModelInfo::GetModelInfo(oldMI)->GetModelName());
				}else
					ms_numPedsLoaded++;
				timeBeforeNextLoad = 300;
			}
		}
	}

	RequestModel(MI_MALE01, STREAMFLAGS_DONT_REMOVE);
	RequestModel(MI_TAXI_D, STREAMFLAGS_DONT_REMOVE);

	gangsToLoad = 0;
	gangCarsToLoad = 0;
	if(info.gangPedThreshold[0] != info.copPedThreshold)
		gangsToLoad = 1;
	for(i = 1; i < NUM_GANGS; i++)
		if(info.gangPedThreshold[i] != info.gangPedThreshold[i-1])
			gangsToLoad |= 1<<i;
	if(info.gangThreshold[0] != info.copThreshold)
		gangCarsToLoad = 1;
	for(i = 1; i < NUM_GANGS; i++)
		if(info.gangThreshold[i] != info.gangThreshold[i-1])
			gangCarsToLoad |= 1<<i;

	if(gangsToLoad == ms_loadedGangs && gangCarsToLoad == ms_loadedGangCars)
		return;

	int gangModelsToload = gangsToLoad | gangCarsToLoad;

	if(gangsToLoad != ms_loadedGangs || gangCarsToLoad != ms_loadedGangCars){
		for(i = 0; i < NUM_GANGS; i++){
			bit = 1<<i;

			if(gangModelsToload & bit && (ms_loadedGangs & bit) == 0){
				RequestModel(CGangs::GetGangPedModel1(i), STREAMFLAGS_DEPENDENCY);
				RequestModel(CGangs::GetGangPedModel2(i), STREAMFLAGS_DEPENDENCY);
				ms_loadedGangs |= bit;
			}else if((gangModelsToload & bit) == 0 && ms_loadedGangs & bit){
				SetModelIsDeletable(CGangs::GetGangPedModel1(i));
				SetModelIsDeletable(CGangs::GetGangPedModel2(i));
				SetModelTxdIsDeletable(CGangs::GetGangPedModel1(i));
				SetModelTxdIsDeletable(CGangs::GetGangPedModel2(i));
				ms_loadedGangs &= ~bit;
			}

			if(CGangs::GetGangVehicleModel(i) != -1){
				if((gangCarsToLoad & bit) && (ms_loadedGangCars & bit) == 0){
					RequestModel(CGangs::GetGangVehicleModel(i), STREAMFLAGS_DEPENDENCY);
				}else if((gangCarsToLoad & bit) == 0 && ms_loadedGangCars & bit){
					SetModelIsDeletable(CGangs::GetGangVehicleModel(i));
					SetModelTxdIsDeletable(CGangs::GetGangVehicleModel(i));
				}
			}
		}
		ms_loadedGangCars = gangCarsToLoad;
	}
}

void
CStreaming::RemoveCurrentZonesModels(void)
{
	int i;

	if (ms_currentPedGrp != -1)
		for (i = 0; i < NUMMODELSPERPEDGROUP; i++) {
			ms_bIsPedFromPedGroupLoaded[i] = false;
			if (CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i] != -1) {
				SetModelIsDeletable(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
				SetModelTxdIsDeletable(CPopulation::ms_pPedGroups[ms_currentPedGrp].models[i]);
			}
		}

	CStreaming::RequestModel(MI_MALE01, STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(MI_TAXI_D, STREAMFLAGS_DONT_REMOVE);

	for (i = 0; i < NUM_GANGS; i++) {
		if (CGangs::GetGangPedModel1(i) != -1) {
			SetModelIsDeletable(CGangs::GetGangPedModel1(i));
			SetModelTxdIsDeletable(CGangs::GetGangPedModel1(i));
		}
		if (CGangs::GetGangPedModel2(i) != -1) {
			SetModelIsDeletable(CGangs::GetGangPedModel2(i));
			SetModelTxdIsDeletable(CGangs::GetGangPedModel2(i));
		}
		if (CGangs::GetGangVehicleModel(i) != -1) {
			SetModelIsDeletable(CGangs::GetGangVehicleModel(i));
			SetModelTxdIsDeletable(CGangs::GetGangVehicleModel(i));
		}
	}

	ms_currentPedGrp = -1;
	ms_loadedGangs = 0;
	ms_loadedGangCars = 0;
}

void
CStreaming::LoadBigBuildingsWhenNeeded(void)
{
	// Very much like CCollision::Update and CCollision::LoadCollisionWhenINeedIt
#ifdef WII
	if(CCutsceneMgr::IsCutsceneProcessing() && gWiiIslandPhase == WII_ISLAND_IDLE)
		return;
#else
	if(CCutsceneMgr::IsCutsceneProcessing())
		return;
#endif

#ifdef WII
	CVector playerPosition = FindPlayerCoors();
	eLevelName crossedLevel = CTheZones::m_CurrLevel;
	uint32 now = WiiIslandWallClockMs();

	if(gWiiIslandPhase == WII_ISLAND_IDLE){
		if(crossedLevel != LEVEL_GENERIC && crossedLevel == CGame::currLevel){
			gWiiIslandLastStablePosition = playerPosition;
			gWiiIslandLastStableLevel = crossedLevel;
			gWiiIslandHasLastStablePosition = true;
		}
		CVector sourceReturnPosition =
			gWiiIslandHasLastStablePosition &&
			gWiiIslandLastStableLevel == CGame::currLevel ?
			gWiiIslandLastStablePosition : playerPosition;
		if(crossedLevel != LEVEL_GENERIC && crossedLevel != CGame::currLevel){
			WiiIslandBeginTransition(crossedLevel, playerPosition,
			                         sourceReturnPosition, true);
			WiiIslandEnterReadPhase();
		}else{
			eLevelName predicted = WiiIslandPredictedLevel(playerPosition);
			if(predicted != LEVEL_GENERIC)
				WiiIslandBeginTransition(predicted,
				                         playerPosition + FindPlayerSpeed() *
				                         WII_ISLAND_PREFETCH_DISTANCE,
				                         sourceReturnPosition, false);
		}
		return;
	}

	if(gWiiIslandPhase == WII_ISLAND_RETIRE){
		if(crossedLevel != LEVEL_GENERIC && crossedLevel != CGame::currLevel){
				SYS_Report("[WII-ISLAND] retire interrupted source=%d target=%d crossed=%d\n",
			           (int)gWiiIslandSourceLevel, (int)gWiiIslandTargetLevel,
			           (int)crossedLevel);
				WiiIslandReleaseRetireProtection("interrupted");
				WiiIslandTransitionReset();
			return;
		}
		if(!gWiiIslandRetireComplete && WiiIslandRetireOldLevelStep()){
			gWiiIslandRetireComplete = true;
			SYS_Report("[WII-ISLAND] retire complete source=%d target=%d dt=%ums\n",
			           (int)gWiiIslandSourceLevel, (int)gWiiIslandTargetLevel,
			           (unsigned)(now - gWiiIslandStartedAtMs));
		}

		if(gWiiIslandVisualHandoffPending){
			WiiIslandRefreshVisualHandoff(playerPosition);
			int32 missingRequired = 0;
			int32 missingOptional = 0;
			bool coreReady = WiiIslandWorkingSetReady(
				&missingRequired, &missingOptional);
			bool visualReady = coreReady;
			bool visualTimeout = gWiiIslandCommittedAtMs != 0 &&
				now - gWiiIslandCommittedAtMs >=
					WII_ISLAND_VISUAL_HANDOFF_TIMEOUT_MS;
			if(visualReady || visualTimeout){
				CStreaming::InstanceBigBuildings(gWiiIslandTargetLevel,
				                                 playerPosition);
				CStreaming::InstanceLoadedModels(playerPosition);
				gWiiIslandVisualHandoffPending = false;
				SYS_Report("[WII-ISLAND] visual handoff target=%d ready=%d timeout=%d required=%d optional=%d dt=%ums\n",
				           (int)gWiiIslandTargetLevel, visualReady ? 1 : 0,
				           visualTimeout ? 1 : 0, missingRequired,
				           missingOptional,
				           (unsigned)(now - gWiiIslandCommittedAtMs));
#if WII_STREAM_P7_BLOCKING_HANDOFF
				if(visualReady)
					WiiIslandReleaseRetireProtection("handoff");
#endif
			}
		}

		if(gWiiIslandRetireComplete && !gWiiIslandVisualHandoffPending){
			WiiIslandReleaseRetireProtection("complete");
			WiiIslandTransitionReset();
		}
		return;
	}

	bool targetStillPredicted = false;
	if(gWiiIslandPhase == WII_ISLAND_PREFETCH){
		eLevelName predicted = WiiIslandPredictedLevel(playerPosition);
		if(crossedLevel == gWiiIslandSourceLevel){
			gWiiIslandSourceReturnPosition = playerPosition;
			gWiiIslandLastStablePosition = playerPosition;
			gWiiIslandLastStableLevel = crossedLevel;
			gWiiIslandHasLastStablePosition = true;
		}
		if(crossedLevel == gWiiIslandTargetLevel){
			gWiiIslandWorkPosition = playerPosition;
			gWiiIslandLastSeenAtMs = now;
			WiiIslandEnterReadPhase();
		}else if(predicted == gWiiIslandTargetLevel){
			targetStillPredicted = true;
			gWiiIslandLastSeenAtMs = now;
		}else if(now - gWiiIslandLastSeenAtMs > 1000u){
			SYS_Report("[WII-ISLAND] cancel prefetch target=%d dt=%ums\n",
			           (int)gWiiIslandTargetLevel,
			           (unsigned)(now - gWiiIslandStartedAtMs));
			WiiIslandReleaseTemporaryPins(false, true);
			WiiIslandTransitionReset();
			return;
		}
	}

	WiiIslandRequestWorkingSet();
	int32 missingRequired = 0;
	int32 missingOptional = 0;
	bool workingSetReady = WiiIslandWorkingSetReady(&missingRequired,
	                                               &missingOptional);
	bool shouldPrepareSplash =
		gWiiIslandPhase == WII_ISLAND_PREFETCH && targetStillPredicted &&
		missingRequired == 0;
	if(gWiiIslandPhase == WII_ISLAND_READ &&
	   now - gWiiIslandReadStartedAtMs >= WII_ISLAND_SPLASH_READ_DELAY_MS)
		shouldPrepareSplash = true;
	if(shouldPrepareSplash && !gWiiIslandSplashPrepareAttempted){
		gWiiIslandSplashPrepareAttempted = true;
		gWiiIslandSplashPrepared =
			WiiPrepareIslandTransitionSplash(gWiiIslandTargetLevel);
		SYS_Report("[WII-ISLAND] splash prepared target=%d phase=%s ready=%d dt=%ums\n",
		           (int)gWiiIslandTargetLevel,
		           WiiIslandTransitionPhaseName(),
		           gWiiIslandSplashPrepared ? 1 : 0,
		           (unsigned)(now - gWiiIslandStartedAtMs));
	}
	bool splashWindowReady = true;
#if WII_STREAM_SPLASH_VISUAL_GATE
	splashWindowReady = now - gWiiIslandReadStartedAtMs >=
	                     WII_ISLAND_SPLASH_MIN_DISPLAY_MS;
#endif
	if(gWiiIslandPhase == WII_ISLAND_READ && workingSetReady &&
	   splashWindowReady){
		if(WiiIslandCommitTransition())
			return;
	}
	if(gWiiIslandPhase == WII_ISLAND_PREFETCH && workingSetReady &&
	   !gWiiIslandPrefetchReadyLogged){
		gWiiIslandPrefetchReadyLogged = true;
		SYS_Report("[WII-ISLAND] prefetch ready target=%d dt=%ums; waiting for boundary\n",
		           (int)gWiiIslandTargetLevel,
		           (unsigned)(now - gWiiIslandStartedAtMs));
	}

	uint32 elapsed = gWiiIslandPhase == WII_ISLAND_READ ?
	                 now - gWiiIslandReadStartedAtMs :
	                 now - gWiiIslandStartedAtMs;
	if(gWiiIslandPhase == WII_ISLAND_READ &&
	   elapsed >= WII_ISLAND_ABORT_TIMEOUT_MS){
		WiiIslandAbortTransition(missingRequired, missingOptional);
		return;
	}else if(gWiiIslandPhase == WII_ISLAND_READ &&
	         elapsed >= WII_ISLAND_HARD_TIMEOUT_MS &&
	         missingRequired == 0 &&
	         (ms_channelError == -1 ||
	          WiiIslandDiscardOptionalRadarChannelError())){
		gWiiIslandHardFallbackUsed = true;
	#if WII_STREAM_MEMORY_DIAGNOSTICS
		gWiiPhase0HardFallbackCount++;
	#endif
		SYS_Report("[WII-ISLAND] optional timeout commit target=%d optional=%d channelError=%d dt=%ums\n",
		           (int)gWiiIslandTargetLevel, missingOptional,
		           ms_channelError, (unsigned)elapsed);
		if(WiiIslandCommitTransition())
			return;
	}else if(elapsed >= WII_ISLAND_HARD_TIMEOUT_MS && ms_channelError == -1 &&
	   gWiiIslandPhase == WII_ISLAND_READ &&
	   !gWiiIslandHardFallbackUsed){
		gWiiIslandHardFallbackUsed = true;
	#if WII_STREAM_MEMORY_DIAGNOSTICS
		gWiiPhase0HardFallbackCount++;
	#endif
		SYS_Report("[WII-ISLAND] hard fallback target=%d required=%d optional=%d dt=%ums\n",
		           (int)gWiiIslandTargetLevel, missingRequired,
		           missingOptional, (unsigned)elapsed);
		CStreaming::LoadAllRequestedModels(true);
		if(WiiIslandWorkingSetReady(&missingRequired, &missingOptional)){
			if(WiiIslandCommitTransition())
				return;
		}
		else if(missingRequired == 0){
			SYS_Report("[WII-ISLAND] degraded commit target=%d optional-missing=%d\n",
			           (int)gWiiIslandTargetLevel, missingOptional);
			if(WiiIslandCommitTransition())
				return;
		}
	}else if(gWiiIslandPhase == WII_ISLAND_READ &&
	         elapsed >= WII_ISLAND_SOFT_TIMEOUT_MS &&
	         (elapsed - WII_ISLAND_SOFT_TIMEOUT_MS) % 1000u < 34u){
		SYS_Report("[WII-ISLAND] waiting phase=%s target=%d required=%d optional=%d dt=%ums pending=%d priority=%d\n",
		           WiiIslandTransitionPhaseName(), (int)gWiiIslandTargetLevel,
		           missingRequired, missingOptional, (unsigned)elapsed,
		           ms_numModelsRequested, ms_numPriorityRequests);
	}
	return;
#endif

	if(CTheZones::m_CurrLevel == LEVEL_GENERIC ||
	   CTheZones::m_CurrLevel == CGame::currLevel)
		return;

	CTimer::Suspend();
	CGame::currLevel = CTheZones::m_CurrLevel;
	ISLAND_LOADING_IS(LOW)
	{
		DMAudio.SetEffectsFadeVol(0);
		CPad::StopPadsShaking();
		CCollision::LoadCollisionScreen(CGame::currLevel);
		DMAudio.Service();

		RemoveUnusedBigBuildings(CGame::currLevel);
		RemoveUnusedBuildings(CGame::currLevel);
		RemoveUnusedModelsInLoadedList();
		CGame::TidyUpMemory(true, true);
	}
	CReplay::EmptyReplayBuffer();
	if(CGame::currLevel != LEVEL_GENERIC)
		LoadSplash(GetLevelSplashScreen(CGame::currLevel));

	ISLAND_LOADING_IS(LOW)
		CStreaming::RequestBigBuildings(CGame::currLevel, TheCamera.GetPosition());
#ifdef NO_ISLAND_LOADING
	else if(FrontEndMenuManager.m_PrefsIslandLoading == CMenuManager::ISLAND_LOADING_MEDIUM) {
		RemoveIslandsNotUsed(CGame::currLevel);
		CStreaming::RequestIslands(CGame::currLevel);
	}
#endif

	CStreaming::LoadAllRequestedModels(false);

	CGame::TidyUpMemory(true, true);
	CTimer::Resume();

	ISLAND_LOADING_IS(LOW)
		DMAudio.SetEffectsFadeVol(127);
}

#ifdef WII
bool
CStreaming::IsIslandTransitionActive(void)
{
	return gWiiIslandPhase != WII_ISLAND_IDLE;
}

bool
CStreaming::IsIslandTransitionBlocking(void)
{
	return gWiiIslandPhase == WII_ISLAND_READ ||
	       gWiiIslandPhase == WII_ISLAND_COMMIT;
}

bool
CStreaming::IsIslandTransitionConversionBudgetActive(void)
{
	return WiiIslandTransitionConversionBudgetActive();
}

bool
CStreaming::ShouldSuppressIslandLOD(int32 modelId)
{
	if(modelId != islandLODmainland && modelId != islandLODbeach)
		return false;
	#ifdef WII
	#if WII_STREAM_ATOMIC_BIG_HANDOFF
	if(gWiiIslandPhase != WII_ISLAND_IDLE && !gWiiIslandAtomicBigReady &&
	   ((gWiiIslandTargetLevel == LEVEL_MAINLAND &&
	     modelId == islandLODmainland) ||
	    (gWiiIslandTargetLevel == LEVEL_BEACH &&
	     modelId == islandLODbeach)))
		return false;
	#endif
	if(gWiiIslandVisualHandoffPending &&
	   ((gWiiIslandTargetLevel == LEVEL_MAINLAND &&
	     modelId == islandLODmainland) ||
	    (gWiiIslandTargetLevel == LEVEL_BEACH &&
	     modelId == islandLODbeach)))
		return false;
#endif
#ifdef NO_ISLAND_LOADING
	if(FrontEndMenuManager.m_PrefsIslandLoading ==
	   CMenuManager::ISLAND_LOADING_HIGH)
		return true;
#endif
	return (CGame::currLevel == LEVEL_MAINLAND && modelId == islandLODmainland) ||
	       (CGame::currLevel == LEVEL_BEACH && modelId == islandLODbeach);
}

bool
CStreaming::ShouldKeepColForIslandTransition(int32 col)
{
	return col >= 0 && col < COLSTORESIZE &&
	       gWiiIslandPhase != WII_ISLAND_IDLE &&
	       gWiiIslandProtectedCols[col] != 0;
}

bool
CStreaming::ShouldKeepRadarTxdForIslandTransition(int32 txd)
{
	return txd >= 0 && txd < TXDSTORESIZE &&
	       (gWiiIslandPhase == WII_ISLAND_PREFETCH ||
	        gWiiIslandPhase == WII_ISLAND_READ ||
	        gWiiIslandPhase == WII_ISLAND_COMMIT ||
	        (gWiiIslandPhase == WII_ISLAND_RETIRE &&
	         gWiiIslandRetireProtectionActive)) &&
	       gWiiIslandProtectedRadarTxds[txd] != 0;
}
#endif


// Find starting offset of the cdimage we next want to read
// Not useful at all on PC...
int32
CStreaming::GetCdImageOffset(int32 lastPosn)
{
	int offset, off;
	int i, img;
	int dist, mindist;

	img = -1;
	mindist = INT32_MAX;
	offset = ms_imageOffsets[ms_lastImageRead];
	if(lastPosn <= offset || lastPosn > offset + ms_imageSize){
		// last read position is not in last image
		for(i = 0; i < NUMCDIMAGES; i++){
			off = ms_imageOffsets[i];
			if(off == -1) continue;
			if((uint32)lastPosn > (uint32)off)
				// after start of image, get distance from end
				// negative if before end!
				dist = lastPosn - (off + ms_imageSize);
			else
				// before image, get offset to start
				// this will never be negative
				dist = off - lastPosn;
			if(dist < mindist){
				img = i;
				mindist = dist;
			}
		}
		assert(img >= 0);
		offset = ms_imageOffsets[img];
		ms_lastImageRead = img;
	}
	return offset;
}

inline bool
StreamingDependencyNotReady(int32 streamId)
{
	// A dependency that is only being read cannot be consumed yet. Allowing a
	// model onto the other channel here makes it reach conversion before its
	// TXD or animation block is fully available.
	return CStreaming::ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_LOADED;
}

inline bool TxdNotLoaded(int32 txdId) { return StreamingDependencyNotReady(txdId + STREAM_OFFSET_TXD); }
inline bool AnimNotLoaded(int32 animId) { return animId != -1 && StreamingDependencyNotReady(animId + STREAM_OFFSET_ANIM); }

static void
AddDependencyCandidate(int32 streamId, int32 lastPosn,
	int32 &streamIdFirst, uint32 &posnFirst,
	int32 &streamIdNext, uint32 &posnNext)
{
	CStreamingInfo *si = &CStreaming::ms_aInfoForModel[streamId];
	uint32 posn, size;
	if(si->m_loadState != STREAMSTATE_INQUEUE ||
	   (streamId >= STREAM_OFFSET_ANIM && CCutsceneMgr::IsCutsceneProcessing()) ||
	   !si->GetCdPosnAndSize(posn, size))
		return;
	if(posn < posnFirst){
		streamIdFirst = streamId;
		posnFirst = posn;
	}
	if(posn < posnNext && posn >= (uint32)lastPosn){
		streamIdNext = streamId;
		posnNext = posn;
	}
}

#ifdef WII
static void
AddAgedCandidate(int32 streamId, uint32 effectiveWait,
	int32 &candidateId, uint32 &candidateWait)
{
	if(streamId < 0 || streamId >= NUMSTREAMINFO)
		return;
	CStreamingInfo *si = &CStreaming::ms_aInfoForModel[streamId];
	uint32 posn, size;
	if(si->m_loadState != STREAMSTATE_INQUEUE ||
	   (streamId >= STREAM_OFFSET_ANIM && CCutsceneMgr::IsCutsceneProcessing()) ||
	   !si->GetCdPosnAndSize(posn, size))
		return;
	if(candidateId == -1 || effectiveWait > candidateWait){
		candidateId = streamId;
		candidateWait = effectiveWait;
	}
}

static bool
WiiStreamIsForegroundRequest(int32 streamId)
{
	return streamId >= 0 && streamId < NUMSTREAMINFO &&
	       (CStreaming::ms_aInfoForModel[streamId].IsPriority() ||
	        gWiiStreamRequestClass[streamId] ==
	            WII_STREAM_REQUEST_WORLD_VISIBLE);
}

static bool
WiiStreamHasForegroundRequest(void)
{
	for(CStreamingInfo *si = CStreaming::ms_startRequestedList.m_next;
	    si != &CStreaming::ms_endRequestedList; si = si->m_next){
		int32 streamId = si - CStreaming::ms_aInfoForModel;
		if(WiiStreamIsForegroundRequest(streamId))
			return true;
	}
	return false;
}
#endif

// Find stream id of next requested file in cdimage
int32
CStreaming::GetNextFileOnCd(int32 lastPosn, bool priority)
{
	CStreamingInfo *si, *next;
	int streamId;
	uint32 posn, size;
	int streamIdFirst, streamIdNext;
	uint32 posnFirst, posnNext;
	int dependencyIdFirst, dependencyIdNext;
	uint32 dependencyPosnFirst, dependencyPosnNext;
#ifdef WII
	bool priorityQueue = priority &&
	                     (ms_numPriorityRequests != 0 ||
	                      WiiStreamHasForegroundRequest());
	if(!priorityQueue)
		gWiiStreamForegroundServicesSinceFair = 0;
	int agedCandidate;
	uint32 agedCandidateWait;
	int fallbackIdFirst, fallbackIdNext;
	uint32 fallbackPosnFirst, fallbackPosnNext;
	int fallbackDependencyIdFirst, fallbackDependencyIdNext;
	uint32 fallbackDependencyPosnFirst, fallbackDependencyPosnNext;
	int radarCandidate;
	uint32 radarCandidateWait;
	bool serveFair = !priorityQueue ||
	                 gWiiStreamForegroundServicesSinceFair >=
	                     WII_STREAM_FOREGROUND_BURST;
	uint32 now = WiiStreamNowMs();
#else
	bool priorityQueue = priority && ms_numPriorityRequests != 0;
#endif

	streamIdFirst = -1;
	streamIdNext = -1;
	posnFirst = UINT32_MAX;
	posnNext = UINT32_MAX;
	dependencyIdFirst = -1;
	dependencyIdNext = -1;
	dependencyPosnFirst = UINT32_MAX;
	dependencyPosnNext = UINT32_MAX;
#ifdef WII
	agedCandidate = -1;
	agedCandidateWait = 0;
	fallbackIdFirst = -1;
	fallbackIdNext = -1;
	fallbackPosnFirst = UINT32_MAX;
	fallbackPosnNext = UINT32_MAX;
	fallbackDependencyIdFirst = -1;
	fallbackDependencyIdNext = -1;
	fallbackDependencyPosnFirst = UINT32_MAX;
	fallbackDependencyPosnNext = UINT32_MAX;
	radarCandidate = -1;
	radarCandidateWait = 0;
#endif

	for(si = ms_startRequestedList.m_next; si != &ms_endRequestedList; si = next){
		next = si->m_next;
		streamId = si - ms_aInfoForModel;
		bool isPriority = si->IsPriority();
#ifdef WII
		bool isWorldVisible =
			gWiiStreamRequestClass[streamId] == WII_STREAM_REQUEST_WORLD_VISIBLE;
		bool isForeground = isPriority || isWorldVisible;
		bool isTransitionPrefetch =
			gWiiStreamRequestClass[streamId] ==
			WII_STREAM_REQUEST_TRANSITION_PREFETCH;
		uint32 queuedAt = gWiiStreamQueuedAtMs[streamId];
		uint32 queuedWait = now != 0 && queuedAt != 0 ? now - queuedAt : 0;
		bool aged = !isForeground && WiiStreamIsAgedFairRequest(streamId, si, now);
		bool prefetchGrant = priority && serveFair && !isForeground &&
		                     isTransitionPrefetch;
		bool fairCandidate = aged || prefetchGrant;
		bool fallback = priorityQueue && !isForeground && serveFair &&
		                !fairCandidate;
		if(WiiStreamIsRadarRequest(streamId) &&
		   queuedWait >= WII_STREAM_RADAR_DEADLINE_MS &&
		   (radarCandidate == -1 || queuedWait > radarCandidateWait)){
			radarCandidate = streamId;
			radarCandidateWait = queuedWait;
		}
#endif

		// Under priority back-pressure, the fair lane first serves the oldest
		// expired request, then falls back to one ordinary request per burst.
#ifdef WII
		if(priorityQueue && !isForeground && !fairCandidate && !fallback)
			continue;
#else
		if(priorityQueue && !isPriority)
			continue;
#endif

		// request Txds or anims if necessary
		if(streamId < STREAM_OFFSET_TXD){
			int txdId = CModelInfo::GetModelInfo(streamId)->GetTxdSlot();
			if(TxdNotLoaded(txdId)){
			#ifdef WII
				int32 txdStreamId = txdId + STREAM_OFFSET_TXD;
				WiiIslandRequeueModel(txdStreamId,
				                      ms_aInfoForModel[txdStreamId].m_flags);
			#else
				ReRequestTxd(txdId);
			#endif
				if(priorityQueue && isForeground)
					AddDependencyCandidate(txdId + STREAM_OFFSET_TXD, lastPosn,
						dependencyIdFirst, dependencyPosnFirst,
						dependencyIdNext, dependencyPosnNext);
#ifdef WII
				else if(fairCandidate)
					AddAgedCandidate(txdId + STREAM_OFFSET_TXD, queuedWait,
						agedCandidate, agedCandidateWait);
				else if(fallback)
					AddDependencyCandidate(txdId + STREAM_OFFSET_TXD, lastPosn,
						fallbackDependencyIdFirst, fallbackDependencyPosnFirst,
						fallbackDependencyIdNext, fallbackDependencyPosnNext);
#endif
				continue;
			}
			int animId = CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex();
			if(AnimNotLoaded(animId)){
			#ifdef WII
				int32 animStreamId = animId + STREAM_OFFSET_ANIM;
				WiiIslandRequeueModel(animStreamId,
				                      ms_aInfoForModel[animStreamId].m_flags);
			#else
				ReRequestAnim(animId);
			#endif
				if(priorityQueue && isForeground)
					AddDependencyCandidate(animId + STREAM_OFFSET_ANIM, lastPosn,
						dependencyIdFirst, dependencyPosnFirst,
						dependencyIdNext, dependencyPosnNext);
#ifdef WII
				else if(fairCandidate)
					AddAgedCandidate(animId + STREAM_OFFSET_ANIM, queuedWait,
						agedCandidate, agedCandidateWait);
				else if(fallback)
					AddDependencyCandidate(animId + STREAM_OFFSET_ANIM, lastPosn,
						fallbackDependencyIdFirst, fallbackDependencyPosnFirst,
						fallbackDependencyIdNext, fallbackDependencyPosnNext);
#endif
				continue;
			}
		}else if(streamId >= STREAM_OFFSET_ANIM && CCutsceneMgr::IsCutsceneProcessing())
			continue;

		if(ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size)){
#ifdef WII
			if(fairCandidate)
				AddAgedCandidate(streamId, queuedWait,
					agedCandidate, agedCandidateWait);
#endif
			int32 *candidateFirst = &streamIdFirst;
			int32 *candidateNext = &streamIdNext;
			uint32 *candidatePosnFirst = &posnFirst;
			uint32 *candidatePosnNext = &posnNext;
#ifdef WII
			if(priorityQueue && !isForeground){
				if(fairCandidate)
					continue;
				else{
					candidateFirst = &fallbackIdFirst;
					candidateNext = &fallbackIdNext;
					candidatePosnFirst = &fallbackPosnFirst;
					candidatePosnNext = &fallbackPosnNext;
				}
			}
#endif
			if(posn < *candidatePosnFirst){
				// find first requested file in image
				*candidateFirst = streamId;
				*candidatePosnFirst = posn;
			}
			if(posn < *candidatePosnNext && posn >= (uint32)lastPosn){
				// find first requested file after last read position
				*candidateNext = streamId;
				*candidatePosnNext = posn;
			}
		}else{
			// empty file
			DecrementRef(streamId);
			si->RemoveFromList();
			si->m_loadState = STREAMSTATE_LOADED;
		}
	}

	// wrap around
	if(streamIdNext == -1)
		streamIdNext = streamIdFirst;

#ifdef WII
	int32 priorityDependencyCandidate = dependencyIdNext != -1 ?
	                                      dependencyIdNext : dependencyIdFirst;
	if(radarCandidate != -1)
		return WiiStreamSelectDispatch(radarCandidate,
			WII_STREAM_DISPATCH_PRIORITY_CHAIN, radarCandidateWait);
	bool priorityServiceAvailable = streamIdNext != -1 ||
	                                priorityDependencyCandidate != -1;
	if(agedCandidate != -1 && (serveFair || !priorityServiceAvailable))
		return WiiStreamSelectDispatch(agedCandidate,
			WII_STREAM_DISPATCH_FAIR, agedCandidateWait);
	uint8 dispatchClass = WII_STREAM_DISPATCH_NORMAL;
	if(priorityQueue){
		int32 fallbackCandidate = fallbackIdNext != -1 ?
			fallbackIdNext : fallbackIdFirst;
		if(fallbackCandidate == -1)
			fallbackCandidate = fallbackDependencyIdNext != -1 ?
				fallbackDependencyIdNext : fallbackDependencyIdFirst;
		if(serveFair && streamIdNext != -1 && fallbackCandidate != -1)
			return WiiStreamSelectDispatch(fallbackCandidate,
				WII_STREAM_DISPATCH_FAIR, 0);
		if(streamIdNext == -1 && serveFair){
			streamIdNext = fallbackCandidate;
			if(streamIdNext != -1)
				dispatchClass = WII_STREAM_DISPATCH_FAIR;
		}
		if(streamIdNext != -1 && dispatchClass == WII_STREAM_DISPATCH_NORMAL &&
		   (ms_aInfoForModel[streamIdNext].IsPriority() ||
		    gWiiStreamRequestClass[streamIdNext] ==
		        WII_STREAM_REQUEST_WORLD_VISIBLE))
			dispatchClass = WII_STREAM_DISPATCH_PRIORITY_CHAIN;
	}
	if(streamIdNext != -1)
		return WiiStreamSelectDispatch(streamIdNext, dispatchClass, 0);
	if(priorityDependencyCandidate != -1)
		return WiiStreamSelectDispatch(priorityDependencyCandidate,
			WII_STREAM_DISPATCH_PRIORITY_CHAIN, 0);
#else
	if(streamIdNext == -1 && priorityQueue){
		streamIdNext = dependencyIdNext;
		if(streamIdNext == -1)
			streamIdNext = dependencyIdFirst;
	}
#endif

#ifdef WII
	return streamIdNext;
#else
	return streamIdNext;
#endif
}

/*
 * Streaming buffer size is half of the largest file.
 * Files larger than the buffer size can only be loaded by channel 0,
 * which then uses both buffers, while channel 1 is idle.
 * ms_bLoadingBigModel is set to true to indicate this state.
 */

// Make channel read from disc
void
CStreaming::RequestModelStream(int32 ch)
{
	int lastPosn, imgOffset, streamId;
	int totalSize;
	uint32 posn, size, unused;
	int i;
	int haveBigFile, havePed;

	lastPosn = CdStreamGetLastPosn();
	imgOffset = GetCdImageOffset(lastPosn);
	streamId = GetNextFileOnCd(lastPosn - imgOffset, true);

	// remove Txds and Anims that aren't requested anymore
	while(streamId != -1){
		if(ms_aInfoForModel[streamId].m_flags & STREAMFLAGS_KEEP_IN_MEMORY)
			break;
		if(streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL){
			if(IsTxdUsedByRequestedModels(streamId - STREAM_OFFSET_TXD))
				break;
		}else if(streamId >= STREAM_OFFSET_ANIM){
			assert(streamId < NUMSTREAMINFO);
			if(AreAnimsUsedByRequestedModels(streamId - STREAM_OFFSET_ANIM))
				break;
		}else
			break;
		RemoveModel(streamId);
		// so try next file
		ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size);
		streamId = GetNextFileOnCd(posn + size, true);
	}

	if(streamId == -1)
		return;

	ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size);
	if(size > (uint32)ms_streamingBufferSize){
		// Can only load big models on channel 0, and 1 has to be idle
		if(ch == 1 || ms_channel[1].state != CHANNELSTATE_IDLE)
			return;
		ms_bLoadingBigModel = true;
	}

	// Load up to 4 adjacent files
	haveBigFile = 0;
	havePed = 0;
	totalSize = 0;
#ifdef WII
	bool foregroundBatch = WiiStreamIsForegroundRequest(streamId);
	bool foregroundPressure = WiiStreamHasForegroundRequest();
#endif
	for(i = 0; i < 4; i++){
		// no more files we can read
		if(streamId == -1 || ms_aInfoForModel[streamId].m_loadState != STREAMSTATE_INQUEUE)
			break;
	#ifdef WII
		if(i > 0 && WiiIslandTransitionConversionBudgetActive())
			break;
	#endif

		// Stop at ordinary adjacent files. The first item may be a dependency
		// fallback when every priority request is temporarily blocked.
		ms_aInfoForModel[streamId].GetCdPosnAndSize(unused, size);
	#ifdef WII
		if(i > 0 &&
		   ((foregroundBatch && !WiiStreamIsForegroundRequest(streamId)) ||
		    (!foregroundBatch && foregroundPressure)))
			break;
	#else
		if(i > 0 && ms_numPriorityRequests != 0 && !ms_aInfoForModel[streamId].IsPriority())
			break;
	#endif

		// Can't load certain combinations of files together
		if(streamId < STREAM_OFFSET_TXD){
			if (havePed && CModelInfo::GetModelInfo(streamId)->GetModelType() == MITYPE_PED ||
			    haveBigFile && CModelInfo::GetModelInfo(streamId)->GetModelType() == MITYPE_VEHICLE ||
			    TxdNotLoaded(CModelInfo::GetModelInfo(streamId)->GetTxdSlot()) ||
			    AnimNotLoaded(CModelInfo::GetModelInfo(streamId)->GetAnimFileIndex()))
				break;
		}else{
			if(haveBigFile && size > 200)
				break;
		}

		// Now add the file
		ms_channel[ch].streamIds[i] = streamId;
		ms_channel[ch].offsets[i] = totalSize;
		totalSize += size;

		// To big for buffer, remove again
		if(totalSize > ms_streamingBufferSize && i > 0){
			totalSize -= size;
			break;
		}
		if(streamId < STREAM_OFFSET_TXD){
			if (CModelInfo::GetModelInfo(streamId)->GetModelType() == MITYPE_PED)
				havePed = 1;
			if (CModelInfo::GetModelInfo(streamId)->GetModelType() == MITYPE_VEHICLE)
				haveBigFile = 1;
		}else{
			if(size > 200)
				haveBigFile = 1;
		}
	#ifdef WII
		WiiStreamOnRequestDispatched(streamId);
	#endif
		ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_READING;
		ms_aInfoForModel[streamId].RemoveFromList();
		DecrementRef(streamId);

		streamId = ms_aInfoForModel[streamId].m_nextID;
	}

	// clear remaining slots
	for(; i < 4; i++)
		ms_channel[ch].streamIds[i] = -1;
	// Now read the data
	assert(!(ms_bLoadingBigModel && ch == 1));	// this would clobber the buffer
	if(CdStreamRead(ch, ms_pStreamingBuffer[ch], imgOffset+posn, totalSize) == STREAM_NONE)
		debug("FUCKFUCKFUCK\n");
	ms_channel[ch].state = CHANNELSTATE_READING;
	ms_channel[ch].field24 = 0;
	ms_channel[ch].size = totalSize;
	ms_channel[ch].position = imgOffset+posn;
	ms_channel[ch].numTries = 0;
}

// Load data previously read from disc
bool
CStreaming::ProcessLoadingChannel(int32 ch)
{
	int status;
	int i, id, cdsize;

	status = CdStreamGetStatus(ch);
	if(status != STREAM_NONE){
		// busy
		if(status != STREAM_READING && status != STREAM_WAITING){
			ms_channelError = ch;
			ms_channel[ch].state = CHANNELSTATE_ERROR;
			ms_channel[ch].status = status;
		}
		return false;
	}

	if(ms_channel[ch].state == CHANNELSTATE_STARTED){
		ms_channel[ch].state = CHANNELSTATE_IDLE;
		FinishLoadingLargeFile(&ms_pStreamingBuffer[ch][ms_channel[ch].offsets[0]*CDSTREAM_SECTOR_SIZE],
			ms_channel[ch].streamIds[0]);
		ms_channel[ch].streamIds[0] = -1;
	}else{
		ms_channel[ch].state = CHANNELSTATE_IDLE;
		for(i = 0; i < 4; i++){
			id = ms_channel[ch].streamIds[i];
			if(id == -1)
				continue;
		#ifdef WII
			if(i > 0 && WiiIslandTransitionConversionBudgetActive()){
				WiiStreamRequeueDispatched(id, "island-convert-budget");
				ms_channel[ch].streamIds[i] = -1;
				continue;
			}
		#endif

			cdsize = ms_aInfoForModel[id].GetCdSize();
			if(id < STREAM_OFFSET_TXD && CModelInfo::GetModelInfo(id)->GetModelType() == MITYPE_VEHICLE &&
			   ms_numVehiclesLoaded >= desiredNumVehiclesLoaded &&
			   !RemoveLoadedVehicle() &&
			   (CanRemoveModel(id) || GetAvailableVehicleSlot() == -1)){
				// can't load vehicle
				if(!CanRemoveModel(id)){
				#ifdef WII
					WiiStreamRequeueDispatched(id, "vehicle-slot");
				#else
					RemoveModel(id);
					ReRequestModel(id);
				#endif
				}else{
					RemoveModel(id);
					if(CTxdStore::GetNumRefs(CModelInfo::GetModelInfo(id)->GetTxdSlot()) == 0)
						RemoveTxd(CModelInfo::GetModelInfo(id)->GetTxdSlot());
				}
			}else{
			#ifdef WII
				if(!MakeSpaceFor(cdsize * CDSTREAM_SECTOR_SIZE)){
					WiiStreamRequeueDispatched(id, "same-frame-pressure");
					ms_channel[ch].streamIds[i] = -1;
					continue;
				}
			#else
				MakeSpaceFor(cdsize * CDSTREAM_SECTOR_SIZE);
			#endif
				ConvertBufferToObject(&ms_pStreamingBuffer[ch][ms_channel[ch].offsets[i]*CDSTREAM_SECTOR_SIZE],
					id);
				if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_STARTED){
					// queue for second part
					ms_channel[ch].state = CHANNELSTATE_STARTED;
					ms_channel[ch].offsets[0] = ms_channel[ch].offsets[i];
					ms_channel[ch].streamIds[0] = id;
					if(i != 0)
						ms_channel[ch].streamIds[i] = -1;
				}else
					ms_channel[ch].streamIds[i] = -1;
			}
		}
	}

	if(ms_bLoadingBigModel && ms_channel[ch].state != CHANNELSTATE_STARTED){
		ms_bLoadingBigModel = false;
		// reset channel 1 after loading a big model
		for(i = 0; i < 4; i++)
			ms_channel[1].streamIds[i] = -1;
		ms_channel[1].state = CHANNELSTATE_IDLE;
	}

	return true;
}

void
CStreaming::RetryLoadFile(int32 ch)
{
	Const char *key;

	CPad::StopPadsShaking();

	if(ms_channel[ch].numTries >= 3){
		switch(ms_channel[ch].status){
		case STREAM_ERROR_NOCD: key = "NOCD"; break;
		case STREAM_ERROR_OPENCD: key = "OPENCD"; break;
		case STREAM_ERROR_WRONGCD: key = "WRONGCD"; break;
		default: key = "CDERROR"; break;
		}
		printf("[STREAM-ERR] show %s ch=%d status=0x%02X tries=%d state=%d pos=0x%08X sectors=%d ids=%d/%d/%d/%d\n",
		       key, ch, ms_channel[ch].status & 0xFF, ms_channel[ch].numTries,
		       ms_channel[ch].state, ms_channel[ch].position, ms_channel[ch].size,
		       ms_channel[ch].streamIds[0], ms_channel[ch].streamIds[1],
		       ms_channel[ch].streamIds[2], ms_channel[ch].streamIds[3]);
		CHud::SetMessage(TheText.Get(key));
		CTimer::SetCodePause(true);
	}

	switch(ms_channel[ch].state){
	case CHANNELSTATE_ERROR:
		ms_channel[ch].numTries++;
	#if defined(WII) && WII_STREAM_MEMORY_DIAGNOSTICS
		gWiiPhase0RequestRetryCount++;
	#endif
		printf("[STREAM-ERR] retry ch=%d try=%d status=0x%02X pos=0x%08X sectors=%d ids=%d/%d/%d/%d\n",
		       ch, ms_channel[ch].numTries, ms_channel[ch].status & 0xFF,
		       ms_channel[ch].position, ms_channel[ch].size,
		       ms_channel[ch].streamIds[0], ms_channel[ch].streamIds[1],
		       ms_channel[ch].streamIds[2], ms_channel[ch].streamIds[3]);
		if (CdStreamGetStatus(ch) == STREAM_READING || CdStreamGetStatus(ch) == STREAM_WAITING) break;
	case CHANNELSTATE_IDLE:
		CdStreamRead(ch, ms_pStreamingBuffer[ch], ms_channel[ch].position, ms_channel[ch].size);
		ms_channel[ch].state = CHANNELSTATE_READING;
		ms_channel[ch].field24 = -600;
		break;
	case CHANNELSTATE_READING:
		if(ProcessLoadingChannel(ch)){
			ms_channelError = -1;
		#ifdef WII
			if(!IsIslandTransitionBlocking())
				CTimer::SetCodePause(false);
		#else
			CTimer::SetCodePause(false);
		#endif
		}
		break;
	}
}

void
CStreaming::LoadRequestedModels(void)
{
	static int currentChannel = 0;

	// GC: prevent re-entry — processing a loaded model/anim can trigger
	// more streaming requests (e.g. model needs its anim loaded first),
	// which would call us again before the current frame is done
	static bool s_bInsideLoadRequested = false;
	if (s_bInsideLoadRequested) return;
	s_bInsideLoadRequested = true;

	// We can't read with channel 1 while channel 0 is using its buffer
	if(ms_bLoadingBigModel)
		currentChannel = 0;

	// We have data, load
	if(ms_channel[currentChannel].state == CHANNELSTATE_READING ||
	   ms_channel[currentChannel].state == CHANNELSTATE_STARTED)
		ProcessLoadingChannel(currentChannel);

	if(ms_channelError == -1){
		// Channel is idle, read more data
		if(ms_channel[currentChannel].state == CHANNELSTATE_IDLE)
			RequestModelStream(currentChannel);
		// Switch channel
		if(ms_channel[currentChannel].state != CHANNELSTATE_STARTED)
			currentChannel = 1 - currentChannel;
	}

	s_bInsideLoadRequested = false;
}


// Let's load models in 4 threads; when one of them becomes idle, process the file, and fill thread with another file. Unfortunately processing models are still single-threaded.
// Currently only supported on POSIX streamer.
// WIP - some files are loaded swapped (CdStreamPosix problem?)
#if 0 //def ONE_THREAD_PER_CHANNEL
void
CStreaming::LoadAllRequestedModels(bool priority)
{
	static bool bInsideLoadAll = false;
	int imgOffset, streamId, status;
	int i;
	uint32 posn, size;

	if(bInsideLoadAll)
		return;
	bInsideLoadAll = true;
#ifdef WII
	bool oldSynchronousLoad = gWiiStreamSynchronousLoad;
	gWiiStreamSynchronousLoad = true;
#endif

	FlushChannels();
	imgOffset = GetCdImageOffset(CdStreamGetLastPosn());

	int streamIds[ARRAY_SIZE(ms_pStreamingBuffer)];
	int streamSizes[ARRAY_SIZE(ms_pStreamingBuffer)];
	int streamPoses[ARRAY_SIZE(ms_pStreamingBuffer)];
	int readOrder[4] = {-1}; // Channel IDs ordered by read time
	int readI = 0;
	int processI = 0;
	bool first = true;

	// All those "first" checks are because of variables aren't initialized in first pass.

	while (true) {
		for (int i=0; i<ARRAY_SIZE(ms_pStreamingBuffer); i++) {

			// Channel has file to load
			if (!first && streamIds[i] != -1) {
				continue;
			}

			if(ms_endRequestedList.m_prev != &ms_startRequestedList){
				streamId = GetNextFileOnCd(0, priority);
				if(streamId == -1){
					streamIds[i] = -1;
					break;
				}

				if (ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size)) {
					streamIds[i] = -1;

					// Big file, needs 2 buffer
					if (size > (uint32)ms_streamingBufferSize) {
						if (i + 1 == ARRAY_SIZE(ms_pStreamingBuffer))
							break;
						else if (!first && streamIds[i+1] != -1)
							continue;

					} else {
						// Buffer of current channel is part of a "big file", pass
						if (i != 0 && streamIds[i-1] != -1 && streamSizes[i-1] > (uint32)ms_streamingBufferSize)
							continue;
					}
					ms_aInfoForModel[streamId].RemoveFromList();
					DecrementRef(streamId);

					streamIds[i] = streamId;
					streamSizes[i] = size;
					streamPoses[i] = posn;

					if (!first)
						assert(readOrder[readI] == -1);

					//printf("read: order %d, ch %d, id %d, size %d\n", readI, i, streamId, size);

					CdStreamRead(i, ms_pStreamingBuffer[i], imgOffset+posn, size);
					readOrder[readI] = i;
					if (first && readI+1 != ARRAY_SIZE(readOrder))
						readOrder[readI+1] = -1;

					readI = (readI + 1) % ARRAY_SIZE(readOrder);
				} else {
					ms_aInfoForModel[streamId].RemoveFromList();
					DecrementRef(streamId);

					ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_LOADED;
					streamIds[i] = -1;
				}
			} else {
				streamIds[i] = -1;
				break;
			}
		}

		first = false;
		int nextChannel = readOrder[processI];

		// Now start processing
		if (nextChannel == -1 || streamIds[nextChannel] == -1)
			break;

		//printf("process: order %d, ch %d, id %d\n", processI, nextChannel, streamIds[nextChannel]);

		// Try again on error
		while (CdStreamSync(nextChannel) != STREAM_NONE) {
			CdStreamRead(nextChannel, ms_pStreamingBuffer[nextChannel], imgOffset+streamPoses[nextChannel], streamSizes[nextChannel]);
		}
		ms_aInfoForModel[streamIds[nextChannel]].m_loadState = STREAMSTATE_READING;

		MakeSpaceFor(streamSizes[nextChannel] * CDSTREAM_SECTOR_SIZE);
		ConvertBufferToObject(ms_pStreamingBuffer[nextChannel], streamIds[nextChannel]);
		if(ms_aInfoForModel[streamIds[nextChannel]].m_loadState == STREAMSTATE_STARTED)
			FinishLoadingLargeFile(ms_pStreamingBuffer[nextChannel], streamIds[nextChannel]);

		if(streamIds[nextChannel] < STREAM_OFFSET_TXD){
			CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(streamIds[nextChannel]);
			if(mi->IsSimple())
				mi->m_alpha = 255;
		}
		streamIds[nextChannel] = -1;
		readOrder[processI] = -1;
		processI = (processI + 1) % ARRAY_SIZE(readOrder);
	}

	ms_bLoadingBigModel = false;
	for(i = 0; i < 4; i++){
		ms_channel[1].streamIds[i] = -1;
		ms_channel[1].offsets[i] = -1;
	}
	ms_channel[1].state = CHANNELSTATE_IDLE;
#ifdef WII
	gWiiStreamSynchronousLoad = oldSynchronousLoad;
#endif
	bInsideLoadAll = false;
}
#else
void
CStreaming::LoadAllRequestedModels(bool priority)
{
	static bool bInsideLoadAll = false;
	int imgOffset, streamId, status;
	int i;
	uint32 posn, size;

	int numRequests = 4*ms_numModelsRequested;
	int workRequests = 4*ms_numModelsRequested + 8;

	if(bInsideLoadAll)
		return;
	bInsideLoadAll = true;
#ifdef WII
	bool oldSynchronousLoad = gWiiStreamSynchronousLoad;
	gWiiStreamSynchronousLoad = true;
#endif

	if(priority)
		numRequests = ms_numPriorityRequests;

	FlushChannels();
	imgOffset = GetCdImageOffset(CdStreamGetLastPosn());

	((void)0); // [GC-DEBUG-DISABLED]
	while(ms_endRequestedList.m_prev != &ms_startRequestedList &&
	      workRequests > 0){
	#ifdef WII
		bool hasRequestedWork = priority ? WiiStreamHasForegroundRequest() :
		                                      numRequests > 0;
	#else
		bool hasRequestedWork = priority ? ms_numPriorityRequests > 0 :
		                                      numRequests > 0;
	#endif
		if(!hasRequestedWork)
			break;
		workRequests--;
		static int diagCnt = 0;
		((void)0); // [GC-DEBUG-DISABLED]
		streamId = GetNextFileOnCd(0, priority);
		if(streamId == -1)
			break;
		// Dependencies consume only the bounded work allowance. A priority pass
		// ends when the live priority count reaches zero, not after one dependency.
		if(!priority)
			numRequests--;
        ((void)0); // [GC-DEBUG-DISABLED]
	#ifdef WII
		WiiStreamOnRequestDispatched(streamId);
	#endif
		ms_aInfoForModel[streamId].RemoveFromList();
		ms_channel[0].streamIds[0] = streamId;
		DecrementRef(streamId);

		if(ms_aInfoForModel[streamId].GetCdPosnAndSize(posn, size)){
			do
				status = CdStreamRead(0, ms_pStreamingBuffer[0], imgOffset+posn, size);
			while(CdStreamSync(0) || status == STREAM_NONE);
			ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_READING;

			if(!MakeSpaceFor(size * CDSTREAM_SECTOR_SIZE)){
#ifdef WII
				WiiStreamRequeueDispatched(streamId, "sync-pressure");
#else
				RemoveModel(streamId);
				ReRequestModel(streamId);
#endif
				continue;
			}
                        ((void)0); // [GC-DEBUG-DISABLED]
			ConvertBufferToObject(ms_pStreamingBuffer[0], streamId);
                        ((void)0); // [GC-DEBUG-DISABLED]
			if(ms_aInfoForModel[streamId].m_loadState == STREAMSTATE_STARTED) {
                            ((void)0); // [GC-DEBUG-DISABLED]
			    FinishLoadingLargeFile(ms_pStreamingBuffer[0], streamId);
                            ((void)0); // [GC-DEBUG-DISABLED]
                        }

			if(streamId < STREAM_OFFSET_TXD){
				CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(streamId);
				if(mi->IsSimple())
					mi->m_alpha = 255;
			}
		}else{
			// empty
			ms_aInfoForModel[streamId].m_loadState = STREAMSTATE_LOADED;
#ifdef WII
			WiiStreamMarkWorldLoaded(streamId);
			WiiStreamDiagOnLoaded(streamId);
			WiiStreamClearDispatchedFlags(streamId);
#endif
		}
	}
    ((void)0); // [GC-DEBUG-DISABLED]
	ms_bLoadingBigModel = false;
	for(i = 0; i < 4; i++){
		ms_channel[1].streamIds[i] = -1;
		ms_channel[1].offsets[i] = -1;
	}
	ms_channel[1].state = CHANNELSTATE_IDLE;
#ifdef WII
	gWiiStreamSynchronousLoad = oldSynchronousLoad;
#endif
	bInsideLoadAll = false;
}
#endif

void
CStreaming::FlushChannels(void)
{
	if(ms_channel[1].state == CHANNELSTATE_STARTED)
		ProcessLoadingChannel(1);

	if(ms_channel[0].state == CHANNELSTATE_READING){
		CdStreamSync(0);
		ProcessLoadingChannel(0);
	}
	if(ms_channel[0].state == CHANNELSTATE_STARTED)
		ProcessLoadingChannel(0);

	if(ms_channel[1].state == CHANNELSTATE_READING){
		CdStreamSync(1);
		ProcessLoadingChannel(1);
	}
	if(ms_channel[1].state == CHANNELSTATE_STARTED)
		ProcessLoadingChannel(1);
}

void
CStreaming::FlushRequestList(void)
{
	CStreamingInfo *si, *next;

	for(si = ms_startRequestedList.m_next; si != &ms_endRequestedList; si = next){
		next = si->m_next;
		RemoveModel(si - ms_aInfoForModel);
	}
#ifdef FLUSHABLE_STREAMING
	if(ms_channel[0].state == CHANNELSTATE_READING) {
		flushStream[0] = 1;
	}
	if(ms_channel[1].state == CHANNELSTATE_READING) {
		flushStream[1] = 1;
	}
#endif
	FlushChannels();
}


void
CStreaming::ImGonnaUseStreamingMemory(void)
{
	PUSH_MEMID(MEMID_STREAM);
}

void
CStreaming::IHaveUsedStreamingMemory(void)
{
	POP_MEMID();
	UpdateMemoryUsed();
}

void
CStreaming::UpdateMemoryUsed(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	ms_memoryUsed =
		gMainHeap.GetMemoryUsed(MEMID_STREAM) +
		gMainHeap.GetMemoryUsed(MEMID_STREAM_MODELS) +
		gMainHeap.GetMemoryUsed(MEMID_STREAM_TEXUTRES) +
		gMainHeap.GetMemoryUsed(MEMID_STREAM_COLLISION) +
		gMainHeap.GetMemoryUsed(MEMID_STREAM_ANIMATION);
#endif
}

#define STREAM_DIST 80.0f

void
CStreaming::AddModelsToRequestList(const CVector &pos, int32 flags)
{
	float xmin, xmax, ymin, ymax;
	int ixmin, ixmax, iymin, iymax;
	int ix, iy;
	int dx, dy, d;
	CSector *sect;

	xmin = pos.x - STREAM_DIST;
	ymin = pos.y - STREAM_DIST;
	xmax = pos.x + STREAM_DIST;
	ymax = pos.y + STREAM_DIST;

	ixmin = CWorld::GetSectorIndexX(xmin);
	if(ixmin < 0) ixmin = 0;
	ixmax = CWorld::GetSectorIndexX(xmax);
	if(ixmax >= NUMSECTORS_X) ixmax = NUMSECTORS_X-1;
	iymin = CWorld::GetSectorIndexY(ymin);
	if(iymin < 0) iymin = 0;
	iymax = CWorld::GetSectorIndexY(ymax);
	if(iymax >= NUMSECTORS_Y) iymax = NUMSECTORS_Y-1;

	CWorld::AdvanceCurrentScanCode();

#ifdef WII
	// Priority back-pressure may stop the wider scan, so always enqueue the
	// player's own sector first.
	int32 centerX = CWorld::GetSectorIndexX(pos.x);
	int32 centerY = CWorld::GetSectorIndexY(pos.y);
	if(centerX >= 0 && centerX < NUMSECTORS_X && centerY >= 0 && centerY < NUMSECTORS_Y){
		CSector *center = CWorld::GetSector(centerX, centerY);
		ProcessEntitiesInSectorList(center->m_lists[ENTITYLIST_BUILDINGS], flags);
		ProcessEntitiesInSectorList(center->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], flags);
		ProcessEntitiesInSectorList(center->m_lists[ENTITYLIST_OBJECTS], flags);
		ProcessEntitiesInSectorList(center->m_lists[ENTITYLIST_DUMMIES], flags);
	}
#endif

	for(iy = iymin; iy <= iymax; iy++){
		dy = iy - CWorld::GetSectorIndexY(pos.y);
		for(ix = ixmin; ix <= ixmax; ix++){

			if(CRenderer::m_loadingPriority && ms_numModelsRequested > 5)
				return;

			dx = ix - CWorld::GetSectorIndexX(pos.x);
			d = dx*dx + dy*dy;
			sect = CWorld::GetSector(ix, iy);
			if(d <= 0){
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], flags);
			}else if(d <= 3*3){
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], pos.x, pos.y, xmin, ymin, xmax, ymax, flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], pos.x, pos.y, xmin, ymin, xmax, ymax, flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], pos.x, pos.y, xmin, ymin, xmax, ymax, flags);
				ProcessEntitiesInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], pos.x, pos.y, xmin, ymin, xmax, ymax, flags);
			}
		}
	}
}

void
CStreaming::ProcessEntitiesInSectorList(CPtrList &list, float x, float y, float xmin, float ymin, float xmax, float ymax, int32 flags)
{
	CPtrNode *node;
	CEntity *e;
	float lodDistSq;
	CVector2D pos;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;

		if(e->m_scanCode == CWorld::GetCurrentScanCode())
			continue;

		e->m_scanCode = CWorld::GetCurrentScanCode();
		if(!e->bStreamingDontDelete && IsAreaVisible(e->m_area) && !e->bDontStream && e->bIsVisible){
			CTimeModelInfo *mi = (CTimeModelInfo*)CModelInfo::GetModelInfo(e->GetModelIndex());
			if (mi->GetModelType() != MITYPE_TIME || CClock::GetIsTimeInRange(mi->GetTimeOn(), mi->GetTimeOff())) {
				lodDistSq = sq(mi->GetLargestLodDistance());
			#if defined(WII) && WII_STREAM_PS2_WORLD_SCAN_RADIUS
				// PS2 SLUS_205.52 sub_2E5A00 clamps the request radius up
				// to at least STREAM_DIST. Keep this profile-scoped until the
				// one-variable route A/B proves the behavior on Wii.
				lodDistSq = Max(lodDistSq, sq(STREAM_DIST));
			#else
				lodDistSq = Min(lodDistSq, sq(STREAM_DIST));
			#endif
				pos = CVector2D(e->GetPosition());
				if(xmin < pos.x && pos.x < xmax &&
				   ymin < pos.y && pos.y < ymax &&
				   (CVector2D(x, y) - pos).MagnitudeSqr() < lodDistSq)
					RequestModelFromWorldScan(e->GetModelIndex(), flags);
			}
		}
	}
}

void
CStreaming::ProcessEntitiesInSectorList(CPtrList &list, int32 flags)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;

		if(e->m_scanCode == CWorld::GetCurrentScanCode())
			continue;

		e->m_scanCode = CWorld::GetCurrentScanCode();
		if(!e->bStreamingDontDelete && IsAreaVisible(e->m_area) && !e->bDontStream && e->bIsVisible){
			CTimeModelInfo *mi = (CTimeModelInfo*)CModelInfo::GetModelInfo(e->GetModelIndex());
			if (mi->GetModelType() != MITYPE_TIME || CClock::GetIsTimeInRange(mi->GetTimeOn(), mi->GetTimeOff()))
				RequestModelFromWorldScan(e->GetModelIndex(), flags);
		}
	}
}

void
CStreaming::DeleteFarAwayRwObjects(const CVector &pos)
{
	int posx, posy;
	int x, y;
	int r, i;
	CSector *sect;

	posx = CWorld::GetSectorIndexX(pos.x);
	posy = CWorld::GetSectorIndexY(pos.y);
	// Move oldSectorX/Y to new sector and delete RW objects in its "wake" for every step:
	// O is the old sector, <- is the direction in which we move it,
	// X are the sectors we delete RW objects from (except we go up to 10)
	//            X
	//          X X
	//        X X X
	//        X X X
	// <- O   X X X
	//        X X X
	//        X X X
	//          X X
	//            X

	while(posx != ms_oldSectorX){
		if(posx < ms_oldSectorX){
			for(r = 2; r <= 10; r++){
				x = ms_oldSectorX + r;
				if(x < 0)
					continue;
				if(x >= NUMSECTORS_X)
					break;

				for(i = -r; i <= r; i++){
					y = ms_oldSectorY + i;
					if(y < 0)
						continue;
					if(y >= NUMSECTORS_Y)
						break;

					sect = CWorld::GetSector(x, y);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
					DeleteRwObjectsInOverlapSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], ms_oldSectorX, ms_oldSectorY);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				}
			}
			ms_oldSectorX--;
		}else{
			for(r = 2; r <= 10; r++){
				x = ms_oldSectorX - r;
				if(x < 0)
					break;
				if(x >= NUMSECTORS_X)
					continue;

				for(i = -r; i <= r; i++){
					y = ms_oldSectorY + i;
					if(y < 0)
						continue;
					if(y >= NUMSECTORS_Y)
						break;

					sect = CWorld::GetSector(x, y);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
					DeleteRwObjectsInOverlapSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], ms_oldSectorX, ms_oldSectorY);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				}
			}
			ms_oldSectorX++;
		}
	}

	while(posy != ms_oldSectorY){
		if(posy < ms_oldSectorY){
			for(r = 2; r <= 10; r++){
				y = ms_oldSectorY + r;
				if(y < 0)
					continue;
				if(y >= NUMSECTORS_Y)
					break;

				for(i = -r; i <= r; i++){
					x = ms_oldSectorX + i;
					if(x < 0)
						continue;
					if(x >= NUMSECTORS_X)
						break;

					sect = CWorld::GetSector(x, y);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
					DeleteRwObjectsInOverlapSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], ms_oldSectorX, ms_oldSectorY);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				}
			}
			ms_oldSectorY--;
		}else{
			for(r = 2; r <= 10; r++){
				y = ms_oldSectorY - r;
				if(y < 0)
					break;
				if(y >= NUMSECTORS_Y)
					continue;

				for(i = -r; i <= r; i++){
					x = ms_oldSectorX + i;
					if(x < 0)
						continue;
					if(x >= NUMSECTORS_X)
						break;

					sect = CWorld::GetSector(x, y);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
					DeleteRwObjectsInOverlapSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP], ms_oldSectorX, ms_oldSectorY);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
					DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				}
			}
			ms_oldSectorY++;
		}
	}
}

void
CStreaming::DeleteAllRwObjects(void)
{
	int x, y;
	CSector *sect;

	for(x = 0; x < NUMSECTORS_X; x++)
		for(y = 0; y < NUMSECTORS_Y; y++){
			sect = CWorld::GetSector(x, y);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS_OVERLAP]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
			DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES_OVERLAP]);
		}
}

void
CStreaming::DeleteRwObjectsAfterDeath(const CVector &pos)
{
	int ix, iy;
	int x, y;
	CSector *sect;

	ix = CWorld::GetSectorIndexX(pos.x);
	iy = CWorld::GetSectorIndexY(pos.y);

	for(x = 0; x < NUMSECTORS_X; x++)
		for(y = 0; y < NUMSECTORS_Y; y++)
			if(Abs(ix - x) > 3.0f &&
			   Abs(iy - y) > 3.0f){
				sect = CWorld::GetSector(x, y);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_OBJECTS_OVERLAP]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES]);
				DeleteRwObjectsInSectorList(sect->m_lists[ENTITYLIST_DUMMIES_OVERLAP]);
			}
}

void
CStreaming::DeleteRwObjectsBehindCamera(size_t mem)
{
	int ix, iy;
	int x, y;
	int xmin, xmax, ymin, ymax;
	int inc;
	CSector *sect;

	if(ms_memoryUsed < mem)
		return;

	ix = CWorld::GetSectorIndexX(TheCamera.GetPosition().x);
	iy = CWorld::GetSectorIndexY(TheCamera.GetPosition().y);

	if(Abs(TheCamera.GetForward().x) > Abs(TheCamera.GetForward().y)){
		// looking west/east

		ymin = Max(iy - 10, 0);
		ymax = Min(iy + 10, NUMSECTORS_Y - 1);
		assert(ymin <= ymax);

		// Delete a block of sectors that we know is behind the camera
		if(TheCamera.GetForward().x > 0.0f){
			// looking east
			xmax = Max(ix - 2, 0);
			xmin = Max(ix - 10, 0);
			inc = 1;
		}else{
			// looking west
			xmax = Min(ix + 2, NUMSECTORS_X - 1);
			xmin = Min(ix + 10, NUMSECTORS_X - 1);
			inc = -1;
		}
		for(y = ymin; y <= ymax; y++){
			for(x = xmin; x != xmax; x += inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}


		while(RemoveLoadedZoneModel())
			if(ms_memoryUsed < mem)
				return;

		// Now a block that intersects with the camera's frustum
		if(TheCamera.GetForward().x > 0.0f){
			// looking east
			xmax = Max(ix + 10, 0);
			xmin = Max(ix - 2, 0);
			inc = 1;
		}else{
			// looking west
			xmax = Min(ix - 10, NUMSECTORS_X - 1);
			xmin = Min(ix + 2, NUMSECTORS_X - 1);
			inc = -1;
		}
		for(y = ymin; y <= ymax; y++){
			for(x = xmin; x != xmax; x += inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}

		// As last resort, delete objects from the last step more aggressively
		for(y = ymin; y <= ymax; y++){
			for(x = xmax; x != xmin; x -= inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}
	}else{
		// looking north/south

		xmin = Max(ix - 10, 0);
		xmax = Min(ix + 10, NUMSECTORS_X - 1);
		assert(xmin <= xmax);

		// Delete a block of sectors that we know is behind the camera
		if(TheCamera.GetForward().y > 0.0f){
			// looking north
			ymax = Max(iy - 2, 0);
			ymin = Max(iy - 10, 0);
			inc = 1;
		}else{
			// looking south
			ymax = Min(iy + 2, NUMSECTORS_Y - 1);
			ymin = Min(iy + 10, NUMSECTORS_Y - 1);
			inc = -1;
		}
		for(x = xmin; x <= xmax; x++){
			for(y = ymin; y != ymax; y += inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}

		while(RemoveLoadedZoneModel())
			if(ms_memoryUsed < mem)
				return;

		// Now a block that intersects with the camera's frustum
		if(TheCamera.GetForward().y > 0.0f){
			// looking north
			ymax = Max(iy + 10, 0);
			ymin = Max(iy - 2, 0);
			inc = 1;
		}else{
			// looking south
			ymax = Min(iy - 10, NUMSECTORS_Y - 1);
			ymin = Min(iy + 2, NUMSECTORS_Y - 1);
			inc = -1;
		}
		for(x = xmin; x <= xmax; x++){
			for(y = ymin; y != ymax; y += inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsNotInFrustumInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}

// this is gone in mobile together with RemoveReferencedTxds
//		if(RemoveReferencedTxds(mem))
//			return;

		// As last resort, delete objects from the last step more aggressively
		for(x = xmin; x <= xmax; x++){
			for(y = ymax; y != ymin; y -= inc){
				sect = CWorld::GetSector(x, y);
				if(DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_BUILDINGS], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_DUMMIES], mem) ||
				   DeleteRwObjectsBehindCameraInSectorList(sect->m_lists[ENTITYLIST_OBJECTS], mem))
					return;
			}
		}
	}

	while(ms_memoryUsed >= mem && RemoveLeastUsedModel(0));
}

void
CStreaming::DeleteRwObjectsInSectorList(CPtrList &list)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;
		if(!e->bStreamingDontDelete && !e->bImBeingRendered)
			e->DeleteRwObject();
	}
}

void
CStreaming::DeleteRwObjectsInOverlapSectorList(CPtrList &list, int32 x, int32 y)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;
		if(e->m_rwObject && !e->bStreamingDontDelete && !e->bImBeingRendered){
			// Now this is pretty weird...
			if(Abs(CWorld::GetSectorIndexX(e->GetPosition().x) - x) >= 1.6f)
//			{
				e->DeleteRwObject();
//				return;		// BUG?
//			}
			else	// FIX?
			if(Abs(CWorld::GetSectorIndexY(e->GetPosition().y) - y) >= 1.6f)
				e->DeleteRwObject();
		}
	}
}

bool
CStreaming::DeleteRwObjectsBehindCameraInSectorList(CPtrList &list, size_t mem)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;
		if(!e->bStreamingDontDelete && !e->bImBeingRendered &&
		   e->m_rwObject && ms_aInfoForModel[e->GetModelIndex()].m_next &&
		   FindPlayerPed()->m_pCurSurface != e){
			e->DeleteRwObject();
			if (CModelInfo::GetModelInfo(e->GetModelIndex())->GetNumRefs() == 0) {
				RemoveModel(e->GetModelIndex());
				if(ms_memoryUsed < mem)
					return true;
			}
		}
	}
	return false;
}

bool
CStreaming::DeleteRwObjectsNotInFrustumInSectorList(CPtrList &list, size_t mem)
{
	CPtrNode *node;
	CEntity *e;

	for(node = list.first; node; node = node->next){
		e = (CEntity*)node->item;
		if(!e->bStreamingDontDelete && !e->bImBeingRendered &&
		   e->m_rwObject && (!e->IsVisible() || e->bOffscreen) && ms_aInfoForModel[e->GetModelIndex()].m_next){
			e->DeleteRwObject();
			if (CModelInfo::GetModelInfo(e->GetModelIndex())->GetNumRefs() == 0) {
				RemoveModel(e->GetModelIndex());
				if(ms_memoryUsed < mem)
					return true;
			}
		}
	}
	return false;
}

static uint32
WiiStreamArchiveRetirePool(const WiiMemoryPoolSnapshot &snapshot)
{
	size_t newlibRawFree =
		(size_t)snapshot.newlibFree + snapshot.rawArena2Remaining;
	if(snapshot.gxFree < WII_STREAM_ARCHIVE_KEEP_GX_FREE_BYTES ||
	   snapshot.gxLargest < WII_STREAM_ARCHIVE_KEEP_GX_LARGEST_BYTES)
		return WII_STREAM_PRESSURE_GX;
	if(snapshot.genericFree < WII_STREAM_ARCHIVE_KEEP_GENERIC_FREE_BYTES ||
	   snapshot.genericLargest < WII_STREAM_ARCHIVE_KEEP_GENERIC_LARGEST_BYTES)
		return WII_STREAM_PRESSURE_GENERIC;
	if(newlibRawFree < WII_STREAM_ARCHIVE_KEEP_NEWLIB_RAW_BYTES)
		return WII_STREAM_PRESSURE_NEWLIB;
	return 0;
}

static bool
WiiStreamRetireOneHeadroomResource(const WiiMemoryPoolSnapshot &snapshot,
	uint32 pressure, uint32 *poolBitOut)
{
	if(poolBitOut)
		*poolBitOut = 0;
	uint32 hardPressure = WiiStreamHardPressureBits(pressure);
	uint32 gxAdmissionPressure =
		WiiStreamAdmissionPressureBits(pressure) &
		WII_STREAM_PRESSURE_GX_ADMISSION;
	if((hardPressure & WII_STREAM_PRESSURE_GX) == 0 &&
	   gxAdmissionPressure == 0)
		return false;

	uint32 poolBit = (hardPressure & WII_STREAM_PRESSURE_GX) != 0 ||
	                 gxAdmissionPressure != 0 ?
	                 WII_STREAM_PRESSURE_GX :
	                 WiiStreamArchiveRetirePool(snapshot);
	uint32 frame = CTimer::GetFrameCounter();
	if(poolBit == 0 || gWiiStreamArchiveRetireFrame == frame)
		return false;

	bool ownsDiagEpisode = false;
#if WII_STREAM_MEMORY_DIAGNOSTICS
	if(!WiiStreamDiagCapturingTrim()){
		WiiStreamDiagBeginTrim(pressure, 0);
		ownsDiagEpisode = true;
	}
	WiiStreamDiagSetTrimPressure(pressure, poolBit);
	WiiStreamDiagSetTrimReason(
		poolBit == WII_STREAM_PRESSURE_GENERIC ? WII_STREAM_TRIM_POOL_GENERIC :
		poolBit == WII_STREAM_PRESSURE_NEWLIB ? WII_STREAM_TRIM_POOL_NEWLIB :
		WII_STREAM_TRIM_POOL_GX);
#endif
	uint64 removalStartTicks = gettime();
	bool didRemove = WiiStreamRemoveLeastUsedForPool(
		STREAMFLAGS_20, poolBit, nil, nil);
	uint64 removalTicks = gettime() - removalStartTicks;
	if(!didRemove){
#if WII_STREAM_MEMORY_DIAGNOSTICS
		if(ownsDiagEpisode)
			WiiStreamDiagEndTrim();
#endif
		return false;
	}
	WiiMemoryPoolSnapshot after;
	WiiMemoryGetPoolSnapshot(&after);
	uint32 nextPressure =
		WiiStreamGetStreamingPressureForSnapshotWithAdmission(after);
	size_t reclaimedBytes = WiiStreamPoolReclaimedBytes(
		poolBit, snapshot, after);
	// Removing a stream entry is not sufficient evidence that the owning pool
	// made room. A model can disappear while its TXD/raster remains resident.
	// Leave the resource removed, but do not count that as headroom retirement.
	bool effectiveRetirement = WiiStreamPoolMadeProgress(
		poolBit, nextPressure, snapshot, after, reclaimedBytes);
#if WII_STREAM_MEMORY_DIAGNOSTICS
	if(ownsDiagEpisode){
		WiiStreamDiagEndTrim();
		if(effectiveRetirement)
			WiiStreamRecordFrameWork(0, removalTicks, 1, 1, 0);
	}
#endif
	if(!effectiveRetirement)
		return false;
	gWiiStreamArchiveRetireFrame = frame;

	if(poolBitOut)
		*poolBitOut = poolBit;
	return true;
}

bool
CStreaming::MakeSpaceFor(int32 size)
{
#ifdef WII
	if(ms_memoryAvailable == 0)
		ms_memoryAvailable = WII_STREAMING_MEMORY_BUDGET_LOW;
#elif defined FIX_BUGS
#define MB (1024 * 1024)
	if(ms_memoryAvailable == 0) {
		extern size_t _dwMemAvailPhys;
		ms_memoryAvailable = (_dwMemAvailPhys - 10 * MB) / 2;
		if(ms_memoryAvailable < 65 * MB) ms_memoryAvailable = 65 * MB;
	}
#undef MB
#endif
#ifdef WII
#if WII_STREAM_MEMORY_DIAGNOSTICS
	uint64 makeSpaceStartTicks = gettime();
	uint64 removalTicks = 0;
#endif
	WiiMemoryPoolSnapshot poolBefore;
	WiiMemoryGetPoolSnapshot(&poolBefore);
	uint32 initialPressure =
		WiiStreamGetStreamingPressureForSnapshotWithAdmission(poolBefore);
	WiiStreamUpdateMemoryBudget(poolBefore, initialPressure);
	size_t archiveCeiling = WiiStreamUpdateArchiveCeiling(poolBefore, initialPressure);
	size_t requested = size > 0 ? (size_t)size : 0;
	size_t transitionReserve = IsIslandTransitionActive() ?
	                           WII_ISLAND_TRANSITION_RESERVE : 0;
	size_t effectiveRequest = requested + transitionReserve;
	// The transition reserve protects real allocator headroom. Keep the archive
	// proxy values for diagnostics, but do not use them to evict models.
	size_t archiveTransitionGuard = transitionReserve == 0 ? 0 :
	                                Min(transitionReserve,
	                                    WII_STREAM_ARCHIVE_TRANSITION_GUARD);
	size_t archiveRequest = requested + archiveTransitionGuard;
	size_t archiveTarget = archiveRequest < archiveCeiling ?
	                       archiveCeiling - archiveRequest : 0;
	uint32 pressure = initialPressure;
	size_t before = ms_memoryUsed;
	int removed = 0;
	int budgetRemovals = 0;
	int archiveRemovals = 0; // dependency-unwind fallback removals
	int pressureRemovals = 0;
	uint32 blockedPressure = 0;
	size_t reclaimedByPool[3] = { 0, 0, 0 };
	int ineffectiveRemovals = 0;
	bool sameFrameDeferred = false;
#if WII_STREAM_MEMORY_DIAGNOSTICS
	WiiStreamDiagBeginTrim(initialPressure, archiveTarget);
#endif
	pressure = WiiStreamGetStreamingPressureForSnapshotWithAdmission(poolBefore);
	while(pressureRemovals < WII_STREAM_PRESSURE_MAX_REMOVALS){
		uint32 activePressure = pressure & ~blockedPressure;
		if(activePressure == 0)
			break;
		uint32 poolBit = WiiStreamSelectPressureBit(
			activePressure, poolBefore, effectiveRequest);
		if(poolBit == 0)
			break;
		uint32 servicePressureBit = WiiStreamPressureServiceBit(poolBit);
		int32 poolIndex = WiiStreamPressureIndex(servicePressureBit);
#if WII_STREAM_MEMORY_DIAGNOSTICS
		WiiStreamDiagSetTrimPressure(pressure, servicePressureBit);
		WiiStreamDiagSetTrimReason(
			servicePressureBit == WII_STREAM_PRESSURE_GENERIC ? WII_STREAM_TRIM_POOL_GENERIC :
			servicePressureBit == WII_STREAM_PRESSURE_NEWLIB ? WII_STREAM_TRIM_POOL_NEWLIB :
			WII_STREAM_TRIM_POOL_GX);
#endif

		int32 removedId = -1;
		bool selectionDeferred = false;
#if WII_STREAM_MEMORY_DIAGNOSTICS
		uint64 removalStartTicks = gettime();
#endif
		bool didRemove = WiiStreamRemoveLeastUsedForPool(
			STREAMFLAGS_20, servicePressureBit, &removedId, &selectionDeferred);
#if WII_STREAM_MEMORY_DIAGNOSTICS
		removalTicks += gettime() - removalStartTicks;
#endif
		if(!didRemove){
			sameFrameDeferred |= selectionDeferred;
			blockedPressure |= poolBit;
			continue;
		}

		WiiMemoryPoolSnapshot poolAfter;
		WiiMemoryGetPoolSnapshot(&poolAfter);
		removed++;
		pressureRemovals++;
		uint32 nextPressure =
			WiiStreamGetStreamingPressureForSnapshotWithAdmission(poolAfter);
		size_t reclaimedBytes = WiiStreamPoolReclaimedBytes(
			servicePressureBit, poolBefore, poolAfter);
		reclaimedByPool[poolIndex] += reclaimedBytes;
		if(!WiiStreamPoolMadeProgress(poolBit, nextPressure,
		                             poolBefore, poolAfter, reclaimedBytes)){
			ineffectiveRemovals++;
			// Do not keep removing logical LRU entries after the owning pool has
			// proved that the selected resource did not release it. The next
			// request/frame may expose the TXD after its dependency is gone.
			blockedPressure |= poolBit;
		}
		pressure = nextPressure;
		poolBefore = poolAfter;
	}

	// If the owning pool is still under hard pressure after targeted removal
	// blocked, try one more resource charged to that same pool. Never fall back
	// to the global LRU here: unrelated model retirement cannot clear the pool.
	uint32 dependencyUnwindFrame = CTimer::GetFrameCounter();
	bool dependencyUnwindAvailable =
		gWiiStreamDependencyUnwindFrame != dependencyUnwindFrame;
	uint32 dependencyPressure =
		WiiStreamHardPressureBits(pressure & blockedPressure);
	if((pressure & blockedPressure & WII_STREAM_PRESSURE_GX_ADMISSION) != 0)
		dependencyPressure |= WII_STREAM_PRESSURE_GX;
	if(dependencyUnwindAvailable && dependencyPressure != 0){
		uint32 dependencyPoolBit = WiiStreamSelectPressureBit(
			dependencyPressure,
			poolBefore, effectiveRequest);
		dependencyPoolBit = WiiStreamPressureServiceBit(dependencyPoolBit);
		if(dependencyPoolBit != 0){
			gWiiStreamDependencyUnwindFrame = dependencyUnwindFrame;
#if WII_STREAM_MEMORY_DIAGNOSTICS
			WiiStreamDiagSetTrimPressure(pressure, dependencyPoolBit);
			WiiStreamDiagSetTrimReason(WII_STREAM_TRIM_DEPENDENCY_UNWIND);
			uint64 removalStartTicks = gettime();
#endif
			int32 removedId = -1;
			bool selectionDeferred = false;
			bool didRemove = WiiStreamRemoveLeastUsedForPool(
				STREAMFLAGS_20, dependencyPoolBit, &removedId, &selectionDeferred);
#if WII_STREAM_MEMORY_DIAGNOSTICS
			removalTicks += gettime() - removalStartTicks;
#endif
			if(didRemove){
				WiiMemoryGetPoolSnapshot(&poolBefore);
				pressure = WiiStreamGetStreamingPressureForSnapshotWithAdmission(
					poolBefore);
				removed++;
				archiveRemovals++;
			}else{
				sameFrameDeferred |= selectionDeferred;
			}
		}
	}
#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING
	// A healthy pool may temporarily carry archive data above its moving cap.
	// Once real retention headroom is gone, retire at most one dependency-safe
	// old resource per frame before allowing GX shrink to mutate textures.
	uint32 archiveRetirePool = 0;
	size_t retentionAllowance =
		WiiStreamArchiveElasticAllowance(poolBefore, pressure);
	if(ms_memoryUsed > archiveCeiling + retentionAllowance &&
	   WiiStreamRetireOneHeadroomResource(
		   poolBefore, pressure, &archiveRetirePool)){
		WiiMemoryGetPoolSnapshot(&poolBefore);
		pressure = WiiStreamGetStreamingPressureForSnapshotWithAdmission(
			poolBefore);
		removed++;
		archiveRemovals++;
	}
#endif

	// A transient reserve crossing no longer collapses the cache to 16 MiB.
	// If targeted reclamation cannot clear non-GX pressure for two seconds,
	// lower the soft budget by one step and release at most one global LRU entry.
	if(WiiStreamApplyPersistentPressure(pressure) &&
	   (WiiStreamHardPressureBits(pressure) & WII_STREAM_PRESSURE_GX) == 0){
		size_t softTarget = effectiveRequest < ms_memoryAvailable ?
		                    ms_memoryAvailable - effectiveRequest : 0;
		if(softTarget != 0 && ms_memoryUsed >= softTarget){
#if WII_STREAM_MEMORY_DIAGNOSTICS
			WiiStreamDiagSetTrimPressure(pressure, 0);
			WiiStreamDiagSetTrimReason(WII_STREAM_TRIM_PERSISTENT_BUDGET);
#endif
#if WII_STREAM_MEMORY_DIAGNOSTICS
			uint64 removalStartTicks = gettime();
#endif
			bool didRemove = RemoveLeastUsedModel(STREAMFLAGS_20);
#if WII_STREAM_MEMORY_DIAGNOSTICS
			removalTicks += gettime() - removalStartTicks;
#endif
			if(didRemove){
				WiiMemoryGetPoolSnapshot(&poolBefore);
				pressure = WiiStreamGetStreamingPressureForSnapshotWithAdmission(
					poolBefore);
				removed++;
				budgetRemovals++;
			}
		}
	}
	bool unresolvedHardPressure = WiiStreamHardPressureBits(pressure) != 0;
	if(removed > 0){
		static uint32 sTrimReports = 0;
		sTrimReports++;
		if(sTrimReports <= 16 || (sTrimReports & (sTrimReports - 1)) == 0)
			SYS_Report("[WII-STREAM] trim flags=0x%X removed=%d budget=%d pressure=%d ineffective=%d used=%uKB->%uKB target=%uKB cap=%uKB archive=%uKB request=%uKB reserve=%uKB guard=%uKB reclaimed=g%u/nl%u/gx%uKB remaining=0x%X hard=0x%X admission=0x%X blocked=0x%X count=%u\n",
			           (unsigned)initialPressure, removed, budgetRemovals,
			           pressureRemovals, ineffectiveRemovals,
			           (unsigned)(before / 1024u),
			           (unsigned)(ms_memoryUsed / 1024u),
			           (unsigned)(archiveTarget / 1024u),
			           (unsigned)(ms_memoryAvailable / 1024u),
			           (unsigned)(archiveCeiling / 1024u),
			           (unsigned)(requested / 1024u),
			           (unsigned)(transitionReserve / 1024u),
			           (unsigned)(archiveTransitionGuard / 1024u),
			           (unsigned)(reclaimedByPool[0] / 1024u),
			           (unsigned)(reclaimedByPool[1] / 1024u),
			           (unsigned)(reclaimedByPool[2] / 1024u),
			           (unsigned)pressure,
			           (unsigned)WiiStreamHardPressureBits(pressure),
			           (unsigned)WiiStreamAdmissionPressureBits(pressure),
			           (unsigned)blockedPressure,
			           (unsigned)sTrimReports);
	}
#if WII_STREAM_MEMORY_DIAGNOSTICS
	WiiStreamDiagEndTrim();
#endif
	// Synchronous loading has no safe requeue consumer: LoadAllRequestedModels
	// would immediately retry the same request and spin while the pressure bit
	// remains set. Let its established hard-OOM fallback handle that case;
	// asynchronous conversion still fails closed and is requeued safely.
	if(unresolvedHardPressure && !gWiiStreamSynchronousLoad){
#if WII_STREAM_MEMORY_DIAGNOSTICS
		WiiStreamRecordFrameWork(gettime() - makeSpaceStartTicks, removalTicks,
		                         removed, archiveRemovals, pressureRemovals);
#endif
		return false;
	}
	if(sameFrameDeferred && pressure != 0 && !gWiiStreamSynchronousLoad){
		static uint32 sConversionDeferrals = 0;
		sConversionDeferrals++;
		if(sConversionDeferrals <= 16 ||
		   (sConversionDeferrals & (sConversionDeferrals - 1)) == 0)
			SYS_Report("[WII-STREAM] conversion deferred size=%uKB pressure=0x%X hard=0x%X admission=0x%X blocked=0x%X count=%u\n",
			           (unsigned)(requested / 1024u), (unsigned)pressure,
			           (unsigned)WiiStreamHardPressureBits(pressure),
			           (unsigned)WiiStreamAdmissionPressureBits(pressure),
			           (unsigned)blockedPressure,
			           (unsigned)sConversionDeferrals);
#if WII_STREAM_MEMORY_DIAGNOSTICS
		WiiStreamRecordFrameWork(gettime() - makeSpaceStartTicks, removalTicks,
		                         removed, archiveRemovals, pressureRemovals);
#endif
		return false;
	}
	#if WII_STREAM_MEMORY_DIAGNOSTICS
	WiiStreamRecordFrameWork(gettime() - makeSpaceStartTicks, removalTicks,
	                         removed, archiveRemovals, pressureRemovals);
	#endif
	return true;
#else
	while(ms_memoryUsed >= ms_memoryAvailable - size)
		if(!RemoveLeastUsedModel(STREAMFLAGS_20)){
			DeleteRwObjectsBehindCamera(ms_memoryAvailable - size);
			return true;
		}
	return true;
#endif
}

void
CStreaming::LoadScene(const CVector &pos)
{
	CStreamingInfo *si, *prev;
	eLevelName level;

	level = CTheZones::GetLevelFromPosition(&pos);
	debug("Start load scene\n");
	for(si = ms_endRequestedList.m_prev; si != &ms_startRequestedList; si = prev){
		prev = si->m_prev;
		if((si->m_flags & (STREAMFLAGS_KEEP_IN_MEMORY|STREAMFLAGS_PRIORITY)) == 0)
			RemoveModel(si - ms_aInfoForModel);
	}
	CRenderer::m_loadingPriority = false;
	DeleteAllRwObjects();
	if(level == LEVEL_GENERIC)
		level = CGame::currLevel;
	CGame::currLevel = level;
	RemoveUnusedBigBuildings(level);
	RequestBigBuildings(level, pos);
	RequestBigBuildings(LEVEL_GENERIC, pos);
	RemoveIslandsNotUsed(level);
	LoadAllRequestedModels(false);
	InstanceBigBuildings(level, pos);
	InstanceBigBuildings(LEVEL_GENERIC, pos);
	AddModelsToRequestList(pos, STREAMFLAGS_20);
	CRadar::StreamRadarSections(pos);

	if (!CGame::IsInInterior()) {
		for (int i = 0; i < 5; i++) {
			CZoneInfo zone;
			CTheZones::GetZoneInfoForTimeOfDay(&pos, &zone);
			int32 model = CCarCtrl::ChooseCarModelToLoad(CCarCtrl::ChooseCarRating(&zone));
			CStreaming::RequestModel(model, STREAMFLAGS_DEPENDENCY);
		}
	}
	LoadAllRequestedModels(false);
	InstanceLoadedModels(pos);

	for(int i = 0; i < NUMSTREAMINFO; i++)
		ms_aInfoForModel[i].m_flags &= ~STREAMFLAGS_20;
	debug("End load scene\n");
}

void
CStreaming::LoadSceneCollision(const CVector &pos)
{
	CColStore::LoadCollision(pos);
	CStreaming::LoadAllRequestedModels(false);
}

void
CStreaming::MemoryCardSave(uint8 *buf, uint32 *size)
{
	int i;

	*size = NUM_DEFAULT_MODELS;
	for(i = 0; i < NUM_DEFAULT_MODELS; i++)
		if(ms_aInfoForModel[i].m_loadState == STREAMSTATE_LOADED)
			buf[i] = ms_aInfoForModel[i].m_flags;
		else
			buf[i] = 0xFF;
}

void
CStreaming::MemoryCardLoad(uint8 *buf, uint32 size)
{
	uint32 i;

	assert(size == NUM_DEFAULT_MODELS);
	for(i = 0; i < size; i++)
		if(ms_aInfoForModel[i].m_loadState == STREAMSTATE_LOADED)
			if(buf[i] != 0xFF)
				ms_aInfoForModel[i].m_flags = buf[i];
}

void
CStreaming::UpdateForAnimViewer(void)
{
	if (CStreaming::ms_channelError == -1) {
		CStreaming::AddModelsToRequestList(CVector(0.0f, 0.0f, 0.0f), 0);
		CStreaming::LoadRequestedModels();
		// original modifier was %d
		sprintf(gString, "Requested %d, memory size %zuK\n", CStreaming::ms_numModelsRequested, 2 * CStreaming::ms_memoryUsed);
	}
	else {
		CStreaming::RetryLoadFile(CStreaming::ms_channelError);
	}
}


void
CStreaming::PrintStreamingBufferState()
{
	char str[128];
	wchar wstr[128];
	uint32 offset, size;

	CTimer::Stop();
	int i = 0;
	while (i < NUMSTREAMINFO) {
		while (true) {
			int j = 0;
			DoRWStuffStartOfFrame(50, 50, 50, 0, 0, 0, 255);
			CPad::UpdatePads();
			CSprite2d::InitPerFrame();
			CFont::InitPerFrame();
			DefinedState();

			CRect unusedRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
			CRGBA unusedColor(255, 255, 255, 255);
			CFont::SetFontStyle(FONT_BANK);
			CFont::SetBackgroundOff();
			CFont::SetWrapx(DEFAULT_SCREEN_WIDTH);
			CFont::SetScale(0.5f, 0.75f);
			CFont::SetCentreOff();
			CFont::SetCentreSize(DEFAULT_SCREEN_WIDTH);
			CFont::SetJustifyOff();
			CFont::SetColor(CRGBA(200, 200, 200, 200));
			CFont::SetBackGroundOnlyTextOff();
			int modelIndex = i;
			if (modelIndex < NUMSTREAMINFO) {
				int y = 24;
				for ( ; j < 34 && modelIndex < NUMSTREAMINFO; modelIndex++) {
					CStreamingInfo *streamingInfo = &ms_aInfoForModel[modelIndex];
					CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(modelIndex);
					if (streamingInfo->m_loadState != STREAMSTATE_LOADED || !streamingInfo->GetCdPosnAndSize(offset, size))
						continue;

					if (modelIndex >= STREAM_OFFSET_TXD)
						sprintf(str, "txd %s, refs %d, size %dK, flags 0x%x", CTxdStore::GetTxdName(modelIndex - STREAM_OFFSET_TXD),
						        CTxdStore::GetNumRefs(modelIndex - STREAM_OFFSET_TXD), 2 * size, streamingInfo->m_flags);
					else
						sprintf(str, "model %d,%s, refs%d, size%dK, flags%x", modelIndex, modelInfo->GetModelName(), modelInfo->GetNumRefs(), 2 * size,
						        streamingInfo->m_flags);
					AsciiToUnicode(str, wstr);
					CFont::PrintString(24.0f, y, wstr);
					y += 12;
					j++;
				}
			}

			if (CPad::GetPad(1)->GetCrossJustDown())
				i = modelIndex;

			if (!CPad::GetPad(1)->GetTriangleJustDown())
				break;

			i = 0;
			CFont::DrawFonts();
			DoRWStuffEndOfFrame();
		}
		CFont::DrawFonts();
		DoRWStuffEndOfFrame();
	}
	CTimer::Update();
}
