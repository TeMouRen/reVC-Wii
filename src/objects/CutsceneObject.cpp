#include "common.h"

#include "main.h"
#include "RwHelper.h"
#include "Lights.h"
#include "PointLights.h"
#include "RpAnimBlend.h"
#include "AnimBlendClumpData.h"
#include "Bones.h"
#include "Renderer.h"
#include "ModelIndices.h"
#include "Shadows.h"
#include "Timecycle.h"
#include "CutsceneShadow.h"
#include "CutsceneObject.h"
#include "ModelIndices.h"
#include "RpAnimBlend.h"


CCutsceneObject::CCutsceneObject(void)
{
	SetStatus(STATUS_SIMPLE);
	bUsesCollision = false;
	bStreamingDontDelete = true;
	ObjectCreatedBy = CUTSCENE_OBJECT;
	m_fMass = 1.0f;
	m_fTurnMass = 1.0f;
	
	m_pAttachTo = nil;
	m_pAttachmentObject = nil;
	m_pShadow = nil;
}

CCutsceneObject::~CCutsceneObject(void)
{
	if ( m_pShadow )
	{
		delete m_pShadow;
		m_pShadow = nil;
	}
}

void
CCutsceneObject::SetModelIndex(uint32 id)
{
	CEntity::SetModelIndex(id);
	if(m_rwObject == nil || RwObjectGetType(m_rwObject) != rpCLUMP){
		printf("[CUTOBJ] SetModelIndex failed for model %u rw=%p\n", id, m_rwObject);
		m_rwObject = nil;
		return;
	}
	RpAnimBlendClumpInit((RpClump*)m_rwObject);
	CAnimBlendClumpData *clumpData = *RPANIMBLENDCLUMPDATA(m_rwObject);
	if(clumpData == nil || clumpData->frames == nil || clumpData->numFrames <= 0){
		printf("[CUTOBJ] Anim init failed for model %u rw=%p data=%p frames=%p num=%d\n",
		       id, m_rwObject, clumpData, clumpData ? clumpData->frames : nil, clumpData ? clumpData->numFrames : -1);
		DeleteRwObject();
		return;
	}
	clumpData->velocity3d = &m_vecMoveSpeed;
	clumpData->frames[0].flag |= AnimBlendFrameData::VELOCITY_EXTRACTION_3D;
}

void
CCutsceneObject::CreateShadow(void)
{
	if ( IsPedModel(GetModelIndex()) )
	{
#if GX_CONSOLE
		// GX still lacks a working cutscene shadow camera texture path.
		// Avoid creating 64x64 CAMERATEXTURE shadows that currently bind as invalid white blocks.
		return;
#endif
		m_pShadow = new CCutsceneShadow();
		if (!m_pShadow->IsInitialized())
			m_pShadow->Create(m_rwObject, 6, true, 4, true);
	}
}

void
CCutsceneObject::ProcessControl(void)
{
	CPhysical::ProcessControl();

	if ( m_pAttachTo )
	{
		if ( m_pAttachmentObject )
			GetMatrix() = CMatrix((RwMatrix*)m_pAttachTo);
		else
			GetMatrix() = CMatrix(RwFrameGetLTM((RwFrame*)m_pAttachTo));
	}
	else
	{
		if(CTimer::GetTimeStep() < 1/100.0f)
			m_vecMoveSpeed *= 100.0f;
		else
			m_vecMoveSpeed *= 1.0f/CTimer::GetTimeStep();
	
		ApplyMoveSpeed();
	}

}

static RpMaterial*
MaterialSetAlpha(RpMaterial *material, void *data)
{
	((RwRGBA*)RpMaterialGetColor(material))->alpha = (uint8)(uintptr)data;
	return material;
}

void
CCutsceneObject::PreRender(void)
{
	if ( m_pAttachTo )
	{
		if ( m_pAttachmentObject )
		{
			m_pAttachmentObject->UpdateRpHAnim();
			GetMatrix() = CMatrix((RwMatrix*)m_pAttachTo);
		}
		else
			GetMatrix() = CMatrix(RwFrameGetLTM((RwFrame*)m_pAttachTo));
		
		if ( RwObjectGetType(m_rwObject) == rpCLUMP && IsClumpSkinned(GetClump()) )
		{
			RpAtomic *atomic = GetFirstAtomic(GetClump());
			atomic->boundingSphere.center = (*RPANIMBLENDCLUMPDATA(GetClump()))->frames[0].hanimFrame->t;
		}
	}
	
	if ( RwObjectGetType(m_rwObject) == rpCLUMP )
		UpdateRpHAnim();
	
	if(IsPedModel(GetModelIndex()))
	{
		if ( m_pShadow == nil )
		{
			CShadows::StoreShadowForPedObject(this,
				CTimeCycle::m_fShadowDisplacementX[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowDisplacementY[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowFrontX[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowFrontY[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowSideX[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowSideY[CTimeCycle::m_CurrentStoredValue]);
		}
		else
		{
			if ( m_pShadow->IsInitialized() )
				m_pShadow->UpdateForCutscene();
			
			CShadows::StoreShadowForCutscenePedObject(this,
				CTimeCycle::m_fShadowDisplacementX[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowDisplacementY[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowFrontX[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowFrontY[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowSideX[CTimeCycle::m_CurrentStoredValue],
				CTimeCycle::m_fShadowSideY[CTimeCycle::m_CurrentStoredValue]);
		}
			
		// For some reason xbox/android limbs are transparent here...
		RpGeometry *geometry = RpAtomicGetGeometry(GetFirstAtomic(GetClump()));
		RpGeometrySetFlags(geometry, RpGeometryGetFlags(geometry) | rpGEOMETRYMODULATEMATERIALCOLOR);
		RpGeometryForAllMaterials(geometry, MaterialSetAlpha, (void*)255);
	}
}

void
CCutsceneObject::Render(void)
{
	SetCullMode(rwCULLMODECULLNONE);
	CObject::Render();
	SetCullMode(rwCULLMODECULLBACK);
}

bool
CCutsceneObject::SetupLighting(void)
{
	ActivateDirectional();
	SetAmbientColoursForPedsCarsAndObjects();

	if(bRenderScorched){
		WorldReplaceNormalLightsWithScorched(Scene.world, 0.1f);
	}else{
		CVector coors = GetPosition();
		float lighting = CPointLights::GenerateLightsAffectingObject(&coors);
		if(lighting != 1.0f){
			SetAmbientAndDirectionalColours(lighting);
			return true;
		}
	}

	return false;
}

void
CCutsceneObject::RemoveLighting(bool reset)
{
	CRenderer::RemoveVehiclePedLights(this, reset);
}
