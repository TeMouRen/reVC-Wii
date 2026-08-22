#include "common.h"

#include "RwHelper.h"
#include "templates.h"
#include "main.h"
#include "Entity.h"
#include "ModelInfo.h"
#include "Lights.h"
#include "RwHelper.h"
#include "Renderer.h"
#include "Camera.h"
#include "VisibilityPlugins.h"
#include "World.h"
#include "Vehicle.h"
#include "custompipes.h"
#include "MemoryHeap.h"

CLinkList<CVisibilityPlugins::AlphaObjectInfo> CVisibilityPlugins::m_alphaList;
CLinkList<CVisibilityPlugins::AlphaObjectInfo> CVisibilityPlugins::m_alphaBoatAtomicList;
CLinkList<CVisibilityPlugins::AlphaObjectInfo> CVisibilityPlugins::m_alphaEntityList;
CLinkList<CVisibilityPlugins::AlphaObjectInfo> CVisibilityPlugins::m_alphaUnderwaterEntityList;
#ifdef NEW_RENDERER
CLinkList<CVisibilityPlugins::AlphaObjectInfo> CVisibilityPlugins::m_alphaBuildingList;
#endif

#ifdef WII
static CVisibilityPlugins::WiiAlphaStats gWiiAlphaStats;

static inline bool
WiiUseAggressiveAlphaCull(void)
{
	if(TheCamera.Cams[TheCamera.ActiveCam].Mode != CCam::MODE_1STPERSON)
		return false;
	return FindPlayerVehicle() != nil;
}
#endif

int32 CVisibilityPlugins::ms_atomicPluginOffset = -1;
int32 CVisibilityPlugins::ms_framePluginOffset = -1;
int32 CVisibilityPlugins::ms_clumpPluginOffset = -1;

RwCamera *CVisibilityPlugins::ms_pCamera;
RwV3d *CVisibilityPlugins::ms_pCameraPosn;
float CVisibilityPlugins::ms_cullCompsDist;
float CVisibilityPlugins::ms_vehicleLod0Dist;
float CVisibilityPlugins::ms_vehicleLod1Dist;
float CVisibilityPlugins::ms_vehicleFadeDist;
float CVisibilityPlugins::ms_bigVehicleLod0Dist;
float CVisibilityPlugins::ms_bigVehicleLod1Dist;
float CVisibilityPlugins::ms_pedLod1Dist;
float CVisibilityPlugins::ms_pedFadeDist;

#ifdef WII
#if 0 // [GX-DARKMESH] geometry scan disabled; enable only for targeted diagnosis.
static bool
WiiMarkDarkMeshGeometryInspected(RpGeometry *geometry)
{
    static RpGeometry *inspected[4096];
    static uint32 numInspected;

    for(uint32 i = 0; i < numInspected; i++)
        if(inspected[i] == geometry)
            return false;
    if(numInspected < ARRAY_SIZE(inspected))
        inspected[numInspected++] = geometry;
    return true;
}

static void
WiiLogDarkPrelitMeshes(RpAtomic *atomic, CSimpleModelInfo *modelInfo)
{
    if(atomic == nil || modelInfo == nil)
        return;

    rw::Geometry *geometry = atomic->geometry;
    if(geometry == nil || geometry->colors == nil ||
       geometry->meshHeader == nil ||
       (geometry->flags & rw::Geometry::PRELIT) == 0 ||
       !WiiMarkDarkMeshGeometryInspected(geometry))
        return;

    rw::Mesh *meshes = geometry->meshHeader->getMeshes();
    for(uint32 meshIndex = 0;
        meshIndex < geometry->meshHeader->numMeshes;
        meshIndex++){
        rw::Mesh *mesh = &meshes[meshIndex];
        if(mesh->indices == nil || mesh->numIndices == 0)
            continue;

        uint32 valid = 0;
        uint32 dark = 0;
        uint32 nearDark = 0;
        uint32 redSum = 0;
        uint32 greenSum = 0;
        uint32 blueSum = 0;
        uint8 redMin = 255, greenMin = 255, blueMin = 255;
        uint8 redMax = 0, greenMax = 0, blueMax = 0;

        for(uint32 i = 0; i < mesh->numIndices; i++){
            uint16 vertexIndex = mesh->indices[i];
            if(vertexIndex >= geometry->numVertices)
                continue;

            const rw::RGBA &color = geometry->colors[vertexIndex];
            redSum += color.red;
            greenSum += color.green;
            blueSum += color.blue;
            if(color.red < redMin) redMin = color.red;
            if(color.green < greenMin) greenMin = color.green;
            if(color.blue < blueMin) blueMin = color.blue;
            if(color.red > redMax) redMax = color.red;
            if(color.green > greenMax) greenMax = color.green;
            if(color.blue > blueMax) blueMax = color.blue;
            if(color.red <= 8 && color.green <= 8 && color.blue <= 8)
                dark++;
            if(color.red <= 24 && color.green <= 24 && color.blue <= 24)
                nearDark++;
            valid++;
        }

        if(valid == 0)
            continue;

        uint32 redAvg = redSum / valid;
        uint32 greenAvg = greenSum / valid;
        uint32 blueAvg = blueSum / valid;
        bool mostlyNearDark = nearDark * 4 >= valid;
        bool lowAverage = redAvg + greenAvg + blueAvg <= 72;
        if(!mostlyNearDark && !lowAverage)
            continue;

        rw::Material *material = mesh->material;
        const char *textureName = material && material->texture ?
                                  material->texture->name : "none";
        const float ambientRed = pAmbient ? pAmbient->color.red : -1.0f;
        const float ambientGreen = pAmbient ? pAmbient->color.green : -1.0f;
        const float ambientBlue = pAmbient ? pAmbient->color.blue : -1.0f;
        printf("[GX-DARKMESH] model=%s geo=%p flags=0x%X mesh=%u tex=%s "
               "idx=%u valid=%u dark=%u near=%u avg=%u,%u,%u "
               "min=%u,%u,%u max=%u,%u,%u "
               "mat=%u,%u,%u,%u surfA=%.3f surfD=%.3f "
               "ambient=%.3f,%.3f,%.3f\n",
               modelInfo->GetModelName(),
               (void*)geometry,
               (unsigned)geometry->flags,
               (unsigned)meshIndex,
               textureName ? textureName : "none",
               (unsigned)mesh->numIndices,
               (unsigned)valid,
               (unsigned)dark,
               (unsigned)nearDark,
               (unsigned)redAvg,
               (unsigned)greenAvg,
               (unsigned)blueAvg,
               (unsigned)redMin,
               (unsigned)greenMin,
               (unsigned)blueMin,
               (unsigned)redMax,
               (unsigned)greenMax,
               (unsigned)blueMax,
               material ? (unsigned)material->color.red : 255u,
               material ? (unsigned)material->color.green : 255u,
               material ? (unsigned)material->color.blue : 255u,
               material ? (unsigned)material->color.alpha : 255u,
               material ? (double)material->surfaceProps.ambient : 1.0,
               material ? (double)material->surfaceProps.diffuse : 1.0,
               (double)ambientRed,
               (double)ambientGreen,
               (double)ambientBlue);
    }
}

static RpAtomic*
WiiAtomicDefaultRenderCallBack(RpAtomic *atomic)
{
    CSimpleModelInfo *modelInfo = nil;
    if(atomic != nil && atomic->clump == nil)
        modelInfo = CVisibilityPlugins::GetAtomicModelInfo(atomic);

    WiiLogDarkPrelitMeshes(atomic, modelInfo);
    return AtomicDefaultRenderCallBack(atomic);
}

#define RENDERCALLBACK WiiAtomicDefaultRenderCallBack
#endif

#define RENDERCALLBACK AtomicDefaultRenderCallBack
#else
#define RENDERCALLBACK AtomicDefaultRenderCallBack
#endif

#ifdef WII
static bool
WiiShouldCullAlphaEntity(CEntity *e, float dist)
{
	if(e == nil)
		return false;
	if(e->IsVehicle() || e->IsPed() || e->bUnderwater)
		return false;
	if(e->IsBuilding() && e->bIsBIGBuilding)
		return false;

	CVehicle *playerVeh = FindPlayerVehicle();
	if(playerVeh == nil)
		return false;

	const float speed2D = playerVeh->m_vecMoveSpeed.Magnitude2D();
	const bool aggressive = WiiUseAggressiveAlphaCull();
	if(!aggressive && speed2D < 0.045f)
		return false;

	float preserveDist = (aggressive ? 58.0f : 82.0f) * TheCamera.LODDistMultiplier;
	if(speed2D > 0.09f)
		preserveDist -= 12.0f * TheCamera.LODDistMultiplier;
	if(speed2D > 0.14f)
		preserveDist -= (aggressive ? 8.0f : 12.0f) * TheCamera.LODDistMultiplier;
	if(aggressive && e->bDrawLast)
		preserveDist -= 12.0f * TheCamera.LODDistMultiplier;
	if(preserveDist < 24.0f)
		preserveDist = 24.0f;

	return dist > preserveDist;
}
#endif

void
CVisibilityPlugins::Initialise(void)
{
	m_alphaList.Init(NUMALPHALIST);
	m_alphaList.head.item.sort = 0.0f;
	m_alphaList.tail.item.sort = 100000000.0f;

	m_alphaBoatAtomicList.Init(NUMBOATALPHALIST);
	m_alphaBoatAtomicList.head.item.sort = 0.0f;
	m_alphaBoatAtomicList.tail.item.sort = 100000000.0f;

#ifdef ASPECT_RATIO_SCALE
	// default 150 is not enough for bigger FOVs
	m_alphaEntityList.Init(NUMALPHAENTITYLIST * 3);
#else
	m_alphaEntityList.Init(NUMALPHAENTITYLIST);
#endif // ASPECT_RATIO_SCALE
	m_alphaEntityList.head.item.sort = 0.0f;
	m_alphaEntityList.tail.item.sort = 100000000.0f;

	m_alphaUnderwaterEntityList.Init(NUMALPHAUNTERWATERENTITYLIST);
	m_alphaUnderwaterEntityList.head.item.sort = 0.0f;
	m_alphaUnderwaterEntityList.tail.item.sort = 100000000.0f;

#ifdef NEW_RENDERER
	m_alphaBuildingList.Init(NUMALPHAENTITYLIST);
	m_alphaBuildingList.head.item.sort = 0.0f;
	m_alphaBuildingList.tail.item.sort = 100000000.0f;
#endif
}

void
CVisibilityPlugins::Shutdown(void)
{
	m_alphaList.Shutdown();
	m_alphaBoatAtomicList.Shutdown();
	m_alphaEntityList.Shutdown();
	m_alphaUnderwaterEntityList.Shutdown();
#ifdef NEW_RENDERER
	m_alphaBuildingList.Shutdown();
#endif
}

#ifdef WII
void
CVisibilityPlugins::ResetWiiAlphaStats(void)
{
	memset(&gWiiAlphaStats, 0, sizeof(gWiiAlphaStats));
}

void
CVisibilityPlugins::GetWiiAlphaStats(WiiAlphaStats &stats)
{
	stats = gWiiAlphaStats;
}
#endif

void
CVisibilityPlugins::InitAlphaEntityList(void)
{
	m_alphaEntityList.Clear();
	m_alphaBoatAtomicList.Clear();
	m_alphaUnderwaterEntityList.Clear();
#ifdef NEW_RENDERER
	m_alphaBuildingList.Clear();
#endif
#ifdef WII
	ResetWiiAlphaStats();
#endif
}

bool
CVisibilityPlugins::InsertEntityIntoSortedList(CEntity *e, float dist)
{
#ifdef FIX_BUGS
	if (!e->m_rwObject) return true;
#endif

	AlphaObjectInfo item;
	item.entity = e;
	item.sort = dist;
#ifdef NEW_RENDERER
	if(gbNewRenderer && e->IsBuilding()){
		bool inserted = !!m_alphaBuildingList.InsertSorted(item);
#ifdef WII
		if(inserted){
			gWiiAlphaStats.insertedEntities++;
			gWiiAlphaStats.insertedBuildings++;
		}
#endif
		return inserted;
	}
#endif
	if(e->bUnderwater && m_alphaUnderwaterEntityList.InsertSorted(item)){
#ifdef WII
		gWiiAlphaStats.insertedEntities++;
		gWiiAlphaStats.insertedUnderwater++;
#endif
		return true;
	}
	bool inserted = !!m_alphaEntityList.InsertSorted(item);
#ifdef WII
	if(inserted)
		gWiiAlphaStats.insertedEntities++;
#endif
	return inserted;
}

void
CVisibilityPlugins::InitAlphaAtomicList(void)
{
	m_alphaList.Clear();
}

bool
CVisibilityPlugins::InsertAtomicIntoSortedList(RpAtomic *a, float dist)
{
	AlphaObjectInfo item;
	item.atomic = a;
	item.sort = dist;
	return !!m_alphaList.InsertSorted(item);
}

bool
CVisibilityPlugins::InsertAtomicIntoBoatSortedList(RpAtomic *a, float dist)
{
	AlphaObjectInfo item;
	item.atomic = a;
	item.sort = dist;
	return !!m_alphaBoatAtomicList.InsertSorted(item);
}

// can't increase this yet unfortunately...
// probably have to fix fading for this so material alpha isn't overwritten
#define VEHICLE_LODDIST_MULTIPLIER (TheCamera.GenerationDistMultiplier)

void
CVisibilityPlugins::SetRenderWareCamera(RwCamera *camera)
{
	ms_pCamera = camera;
	ms_pCameraPosn = RwMatrixGetPos(RwFrameGetMatrix(RwCameraGetFrame(camera)));

	if(TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_TOPDOWN ||
	   TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_TOP_DOWN_PED)
		ms_cullCompsDist = 1000000.0f;
	else
		ms_cullCompsDist = sq(TheCamera.LODDistMultiplier * 20.0f);

	ms_vehicleLod0Dist = sq(70.0f * VEHICLE_LODDIST_MULTIPLIER);
	ms_vehicleLod1Dist = sq(90.0f * VEHICLE_LODDIST_MULTIPLIER);
	ms_vehicleFadeDist = sq(100.0f * VEHICLE_LODDIST_MULTIPLIER);
	ms_bigVehicleLod0Dist = sq(60.0f * VEHICLE_LODDIST_MULTIPLIER);
	ms_bigVehicleLod1Dist = sq(150.0f * VEHICLE_LODDIST_MULTIPLIER);
	ms_pedLod1Dist = sq(60.0f * TheCamera.LODDistMultiplier);
	ms_pedFadeDist = sq(70.0f * TheCamera.LODDistMultiplier);
}

static float DistToCameraSq;
static float PitchToCamera;

void
CVisibilityPlugins::SetupVehicleVariables(RpClump *vehicle)
{
	if (RwObjectGetType((RwObject*)vehicle) != rpCLUMP)
		return;
	DistToCameraSq = GetDistanceSquaredFromCamera(RpClumpGetFrame(vehicle));
	RwV3d distToCam;
	RwV3dSub(&distToCam, ms_pCameraPosn, &RwFrameGetMatrix(RpClumpGetFrame(vehicle))->pos);
	float dist2d = Sqrt(SQR(distToCam.x) + SQR(distToCam.y));
	PitchToCamera = Atan2(distToCam.z, dist2d);
}

RpMaterial*
SetAlphaCB(RpMaterial *material, void *data)
{
	((RwRGBA*)RpMaterialGetColor(material))->alpha = (uint8)(uintptr)data;
	return material;
}

RpMaterial*
SetTextureCB(RpMaterial *material, void *data)
{
	RpMaterialSetTexture(material, (RwTexture*)data);
	return material;
}

void
CVisibilityPlugins::RenderAtomicList(CLinkList<AlphaObjectInfo> &list)
{
	CLink<AlphaObjectInfo> *node;
	for(node = list.tail.prev; node != &list.head; node = node->prev)
		RENDERCALLBACK(node->item.atomic);
}

void
CVisibilityPlugins::RenderAlphaAtomics(void)
{
	RenderAtomicList(m_alphaList);
}

void
CVisibilityPlugins::RenderBoatAlphaAtomics(void)
{
	SetCullMode(rwCULLMODECULLNONE);
	CLink<AlphaObjectInfo> *node;
	for(node = m_alphaBoatAtomicList.tail.prev; node != &m_alphaBoatAtomicList.head; node = node->prev){
#ifdef WII
		gWiiAlphaStats.renderedBoatAtomics++;
#endif
		RENDERCALLBACK(node->item.atomic);
	}
	SetCullMode(rwCULLMODECULLBACK);
}

void
CVisibilityPlugins::RenderFadingEntities(CLinkList<AlphaObjectInfo> &list)
{
	CLink<AlphaObjectInfo> *node;
	CSimpleModelInfo *mi;
	for(node = list.tail.prev; node != &list.head; node = node->prev){
		CEntity *e = node->item.entity;
		if(e->m_rwObject == nil)
			continue;
#ifdef EXTENDED_PIPELINES
		if(CustomPipes::bRenderingEnvMap && (e->IsPed() || e->IsVehicle()))
			continue;
#endif
		mi = (CSimpleModelInfo *)CModelInfo::GetModelInfo(e->GetModelIndex());
		if(mi->GetModelType() == MITYPE_SIMPLE && mi->m_noZwrite)
			RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, FALSE);

		if(e->bDistanceFade){
#ifdef WII
			gWiiAlphaStats.renderedDistanceFade++;
#endif
			DeActivateDirectional();
			SetAmbientColours();
			e->bImBeingRendered = true;
			PUSH_RENDERGROUP(mi->GetModelName());
			RenderFadingAtomic((RpAtomic*)e->m_rwObject, node->item.sort);
			POP_RENDERGROUP();
			e->bImBeingRendered = false;
		}else{
#ifdef WII
			gWiiAlphaStats.renderedSorted++;
#endif
			CRenderer::RenderOneNonRoad(e);
		}

		if(mi->GetModelType() == MITYPE_SIMPLE && mi->m_noZwrite)
			RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
	}
}

void
CVisibilityPlugins::RenderFadingEntities(void)
{
	RenderFadingEntities(m_alphaEntityList);
	RenderBoatAlphaAtomics();
}

void
CVisibilityPlugins::RenderFadingUnderwaterEntities(void)
{
	RenderFadingEntities(m_alphaUnderwaterEntityList);
}

RpAtomic*
CVisibilityPlugins::RenderWheelAtomicCB(RpAtomic *atomic)
{
	RpAtomic *lodatm;
	float len;
	CSimpleModelInfo *mi;

	mi = GetAtomicModelInfo(atomic);
	len = Sqrt(DistToCameraSq);
	lodatm = mi->GetAtomicFromDistance(len * TheCamera.LODDistMultiplier / VEHICLE_LODDIST_MULTIPLIER);
	if(lodatm){
		if(RpAtomicGetGeometry(lodatm) != RpAtomicGetGeometry(atomic))
			RpAtomicSetGeometry(atomic, RpAtomicGetGeometry(lodatm), rpATOMICSAMEBOUNDINGSPHERE);
		RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderObjNormalAtomic(RpAtomic *atomic)
{
	RwMatrix *m;
	RwV3d view;
	float len;

	m = RwFrameGetLTM(RpAtomicGetFrame(atomic));
	RwV3dSub(&view, RwMatrixGetPos(m), ms_pCameraPosn);
	len = RwV3dLength(&view);
	if(RwV3dDotProduct(&view, RwMatrixGetUp(m)) < -0.3f*len && len > 8.0f)
		return atomic;
	RENDERCALLBACK(atomic);
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderAlphaAtomic(RpAtomic *atomic, int alpha)
{
	RpGeometry *geo;
	uint32 flags;

	geo = RpAtomicGetGeometry(atomic);
	flags = RpGeometryGetFlags(geo);
	RpGeometrySetFlags(geo, flags | rpGEOMETRYMODULATEMATERIALCOLOR);
	RpGeometryForAllMaterials(geo, SetAlphaCB, (void*)alpha);
	RENDERCALLBACK(atomic);
	RpGeometryForAllMaterials(geo, SetAlphaCB, (void*)255);
	RpGeometrySetFlags(geo, flags);
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderWeaponCB(RpAtomic *atomic)
{
	RwMatrix *m;
	RwV3d view;
	float maxdist, distsq;
	CSimpleModelInfo *mi;

	mi = GetAtomicModelInfo(atomic);
	m = RwFrameGetLTM(RpAtomicGetFrame(atomic));
	RwV3dSub(&view, RwMatrixGetPos(m), ms_pCameraPosn);
	maxdist = mi->GetLodDistance(0);
	distsq = RwV3dDotProduct(&view, &view);
	if(distsq < maxdist*maxdist)
		RENDERCALLBACK(atomic);
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderFadingAtomic(RpAtomic *atomic, float camdist)
{
	RpAtomic *lodatm;
	float fadefactor;
	uint32 alpha;
	CSimpleModelInfo *mi;

	mi = GetAtomicModelInfo(atomic);
	lodatm = mi->GetAtomicFromDistance(camdist - FADE_DISTANCE);
	if(mi->m_additive)
		RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDONE);

	fadefactor = (mi->GetLargestLodDistance() - (camdist - FADE_DISTANCE))/FADE_DISTANCE;
	if(fadefactor > 1.0f)
		fadefactor = 1.0f;
	alpha = mi->m_alpha * fadefactor;
	if(alpha == 255)
		RENDERCALLBACK(atomic);
	else{
		RpGeometry *geo = RpAtomicGetGeometry(lodatm);
		uint32 flags = RpGeometryGetFlags(geo);
		RpGeometrySetFlags(geo, flags | rpGEOMETRYMODULATEMATERIALCOLOR);
		RpGeometryForAllMaterials(geo, SetAlphaCB, (void*)alpha);
		if(geo != RpAtomicGetGeometry(atomic))
			RpAtomicSetGeometry(atomic, geo, rpATOMICSAMEBOUNDINGSPHERE); // originally 5 (mistake?)
		RENDERCALLBACK(atomic);
		RpGeometryForAllMaterials(geo, SetAlphaCB, (void*)255);
		RpGeometrySetFlags(geo, flags);
	}

	if(mi->m_additive)
		RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);

	return atomic;
}



RpAtomic*
CVisibilityPlugins::RenderVehicleHiDetailCB(RpAtomic *atomic)
{
	RwFrame *clumpframe;
	float dot;
	uint32 flags;

	clumpframe = RpClumpGetFrame(RpAtomicGetClump(atomic));
	if(DistToCameraSq < ms_vehicleLod0Dist){
		flags = GetAtomicId(atomic);
		if(DistToCameraSq > ms_cullCompsDist && (flags & ATOMIC_FLAG_NOCULL) == 0 && PitchToCamera < 0.2f){
			dot = GetDotProductWithCameraVector(RwFrameGetLTM(RpAtomicGetFrame(atomic)),
				RwFrameGetLTM(clumpframe), flags);
			if(dot > 0.0f && ((flags & ATOMIC_FLAG_ANGLECULL) || 0.1f*DistToCameraSq < dot*dot))
				return atomic;
		}
		RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleHiDetailAlphaCB(RpAtomic *atomic)
{
	RwFrame *clumpframe;
	float dot;
	uint32 flags;

	clumpframe = RpClumpGetFrame(RpAtomicGetClump(atomic));
	if(DistToCameraSq < ms_vehicleLod0Dist){
		flags = GetAtomicId(atomic);
		dot = GetDotProductWithCameraVector(RwFrameGetLTM(RpAtomicGetFrame(atomic)),
			RwFrameGetLTM(clumpframe), flags);
		if(DistToCameraSq > ms_cullCompsDist && (flags & ATOMIC_FLAG_NOCULL) == 0 && PitchToCamera < 0.2f)
			if(dot > 0.0f && ((flags & ATOMIC_FLAG_ANGLECULL) || 0.1f*DistToCameraSq < dot*dot))
				return atomic;

		if(flags & ATOMIC_FLAG_DRAWLAST){
			// sort before clump
			if(!InsertAtomicIntoSortedList(atomic, DistToCameraSq - 0.0001f))
				RENDERCALLBACK(atomic);
		}else{
			if(!InsertAtomicIntoSortedList(atomic, DistToCameraSq + dot))
				RENDERCALLBACK(atomic);
		}
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleHiDetailCB_BigVehicle(RpAtomic *atomic)
{
	RwFrame *clumpframe;
	float dot;
	uint32 flags;

	clumpframe = RpClumpGetFrame(RpAtomicGetClump(atomic));
	if(DistToCameraSq < ms_bigVehicleLod0Dist){
		flags = GetAtomicId(atomic);
		if(DistToCameraSq > ms_cullCompsDist && (flags & ATOMIC_FLAG_NOCULL) == 0 && PitchToCamera < 0.2f){
			dot = GetDotProductWithCameraVector(RwFrameGetLTM(RpAtomicGetFrame(atomic)),
				RwFrameGetLTM(clumpframe), flags);
			if(dot > 0.0f)
				return atomic;
		}
		RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleHiDetailAlphaCB_BigVehicle(RpAtomic *atomic)
{
	RwFrame *clumpframe;
	float dot;
	uint32 flags;

	clumpframe = RpClumpGetFrame(RpAtomicGetClump(atomic));
	if(DistToCameraSq < ms_bigVehicleLod0Dist){
		flags = GetAtomicId(atomic);
		dot = GetDotProductWithCameraVector(RwFrameGetLTM(RpAtomicGetFrame(atomic)),
			RwFrameGetLTM(clumpframe), flags);
		if(DistToCameraSq > ms_cullCompsDist && (flags & ATOMIC_FLAG_NOCULL) == 0 && PitchToCamera < 0.2f)
			if(dot > 0.0f && ((flags & ATOMIC_FLAG_ANGLECULL) || 0.1f*DistToCameraSq < dot*dot))
				return atomic;

		if(!InsertAtomicIntoSortedList(atomic, DistToCameraSq + dot))
			RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleHiDetailCB_Boat(RpAtomic *atomic)
{
	if(DistToCameraSq < ms_vehicleLod0Dist)
		RENDERCALLBACK(atomic);
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleHiDetailAlphaCB_Boat(RpAtomic *atomic)
{
	if(DistToCameraSq < ms_vehicleLod0Dist){
		if(GetAtomicId(atomic) & ATOMIC_FLAG_DRAWLAST){
			if(!InsertAtomicIntoBoatSortedList(atomic, DistToCameraSq))
				RENDERCALLBACK(atomic);
		}else
			RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleLoDetailCB_Boat(RpAtomic *atomic)
{
	RpClump *clump;
	int32 alpha;

	clump = RpAtomicGetClump(atomic);
	if(DistToCameraSq >= ms_vehicleLod0Dist){
		alpha = GetClumpAlpha(clump);
		if(alpha == 255)
			RENDERCALLBACK(atomic);
		else
			RenderAlphaAtomic(atomic, alpha);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleLowDetailCB_BigVehicle(RpAtomic *atomic)
{
	RwFrame *clumpframe;
	float dot;
	uint32 flags;

	clumpframe = RpClumpGetFrame(RpAtomicGetClump(atomic));
	if(DistToCameraSq >= ms_bigVehicleLod0Dist &&
	   DistToCameraSq < ms_bigVehicleLod1Dist){
		flags = GetAtomicId(atomic);
		if(DistToCameraSq > ms_cullCompsDist && (flags & ATOMIC_FLAG_NOCULL) == 0 && PitchToCamera < 0.2f){
			dot = GetDotProductWithCameraVector(RwFrameGetLTM(RpAtomicGetFrame(atomic)),
				RwFrameGetLTM(clumpframe), flags);
			if(dot > 0.0f)
				return atomic;
		}
		RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleLowDetailAlphaCB_BigVehicle(RpAtomic *atomic)
{
	RwFrame *clumpframe;
	float dot;
	uint32 flags;

	clumpframe = RpClumpGetFrame(RpAtomicGetClump(atomic));
	if(DistToCameraSq >= ms_bigVehicleLod0Dist &&
	   DistToCameraSq < ms_bigVehicleLod1Dist){
		flags = GetAtomicId(atomic);
		dot = GetDotProductWithCameraVector(RwFrameGetLTM(RpAtomicGetFrame(atomic)),
			RwFrameGetLTM(clumpframe), flags);
		if(dot > 0.0f)
			if(DistToCameraSq > ms_cullCompsDist && (flags & ATOMIC_FLAG_NOCULL) == 0 && PitchToCamera < 0.2f)
				return atomic;

		if(!InsertAtomicIntoSortedList(atomic, DistToCameraSq + dot))
			RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleReallyLowDetailCB(RpAtomic *atomic)
{
	RpClump *clump;
	int32 alpha;

	clump = RpAtomicGetClump(atomic);
	if(DistToCameraSq >= ms_vehicleLod0Dist){
		alpha = GetClumpAlpha(clump);
		if(alpha == 255)
			RENDERCALLBACK(atomic);
		else
			RenderAlphaAtomic(atomic, alpha);
	}
	return atomic;

}

RpAtomic*
CVisibilityPlugins::RenderVehicleReallyLowDetailCB_BigVehicle(RpAtomic *atomic)
{
	if(DistToCameraSq >= ms_bigVehicleLod1Dist)
		RENDERCALLBACK(atomic);
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderTrainHiDetailCB(RpAtomic *atomic)
{
	RwFrame *clumpframe;
	float dot;
	uint32 flags;

	clumpframe = RpClumpGetFrame(RpAtomicGetClump(atomic));
	if(DistToCameraSq < ms_bigVehicleLod1Dist){
		flags = GetAtomicId(atomic);
		if(DistToCameraSq > ms_cullCompsDist && (flags & ATOMIC_FLAG_NOCULL) == 0 && PitchToCamera < 0.2f){
			dot = GetDotProductWithCameraVector(RwFrameGetLTM(RpAtomicGetFrame(atomic)),
				RwFrameGetLTM(clumpframe), flags);
			if(dot > 0.0f && ((flags & ATOMIC_FLAG_ANGLECULL) || 0.1f*DistToCameraSq < dot*dot))
				return atomic;
		}
		RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderTrainHiDetailAlphaCB(RpAtomic *atomic)
{
	RwFrame *clumpframe;
	float dot;
	uint32 flags;

	clumpframe = RpClumpGetFrame(RpAtomicGetClump(atomic));
	if(DistToCameraSq < ms_bigVehicleLod1Dist){
		flags = GetAtomicId(atomic);
		dot = GetDotProductWithCameraVector(RwFrameGetLTM(RpAtomicGetFrame(atomic)),
			RwFrameGetLTM(clumpframe), flags);
		if(DistToCameraSq > ms_cullCompsDist && (flags & ATOMIC_FLAG_NOCULL) == 0 && PitchToCamera < 0.2f)
			if(dot > 0.0f && ((flags & ATOMIC_FLAG_ANGLECULL) || 0.1f*DistToCameraSq < dot*dot))
				return atomic;

		if(flags & ATOMIC_FLAG_DRAWLAST){
			if(!InsertAtomicIntoSortedList(atomic, DistToCameraSq))
				RENDERCALLBACK(atomic);
		}else{
			if(!InsertAtomicIntoSortedList(atomic, DistToCameraSq + dot))
				RENDERCALLBACK(atomic);
		}
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleRotorAlphaCB(RpAtomic *atomic)
{
	RwFrame *clumpframe;
	float dot;
	RwV3d cam2atm;

	clumpframe = RpClumpGetFrame(RpAtomicGetClump(atomic));
	if(DistToCameraSq < ms_bigVehicleLod1Dist){
		RwV3dSub(&cam2atm, &RwFrameGetLTM(RpAtomicGetFrame(atomic))->pos, ms_pCameraPosn);
		dot = RwV3dDotProduct(&cam2atm, &RwFrameGetLTM(clumpframe)->at);
		if(!InsertAtomicIntoSortedList(atomic, DistToCameraSq + dot*20.0f))
			RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderVehicleTailRotorAlphaCB(RpAtomic *atomic)
{
	RwMatrix *clumpMat, *atmMat;
	float dot;
	RwV3d cam2atm;

	if(DistToCameraSq < ms_bigVehicleLod0Dist){
		atmMat = RwFrameGetLTM(RpAtomicGetFrame(atomic));
		clumpMat = RwFrameGetLTM(RpClumpGetFrame(RpAtomicGetClump(atomic)));
		RwV3dSub(&cam2atm, &atmMat->pos, ms_pCameraPosn);
		dot = RwV3dDotProduct(&cam2atm, &clumpMat->up) + RwV3dDotProduct(&cam2atm, &clumpMat->right);
		if(!InsertAtomicIntoSortedList(atomic, DistToCameraSq - dot))
			RENDERCALLBACK(atomic);
	}
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderPlayerCB(RpAtomic *atomic)
{
	if(CWorld::Players[0].m_pSkinTexture)
		RpGeometryForAllMaterials(RpAtomicGetGeometry(atomic), SetTextureCB, CWorld::Players[0].m_pSkinTexture);
	RENDERCALLBACK(atomic);
	return atomic;
}

RpAtomic*
CVisibilityPlugins::RenderPedCB(RpAtomic *atomic)
{
	RpClump *clump;
	float dist;
	int32 alpha;

	clump = RpAtomicGetClump(atomic);
	dist = GetDistanceSquaredFromCamera(RpClumpGetFrame(clump));
	if(dist < ms_pedLod1Dist){
		alpha = GetClumpAlpha(clump);
		if(alpha == 255)
			RENDERCALLBACK(atomic);
		else
			RenderAlphaAtomic(atomic, alpha);
	}
	return atomic;
}

float
CVisibilityPlugins::GetDistanceSquaredFromCamera(RwV3d *pos)
{
	RwV3d dist;
	RwV3dSub(&dist, pos, ms_pCameraPosn);
	return RwV3dDotProduct(&dist, &dist);
}

float
CVisibilityPlugins::GetDistanceSquaredFromCamera(RwFrame *frame)
{
	RwMatrix *m;
	RwV3d dist;
	m = RwFrameGetLTM(frame);
	RwV3dSub(&dist, RwMatrixGetPos(m), ms_pCameraPosn);
	return RwV3dDotProduct(&dist, &dist);
}

float
CVisibilityPlugins::GetDotProductWithCameraVector(RwMatrix *atomicMat, RwMatrix *clumpMat, uint32 flags)
{
	RwV3d dist;
	float dot, dotdoor;

	// Vehicle forward is the y axis (RwMatrix.up)
	// Vehicle right is the x axis (RwMatrix.right)

	RwV3dSub(&dist, RwMatrixGetPos(atomicMat), ms_pCameraPosn);
	// forward/backward facing
	if(flags & (ATOMIC_FLAG_FRONT | ATOMIC_FLAG_REAR))
		dot = RwV3dDotProduct(&dist, RwMatrixGetUp(clumpMat));
	// left/right facing
	else if(flags & (ATOMIC_FLAG_LEFT | ATOMIC_FLAG_RIGHT))
		dot = RwV3dDotProduct(&dist, RwMatrixGetRight(clumpMat));
	else
		dot = 0.0f;
	if(flags & (ATOMIC_FLAG_LEFT | ATOMIC_FLAG_REAR))
		dot = -dot;

	if(flags & (ATOMIC_FLAG_REARDOOR | ATOMIC_FLAG_FRONTDOOR)){
		if(flags & ATOMIC_FLAG_REARDOOR)
			dotdoor = -RwV3dDotProduct(&dist, RwMatrixGetUp(clumpMat));
		else if(flags & ATOMIC_FLAG_FRONTDOOR)
			dotdoor = RwV3dDotProduct(&dist, RwMatrixGetUp(clumpMat));
		else
			dotdoor = 0.0f;

		if(dot < 0.0f && dotdoor < 0.0f)
			dot += dotdoor;
		if(dot > 0.0f && dotdoor > 0.0f)
			dot += dotdoor;
	}

	return dot;
}

/* These are all unused */

bool
CVisibilityPlugins::DefaultVisibilityCB(RpClump *clump)
{
	return true;
}

bool
CVisibilityPlugins::FrustumSphereCB(RpClump *clump)
{
	RwSphere sphere;
	RwFrame *frame = RpClumpGetFrame(clump);

	CClumpModelInfo *modelInfo = (CClumpModelInfo*)GetFrameHierarchyId(frame);
	sphere.radius = modelInfo->GetColModel()->boundingSphere.radius;
	sphere.center.x = modelInfo->GetColModel()->boundingSphere.center.x;
	sphere.center.y = modelInfo->GetColModel()->boundingSphere.center.y;
	sphere.center.z = modelInfo->GetColModel()->boundingSphere.center.z;
	RwV3dTransformPoints(&sphere.center, &sphere.center, 1, RwFrameGetLTM(frame));
	return RwCameraFrustumTestSphere(ms_pCamera, &sphere) != rwSPHEREOUTSIDE;
}

bool
CVisibilityPlugins::MloVisibilityCB(RpClump *clump)
{
	RwFrame *frame = RpClumpGetFrame(clump);
	CMloModelInfo *modelInfo = (CMloModelInfo*)GetFrameHierarchyId(frame);
	if (SQR(modelInfo->drawDist) < GetDistanceSquaredFromCamera(frame))
		return false;
	return CVisibilityPlugins::FrustumSphereCB(clump);
}

bool
CVisibilityPlugins::VehicleVisibilityCB(RpClump *clump)
{
	RwFrame *frame = RpClumpGetFrame(clump);
	if (ms_vehicleLod1Dist < GetDistanceSquaredFromCamera(frame))
		return false;
	return FrustumSphereCB(clump);
}

bool
CVisibilityPlugins::VehicleVisibilityCB_BigVehicle(RpClump *clump)
{
	return FrustumSphereCB(clump);
}




//
// RW Plugins
//

enum
{
	ID_VISIBILITYATOMIC = MAKECHUNKID(rwVENDORID_ROCKSTAR, 0x00),
	ID_VISIBILITYCLUMP  = MAKECHUNKID(rwVENDORID_ROCKSTAR, 0x01),
	ID_VISIBILITYFRAME  = MAKECHUNKID(rwVENDORID_ROCKSTAR, 0x02),
};

bool
CVisibilityPlugins::PluginAttach(void)
{
	ms_atomicPluginOffset = RpAtomicRegisterPlugin(sizeof(AtomicExt),
		ID_VISIBILITYATOMIC,
		AtomicConstructor, AtomicDestructor, AtomicCopyConstructor);

	ms_framePluginOffset = RwFrameRegisterPlugin(sizeof(FrameExt),
		ID_VISIBILITYFRAME,
		FrameConstructor, FrameDestructor, FrameCopyConstructor);

	ms_clumpPluginOffset = RpClumpRegisterPlugin(sizeof(ClumpExt),
		ID_VISIBILITYCLUMP,
		ClumpConstructor, ClumpDestructor, ClumpCopyConstructor);
	return ms_atomicPluginOffset != -1 && ms_clumpPluginOffset != -1;
}

#define ATOMICEXT(o) (RWPLUGINOFFSET(AtomicExt, o, ms_atomicPluginOffset))
#define FRAMEEXT(o) (RWPLUGINOFFSET(FrameExt, o, ms_framePluginOffset))
#define CLUMPEXT(o) (RWPLUGINOFFSET(ClumpExt, o, ms_clumpPluginOffset))

//
// Atomic
//

void*
CVisibilityPlugins::AtomicConstructor(void *object, int32, int32)
{
	ATOMICEXT(object)->modelInfo = nil;
	return object;
}

void*
CVisibilityPlugins::AtomicDestructor(void *object, int32, int32)
{
	return object;
}

void*
CVisibilityPlugins::AtomicCopyConstructor(void *dst, const void *src, int32, int32)
{
	*ATOMICEXT(dst) = *ATOMICEXT(src);
	return dst;
}

void
CVisibilityPlugins::SetAtomicModelInfo(RpAtomic *atomic,
                                       CSimpleModelInfo *modelInfo)
{
	AtomicExt *ext = ATOMICEXT(atomic);
	ext->modelInfo = modelInfo;
}

CSimpleModelInfo*
CVisibilityPlugins::GetAtomicModelInfo(RpAtomic *atomic)
{
	return ATOMICEXT(atomic)->modelInfo;
}

void
CVisibilityPlugins::SetAtomicFlag(RpAtomic *atomic, int f)
{
	ATOMICEXT(atomic)->flags |= f;
}

void
CVisibilityPlugins::ClearAtomicFlag(RpAtomic *atomic, int f)
{
	ATOMICEXT(atomic)->flags &= ~f;
}

void
CVisibilityPlugins::SetAtomicId(RpAtomic *atomic, int id)
{
	ATOMICEXT(atomic)->flags = id;
}

int
CVisibilityPlugins::GetAtomicId(RpAtomic *atomic)
{
	return ATOMICEXT(atomic)->flags;
}

void
CVisibilityPlugins::SetAtomicRenderCallback(RpAtomic *atomic, RpAtomicCallBackRender cb)
{
	if(cb == nil)
		cb = RENDERCALLBACK;	// not necessary
	RpAtomicSetRenderCallBack(atomic, cb);
}

//
// Frame
//

void*
CVisibilityPlugins::FrameConstructor(void *object, int32, int32)
{
	FRAMEEXT(object)->id = 0;
	return object;
}

void*
CVisibilityPlugins::FrameDestructor(void *object, int32, int32)
{
	return object;
}

void*
CVisibilityPlugins::FrameCopyConstructor(void *dst, const void *src, int32, int32)
{
	*FRAMEEXT(dst) = *FRAMEEXT(src);
	return dst;
}

void
CVisibilityPlugins::SetFrameHierarchyId(RwFrame *frame, intptr id)
{
	FRAMEEXT(frame)->id = id;
}

intptr
CVisibilityPlugins::GetFrameHierarchyId(RwFrame *frame)
{
	return FRAMEEXT(frame)->id;
}


//
// Clump
//

void*
CVisibilityPlugins::ClumpConstructor(void *object, int32, int32)
{
	ClumpExt *ext = CLUMPEXT(object);
	ext->visibilityCB = DefaultVisibilityCB;
	ext->alpha = 0xFF;
	return object;
}

void*
CVisibilityPlugins::ClumpDestructor(void *object, int32, int32)
{
	return object;
}

void*
CVisibilityPlugins::ClumpCopyConstructor(void *dst, const void *src, int32, int32)
{
	CLUMPEXT(dst)->visibilityCB = CLUMPEXT(src)->visibilityCB;
	return dst;
}

void
CVisibilityPlugins::SetClumpModelInfo(RpClump *clump, CClumpModelInfo *modelInfo)
{
	CVehicleModelInfo *vmi;
	SetFrameHierarchyId(RpClumpGetFrame(clump), (intptr)modelInfo);

	// Unused
	switch (modelInfo->GetModelType()) {
	case MITYPE_MLO:
		CLUMPEXT(clump)->visibilityCB = MloVisibilityCB;
		break;
	case MITYPE_VEHICLE:
		vmi = (CVehicleModelInfo*)modelInfo;
		if(vmi->m_vehicleType == VEHICLE_TYPE_TRAIN ||
		   vmi->m_vehicleType == VEHICLE_TYPE_HELI ||
		   vmi->m_vehicleType == VEHICLE_TYPE_PLANE)
			CLUMPEXT(clump)->visibilityCB = VehicleVisibilityCB_BigVehicle;
		else
			CLUMPEXT(clump)->visibilityCB = VehicleVisibilityCB;
		break;
	default: break;
	}
}

CClumpModelInfo*
CVisibilityPlugins::GetClumpModelInfo(RpClump *clump)
{
	return (CClumpModelInfo*)GetFrameHierarchyId(RpClumpGetFrame(clump));
}

void
CVisibilityPlugins::SetClumpAlpha(RpClump *clump, int alpha)
{
	CLUMPEXT(clump)->alpha = alpha;
}

int
CVisibilityPlugins::GetClumpAlpha(RpClump *clump)
{
	return CLUMPEXT(clump)->alpha;
}

bool
CVisibilityPlugins::IsClumpVisible(RpClump *clump)
{
	return CLUMPEXT(clump)->visibilityCB(clump);
}
