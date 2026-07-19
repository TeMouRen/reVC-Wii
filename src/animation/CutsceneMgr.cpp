#include "common.h"

#include "General.h"
#include "Game.h"
#include "CutsceneMgr.h"
#include "Directory.h"
#include "Camera.h"
#include "Streaming.h"
#include "FileMgr.h"
#include "main.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "AnimBlendAssocGroup.h"
#include "AnimBlendClumpData.h"
#include "Pad.h"
#include "DMAudio.h"
#include "World.h"
#include "PlayerPed.h"
#include "Wanted.h"
#include "RpAnimBlend.h"
#include "ModelIndices.h"
#include "TempColModels.h"
#include "ColStore.h"
#include "Radar.h"
#include "Pools.h"

#ifdef USE_CUSTOM_ALLOCATOR
#include "MemoryHeap.h"
#endif

#if GX_CONSOLE
#include "gxmemory.h"
#endif

#if GX_CONSOLE
static uint32
GxIntroCutsceneTexBudget(void)
{
#ifdef WII
	return 44u * 1024u * 1024u;
#else
	return 18u * 1024u * 1024u;
#endif
}

static uint32
GxCutsceneLoadTexBudget(void)
{
#ifdef WII
	return 48u * 1024u * 1024u;
#else
	return 20u * 1024u * 1024u;
#endif
}

static uint32
GxPostCutsceneTexBudget(void)
{
#ifdef WII
	return 48u * 1024u * 1024u;
#else
	return 22u * 1024u * 1024u;
#endif
}
#endif

static bool
IsIntroCutsceneName(const char *name)
{
	return name != nil &&
		!CGeneral::faststrncmp(name, "INT_", 4);
}

static int32
CutsceneLargestFreeBlockForLog(void)
{
#ifdef USE_CUSTOM_ALLOCATOR
	return gMainHeap.GetLargestFreeBlock();
#else
	return -1;
#endif
}

const struct {
	const char *szTrackName;
	int iTrackId;
} musicNameIdAssoc[] = {
	{ "ASS_1", STREAMED_SOUND_CUTSCENE_ASS_1 },
	{ "ASS_2", STREAMED_SOUND_CUTSCENE_ASS_2 },
	{ "BANK_1", STREAMED_SOUND_CUTSCENE_BANK_1 },
	{ "BANK_2A", STREAMED_SOUND_CUTSCENE_BANK_2A },
	{ "BANK_2B", STREAMED_SOUND_CUTSCENE_BANK_2B },
	{ "BANK_3A", STREAMED_SOUND_CUTSCENE_BANK_3A },
	{ "BANK_3B", STREAMED_SOUND_CUTSCENE_BANK_3B },
	{ "BANK_4", STREAMED_SOUND_CUTSCENE_BANK_4 },
	{ "BIKE_1", STREAMED_SOUND_CUTSCENE_BIKE_1 },
	{ "BIKE_2", STREAMED_SOUND_CUTSCENE_BIKE_2 },
	{ "BIKE_3", STREAMED_SOUND_CUTSCENE_BIKE_3 },
	{ "BUD_1", STREAMED_SOUND_CUTSCENE_BUD_1 },
	{ "BUD_2", STREAMED_SOUND_CUTSCENE_BUD_2 },
	{ "BUD_3", STREAMED_SOUND_CUTSCENE_BUD_3 },
	{ "CAP_1", STREAMED_SOUND_CUTSCENE_CAP_1 },
	{ "CAR_1", STREAMED_SOUND_CUTSCENE_CAR_1 },
	{ "CNT_1A", STREAMED_SOUND_CUTSCENE_CNT_1A },
	{ "CNT_1B", STREAMED_SOUND_CUTSCENE_CNT_1B },
	{ "CNT_2", STREAMED_SOUND_CUTSCENE_CNT_2 },
	{ "COK_1", STREAMED_SOUND_CUTSCENE_COK_1 },
	{ "COK_2A", STREAMED_SOUND_CUTSCENE_COK_2A },
	{ "COK_2B", STREAMED_SOUND_CUTSCENE_COK_2B },
	{ "COK_3", STREAMED_SOUND_CUTSCENE_COK_3 },
	{ "COK_4A", STREAMED_SOUND_CUTSCENE_COK_4A },
	{ "COK_4A2", STREAMED_SOUND_CUTSCENE_COK_4A2 },
	{ "COK_4B", STREAMED_SOUND_CUTSCENE_COK_4B },
	{ "COL_1", STREAMED_SOUND_CUTSCENE_COL_1 },
	{ "COL_2", STREAMED_SOUND_CUTSCENE_COL_2 },
	{ "COL_3A", STREAMED_SOUND_CUTSCENE_COL_3A },
	{ "COL_4A", STREAMED_SOUND_CUTSCENE_COL_4A },
	{ "COL_5A", STREAMED_SOUND_CUTSCENE_COL_5A },
	{ "COL_5B", STREAMED_SOUND_CUTSCENE_COL_5B },
	{ "CUB_1", STREAMED_SOUND_CUTSCENE_CUB_1 },
	{ "CUB_2", STREAMED_SOUND_CUTSCENE_CUB_2 },
	{ "CUB_3", STREAMED_SOUND_CUTSCENE_CUB_3 },
	{ "CUB_4", STREAMED_SOUND_CUTSCENE_CUB_4 },
	{ "DRUG_1", STREAMED_SOUND_CUTSCENE_DRUG_1 },
	{ "FIN", STREAMED_SOUND_CUTSCENE_FIN },
	{ "FIN_2", STREAMED_SOUND_CUTSCENE_FIN2 },
	{ "FINALE", STREAMED_SOUND_CUTSCENE_FINALE },
	{ "HAT_1", STREAMED_SOUND_CUTSCENE_HAT_1 },
	{ "HAT_2", STREAMED_SOUND_CUTSCENE_HAT_2 },
	{ "HAT_3", STREAMED_SOUND_CUTSCENE_HAT_3 },
	{ "ICE_1", STREAMED_SOUND_CUTSCENE_ICE_1 },
	{ "INT_A", STREAMED_SOUND_CUTSCENE_INT_A },
	{ "INT_B", STREAMED_SOUND_CUTSCENE_INT_B },
	{ "INT_D", STREAMED_SOUND_CUTSCENE_INT_D },
	{ "INT_M", STREAMED_SOUND_CUTSCENE_INT_M },
	{ "LAW_1A", STREAMED_SOUND_CUTSCENE_LAW_1A },
	{ "LAW_1B", STREAMED_SOUND_CUTSCENE_LAW_1B },
	{ "LAW_2A", STREAMED_SOUND_CUTSCENE_LAW_2A },
	{ "LAW_2B", STREAMED_SOUND_CUTSCENE_LAW_2B },
	{ "LAW_2C", STREAMED_SOUND_CUTSCENE_LAW_2C },
	{ "LAW_3", STREAMED_SOUND_CUTSCENE_LAW_3 },
	{ "LAW_4", STREAMED_SOUND_CUTSCENE_LAW_4 },
	{ "PHIL_1", STREAMED_SOUND_CUTSCENE_PHIL_1 },
	{ "PHIL_2", STREAMED_SOUND_CUTSCENE_PHIL_2 },
	{ "PORN_1", STREAMED_SOUND_CUTSCENE_PORN_1 },
	{ "PORN_2", STREAMED_SOUND_CUTSCENE_PORN_2 },
	{ "PORN_3", STREAMED_SOUND_CUTSCENE_PORN_3 },
	{ "PORN_4", STREAMED_SOUND_CUTSCENE_PORN_4 },
	{ "RESC_1A", STREAMED_SOUND_CUTSCENE_RESC_1A },
	{ "ROK_1", STREAMED_SOUND_CUTSCENE_ROK_1 },
	{ "ROK_2", STREAMED_SOUND_CUTSCENE_ROK_2 },
	{ "ROK_3A", STREAMED_SOUND_CUTSCENE_ROK_3A },
	{ "STRIPA", STREAMED_SOUND_CUTSCENE_STRIPA },
	{ "TAX_1", STREAMED_SOUND_CUTSCENE_TAX_1 },
	{ "TEX_1", STREAMED_SOUND_CUTSCENE_TEX_1 },
	{ "TEX_2", STREAMED_SOUND_CUTSCENE_TEX_2 },
	{ "TEX_3", STREAMED_SOUND_CUTSCENE_TEX_3 },
	{ "GSPOT", STREAMED_SOUND_CUTSCENE_GLIGHT },
	{ "FIST", STREAMED_SOUND_CUTSCENE_FIST },
	{ "EL_PH1", STREAMED_SOUND_CUTSCENE_ELBURRO1_PH1 },
	{ "EL_PH2", STREAMED_SOUND_CUTSCENE_ELBURRO2_PH2 },
	{ NULL, 0 }
};

int
FindCutsceneAudioTrackId(const char *szCutsceneName)
{
	for (int i = 0; musicNameIdAssoc[i].szTrackName; i++) {
		if (!CGeneral::faststricmp(musicNameIdAssoc[i].szTrackName, szCutsceneName))
			return musicNameIdAssoc[i].iTrackId;
	}
	return -1;
}

bool CCutsceneMgr::ms_running;
bool CCutsceneMgr::ms_cutsceneProcessing;
CDirectory *CCutsceneMgr::ms_pCutsceneDir;
CCutsceneObject *CCutsceneMgr::ms_pCutsceneObjects[NUMCUTSCENEOBJECTS];
int32 CCutsceneMgr::ms_numCutsceneObjs;
bool CCutsceneMgr::ms_loaded;
bool CCutsceneMgr::ms_animLoaded;
bool CCutsceneMgr::ms_useLodMultiplier;
char CCutsceneMgr::ms_cutsceneName[CUTSCENENAMESIZE];
CAnimBlendAssocGroup CCutsceneMgr::ms_cutsceneAssociations;
CVector CCutsceneMgr::ms_cutsceneOffset;
float CCutsceneMgr::ms_cutsceneTimer;
bool CCutsceneMgr::ms_wasCutsceneSkipped;
uint32 CCutsceneMgr::ms_cutsceneLoadStatus;
bool CCutsceneMgr::ms_useCutsceneShadows = true;

bool bCamLoaded;
bool bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver; // pls don't shrink the name :P
int32 NumberOfSavedWeapons;
eWeaponType SavedWeaponIDs[TOTAL_WEAPON_SLOTS];
int32 SavedWeaponAmmo[TOTAL_WEAPON_SLOTS];
char uncompressedAnims[8][32];
uint32 numUncompressedAnims;


RpAtomic *
CalculateBoundingSphereRadiusCB(RpAtomic *atomic, void *data)
{
	float radius = RpAtomicGetBoundingSphere(atomic)->radius;
	RwV3d center = RpAtomicGetBoundingSphere(atomic)->center;

	for (RwFrame *frame = RpAtomicGetFrame(atomic); RwFrameGetParent(frame); frame = RwFrameGetParent(frame))
		RwV3dTransformPoints(&center, &center, 1, RwFrameGetMatrix(frame));

	float size = RwV3dLength(&center) + radius;
	if (size > *(float *)data)
		*(float *)data = size;
	return atomic;
}

void
CCutsceneMgr::Initialise(void)
{
	ms_numCutsceneObjs = 0;
	ms_loaded = false;
	ms_wasCutsceneSkipped = false;
	ms_running = false;
	ms_useLodMultiplier = false;
	ms_animLoaded = false;
	ms_cutsceneProcessing = false;
	ms_cutsceneLoadStatus = 0;
	ms_cutsceneTimer = 0.0f;
	ms_cutsceneName[0] = '\0';
	bCamLoaded = false;

	ms_pCutsceneDir = new CDirectory(CUTSCENEDIRSIZE);
	ms_pCutsceneDir->ReadDirFile("ANIM\\CUTS.DIR");

	numUncompressedAnims = 0;
	uncompressedAnims[0][0] = '\0';
}

void
CCutsceneMgr::Shutdown(void)
{
	delete ms_pCutsceneDir;
}

void
CCutsceneMgr::LoadCutsceneData(const char *szCutsceneName)
{
	int file;
	uint32 size;
	uint32 offset;
	CPlayerPed *pPlayerPed;

	ms_cutsceneProcessing = true;
	ms_wasCutsceneSkipped = false;
	CTimer::Suspend();
	if (!bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver)
		CStreaming::RemoveCurrentZonesModels();

	CStreaming::RemoveUnusedModelsInLoadedList();
	CGame::DrasticTidyUpMemory(true);

	strcpy(ms_cutsceneName, szCutsceneName);
#if GX_CONSOLE
	if (IsIntroCutsceneName(ms_cutsceneName)) {
		CGame::playingIntro = true;
		rw::gx::pushCriticalUiUploadContext(ms_cutsceneName);
		rw::gx::texPoolSetSoftBudget(GxIntroCutsceneTexBudget());
		rw::gx::texPoolEnforceBudget("intro-cutscene-preload");
	}
#endif

	RwStream *stream;
	stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, "ANIM\\CUTS.IMG");
	assert(stream);

	// Load animations
	sprintf(gString, "%s.IFP", szCutsceneName);
	if (ms_pCutsceneDir->FindItem(gString, offset, size)) {
#if GX_CONSOLE
		if (IsIntroCutsceneName(szCutsceneName)) {
			rw::gx::texPoolSetSoftBudget(GxIntroCutsceneTexBudget());
			rw::gx::texPoolEnforceBudget("intro-cutscene-anim");
		} else {
			// Wii now has a dedicated 64MB GX pool in MEM2, so keep cutscene
			// texture budgets close to gameplay quality instead of forcing the
			// old aggressively blurry safety path.
			rw::gx::texPoolSetSoftBudget(GxCutsceneLoadTexBudget());
			rw::gx::texPoolEnforceBudget("cutscene-anim");
		}
#endif
		CStreaming::MakeSpaceFor(size << 11);
		CStreaming::ImGonnaUseStreamingMemory();
		RwStreamSkip(stream,  offset << 11);
		CAnimManager::LoadAnimFile(stream, true, uncompressedAnims);
		ms_cutsceneAssociations.CreateAssociations(szCutsceneName);
		CStreaming::IHaveUsedStreamingMemory();
		ms_animLoaded = true;
	} else {
		ms_animLoaded = false;
	}
	RwStreamClose(stream, nil);

	// Load camera data
	file = CFileMgr::OpenFile("ANIM\\CUTS.IMG", "rb");
	sprintf(gString, "%s.DAT", szCutsceneName);
	if (ms_pCutsceneDir->FindItem(gString, offset, size)) {
		CStreaming::ImGonnaUseStreamingMemory();
		CFileMgr::Seek(file, offset << 11, SEEK_SET);
		TheCamera.LoadPathSplines(file);
		CStreaming::IHaveUsedStreamingMemory();
		bCamLoaded = true;
	} else {
		bCamLoaded = false;
	}

	CFileMgr::CloseFile(file);

	if (CGeneral::faststricmp(ms_cutsceneName, "finale")) {
		DMAudio.ChangeMusicMode(MUSICMODE_CUTSCENE);
		int trackId = FindCutsceneAudioTrackId(szCutsceneName);
		if (trackId != -1)
			DMAudio.PreloadCutSceneMusic(trackId);
	}

	ms_cutsceneTimer = 0.0f;
	ms_loaded = true;
	ms_cutsceneOffset = CVector(0.0f, 0.0f, 0.0f);

	pPlayerPed = FindPlayerPed();
	pPlayerPed->m_pWanted->ClearQdCrimes();
	pPlayerPed->bIsVisible = false;
	pPlayerPed->m_fCurrentStamina = pPlayerPed->m_fMaxStamina;
	CPad::GetPad(0)->SetDisablePlayerControls(PLAYERCONTROL_CUTSCENE);
	CWorld::Players[CWorld::PlayerInFocus].MakePlayerSafe(true);

#ifdef WII
	printf("[CUT-WII] data_loaded name=%s anim=%d cam=%d loaded=%d processing=%d running=%d intro=%d\n",
		ms_cutsceneName,
		ms_animLoaded ? 1 : 0,
		bCamLoaded ? 1 : 0,
		ms_loaded ? 1 : 0,
		ms_cutsceneProcessing ? 1 : 0,
		ms_running ? 1 : 0,
		CGame::playingIntro ? 1 : 0);
#endif
	CTimer::Resume();
}

void
CCutsceneMgr::FinishCutscene()
{
	ms_wasCutsceneSkipped = true;
#ifdef WII
	printf("[CUT-WII] finish_cutscene name=%s camLoaded=%d timer=%.3f status=%u running=%d\n",
		ms_cutsceneName,
		bCamLoaded ? 1 : 0,
		ms_cutsceneTimer,
		ms_cutsceneLoadStatus,
		ms_running ? 1 : 0);
#endif
	if (bCamLoaded) {
		CCutsceneMgr::ms_cutsceneTimer = TheCamera.GetCutSceneFinishTime() * 0.001f;
		TheCamera.FinishCutscene();
	}

	FindPlayerPed()->bIsVisible = true;
	CWorld::Players[CWorld::PlayerInFocus].MakePlayerSafe(false);
}

void
CCutsceneMgr::SetupCutsceneToStart(void)
{
	((void)0);

#ifdef WII
	printf("[CUT-WII] setup_to_start name=%s camLoaded=%d objs=%d status=%u running=%d\n",
		ms_cutsceneName,
		bCamLoaded ? 1 : 0,
		ms_numCutsceneObjs,
		ms_cutsceneLoadStatus,
		ms_running ? 1 : 0);
#endif
	if (bCamLoaded) {
		TheCamera.SetCamCutSceneOffSet(ms_cutsceneOffset);
		TheCamera.TakeControlWithSpline(JUMP_CUT);
		TheCamera.SetWideScreenOn();
	}

	ms_cutsceneOffset.z++;

	for (int i = ms_numCutsceneObjs - 1; i >= 0; i--) {
		CCutsceneObject *obj = ms_pCutsceneObjects[i];
		if (obj == nil || obj->m_rwObject == nil || RwObjectGetType(obj->m_rwObject) != rpCLUMP) {
			printf("[CUTSCENE] skipping invalid object at start idx=%d obj=%p rw=%p\n",
			       i, obj, obj ? obj->m_rwObject : nil);
			continue;
		}
		if (CAnimBlendAssociation *pAnimBlendAssoc = RpAnimBlendClumpGetFirstAssociation((RpClump*)obj->m_rwObject)) {
			assert(pAnimBlendAssoc->hierarchy->sequences[0].HasTranslation());
			if (obj->m_pAttachTo != nil) {
				pAnimBlendAssoc->flags &= (~ASSOC_HAS_TRANSLATION);
			} else {
				if (pAnimBlendAssoc->hierarchy->IsCompressed()){
					KeyFrameTransCompressed *keyFrames = ((KeyFrameTransCompressed*)pAnimBlendAssoc->hierarchy->sequences[0].GetKeyFrameCompressed(0));
					CVector trans;
					keyFrames->GetTranslation(&trans);
					obj->SetPosition(ms_cutsceneOffset + trans);
				}else{
					KeyFrameTrans *keyFrames = ((KeyFrameTrans*)pAnimBlendAssoc->hierarchy->sequences[0].GetKeyFrame(0));
					obj->SetPosition(ms_cutsceneOffset + keyFrames->translation);
				}
			}
			pAnimBlendAssoc->SetRun();
		} else {
			obj->SetPosition(ms_cutsceneOffset);
		}
		CWorld::Add(obj);
		obj->UpdateRpHAnim();
	}

	// On Wii we already entered this path from a freshly-updated gameplay frame.
	// Advancing the timer twice here causes the first cutscene frame to jump,
	// which makes intro transitions and early animation cadence look jerky
	// despite rendering at 60 Hz.
	CTimer::Update();
	ms_running = true;
	ms_cutsceneTimer = 0.0f;
	((void)0);
}

void
CCutsceneMgr::SetCutsceneAnim(const char *animName, CObject *pObject)
{
	CAnimBlendAssociation *pNewAnim;
	CAnimBlendClumpData *pAnimBlendClumpData;

	if (pObject == nil || pObject->m_rwObject == nil || RwObjectGetType(pObject->m_rwObject) != rpCLUMP) {
		printf("[CUTSCENE] SetCutsceneAnim skipped invalid object anim=%s obj=%p rw=%p\n",
		       animName, pObject, pObject ? pObject->m_rwObject : nil);
		return;
	}
	debug("Give cutscene anim %s\n", animName);
	RpAnimBlendClumpRemoveAllAssociations((RpClump*)pObject->m_rwObject);

	pNewAnim = ms_cutsceneAssociations.GetAnimation(animName);
	if (!pNewAnim) {
		debug("\n\nHaven't I told you I can't find the fucking animation %s\n\n\n", animName);
		return;
	}

	if (pNewAnim->hierarchy->IsCompressed())
		pNewAnim->hierarchy->keepCompressed = true;

	CStreaming::ImGonnaUseStreamingMemory();
	pNewAnim = ms_cutsceneAssociations.CopyAnimation(animName);
	CStreaming::IHaveUsedStreamingMemory();
	if (pNewAnim == nil) {
		printf("[CUTSCENE] CopyAnimation failed for %s\n", animName);
		return;
	}

	pNewAnim->SetCurrentTime(0.0f);
	pNewAnim->flags |= ASSOC_HAS_TRANSLATION;
	pNewAnim->flags &= ~ASSOC_RUNNING;

	pAnimBlendClumpData = *RPANIMBLENDCLUMPDATA(pObject->m_rwObject);
	if (pAnimBlendClumpData == nil || pAnimBlendClumpData->frames == nil || pAnimBlendClumpData->numFrames <= 0) {
		printf("[CUTSCENE] missing clump anim data for %s obj=%p rw=%p data=%p\n",
		       animName, pObject, pObject->m_rwObject, pAnimBlendClumpData);
		delete pNewAnim;
		return;
	}
	pAnimBlendClumpData->link.Prepend(&pNewAnim->link);

	if (pNewAnim->hierarchy->keepCompressed)
		pAnimBlendClumpData->frames->flag |= AnimBlendFrameData::COMPRESSED;
}

void
CCutsceneMgr::SetCutsceneAnimToLoop(const char* animName)
{
	ms_cutsceneAssociations.GetAnimation(animName)->flags |= ASSOC_REPEAT;
}

CCutsceneHead *
CCutsceneMgr::AddCutsceneHead(CObject *pObject, int modelId)
{
	return nil;
}

void UpdateCutsceneObjectBoundingBox(RpClump* clump, int modelId)
{
	if (clump == nil)
		return;
	if (modelId >= MI_CUTOBJ01 && modelId <= MI_CUTOBJ05) {
		CColModel* pColModel = &CTempColModels::ms_colModelCutObj[modelId - MI_CUTOBJ01];
		float radius = 0.0f;
		RpClumpForAllAtomics(clump, CalculateBoundingSphereRadiusCB, &radius);
		pColModel->boundingSphere.radius = radius;
		pColModel->boundingBox.min = CVector(-radius, -radius, -radius);
		pColModel->boundingBox.max = CVector(radius, radius, radius);
	}
}

CCutsceneObject *
CCutsceneMgr::CreateCutsceneObject(int modelId)
{
	CBaseModelInfo *pModelInfo;
	CColModel *pColModel;
	CCutsceneObject *pCutsceneObject;

	CStreaming::ImGonnaUseStreamingMemory();
	debug("Created cutscene object %s\n", CModelInfo::GetModelInfo(modelId)->GetModelName());
	if (modelId >= MI_CUTOBJ01 && modelId <= MI_CUTOBJ05) {
		pModelInfo = CModelInfo::GetModelInfo(modelId);
		pColModel = &CTempColModels::ms_colModelCutObj[modelId - MI_CUTOBJ01];
		pModelInfo->SetColModel(pColModel);
		if (pModelInfo->GetRwObject() && RwObjectGetType(pModelInfo->GetRwObject()) == rpCLUMP)
			UpdateCutsceneObjectBoundingBox((RpClump*)pModelInfo->GetRwObject(), modelId);
	} else if (modelId >= MI_SPECIAL01 && modelId <= MI_SPECIAL21) {
		pModelInfo = CModelInfo::GetModelInfo(modelId);
		if (pModelInfo->GetColModel() == &CTempColModels::ms_colModelPed1) {
			CColModel *colModel = new CColModel();
			if (colModel == nil) {
				printf("[CUTSCENE] col model alloc failed for special model %d\n", modelId);
				CStreaming::IHaveUsedStreamingMemory();
				return nil;
			}
			colModel->boundingSphere.radius = 2.0f;
			colModel->boundingSphere.center = CVector(0.0f, 0.0f, 0.0f);
			pModelInfo->SetColModel(colModel, true);
		}
		pColModel = pModelInfo->GetColModel();
		float radius = 2.0f;
		pColModel->boundingSphere.radius = radius;
		pColModel->boundingBox.min = CVector(-radius, -radius, -radius);
		pColModel->boundingBox.max = CVector(radius, radius, radius);
	}

	pCutsceneObject = new CCutsceneObject();
	if (pCutsceneObject == nil) {
		printf("[CUTSCENE] CreateCutsceneObject alloc failed for model %d\n", modelId);
		CStreaming::IHaveUsedStreamingMemory();
		return nil;
	}
	pCutsceneObject->SetModelIndex(modelId);
	if (pCutsceneObject->m_rwObject == nil) {
		printf("[CUTSCENE] CreateCutsceneObject rw init failed for model %d\n", modelId);
		delete pCutsceneObject;
		CStreaming::IHaveUsedStreamingMemory();
		return nil;
	}
	if (ms_useCutsceneShadows)
		pCutsceneObject->CreateShadow();
	ms_pCutsceneObjects[ms_numCutsceneObjs++] = pCutsceneObject;
	CStreaming::IHaveUsedStreamingMemory();
	return pCutsceneObject;
}

void
CCutsceneMgr::DeleteCutsceneData(void)
{
	if (!ms_loaded) return;
#ifdef WII
	printf("[CUT-WII] delete_cutscene name=%s loaded=%d processing=%d running=%d status=%u objs=%d\n",
		ms_cutsceneName,
		ms_loaded ? 1 : 0,
		ms_cutsceneProcessing ? 1 : 0,
		ms_running ? 1 : 0,
		ms_cutsceneLoadStatus,
		ms_numCutsceneObjs);
#endif
	CTimer::Suspend();
#if GX_CONSOLE
	const bool wasIntroCutscene = IsIntroCutsceneName(ms_cutsceneName);
#endif

	ms_cutsceneProcessing = false;
	ms_useLodMultiplier = false;
	ms_useCutsceneShadows = true;

	for (--ms_numCutsceneObjs; ms_numCutsceneObjs >= 0; ms_numCutsceneObjs--) {
		if (ms_pCutsceneObjects[ms_numCutsceneObjs]) {
			CWorld::Remove(ms_pCutsceneObjects[ms_numCutsceneObjs]);
			ms_pCutsceneObjects[ms_numCutsceneObjs]->DeleteRwObject();
			delete ms_pCutsceneObjects[ms_numCutsceneObjs];
		}
		ms_pCutsceneObjects[ms_numCutsceneObjs] = nil;
	}
	ms_numCutsceneObjs = 0;

	for (int i = MI_SPECIAL01; i < MI_SPECIAL21; i++) {
		CBaseModelInfo *minfo = CModelInfo::GetModelInfo(i);
		CColModel *colModel = minfo->GetColModel();
		if (colModel != &CTempColModels::ms_colModelPed1) {
			delete colModel;
			minfo->SetColModel(&CTempColModels::ms_colModelPed1);
		}
	}

	if (ms_animLoaded)
		CAnimManager::RemoveLastAnimFile();

	ms_animLoaded = false;
	numUncompressedAnims = 0;
	uncompressedAnims[0][0] = '\0';

	if (bCamLoaded) {
		TheCamera.RestoreWithJumpCut();
		TheCamera.SetWideScreenOff();
		TheCamera.DeleteCutSceneCamDataMemory();
		bCamLoaded = false;
	}
	ms_running = false;
	ms_loaded = false;
	ms_cutsceneLoadStatus = 0;
	ms_cutsceneTimer = 0.0f;
	ms_cutsceneName[0] = '\0';

	FindPlayerPed()->bIsVisible = true;
	CPad::GetPad(0)->SetEnablePlayerControls(PLAYERCONTROL_CUTSCENE);
	CWorld::Players[CWorld::PlayerInFocus].MakePlayerSafe(false);

	if (CGeneral::faststricmp(ms_cutsceneName, "finale")) {
		DMAudio.StopCutSceneMusic();
		DMAudio.ChangeMusicMode(MUSICMODE_GAME);
	}
#if GX_CONSOLE
	if (wasIntroCutscene) {
		rw::gx::popCriticalUiUploadContext(ms_cutsceneName);
		CGame::playingIntro = false;
		// Keep intro cleanup on the conservative budget path so later
		// cutscene/gameplay rwMalloc calls are not starved by texture growth.
		rw::gx::texPoolSetSoftBudget(GxCutsceneLoadTexBudget());
		rw::gx::texPoolEnforceBudget("intro-cutscene-end");
	} else {
		// Keep post-cutscene gameplay near the full-quality Wii budget now that
		// the dedicated GX MEM2 pool is large enough to avoid the old 26MB cap.
		rw::gx::texPoolSetSoftBudget(GxPostCutsceneTexBudget());
		rw::gx::texPoolEnforceBudget("cutscene-end");
	}
#endif

	CStreaming::ms_disableStreaming = false;
	CWorld::bProcessCutsceneOnly = false;

	if(bCamLoaded)
		CGame::DrasticTidyUpMemory(TheCamera.GetScreenFadeStatus() == FADE_2);
	
	CPad::GetPad(0)->Clear(false);
	if (bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver) {
		CStreaming::LoadInitialPeds();
		CStreaming::LoadInitialWeapons();
		CStreaming::LoadInitialVehicles();
		bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver = false;
		
		CPlayerPed *pPlayerPed = FindPlayerPed();
		for (int i = 0; i < NumberOfSavedWeapons; i++) {
			int32 weaponModelId = CWeaponInfo::GetWeaponInfo(SavedWeaponIDs[i])->m_nModelId;
			uint8 flags = CStreaming::ms_aInfoForModel[weaponModelId].m_flags;
			CStreaming::RequestModel(weaponModelId, STREAMFLAGS_DONT_REMOVE);
			CStreaming::LoadAllRequestedModels(false);
			if (CWeaponInfo::GetWeaponInfo(SavedWeaponIDs[i])->m_nModel2Id != -1) {
				CStreaming::RequestModel(CWeaponInfo::GetWeaponInfo(SavedWeaponIDs[i])->m_nModel2Id, 0);
				CStreaming::LoadAllRequestedModels(false);
			}
			if (!(flags & STREAMFLAGS_DONT_REMOVE))
				CStreaming::SetModelIsDeletable(weaponModelId);
			pPlayerPed->GiveWeapon(SavedWeaponIDs[i], SavedWeaponAmmo[i], true);
		}
		NumberOfSavedWeapons = 0;
	}

	CTimer::Resume();
}

void
CCutsceneMgr::Update(void)
{
	enum {
		CUTSCENE_LOADING_0 = 0,
		CUTSCENE_LOADING_AUDIO,
		CUTSCENE_LOADING_2,
		CUTSCENE_LOADING_3,
		CUTSCENE_LOADING_4
	};

#ifdef WII
	static uint32 s_lastLoggedLoadStatus = 0xFFFFFFFFu;
	static bool s_lastLoggedRunning = false;
	static bool s_lastLoggedProcessing = false;
	if (s_lastLoggedLoadStatus != ms_cutsceneLoadStatus ||
	    s_lastLoggedRunning != ms_running ||
	    s_lastLoggedProcessing != ms_cutsceneProcessing) {
		printf("[CUT-WII] update_state name=%s status=%u running=%d processing=%d loaded=%d camLoaded=%d timer=%.3f\n",
			ms_cutsceneName,
			ms_cutsceneLoadStatus,
			ms_running ? 1 : 0,
			ms_cutsceneProcessing ? 1 : 0,
			ms_loaded ? 1 : 0,
			bCamLoaded ? 1 : 0,
			ms_cutsceneTimer);
		s_lastLoggedLoadStatus = ms_cutsceneLoadStatus;
		s_lastLoggedRunning = ms_running;
		s_lastLoggedProcessing = ms_cutsceneProcessing;
	}
#endif

	switch (ms_cutsceneLoadStatus) {
	case CUTSCENE_LOADING_AUDIO:
		SetupCutsceneToStart();
		if (CGeneral::faststricmp(ms_cutsceneName, "finale"))
			DMAudio.PlayPreloadedCutSceneMusic();
		ms_cutsceneLoadStatus++;
		break;
	case CUTSCENE_LOADING_2:
	case CUTSCENE_LOADING_3:
		ms_cutsceneLoadStatus++;
		break;
	case CUTSCENE_LOADING_4:
		ms_cutsceneLoadStatus = CUTSCENE_LOADING_0;
		break;
	default:
		break;
	}

	if (!ms_running) return;

	ms_cutsceneTimer += CTimer::GetTimeStepNonClippedInSeconds();

	for (int i = 0; i < ms_numCutsceneObjs; i++) {
		CCutsceneObject *obj = ms_pCutsceneObjects[i];
		if (obj == nil || obj->m_rwObject == nil || RwObjectGetType(obj->m_rwObject) != rpCLUMP)
			continue;
		int modelId = obj->GetModelIndex();
		if (modelId >= MI_CUTOBJ01 && modelId <= MI_CUTOBJ05)
			UpdateCutsceneObjectBoundingBox(obj->GetClump(), modelId);

		if (obj->m_pAttachTo != nil && modelId >= MI_SPECIAL01 && modelId <= MI_SPECIAL21)
			UpdateCutsceneObjectBoundingBox(obj->GetClump(), modelId);
	}

	if (bCamLoaded)
		if (CGeneral::faststricmp(ms_cutsceneName, "finale") && TheCamera.Cams[TheCamera.ActiveCam].Mode == CCam::MODE_FLYBY && ms_cutsceneLoadStatus == CUTSCENE_LOADING_0) {
			if (CPad::GetPad(0)->GetCrossJustDown()
				|| (CGame::playingIntro && CPad::GetPad(0)->GetStartJustDown())
#ifdef GTA_PC_CONTROLS
				|| CPad::GetPad(0)->GetLeftMouseJustDown()
				|| CPad::GetPad(0)->GetEnterJustDown()
				|| CPad::GetPad(0)->GetCharJustDown(' ')
#endif
			)
					FinishCutscene();
		}
}

bool CCutsceneMgr::HasCutsceneFinished(void) { return !bCamLoaded || TheCamera.GetPositionAlongSpline() == 1.0f; }

void
CCutsceneMgr::LoadAnimationUncompressed(char const* name)
{
	strcpy(uncompressedAnims[numUncompressedAnims], name);
	
	// Because that's how CAnimManager knows the end of array
	++numUncompressedAnims;
	assert(numUncompressedAnims < ARRAY_SIZE(uncompressedAnims));
	uncompressedAnims[numUncompressedAnims][0] = '\0';
}

void
CCutsceneMgr::AttachObjectToParent(CObject *pObject, CEntity *pAttachTo)
{
	if (pObject == nil || pAttachTo == nil || pAttachTo->m_rwObject == nil || RwObjectGetType(pAttachTo->m_rwObject) != rpCLUMP) {
		if (pObject)
			((CCutsceneObject*)pObject)->m_pAttachTo = nil;
		return;
	}
	((CCutsceneObject*)pObject)->m_pAttachmentObject = nil;
	((CCutsceneObject*)pObject)->m_pAttachTo = RpClumpGetFrame(pAttachTo->GetClump());

	debug("Attach %s to %s\n", CModelInfo::GetModelInfo(pObject->GetModelIndex())->GetModelName(), CModelInfo::GetModelInfo(pAttachTo->GetModelIndex())->GetModelName());
}

void
CCutsceneMgr::AttachObjectToFrame(CObject *pObject, CEntity *pAttachTo, const char *frame)
{
	if (pObject == nil || pAttachTo == nil || pAttachTo->m_rwObject == nil || RwObjectGetType(pAttachTo->m_rwObject) != rpCLUMP) {
		if (pObject)
			((CCutsceneObject*)pObject)->m_pAttachTo = nil;
		return;
	}
	const char *objectName = CModelInfo::GetModelInfo(pObject->GetModelIndex())->GetModelName();
	const char *parentName = CModelInfo::GetModelInfo(pAttachTo->GetModelIndex())->GetModelName();
	AnimBlendFrameData *frameData;

	((void)0);

	((CCutsceneObject*)pObject)->m_pAttachmentObject = nil;
	frameData = RpAnimBlendClumpFindFrame(pAttachTo->GetClump(), frame);
	if (frameData == nil || frameData->frame == nil) {
		((void)0);
		((CCutsceneObject*)pObject)->m_pAttachTo = nil;
		return;
	}

	((CCutsceneObject*)pObject)->m_pAttachTo = frameData->frame;
	debug("Attach %s to component %s of %s\n", objectName, frame, parentName);
	if (RwObjectGetType(pObject->m_rwObject) == rpCLUMP) {
		RpClump *clump = (RpClump*)pObject->m_rwObject;
		((void)0);
		if (IsClumpSkinned(clump))
			RpAtomicGetBoundingSphere(GetFirstAtomic(clump))->radius *= 1.1f;
	}
	((void)0);
}

void
CCutsceneMgr::AttachObjectToBone(CObject *pObject, CObject *pAttachTo, int bone)
{
	if (pObject == nil || pAttachTo == nil || pAttachTo->m_rwObject == nil || RwObjectGetType(pAttachTo->m_rwObject) != rpCLUMP) {
		if (pObject)
			((CCutsceneObject*)pObject)->m_pAttachTo = nil;
		return;
	}
	RpHAnimHierarchy *hanim = GetAnimHierarchyFromSkinClump(pAttachTo->GetClump());
	if (hanim == nil) {
		((CCutsceneObject*)pObject)->m_pAttachTo = nil;
		return;
	}
	RwInt32 id = RpHAnimIDGetIndex(hanim, bone);
	if (id < 0) {
		((CCutsceneObject*)pObject)->m_pAttachTo = nil;
		return;
	}
	RwMatrix *matrixArray = RpHAnimHierarchyGetMatrixArray(hanim);
	((CCutsceneObject*)pObject)->m_pAttachmentObject = pAttachTo;
	((CCutsceneObject*)pObject)->m_pAttachTo = &matrixArray[id];
	debug("Attach %s to %s\n",
		CModelInfo::GetModelInfo(pObject->GetModelIndex())->GetModelName(),
		CModelInfo::GetModelInfo(pAttachTo->GetModelIndex())->GetModelName());
}

void
CCutsceneMgr::RemoveEverythingFromTheWorldForTheBiggestFuckoffCutsceneEver()
{
	CStreaming::ms_disableStreaming = true;
	CColStore::RemoveAllCollision();
	CWorld::bProcessCutsceneOnly = true;
	ms_cutsceneProcessing = true;

	for (int i = CPools::GetPedPool()->GetSize() - 1; i >= 0; i--) {
		CPed *pPed = CPools::GetPedPool()->GetSlot(i);
		if (pPed) {
			if (!pPed->IsPlayer() && pPed->CanBeDeleted()) {
				CWorld::Remove(pPed);
				delete pPed;
			}
		}
	}

	for (int i = CPools::GetVehiclePool()->GetSize() - 1; i >= 0; i--) {
		CVehicle *pVehicle = CPools::GetVehiclePool()->GetSlot(i);
		if (pVehicle) {
			if (pVehicle->CanBeDeleted()) {
				CWorld::Remove(pVehicle);
				delete pVehicle;
			}
		}
	}

	bIsEverythingRemovedFromTheWorldForTheBiggestFuckoffCutsceneEver = true;
	CStreaming::RemoveCurrentZonesModels();
	CStreaming::SetModelIsDeletable(MI_MALE01);
	CStreaming::SetModelTxdIsDeletable(MI_MALE01);
	CStreaming::SetModelIsDeletable(MI_TAXI_D);
	CStreaming::SetModelTxdIsDeletable(MI_TAXI_D);
	CStreaming::SetModelIsDeletable(MI_NIGHTSTICK);
	CStreaming::SetModelTxdIsDeletable(MI_NIGHTSTICK);
	CStreaming::SetModelIsDeletable(MI_MISSILE);
	CStreaming::SetModelTxdIsDeletable(MI_MISSILE);
	CStreaming::SetModelIsDeletable(MI_POLICE);
	CStreaming::SetModelTxdIsDeletable(MI_POLICE);

	while (CStreaming::RemoveLoadedVehicle()) ;

	CRadar::RemoveRadarSections();

	for (int i = CPools::GetDummyPool()->GetSize() - 1; i >= 0; i--) {
		CDummy* pDummy = CPools::GetDummyPool()->GetSlot(i);
		if (pDummy)
			pDummy->DeleteRwObject();
	}

	for (int i = CPools::GetObjectPool()->GetSize() - 1; i >= 0; i--) {
		CObject* pObject = CPools::GetObjectPool()->GetSlot(i);
		if (pObject)
			pObject->DeleteRwObject();
	}

	for (int i = CPools::GetBuildingPool()->GetSize() - 1; i >= 0; i--) {
		CBuilding* pBuilding = CPools::GetBuildingPool()->GetSlot(i);
		if (pBuilding && pBuilding->m_rwObject != nil && pBuilding->bIsBIGBuilding && pBuilding->bStreamBIGBuilding) {
			if (pBuilding->bIsBIGBuilding)
				CStreaming::RequestModel(pBuilding->GetModelIndex(), 0);
			if (!pBuilding->bImBeingRendered)
				pBuilding->DeleteRwObject();
		}
	}

	CPlayerPed *pPlayerPed = FindPlayerPed();
	pPlayerPed->RemoveWeaponAnims(0, -1000.0f);
	NumberOfSavedWeapons = 0;

	for (int i = 0; i < TOTAL_WEAPON_SLOTS; i++) {
		if (pPlayerPed->m_weapons[i].m_eWeaponType != WEAPONTYPE_UNARMED) {
			SavedWeaponIDs[NumberOfSavedWeapons] = pPlayerPed->m_weapons[i].m_eWeaponType;
			SavedWeaponAmmo[NumberOfSavedWeapons] = pPlayerPed->m_weapons[i].m_nAmmoTotal;
			NumberOfSavedWeapons++;
		}
	}

	pPlayerPed->ClearWeapons();
	CGame::DrasticTidyUpMemory(true);
}
