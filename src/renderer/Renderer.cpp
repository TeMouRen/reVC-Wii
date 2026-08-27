#define WITHD3D
#include "common.h"

#include "main.h"
#include "Lights.h"
#include "ModelInfo.h"
#include "Treadable.h"
#include "Ped.h"
#include "Vehicle.h"
#include "Boat.h"
#include "Heli.h"
#include "Bike.h"
#include "Object.h"
#include "PathFind.h"
#include "Collision.h"
#include "VisibilityPlugins.h"
#include "Clock.h"
#include "World.h"
#include "Camera.h"
#include "ModelIndices.h"
#include "Streaming.h"
#include "Shadows.h"
#include "PointLights.h"
#include "Occlusion.h"
#include "Renderer.h"
#include "custompipes.h"
#include "Frontend.h"

bool gbShowPedRoadGroups;
bool gbShowCarRoadGroups;
bool gbShowCollisionPolys;
bool gbShowCollisionPolysReflections;
bool gbShowCollisionPolysNoShadows;
bool gbShowCollisionLines;
bool gbBigWhiteDebugLightSwitchedOn;

bool gbDontRenderBuildings;
bool gbDontRenderBigBuildings;
bool gbDontRenderPeds;
bool gbDontRenderObjects;
bool gbDontRenderVehicles;

#ifdef WII
static inline bool
WiiDisableDistanceFade(const CSimpleModelInfo *mi)
{
	if(mi == nil)
		return false;
	if(TheCamera.Cams[TheCamera.ActiveCam].Mode != CCam::MODE_1STPERSON)
		return false;
	CVehicle *veh = FindPlayerVehicle();
	if(veh == nil)
		return false;
	if(veh->m_vecMoveSpeed.Magnitude2D() < 0.03f)
		return false;
	return mi->m_drawLast || mi->m_additive || mi->m_noZwrite;
}

struct WiiLodHoleFillPair
{
	CEntity *lod;
	CEntity *nearEntity;
};

static WiiLodHoleFillPair gWiiLodHoleFillPairs[NUMVISIBLEENTITIES];
static int32 gWiiLodHoleFillPairCount;
#if WII_STREAM_BIG_BUILDING_PROBE
static uint32 gWiiLodHoleFillProbeFrame;
#endif

// Find the actual near entity referenced by this world LOD. The near model is
// used only outside its normal fade range and only after its streamed resource
// is already loaded; this does not extend the streaming or GX admission policy.
static CEntity *
WiiPrepareRelatedNearEntity(CEntity *lod)
{
	if(lod == nil || !lod->IsBuilding() || !lod->bIsBIGBuilding ||
	   !lod->bIsVisible || lod->m_rwObject == nil)
		return nil;

	CBaseModelInfo *lodBase = CModelInfo::GetModelInfo(lod->GetModelIndex());
	if(lodBase == nil || !lodBase->IsSimple())
		return nil;
	CSimpleModelInfo *lodMi = (CSimpleModelInfo*)lodBase;
	CSimpleModelInfo *nearMi = lodMi->GetRelatedModel();
	if(nearMi == nil || nearMi->GetRwObject() == nil ||
	   nearMi->m_drawLast || nearMi->m_additive || nearMi->m_noZwrite)
		return nil;

	const float radius = 32.0f;
	const CVector &lodPos = lod->GetPosition();
	int minX = CWorld::GetSectorIndexX(lodPos.x - radius);
	int minY = CWorld::GetSectorIndexY(lodPos.y - radius);
	int maxX = CWorld::GetSectorIndexX(lodPos.x + radius);
	int maxY = CWorld::GetSectorIndexY(lodPos.y + radius);
	if(minX < 0) minX = 0;
	if(minY < 0) minY = 0;
	if(maxX >= NUMSECTORS_X) maxX = NUMSECTORS_X - 1;
	if(maxY >= NUMSECTORS_Y) maxY = NUMSECTORS_Y - 1;

	CEntity *nearest = nil;
	float nearestDistanceSq = SQR(0.25f);
	for(int y = minY; y <= maxY; y++)
		for(int x = minX; x <= maxX; x++){
			CSector *sector = CWorld::GetSector(x, y);
			CPtrList *lists[2] = {
				&sector->m_lists[ENTITYLIST_BUILDINGS],
				&sector->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]
			};
			for(int listIndex = 0; listIndex < 2; listIndex++)
				for(CPtrNode *node = lists[listIndex]->first; node; node = node->next){
					CEntity *candidate = (CEntity*)node->item;
					if(candidate == nil || candidate == lod || !candidate->IsBuilding() ||
					   candidate->bIsBIGBuilding || !candidate->bIsVisible ||
					   candidate->m_area != lod->m_area ||
					   candidate->m_level != lod->m_level ||
					   !candidate->GetIsOnScreen() ||
					   CModelInfo::GetModelInfo(candidate->GetModelIndex()) != nearMi)
						continue;
					float distanceSq = (candidate->GetPosition() - lodPos).MagnitudeSqr();
					if(distanceSq <= nearestDistanceSq &&
					   DotProduct(candidate->GetForward(), lod->GetForward()) > 0.999f &&
					   DotProduct(candidate->GetUp(), lod->GetUp()) > 0.999f){
						nearest = candidate;
						nearestDistanceSq = distanceSq;
					}
				}
		}

	if(nearest == nil || nearest->bDrawLast)
		return nil;
	int32 modelId = nearest->GetModelIndex();
	if(modelId < 0 || modelId >= STREAM_OFFSET_TXD ||
	   CStreaming::ms_aInfoForModel[modelId].m_loadState != STREAMSTATE_LOADED)
		return nil;

	float cameraDistance = (nearest->GetPosition() - TheCamera.GetPosition()).Magnitude();
	if(cameraDistance >= LOD_DISTANCE ||
	   nearMi->GetAtomicFromDistance(cameraDistance - FADE_DISTANCE) != nil)
		return nil;

	if(nearest->m_rwObject == nil)
		nearest->CreateRwObject();
	if(nearest->m_rwObject == nil || RwObjectGetType(nearest->m_rwObject) != rpATOMIC)
		return nil;

	RpAtomic *nearAtomic = nearMi->GetFirstAtomicFromDistance(0.0f);
	RpAtomic *entityAtomic = (RpAtomic*)nearest->m_rwObject;
	if(nearAtomic == nil)
		return nil;
	if(RpAtomicGetGeometry(nearAtomic) != RpAtomicGetGeometry(entityAtomic))
		RpAtomicSetGeometry(entityAtomic, RpAtomicGetGeometry(nearAtomic),
		                    rpATOMICSAMEBOUNDINGSPHERE);
	// This model was loaded before its normal draw range, so finish the
	// load-time alpha ramp before using it as the LOD replacement. Distance
	// fading still applies when the entity reaches its configured range.
	nearMi->m_alpha = 255;
	return nearest;
}

static void
WiiQueueLodHoleFill(CEntity *lod)
{
	if(gWiiLodHoleFillPairCount >= NUMVISIBLEENTITIES)
		return;
	for(int32 i = 0; i < gWiiLodHoleFillPairCount; i++)
		if(gWiiLodHoleFillPairs[i].lod == lod)
			return;
	CEntity *nearEntity = WiiPrepareRelatedNearEntity(lod);
	if(nearEntity == nil)
		return;
	WiiLodHoleFillPair &pair = gWiiLodHoleFillPairs[gWiiLodHoleFillPairCount++];
	pair.lod = lod;
	pair.nearEntity = nearEntity;
#if WII_STREAM_BIG_BUILDING_PROBE
	uint32 frame = CTimer::GetFrameCounter();
	if(lod->GetModelIndex() == 775 && frame - gWiiLodHoleFillProbeFrame >= 120){
		gWiiLodHoleFillProbeFrame = frame;
		printf("[WII-LOD-HOLE-FILL] frame=%u lod=%d('%s') near=%d('%s') "
		       "dist=%.3f mode=loaded-near-replaces-lod\n",
		       (unsigned)frame, lod->GetModelIndex(),
		       CModelInfo::GetModelInfo(lod->GetModelIndex())->GetModelName(),
		       nearEntity->GetModelIndex(),
		       CModelInfo::GetModelInfo(nearEntity->GetModelIndex())->GetModelName(),
		       (nearEntity->GetPosition() - TheCamera.GetPosition()).Magnitude());
	}
#endif
}

static bool
WiiIsLodHoleFillQueued(CEntity *lod)
{
	for(int32 i = 0; i < gWiiLodHoleFillPairCount; i++)
		if(gWiiLodHoleFillPairs[i].lod == lod)
			return true;
	return false;
}

static uint32 gWiiBigBuildingRequestFrame = UINT32_MAX;
static int32 gWiiBigBuildingRequestsThisFrame;

#if WII_STREAM_BIG_BUILDING_PROBE
struct WiiLodCompanionProbeCandidate
{
	CEntity *entity;
	float distanceSq;
};

static uint8 gWiiLodCompanionProbeLastState[STREAM_OFFSET_TXD];
static uint8 gWiiLodCompanionProbeLastRw[STREAM_OFFSET_TXD];
static bool gWiiLodCompanionProbeInitialized;
static uint8 gWiiNearLodEntityProbeLastState[STREAM_OFFSET_TXD];
static uint8 gWiiNearLodEntityProbeLastVis[STREAM_OFFSET_TXD];
static bool gWiiNearLodEntityProbeSeen[STREAM_OFFSET_TXD];
static uint8 gWiiBigProbeInvisibleSeen[STREAM_OFFSET_TXD];

struct WiiCamJonesDrawProbeState
{
	bool valid;
	int32 visibility;
	uint8 modelAlpha;
	uint8 distanceFade;
	uint8 rw;
	const void *nearAtomic;
	const void *fadeAtomic;
	const void *selectedAtomic;
	const void *selectedGeometry;
	const void *entityGeometry;
	uint8 relatedAlpha;
	uint8 relatedRw;
	uint8 extendedNear;
};

static WiiCamJonesDrawProbeState gWiiCamJonesScanProbeState[3];
static WiiCamJonesDrawProbeState gWiiCamJonesRenderProbeState[3];

static int
WiiCamJonesProbeSlot(int32 modelId)
{
	if(modelId == 775)
		return 0;
	if(modelId == 720)
		return 1;
	if(modelId == 826)
		return 2;
	return -1;
}

static void WiiProbeCamJonesRender(CEntity *ent, RpAtomic *drawAtomic,
	RpAtomic *lodAtomic, uint32 drawAlpha, const char *reason);

static const char *
WiiLodCompanionProbeStateName(uint8 state)
{
	switch(state){
	case STREAMSTATE_NOTLOADED: return "notloaded";
	case STREAMSTATE_LOADED: return "loaded";
	case STREAMSTATE_INQUEUE: return "inqueue";
	case STREAMSTATE_READING: return "reading";
	case STREAMSTATE_STARTED: return "started";
	default: return "unknown";
	}
}

static void
WiiProbeLodCompanions(CEntity *lod)
{
	if(lod == nil || !lod->bIsBIGBuilding || !lod->bIsVisible)
		return;
	if(!gWiiLodCompanionProbeInitialized){
		memset(gWiiLodCompanionProbeLastState, 0xFF,
		       sizeof(gWiiLodCompanionProbeLastState));
		memset(gWiiLodCompanionProbeLastRw, 0xFF,
		       sizeof(gWiiLodCompanionProbeLastRw));
		gWiiLodCompanionProbeInitialized = true;
	}

	const float radius = 20.0f;
	const int maxCandidates = 8;
	WiiLodCompanionProbeCandidate nearest[maxCandidates];
	int nearestCount = 0;
	const CVector &pos = lod->GetPosition();
	int minX = CWorld::GetSectorIndexX(pos.x - radius);
	int minY = CWorld::GetSectorIndexY(pos.y - radius);
	int maxX = CWorld::GetSectorIndexX(pos.x + radius);
	int maxY = CWorld::GetSectorIndexY(pos.y + radius);
	if(minX < 0) minX = 0;
	if(minY < 0) minY = 0;
	if(maxX >= NUMSECTORS_X) maxX = NUMSECTORS_X - 1;
	if(maxY >= NUMSECTORS_Y) maxY = NUMSECTORS_Y - 1;

	for(int y = minY; y <= maxY; y++){
		for(int x = minX; x <= maxX; x++){
			CSector *sector = CWorld::GetSector(x, y);
			CPtrList *lists[2] = {
				&sector->m_lists[ENTITYLIST_BUILDINGS],
				&sector->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]
			};
			for(int listIndex = 0; listIndex < 2; listIndex++){
				for(CPtrNode *node = lists[listIndex]->first; node; node = node->next){
					CEntity *candidate = (CEntity*)node->item;
					if(candidate == nil || candidate == lod || !candidate->IsBuilding() ||
					   candidate->bIsBIGBuilding)
						continue;
					int32 modelId = candidate->GetModelIndex();
					if(modelId < 0 || modelId >= STREAM_OFFSET_TXD)
						continue;
					float distanceSq = (candidate->GetPosition() - pos).MagnitudeSqr();
					if(distanceSq > SQR(radius))
						continue;

					int insert = nearestCount;
					if(insert > maxCandidates)
						insert = maxCandidates;
					while(insert > 0 &&
					      nearest[insert - 1].distanceSq > distanceSq)
						insert--;
					if(insert >= maxCandidates)
						continue;
					if(nearestCount < maxCandidates)
						nearestCount++;
					for(int move = nearestCount - 1; move > insert; move--)
						nearest[move] = nearest[move - 1];
					nearest[insert].entity = candidate;
					nearest[insert].distanceSq = distanceSq;
				}
			}
		}
	}

	for(int i = 0; i < nearestCount; i++){
		CEntity *candidate = nearest[i].entity;
		int32 modelId = candidate->GetModelIndex();
		uint8 state = CStreaming::ms_aInfoForModel[modelId].m_loadState;
		uint8 rw = candidate->m_rwObject != nil;
		if(gWiiLodCompanionProbeLastState[modelId] == state &&
		   gWiiLodCompanionProbeLastRw[modelId] == rw)
			continue;
		gWiiLodCompanionProbeLastState[modelId] = state;
		gWiiLodCompanionProbeLastRw[modelId] = rw;
		CBaseModelInfo *candidateInfo = CModelInfo::GetModelInfo(modelId);
		const CVector &cameraPos = TheCamera.GetPosition();
		const CVector &candidatePos = candidate->GetPosition();
		printf("[WII-LOD-COMPANION] frame=%u lod=%d('%s') candidate=%d('%s') "
		       "lod_pos=(%.3f,%.3f,%.3f) candidate_pos=(%.3f,%.3f,%.3f) "
		       "cam=(%.3f,%.3f,%.3f) dist=%.3f state=%u(%s) rw=%u visible=%u "
		       "flags=0x%02X requested=%d priority=%d\n",
		       (unsigned)CTimer::GetFrameCounter(), lod->GetModelIndex(),
		       CModelInfo::GetModelInfo(lod->GetModelIndex())->GetModelName(),
		       modelId, candidateInfo ? candidateInfo->GetModelName() : "<unknown>",
		       pos.x, pos.y, pos.z,
		       candidatePos.x, candidatePos.y, candidatePos.z,
		       cameraPos.x, cameraPos.y, cameraPos.z,
		       Sqrt(nearest[i].distanceSq), (unsigned)state,
		       WiiLodCompanionProbeStateName(state), (unsigned)rw,
		       (unsigned)candidate->bIsVisible,
		       (unsigned)CStreaming::ms_aInfoForModel[modelId].m_flags,
		       state != STREAMSTATE_NOTLOADED,
		       CStreaming::ms_aInfoForModel[modelId].IsPriority());
	}
}

#endif
#endif

// unused
int16 TestCloseThings;
int16 TestBigThings;

struct EntityInfo
{
	CEntity *ent;
	float sort;
};

CLinkList<EntityInfo> gSortedVehiclesAndPeds;

int32 CRenderer::ms_nNoOfVisibleEntities;
CEntity *CRenderer::ms_aVisibleEntityPtrs[NUMVISIBLEENTITIES];
CEntity *CRenderer::ms_aInVisibleEntityPtrs[NUMINVISIBLEENTITIES];
int32 CRenderer::ms_nNoOfInVisibleEntities;
#ifdef NEW_RENDERER
int32 CRenderer::ms_nNoOfVisibleVehicles;
CEntity *CRenderer::ms_aVisibleVehiclePtrs[NUMVISIBLEENTITIES];
int32 CRenderer::ms_nNoOfVisibleBuildings;
CEntity *CRenderer::ms_aVisibleBuildingPtrs[NUMVISIBLEENTITIES];
#endif

CVUVECTOR CRenderer::ms_vecCameraPosition;
CVehicle *CRenderer::m_pFirstPersonVehicle;
bool CRenderer::m_loadingPriority;
#ifdef WII
float CRenderer::ms_lodDistScale = 1.1f;
#else
float CRenderer::ms_lodDistScale = 1.2f;
#endif

// unused
BlockedRange CRenderer::aBlockedRanges[16];
BlockedRange* CRenderer::pFullBlockedRanges;
BlockedRange* CRenderer::pEmptyBlockedRanges;

void
CRenderer::Init(void)
{
	gSortedVehiclesAndPeds.Init(40);
	SortBIGBuildings();
}

void
CRenderer::Shutdown(void)
{
	gSortedVehiclesAndPeds.Shutdown();
}

void
CRenderer::PreRender(void)
{
	int i;
	CLink<CVisibilityPlugins::AlphaObjectInfo> *node;

	for(i = 0; i < ms_nNoOfVisibleEntities; i++)
		ms_aVisibleEntityPtrs[i]->PreRender();

#ifdef NEW_RENDERER
	if(gbNewRenderer){
		for(i = 0; i < ms_nNoOfVisibleVehicles; i++)
			ms_aVisibleVehiclePtrs[i]->PreRender();
		// How is this done with cWorldStream?
		for(i = 0; i < ms_nNoOfVisibleBuildings; i++)
			ms_aVisibleBuildingPtrs[i]->PreRender();
		for(node = CVisibilityPlugins::m_alphaBuildingList.head.next;
		    node != &CVisibilityPlugins::m_alphaBuildingList.tail;
		    node = node->next)
			((CEntity*)node->item.entity)->PreRender();
	}
#endif

	for (i = 0; i < ms_nNoOfInVisibleEntities; i++) {
#ifdef SQUEEZE_PERFORMANCE
		if (ms_aInVisibleEntityPtrs[i]->IsVehicle() && ((CVehicle*)ms_aInVisibleEntityPtrs[i])->IsHeli())
#endif
		ms_aInVisibleEntityPtrs[i]->PreRender();
	}

	for(node = CVisibilityPlugins::m_alphaEntityList.head.next;
	    node != &CVisibilityPlugins::m_alphaEntityList.tail;
	    node = node->next)
		((CEntity*)node->item.entity)->PreRender();

	CHeli::SpecialHeliPreRender();
	CShadows::RenderExtraPlayerShadows();
}

void
CRenderer::RenderOneRoad(CEntity *e)
{
#ifndef FINAL
	if(gbDontRenderBuildings)
		return;
#endif
#ifndef MASTER
	if(gbShowCollisionPolys || gbShowCollisionPolysReflections || gbShowCollisionPolysNoShadows)
		CCollision::DrawColModel_Coloured(e->GetMatrix(), *CModelInfo::GetColModel(e->GetModelIndex()), e->GetModelIndex());
	else
#endif
	{
		PUSH_RENDERGROUP(CModelInfo::GetModelInfo(e->GetModelIndex())->GetModelName());

		e->Render();

		POP_RENDERGROUP();
	}
}

void
CRenderer::RenderOneNonRoad(CEntity *e)
{
	CPed *ped;
	CVehicle *veh;
	int i;
	bool resetLights;

#ifndef MASTER
	if(gbShowCollisionPolys || gbShowCollisionPolysReflections || gbShowCollisionPolysNoShadows){
		if(!e->IsVehicle()){
			CCollision::DrawColModel_Coloured(e->GetMatrix(), *CModelInfo::GetColModel(e->GetModelIndex()), e->GetModelIndex());
			return;
		}
	}else
#endif
#ifndef FINAL
	if(e->IsBuilding()){
		if(e->bIsBIGBuilding){
			if(gbDontRenderBigBuildings)
				return;
		}else{
			if(gbDontRenderBuildings)
				return;
		}
	}else
#endif
	if(e->IsPed()){
#ifndef FINAL
		if(gbDontRenderPeds)
			return;
#endif
		ped = (CPed*)e;
		if(ped->m_nPedState == PED_DRIVING)
			return;
	}
#ifndef FINAL
	else if(e->IsObject() || e->IsDummy()){
		if(gbDontRenderObjects)
			return;
	}else if(e->IsVehicle()){
		// re3 addition
		if(gbDontRenderVehicles)
			return;
	}
#endif

	PUSH_RENDERGROUP(CModelInfo::GetModelInfo(e->GetModelIndex())->GetModelName());

	resetLights = e->SetupLighting();

	if(e->IsVehicle()){
		// unfortunately can't use GetClump here
		CVisibilityPlugins::SetupVehicleVariables((RpClump*)e->m_rwObject);
		CVisibilityPlugins::InitAlphaAtomicList();
	}

	// Render Peds in vehicle before vehicle itself
	if(e->IsVehicle()){
		veh = (CVehicle*)e;
		if(veh->pDriver && veh->pDriver->m_nPedState == PED_DRIVING)
			veh->pDriver->Render();
		for(i = 0; i < 8; i++)
			if(veh->pPassengers[i] && veh->pPassengers[i]->m_nPedState == PED_DRIVING)
				veh->pPassengers[i]->Render();
		SetCullMode(rwCULLMODECULLNONE);
	}
	e->Render();

	if(e->IsVehicle()){
		e->bImBeingRendered = true;
		CVisibilityPlugins::RenderAlphaAtomics();
		e->bImBeingRendered = false;
		SetCullMode(rwCULLMODECULLBACK);
	}

	e->RemoveLighting(resetLights);

	POP_RENDERGROUP();
}

void
CRenderer::RenderFirstPersonVehicle(void)
{
	if(m_pFirstPersonVehicle == nil)
		return;
	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
	RenderOneNonRoad(m_pFirstPersonVehicle);
	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)FALSE);
}

inline bool IsRoad(CEntity *e) { return e->IsBuilding() && ((CSimpleModelInfo*)CModelInfo::GetModelInfo(e->GetModelIndex()))->m_wetRoadReflection; }

void
CRenderer::RenderRoads(void)
{
	int i;
	CEntity *e;

	PUSH_RENDERGROUP("CRenderer::RenderRoads");
	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	SetCullMode(rwCULLMODECULLBACK);
	DeActivateDirectional();
	SetAmbientColours();

	for(i = 0; i < ms_nNoOfVisibleEntities; i++){
		e = ms_aVisibleEntityPtrs[i];
		if(IsRoad(e))
			RenderOneRoad(e);
	}
	POP_RENDERGROUP();
}

inline bool PutIntoSortedVehicleList(CVehicle *veh)
{
	if(veh->IsBoat()){
		int mode = TheCamera.Cams[TheCamera.ActiveCam].Mode;
		if(mode == CCam::MODE_WHEELCAM ||
		   mode == CCam::MODE_1STPERSON && TheCamera.GetLookDirection() != LOOKING_FORWARD && TheCamera.GetLookDirection() != LOOKING_BEHIND ||
		   veh->m_rwObject == nil || RwObjectGetType(veh->m_rwObject) != rpCLUMP ||
		   CVisibilityPlugins::GetClumpAlpha((RpClump*)veh->m_rwObject) != 255)
			return false;
		return true;
	}else
		return veh->bTouchingWater;		
}

void
CRenderer::RenderEverythingBarRoads(void)
{
	int i;
	CEntity *e;
	EntityInfo ei;

	PUSH_RENDERGROUP("CRenderer::RenderEverythingBarRoads");
	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	SetCullMode(rwCULLMODECULLBACK);
	gSortedVehiclesAndPeds.Clear();

	for(i = 0; i < ms_nNoOfVisibleEntities; i++){
		e = ms_aVisibleEntityPtrs[i];

		if(IsRoad(e))
			continue;

#ifdef WII
		if(e->IsBuilding() && e->bIsBIGBuilding && WiiIsLodHoleFillQueued(e))
			continue;
#endif

#ifdef EXTENDED_PIPELINES
		if(CustomPipes::bRenderingEnvMap && (e->IsPed() || e->IsVehicle()))
			continue;
#endif

		if(e->IsVehicle() ||
		   e->IsPed() && e->m_rwObject != nil && RwObjectGetType(e->m_rwObject) == rpCLUMP &&
		   CVisibilityPlugins::GetClumpAlpha((RpClump*)e->m_rwObject) != 255){
			if(e->IsVehicle() && PutIntoSortedVehicleList((CVehicle*)e)){
				ei.ent = e;
				ei.sort = (ms_vecCameraPosition - e->GetPosition()).MagnitudeSqr();
				gSortedVehiclesAndPeds.InsertSorted(ei);
			}else{
				if(!CVisibilityPlugins::InsertEntityIntoSortedList(e, (ms_vecCameraPosition - e->GetPosition()).Magnitude())){
					printf("Ran out of space in alpha entity list");
					RenderOneNonRoad(e);
				}
			}
		}else
			RenderOneNonRoad(e);
	}
#ifdef WII
	// The loaded related model replaces its coarse LOD while it is outside the
	// normal draw range. Drawing only one geometry avoids both the brightness
	// change from overlapping fades and the LOD covering the sharper facade.
	for(i = 0; i < gWiiLodHoleFillPairCount; i++){
		RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)TRUE);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
		RenderOneNonRoad(gWiiLodHoleFillPairs[i].nearEntity);
	}
#endif
	POP_RENDERGROUP();
}

void
CRenderer::RenderBoats(void)
{
	CLink<EntityInfo> *node;

	PUSH_RENDERGROUP("CRenderer::RenderBoats");
	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	SetCullMode(rwCULLMODECULLBACK);

#ifdef NEW_RENDERER
	int i;
	CEntity *e;
	EntityInfo ei;
	if(gbNewRenderer){
		gSortedVehiclesAndPeds.Clear();
		// not the real thing
		for(i = 0; i < ms_nNoOfVisibleVehicles; i++){
			e = ms_aVisibleVehiclePtrs[i];
			if(e->IsVehicle() && PutIntoSortedVehicleList((CVehicle*)e)){
				ei.ent = e;
				ei.sort = (ms_vecCameraPosition - e->GetPosition()).MagnitudeSqr();
				gSortedVehiclesAndPeds.InsertSorted(ei);
			}
		}
	}
#endif

	for(node = gSortedVehiclesAndPeds.tail.prev;
	    node != &gSortedVehiclesAndPeds.head;
	    node = node->prev){
		CVehicle *v = (CVehicle*)node->item.ent;
		RenderOneNonRoad(v);
	}
	POP_RENDERGROUP();
}

#ifdef NEW_RENDERER
#ifndef LIBRW
#error "Need librw for EXTENDED_PIPELINES"
#endif
#include "WaterLevel.h"

enum {
	// blend passes
	PASS_NOZ,	// no z-write
	PASS_ADD,	// additive
	PASS_BLEND	// normal blend
};

static void
SetStencilState(int state)
{
	switch(state){
	// disable stencil
	case 0:
		rw::SetRenderState(rw::STENCILENABLE, FALSE);
		break;
	// test against stencil
	case 1:
		rw::SetRenderState(rw::STENCILENABLE, TRUE);
		rw::SetRenderState(rw::STENCILFUNCTION, rw::STENCILNOTEQUAL);
		rw::SetRenderState(rw::STENCILPASS, rw::STENCILKEEP);
		rw::SetRenderState(rw::STENCILFAIL, rw::STENCILKEEP);
		rw::SetRenderState(rw::STENCILZFAIL, rw::STENCILKEEP);
		rw::SetRenderState(rw::STENCILFUNCTIONMASK, 0xFF);
		rw::SetRenderState(rw::STENCILFUNCTIONREF, 0xFF);
		break;
	// write to stencil
	case 2:
		rw::SetRenderState(rw::STENCILENABLE, TRUE);
		rw::SetRenderState(rw::STENCILFUNCTION, rw::STENCILALWAYS);
		rw::SetRenderState(rw::STENCILPASS, rw::STENCILREPLACE);
		rw::SetRenderState(rw::STENCILFUNCTIONREF, 0xFF);
		break;
	}
}

void
CRenderer::RenderOneBuilding(CEntity *ent, float camdist)
{
	if(ent->m_rwObject == nil)
		return;

	ent->bImBeingRendered = true;	// TODO: this seems wrong, but do we even need it?

	assert(RwObjectGetType(ent->m_rwObject) == rpATOMIC);
	RpAtomic *atomic = (RpAtomic*)ent->m_rwObject;
	CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(ent->GetModelIndex());

	int pass = PASS_BLEND;
	if(mi->m_additive)	// very questionable
		pass = PASS_ADD;
	if(mi->m_noZwrite)
		pass = PASS_NOZ;

	if(ent->bDistanceFade){
		RpAtomic *lodatm;
		float fadefactor;
		uint32 alpha;
		bool usedCurrentAtomic = false;

		lodatm = mi->GetAtomicFromDistance(camdist - FADE_DISTANCE);
		if(lodatm == nil){
			lodatm = atomic;
			usedCurrentAtomic = true;
		}
		fadefactor = (mi->GetLargestLodDistance() - (camdist - FADE_DISTANCE))/FADE_DISTANCE;
		if(fadefactor > 1.0f)
			fadefactor = 1.0f;
		alpha = mi->m_alpha * fadefactor;
	#if WII_STREAM_BIG_BUILDING_PROBE
		WiiProbeCamJonesRender(ent, atomic, lodatm, alpha,
			usedCurrentAtomic ? "distance_fade_current_atomic" : "distance_fade");
	#endif

		if(alpha == 255)
			WorldRender::AtomicFirstPass(atomic, pass);
		else{
			// not quite sure what this is about, do we have to do that?
			RpGeometry *geo = RpAtomicGetGeometry(lodatm);
			if(geo != RpAtomicGetGeometry(atomic))
				RpAtomicSetGeometry(atomic, geo, rpATOMICSAMEBOUNDINGSPHERE);
			WorldRender::AtomicFullyTransparent(atomic, pass, alpha);
		}
	}else{
	#if WII_STREAM_BIG_BUILDING_PROBE
		WiiProbeCamJonesRender(ent, atomic, nil, 255, "opaque");
	#endif
		WorldRender::AtomicFirstPass(atomic, pass);
	}

	ent->bImBeingRendered = false;	// TODO: this seems wrong, but do we even need it?
}

void
CRenderer::RenderWorld(int pass)
{
	int i;
	CEntity *e;
	CLink<CVisibilityPlugins::AlphaObjectInfo> *node;

	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)TRUE);
	SetCullMode(rwCULLMODECULLBACK);
	DeActivateDirectional();
	SetAmbientColours();

	// Temporary...have to figure out sorting better
	switch(pass){
	case 0:
		// Roads
		PUSH_RENDERGROUP("CRenderer::RenderWorld - Roads");
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)FALSE);
		for(i = 0; i < ms_nNoOfVisibleBuildings; i++){
			e = ms_aVisibleBuildingPtrs[i];
			if(e->bIsBIGBuilding || IsRoad(e))
				RenderOneBuilding(e);
		}
		for(node = CVisibilityPlugins::m_alphaBuildingList.tail.prev;
		    node != &CVisibilityPlugins::m_alphaBuildingList.head;
		    node = node->prev){
			e = node->item.entity;
			if(e->bIsBIGBuilding || IsRoad(e))
				RenderOneBuilding(e, node->item.sort);
		}
		POP_RENDERGROUP();
		break;
	case 1:
		// Opaque
		PUSH_RENDERGROUP("CRenderer::RenderWorld - Opaque");
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)FALSE);
		for(i = 0; i < ms_nNoOfVisibleBuildings; i++){
			e = ms_aVisibleBuildingPtrs[i];
			if(!(e->bIsBIGBuilding || IsRoad(e)))
				RenderOneBuilding(e);
		}
		for(node = CVisibilityPlugins::m_alphaBuildingList.tail.prev;
		    node != &CVisibilityPlugins::m_alphaBuildingList.head;
		    node = node->prev){
			e = node->item.entity;
			if(!(e->bIsBIGBuilding || IsRoad(e)))
				RenderOneBuilding(e, node->item.sort);
		}
		// Now we have iterated through all visible buildings (unsorted and sorted)
		// and the transparency list is done.

		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, FALSE);
		WorldRender::RenderBlendPass(PASS_NOZ);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
		POP_RENDERGROUP();
		break;
	case 2:
		// Transparent
		PUSH_RENDERGROUP("CRenderer::RenderWorld - Transparent");
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
		RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDONE);
		WorldRender::RenderBlendPass(PASS_ADD);
		RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
		WorldRender::RenderBlendPass(PASS_BLEND);
		POP_RENDERGROUP();
		break;
	}
}

void
CRenderer::RenderPeds(void)
{
	int i;
	CEntity *e;

	PUSH_RENDERGROUP("CRenderer::RenderPeds");
	for(i = 0; i < ms_nNoOfVisibleVehicles; i++){
		e = ms_aVisibleVehiclePtrs[i];
		if(e->IsPed())
			RenderOneNonRoad(e);
	}
	POP_RENDERGROUP();
}

void
CRenderer::RenderVehicles(void)
{
	int i;
	CEntity *e;
	EntityInfo ei;
	CLink<EntityInfo> *node;

	PUSH_RENDERGROUP("CRenderer::RenderVehicles");
	// not the real thing
	for(i = 0; i < ms_nNoOfVisibleVehicles; i++){
		e = ms_aVisibleVehiclePtrs[i];
		if(!e->IsVehicle())
			continue;
		if(PutIntoSortedVehicleList((CVehicle*)e))
			continue;	// boats handled elsewhere
		ei.ent = e;
		ei.sort = (ms_vecCameraPosition - e->GetPosition()).MagnitudeSqr();
		gSortedVehiclesAndPeds.InsertSorted(ei);
	}

	for(node = gSortedVehiclesAndPeds.tail.prev;
	    node != &gSortedVehiclesAndPeds.head;
	    node = node->prev)
		RenderOneNonRoad(node->item.ent);
	POP_RENDERGROUP();
}

void
CRenderer::RenderTransparentWater(void)
{
	int i;
	CEntity *e;

	PUSH_RENDERGROUP("CRenderer::RenderTransparentWater");
	RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nil);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDZERO);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDONE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
	SetStencilState(2);

	for(i = 0; i < ms_nNoOfVisibleVehicles; i++){
		e = ms_aVisibleVehiclePtrs[i];
		if(e->IsVehicle() && ((CVehicle*)e)->IsBoat())
			((CBoat*)e)->RenderWaterOutPolys();
	}

	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
	SetStencilState(1);

	CWaterLevel::RenderTransparentWater();

	SetStencilState(0);
	POP_RENDERGROUP();
}

void
CRenderer::ClearForFrame(void)
{
	ms_nNoOfVisibleEntities = 0;
	ms_nNoOfVisibleVehicles = 0;
	ms_nNoOfVisibleBuildings = 0;
	ms_nNoOfInVisibleEntities = 0;
	gSortedVehiclesAndPeds.Clear();

	WorldRender::numBlendInsts[PASS_NOZ] = 0;
	WorldRender::numBlendInsts[PASS_ADD] = 0;
	WorldRender::numBlendInsts[PASS_BLEND] = 0;
}
#endif

void
CRenderer::RenderFadingInEntities(void)
{
	PUSH_RENDERGROUP("CRenderer::RenderFadingInEntities");
	RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
	SetCullMode(rwCULLMODECULLBACK);
	DeActivateDirectional();
	SetAmbientColours();
	CVisibilityPlugins::RenderFadingEntities();
	POP_RENDERGROUP();
}

void
CRenderer::RenderFadingInUnderwaterEntities(void)
{
	PUSH_RENDERGROUP("CRenderer::RenderFadingInUnderwaterEntities");
	DeActivateDirectional();
	SetAmbientColours();
	CVisibilityPlugins::RenderFadingUnderwaterEntities();
	POP_RENDERGROUP();
}

void
CRenderer::RenderCollisionLines(void)
{
	int i;

	// game doesn't draw fading in entities
	// this should probably be fixed
	for(i = 0; i < ms_nNoOfVisibleEntities; i++){
		CEntity *e = ms_aVisibleEntityPtrs[i];
		if(Abs(e->GetPosition().x - ms_vecCameraPosition.x) < 100.0f &&
		   Abs(e->GetPosition().y - ms_vecCameraPosition.y) < 100.0f)
			CCollision::DrawColModel(e->GetMatrix(), *e->GetColModel());
	}
}

enum Visbility
{
	VIS_INVISIBLE,
	VIS_VISIBLE,
	VIS_OFFSCREEN,
	VIS_STREAMME
};

#if WII_STREAM_BIG_BUILDING_PROBE
static const char *
WiiVisibilityProbeName(int32 visibility)
{
	switch(visibility){
	case VIS_VISIBLE: return "visible";
	case VIS_INVISIBLE: return "invisible";
	case VIS_OFFSCREEN: return "offscreen";
	case VIS_STREAMME: return "streamme";
	default: return "unknown";
	}
}

static bool
WiiProbeEntityNearVisibleLod(CEntity *candidate)
{
	if(candidate == nil || candidate->bIsBIGBuilding || !candidate->bIsVisible)
		return false;

	const float radius = 20.0f;
	const CVector &pos = candidate->GetPosition();
	int minX = CWorld::GetSectorIndexX(pos.x - radius);
	int minY = CWorld::GetSectorIndexY(pos.y - radius);
	int maxX = CWorld::GetSectorIndexX(pos.x + radius);
	int maxY = CWorld::GetSectorIndexY(pos.y + radius);
	if(minX < 0) minX = 0;
	if(minY < 0) minY = 0;
	if(maxX >= NUMSECTORS_X) maxX = NUMSECTORS_X - 1;
	if(maxY >= NUMSECTORS_Y) maxY = NUMSECTORS_Y - 1;

	for(int y = minY; y <= maxY; y++)
		for(int x = minX; x <= maxX; x++){
			CSector *sector = CWorld::GetSector(x, y);
			CPtrList *lists[2] = {
				&sector->m_lists[ENTITYLIST_BUILDINGS],
				&sector->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]
			};
			for(int listIndex = 0; listIndex < 2; listIndex++)
				for(CPtrNode *node = lists[listIndex]->first; node; node = node->next){
					CEntity *lod = (CEntity*)node->item;
					if(lod == nil || !lod->bIsBIGBuilding || !lod->bIsVisible)
						continue;
					if((lod->GetPosition() - pos).MagnitudeSqr() <= radius * radius)
						return true;
				}
		}
	return false;
}

static void
WiiProbeNearLodEntityVisibility(CEntity *ent, int32 visibility, const char *phase)
{
	if(ent == nil || ent->bIsBIGBuilding || !ent->IsBuilding() ||
	   !WiiProbeEntityNearVisibleLod(ent))
		return;

	int32 modelId = ent->GetModelIndex();
	if(modelId < 0 || modelId >= STREAM_OFFSET_TXD)
		return;
	uint8 state = CStreaming::ms_aInfoForModel[modelId].m_loadState;
	uint8 vis = (uint8)visibility;
	if(gWiiNearLodEntityProbeSeen[modelId] &&
	   gWiiNearLodEntityProbeLastState[modelId] == state &&
	   gWiiNearLodEntityProbeLastVis[modelId] == vis)
		return;
	gWiiNearLodEntityProbeSeen[modelId] = true;
	gWiiNearLodEntityProbeLastState[modelId] = state;
	gWiiNearLodEntityProbeLastVis[modelId] = vis;

	CBaseModelInfo *modelInfo = CModelInfo::GetModelInfo(modelId);
	const CVector &entityPos = ent->GetPosition();
	const CVector &cameraPos = TheCamera.GetPosition();
	float cameraDist = (entityPos - cameraPos).Magnitude();
	printf("[WII-LOD-ENTITY] phase=%s frame=%u model=%d name='%s' "
	       "state=%u(%s) rw=%u visible=%u onscreen=%u occluded=%u "
	       "ent=(%.3f,%.3f,%.3f) cam=(%.3f,%.3f,%.3f) "
	       "vis=%u(%s) dist=%.3f flags=0x%02X requested=%d priority=%d\n",
	       phase ? phase : "unknown", (unsigned)CTimer::GetFrameCounter(),
	       modelId, modelInfo && modelInfo->GetModelName() ?
	       modelInfo->GetModelName() : "<unknown>",
	       (unsigned)state, WiiLodCompanionProbeStateName(state),
	       ent->m_rwObject != nil ? 1u : 0u, ent->bIsVisible ? 1u : 0u,
	       ent->GetIsOnScreen() ? 1u : 0u, ent->IsEntityOccluded() ? 1u : 0u,
	       entityPos.x, entityPos.y, entityPos.z,
	       cameraPos.x, cameraPos.y, cameraPos.z,
	       (unsigned)vis, WiiVisibilityProbeName(visibility), cameraDist,
	       (unsigned)CStreaming::ms_aInfoForModel[modelId].m_flags,
	       CStreaming::ms_aInfoForModel[modelId].m_loadState != STREAMSTATE_NOTLOADED,
	       CStreaming::ms_aInfoForModel[modelId].IsPriority());
}

static void
WiiProbeCamJonesScan(CEntity *ent, int32 visibility, const char *phase)
{
	if(ent == nil)
		return;
	int32 modelId = ent->GetModelIndex();
	int slot = WiiCamJonesProbeSlot(modelId);
	if(slot < 0)
		return;

	CBaseModelInfo *base = CModelInfo::GetModelInfo(modelId);
	if(base == nil || !base->IsSimple())
		return;
	CSimpleModelInfo *mi = (CSimpleModelInfo*)base;
	float distance = (TheCamera.GetPosition() - ent->GetPosition()).Magnitude();
	RpAtomic *nearAtomic = modelId == 775 ?
		mi->GetFirstAtomicFromDistance(distance) :
		mi->GetAtomicFromDistance(distance);
	RpAtomic *fadeAtomic = modelId == 775 ?
		mi->GetFirstAtomicFromDistance(distance - FADE_DISTANCE) :
		mi->GetAtomicFromDistance(distance - FADE_DISTANCE);
	RpAtomic *selectedAtomic = nearAtomic ? nearAtomic : fadeAtomic;
	RpGeometry *selectedGeometry = selectedAtomic ?
		RpAtomicGetGeometry(selectedAtomic) : nil;
	RpGeometry *entityGeometry = ent->m_rwObject ?
		RpAtomicGetGeometry((RpAtomic*)ent->m_rwObject) : nil;
	CSimpleModelInfo *related = modelId == 775 ? mi->GetRelatedModel() : nil;
	RpAtomic *firstAtomic = mi->GetFirstAtomicFromDistance(0.0f);
	bool extendedNear = modelId == 720 && nearAtomic == nil &&
		ent->m_rwObject != nil && firstAtomic != nil &&
		entityGeometry == RpAtomicGetGeometry(firstAtomic) &&
		distance < LOD_DISTANCE;

	WiiCamJonesDrawProbeState state;
	state.valid = true;
	state.visibility = visibility;
	state.modelAlpha = mi->m_alpha;
	state.distanceFade = ent->bDistanceFade ? 1 : 0;
	state.rw = ent->m_rwObject ? 1 : 0;
	state.nearAtomic = nearAtomic;
	state.fadeAtomic = fadeAtomic;
	state.selectedAtomic = selectedAtomic;
	state.selectedGeometry = selectedGeometry;
	state.entityGeometry = entityGeometry;
	state.relatedAlpha = related ? related->m_alpha : 0;
	state.relatedRw = related && related->GetRwObject() ? 1 : 0;
	state.extendedNear = extendedNear ? 1 : 0;

	WiiCamJonesDrawProbeState &last = gWiiCamJonesScanProbeState[slot];
	if(last.valid &&
	   last.visibility == state.visibility &&
	   last.modelAlpha == state.modelAlpha &&
	   last.distanceFade == state.distanceFade &&
	   last.rw == state.rw &&
	   last.nearAtomic == state.nearAtomic &&
	   last.fadeAtomic == state.fadeAtomic &&
	   last.selectedAtomic == state.selectedAtomic &&
	   last.selectedGeometry == state.selectedGeometry &&
	   last.entityGeometry == state.entityGeometry &&
	   last.relatedAlpha == state.relatedAlpha &&
	   last.relatedRw == state.relatedRw &&
	   last.extendedNear == state.extendedNear)
		return;
	last = state;

	CStreamingInfo &stream = CStreaming::ms_aInfoForModel[modelId];
	const CVector &entityPos = ent->GetPosition();
	const CVector &cameraPos = TheCamera.GetPosition();
	printf("[WII-CAMJONES-DRAW] stage=scan phase=%s frame=%u "
	       "model=%d name='%s' vis=%u(%s) dist=%.3f "
	       "ent=(%.3f,%.3f,%.3f) cam=(%.3f,%.3f,%.3f) "
	       "m_alpha=%u bDistanceFade=%u rw=%u "
	       "near_atomic=%p fade_atomic=%p selected_atomic=%p "
	       "selected_source=%s selected_geo=%p entity_geo=%p "
	       "related=%p('%s') related_alpha=%u related_rw=%u "
	       "extended_near=%u stream=%u(%s)\n",
	       phase ? phase : "unknown", (unsigned)CTimer::GetFrameCounter(),
	       modelId, base->GetModelName() ? base->GetModelName() : "<unknown>",
	       (unsigned)visibility, WiiVisibilityProbeName(visibility), distance,
	       entityPos.x, entityPos.y, entityPos.z,
	       cameraPos.x, cameraPos.y, cameraPos.z,
	       (unsigned)mi->m_alpha, ent->bDistanceFade ? 1u : 0u,
	       ent->m_rwObject ? 1u : 0u,
	       (void*)nearAtomic, (void*)fadeAtomic, (void*)selectedAtomic,
	       nearAtomic ? "near" : fadeAtomic ? "fade" : "none",
	       (void*)selectedGeometry, (void*)entityGeometry,
	       (void*)related,
	       related && related->GetModelName() ? related->GetModelName() : "<none>",
	       related ? (unsigned)related->m_alpha : 0u,
	       related && related->GetRwObject() ? 1u : 0u,
	       extendedNear ? 1u : 0u,
	       (unsigned)stream.m_loadState, WiiLodCompanionProbeStateName(stream.m_loadState));
}

static void
WiiProbeCamJonesRender(CEntity *ent, RpAtomic *drawAtomic, RpAtomic *lodAtomic,
	uint32 drawAlpha, const char *reason)
{
	if(ent == nil)
		return;
	int slot = WiiCamJonesProbeSlot(ent->GetModelIndex());
	if(slot < 0)
		return;
	CBaseModelInfo *base = CModelInfo::GetModelInfo(ent->GetModelIndex());
	if(base == nil || !base->IsSimple())
		return;
	CSimpleModelInfo *mi = (CSimpleModelInfo*)base;
	RpGeometry *drawGeometry = drawAtomic ? RpAtomicGetGeometry(drawAtomic) : nil;
	RpGeometry *lodGeometry = lodAtomic ? RpAtomicGetGeometry(lodAtomic) : nil;
	WiiCamJonesDrawProbeState &last = gWiiCamJonesRenderProbeState[slot];
	if(last.valid && last.modelAlpha == mi->m_alpha &&
	   last.distanceFade == (ent->bDistanceFade ? 1 : 0) &&
	   last.rw == (ent->m_rwObject ? 1 : 0) &&
	   last.selectedAtomic == drawAtomic &&
	   last.selectedGeometry == drawGeometry &&
	   last.nearAtomic == lodAtomic &&
	   last.fadeAtomic == lodGeometry &&
	   last.relatedAlpha == (uint8)drawAlpha)
		return;
	last.valid = true;
	last.modelAlpha = mi->m_alpha;
	last.distanceFade = ent->bDistanceFade ? 1 : 0;
	last.rw = ent->m_rwObject ? 1 : 0;
	last.selectedAtomic = drawAtomic;
	last.selectedGeometry = drawGeometry;
	last.nearAtomic = lodAtomic;
	last.fadeAtomic = lodGeometry;
	last.relatedAlpha = (uint8)drawAlpha;

	printf("[WII-CAMJONES-DRAW] stage=render frame=%u model=%d name='%s' "
	       "m_alpha=%u bDistanceFade=%u rw=%u draw_alpha=%u "
	       "draw_atomic=%p draw_geo=%p lod_atomic=%p lod_geo=%p reason=%s\n",
	       (unsigned)CTimer::GetFrameCounter(), ent->GetModelIndex(),
	       base->GetModelName() ? base->GetModelName() : "<unknown>",
	       (unsigned)mi->m_alpha, ent->bDistanceFade ? 1u : 0u,
	       ent->m_rwObject ? 1u : 0u, (unsigned)drawAlpha,
	       (void*)drawAtomic, (void*)drawGeometry, (void*)lodAtomic,
	       (void*)lodGeometry, reason ? reason : "unknown");
}

static void
WiiProbeBigBuildingInvisible(CEntity *ent, int visibility)
{
	if(ent == nil || visibility != VIS_INVISIBLE || !ent->bIsBIGBuilding)
		return;
	int32 modelId = ent->GetModelIndex();
	if(modelId < 0 || modelId >= STREAM_OFFSET_TXD)
		return;
	CStreamingInfo &stream = CStreaming::ms_aInfoForModel[modelId];
	if(stream.m_loadState != STREAMSTATE_NOTLOADED ||
	   gWiiBigProbeInvisibleSeen[modelId])
		return;
	gWiiBigProbeInvisibleSeen[modelId] = 1;
	CBaseModelInfo *base = CModelInfo::GetModelInfo(modelId);
	CSimpleModelInfo *mi = base && base->IsSimple() ?
	                       (CSimpleModelInfo*)base : nil;
	const CVector &entityPos = ent->GetPosition();
	const CVector &cameraPos = TheCamera.GetPosition();
	float distance = (cameraPos - entityPos).Magnitude();
	printf("[WII-BIG] event=scan_invisible frame=%u model=%d name='%s' "
	       "model_state=%s rw=%d visible=%d on_screen=%d on_screen_complex=%d "
	       "occluded=%d offscreen=%d area=%d dist=%.1f lod0=%.1f "
	       "ent=(%.3f,%.3f,%.3f) cam=(%.3f,%.3f,%.3f) "
	       "requested=%d priority=%d flags=0x%02X reason=visibility\n",
	       (unsigned)CTimer::GetFrameCounter(), modelId,
	       base && base->GetModelName() ? base->GetModelName() : "<unnamed>",
	       WiiLodCompanionProbeStateName(stream.m_loadState),
	       ent->m_rwObject ? 1 : 0, ent->IsVisible() ? 1 : 0,
	       ent->GetIsOnScreen() ? 1 : 0, ent->GetIsOnScreenComplex() ? 1 : 0,
	       ent->IsEntityOccluded() ? 1 : 0, ent->bOffscreen ? 1 : 0,
	       (int)ent->m_area, distance, mi ? mi->GetLodDistance(0) : 0.0f,
	       entityPos.x, entityPos.y, entityPos.z,
	       cameraPos.x, cameraPos.y, cameraPos.z,
	       stream.m_loadState != STREAMSTATE_NOTLOADED ? 1 : 0,
	       stream.IsPriority() ? 1 : 0, (unsigned)stream.m_flags);
}
#endif

// Time Objects can be time culled if
//   other == -1 || CModelInfo::GetModelInfo(other)->GetRwObject()
// i.e. we have to draw even at the wrong time if
//   other != -1 && CModelInfo::GetModelInfo(other)->GetRwObject() == nil

#define OTHERUNAVAILABLE (other != -1 && CModelInfo::GetModelInfo(other)->GetRwObject() == nil)
#define CANTIMECULL (!OTHERUNAVAILABLE)

int32
CRenderer::SetupEntityVisibility(CEntity *ent)
{
	CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(ent->m_modelIndex);
	CTimeModelInfo *ti;
	int32 other;
	float dist;

	bool request = true;
	if(mi->GetModelType() == MITYPE_TIME){
 		ti = (CTimeModelInfo*)mi;
		other = ti->GetOtherTimeModel();
		if(CClock::GetIsTimeInRange(ti->GetTimeOn(), ti->GetTimeOff())){
			// don't fade in, or between time objects
			if(CANTIMECULL)
				ti->m_alpha = 255;
		}else{
			// Hide if possible
			if(CANTIMECULL){
				ent->DeleteRwObject();
				return VIS_INVISIBLE;
			}
			// can't cull, so we'll try to draw this one, but don't request
			// it since what we really want is the other one.
			request = false;
		}
	}else{
		if(mi->GetModelType() != MITYPE_SIMPLE && mi->GetModelType() != MITYPE_WEAPON){
			if(FindPlayerVehicle() == ent &&
			   TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_1STPERSON &&
			   !(FindPlayerVehicle()->IsBike() && ((CBike*)FindPlayerVehicle())->bWheelieCam)){
				// Player's vehicle in first person mode
				CVehicle *veh = (CVehicle*)ent;
				int model = veh->GetModelIndex();
				int direction = TheCamera.Cams[TheCamera.ActiveCam].DirectionWasLooking;
				if(direction == LOOKING_FORWARD ||
				   ent->GetModelIndex() == MI_RHINO ||
				   ent->GetModelIndex() == MI_COACH ||
				   TheCamera.m_bInATunnelAndABigVehicle ||
				   direction == LOOKING_BEHIND && veh->pHandling->Flags & HANDLING_UNKNOWN){
					ent->bNoBrightHeadLights = true;
					return VIS_OFFSCREEN;
				}

				if(direction != LOOKING_BEHIND ||
				   !veh->IsBoat() || model == MI_REEFER || model == MI_TROPIC || model == MI_PREDATOR || model == MI_SKIMMER){
					m_pFirstPersonVehicle = (CVehicle*)ent;
					ent->bNoBrightHeadLights = false;
					return VIS_OFFSCREEN;
				}
			}

			// All sorts of Clumps
			if(ent->m_rwObject == nil || !ent->bIsVisible)
				return VIS_INVISIBLE;
			if(!ent->GetIsOnScreen() || ent->IsEntityOccluded())
				return VIS_OFFSCREEN;
			if(ent->bDrawLast){
				dist = (ent->GetPosition() - ms_vecCameraPosition).Magnitude();
				CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist);
				ent->bDistanceFade = false;
				return VIS_INVISIBLE;
			}
			return VIS_VISIBLE;
		}
		if(ent->bDontStream){
			if(ent->m_rwObject == nil || !ent->bIsVisible)
				return VIS_INVISIBLE;
			if(!ent->GetIsOnScreen() || ent->IsEntityOccluded())
				return VIS_OFFSCREEN;
			if(ent->bDrawLast){
				dist = (ent->GetPosition() - ms_vecCameraPosition).Magnitude();
				CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist);
				ent->bDistanceFade = false;
				return VIS_INVISIBLE;
			}
			return VIS_VISIBLE;
		}
	}

	// Simple ModelInfo

	if(!IsAreaVisible(ent->m_area))
		return VIS_INVISIBLE;

	dist = (ent->GetPosition() - ms_vecCameraPosition).Magnitude();

#ifndef FIX_BUGS
	// Whatever this is supposed to do, it breaks fading for objects
	// whose draw dist is > LOD_DISTANCE-FADE_DISTANCE, i.e. 280
	// because decreasing dist here makes the object visible above LOD_DISTANCE
	// before fading normally once below LOD_DISTANCE.
	// aha! this must be a workaround for the fact that we're not taking
	// the LOD multiplier into account here anywhere
	if(LOD_DISTANCE < dist && dist < mi->GetLargestLodDistance() + FADE_DISTANCE)
		dist += mi->GetLargestLodDistance() - LOD_DISTANCE;
#endif

	if(ent->IsObject() && ent->bRenderDamaged)
		mi->m_isDamaged = true;

	RpAtomic *a = mi->GetAtomicFromDistance(dist);
	if(a){
		mi->m_isDamaged = false;
		if(ent->m_rwObject == nil)
			ent->CreateRwObject();
		if(ent->m_rwObject == nil)
			return VIS_INVISIBLE;
		RpAtomic *rwobj = (RpAtomic*)ent->m_rwObject;
		// Make sure our atomic uses the right geometry and not
		// that of an atomic for another draw distance.
		if(RpAtomicGetGeometry(a) != RpAtomicGetGeometry(rwobj))
			RpAtomicSetGeometry(rwobj, RpAtomicGetGeometry(a), rpATOMICSAMEBOUNDINGSPHERE); // originally 5 (mistake?)
		mi->IncreaseAlpha();
#ifdef WII
		if(WiiDisableDistanceFade(mi))
			mi->m_alpha = 255;
#endif
		if(ent->m_rwObject == nil || !ent->bIsVisible)
			return VIS_INVISIBLE;

		if(!ent->GetIsOnScreen() || ent->IsEntityOccluded()){
			mi->m_alpha = 255;
			return VIS_OFFSCREEN;
		}

		if(mi->m_alpha != 255){
			CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist);
			ent->bDistanceFade = true;
			return VIS_INVISIBLE;
		}

		if(mi->m_drawLast || ent->bDrawLast){
			if(CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist)){
				ent->bDistanceFade = false;
				return VIS_INVISIBLE;
			}
		}
		return VIS_VISIBLE;
	}

	// Object is not loaded, figure out what to do
	// On Wii, a short-range static building can sit in front of a loaded
	// world LOD (garage doors and similar facade pieces are common examples).
	// Admit those visible building entities across the existing world LOD
	// horizon so their normal RW instance is ready before the camera reaches
	// the near-model draw distance.  Keep the lookahead bounded to buildings
	// inside the current frustum; this does not change the streaming queue or
	// texture/GX policy.
	bool wiiVisibleBuildingLookahead = false;
#ifdef WII
	wiiVisibleBuildingLookahead =
		ent->IsBuilding() &&
		ent->GetIsOnScreen() &&
		dist < LOD_DISTANCE &&
		mi->GetLargestLodDistance() < LOD_DISTANCE;
#endif

	if(mi->m_noFade){
		mi->m_isDamaged = false;
		// request model
		if((dist - STREAM_DISTANCE < mi->GetLargestLodDistance() ||
		    wiiVisibleBuildingLookahead) && request)
			return VIS_STREAMME;
		return VIS_INVISIBLE;
	}

	// We might be fading

	a = mi->GetAtomicFromDistance(dist - FADE_DISTANCE);
	mi->m_isDamaged = false;
	if(a == nil){
		// request model
		if((dist - FADE_DISTANCE - STREAM_DISTANCE < mi->GetLargestLodDistance() ||
		    wiiVisibleBuildingLookahead) && request)
			return VIS_STREAMME;
		return VIS_INVISIBLE;
	}

	if(ent->m_rwObject == nil)
		ent->CreateRwObject();
	if(ent->m_rwObject == nil)
		return VIS_INVISIBLE;
	RpAtomic *rwobj = (RpAtomic*)ent->m_rwObject;
	if(RpAtomicGetGeometry(a) != RpAtomicGetGeometry(rwobj))
		RpAtomicSetGeometry(rwobj, RpAtomicGetGeometry(a), rpATOMICSAMEBOUNDINGSPHERE); // originally 5 (mistake?)
	mi->IncreaseAlpha();
#ifdef WII
	if(WiiDisableDistanceFade(mi))
		mi->m_alpha = 255;
#endif
	if(ent->m_rwObject == nil || !ent->bIsVisible)
		return VIS_INVISIBLE;

	if(!ent->GetIsOnScreen() || ent->IsEntityOccluded()){
		mi->m_alpha = 255;
		return VIS_OFFSCREEN;
	}else{
#ifdef WII
		if(WiiDisableDistanceFade(mi)){
			ent->bDistanceFade = false;
			if(mi->m_drawLast || ent->bDrawLast){
				if(CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist))
					return VIS_INVISIBLE;
			}
			return VIS_VISIBLE;
		}
#endif
		CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist);
		ent->bDistanceFade = true;
		return VIS_OFFSCREEN;	// Why this?
	}
}

int32
CRenderer::SetupBigBuildingVisibility(CEntity *ent)
{
	CSimpleModelInfo *mi = (CSimpleModelInfo*)CModelInfo::GetModelInfo(ent->m_modelIndex);
	CTimeModelInfo *ti;
	int32 other;

	if(!IsAreaVisible(ent->m_area))
		return VIS_INVISIBLE;

#ifdef WII
	// The current island's coarse proxy overlaps its detailed world geometry.
	// Keep only the opposite island proxy available for distant rendering.
	if(CStreaming::ShouldSuppressIslandLOD(ent->GetModelIndex())){
		if(ent->m_rwObject && !ent->bImBeingRendered)
			ent->DeleteRwObject();
		return VIS_INVISIBLE;
	}
#endif

	bool request = true;
	if(mi->GetModelType() == MITYPE_TIME){
		ti = (CTimeModelInfo*)mi;
		other = ti->GetOtherTimeModel();
		if(CClock::GetIsTimeInRange(ti->GetTimeOn(), ti->GetTimeOff())){
			// don't fade in, or between time objects
			if(CANTIMECULL)
				ti->m_alpha = 255;
		}else{
			// Hide if possible
			if(CANTIMECULL){
				ent->DeleteRwObject();
				return VIS_INVISIBLE;
			}
			// can't cull, so we'll try to draw this one, but don't request
			// it since what we really want is the other one.
			request = false;
		}
	}else if(mi->GetModelType() == MITYPE_VEHICLE)
		return ent->IsVisible() ? VIS_VISIBLE : VIS_INVISIBLE;

	float dist = (ms_vecCameraPosition-ent->GetPosition()).Magnitude();
	CSimpleModelInfo *nonLOD = mi->GetRelatedModel();

	// Find out whether to draw below near distance.
	// This is only the case if there is a non-LOD which is either not
	// loaded or not completely faded in yet.
	if(dist < mi->GetNearDistance() && dist < LOD_DISTANCE){
		// No non-LOD or non-LOD is completely visible.
		if(nonLOD == nil ||
		   nonLOD->GetRwObject() && nonLOD->m_alpha == 255)
			return VIS_INVISIBLE;

		// But if it is a time object, we'd rather draw the wrong
		// non-LOD than the right LOD.
		if(nonLOD->GetModelType() == MITYPE_TIME){
			ti = (CTimeModelInfo*)nonLOD;
			other = ti->GetOtherTimeModel();
			if(other != -1 && CModelInfo::GetModelInfo(other)->GetRwObject())
				return VIS_INVISIBLE;
		}
	}

	RpAtomic *a = mi->GetFirstAtomicFromDistance(dist);
	if(a){
		if(ent->m_rwObject == nil)
			ent->CreateRwObject();
		if(ent->m_rwObject == nil)
			return VIS_INVISIBLE;
		RpAtomic *rwobj = (RpAtomic*)ent->m_rwObject;

		// Make sure our atomic uses the right geometry and not
		// that of an atomic for another draw distance.
		if(RpAtomicGetGeometry(a) != RpAtomicGetGeometry(rwobj))
			RpAtomicSetGeometry(rwobj, RpAtomicGetGeometry(a), rpATOMICSAMEBOUNDINGSPHERE); // originally 5 (mistake?)
		mi->IncreaseAlpha();
#ifdef WII
		if(WiiDisableDistanceFade(mi))
			mi->m_alpha = 255;
#endif
		if(!ent->IsVisible() || !ent->GetIsOnScreenComplex() || ent->IsEntityOccluded()){
			mi->m_alpha = 255;
			return VIS_INVISIBLE;
		}

		if(mi->m_alpha != 255){
			CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist);
			ent->bDistanceFade = true;
			return VIS_INVISIBLE;
		}

		if(mi->m_drawLast){
			CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist);
			ent->bDistanceFade = false;
			return VIS_INVISIBLE;
		}
		return VIS_VISIBLE;
	}

#ifdef WII
	// Static island LODs are no longer permanently resident. Pull a missing
	// model only when it becomes visible; the streaming lists may evict it
	// again after it leaves the working set.
	if(mi->GetRwObject() == nil &&
	   CStreaming::ms_aInfoForModel[ent->GetModelIndex()].m_loadState ==
	   STREAMSTATE_NOTLOADED &&
	   dist-STREAM_DISTANCE < mi->GetLodDistance(0) && request)
		return ent->GetIsOnScreen() ? VIS_STREAMME : VIS_INVISIBLE;
#endif

	if(mi->m_noFade){
		ent->DeleteRwObject();
		return VIS_INVISIBLE;
	}


	// get faded atomic
	a = mi->GetFirstAtomicFromDistance(dist - FADE_DISTANCE);
	if(a == nil){
		if(ent->bStreamBIGBuilding && dist-STREAM_DISTANCE < mi->GetLodDistance(0) && request){
			return ent->GetIsOnScreen() ? VIS_STREAMME : VIS_INVISIBLE;
		}else{
			ent->DeleteRwObject();
			return VIS_INVISIBLE;
		}
	}

	// Fade...
	if(ent->m_rwObject == nil)
		ent->CreateRwObject();
	if(ent->m_rwObject == nil)
		return VIS_INVISIBLE;
	RpAtomic *rwobj = (RpAtomic*)ent->m_rwObject;
	if(RpAtomicGetGeometry(a) != RpAtomicGetGeometry(rwobj))
		RpAtomicSetGeometry(rwobj, RpAtomicGetGeometry(a), rpATOMICSAMEBOUNDINGSPHERE); // originally 5 (mistake?)
	mi->IncreaseAlpha();
#ifdef WII
	if(WiiDisableDistanceFade(mi))
		mi->m_alpha = 255;
#endif
	if(!ent->IsVisible() || !ent->GetIsOnScreenComplex() || ent->IsEntityOccluded()){
		mi->m_alpha = 255;
		return VIS_INVISIBLE;
	}
#ifdef WII
	if(WiiDisableDistanceFade(mi)){
		ent->bDistanceFade = false;
		if(mi->m_drawLast){
			CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist);
			return VIS_INVISIBLE;
		}
		return VIS_VISIBLE;
	}
#endif
	CVisibilityPlugins::InsertEntityIntoSortedList(ent, dist);
	ent->bDistanceFade = true;
	return VIS_INVISIBLE;
}

void
CRenderer::ConstructRenderList(void)
{
	COcclusion::ProcessBeforeRendering();
#ifdef NEW_RENDERER
	if(!gbNewRenderer)
#endif
{
	ms_nNoOfVisibleEntities = 0;
	ms_nNoOfInVisibleEntities = 0;
}
	ms_vecCameraPosition = TheCamera.GetPosition();
#ifdef WII
	gWiiLodHoleFillPairCount = 0;
#endif

	// unused
	pFullBlockedRanges = nil;
	pEmptyBlockedRanges = aBlockedRanges;
	for(int i = 0; i < 16; i++){
		aBlockedRanges[i].prev = &aBlockedRanges[i-1];
		aBlockedRanges[i].next = &aBlockedRanges[i+1];
	}
	aBlockedRanges[0].prev = nil;
	aBlockedRanges[15].next = nil;

	// unused
	TestCloseThings = 0;
	TestBigThings = 0;

	ScanWorld();
}

void
LimitFrustumVector(CVector &vec1, const CVector &vec2, float l)
{
	float f;
	f = (l - vec2.z) / (vec1.z - vec2.z);
	vec1.x = f*(vec1.x - vec2.x) + vec2.x;
	vec1.y = f*(vec1.y - vec2.y) + vec2.y;
	vec1.z = f*(vec1.z - vec2.z) + vec2.z;
}

enum Corners
{
	CORNER_CAM = 0,
	CORNER_FAR_TOPLEFT,
	CORNER_FAR_TOPRIGHT,
	CORNER_FAR_BOTRIGHT,
	CORNER_FAR_BOTLEFT,
	CORNER_LOD_LEFT,
	CORNER_LOD_RIGHT,
	CORNER_PRIO_LEFT,
	CORNER_PRIO_RIGHT,
};

void
CRenderer::ScanWorld(void)
{
	float f = RwCameraGetFarClipPlane(TheCamera.m_pRwCamera);
	RwV2d vw = *RwCameraGetViewWindow(TheCamera.m_pRwCamera);
	CVector vectors[9];
	RwMatrix *cammatrix;
	RwV2d poly[3];

	memset(vectors, 0, sizeof(vectors));
	vectors[CORNER_FAR_TOPLEFT].x = -vw.x * f;
	vectors[CORNER_FAR_TOPLEFT].y = vw.y * f;
	vectors[CORNER_FAR_TOPLEFT].z = f;
	vectors[CORNER_FAR_TOPRIGHT].x = vw.x * f;
	vectors[CORNER_FAR_TOPRIGHT].y = vw.y * f;
	vectors[CORNER_FAR_TOPRIGHT].z = f;
	vectors[CORNER_FAR_BOTRIGHT].x = vw.x * f;
	vectors[CORNER_FAR_BOTRIGHT].y = -vw.y * f;
	vectors[CORNER_FAR_BOTRIGHT].z = f;
	vectors[CORNER_FAR_BOTLEFT].x = -vw.x * f;
	vectors[CORNER_FAR_BOTLEFT].y = -vw.y * f;
	vectors[CORNER_FAR_BOTLEFT].z = f;

	cammatrix = RwFrameGetMatrix(RwCameraGetFrame(TheCamera.m_pRwCamera));

	m_pFirstPersonVehicle = nil;
	CVisibilityPlugins::InitAlphaEntityList();
	CWorld::AdvanceCurrentScanCode();

	// unused
	static CVector prevPos;
	static CVector prevFwd;
	static bool smallMovement;
	smallMovement = (TheCamera.GetPosition() - prevPos).MagnitudeSqr() < SQR(4.0f) &&
		DotProduct(TheCamera.GetForward(), prevFwd) > 0.98f;
	prevPos = TheCamera.GetPosition();
	prevFwd = TheCamera.GetForward();

	if(cammatrix->at.z > 0.0f){
		// looking up, bottom corners are further away
		vectors[CORNER_LOD_LEFT] = vectors[CORNER_FAR_BOTLEFT] * LOD_DISTANCE/f;
		vectors[CORNER_LOD_RIGHT] = vectors[CORNER_FAR_BOTRIGHT] * LOD_DISTANCE/f;
	}else{
		// looking down, top corners are further away
		vectors[CORNER_LOD_LEFT] = vectors[CORNER_FAR_TOPLEFT] * LOD_DISTANCE/f;
		vectors[CORNER_LOD_RIGHT] = vectors[CORNER_FAR_TOPRIGHT] * LOD_DISTANCE/f;
	}
	vectors[CORNER_PRIO_LEFT].x = vectors[CORNER_LOD_LEFT].x * 0.2f;
	vectors[CORNER_PRIO_LEFT].y = vectors[CORNER_LOD_LEFT].y * 0.2f;
	vectors[CORNER_PRIO_LEFT].z = vectors[CORNER_LOD_LEFT].z;
	vectors[CORNER_PRIO_RIGHT].x = vectors[CORNER_LOD_RIGHT].x * 0.2f;
	vectors[CORNER_PRIO_RIGHT].y = vectors[CORNER_LOD_RIGHT].y * 0.2f;
	vectors[CORNER_PRIO_RIGHT].z = vectors[CORNER_LOD_RIGHT].z;
	RwV3dTransformPoints(vectors, vectors, 9, cammatrix);

	m_loadingPriority = false;
	if(TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_TOPDOWN ||
#ifdef FIX_BUGS
	   TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_GTACLASSIC ||
#endif
	   TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_TOP_DOWN_PED){
		CRect rect;
		int x1, x2, y1, y2;
		LimitFrustumVector(vectors[CORNER_FAR_TOPLEFT], vectors[CORNER_CAM], -100.0f);
		rect.ContainPoint(vectors[CORNER_FAR_TOPLEFT]);
		LimitFrustumVector(vectors[CORNER_FAR_TOPRIGHT], vectors[CORNER_CAM], -100.0f);
		rect.ContainPoint(vectors[CORNER_FAR_TOPRIGHT]);
		LimitFrustumVector(vectors[CORNER_FAR_BOTRIGHT], vectors[CORNER_CAM], -100.0f);
		rect.ContainPoint(vectors[CORNER_FAR_BOTRIGHT]);
		LimitFrustumVector(vectors[CORNER_FAR_BOTLEFT], vectors[CORNER_CAM], -100.0f);
		rect.ContainPoint(vectors[CORNER_FAR_BOTLEFT]);
		x1 = CWorld::GetSectorIndexX(rect.left);
		if(x1 < 0) x1 = 0;
		x2 = CWorld::GetSectorIndexX(rect.right);
		if(x2 >= NUMSECTORS_X-1) x2 = NUMSECTORS_X-1;
		y1 = CWorld::GetSectorIndexY(rect.top);
		if(y1 < 0) y1 = 0;
		y2 = CWorld::GetSectorIndexY(rect.bottom);
		if(y2 >= NUMSECTORS_Y-1) y2 = NUMSECTORS_Y-1;
		for(; x1 <= x2; x1++)
			for(int y = y1; y <= y2; y++)
				ScanSectorList(CWorld::GetSector(x1, y)->m_lists);
	}else{
#ifdef GTA_TRAIN
		CVehicle *train = FindPlayerTrain();
		if(train && train->GetPosition().z < 0.0f){
			poly[0].x = CWorld::GetSectorX(vectors[CORNER_CAM].x);
			poly[0].y = CWorld::GetSectorY(vectors[CORNER_CAM].y);
			poly[1].x = CWorld::GetSectorX(vectors[CORNER_LOD_LEFT].x);
			poly[1].y = CWorld::GetSectorY(vectors[CORNER_LOD_LEFT].y);
			poly[2].x = CWorld::GetSectorX(vectors[CORNER_LOD_RIGHT].x);
			poly[2].y = CWorld::GetSectorY(vectors[CORNER_LOD_RIGHT].y);
			ScanSectorPoly(poly, 3, ScanSectorList_Subway);
		}else
#endif
		{
			// Big-building LODs are the geometry that closes visible holes in the
			// distant world. Admit them before ordinary sector priority requests so
			// the existing bounded priority slots serve the visible proxy first.
#ifdef NO_ISLAND_LOADING
			if (FrontEndMenuManager.m_PrefsIslandLoading == CMenuManager::ISLAND_LOADING_HIGH) {
				ScanBigBuildingList(CWorld::GetBigBuildingList(LEVEL_BEACH));
				ScanBigBuildingList(CWorld::GetBigBuildingList(LEVEL_MAINLAND));
			} else
#endif
			{
#ifdef FIX_BUGS
			if(CCollision::ms_collisionInMemory != LEVEL_GENERIC)
#endif
				ScanBigBuildingList(CWorld::GetBigBuildingList(CGame::currLevel));
			}
			ScanBigBuildingList(CWorld::GetBigBuildingList(LEVEL_GENERIC));

			if(f > LOD_DISTANCE){
				// priority
				poly[0].x = CWorld::GetSectorX(vectors[CORNER_CAM].x);
				poly[0].y = CWorld::GetSectorY(vectors[CORNER_CAM].y);
				poly[1].x = CWorld::GetSectorX(vectors[CORNER_PRIO_LEFT].x);
				poly[1].y = CWorld::GetSectorY(vectors[CORNER_PRIO_LEFT].y);
				poly[2].x = CWorld::GetSectorX(vectors[CORNER_PRIO_RIGHT].x);
				poly[2].y = CWorld::GetSectorY(vectors[CORNER_PRIO_RIGHT].y);
				ScanSectorPoly(poly, 3, ScanSectorList_Priority);

				// below LOD
				poly[0].x = CWorld::GetSectorX(vectors[CORNER_CAM].x);
				poly[0].y = CWorld::GetSectorY(vectors[CORNER_CAM].y);
				poly[1].x = CWorld::GetSectorX(vectors[CORNER_LOD_LEFT].x);
				poly[1].y = CWorld::GetSectorY(vectors[CORNER_LOD_LEFT].y);
				poly[2].x = CWorld::GetSectorX(vectors[CORNER_LOD_RIGHT].x);
				poly[2].y = CWorld::GetSectorY(vectors[CORNER_LOD_RIGHT].y);
				ScanSectorPoly(poly, 3, ScanSectorList);
			}else{
				poly[0].x = CWorld::GetSectorX(vectors[CORNER_CAM].x);
				poly[0].y = CWorld::GetSectorY(vectors[CORNER_CAM].y);
				poly[1].x = CWorld::GetSectorX(vectors[CORNER_FAR_TOPLEFT].x);
				poly[1].y = CWorld::GetSectorY(vectors[CORNER_FAR_TOPLEFT].y);
				poly[2].x = CWorld::GetSectorX(vectors[CORNER_FAR_TOPRIGHT].x);
				poly[2].y = CWorld::GetSectorY(vectors[CORNER_FAR_TOPRIGHT].y);
				ScanSectorPoly(poly, 3, ScanSectorList);
			}
		}
	}
}

void
CRenderer::RequestObjectsInFrustum(void)
{
	float f = RwCameraGetFarClipPlane(TheCamera.m_pRwCamera);
	RwV2d vw = *RwCameraGetViewWindow(TheCamera.m_pRwCamera);
	CVector vectors[9];
	RwMatrix *cammatrix;
	RwV2d poly[3];

	memset(vectors, 0, sizeof(vectors));
	vectors[CORNER_FAR_TOPLEFT].x = -vw.x * f;
	vectors[CORNER_FAR_TOPLEFT].y = vw.y * f;
	vectors[CORNER_FAR_TOPLEFT].z = f;
	vectors[CORNER_FAR_TOPRIGHT].x = vw.x * f;
	vectors[CORNER_FAR_TOPRIGHT].y = vw.y * f;
	vectors[CORNER_FAR_TOPRIGHT].z = f;
	vectors[CORNER_FAR_BOTRIGHT].x = vw.x * f;
	vectors[CORNER_FAR_BOTRIGHT].y = -vw.y * f;
	vectors[CORNER_FAR_BOTRIGHT].z = f;
	vectors[CORNER_FAR_BOTLEFT].x = -vw.x * f;
	vectors[CORNER_FAR_BOTLEFT].y = -vw.y * f;
	vectors[CORNER_FAR_BOTLEFT].z = f;

	cammatrix = RwFrameGetMatrix(RwCameraGetFrame(TheCamera.m_pRwCamera));

	CWorld::AdvanceCurrentScanCode();
	ms_vecCameraPosition = TheCamera.GetPosition();

	if(cammatrix->at.z > 0.0f){
		// looking up, bottom corners are further away
		vectors[CORNER_LOD_LEFT] = vectors[CORNER_FAR_BOTLEFT] * LOD_DISTANCE/f;
		vectors[CORNER_LOD_RIGHT] = vectors[CORNER_FAR_BOTRIGHT] * LOD_DISTANCE/f;
	}else{
		// looking down, top corners are further away
		vectors[CORNER_LOD_LEFT] = vectors[CORNER_FAR_TOPLEFT] * LOD_DISTANCE/f;
		vectors[CORNER_LOD_RIGHT] = vectors[CORNER_FAR_TOPRIGHT] * LOD_DISTANCE/f;
	}
	vectors[CORNER_PRIO_LEFT].x = vectors[CORNER_LOD_LEFT].x * 0.2f;
	vectors[CORNER_PRIO_LEFT].y = vectors[CORNER_LOD_LEFT].y * 0.2f;
	vectors[CORNER_PRIO_LEFT].z = vectors[CORNER_LOD_LEFT].z;
	vectors[CORNER_PRIO_RIGHT].x = vectors[CORNER_LOD_RIGHT].x * 0.2f;
	vectors[CORNER_PRIO_RIGHT].y = vectors[CORNER_LOD_RIGHT].y * 0.2f;
	vectors[CORNER_PRIO_RIGHT].z = vectors[CORNER_LOD_RIGHT].z;
	RwV3dTransformPoints(vectors, vectors, 9, cammatrix);

	if(TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_TOPDOWN ||
#ifdef FIX_BUGS
	   TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_GTACLASSIC ||
#endif
	   TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_TOP_DOWN_PED){
		CRect rect;
		int x1, x2, y1, y2;
		LimitFrustumVector(vectors[CORNER_FAR_TOPLEFT], vectors[CORNER_CAM], -100.0f);
		rect.ContainPoint(vectors[CORNER_FAR_TOPLEFT]);
		LimitFrustumVector(vectors[CORNER_FAR_TOPRIGHT], vectors[CORNER_CAM], -100.0f);
		rect.ContainPoint(vectors[CORNER_FAR_TOPRIGHT]);
		LimitFrustumVector(vectors[CORNER_FAR_BOTRIGHT], vectors[CORNER_CAM], -100.0f);
		rect.ContainPoint(vectors[CORNER_FAR_BOTRIGHT]);
		LimitFrustumVector(vectors[CORNER_FAR_BOTLEFT], vectors[CORNER_CAM], -100.0f);
		rect.ContainPoint(vectors[CORNER_FAR_BOTLEFT]);
		x1 = CWorld::GetSectorIndexX(rect.left);
		if(x1 < 0) x1 = 0;
		x2 = CWorld::GetSectorIndexX(rect.right);
		if(x2 >= NUMSECTORS_X-1) x2 = NUMSECTORS_X-1;
		y1 = CWorld::GetSectorIndexY(rect.top);
		if(y1 < 0) y1 = 0;
		y2 = CWorld::GetSectorIndexY(rect.bottom);
		if(y2 >= NUMSECTORS_Y-1) y2 = NUMSECTORS_Y-1;
		for(; x1 <= x2; x1++)
			for(int y = y1; y <= y2; y++)
				ScanSectorList_RequestModels(CWorld::GetSector(x1, y)->m_lists);
	}else{
		poly[0].x = CWorld::GetSectorX(vectors[CORNER_CAM].x);
		poly[0].y = CWorld::GetSectorY(vectors[CORNER_CAM].y);
		poly[1].x = CWorld::GetSectorX(vectors[CORNER_LOD_LEFT].x);
		poly[1].y = CWorld::GetSectorY(vectors[CORNER_LOD_LEFT].y);
		poly[2].x = CWorld::GetSectorX(vectors[CORNER_LOD_RIGHT].x);
		poly[2].y = CWorld::GetSectorY(vectors[CORNER_LOD_RIGHT].y);
		ScanSectorPoly(poly, 3, ScanSectorList_RequestModels);
	}
}

bool
CEntity::SetupLighting(void)
{
	return false;
}

void
CEntity::RemoveLighting(bool)
{
}

bool
CPed::SetupLighting(void)
{
	ActivateDirectional();
	SetAmbientColoursForPedsCarsAndObjects();

#ifndef MASTER
	// Originally this was being called through iteration of Sectors, but putting it here is better.
	if (GetDebugDisplay() != 0 && !IsPlayer())
		DebugRenderOnePedText();
#endif

	if (bRenderScorched) {
		WorldReplaceNormalLightsWithScorched(Scene.world, 0.1f);
	} else {
		// Note that this lightMult is only affected by LIGHT_DARKEN. If there's no LIGHT_DARKEN, it will be 1.0.
		float lightMult = CPointLights::GenerateLightsAffectingObject(&GetPosition());
		if (lightMult != 1.0f) {
			SetAmbientAndDirectionalColours(lightMult);
			return true;
		}
	}
	return false;
}

void
CPed::RemoveLighting(bool reset)
{
	if (!bRenderScorched) {
		CRenderer::RemoveVehiclePedLights(this, reset);
		if (reset)
			ReSetAmbientAndDirectionalColours();
	}
	SetAmbientColours();
	DeActivateDirectional();
}

float
CalcNewDelta(RwV2d *a, RwV2d *b)
{
	return (b->x - a->x) / (b->y - a->y);
}

#ifdef FIX_BUGS
#define TOINT(x) ((int)Floor(x))
#else
#define TOINT(x) ((int)(x))
#endif

void
CRenderer::ScanSectorPoly(RwV2d *poly, int32 numVertices, void (*scanfunc)(CPtrList *))
{
	float miny, maxy;
	int y, yend;
	int x, xstart, xend;
	int i;
	int a1, a2, b1, b2;
	float deltaA, deltaB;
	float xA, xB;

	miny = poly[0].y;
	maxy = poly[0].y;
	a2 = 0;
	xstart = 9999;
	xend = -9999;

	for(i = 1; i < numVertices; i++){
		if(poly[i].y > maxy)
			maxy = poly[i].y;
		if(poly[i].y < miny){
			miny = poly[i].y;
			a2 = i;
		}
	}
	y = TOINT(miny);
	yend = TOINT(maxy);

	// Go left in poly to find first edge b
	b2 = a2;
	for(i = 0; i < numVertices; i++){
		b1 = b2--;
		if(b2 < 0) b2 = numVertices-1;
		if(poly[b1].x < xstart)
			xstart = TOINT(poly[b1].x);
		if(TOINT(poly[b1].y) != TOINT(poly[b2].y))
			break;
	}
	// Go right to find first edge a
	for(i = 0; i < numVertices; i++){
		a1 = a2++;
		if(a2 == numVertices) a2 = 0;
		if(poly[a1].x > xend)
			xend = TOINT(poly[a1].x);
		if(TOINT(poly[a1].y) != TOINT(poly[a2].y))
			break;
	}

	// prestep x1 and x2 to next integer y
	deltaA = CalcNewDelta(&poly[a1], &poly[a2]);
	xA = deltaA * (Ceil(poly[a1].y) - poly[a1].y) + poly[a1].x;
	deltaB = CalcNewDelta(&poly[b1], &poly[b2]);
	xB = deltaB * (Ceil(poly[b1].y) - poly[b1].y) + poly[b1].x;

	if(y != yend){
		if(deltaB < 0.0f && TOINT(xB) < xstart)
			xstart = TOINT(xB);
		if(deltaA >= 0.0f && TOINT(xA) > xend)
			xend = TOINT(xA);
	}

	while(y <= yend && y < NUMSECTORS_Y){
		// scan one x-line
		if(y >= 0 && xstart < NUMSECTORS_X)
			for(x = xstart; x <= xend && x != NUMSECTORS_X; x++)
				if(x >= 0)
					scanfunc(CWorld::GetSector(x, y)->m_lists);

		// advance one scan line
		y++;
		xA += deltaA;
		xB += deltaB;

		// update left side
		if(y == TOINT(poly[b2].y)){
			// reached end of edge
			if(y == yend){
				if(deltaB < 0.0f){
					do{
						xstart = TOINT(poly[b2--].x);
						if(b2 < 0) b2 = numVertices-1;
					}while(xstart > TOINT(poly[b2].x));
				}else
					xstart = TOINT(xB - deltaB);
			}else{
				// switch edges
				if(deltaB < 0.0f)
					xstart = TOINT(poly[b2].x);
				else
					xstart = TOINT(xB - deltaB);
				do{
					b1 = b2--;
					if(b2 < 0) b2 = numVertices-1;
					if(TOINT(poly[b1].x) < xstart)
						xstart = TOINT(poly[b1].x);
				}while(y == TOINT(poly[b2].y));
				deltaB = CalcNewDelta(&poly[b1], &poly[b2]);
				xB = deltaB * (Ceil(poly[b1].y) - poly[b1].y) + poly[b1].x;
				if(deltaB < 0.0f && TOINT(xB) < xstart)
					xstart = TOINT(xB);
			}
		}else{
			if(deltaB < 0.0f)
				xstart = TOINT(xB);
			else
				xstart = TOINT(xB - deltaB);
		}

		// update right side
		if(y == TOINT(poly[a2].y)){
			// reached end of edge
			if(y == yend){
				if(deltaA < 0.0f)
					xend = TOINT(xA - deltaA);
				else{
					do{
						xend = TOINT(poly[a2++].x);
						if(a2 == numVertices) a2 = 0;
					}while(xend < TOINT(poly[a2].x));
				}
			}else{
				// switch edges
				if(deltaA < 0.0f)
					xend = TOINT(xA - deltaA);
				else
					xend = TOINT(poly[a2].x);
				do{
					a1 = a2++;
					if(a2 == numVertices) a2 = 0;
					if(TOINT(poly[a1].x) > xend)
						xend = TOINT(poly[a1].x);
				}while(y == TOINT(poly[a2].y));
				deltaA = CalcNewDelta(&poly[a1], &poly[a2]);
				xA = deltaA * (Ceil(poly[a1].y) - poly[a1].y) + poly[a1].x;
				if(deltaA >= 0.0f && TOINT(xA) > xend)
					xend = TOINT(xA);
			}
		}else{
			if(deltaA < 0.0f)
				xend = TOINT(xA - deltaA);
			else
				xend = TOINT(xA);
		}
	}
}

void
CRenderer::InsertEntityIntoList(CEntity *ent)
{
#ifdef FIX_BUGS
	if (!ent->m_rwObject) return;
#endif

#ifdef NEW_RENDERER
	// TODO: there are more flags being checked here
	if(gbNewRenderer && (ent->IsVehicle() || ent->IsPed()))
		ms_aVisibleVehiclePtrs[ms_nNoOfVisibleVehicles++] = ent;
	else if(gbNewRenderer && ent->IsBuilding())
		ms_aVisibleBuildingPtrs[ms_nNoOfVisibleBuildings++] = ent;
	else
#endif
		ms_aVisibleEntityPtrs[ms_nNoOfVisibleEntities++] = ent;
}

void
CRenderer::ScanBigBuildingList(CPtrList &list)
{
	CPtrNode *node;
	CEntity *ent;
	int vis;
#ifdef WII
	uint32 frame = CTimer::GetFrameCounter();
	if(gWiiBigBuildingRequestFrame != frame){
		gWiiBigBuildingRequestFrame = frame;
		gWiiBigBuildingRequestsThisFrame = 0;
	}
#endif

	int f = CTimer::GetFrameCounter() & 3;
	for(node = list.first; node; node = node->next){
		ent = (CEntity*)node->item;
		bool needsVisibilitySetup = ent->bOffscreen || (ent->m_randomSeed&3) != f;
#ifdef WII
		// A loaded model resource is not enough for the entity fast path: the
		// per-entity RW instance must also be bound before it can be visible.
		if(ent->m_rwObject == nil ||
		   CStreaming::ms_aInfoForModel[ent->GetModelIndex()].m_loadState !=
		   STREAMSTATE_LOADED)
			needsVisibilitySetup = true;
#endif
		if(needsVisibilitySetup){
			ent->bOffscreen = true;
			vis = SetupBigBuildingVisibility(ent);
		#if defined(WII) && WII_STREAM_BIG_BUILDING_PROBE
			WiiProbeBigBuildingInvisible(ent, vis);
			WiiProbeCamJonesScan(ent, vis, "big");
		#endif
		}else{
			vis = VIS_VISIBLE;
		#if defined(WII) && WII_STREAM_BIG_BUILDING_PROBE
			WiiProbeCamJonesScan(ent, vis, "big_fast");
		#endif
		}
		switch(vis){
		case VIS_VISIBLE:
#if defined(WII) && WII_STREAM_BIG_BUILDING_PROBE
			WiiProbeLodCompanions(ent);
			if(ent->m_rwObject == nil ||
			   CStreaming::ms_aInfoForModel[ent->GetModelIndex()].m_loadState !=
			   STREAMSTATE_LOADED)
				CStreaming::ProbeBigBuilding("visible_missing", ent->GetModelIndex(),
				                             0, "visible_without_rw");
#endif
		#ifdef WII
			WiiQueueLodHoleFill(ent);
		#endif
			InsertEntityIntoList(ent);
			ent->bOffscreen = false;
			break;
		case VIS_STREAMME:
			if(!CStreaming::ms_disableStreaming){
#ifdef WII
				uint8 streamState =
					CStreaming::ms_aInfoForModel[ent->GetModelIndex()].m_loadState;
				// A loaded or actively reading model cannot be made more ready by
				// another request. Keep the admission path for queued work only.
				if(streamState == STREAMSTATE_LOADED ||
				   streamState == STREAMSTATE_READING ||
				   streamState == STREAMSTATE_STARTED)
					break;
				// Once a visible big building is already queued at priority, do
				// not consume another per-frame admission slot retrying it.
				// Non-priority queued work still falls through so it can promote.
				if(streamState == STREAMSTATE_INQUEUE &&
				   CStreaming::ms_aInfoForModel[ent->GetModelIndex()].IsPriority())
					break;

				// A visible big building must use the existing priority queue. A
				// non-priority request would be discarded by CStreaming::Update's
				// end-of-frame request cleanup before it can become visible.
				bool canPromote = ent->m_rwObject == nil &&
				                  !m_loadingPriority;
				bool frameCap = gWiiBigBuildingRequestsThisFrame >= 2;
				bool backlogCap = CStreaming::ms_numModelsRequested >= 24;
				if(!canPromote || frameCap || backlogCap){
				#if WII_STREAM_BIG_BUILDING_PROBE
					CStreaming::ProbeBigBuilding("scan_skip", ent->GetModelIndex(), 0,
					                             !canPromote ? "priority_cap" :
					                             frameCap && backlogCap ?
					                             "frame_cap+backlog_cap" :
					                             frameCap ? "frame_cap" : "backlog_cap");
				#endif
					break;
				}
#endif
				int32 flags = 0;
#ifdef WII
				if(canPromote)
					flags = STREAMFLAGS_PRIORITY;
#else
				if(!m_loadingPriority &&
				   CStreaming::ms_numModelsRequested < 10 &&
				   CStreaming::ms_numPriorityRequests < 4)
					flags = STREAMFLAGS_PRIORITY;
#endif
#if defined(WII) && WII_STREAM_BIG_BUILDING_PROBE
				CStreaming::ProbeBigBuilding("scan_request", ent->GetModelIndex(),
				                             flags, "visible_streamme");
#endif
				CStreaming::RequestModel(ent->GetModelIndex(), flags);
#ifdef WII
				gWiiBigBuildingRequestsThisFrame++;
#endif
			}
			break;
		}
	}
}

void
CRenderer::ScanSectorList(CPtrList *lists)
{
	CPtrNode *node;
	CPtrList *list;
	CEntity *ent;
	int i;
	float dx, dy;

	for(i = 0; i < NUMSECTORENTITYLISTS; i++){
		list = &lists[i];
		for(node = list->first; node; node = node->next){
			ent = (CEntity*)node->item;
			if(ent->m_scanCode == CWorld::GetCurrentScanCode())
				continue;	// already seen
			ent->m_scanCode = CWorld::GetCurrentScanCode();
			ent->bOffscreen = false;

			int32 visibility = SetupEntityVisibility(ent);
#if defined(WII) && WII_STREAM_BIG_BUILDING_PROBE
			WiiProbeNearLodEntityVisibility(ent, visibility, "normal");
			WiiProbeCamJonesScan(ent, visibility, "normal");
#endif
			switch(visibility){
			case VIS_VISIBLE:
				InsertEntityIntoList(ent);
				break;
			case VIS_INVISIBLE:
				if(!IsGlass(ent->GetModelIndex()))
					break;
				// fall through
			case VIS_OFFSCREEN:
				ent->bOffscreen = true;
				dx = ms_vecCameraPosition.x - ent->GetPosition().x;
				dy = ms_vecCameraPosition.y - ent->GetPosition().y;
				if(dx > -30.0f && dx < 30.0f &&
				   dy > -30.0f && dy < 30.0f &&
				   ms_nNoOfInVisibleEntities < NUMINVISIBLEENTITIES - 1)
					ms_aInVisibleEntityPtrs[ms_nNoOfInVisibleEntities++] = ent;
				break;
			case VIS_STREAMME:
				if(!CStreaming::ms_disableStreaming)
					if(!m_loadingPriority || ent->GetIsOnScreen())
					CStreaming::RequestModel(ent->GetModelIndex(), 0);
				break;
			}
		}
	}
}

void
CRenderer::ScanSectorList_Priority(CPtrList *lists)
{
	CPtrNode *node;
	CPtrList *list;
	CEntity *ent;
	int i;
	float dx, dy;

	for(i = 0; i < NUMSECTORENTITYLISTS; i++){
		list = &lists[i];
		for(node = list->first; node; node = node->next){
			ent = (CEntity*)node->item;
			if(ent->m_scanCode == CWorld::GetCurrentScanCode())
				continue;	// already seen
			ent->m_scanCode = CWorld::GetCurrentScanCode();
			ent->bOffscreen = false;

			int32 visibility = SetupEntityVisibility(ent);
#if defined(WII) && WII_STREAM_BIG_BUILDING_PROBE
			WiiProbeNearLodEntityVisibility(ent, visibility, "priority");
			WiiProbeCamJonesScan(ent, visibility, "priority");
#endif
			switch(visibility){
			case VIS_VISIBLE:
				InsertEntityIntoList(ent);
				break;
			case VIS_INVISIBLE:
				if(!IsGlass(ent->GetModelIndex()))
					break;
				// fall through
			case VIS_OFFSCREEN:
				ent->bOffscreen = true;
				dx = ms_vecCameraPosition.x - ent->GetPosition().x;
				dy = ms_vecCameraPosition.y - ent->GetPosition().y;
				if(dx > -30.0f && dx < 30.0f &&
				   dy > -30.0f && dy < 30.0f &&
				   ms_nNoOfInVisibleEntities < NUMINVISIBLEENTITIES - 1)
					ms_aInVisibleEntityPtrs[ms_nNoOfInVisibleEntities++] = ent;
				break;
			case VIS_STREAMME:
				if(!CStreaming::ms_disableStreaming){
					CStreaming::RequestModel(ent->GetModelIndex(), STREAMFLAGS_PRIORITY);
					if(CStreaming::ms_aInfoForModel[ent->GetModelIndex()].m_loadState != STREAMSTATE_LOADED)
						m_loadingPriority = true;
				}
				break;
			}
		}
	}
}

#ifdef GTA_TRAIN
void
CRenderer::ScanSectorList_Subway(CPtrList *lists)
{
	CPtrNode *node;
	CPtrList *list;
	CEntity *ent;
	int i;
	float dx, dy;

	for(i = 0; i < NUMSECTORENTITYLISTS; i++){
		list = &lists[i];
		for(node = list->first; node; node = node->next){
			ent = (CEntity*)node->item;
			if(ent->m_scanCode == CWorld::GetCurrentScanCode())
				continue;	// already seen
			ent->m_scanCode = CWorld::GetCurrentScanCode();
			ent->bOffscreen = false;
			switch(SetupEntityVisibility(ent)){
			case VIS_VISIBLE:
				InsertEntityIntoList(ent);
				break;
			case VIS_OFFSCREEN:
				ent->bOffscreen = true;
				dx = ms_vecCameraPosition.x - ent->GetPosition().x;
				dy = ms_vecCameraPosition.y - ent->GetPosition().y;
				if(dx > -30.0f && dx < 30.0f &&
				   dy > -30.0f && dy < 30.0f &&
				   ms_nNoOfInVisibleEntities < NUMINVISIBLEENTITIES - 1)
					ms_aInVisibleEntityPtrs[ms_nNoOfInVisibleEntities++] = ent;
				break;
			}
		}
	}
}
#endif

void
CRenderer::ScanSectorList_RequestModels(CPtrList *lists)
{
	CPtrNode *node;
	CPtrList *list;
	CEntity *ent;
	int i;

	for(i = 0; i < NUMSECTORENTITYLISTS; i++){
		list = &lists[i];
		for(node = list->first; node; node = node->next){
			ent = (CEntity*)node->item;
			if(ent->m_scanCode == CWorld::GetCurrentScanCode())
				continue;	// already seen
			ent->m_scanCode = CWorld::GetCurrentScanCode();
			if(ShouldModelBeStreamed(ent, ms_vecCameraPosition))
				CStreaming::RequestModel(ent->GetModelIndex(), 0);
		}
	}
}

// Put big buildings in front
// This seems pointless because the sector lists shouldn't have big buildings in the first place
void
CRenderer::SortBIGBuildings(void)
{
	int x, y;
	for(y = 0; y < NUMSECTORS_Y; y++)
		for(x = 0; x < NUMSECTORS_X; x++){
			SortBIGBuildingsForSectorList(&CWorld::GetSector(x, y)->m_lists[ENTITYLIST_BUILDINGS]);
			SortBIGBuildingsForSectorList(&CWorld::GetSector(x, y)->m_lists[ENTITYLIST_BUILDINGS_OVERLAP]);
		}
}

void
CRenderer::SortBIGBuildingsForSectorList(CPtrList *list)
{
	CPtrNode *node;
	CEntity *ent;

	for(node = list->first; node; node = node->next){
		ent = (CEntity*)node->item;
		if(ent->bIsBIGBuilding){
			list->RemoveNode(node);
			list->InsertNode(node);
		}
	}
}

bool
CRenderer::ShouldModelBeStreamed(CEntity *ent, const CVector &campos)
{
	if(!IsAreaVisible(ent->m_area))
		return false;
	CTimeModelInfo *mi = (CTimeModelInfo *)CModelInfo::GetModelInfo(ent->GetModelIndex());
	if(mi->GetModelType() == MITYPE_TIME)
		if(!CClock::GetIsTimeInRange(mi->GetTimeOn(), mi->GetTimeOff()))
			return false;
	float dist = (ent->GetPosition() - campos).Magnitude();
	if(mi->m_noFade)
		return dist - STREAM_DISTANCE < mi->GetLargestLodDistance();
	else
		return dist - FADE_DISTANCE - STREAM_DISTANCE < mi->GetLargestLodDistance();
}

void
CRenderer::RemoveVehiclePedLights(CEntity *ent, bool reset)
{
	if(!ent->bRenderScorched){
		CPointLights::RemoveLightsAffectingObject();
		if(reset)
			ReSetAmbientAndDirectionalColours();
	}
	SetAmbientColours();
	DeActivateDirectional();
}
