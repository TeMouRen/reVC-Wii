#include "common.h"

#include "CarCtrl.h"

#include "Accident.h"
#include "Automobile.h"
#include "Bike.h"
#include "Camera.h"
#include "CarAI.h"
#include "CarGen.h"
#include "Cranes.h"
#include "Curves.h"
#include "CutsceneMgr.h"
#include "Gangs.h"
#include "Game.h"
#include "Garages.h"
#include "General.h"
#include "IniFile.h"
#include "ModelIndices.h"
#include "PathFind.h"
#include "Ped.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Population.h"
#include "Wanted.h"
#include "Pools.h"
#include "Renderer.h"
#include "RoadBlocks.h"
#include "Timer.h"
#include "TrafficLights.h"
#include "Streaming.h"
#include "VisibilityPlugins.h"
#include "Vehicle.h"
#include "Fire.h"
#include "WaterLevel.h"
#include "World.h"
#include "Zones.h"
#include "Pickups.h"

#define DISTANCE_TO_SPAWN_ROADBLOCK_PEDS (51.0f)
#define DISTANCE_TO_SCAN_FOR_DANGER (14.0f)
#define DISTANCE_TO_SCAN_FOR_PED_DANGER (11.0f)
#define SAFE_DISTANCE_TO_PED (3.0f)
#define INFINITE_Z (1000000000.0f)

#define VEHICLE_HEIGHT_DIFF_TO_CONSIDER_WEAVING (4.0f)
#define PED_HEIGHT_DIFF_TO_CONSIDER_WEAVING (4.0f)
#define OBJECT_HEIGHT_DIFF_TO_CONSIDER_WEAVING (8.0f)
#define WIDTH_COEF_TO_WEAVE_SAFELY (1.2f)
#define OBJECT_WIDTH_TO_WEAVE (0.3f)
#define PED_WIDTH_TO_WEAVE (0.8f)

#if REAL_GAMECUBE
static bool gLoggedBadTrafficScan;
static bool gLoggedBadPedDangerScan;
static bool gLoggedBadWeaveScan;

struct GcSavedIntroRouteState
{
	int32 currentRouteNode;
	int32 nextRouteNode;
	int32 prevRouteNode;
	uint32 currentPathNodeInfo;
	uint32 nextPathNodeInfo;
	uint32 previousPathNodeInfo;
	int32 timeEnteredCurve;
	int32 timeToSpendOnCurrentCurve;
	int8 previousDirection;
	int8 currentDirection;
	int8 nextDirection;
	int8 currentLane;
	int8 nextLane;
	int16 pathFindNodesCount;
};

struct GcCommonFollowPathTargetInfo
{
	CVector2D currentLanePos;
	CVector2D nextLanePos;
	CVector2D commonTarget;
	CVector2D gcCommonTarget;
	float distToNode;
	float spanBetweenNodes;
	float dp;
	float facingDp;
	int switchReason;
	bool gcAdjusted;
};

static CVector2D
GcGetPathLinkLanePosition(CCarPathLink *pLink, int8 lane, int8 direction);

static bool
GcIsScriptedIntroAdmiral(CVehicle *pVehicle)
{
	if (pVehicle == nil || pVehicle->GetModelIndex() != MI_ADMIRAL)
		return false;
	if (pVehicle->VehicleCreatedBy != MISSION_VEHICLE)
		return false;
	if (pVehicle->GetStatus() == STATUS_PLAYER ||
		pVehicle->GetStatus() == STATUS_PLAYER_REMOTE ||
		pVehicle->GetStatus() == STATUS_PLAYER_DISABLED ||
		pVehicle->GetStatus() == STATUS_WRECKED)
		return false;

	if (CGame::playingIntro || CCutsceneMgr::IsRunning() || CCutsceneMgr::ms_cutsceneLoadStatus != 0)
		return true;

	switch (pVehicle->AutoPilot.m_nCarMission) {
	case MISSION_NONE:
	case MISSION_WAITFORDELETION:
	case MISSION_STOP_FOREVER:
		return false;
	default:
		return true;
	}
}

static bool
GcShouldIgnoreScriptedIntroObjectForAI(CVehicle *pVehicle, CEntity *entity)
{
	return GcIsScriptedIntroAdmiral(pVehicle) &&
		entity != nil && entity->IsObject() &&
		entity->GetModelIndex() == MI_FIRE_HYDRANT;
}

static int
GcGetEffectiveDrivingStyle(CVehicle *pVehicle)
{
	return pVehicle != nil ? pVehicle->AutoPilot.m_nDrivingStyle : DRIVINGSTYLE_STOP_FOR_CARS;
}

static float
GcGetScriptedIntroDistanceToFinal(CVehicle *pVehicle)
{
	if (pVehicle == nil)
		return 999999.9f;

	CVector2D distanceToFinal(
		pVehicle->AutoPilot.m_vecDestinationCoors.x - pVehicle->GetPosition().x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y - pVehicle->GetPosition().y);
	return distanceToFinal.Magnitude();
}

static bool
GcShouldBypassScriptedIntroTrafficAvoidance(CVehicle *pVehicle)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle))
		return false;

	switch (pVehicle->AutoPilot.m_nCarMission) {
	case MISSION_GOTOCOORDS:
	case MISSION_GOTOCOORDS_STRAIGHT:
	case MISSION_GOTOCOORDS_ACCURATE:
	case MISSION_GOTO_COORDS_STRAIGHT_ACCURATE:
		break;
	default:
		return false;
	}

	float distanceToFinal = GcGetScriptedIntroDistanceToFinal(pVehicle);
	// Keep traffic weaving alive through the ordinary road-follow section so the
	// Admiral still reacts to real blockers like the hotel-side Faggio. Once the
	// scripted route queue has emptied and we're steering from the custom
	// terminal carry/final-approach targets, however, the generic weave angle
	// starts fighting the script target and produces the current "preview nibble
	// then deeper turn" shape seen in the log.
	if (distanceToFinal < 5.5f)
		return true;
	if (distanceToFinal < 18.5f &&
	    pVehicle->AutoPilot.m_nPathFindNodesCount == 0 &&
	    pVehicle->AutoPilot.m_nCurrentRouteNode != pVehicle->AutoPilot.m_nNextRouteNode &&
	    pVehicle->AutoPilot.m_nCurrentPathNodeInfo != pVehicle->AutoPilot.m_nNextPathNodeInfo)
		return true;
	return false;
}

static bool
GcShouldTraceAdmiralState(CVehicle *pVehicle, const char *stage)
{
	static CVehicle *sLastVehicle;
	static uint32 sLastMission;
	static uint32 sLastTemp;
	static uint32 sLastCurrentRoute;
	static uint32 sLastNextRoute;
	static uint32 sLastCurrentPath;
	static uint32 sLastNextPath;
	static int8 sLastCurrentLane;
	static int8 sLastNextLane;

	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return false;

	bool forced = stage[0] == 'j' || stage[0] == 's';
	bool changed = pVehicle != sLastVehicle ||
		(uint32)pVehicle->AutoPilot.m_nCarMission != sLastMission ||
		(uint32)pVehicle->AutoPilot.m_nTempAction != sLastTemp ||
		(uint32)pVehicle->AutoPilot.m_nCurrentRouteNode != sLastCurrentRoute ||
		(uint32)pVehicle->AutoPilot.m_nNextRouteNode != sLastNextRoute ||
		(uint32)pVehicle->AutoPilot.m_nCurrentPathNodeInfo != sLastCurrentPath ||
		(uint32)pVehicle->AutoPilot.m_nNextPathNodeInfo != sLastNextPath ||
		pVehicle->AutoPilot.m_nCurrentLane != sLastCurrentLane ||
		pVehicle->AutoPilot.m_nNextLane != sLastNextLane;
	bool periodic = (CTimer::GetFrameCounter() % 30) == 0;

	if (!(forced || changed || periodic))
		return false;

	sLastVehicle = pVehicle;
	sLastMission = (uint32)pVehicle->AutoPilot.m_nCarMission;
	sLastTemp = (uint32)pVehicle->AutoPilot.m_nTempAction;
	sLastCurrentRoute = (uint32)pVehicle->AutoPilot.m_nCurrentRouteNode;
	sLastNextRoute = (uint32)pVehicle->AutoPilot.m_nNextRouteNode;
	sLastCurrentPath = (uint32)pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	sLastNextPath = (uint32)pVehicle->AutoPilot.m_nNextPathNodeInfo;
	sLastCurrentLane = pVehicle->AutoPilot.m_nCurrentLane;
	sLastNextLane = pVehicle->AutoPilot.m_nNextLane;
	return true;
}

static void
GcTraceAdmiralPath(CVehicle *pVehicle, const char *stage, float targetX = 0.0f, float targetY = 0.0f,
	float steer = 0.0f, float accel = 0.0f, float brake = 0.0f, bool handbrake = false)
{
	if (pVehicle == nil || pVehicle->GetModelIndex() != MI_ADMIRAL)
		return;
	if (!GcShouldTraceAdmiralState(pVehicle, stage))
		return;

	const CVector &pos = pVehicle->GetPosition();
	const CVector &move = pVehicle->GetMoveSpeed();
	const CVector &fwd = pVehicle->GetForward();
	const int effectiveStyle = GcGetEffectiveDrivingStyle(pVehicle);
	printf("[CARCTRL-STATE] stage=%s frame=%u status=%u mission=%u temp=%u style=%u effStyle=%u cruise=%u count=%d target=(%f,%f) dest=(%f,%f,%f) nodes=%d/%d/%d path=%u/%u/%u lanes=%d/%d pos=(%f,%f,%f) move=(%f,%f,%f) fwd=(%f,%f,%f) ctl=(%f,%f,%f,%d)\n",
		stage, CTimer::GetFrameCounter(),
		pVehicle->GetStatus(), (uint32)pVehicle->AutoPilot.m_nCarMission,
		(uint32)pVehicle->AutoPilot.m_nTempAction, (uint32)pVehicle->AutoPilot.m_nDrivingStyle,
		(uint32)effectiveStyle,
		(uint32)pVehicle->AutoPilot.m_nCruiseSpeed, (int)pVehicle->AutoPilot.m_nPathFindNodesCount, targetX, targetY,
		pVehicle->AutoPilot.m_vecDestinationCoors.x, pVehicle->AutoPilot.m_vecDestinationCoors.y,
		pVehicle->AutoPilot.m_vecDestinationCoors.z, pVehicle->AutoPilot.m_nPrevRouteNode,
		pVehicle->AutoPilot.m_nCurrentRouteNode, pVehicle->AutoPilot.m_nNextRouteNode,
		pVehicle->AutoPilot.m_nPreviousPathNodeInfo, pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
		pVehicle->AutoPilot.m_nNextPathNodeInfo, (int)pVehicle->AutoPilot.m_nCurrentLane,
		(int)pVehicle->AutoPilot.m_nNextLane, pos.x, pos.y, pos.z, move.x, move.y, move.z,
		fwd.x, fwd.y, fwd.z, steer, accel, brake, handbrake ? 1 : 0);
}

static void
GcTraceAdmiralPathDecision(CVehicle *pVehicle, const char *stage,
	float currentX, float currentY, float nextX, float nextY,
	float targetX, float targetY, float distToNode, float spanBetweenNodes,
	float dp, float facingDp, int switchReason, bool switchedNode,
	float angleForward, float angleTarget, float steerAngle, float maxAngle,
	float currentSpeed, float speedTarget,
	float speedStyleMultiplier, float speedAngleMultiplier, float speedNodesMultiplier,
	bool handbrake)
{
	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return;
	if (switchReason == 0 && !switchedNode && (CTimer::GetFrameCounter() % 30) != 0)
		return;

	const int effectiveStyle = GcGetEffectiveDrivingStyle(pVehicle);
	const bool wantsTrafficAvoidance = effectiveStyle == DRIVINGSTYLE_AVOID_CARS;
	const bool bypassTrafficAvoidance =
		wantsTrafficAvoidance && GcShouldBypassScriptedIntroTrafficAvoidance(pVehicle);
	const bool activeTrafficAvoidance = wantsTrafficAvoidance && !bypassTrafficAvoidance;

	printf("[CARCTRL-PATH] stage=%s frame=%u mission=%u temp=%u nodes=%d/%d path=%u/%u lanes=%d/%d cur=(%f,%f) next=(%f,%f) target=(%f,%f) dist=%f span=%f dp=%f face=%f sw=%d picked=%d angF=%f angT=%f steer=%f max=%f curSpd=%f tgtSpd=%f mul=(%f,%f,%f) avoid=%d bypass=%d hand=%d\n",
		stage, CTimer::GetFrameCounter(),
		(uint32)pVehicle->AutoPilot.m_nCarMission,
		(uint32)pVehicle->AutoPilot.m_nTempAction,
		pVehicle->AutoPilot.m_nCurrentRouteNode,
		pVehicle->AutoPilot.m_nNextRouteNode,
		(uint32)pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
		(uint32)pVehicle->AutoPilot.m_nNextPathNodeInfo,
		(int)pVehicle->AutoPilot.m_nCurrentLane,
		(int)pVehicle->AutoPilot.m_nNextLane,
		currentX, currentY, nextX, nextY, targetX, targetY,
		distToNode, spanBetweenNodes, dp, facingDp, switchReason, switchedNode ? 1 : 0,
		angleForward, angleTarget, steerAngle, maxAngle,
		currentSpeed, speedTarget,
		speedStyleMultiplier, speedAngleMultiplier, speedNodesMultiplier,
		activeTrafficAvoidance ? 1 : 0,
		bypassTrafficAvoidance ? 1 : 0,
		handbrake ? 1 : 0);
}

static void
GcTraceAdmiralHeadingDecision(CVehicle *pVehicle, float targetX, float targetY,
	float angleForward, float angleToTarget, float steerAngle, float maxAngle,
	float currentSpeed, float speedTarget, float speedMultiplier, bool handbrake)
{
	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return;
	if ((CTimer::GetFrameCounter() % 30) != 0 &&
	    !handbrake &&
	    ABS(steerAngle) < 0.15f)
		return;

	const int effectiveStyle = GcGetEffectiveDrivingStyle(pVehicle);
	const bool wantsTrafficAvoidance = effectiveStyle == DRIVINGSTYLE_AVOID_CARS;
	const bool bypassTrafficAvoidance =
		wantsTrafficAvoidance && GcShouldBypassScriptedIntroTrafficAvoidance(pVehicle);
	const bool activeTrafficAvoidance = wantsTrafficAvoidance && !bypassTrafficAvoidance;

	printf("[CARCTRL-HEAD] frame=%u mission=%u temp=%u target=(%f,%f) angF=%f angT=%f steer=%f max=%f curSpd=%f tgtSpd=%f mul=%f avoid=%d bypass=%d hand=%d\n",
		CTimer::GetFrameCounter(),
		(uint32)pVehicle->AutoPilot.m_nCarMission,
		(uint32)pVehicle->AutoPilot.m_nTempAction,
		targetX, targetY, angleForward, angleToTarget, steerAngle, maxAngle,
		currentSpeed, speedTarget, speedMultiplier,
		activeTrafficAvoidance ? 1 : 0,
		bypassTrafficAvoidance ? 1 : 0,
		handbrake ? 1 : 0);
}

static void
GcTraceAdmiralPathSearch(CVehicle *pVehicle, const char *stage,
	int startNode, int rawCount, int finalCount,
	int currentRouteNode, int nextRouteNode,
	uint32 currentPathInfo, uint32 nextPathInfo,
	int firstNode, int secondNode)
{
	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return;

	printf("[CARCTRL-SEARCH] stage=%s frame=%u start=%d raw=%d final=%d curRoute=%d nextRoute=%d curPath=%u nextPath=%u first=%d second=%d pos=(%f,%f,%f) dest=(%f,%f,%f)\n",
		stage, CTimer::GetFrameCounter(),
		startNode, rawCount, finalCount,
		currentRouteNode, nextRouteNode,
		currentPathInfo, nextPathInfo,
		firstNode, secondNode,
		pVehicle->GetPosition().x, pVehicle->GetPosition().y, pVehicle->GetPosition().z,
		pVehicle->AutoPilot.m_vecDestinationCoors.x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y,
		pVehicle->AutoPilot.m_vecDestinationCoors.z);
}

static void
GcTraceAdmiralSwitchGate(CVehicle *pVehicle, const char *stage,
	float scalarDistanceToNextNode, float dp, float facingDp,
	bool switchNearNode, bool switchFacingNode,
	bool switchOvershoot, bool switchSameLink)
{
	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return;

	printf("[CARCTRL-SWITCH] stage=%s frame=%u curRoute=%d nextRoute=%d curPath=%u nextPath=%u dist=%f dp=%f face=%f near=%d facing=%d over=%d same=%d pos=(%f,%f,%f)\n",
		stage, CTimer::GetFrameCounter(),
		pVehicle->AutoPilot.m_nCurrentRouteNode,
		pVehicle->AutoPilot.m_nNextRouteNode,
		(uint32)pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
		(uint32)pVehicle->AutoPilot.m_nNextPathNodeInfo,
		scalarDistanceToNextNode, dp, facingDp,
		switchNearNode ? 1 : 0,
		switchFacingNode ? 1 : 0,
		switchOvershoot ? 1 : 0,
		switchSameLink ? 1 : 0,
		pVehicle->GetPosition().x, pVehicle->GetPosition().y, pVehicle->GetPosition().z);
}

static bool
GcTraceShouldLogNodeSequence(CVehicle *pVehicle)
{
	return pVehicle != nil && GcIsScriptedIntroAdmiral(pVehicle);
}

static void
GcTraceAdmiralNodeSequence(CVehicle *pVehicle, const char *stage, const CVector &searchPos,
	CPathNode **nodes, int count)
{
	if (!GcTraceShouldLogNodeSequence(pVehicle))
		return;

	int nodeIds[4] = { -1, -1, -1, -1 };
	float nodeX[2] = { 0.0f, 0.0f };
	float nodeY[2] = { 0.0f, 0.0f };
	float dist0 = -1.0f;
	float dist1 = -1.0f;
	float dot01 = 0.0f;
	int dropFirst = 0;

	for (int i = 0; i < 4 && i < count; i++) {
		if (nodes[i] != nil)
			nodeIds[i] = nodes[i] - ThePaths.m_pathNodes;
	}

	if (count > 0 && nodes[0] != nil) {
		nodeX[0] = nodes[0]->GetPosition().x;
		nodeY[0] = nodes[0]->GetPosition().y;
		dist0 = (nodes[0]->GetPosition() - searchPos).Magnitude2D();
	}
	if (count > 1 && nodes[0] != nil && nodes[1] != nil) {
		nodeX[1] = nodes[1]->GetPosition().x;
		nodeY[1] = nodes[1]->GetPosition().y;
		dist1 = (nodes[1]->GetPosition() - searchPos).Magnitude2D();
		dot01 = DotProduct2D(nodes[1]->GetPosition() - searchPos, nodes[0]->GetPosition() - searchPos);
		dropFirst = dot01 < 0.0f ? 1 : 0;
	}

	printf("[CARCTRL-NODES] stage=%s frame=%u count=%d nodes=%d/%d/%d/%d curRoute=%d nextRoute=%d curPath=%u nextPath=%u pos=(%f,%f) dest=(%f,%f) dot01=%f drop=%d dist=(%f,%f) p0=(%f,%f) p1=(%f,%f)\n",
		stage, CTimer::GetFrameCounter(), count,
		nodeIds[0], nodeIds[1], nodeIds[2], nodeIds[3],
		pVehicle->AutoPilot.m_nCurrentRouteNode,
		pVehicle->AutoPilot.m_nNextRouteNode,
		(uint32)pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
		(uint32)pVehicle->AutoPilot.m_nNextPathNodeInfo,
		searchPos.x, searchPos.y,
		pVehicle->AutoPilot.m_vecDestinationCoors.x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y,
		dot01, dropFirst, dist0, dist1,
		nodeX[0], nodeY[0], nodeX[1], nodeY[1]);
}

static void
GcTraceAdmiralLinkChoice(CVehicle *pVehicle, const char *stage,
	int curRouteNode, int nextRouteNode,
	int curLink, int nextLink,
	uint32 curConnection, uint32 nextConnection,
	float altLinkDist)
{
	if (!GcTraceShouldLogNodeSequence(pVehicle))
		return;

	printf("[CARCTRL-LINK] stage=%s frame=%u curRoute=%d nextRoute=%d curLink=%d nextLink=%d curConn=%u nextConn=%u curDir=%d nextDir=%d lanes=%d/%d altDist=%f pos=(%f,%f,%f) fwd=(%f,%f,%f)\n",
		stage, CTimer::GetFrameCounter(),
		curRouteNode, nextRouteNode,
		curLink, nextLink,
		curConnection, nextConnection,
		(int)pVehicle->AutoPilot.m_nCurrentDirection,
		(int)pVehicle->AutoPilot.m_nNextDirection,
		(int)pVehicle->AutoPilot.m_nCurrentLane,
		(int)pVehicle->AutoPilot.m_nNextLane,
		altLinkDist,
		pVehicle->GetPosition().x, pVehicle->GetPosition().y, pVehicle->GetPosition().z,
		pVehicle->GetForward().x, pVehicle->GetForward().y, pVehicle->GetForward().z);
}

static bool
GcShouldPreserveScriptedIntroSearchStartNode(CVehicle *pVehicle,
	const GcSavedIntroRouteState *savedState, CPathNode **nodes, int16 count)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle) || savedState == nil || nodes == nil)
		return false;
	if (count < 2 || nodes[0] == nil || nodes[1] == nil)
		return false;

	int firstNode = nodes[0] - ThePaths.m_pathNodes;
	if (firstNode != savedState->currentRouteNode &&
	    firstNode != savedState->nextRouteNode)
		return false;
	if (savedState->currentRouteNode != 784 &&
	    savedState->currentRouteNode != 783 &&
	    savedState->currentRouteNode != 782)
		return false;
	if (savedState->nextRouteNode != 783 &&
	    savedState->nextRouteNode != 782 &&
	    savedState->nextRouteNode != 781)
		return false;
	if (savedState->currentPathNodeInfo != 866 &&
	    savedState->currentPathNodeInfo != 868)
		return false;
	if (savedState->nextPathNodeInfo != 869 &&
	    savedState->nextPathNodeInfo != 867)
		return false;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[savedState->currentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[savedState->nextPathNodeInfo];
	CVector2D currentAnchor = GcGetPathLinkLanePosition(
		pCurrentLink, savedState->currentLane, savedState->currentDirection);
	CVector2D nextAnchor = GcGetPathLinkLanePosition(
		pNextLink, savedState->nextLane, savedState->nextDirection);
	CVector2D activeSegment = nextAnchor - currentAnchor;
	float activeSegmentLen = activeSegment.Magnitude();
	if (activeSegmentLen <= 0.001f)
		return false;

	CVector2D activeSegmentDir = activeSegment / activeSegmentLen;
	float alongActiveSegment = DotProduct2D(
		(CVector2D)pVehicle->GetPosition() - currentAnchor, activeSegmentDir);

	// Only protect the queued handoff node while the car is still plausibly on
	// the live lane segment. Once the vehicle has already carried well past the
	// segment end, forcing the old start node to stay in the fresh search result
	// makes the reroute restart from a node that is physically behind the car,
	// which is the direct cause of the intro Admiral's spin at the hotel turn.
	if (alongActiveSegment > activeSegmentLen + 5.0f)
		return false;

	// The generic start-node trim only looks at node-center positions. For the
	// intro Admiral that can drop the active route handoff node while the car is
	// still legitimately following the lane geometry into it, which skips an
	// entire road segment and produces the early hotel turn.
	return DotProduct2D(nodes[1]->GetPosition() - pVehicle->GetPosition(),
		nodes[0]->GetPosition() - pVehicle->GetPosition()) < 0.0f;
}

static void
GcRemoveBadStartNodeForScriptedIntro(CVehicle *pVehicle,
	const GcSavedIntroRouteState *savedState,
	CPathNode **nodes, int16 *count, const char *preserveStage)
{
	if (GcShouldPreserveScriptedIntroSearchStartNode(pVehicle, savedState, nodes, *count)) {
		GcTraceAdmiralNodeSequence(pVehicle, preserveStage,
			pVehicle->GetPosition(), nodes, *count);
		return;
	}
	ThePaths.RemoveBadStartNode(pVehicle->GetPosition(), nodes, count);
}

static bool
GcShouldTraceAdmiralGuide(CVehicle *pVehicle, const char *stage)
{
	static CVehicle *sLastVehicle;
	static const char *sLastStage;

	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return false;

	bool changed = pVehicle != sLastVehicle || stage != sLastStage;
	bool periodic = (CTimer::GetFrameCounter() % 15) == 0;
	if (!(changed || periodic))
		return false;

	sLastVehicle = pVehicle;
	sLastStage = stage;
	return true;
}

static void
GcTraceAdmiralGuideDecision(CVehicle *pVehicle, const char *stage,
	const CVector2D &currentAnchor, const CVector2D &nextAnchor,
	const CVector2D &finalTarget, const CVector2D &vehiclePos,
	const CVector2D &target, float currentCurvePos,
	float distToNextAnchor, float distanceToFinal,
	float firstAlong, float firstLen, float firstLateral,
	float secondAlong, float secondLen, float secondLateral)
{
	if (!GcShouldTraceAdmiralGuide(pVehicle, stage))
		return;

	printf("[CARCTRL-GUIDE] stage=%s frame=%u mission=%u temp=%u cur=(%f,%f) next=(%f,%f) final=(%f,%f) pos=(%f,%f) target=(%f,%f) curve=%f distNext=%f distFinal=%f first=(%f/%f/%f) second=(%f/%f/%f) lanes=%d/%d path=%u/%u\n",
		stage, CTimer::GetFrameCounter(),
		(uint32)pVehicle->AutoPilot.m_nCarMission,
		(uint32)pVehicle->AutoPilot.m_nTempAction,
		currentAnchor.x, currentAnchor.y,
		nextAnchor.x, nextAnchor.y,
		finalTarget.x, finalTarget.y,
		vehiclePos.x, vehiclePos.y,
		target.x, target.y,
		currentCurvePos, distToNextAnchor, distanceToFinal,
		firstAlong, firstLen, firstLateral,
		secondAlong, secondLen, secondLateral,
		(int)pVehicle->AutoPilot.m_nCurrentLane,
		(int)pVehicle->AutoPilot.m_nNextLane,
		(uint32)pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
		(uint32)pVehicle->AutoPilot.m_nNextPathNodeInfo);
}

static bool
GcShouldTraceAdmiralCompare(CVehicle *pVehicle, const char *stage)
{
	static CVehicle *sLastVehicle;
	static const char *sLastStage;

	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return false;

	bool changed = pVehicle != sLastVehicle || stage != sLastStage;
	bool periodic = (CTimer::GetFrameCounter() % 15) == 0;
	if (!(changed || periodic))
		return false;

	sLastVehicle = pVehicle;
	sLastStage = stage;
	return true;
}

static bool
GcBuildCommonFollowPathTargetInfo(CVehicle *pVehicle, GcCommonFollowPathTargetInfo *outInfo)
{
	if (pVehicle == nil || outInfo == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return false;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentPathLinkForward(
		pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection,
		pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection);
	float nextPathLinkForwardX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float nextPathLinkForwardY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;

	outInfo->currentLanePos = CVector2D(
		pCurrentLink->GetX() +
			((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.y,
		pCurrentLink->GetY() -
			((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.x);
	outInfo->nextLanePos = CVector2D(
		pNextLink->GetX() +
			((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() -
			((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX);

	CVector2D distanceToNextNode = (CVector2D)pVehicle->GetPosition() - outInfo->currentLanePos;
	outInfo->distToNode = distanceToNextNode.Magnitude();
	CVector2D distanceBetweenNodes = outInfo->nextLanePos - outInfo->currentLanePos;
	outInfo->dp = DotProduct2D(distanceBetweenNodes, distanceToNextNode);
	outInfo->spanBetweenNodes = distanceBetweenNodes.Magnitude();
	const float kDistSelectNew = 5.0f;
	const float kDistFacingSelectNew = 8.0f;
	const float kDistSlowdown = 40.0f;
	outInfo->facingDp = 0.0f;
	if (outInfo->distToNode > 0.0001f && outInfo->spanBetweenNodes > 0.0001f)
		outInfo->facingDp = outInfo->dp / (outInfo->distToNode * outInfo->spanBetweenNodes);

	bool switchNearNode = outInfo->distToNode < kDistSelectNew;
	bool switchFacingNode = outInfo->dp > 0.0f &&
		outInfo->distToNode < kDistFacingSelectNew;
	bool switchOvershoot = outInfo->facingDp > 0.7f;
	bool switchSameLink =
		pVehicle->AutoPilot.m_nNextPathNodeInfo == pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	outInfo->switchReason = (switchNearNode ? 1 : 0) |
		(switchFacingNode ? 2 : 0) |
		(switchOvershoot ? 4 : 0) |
		(switchSameLink ? 8 : 0);

	outInfo->commonTarget =
		outInfo->currentLanePos - currentPathLinkForward * outInfo->distToNode * 0.4f;
	outInfo->gcCommonTarget = outInfo->commonTarget;
	outInfo->gcAdjusted = false;

	if (GcIsScriptedIntroAdmiral(pVehicle) &&
	    outInfo->facingDp < 0.0f &&
	    outInfo->distToNode > kDistSelectNew) {
		outInfo->gcCommonTarget = outInfo->currentLanePos;
		outInfo->gcAdjusted = true;
	}

	if (outInfo->distToNode > kDistSlowdown) {
		outInfo->commonTarget = outInfo->currentLanePos;
		outInfo->gcCommonTarget = outInfo->currentLanePos;
		outInfo->gcAdjusted = false;
	}

	return true;
}

static void
GcTraceAdmiralTargetComparison(CVehicle *pVehicle, const char *stage, const CVector2D &customTarget)
{
	if (!GcShouldTraceAdmiralCompare(pVehicle, stage))
		return;

	GcCommonFollowPathTargetInfo commonInfo;
	if (!GcBuildCommonFollowPathTargetInfo(pVehicle, &commonInfo))
		return;

	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	CVector2D vehicleForward = pVehicle->GetForward();
	float forwardLen = vehicleForward.Magnitude();
	if (forwardLen > 0.0001f)
		vehicleForward /= forwardLen;
	else
		vehicleForward = CVector2D(1.0f, 0.0f);

	float angleForward = CGeneral::GetATanOfXY(vehicleForward.x, vehicleForward.y);
	CVector2D toCustom = customTarget - vehiclePos;
	CVector2D toCommon = commonInfo.commonTarget - vehiclePos;
	CVector2D toGcCommon = commonInfo.gcCommonTarget - vehiclePos;
	float customTurnAbs = 0.0f;
	float commonTurnAbs = 0.0f;
	float gcCommonTurnAbs = 0.0f;
	if (toCustom.Magnitude() > 0.001f)
		customTurnAbs = Abs(CCarCtrl::LimitRadianAngle(
			CGeneral::GetATanOfXY(toCustom.x, toCustom.y) - angleForward));
	if (toCommon.Magnitude() > 0.001f)
		commonTurnAbs = Abs(CCarCtrl::LimitRadianAngle(
			CGeneral::GetATanOfXY(toCommon.x, toCommon.y) - angleForward));
	if (toGcCommon.Magnitude() > 0.001f)
		gcCommonTurnAbs = Abs(CCarCtrl::LimitRadianAngle(
			CGeneral::GetATanOfXY(toGcCommon.x, toGcCommon.y) - angleForward));

	printf("[CARCTRL-CMP] stage=%s frame=%u mission=%u temp=%u pos=(%f,%f) custom=(%f,%f) common=(%f,%f) gcCommon=(%f,%f) cur=(%f,%f) next=(%f,%f) dist=%f span=%f dp=%f face=%f sw=%d gcAdj=%d turn=(%f/%f/%f) delta=(%f/%f)\n",
		stage, CTimer::GetFrameCounter(),
		(uint32)pVehicle->AutoPilot.m_nCarMission,
		(uint32)pVehicle->AutoPilot.m_nTempAction,
		vehiclePos.x, vehiclePos.y,
		customTarget.x, customTarget.y,
		commonInfo.commonTarget.x, commonInfo.commonTarget.y,
		commonInfo.gcCommonTarget.x, commonInfo.gcCommonTarget.y,
		commonInfo.currentLanePos.x, commonInfo.currentLanePos.y,
		commonInfo.nextLanePos.x, commonInfo.nextLanePos.y,
		commonInfo.distToNode, commonInfo.spanBetweenNodes,
		commonInfo.dp, commonInfo.facingDp, commonInfo.switchReason,
		commonInfo.gcAdjusted ? 1 : 0,
		customTurnAbs, commonTurnAbs, gcCommonTurnAbs,
		(customTarget - commonInfo.commonTarget).Magnitude(),
		(customTarget - commonInfo.gcCommonTarget).Magnitude());
}

static bool
GcCommitScriptedIntroGuideTarget(CVehicle *pVehicle, const CVector2D &candidate, CVector2D *outTarget)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle) || outTarget == nil)
		return false;

	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	CVector2D toTarget = candidate - vehiclePos;
	float toTargetLen = toTarget.Magnitude();
	if (toTargetLen < 0.25f)
		return false;

	CVector2D toFinal(
		pVehicle->AutoPilot.m_vecDestinationCoors.x - vehiclePos.x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y - vehiclePos.y);
	float toFinalLen = toFinal.Magnitude();
	if (toFinalLen > 1.0f && DotProduct2D(toTarget, toFinal) <= 0.0f)
		return false;

	CVector2D vehicleForward = pVehicle->GetForward();
	float forwardLen = vehicleForward.Magnitude();
	if (forwardLen > 0.0001f) {
		vehicleForward /= forwardLen;
		toTarget /= toTargetLen;
		if (toFinalLen > 2.0f && DotProduct2D(toTarget, vehicleForward) < -0.15f)
			return false;
	}

	*outTarget = candidate;
	return true;
}

static bool
GcGetScriptedIntroFinalApproachTarget(CVehicle *pVehicle, CVector2D currentNodePos, CVector2D *outTarget)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle) || outTarget == nil)
		return false;

	CVector2D finalTarget(pVehicle->AutoPilot.m_vecDestinationCoors.x, pVehicle->AutoPilot.m_vecDestinationCoors.y);
	CVector2D segment = finalTarget - currentNodePos;
	float segmentLen = segment.Magnitude();
	if (segmentLen < 0.001f)
		return false;

	CVector2D segmentDir = segment / segmentLen;
	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	CVector2D toVehicle = vehiclePos - currentNodePos;
	float along = DotProduct2D(toVehicle, segmentDir);
	along = Max(0.0f, along);

	CVector2D vehicleForward = pVehicle->GetForward();
	float vehicleForwardLen = vehicleForward.Magnitude();
	if (vehicleForwardLen > 0.0001f)
		vehicleForward /= vehicleForwardLen;
	else
		vehicleForward = segmentDir;

	CVector2D closestPoint = currentNodePos + segmentDir * Min(segmentLen, along);
	float lateralError = (vehiclePos - closestPoint).Magnitude();
	float alignment = DotProduct2D(vehicleForward, segmentDir);
	float distanceToFinal = (finalTarget - vehiclePos).Magnitude();
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	CVector2D laneRight(-segmentDir.y, segmentDir.x);
	float signedLateralError = DotProduct2D(vehiclePos - closestPoint, laneRight);

	// When the intro Admiral is close to the end of the segment, keep the guide
	// point near or slightly beyond the segment end instead of holding it a few
	// meters short. The old hold-back caused the car to arrive too fast with the
	// target still tucked inside the corner, which produced the sudden late turn.
	if (distanceToFinal < 12.0f &&
	    distanceToFinal > 0.75f &&
	    (currentSpeed > 14.0f || alignment < 0.985f || lateralError > 1.0f)) {
		float guideExtension = Min(4.0f,
			Max(1.5f, currentSpeed * 0.10f + Abs(signedLateralError) * 0.4f));
		float guideCap = segmentLen + guideExtension;
		float guideAdvance = Max(3.0f, Min(5.0f, currentSpeed * 0.20f));
		float guideLookAhead = Min(guideCap, along + guideAdvance);
		guideLookAhead = Max(guideLookAhead, Min(guideCap, segmentLen - 0.75f));
		*outTarget = currentNodePos + segmentDir * guideLookAhead;
		GcTraceAdmiralGuideDecision(pVehicle, "final-extend",
			currentNodePos, currentNodePos, finalTarget, vehiclePos, *outTarget,
			-1.0f, distanceToFinal, distanceToFinal,
			along, segmentLen, lateralError,
			0.0f, 0.0f, signedLateralError);
		return true;
	}

	float finalHoldBack = 0.0f;

	// Keep the scripted Admiral on a soft approach line instead of snapping directly
	// to the final point while the car is still yawed off the segment.
	if (distanceToFinal > 4.0f && (alignment < 0.98f || lateralError > 1.5f))
		finalHoldBack = Min(2.0f, Max(1.0f, lateralError * 0.5f));

	float maxLookAhead = segmentLen;
	if (finalHoldBack > 0.0f && segmentLen > finalHoldBack)
		maxLookAhead = segmentLen - finalHoldBack;

	float lookAhead = Min(maxLookAhead, along + 6.0f);
	lookAhead = Max(lookAhead, Min(maxLookAhead, along + 1.5f));
	*outTarget = currentNodePos + segmentDir * lookAhead;
	GcTraceAdmiralGuideDecision(pVehicle, "final-hold",
		currentNodePos, currentNodePos, finalTarget, vehiclePos, *outTarget,
		-1.0f, distanceToFinal, distanceToFinal,
		along, segmentLen, lateralError,
		0.0f, 0.0f, signedLateralError);
	return true;
}

static float
GcGetScriptedIntroTargetTurnAbs(CVehicle *pVehicle, const CVector2D &target)
{
	CVector2D vehicleForward = pVehicle->GetForward();
	float vehicleForwardLen = vehicleForward.Magnitude();
	if (vehicleForwardLen > 0.0001f)
		vehicleForward /= vehicleForwardLen;
	else
		vehicleForward = CVector2D(1.0f, 0.0f);

	CVector2D toTarget = target - (CVector2D)pVehicle->GetPosition();
	if (toTarget.Magnitude() <= 0.0001f)
		return 0.0f;

	float angleForward = CGeneral::GetATanOfXY(vehicleForward.x, vehicleForward.y);
	float angleTarget = CGeneral::GetATanOfXY(toTarget.x, toTarget.y);
	return Abs(CCarCtrl::LimitRadianAngle(angleTarget - angleForward));
}

static CVector2D
GcGetPathLinkLanePosition(CCarPathLink *pLink, int8 lane, int8 direction)
{
	CVector2D linkForward(pLink->GetDirX() * direction, pLink->GetDirY() * direction);
	return CVector2D(
		pLink->GetX() + ((lane + pLink->OneWayLaneOffset()) * LANE_WIDTH) * linkForward.y,
		pLink->GetY() - ((lane + pLink->OneWayLaneOffset()) * LANE_WIDTH) * linkForward.x);
}

static bool
GcHasMeaningfulScriptedIntroSecondSegment(const CVector2D &nextAnchor, const CVector2D &finalTarget,
	CVector2D *outDir, float *outLen)
{
	CVector2D secondSegment = finalTarget - nextAnchor;
	float secondLen = secondSegment.Magnitude();
	if (outLen != nil)
		*outLen = secondLen;
	if (secondLen < 1.5f)
		return false;
	if (outDir != nil)
		*outDir = secondSegment / secondLen;
	return true;
}

static CVector2D
GcBlendScriptedIntroSecondSegmentRecoveryTarget(CVehicle *pVehicle, const CVector2D &vehiclePos,
	const CVector2D &baseCandidate,
	const CVector2D &nextAnchor,
	const CVector2D &firstDir,
	const CVector2D &secondDir,
	float secondLen,
	float secondAlong,
	float secondLateral,
	float distanceToFinal,
	float currentSpeed)
{
	if (secondLen <= 0.001f || secondLateral <= 0.35f)
		return baseCandidate;

	float clampedAlong = Min(secondLen, Max(0.0f, secondAlong));
	CVector2D laneNormal(-secondDir.y, secondDir.x);
	CVector2D lineClosest = nextAnchor + secondDir * clampedAlong;
	float signedLateral = DotProduct2D(vehiclePos - lineClosest, laneNormal);

	float previewLead = Max(2.75f, Min(5.75f, 1.35f + currentSpeed * 0.22f));
	float previewAlong = Min(secondLen, Max(clampedAlong + 1.25f, clampedAlong + previewLead));
	if (distanceToFinal < 7.0f)
		previewAlong = Min(secondLen, Max(previewAlong,
			secondLen - Max(1.0f, Min(2.5f, distanceToFinal * 0.35f))));

	float turnCross = firstDir.x * secondDir.y - firstDir.y * secondDir.x;
	float desiredSignedLateral = 0.0f;
	if (Abs(turnCross) > 0.05f && distanceToFinal < 5.5f) {
		// Once the car is inside the hotel approach, recover toward the lane center.
		// Holding an outside offset here leaves the Admiral visibly too far left in
		// the narrow final gap between the parked cars.
		float settleBias = Max(0.0f, Min(1.0f, (5.5f - distanceToFinal) / 4.5f));
		float desiredInsideOffset = Max(0.0f, Min(0.28f, (5.5f - distanceToFinal) * 0.06f));
		desiredSignedLateral =
			(turnCross < 0.0f ? -1.0f : 1.0f) * desiredInsideOffset * settleBias;
	}
	CVector2D recoveryCandidate =
		nextAnchor + secondDir * previewAlong + laneNormal * desiredSignedLateral;
	float lateralError = signedLateral - desiredSignedLateral;
	float recoveryGain = Max(0.10f, Min(0.18f, 0.06f + currentSpeed * 0.005f));
	float maxRecovery = Max(0.10f, Min(0.32f, 0.08f + distanceToFinal * 0.03f));
	float recoveryShift = Max(-maxRecovery, Min(maxRecovery, lateralError * recoveryGain));
	recoveryCandidate -= laneNormal * recoveryShift;

	float recoveryBlend = Max(0.08f, Min(0.24f,
		(secondLateral - 0.35f) * 0.10f + currentSpeed * 0.0035f));
	if (distanceToFinal < 4.0f)
		recoveryBlend = Min(recoveryBlend, 0.18f);

	CVector2D blendedTarget = baseCandidate * (1.0f - recoveryBlend) + recoveryCandidate * recoveryBlend;
	if (pVehicle != nil && GcIsScriptedIntroAdmiral(pVehicle) && (CTimer::GetFrameCounter() % 15) == 0) {
		printf("[CARCTRL-RECOVERY] frame=%u signedLat=%f desiredLat=%f secondLat=%f secondAlong=%f secondLen=%f distFinal=%f shift=%f blend=%f base=(%f,%f) rec=(%f,%f) out=(%f,%f)\n",
			CTimer::GetFrameCounter(), signedLateral, desiredSignedLateral,
			secondLateral, secondAlong, secondLen, distanceToFinal,
			recoveryShift, recoveryBlend,
			baseCandidate.x, baseCandidate.y,
			recoveryCandidate.x, recoveryCandidate.y,
			blendedTarget.x, blendedTarget.y);
	}
	return blendedTarget;
}

static bool
GcGetScriptedIntroSegmentContinuationTarget(CVehicle *pVehicle, CVector2D *outTarget)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle) || outTarget == nil)
		return false;
	if (pVehicle->AutoPilot.m_nCurrentPathNodeInfo == pVehicle->AutoPilot.m_nNextPathNodeInfo)
		return false;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentAnchor = GcGetPathLinkLanePosition(
		pCurrentLink, pVehicle->AutoPilot.m_nCurrentLane, pVehicle->AutoPilot.m_nCurrentDirection);
	CVector2D nextAnchor = GcGetPathLinkLanePosition(
		pNextLink, pVehicle->AutoPilot.m_nNextLane, pVehicle->AutoPilot.m_nNextDirection);
	CVector2D segment = nextAnchor - currentAnchor;
	float segmentLen = segment.Magnitude();
	if (segmentLen <= 0.001f)
		return false;

	CVector2D segmentDir = segment / segmentLen;
	CVector2D laneRight(-segmentDir.y, segmentDir.x);
	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	CVector2D finalTarget(
		pVehicle->AutoPilot.m_vecDestinationCoors.x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y);
	float alongVehicle = DotProduct2D(vehiclePos - currentAnchor, segmentDir);
	float alongFinal = DotProduct2D(finalTarget - currentAnchor, segmentDir);
	float lateralVehicle = Abs(DotProduct2D(vehiclePos - currentAnchor, laneRight));
	float lateralFinal = Abs(DotProduct2D(finalTarget - currentAnchor, laneRight));
	float finalFromNext = (finalTarget - nextAnchor).Magnitude();
	float distanceToFinal = GcGetScriptedIntroDistanceToFinal(pVehicle);
	float maxSegmentOvershoot = Max(2.0f, Min(7.0f, distanceToFinal * 0.7f));

	if (alongFinal < alongVehicle - 2.0f)
		return false;
	if (alongFinal > segmentLen + 35.0f)
		return false;
	if (alongVehicle > segmentLen + maxSegmentOvershoot)
		return false;
	if (lateralFinal > 6.0f)
		return false;
	if (lateralVehicle > 8.0f && distanceToFinal > 12.0f)
		return false;
	if (alongVehicle < -0.5f)
		return false;
	if (finalFromNext < 1.5f)
		return false;

	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float lookAhead = Max(4.0f, Min(12.0f, 3.0f + currentSpeed * 0.35f));
	float baseAlong = Max(0.0f, alongVehicle);
	float minLead = alongVehicle < 0.0f
		? Max(3.5f, lookAhead * 0.75f)
		: 2.0f;
	float targetAlong = Max(baseAlong + minLead, baseAlong + lookAhead);
	targetAlong = Min(targetAlong, alongFinal);
	targetAlong = Max(targetAlong, 0.5f);
	CVector2D candidate = currentAnchor + segmentDir * targetAlong;
	if (!GcCommitScriptedIntroGuideTarget(pVehicle, candidate, outTarget))
		return false;
	GcTraceAdmiralGuideDecision(pVehicle, "segment-continue",
		currentAnchor, nextAnchor, finalTarget, vehiclePos, *outTarget,
		-1.0f, (nextAnchor - vehiclePos).Magnitude(), distanceToFinal,
		alongVehicle, segmentLen, lateralVehicle,
		alongFinal, segmentLen, lateralFinal);
	return true;
}

static bool
GcShouldDeferScriptedIntroActiveSegmentAdvance(CVehicle *pVehicle);

static bool
GcGetScriptedIntroLaneGuideTarget(CVehicle *pVehicle, CVector2D *outTarget);

static bool
GcGetScriptedIntroActiveSegmentHandoffTarget(CVehicle *pVehicle, CVector2D *outTarget)
{
	if (!GcShouldDeferScriptedIntroActiveSegmentAdvance(pVehicle) || outTarget == nil)
		return false;

	if (GcGetScriptedIntroLaneGuideTarget(pVehicle, outTarget))
		return true;
	if (GcGetScriptedIntroSegmentContinuationTarget(pVehicle, outTarget))
		return true;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentAnchor = GcGetPathLinkLanePosition(
		pCurrentLink, pVehicle->AutoPilot.m_nCurrentLane, pVehicle->AutoPilot.m_nCurrentDirection);
	CVector2D nextAnchor = GcGetPathLinkLanePosition(
		pNextLink, pVehicle->AutoPilot.m_nNextLane, pVehicle->AutoPilot.m_nNextDirection);
	CVector2D segment = nextAnchor - currentAnchor;
	float segmentLen = segment.Magnitude();
	if (segmentLen <= 0.001f)
		return false;

	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	CVector2D segmentDir = segment / segmentLen;
	float along = DotProduct2D(vehiclePos - currentAnchor, segmentDir);
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float baseAlong = Max(0.0f, along);
	float targetAlong = Min(segmentLen,
		Max(baseAlong + 2.0f,
		    baseAlong + Max(3.0f, Min(8.0f, 2.0f + currentSpeed * 0.25f))));
	CVector2D candidate = currentAnchor + segmentDir * targetAlong;
	return GcCommitScriptedIntroGuideTarget(pVehicle, candidate, outTarget);
}

static bool
GcIsScriptedIntroTightFinalApproach(CVehicle *pVehicle)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle))
		return false;
	float distanceToFinal = GcGetScriptedIntroDistanceToFinal(pVehicle);
	if (distanceToFinal > 8.0f)
		return false;
	if (pVehicle->AutoPilot.m_nCurrentPathNodeInfo == pVehicle->AutoPilot.m_nNextPathNodeInfo)
		return false;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentAnchor = GcGetPathLinkLanePosition(
		pCurrentLink, pVehicle->AutoPilot.m_nCurrentLane, pVehicle->AutoPilot.m_nCurrentDirection);
	CVector2D nextAnchor = GcGetPathLinkLanePosition(
		pNextLink, pVehicle->AutoPilot.m_nNextLane, pVehicle->AutoPilot.m_nNextDirection);
	CVector2D segment = nextAnchor - currentAnchor;
	float segmentLen = segment.Magnitude();
	if (segmentLen <= 0.001f)
		return false;

	CVector2D segmentDir = segment / segmentLen;
	CVector2D laneRight(-segmentDir.y, segmentDir.x);
	CVector2D finalTarget(
		pVehicle->AutoPilot.m_vecDestinationCoors.x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y);
	CVector2D nextToFinal = finalTarget - nextAnchor;
	float alongPastNext = DotProduct2D(nextToFinal, segmentDir);
	float lateralFromNext = Abs(DotProduct2D(nextToFinal, laneRight));
	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	float secondAlong = DotProduct2D(vehiclePos - nextAnchor, nextToFinal.Magnitude() > 0.001f ? nextToFinal / nextToFinal.Magnitude() : segmentDir);
	float secondLateral = 99999.0f;
	CVector2D vehicleForward = pVehicle->GetForward();
	float vehicleForwardLen = vehicleForward.Magnitude();
	if (vehicleForwardLen > 0.0001f)
		vehicleForward /= vehicleForwardLen;
	if (nextToFinal.Magnitude() > 0.001f) {
		CVector2D secondDir = nextToFinal / nextToFinal.Magnitude();
		CVector2D secondClosest = nextAnchor + secondDir * Min(nextToFinal.Magnitude(), Max(0.0f, secondAlong));
		secondLateral = (vehiclePos - secondClosest).Magnitude();
		if (vehicleForwardLen > 0.0001f && DotProduct2D(vehicleForward, secondDir) < 0.975f)
			return false;
	}

	// The custom intro approach is only valid when the script target still lives
	// near the active lane end. If the destination has moved well past this lane
	// end, we should fall back to normal road/path steering instead of trying to
	// "hold" an outdated terminal segment.
	if (alongPastNext < -4.0f || alongPastNext > 6.0f)
		return false;
	if (lateralFromNext > 4.0f)
		return false;
	if (secondAlong < 6.0f)
		return false;
	if (secondLateral > 0.85f)
		return false;
	return true;
}

static bool
GcGetScriptedIntroLaneGuideTarget(CVehicle *pVehicle, CVector2D *outTarget)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle) || outTarget == nil)
		return false;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentAnchor = GcGetPathLinkLanePosition(
		pCurrentLink, pVehicle->AutoPilot.m_nCurrentLane, pVehicle->AutoPilot.m_nCurrentDirection);
	CVector2D nextAnchor = GcGetPathLinkLanePosition(
		pNextLink, pVehicle->AutoPilot.m_nNextLane, pVehicle->AutoPilot.m_nNextDirection);
	CVector2D finalTarget(
		pVehicle->AutoPilot.m_vecDestinationCoors.x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y);
	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float distanceToFinal = (finalTarget - vehiclePos).Magnitude();
	float distToNextAnchor = (nextAnchor - vehiclePos).Magnitude();

	CVector currentCurveAnchor(currentAnchor.x, currentAnchor.y, 0.0f);
	CVector nextCurveAnchor(nextAnchor.x, nextAnchor.y, 0.0f);
	CVector currentCurveDir(
		pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection,
		pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection,
		0.0f);
	CVector nextCurveDir(
		pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection,
		pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection,
		0.0f);
	float currentCurvePos = 0.0f;
	if (pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve > 0)
		currentCurvePos = CCarCtrl::GetPositionAlongCurrentCurve(pVehicle);
	currentCurvePos = Max(0.0f, Min(1.2f, currentCurvePos));

	if (currentCurveDir.Magnitude2D() > 0.0001f && nextCurveDir.Magnitude2D() > 0.0001f &&
	    pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve > 0 &&
	    distanceToFinal > 10.0f &&
	    distToNextAnchor > 6.0f) {
		float leadPos = Max(0.06f, Min(0.18f, 0.04f + currentSpeed * 0.006f));
		float samplePos = Min(1.0f, currentCurvePos + leadPos);
		CVector curvePoint;
		CVector curveDirection;
		CCurves::CalcCurvePoint(
			&currentCurveAnchor,
			&nextCurveAnchor,
			&currentCurveDir,
			&nextCurveDir,
			samplePos,
			pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve,
			&curvePoint,
			&curveDirection);

		if (currentCurvePos < 0.97f || distanceToFinal > 14.0f) {
			if (GcCommitScriptedIntroGuideTarget(
				pVehicle, CVector2D(curvePoint.x, curvePoint.y), outTarget)) {
				GcTraceAdmiralGuideDecision(pVehicle, "lane-curve",
					currentAnchor, nextAnchor, finalTarget, vehiclePos, *outTarget,
					currentCurvePos, distToNextAnchor, distanceToFinal,
					0.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 0.0f);
				return true;
			}
		}

		CVector curveExitPoint;
		CVector curveExitDir;
		CCurves::CalcCurvePoint(
			&currentCurveAnchor,
			&nextCurveAnchor,
			&currentCurveDir,
			&nextCurveDir,
			1.0f,
			pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve,
			&curveExitPoint,
			&curveExitDir);
		CVector2D exitPoint(curveExitPoint.x, curveExitPoint.y);
		CVector2D exitDir(curveExitDir.x, curveExitDir.y);
		float exitDirLen = exitDir.Magnitude();
		if (exitDirLen > 0.0001f)
			exitDir /= exitDirLen;
		else {
			exitDir = finalTarget - exitPoint;
			float fallbackLen = exitDir.Magnitude();
			if (fallbackLen > 0.0001f)
				exitDir /= fallbackLen;
		}

		if (exitDir.Magnitude() > 0.0001f) {
			float roadLead = Max(1.5f, Min(4.0f, 1.0f + currentSpeed * 0.16f));
			CVector2D roadTarget = exitPoint + exitDir * roadLead;
			if (currentCurvePos > 0.92f && distanceToFinal > 4.5f) {
				if (GcCommitScriptedIntroGuideTarget(pVehicle, roadTarget, outTarget)) {
					GcTraceAdmiralGuideDecision(pVehicle, "lane-road",
						currentAnchor, nextAnchor, finalTarget, vehiclePos, *outTarget,
						currentCurvePos, distToNextAnchor, distanceToFinal,
						0.0f, 0.0f, 0.0f,
						0.0f, 0.0f, 0.0f);
					return true;
				}
			}

			float snapBlend = Max(0.0f, Min(1.0f, (4.5f - distanceToFinal) / 4.5f));
			if (GcCommitScriptedIntroGuideTarget(
				pVehicle, roadTarget * (1.0f - snapBlend) + finalTarget * snapBlend, outTarget)) {
				GcTraceAdmiralGuideDecision(pVehicle, "lane-road-blend",
					currentAnchor, nextAnchor, finalTarget, vehiclePos, *outTarget,
					currentCurvePos, distToNextAnchor, distanceToFinal,
					0.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 0.0f);
				return true;
			}
		}
	}


	CVector2D firstSegment = nextAnchor - currentAnchor;
	float firstLen = firstSegment.Magnitude();
	if (firstLen < 0.001f)
		return false;
	CVector2D firstDir = firstSegment / firstLen;
	float firstAlong = DotProduct2D(vehiclePos - currentAnchor, firstDir);
	CVector2D firstClosest = currentAnchor + firstDir * Min(firstLen, Max(0.0f, firstAlong));
	float firstLateral = (vehiclePos - firstClosest).Magnitude();

	CVector2D secondDir(0.0f, 0.0f);
	float secondLen = 0.0f;
	float secondAlong = 0.0f;
	float secondLateral = 99999.0f;
	bool hasSecondSegment =
		GcHasMeaningfulScriptedIntroSecondSegment(nextAnchor, finalTarget, &secondDir, &secondLen);
	bool holdFirstSegmentIntoTurn = false;
	if (hasSecondSegment) {
		secondAlong = DotProduct2D(vehiclePos - nextAnchor, secondDir);
		CVector2D secondClosest = nextAnchor + secondDir * Min(secondLen, Max(0.0f, secondAlong));
		secondLateral = (vehiclePos - secondClosest).Magnitude();
		float turnCross = firstDir.x * secondDir.y - firstDir.y * secondDir.x;
		float pastNextAnchor = firstAlong - firstLen;
		float entryCarry = Max(0.40f, Min(1.25f, 0.30f + currentSpeed * 0.04f));
		float previewDistance = Max(0.90f, Min(1.80f, 0.70f + currentSpeed * 0.03f));
		float secondEntryAlong = Max(0.55f, Min(1.35f, 0.30f + currentSpeed * 0.05f));

		// The retail intro does not begin the hotel turn with a "preview nibble"
		// before the car reaches the junction. Holding the first segment until we
		// have either passed the node a bit or genuinely projected onto the second
		// segment avoids the current double-steer shape.
		holdFirstSegmentIntoTurn =
			Abs(turnCross) > 0.20f &&
			distanceToFinal > 10.0f &&
			(pastNextAnchor < entryCarry || distToNextAnchor > previewDistance) &&
			secondAlong < secondEntryAlong;
	}

	bool useSecondSegment = false;
	if (hasSecondSegment) {
		float previewSecondLookAhead = Max(2.5f, Min(6.0f, currentSpeed * 0.20f + 2.0f));
		float previewSecondExtension = 0.0f;
		if (currentSpeed > 8.0f || secondLateral > 0.75f)
			previewSecondExtension = Min(3.0f, Max(1.0f, currentSpeed * 0.08f));
		float previewSecondAlong = Max(0.5f, secondAlong + previewSecondLookAhead);
		previewSecondAlong = Min(secondLen + previewSecondExtension, previewSecondAlong);

		float previewFirstLookAhead = Max(3.0f, Min(8.0f, currentSpeed * 0.25f + 2.5f));
		float previewFirstAlong = Max(1.0f, firstAlong + previewFirstLookAhead);
		previewFirstAlong = Min(firstLen, previewFirstAlong);

		CVector2D firstCandidate = currentAnchor + firstDir * previewFirstAlong;
		CVector2D secondCandidate = nextAnchor + secondDir * previewSecondAlong;
		CVector2D vehicleForward = pVehicle->GetForward();
		float vehicleForwardLen = vehicleForward.Magnitude();
		if (vehicleForwardLen > 0.0001f)
			vehicleForward /= vehicleForwardLen;
		else
			vehicleForward = firstDir;

		float firstTurnAbs = 999.0f;
		float secondTurnAbs = 999.0f;
		CVector2D toFirst = firstCandidate - vehiclePos;
		CVector2D toSecond = secondCandidate - vehiclePos;
		if (toFirst.Magnitude() > 0.001f && toSecond.Magnitude() > 0.001f) {
			float angleForward = CGeneral::GetATanOfXY(vehicleForward.x, vehicleForward.y);
			float firstAngle = CGeneral::GetATanOfXY(toFirst.x, toFirst.y);
			float secondAngle = CGeneral::GetATanOfXY(toSecond.x, toSecond.y);
			firstTurnAbs = Abs(CCarCtrl::LimitRadianAngle(firstAngle - angleForward));
			secondTurnAbs = Abs(CCarCtrl::LimitRadianAngle(secondAngle - angleForward));
		}

		float nearNextAnchor = Max(2.0f, Min(3.25f, 1.0f + currentSpeed * 0.10f));
		float lateTurnSlack = Max(1.0f, Min(2.0f, currentSpeed * 0.06f));
		float transitionSlack = Max(0.75f, Min(1.5f, 0.60f + currentSpeed * 0.05f));
		float transitionAlongSlack = Max(0.20f, Min(0.55f, 0.15f + currentSpeed * 0.02f));
		bool canEnterSecondSegment =
			secondAlong > transitionAlongSlack ||
			distToNextAnchor < transitionSlack ||
			firstAlong > firstLen - transitionSlack;

		if (!holdFirstSegmentIntoTurn &&
		    (canEnterSecondSegment &&
		     firstAlong > firstLen - lateTurnSlack) ||
		    !holdFirstSegmentIntoTurn &&
		    (canEnterSecondSegment &&
		     distToNextAnchor < nearNextAnchor &&
		     firstAlong > firstLen * 0.80f) ||
		    !holdFirstSegmentIntoTurn &&
		    (distanceToFinal < 7.0f &&
		     canEnterSecondSegment &&
		     firstAlong > firstLen * 0.90f &&
		     secondLateral + 0.35f < firstLateral) ||
		    // Don't jump onto the second segment just because its preview angle
		    // looks cleaner while we're still physically short of the junction.
		    // That early diagonal cut is what sends the intro Admiral into the
		    // hotel corner instead of carrying the first lane to the node.
		    !holdFirstSegmentIntoTurn &&
		    (canEnterSecondSegment &&
		     firstAlong > firstLen * 0.82f &&
		     secondTurnAbs + 0.12f < firstTurnAbs &&
		     secondLateral < firstLateral + 1.25f))
			useSecondSegment = true;

		float carryLookAhead = Max(3.0f, Min(8.0f, currentSpeed * 0.25f + 2.5f));
		float carryTargetAlong = Max(1.0f, firstAlong + carryLookAhead);
		if (!holdFirstSegmentIntoTurn &&
		    carryTargetAlong > firstLen &&
		    firstAlong > firstLen * 0.82f &&
		    distToNextAnchor < Max(3.5f, nearNextAnchor * 1.35f) &&
		    secondAlong < Max(1.5f, currentSpeed * 0.06f)) {
			float carryIntoSecond = Min(secondLen, carryTargetAlong - firstLen);
			float carryCap = Max(1.5f, Min(3.25f, distToNextAnchor * 1.80f));
			carryIntoSecond = Min(carryCap, carryIntoSecond);
			if (secondAlong > 0.0f)
				carryIntoSecond = Max(carryIntoSecond, Min(carryCap, secondAlong));

			CVector2D straightCandidate = nextAnchor + secondDir * Max(0.35f, carryIntoSecond);
			CVector2D candidate = straightCandidate;
			if (firstLen + secondLen > 0.001f) {
				float curveLeadDist = Max(2.5f, Min(6.5f, currentSpeed * 0.22f + 1.75f));
				float progressDist = firstLen + Max(0.0f, Min(secondLen, secondAlong));
				float curveSampleDist = Min(firstLen + secondLen, progressDist + curveLeadDist);
				float curveSamplePos = Max(0.0f, Min(1.0f, curveSampleDist / (firstLen + secondLen)));

				CVector curveStart(currentAnchor.x, currentAnchor.y, 0.0f);
				CVector curveEnd(finalTarget.x, finalTarget.y, 0.0f);
				CVector curveDir1(firstDir.x, firstDir.y, 0.0f);
				CVector curveDir2(secondDir.x, secondDir.y, 0.0f);
				CVector curvePoint;
				CVector curveDirection;
				CCurves::CalcCurvePoint(
					&curveStart,
					&curveEnd,
					&curveDir1,
					&curveDir2,
					curveSamplePos,
					1000,
					&curvePoint,
					&curveDirection);

				CVector2D curveCandidate(curvePoint.x, curvePoint.y);
				CVector2D curveTangent(curveDirection.x, curveDirection.y);
				float curveTangentLen = curveTangent.Magnitude();
				if (curveTangentLen > 0.0001f)
					curveTangent /= curveTangentLen;
				else
					curveTangent = secondDir;
				float tangentLead = Max(1.75f, Min(4.25f, currentSpeed * 0.16f + 1.35f));
				CVector2D tangentCandidate = curveCandidate + curveTangent * tangentLead;
				float curveBlend = Max(0.60f, Min(0.88f,
					0.46f + currentSpeed * 0.02f + Max(0.0f, secondLateral) * 0.05f));
				candidate = straightCandidate * (1.0f - curveBlend) + tangentCandidate * curveBlend;
				candidate = GcBlendScriptedIntroSecondSegmentRecoveryTarget(
					pVehicle, vehiclePos, candidate, nextAnchor, firstDir, secondDir, secondLen,
					secondAlong, secondLateral, distanceToFinal, currentSpeed);
			}
			if (GcCommitScriptedIntroGuideTarget(pVehicle, candidate, outTarget)) {
				GcTraceAdmiralGuideDecision(pVehicle, "lane-carry-early",
					currentAnchor, nextAnchor, finalTarget, vehiclePos, *outTarget,
					currentCurvePos, distToNextAnchor, distanceToFinal,
					firstAlong, firstLen, firstLateral,
					secondAlong, secondLen, secondLateral);
				return true;
			}
		}
	}

	if (useSecondSegment) {
		float lookAhead = Max(2.5f, Min(6.0f, currentSpeed * 0.20f + 2.0f));
		if (secondLateral > 0.5f)
			lookAhead = Max(2.0f, lookAhead - Min(1.5f, secondLateral * 0.75f));
		float extension = 0.0f;
		if (currentSpeed > 8.0f || secondLateral > 0.75f)
			extension = Min(3.0f, Max(1.0f, currentSpeed * 0.08f));
		float targetAlong = Max(0.5f, secondAlong + lookAhead);
		targetAlong = Min(secondLen + extension, targetAlong);
		CVector2D straightCandidate = nextAnchor + secondDir * targetAlong;
		CVector2D candidate = straightCandidate;
		bool usedCurveBlend = false;
		CVector2D vehicleForward = pVehicle->GetForward();
		float vehicleForwardLen = vehicleForward.Magnitude();
		if (vehicleForwardLen > 0.0001f)
			vehicleForward /= vehicleForwardLen;
		else
			vehicleForward = secondDir;
		float secondForwardDp = DotProduct2D(vehicleForward, secondDir);
		float turnCross = firstDir.x * secondDir.y - firstDir.y * secondDir.x;
		float turnMagnitude = Abs(turnCross);
		float headingMisalignment = Max(0.0f, 1.0f - secondForwardDp);
		bool keepCurveTarget =
			distanceToFinal > 2.5f &&
			(secondLateral > 0.20f ||
			 headingMisalignment > 0.035f ||
			 (turnMagnitude > 0.20f &&
			  secondAlong < Max(4.5f, Min(7.0f, 2.5f + currentSpeed * 0.14f))));

		// Once the Admiral has entered the second segment, a pure "point farther
		// down the final straight" target lets the car carry too much of its
		// outgoing tangent from the first segment. That is why the latest build
		// still drifts into the hotel corner/Faggio before correcting. Blend in a
		// proper transition curve so the target keeps bending the car back toward
		// the road centerline while the turn is still settling.
		if (keepCurveTarget) {
			const float totalLen = firstLen + secondLen;
			if (totalLen > 0.001f) {
				float progressDist = firstLen + Max(0.0f, Min(secondLen, secondAlong));
				float curveLeadDist = Max(1.5f, Min(4.5f, currentSpeed * 0.18f + 0.85f));
				float curveSampleDist = Min(totalLen, progressDist + curveLeadDist);
				float curveSamplePos = Max(0.0f, Min(1.0f, curveSampleDist / totalLen));

				CVector curveStart(currentAnchor.x, currentAnchor.y, 0.0f);
				CVector curveEnd(finalTarget.x, finalTarget.y, 0.0f);
				CVector curveDir1(firstDir.x, firstDir.y, 0.0f);
				CVector curveDir2(secondDir.x, secondDir.y, 0.0f);
				CVector curvePoint;
				CVector curveDirection;
				CCurves::CalcCurvePoint(
					&curveStart,
					&curveEnd,
					&curveDir1,
					&curveDir2,
					curveSamplePos,
					1000,
					&curvePoint,
					&curveDirection);

				CVector2D curveCandidate(curvePoint.x, curvePoint.y);
				CVector2D curveTangent(curveDirection.x, curveDirection.y);
				float curveTangentLen = curveTangent.Magnitude();
				if (curveTangentLen > 0.0001f)
					curveTangent /= curveTangentLen;
				else
					curveTangent = secondDir;
				float tangentLead = Max(1.25f, Min(3.5f, currentSpeed * 0.14f + 0.90f));
				CVector2D tangentCandidate = curveCandidate + curveTangent * tangentLead;
				float curveBlend = Max(0.42f, Min(0.90f,
					secondLateral * 0.12f +
					headingMisalignment * 1.35f +
					currentSpeed * 0.014f));
				if (distanceToFinal < 7.5f) {
					float settleBlend = Max(0.20f, Min(1.0f, (distanceToFinal - 1.5f) / 6.0f));
					curveBlend = Min(curveBlend, settleBlend);
				}
				candidate = straightCandidate * (1.0f - curveBlend) + tangentCandidate * curveBlend;
				usedCurveBlend = true;
			}
		}

		candidate = GcBlendScriptedIntroSecondSegmentRecoveryTarget(
			pVehicle, vehiclePos, candidate, nextAnchor, firstDir, secondDir, secondLen,
			secondAlong, secondLateral, distanceToFinal, currentSpeed);

		if (!GcCommitScriptedIntroGuideTarget(pVehicle, candidate, outTarget)) {
			if (!GcCommitScriptedIntroGuideTarget(pVehicle, straightCandidate, outTarget))
				return false;
			usedCurveBlend = false;
		}
		GcTraceAdmiralGuideDecision(pVehicle, usedCurveBlend ? "lane-second-curve" : "lane-second",
			currentAnchor, nextAnchor, finalTarget, vehiclePos, *outTarget,
			currentCurvePos, distToNextAnchor, distanceToFinal,
			firstAlong, firstLen, firstLateral,
			secondAlong, secondLen, secondLateral);
		return true;
	}

	float lookAhead = Max(3.0f, Min(8.0f, currentSpeed * 0.25f + 2.5f));
	float targetAlong = Max(1.0f, firstAlong + lookAhead);
	if (!holdFirstSegmentIntoTurn &&
	    hasSecondSegment &&
	    firstAlong > firstLen * 0.60f &&
	    secondAlong < Max(0.75f, currentSpeed * 0.08f) &&
	    distanceToFinal > 7.0f &&
	    distToNextAnchor < Max(4.5f, Min(6.0f, 4.25f + currentSpeed * 0.15f))) {
		const float totalLen = firstLen + secondLen;
		if (totalLen > 0.001f) {
			float curveLeadDist = Max(2.25f, Min(6.5f, currentSpeed * 0.22f + 1.5f));
			float curveProgressDist = Max(0.0f, firstAlong);
			float curveSampleDist = Min(totalLen, curveProgressDist + curveLeadDist);
			float curveSamplePos = Max(0.0f, Min(1.0f, curveSampleDist / totalLen));

			CVector curveStart(currentAnchor.x, currentAnchor.y, 0.0f);
			CVector curveEnd(finalTarget.x, finalTarget.y, 0.0f);
			CVector curveDir1(firstDir.x, firstDir.y, 0.0f);
			CVector curveDir2(secondDir.x, secondDir.y, 0.0f);
			CVector curvePoint;
			CVector curveDirection;
			CCurves::CalcCurvePoint(
				&curveStart,
				&curveEnd,
				&curveDir1,
				&curveDir2,
				curveSamplePos,
				1000,
				&curvePoint,
				&curveDirection);

			CVector2D straightEntryCandidate = currentAnchor + firstDir * Min(firstLen, targetAlong);
			CVector2D curveCandidate(curvePoint.x, curvePoint.y);
			CVector2D curveTangent(curveDirection.x, curveDirection.y);
			float curveTangentLen = curveTangent.Magnitude();
			if (curveTangentLen > 0.0001f)
				curveTangent /= curveTangentLen;
			else
				curveTangent = firstDir;
			float tangentLead = Max(2.0f, Min(4.75f, currentSpeed * 0.18f + 1.45f));
			CVector2D tangentCandidate = curveCandidate + curveTangent * tangentLead;
			float entryProgress = Max(0.0f, Min(1.0f,
				(firstAlong - firstLen * 0.60f) / Max(0.75f, firstLen * 0.22f)));
			float distProgress = Max(0.0f, Min(1.0f,
				(Max(4.5f, Min(6.0f, 4.25f + currentSpeed * 0.15f)) - distToNextAnchor) /
				2.0f));
			float curveBlend = Max(0.42f, Min(0.82f,
				0.30f + entryProgress * 0.24f + distProgress * 0.18f + currentSpeed * 0.012f));
			CVector2D candidate =
				straightEntryCandidate * (1.0f - curveBlend) + tangentCandidate * curveBlend;
			if (GcCommitScriptedIntroGuideTarget(pVehicle, candidate, outTarget)) {
				GcTraceAdmiralGuideDecision(pVehicle, "lane-first-curve",
					currentAnchor, nextAnchor, finalTarget, vehiclePos, *outTarget,
					currentCurvePos, distToNextAnchor, distanceToFinal,
					firstAlong, firstLen, firstLateral,
					secondAlong, secondLen, secondLateral);
				return true;
			}
		}
	}
	if (!holdFirstSegmentIntoTurn &&
	    hasSecondSegment &&
	    targetAlong > firstLen &&
	    firstAlong > firstLen - 1.0f &&
	    distToNextAnchor < 2.0f) {
		float carryIntoSecond = Min(secondLen, targetAlong - firstLen);
		CVector2D candidate = nextAnchor + secondDir * carryIntoSecond;
		if (!GcCommitScriptedIntroGuideTarget(pVehicle, candidate, outTarget))
			return false;
		GcTraceAdmiralGuideDecision(pVehicle, "lane-carry",
			currentAnchor, nextAnchor, finalTarget, vehiclePos, *outTarget,
			currentCurvePos, distToNextAnchor, distanceToFinal,
			firstAlong, firstLen, firstLateral,
			secondAlong, secondLen, secondLateral);
		return true;
	}

	targetAlong = Min(firstLen, targetAlong);
	CVector2D candidate = currentAnchor + firstDir * targetAlong;
	if (!GcCommitScriptedIntroGuideTarget(pVehicle, candidate, outTarget))
		return false;
	GcTraceAdmiralGuideDecision(pVehicle, "lane-first",
		currentAnchor, nextAnchor, finalTarget, vehiclePos, *outTarget,
		currentCurvePos, distToNextAnchor, distanceToFinal,
		firstAlong, firstLen, firstLateral,
		secondAlong, secondLen, secondLateral);
	return true;
}

static bool
GcGetScriptedIntroApproachAnchor(CVehicle *pVehicle, CVector2D *outAnchor)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle) || outAnchor == nil)
		return false;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentAnchor = GcGetPathLinkLanePosition(
		pCurrentLink, pVehicle->AutoPilot.m_nCurrentLane, pVehicle->AutoPilot.m_nCurrentDirection);
	CVector2D nextAnchor = GcGetPathLinkLanePosition(
		pNextLink, pVehicle->AutoPilot.m_nNextLane, pVehicle->AutoPilot.m_nNextDirection);
	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	CVector2D vehicleForward = pVehicle->GetForward();
	float forwardLen = vehicleForward.Magnitude();
	if (forwardLen > 0.0001f)
		vehicleForward /= forwardLen;
	else
		vehicleForward = CVector2D(1.0f, 0.0f);

	CVector2D finalTarget(
		pVehicle->AutoPilot.m_vecDestinationCoors.x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y);
	CVector2D finalSegment = finalTarget - nextAnchor;
	float finalSegmentLen = finalSegment.Magnitude();
	if (finalSegmentLen > 1.5f) {
		CVector2D finalSegmentDir = finalSegment / finalSegmentLen;
		float finalAlong = DotProduct2D(vehiclePos - nextAnchor, finalSegmentDir);
		CVector2D finalClosest =
			nextAnchor + finalSegmentDir * Min(finalSegmentLen, Max(0.0f, finalAlong));
		float finalLateral = (vehiclePos - finalClosest).Magnitude();
		float finalDistance = (finalTarget - vehiclePos).Magnitude();
		CVector2D incomingSegment = nextAnchor - currentAnchor;
		float incomingLen = incomingSegment.Magnitude();
		if (incomingLen > 0.001f) {
			CVector2D incomingDir = incomingSegment / incomingLen;
			float incomingAlong = DotProduct2D(vehiclePos - currentAnchor, incomingDir);
			if (finalDistance <= 6.0f &&
			    finalAlong > finalSegmentLen - Max(3.0f, Min(5.5f, finalDistance + 1.0f)) &&
			    incomingAlong > incomingLen + 1.0f &&
			    finalLateral < 6.0f) {
				GcTraceAdmiralGuideDecision(pVehicle, "anchor-final-segment",
					currentAnchor, nextAnchor, finalTarget, vehiclePos, nextAnchor,
					-1.0f, (nextAnchor - vehiclePos).Magnitude(), finalDistance,
					incomingAlong, incomingLen, finalLateral,
					finalAlong, finalSegmentLen, finalLateral);
				*outAnchor = nextAnchor;
				return true;
			}
		}
	}

	CVector2D currentTarget;
	if (!GcGetScriptedIntroFinalApproachTarget(pVehicle, currentAnchor, &currentTarget))
		return false;

	CVector2D currentToTarget = currentTarget - vehiclePos;
	float currentLen = currentToTarget.Magnitude();
	if (currentLen < 0.001f) {
		*outAnchor = currentAnchor;
		return true;
	}
	currentToTarget /= currentLen;

	float angleForward = CGeneral::GetATanOfXY(vehicleForward.x, vehicleForward.y);
	float currentAngle = CGeneral::GetATanOfXY(currentToTarget.x, currentToTarget.y);
	float currentTurnAbs = Abs(CCarCtrl::LimitRadianAngle(currentAngle - angleForward));
	float currentForwardDp = DotProduct2D(vehicleForward, currentToTarget);

	*outAnchor = currentAnchor;

	CVector2D nextTarget;
	if (!GcGetScriptedIntroFinalApproachTarget(pVehicle, nextAnchor, &nextTarget))
		return true;

	CVector2D nextToTarget = nextTarget - vehiclePos;
	float nextLen = nextToTarget.Magnitude();
	if (nextLen < 0.001f)
		return true;
	nextToTarget /= nextLen;

	float nextAngle = CGeneral::GetATanOfXY(nextToTarget.x, nextToTarget.y);
	float nextTurnAbs = Abs(CCarCtrl::LimitRadianAngle(nextAngle - angleForward));
	float nextForwardDp = DotProduct2D(vehicleForward, nextToTarget);

	// Default to the current path link. Only jump to the next link when the
	// current-link approach is clearly geometrically wrong for the intro car.
	if ((currentForwardDp < -0.05f && nextForwardDp > currentForwardDp + 0.15f) ||
	    (currentTurnAbs > 0.35f && nextTurnAbs + 0.10f < currentTurnAbs && nextForwardDp > -0.10f))
		*outAnchor = nextAnchor;
	return true;
}

static bool
GcGetScriptedIntroHeadingTarget(CVehicle *pVehicle, CVector2D *outTarget)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle) || outTarget == nil)
		return false;

	// Stay on the active road segment until the destination actually belongs to
	// the local end-of-segment geometry. A pure distance cutoff is too coarse
	// here and lets the intro Admiral snap toward the parking point while it
	// still should be following the road into the turn.
	if (!GcIsScriptedIntroTightFinalApproach(pVehicle) &&
	    GcGetScriptedIntroSegmentContinuationTarget(pVehicle, outTarget))
		return true;

	if (GcGetScriptedIntroLaneGuideTarget(pVehicle, outTarget))
		return true;

	CVector2D approachAnchor;
	if (!GcGetScriptedIntroApproachAnchor(pVehicle, &approachAnchor))
		return false;
	return GcGetScriptedIntroFinalApproachTarget(pVehicle, approachAnchor, outTarget);
}

static bool
GcShouldContinueScriptedIntroTerminalRoadSegment(CVehicle *pVehicle)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle))
		return false;
	if (pVehicle->AutoPilot.m_nPathFindNodesCount != 0)
		return false;
	if (pVehicle->AutoPilot.m_nCurrentRouteNode == pVehicle->AutoPilot.m_nNextRouteNode)
		return false;
	if (pVehicle->AutoPilot.m_nCurrentPathNodeInfo == pVehicle->AutoPilot.m_nNextPathNodeInfo)
		return false;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentAnchor = GcGetPathLinkLanePosition(
		pCurrentLink, pVehicle->AutoPilot.m_nCurrentLane, pVehicle->AutoPilot.m_nCurrentDirection);
	CVector2D nextAnchor = GcGetPathLinkLanePosition(
		pNextLink, pVehicle->AutoPilot.m_nNextLane, pVehicle->AutoPilot.m_nNextDirection);
	CVector2D segment = nextAnchor - currentAnchor;
	float segmentLen = segment.Magnitude();
	if (segmentLen <= 0.001f)
		return false;

	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	CVector2D segmentDir = segment / segmentLen;
	float along = DotProduct2D(vehiclePos - currentAnchor, segmentDir);
	float alongPastNext = along - segmentLen;
	CVector2D closestPoint = currentAnchor + segmentDir * Min(segmentLen, Max(0.0f, along));
	float lateral = (vehiclePos - closestPoint).Magnitude();
	float finalDistance = GcGetScriptedIntroDistanceToFinal(pVehicle);

	// This is not a "stay on follow-path" gate anymore. We only use it to delay
	// the final-approach handoff for a short carry phase after the queue empties,
	// so the Admiral keeps travelling down the road a bit farther before turning
	// into the hotel entrance like the original does.
	if (finalDistance <= 4.5f)
		return false;
	if (lateral > 3.5f)
		return false;
	if (alongPastNext > 5.0f)
		return false;
	return true;
}

static bool
GcGetScriptedIntroTerminalRoadGuideTarget(CVehicle *pVehicle, CVector2D *outTarget)
{
	if (!GcShouldContinueScriptedIntroTerminalRoadSegment(pVehicle) || outTarget == nil)
		return false;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentAnchor = GcGetPathLinkLanePosition(
		pCurrentLink, pVehicle->AutoPilot.m_nCurrentLane, pVehicle->AutoPilot.m_nCurrentDirection);
	CVector2D nextAnchor = GcGetPathLinkLanePosition(
		pNextLink, pVehicle->AutoPilot.m_nNextLane, pVehicle->AutoPilot.m_nNextDirection);
	CVector2D segment = nextAnchor - currentAnchor;
	float segmentLen = segment.Magnitude();
	if (segmentLen <= 0.001f)
		return false;

	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	CVector2D segmentDir = segment / segmentLen;
	CVector2D finalTarget(
		pVehicle->AutoPilot.m_vecDestinationCoors.x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y);
	float along = DotProduct2D(vehiclePos - currentAnchor, segmentDir);
	float alongPastNext = along - segmentLen;
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float finalDistance = GcGetScriptedIntroDistanceToFinal(pVehicle);
	float distToNextAnchor = (nextAnchor - vehiclePos).Magnitude();
	CVector2D secondDir(0.0f, 0.0f);
	float secondLen = 0.0f;
	float secondAlong = 0.0f;
	bool hasSecondSegment =
		GcHasMeaningfulScriptedIntroSecondSegment(nextAnchor, finalTarget, &secondDir, &secondLen);
	if (hasSecondSegment)
		secondAlong = DotProduct2D(vehiclePos - nextAnchor, secondDir);

	// Do not let the temporary terminal-carry target pull the Admiral toward the
	// new final segment before the car has actually reached the junction. That
	// premature handoff is the source of the visible "tiny preview turn" before
	// the real hotel turn begins.
	if (hasSecondSegment) {
		float enterSecondThreshold = Max(0.35f, Min(1.25f, 0.20f + currentSpeed * 0.04f));
		float nearNextThreshold = Max(1.5f, Min(2.75f, 1.0f + currentSpeed * 0.06f));
		if (secondAlong < enterSecondThreshold &&
		    distToNextAnchor > nearNextThreshold &&
		    alongPastNext < 0.75f)
			return false;
	}

	float carryLead = Max(3.0f, Min(7.0f, 2.25f + currentSpeed * 0.22f));
	float carryLimit = segmentLen + Max(3.5f, Min(6.5f, 2.5f + currentSpeed * 0.18f));
	float targetAlong = Max(segmentLen + 2.0f, along + carryLead);
	targetAlong = Min(carryLimit, targetAlong);

	CVector2D carryTarget = currentAnchor + segmentDir * targetAlong;
	CVector2D candidate = carryTarget;

	CVector2D approachTarget;
	if (GcGetScriptedIntroFinalApproachTarget(pVehicle, nextAnchor, &approachTarget)) {
		float blend = Max(0.0f, Min(1.0f, (alongPastNext - 1.0f) / 3.5f));
		if (finalDistance < 8.0f)
			blend = Max(blend, Max(0.0f, Min(1.0f, (8.0f - finalDistance) / 3.5f)));

		if (hasSecondSegment) {
			float turnCross = segmentDir.x * secondDir.y - segmentDir.y * secondDir.x;
			float carryTurnAbs = GcGetScriptedIntroTargetTurnAbs(pVehicle, carryTarget);
			float approachTurnAbs = GcGetScriptedIntroTargetTurnAbs(pVehicle, approachTarget);

			// Once the script destination has moved onto a real second segment,
			// do not keep forcing the car toward an extrapolated point on the old
			// segment when the second-segment target is already the smoother and
			// more geometrically correct continuation. That old carry point is what
			// created the visible "preview nibble, then hard correction" pattern.
			if (Abs(turnCross) > 0.12f &&
			    secondAlong < Max(2.0f, Min(4.0f, 1.0f + currentSpeed * 0.10f)) &&
			    approachTurnAbs + 0.10f < carryTurnAbs) {
				candidate = approachTarget;
			} else {
				candidate = carryTarget * (1.0f - blend) + approachTarget * blend;
			}
		} else {
			candidate = carryTarget * (1.0f - blend) + approachTarget * blend;
		}
	}

	if (!GcCommitScriptedIntroGuideTarget(pVehicle, candidate, outTarget)) {
		if (!GcCommitScriptedIntroGuideTarget(pVehicle, carryTarget, outTarget))
			return false;
	}
	return true;
}

static void
GcTraceScriptedIntroVelocitySample(CVehicle *pVehicle, const char *stage)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle))
		return;

	const CVector &move = pVehicle->GetMoveSpeed();
	const CVector &turn = pVehicle->GetTurnSpeed();
	const CVector &fwd = pVehicle->GetForward();
	float speed2d = Sqrt(move.x * move.x + move.y * move.y) * GAME_SPEED_TO_CARAI_SPEED;
	float fwdSpeed = DotProduct(move, fwd) * GAME_SPEED_TO_CARAI_SPEED;
	printf("[CARCTRL-VEL] stage=%s frame=%u mission=%u temp=%u status=%u count=%d nodes=%d/%d/%d path=%u/%u/%u pos=(%f,%f,%f) move=(%f,%f,%f) turn=(%f,%f,%f) speed2d=%f fwd2d=%f gas=%f brake=%f steer=%f hand=%d\n",
		stage,
		CTimer::GetFrameCounter(),
		(uint32)pVehicle->AutoPilot.m_nCarMission,
		(uint32)pVehicle->AutoPilot.m_nTempAction,
		(uint32)pVehicle->GetStatus(),
		(int)pVehicle->AutoPilot.m_nPathFindNodesCount,
		pVehicle->AutoPilot.m_nPrevRouteNode,
		pVehicle->AutoPilot.m_nCurrentRouteNode,
		pVehicle->AutoPilot.m_nNextRouteNode,
		pVehicle->AutoPilot.m_nPreviousPathNodeInfo,
		pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
		pVehicle->AutoPilot.m_nNextPathNodeInfo,
		pVehicle->GetPosition().x, pVehicle->GetPosition().y, pVehicle->GetPosition().z,
		move.x, move.y, move.z,
		turn.x, turn.y, turn.z,
		speed2d, fwdSpeed,
		pVehicle->m_fGasPedal, pVehicle->m_fBrakePedal, pVehicle->m_fSteerAngle,
		pVehicle->bIsHandbrakeOn ? 1 : 0);
}

static bool
GcShouldHoldScriptedIntroFinalSegment(CVehicle *pVehicle, int searchStartNode)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle))
		return false;
	if (pVehicle->AutoPilot.m_nCurrentRouteNode == pVehicle->AutoPilot.m_nNextRouteNode)
		return false;

	int finalNode = ThePaths.FindNodeClosestToCoors(
		pVehicle->AutoPilot.m_vecDestinationCoors, 0, 999999.9f);
	if (finalNode < 0 || finalNode != searchStartNode)
		return false;

	CVector2D distanceToFinal(
		pVehicle->AutoPilot.m_vecDestinationCoors.x - pVehicle->GetPosition().x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y - pVehicle->GetPosition().y);
	float finalDistance = distanceToFinal.Magnitude();
	if (finalDistance <= 4.0f || finalDistance > 14.0f)
		return false;
	if (!GcIsScriptedIntroTightFinalApproach(pVehicle))
		return false;

	CVector2D currentNodePos = ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nCurrentRouteNode].GetPosition();
	CVector2D nextNodePos = ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode].GetPosition();
	CVector2D segment = nextNodePos - currentNodePos;
	float segmentLen = segment.Magnitude();
	if (segmentLen <= 0.001f)
		return false;

	segment /= segmentLen;
	float along = DotProduct2D((CVector2D)pVehicle->GetPosition() - currentNodePos, segment);
	if (along < -1.0f || along > segmentLen + 10.0f)
		return false;

	return true;
}

static bool
GcIsHoldingScriptedIntroFinalSegment(CVehicle *pVehicle)
{
	if (pVehicle == nil)
		return false;
	if (pVehicle->AutoPilot.m_nPathFindNodesCount != 0)
		return false;
	return GcShouldHoldScriptedIntroFinalSegment(
		pVehicle, pVehicle->AutoPilot.m_nNextRouteNode);
}

static float
GcLimitScriptedIntroApproachSpeed(CVehicle *pVehicle, float speedTarget)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle))
		return speedTarget;
	if (GcShouldContinueScriptedIntroTerminalRoadSegment(pVehicle))
		return speedTarget;

	CVector2D distanceToFinal(
		pVehicle->AutoPilot.m_vecDestinationCoors.x - pVehicle->GetPosition().x,
		pVehicle->AutoPilot.m_vecDestinationCoors.y - pVehicle->GetPosition().y);
	float finalDistance = distanceToFinal.Magnitude();
	if (finalDistance >= 14.0f)
		return speedTarget;

	const bool tightFinalApproach = GcIsScriptedIntroTightFinalApproach(pVehicle);
	if (!tightFinalApproach &&
	    pVehicle->AutoPilot.m_nCurrentPathNodeInfo != pVehicle->AutoPilot.m_nNextPathNodeInfo) {
		CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
		CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
		CVector2D currentAnchor = GcGetPathLinkLanePosition(
			pCurrentLink, pVehicle->AutoPilot.m_nCurrentLane, pVehicle->AutoPilot.m_nCurrentDirection);
		CVector2D nextAnchor = GcGetPathLinkLanePosition(
			pNextLink, pVehicle->AutoPilot.m_nNextLane, pVehicle->AutoPilot.m_nNextDirection);
		CVector2D secondSegment(
			pVehicle->AutoPilot.m_vecDestinationCoors.x - nextAnchor.x,
			pVehicle->AutoPilot.m_vecDestinationCoors.y - nextAnchor.y);
		float secondLen = secondSegment.Magnitude();
		if (secondLen > 0.001f) {
			CVector2D secondDir = secondSegment / secondLen;
			float secondAlong = DotProduct2D((CVector2D)pVehicle->GetPosition() - nextAnchor, secondDir);
			float taperStartAlong = Max(7.5f, Min(secondLen - 3.0f, secondLen * 0.60f));

			// Do not start the scripted speed taper just because the destination is
			// nearby in absolute distance. The retail intro keeps carrying along the
			// second road segment and only bleeds speed once most of that segment is
			// actually behind the car.
			if (secondAlong < taperStartAlong && finalDistance > 6.0f)
				return speedTarget;
		}
	}

	float taperedLimit = Max(9.0f, finalDistance * 1.15f + 2.75f);
	if (tightFinalApproach)
		taperedLimit = Max(taperedLimit, 10.5f);
	else
		taperedLimit = Max(taperedLimit, 11.5f);
	return Min(speedTarget, taperedLimit);
}

static bool
GcShouldKeepScriptedGotoMission(CVehicle *pVehicle)
{
	if (pVehicle == nil)
		return false;
	if (!GcIsScriptedIntroAdmiral(pVehicle))
		return false;

	switch (pVehicle->AutoPilot.m_nCarMission) {
	case MISSION_GOTOCOORDS:
	case MISSION_GOTOCOORDS_STRAIGHT:
	case MISSION_GOTOCOORDS_ACCURATE:
	case MISSION_GOTO_COORDS_STRAIGHT_ACCURATE:
		return true;
	default:
		return false;
	}
}

static void
GcPreventScriptedIntroStraightMission(CVehicle *pVehicle)
{
	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return;

	switch (pVehicle->AutoPilot.m_nCarMission) {
	case MISSION_GOTOCOORDS_STRAIGHT:
		pVehicle->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS;
		break;
	case MISSION_GOTO_COORDS_STRAIGHT_ACCURATE:
		pVehicle->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS_ACCURATE;
		break;
	default:
		break;
	}
}

static void
GcResumeScriptedGotoPathMission(CVehicle *pVehicle)
{
	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return;

	switch (pVehicle->AutoPilot.m_nCarMission) {
	case MISSION_GOTOCOORDS_STRAIGHT:
		pVehicle->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS;
		break;
	case MISSION_GOTO_COORDS_STRAIGHT_ACCURATE:
		pVehicle->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS_ACCURATE;
		break;
	default:
		break;
	}
}

static void
GcSaveIntroRouteState(CVehicle *pVehicle, GcSavedIntroRouteState *outState)
{
	if (pVehicle == nil || outState == nil)
		return;

	outState->currentRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	outState->nextRouteNode = pVehicle->AutoPilot.m_nNextRouteNode;
	outState->prevRouteNode = pVehicle->AutoPilot.m_nPrevRouteNode;
	outState->currentPathNodeInfo = pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	outState->nextPathNodeInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	outState->previousPathNodeInfo = pVehicle->AutoPilot.m_nPreviousPathNodeInfo;
	outState->timeEnteredCurve = pVehicle->AutoPilot.m_nTimeEnteredCurve;
	outState->timeToSpendOnCurrentCurve = pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
	outState->previousDirection = pVehicle->AutoPilot.m_nPreviousDirection;
	outState->currentDirection = pVehicle->AutoPilot.m_nCurrentDirection;
	outState->nextDirection = pVehicle->AutoPilot.m_nNextDirection;
	outState->currentLane = pVehicle->AutoPilot.m_nCurrentLane;
	outState->nextLane = pVehicle->AutoPilot.m_nNextLane;
	outState->pathFindNodesCount = pVehicle->AutoPilot.m_nPathFindNodesCount;
}

static void
GcRestoreIntroRouteState(CVehicle *pVehicle, const GcSavedIntroRouteState *state)
{
	if (pVehicle == nil || state == nil)
		return;

	pVehicle->AutoPilot.m_nCurrentRouteNode = state->currentRouteNode;
	pVehicle->AutoPilot.m_nNextRouteNode = state->nextRouteNode;
	pVehicle->AutoPilot.m_nPrevRouteNode = state->prevRouteNode;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = state->currentPathNodeInfo;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = state->nextPathNodeInfo;
	pVehicle->AutoPilot.m_nPreviousPathNodeInfo = state->previousPathNodeInfo;
	pVehicle->AutoPilot.m_nTimeEnteredCurve = state->timeEnteredCurve;
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = state->timeToSpendOnCurrentCurve;
	pVehicle->AutoPilot.m_nPreviousDirection = state->previousDirection;
	pVehicle->AutoPilot.m_nCurrentDirection = state->currentDirection;
	pVehicle->AutoPilot.m_nNextDirection = state->nextDirection;
	pVehicle->AutoPilot.m_nCurrentLane = state->currentLane;
	pVehicle->AutoPilot.m_nNextLane = state->nextLane;
	pVehicle->AutoPilot.m_nPathFindNodesCount = state->pathFindNodesCount;
}

static void
GcRebuildIntroRouteFromSavedNodes(CVehicle *pVehicle, const GcSavedIntroRouteState *state)
{
	if (pVehicle == nil || state == nil)
		return;

	pVehicle->AutoPilot.m_nPrevRouteNode = state->prevRouteNode;
	pVehicle->AutoPilot.m_nCurrentRouteNode = state->currentRouteNode;
	pVehicle->AutoPilot.m_nNextRouteNode = state->nextRouteNode;
	pVehicle->AutoPilot.m_nPathFindNodesCount = 0;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = 0;
	pVehicle->AutoPilot.m_nPreviousPathNodeInfo = 0;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = 0;
	pVehicle->AutoPilot.m_nTimeEnteredCurve = CTimer::GetTimeInMilliseconds();
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = 0;
	pVehicle->AutoPilot.m_nCurrentLane = 0;
	pVehicle->AutoPilot.m_nNextLane = 0;
	pVehicle->AutoPilot.m_nPreviousDirection = 0;
	pVehicle->AutoPilot.m_nCurrentDirection = 0;
	pVehicle->AutoPilot.m_nNextDirection = 0;

	if (pVehicle->AutoPilot.m_nCurrentRouteNode != pVehicle->AutoPilot.m_nNextRouteNode)
		CCarCtrl::FindLinksToGoWithTheseNodes(pVehicle);
}

static bool
GcHasUsableIntroRouteState(const GcSavedIntroRouteState *state)
{
	if (state == nil)
		return false;
	if (state->currentRouteNode == state->nextRouteNode)
		return false;
	if (state->currentPathNodeInfo == 0 && state->nextPathNodeInfo == 0)
		return false;
	if (state->currentDirection == 0 && state->nextDirection == 0 &&
	    state->timeToSpendOnCurrentCurve == 0)
		return false;
	return true;
}

static bool
GcTryContinueScriptedIntroAlongActiveSegment(CVehicle *pVehicle,
	const GcSavedIntroRouteState *savedState,
	CPathNode **nodes, int16 *count)
{
	if (pVehicle == nil || !GcIsScriptedIntroAdmiral(pVehicle))
		return false;
	if (savedState == nil || nodes == nil || count == nil)
		return false;
	if (!GcHasUsableIntroRouteState(savedState) || *count <= 0 || nodes[0] == nil)
		return false;

	const int32 firstNode = nodes[0] - ThePaths.m_pathNodes;
	if (firstNode != savedState->nextRouteNode)
		return false;

	// The reroute starts with the node the Admiral is already driving toward.
	// Re-queuing that duplicate prefix alongside the live  current->next segment
	// creates a contradictory state: follow-path sees a deep-in-segment vehicle
	// plus a queued duplicate of nextRouteNode and immediately consumes it,
	// which is exactly the early advance that throws the car into the spin.
	//
	// The correct recovery is to keep only the live segment now, update the
	// destination, and let the normal segment handoff trigger a fresh path
	// search from the real handoff node on the frame where the segment ends.
	GcRestoreIntroRouteState(pVehicle, savedState);
	pVehicle->AutoPilot.m_nPathFindNodesCount = 0;
	pVehicle->AutoPilot.m_aPathFindNodesInfo[0] = nil;
	*count = 0;
	return true;
}

static bool
GcHasScriptedIntroActiveSegmentRoute(const CVehicle *pVehicle)
{
	if (pVehicle == nil || !GcIsScriptedIntroAdmiral((CVehicle*)pVehicle))
		return false;
	if (pVehicle->AutoPilot.m_nCurrentRouteNode == 0 ||
	    pVehicle->AutoPilot.m_nNextRouteNode == 0)
		return false;
	if (pVehicle->AutoPilot.m_nCurrentRouteNode == pVehicle->AutoPilot.m_nNextRouteNode)
		return false;
	if (pVehicle->AutoPilot.m_nCurrentPathNodeInfo == pVehicle->AutoPilot.m_nNextPathNodeInfo)
		return false;
	return true;
}

static bool
GcHasQueuedScriptedIntroActiveTargetDuplicate(const CVehicle *pVehicle)
{
	if (pVehicle == nil || !GcIsScriptedIntroAdmiral((CVehicle*)pVehicle))
		return false;
	if (pVehicle->AutoPilot.m_nPathFindNodesCount <= 0)
		return false;

	CPathNode *firstQueuedNode = pVehicle->AutoPilot.m_aPathFindNodesInfo[0];
	if (firstQueuedNode == nil)
		return false;

	return firstQueuedNode - ThePaths.m_pathNodes == pVehicle->AutoPilot.m_nNextRouteNode;
}

static bool
GcShouldDeferScriptedIntroActiveSegmentAdvance(CVehicle *pVehicle)
{
	if (!GcHasScriptedIntroActiveSegmentRoute(pVehicle))
		return false;
	if (!GcHasQueuedScriptedIntroActiveTargetDuplicate(pVehicle))
		return false;

	CCarPathLink *pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink *pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentAnchor = GcGetPathLinkLanePosition(
		pCurrentLink, pVehicle->AutoPilot.m_nCurrentLane, pVehicle->AutoPilot.m_nCurrentDirection);
	CVector2D nextAnchor = GcGetPathLinkLanePosition(
		pNextLink, pVehicle->AutoPilot.m_nNextLane, pVehicle->AutoPilot.m_nNextDirection);
	CVector2D segment = nextAnchor - currentAnchor;
	float segmentLen = segment.Magnitude();
	if (segmentLen <= 0.001f)
		return false;

	CVector2D vehiclePos = (CVector2D)pVehicle->GetPosition();
	CVector2D segmentDir = segment / segmentLen;
	float along = DotProduct2D(vehiclePos - currentAnchor, segmentDir);
	CVector2D closestPoint = currentAnchor + segmentDir * Min(segmentLen, Max(0.0f, along));
	float distToNextAnchor = (nextAnchor - vehiclePos).Magnitude();
	float lateral = (vehiclePos - closestPoint).Magnitude();

	// The duplicate queue entry means the fresh path search starts with the node
	// the Admiral is already driving toward. Do not consume that duplicate until
	// the live road segment is genuinely finished, otherwise follow-path promotes
	// the car into the next segment a frame too early and the physics controller
	// restarts from an almost-zero-speed yaw mismatch.
	if (distToNextAnchor <= 0.9f)
		return false;
	if (along >= segmentLen + 0.75f)
		return false;
	if (lateral > 4.0f && along > segmentLen)
		return false;
	return true;
}
#endif

#define PATH_DIRECTION_NONE (0)
#define PATH_DIRECTION_STRAIGHT (1)
#define PATH_DIRECTION_RIGHT (2)
#define PATH_DIRECTION_LEFT (4)

#define ATTEMPTS_TO_FIND_NEXT_NODE (15)

#define DISTANCE_TO_SWITCH_FROM_BLOCK_TO_STOP (5.0f)
#define DISTANCE_TO_SWITCH_FROM_STOP_TO_BLOCK (10.0f)
#define MAX_SPEED_TO_ACCOUNT_IN_INTERCEPTING (0.13f)
#define DISTANCE_TO_NEXT_NODE_TO_CONSIDER_SLOWING_DOWN (40.0f)
#define MAX_ANGLE_TO_STEER_AT_HIGH_SPEED (0.2f)
#define MIN_SPEED_TO_START_LIMITING_STEER (0.45f)
#define DISTANCE_TO_NEXT_NODE_TO_SELECT_NEW (5.0f)
#define DISTANCE_TO_FACING_NEXT_NODE_TO_SELECT_NEW (8.0f)
#define DEFAULT_MAX_STEER_ANGLE (0.5f)
#define MIN_LOWERING_SPEED_COEFFICIENT (0.4f)
#define MAX_ANGLE_FOR_SPEED_LIMITING (1.2f)
#define MIN_ANGLE_FOR_SPEED_LIMITING (0.4f)
#define MIN_ANGLE_FOR_SPEED_LIMITING_BETWEEN_NODES (0.1f)
#define MIN_ANGLE_TO_APPLY_HANDBRAKE (0.7f)
#define MIN_SPEED_TO_APPLY_HANDBRAKE (0.3f)

#define PROBABILITY_OF_DEAD_PED_ACCIDENT (0.005f)
#define DISTANCE_BETWEEN_CAR_AND_DEAD_PED (6.0f)
#define PROBABILITY_OF_PASSENGER_IN_VEHICLE (0.125f)

#define ONSCREEN_DESPAWN_RANGE (120.0f)
#define MINIMAL_DISTANCE_TO_SPAWN_ONSCREEN (100.0f)
#define REQUEST_ONSCREEN_DISTANCE ((ONSCREEN_DESPAWN_RANGE + MINIMAL_DISTANCE_TO_SPAWN_ONSCREEN) / 2)
#define OFFSCREEN_DESPAWN_RANGE (40.0f)
#define EXTENDED_RANGE_DESPAWN_MULTIPLIER (1.5f)

bool CCarCtrl::bMadDriversCheat;
int CCarCtrl::NumLawEnforcerCars;
int CCarCtrl::NumAmbulancesOnDuty;
int CCarCtrl::NumFiretrucksOnDuty;
bool CCarCtrl::bCarsGeneratedAroundCamera;
float CCarCtrl::CarDensityMultiplier = 1.0f;
int32 CCarCtrl::NumMissionCars;
int32 CCarCtrl::NumRandomCars;
int32 CCarCtrl::NumParkedCars;
int32 CCarCtrl::NumPermanentCars;
int8 CCarCtrl::CountDownToCarsAtStart;
int32 CCarCtrl::MaxNumberOfCarsInUse = DEFAULT_MAX_NUMBER_OF_CARS;
uint32 CCarCtrl::LastTimeLawEnforcerCreated;
uint32 CCarCtrl::LastTimeFireTruckCreated;
uint32 CCarCtrl::LastTimeAmbulanceCreated;
int32 CCarCtrl::MiamiViceCycle;
uint32 CCarCtrl::LastTimeMiamiViceGenerated;
int32 CCarCtrl::TotalNumOfCarsOfRating[TOTAL_CUSTOM_CLASSES];
int32 CCarCtrl::CarArrays[TOTAL_CUSTOM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];
int32 CCarCtrl::NumRequestsOfCarRating[TOTAL_CUSTOM_CLASSES];
int32 CCarCtrl::NumOfLoadedCarsOfRating[TOTAL_CUSTOM_CLASSES];
int32 CCarCtrl::CarFreqArrays[TOTAL_CUSTOM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];
int32 CCarCtrl::LoadedCarsArray[TOTAL_CUSTOM_CLASSES][MAX_CAR_MODELS_IN_ARRAY];
CVehicle* apCarsToKeep[MAX_CARS_TO_KEEP];
uint32 aCarsToKeepTime[MAX_CARS_TO_KEEP];

void
CCarCtrl::GenerateRandomCars()
{
	if (CCutsceneMgr::IsRunning()) {
		CountDownToCarsAtStart = 2;
		return;
	}
	if (NumRandomCars < 30){
		if (CountDownToCarsAtStart == 0)
			GenerateOneRandomCar();
		else if (--CountDownToCarsAtStart == 0) {
			for (int i = 0; i < 100; i++)
				GenerateOneRandomCar();
			CTheCarGenerators::GenerateEvenIfPlayerIsCloseCounter = 20;
		}
	}
	/* Approximately once per 4 seconds. */
	if ((CTimer::GetTimeInMilliseconds() & 0xFFFFF000) != (CTimer::GetPreviousTimeInMilliseconds() & 0xFFFFF000))
		GenerateEmergencyServicesCar();
}

void
CCarCtrl::GenerateOneRandomCar()
{
	static int32 unk = 0;
	bool bTopDownCamera = false;
	CPlayerInfo* pPlayer = &CWorld::Players[CWorld::PlayerInFocus];
	CVector vecTargetPos = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	CVector2D vecPlayerSpeed = FindPlayerSpeed();
	CZoneInfo zone;
	CTheZones::GetZoneInfoForTimeOfDay(&vecTargetPos, &zone);
	pPlayer->m_nTrafficMultiplier = pPlayer->m_fRoadDensity * zone.carDensity;
	if (NumRandomCars >= pPlayer->m_nTrafficMultiplier * CarDensityMultiplier * CIniFile::CarNumberMultiplier)
		return;
	if (NumFiretrucksOnDuty + NumAmbulancesOnDuty + NumParkedCars + NumMissionCars + NumLawEnforcerCars + NumRandomCars >= MaxNumberOfCarsInUse)
		return;
	CWanted* pWanted = pPlayer->m_pPed->m_pWanted;
	int carClass;
	int carModel;
	if (pWanted->GetWantedLevel() > 1 && NumLawEnforcerCars < pWanted->m_MaximumLawEnforcerVehicles &&
		pWanted->m_CurrentCops < pWanted->m_MaxCops && !CGame::IsInInterior() && (
			pWanted->GetWantedLevel() > 3 ||
			pWanted->GetWantedLevel() > 2 && CTimer::GetTimeInMilliseconds() > LastTimeLawEnforcerCreated + 5000 ||
			pWanted->GetWantedLevel() > 1 && CTimer::GetTimeInMilliseconds() > LastTimeLawEnforcerCreated + 8000)) {
		/* Last pWanted->GetWantedLevel() > 1 is unnecessary but I added it for better readability. */
		/* Wouldn't be surprised it was there originally but was optimized out. */
		carClass = COPS;
		carModel = ChoosePoliceCarModel();
	}else{
		carModel = ChooseModel(&zone, &carClass);
		if (carModel == -1 || (carClass == COPS && pWanted->GetWantedLevel() >= 1))
			/* All cop spawns with wanted level are handled by condition above. */
			/* In particular it means that cop cars never spawn if player has wanted level of 1. */
			return;
	}
	float frontX, frontY;
	float preferredDistance, angleLimit;
	bool invertAngleLimitTest;
	CVector spawnPosition;
	int32 curNodeId, nextNodeId;
	float positionBetweenNodes;
	bool testForCollision;
	CVehicle* pPlayerVehicle = FindPlayerVehicle();
	CVector2D vecPlayerVehicleSpeed;
	float fPlayerVehicleSpeed;
	if (pPlayerVehicle) {
		vecPlayerVehicleSpeed = FindPlayerVehicle()->GetMoveSpeed();
		fPlayerVehicleSpeed = vecPlayerVehicleSpeed.Magnitude();
	}
	if (TheCamera.GetForward().z < -0.9f){
		/* Player uses topdown camera. */
		/* Spawn essentially anywhere. */
		frontX = frontY = 0.707f; /* 45 degrees */
		angleLimit = -1.0f;
		bTopDownCamera = true;
		invertAngleLimitTest = true;
		preferredDistance = OFFSCREEN_DESPAWN_RANGE + 15.0f;
		/* BUG: testForCollision not initialized in original game. */
		testForCollision = false;
	}else if (!pPlayerVehicle){
		/* Player is not in vehicle. */
		testForCollision = true;
		frontX = TheCamera.CamFrontXNorm;
		frontY = TheCamera.CamFrontYNorm;
		switch (CTimer::GetFrameCounter() & 1) {
		case 0:
			/* Spawn a vehicle relatively far away from player. */
			/* Forward to his current direction (camera direction). */
			angleLimit = 0.707f; /* 45 degrees */
			invertAngleLimitTest = true;
			preferredDistance = REQUEST_ONSCREEN_DISTANCE * TheCamera.GenerationDistMultiplier;
			break;
		case 1:
			/* Spawn a vehicle close to player to his side. */
			/* Kinda not within camera angle. */
			angleLimit = 0.707f; /* 45 degrees */
			invertAngleLimitTest = false;
			preferredDistance = OFFSCREEN_DESPAWN_RANGE;
			break;
		}
	}else if (fPlayerVehicleSpeed > 0.4f){ /* 72 km/h */
		/* Player is moving fast in vehicle */
		/* Prefer spawning vehicles very far away from him. */
		frontX = vecPlayerVehicleSpeed.x / fPlayerVehicleSpeed;
		frontY = vecPlayerVehicleSpeed.y / fPlayerVehicleSpeed;
		testForCollision = false;
		switch (CTimer::GetFrameCounter() & 3) {
		case 0:
		case 1:
			/* Spawn a vehicle in a very narrow gap in front of a player */
			angleLimit = 0.85f; /* approx 30 degrees */
			invertAngleLimitTest = true;
			preferredDistance = REQUEST_ONSCREEN_DISTANCE * TheCamera.GenerationDistMultiplier;
			break;
		case 2:
			/* Spawn a vehicle relatively far away from player. */
			/* Forward to his current direction (camera direction). */
			angleLimit = 0.707f; /* 45 degrees */
			invertAngleLimitTest = true;
			preferredDistance = REQUEST_ONSCREEN_DISTANCE * TheCamera.GenerationDistMultiplier;
			break;
		case 3:
			/* Spawn a vehicle close to player to his side. */
			/* Kinda not within camera angle. */
			angleLimit = 0.707f; /* 45 degrees */
			invertAngleLimitTest = false;
			preferredDistance = OFFSCREEN_DESPAWN_RANGE;
			break;
		}
	}else if (fPlayerVehicleSpeed > 0.1f){ /* 18 km/h */
		/* Player is moving moderately fast in vehicle */
		/* Spawn more vehicles to player's side. */
		frontX = vecPlayerVehicleSpeed.x / fPlayerVehicleSpeed;
		frontY = vecPlayerVehicleSpeed.y / fPlayerVehicleSpeed;
		testForCollision = false;
		switch (CTimer::GetFrameCounter() & 3) {
		case 0:
			/* Spawn a vehicle in a very narrow gap in front of a player */
			angleLimit = 0.85f; /* approx 30 degrees */
			invertAngleLimitTest = true;
			preferredDistance = REQUEST_ONSCREEN_DISTANCE * TheCamera.GenerationDistMultiplier;
			break;
		case 1:
			/* Spawn a vehicle relatively far away from player. */
			/* Forward to his current direction (camera direction). */
			angleLimit = 0.707f; /* 45 degrees */
			invertAngleLimitTest = true;
			preferredDistance = REQUEST_ONSCREEN_DISTANCE * TheCamera.GenerationDistMultiplier;
			break;
		case 2:
		case 3:
			/* Spawn a vehicle close to player to his side. */
			/* Kinda not within camera angle. */
			angleLimit = 0.707f; /* 45 degrees */
			invertAngleLimitTest = false;
			preferredDistance = OFFSCREEN_DESPAWN_RANGE;
			break;
		}
	}else{
		/* Player is in vehicle but moving very slow. */
		/* Then use camera direction instead of vehicle direction. */
		testForCollision = true;
		frontX = TheCamera.CamFrontXNorm;
		frontY = TheCamera.CamFrontYNorm;
		switch (CTimer::GetFrameCounter() & 1) {
		case 0:
			/* Spawn a vehicle relatively far away from player. */
			/* Forward to his current direction (camera direction). */
			angleLimit = 0.707f; /* 45 degrees */
			invertAngleLimitTest = true;
			preferredDistance = REQUEST_ONSCREEN_DISTANCE * TheCamera.GenerationDistMultiplier;
			break;
		case 1:
			/* Spawn a vehicle close to player to his side. */
			/* Kinda not within camera angle. */
			angleLimit = 0.707f; /* 45 degrees */
			invertAngleLimitTest = false;
			preferredDistance = OFFSCREEN_DESPAWN_RANGE;
			break;
		}
	}
	if (!ThePaths.GenerateCarCreationCoors(vecTargetPos.x, vecTargetPos.y, frontX, frontY,
		preferredDistance, angleLimit, invertAngleLimitTest, &spawnPosition, &curNodeId, &nextNodeId,
		&positionBetweenNodes, carClass == COPS && pWanted->GetWantedLevel() >= 1))
		return;
	CPathNode* pCurNode = &ThePaths.m_pathNodes[curNodeId];
	CPathNode* pNextNode = &ThePaths.m_pathNodes[nextNodeId];
	bool bBoatGenerated = false;
	if ((CGeneral::GetRandomNumber() & 0xF) > Min(pCurNode->spawnRate, pNextNode->spawnRate))
		return;
	if (pCurNode->bWaterPath) {
		bBoatGenerated = true;
		if (carClass == COPS) {
			carModel = MI_PREDATOR;
			carClass = COPS_BOAT;
			if (!CStreaming::HasModelLoaded(MI_PREDATOR)) {
				CStreaming::RequestModel(MI_PREDATOR, STREAMFLAGS_DEPENDENCY);
				return;
			}
		}
		else {
			int i;
			carModel = -1;
			for (i = 10; i > 0 && (carModel == -1 || !CStreaming::HasModelLoaded(carModel)); i--) {
				carModel = ChooseBoatModel(ChooseBoatRating(&zone));
			}
			if (i == 0)
				return;
		}
		if (pCurNode->bOnlySmallBoats || pNextNode->bOnlySmallBoats) {
			if (BoatWithTallMast(carModel))
				return;
		}
	}
	int16 colliding;
	CWorld::FindObjectsKindaColliding(spawnPosition, bBoatGenerated ? 40.0f : 10.0f, true, &colliding, 2, nil, false, true, true, false, false);
	if (colliding)
		/* If something is already present in spawn position, do not create vehicle*/
		return;
	if (!bBoatGenerated && !ThePaths.TestCoorsCloseness(vecTargetPos, false, spawnPosition))
		/* Testing if spawn position can reach target position via valid path. */
		return;
	int16 idInNode = 0;

	while (idInNode < pCurNode->numLinks &&
		ThePaths.ConnectedNode(idInNode + pCurNode->firstLink) != nextNodeId)
		idInNode++;
	int16 connectionId = ThePaths.m_carPathConnections[idInNode + pCurNode->firstLink];
	CCarPathLink* pPathLink = &ThePaths.m_carPathLinks[connectionId];
	int16 lanesOnCurrentRoad = pPathLink->pathNodeIndex == nextNodeId ? pPathLink->numLeftLanes : pPathLink->numRightLanes;
	CVehicleModelInfo* pModelInfo = (CVehicleModelInfo*)CModelInfo::GetModelInfo(carModel);
	if (lanesOnCurrentRoad == 0)
		/* Not spawning vehicle if road is one way and intended direction is opposide to that way. */
		return;
	CVehicle* pVehicle;
	if (CModelInfo::IsBoatModel(carModel))
		pVehicle = new CBoat(carModel, RANDOM_VEHICLE);
	else if (CModelInfo::IsBikeModel(carModel))
		pVehicle = new CBike(carModel, RANDOM_VEHICLE);
	else
		pVehicle = new CAutomobile(carModel, RANDOM_VEHICLE);
	pVehicle->AutoPilot.m_nPrevRouteNode = 0;
	pVehicle->AutoPilot.m_nCurrentRouteNode = curNodeId;
	pVehicle->AutoPilot.m_nNextRouteNode = nextNodeId;
	switch (carClass) {
	case COPS:
		pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		if (CWorld::Players[CWorld::PlayerInFocus].m_pPed->m_pWanted->GetWantedLevel() != 0){
			pVehicle->AutoPilot.m_nCruiseSpeed = CCarAI::FindPoliceCarSpeedForWantedLevel(pVehicle);
			pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed / 2;
			pVehicle->AutoPilot.m_nCarMission = CCarAI::FindPoliceCarMissionForWantedLevel();
			pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
		}else{
			pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(12, 16);
			pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed;
			pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_STOP_FOR_CARS;
			pVehicle->AutoPilot.m_nCarMission = MISSION_CRUISE;
		}
		if (carModel == MI_FBIRANCH){
			pVehicle->m_currentColour1 = 0;
			pVehicle->m_currentColour2 = 0;
		}
		pVehicle->bCreatedAsPoliceVehicle = true;
		break;
	case COPS_BOAT:
		pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(4, 16);
		pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed;
		pVehicle->AutoPilot.m_nCarMission = CCarAI::FindPoliceBoatMissionForWantedLevel();
		pVehicle->bCreatedAsPoliceVehicle = true;
		break;
	default:
		pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(9, 14);
		if (carClass == EXEC)
			pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(12, 18);
		else if (carClass == POOR)
			pVehicle->AutoPilot.m_nCruiseSpeed = CGeneral::GetRandomNumberInRange(7, 10);
		if (pVehicle->GetColModel()->boundingBox.max.y - pVehicle->GetColModel()->boundingBox.min.y > 10.0f || carClass == BIG) {
			pVehicle->AutoPilot.m_nCruiseSpeed *= 3;
			pVehicle->AutoPilot.m_nCruiseSpeed /= 4;
		}
		pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed;
		pVehicle->AutoPilot.m_nCarMission = MISSION_CRUISE;
		pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_STOP_FOR_CARS;
		break;
	}
	if (pVehicle && pVehicle->GetModelIndex() == MI_MRWHOOP)
		pVehicle->m_bSirenOrAlarm = true;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = connectionId;
	pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane = CGeneral::GetRandomNumber() % lanesOnCurrentRoad;
	CBox* boundingBox = &CModelInfo::GetColModel(pVehicle->GetModelIndex())->boundingBox;
	float carLength = 1.0f + (boundingBox->max.y - boundingBox->min.y) / 2;
	float distanceBetweenNodes = (pCurNode->GetPosition() - pNextNode->GetPosition()).Magnitude2D();
	/* If car is so long that it doesn't fit between two car nodes, place it directly in the middle. */
	/* Otherwise put it at least in a way that full vehicle length fits between two nodes. */
	if (distanceBetweenNodes / 2 < carLength)
		positionBetweenNodes = 0.5f;
	else
		positionBetweenNodes = Min(1.0f - carLength / distanceBetweenNodes, Max(carLength / distanceBetweenNodes, positionBetweenNodes));
	pVehicle->AutoPilot.m_nNextDirection = (curNodeId >= nextNodeId) ? 1 : -1;
	if (pCurNode->numLinks == 1){
		/* Do not create vehicle if there is nowhere to go. */
		delete pVehicle;
		return;
	}
	int16 nextConnection = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	int16 newLink;
	while (nextConnection == pVehicle->AutoPilot.m_nNextPathNodeInfo){
		newLink = CGeneral::GetRandomNumber() % pCurNode->numLinks;
		nextConnection = ThePaths.m_carPathConnections[newLink + pCurNode->firstLink];
	}
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = nextConnection;
	pVehicle->AutoPilot.m_nCurrentDirection = (ThePaths.ConnectedNode(newLink + pCurNode->firstLink) >= curNodeId) ? 1 : -1;
	CVector2D vecBetweenNodes = pNextNode->GetPosition() - pCurNode->GetPosition();
	float forwardX, forwardY;
	float distBetweenNodes = vecBetweenNodes.Magnitude();
	if (distanceBetweenNodes == 0.0f){
		forwardX = 1.0f;
		forwardY = 0.0f;
	}else{
		forwardX = vecBetweenNodes.x / distBetweenNodes;
		forwardY = vecBetweenNodes.y / distBetweenNodes;
	}
	/* I think the following might be some form of SetRotateZOnly. */
	/* Setting up direction between two car nodes. */
	pVehicle->GetForward() = CVector(forwardX, forwardY, 0.0f);
	pVehicle->GetRight() = CVector(forwardY, -forwardX, 0.0f);
	pVehicle->GetUp() = CVector(0.0f, 0.0f, 1.0f);

	float currentPathLinkForwardX = pVehicle->AutoPilot.m_nCurrentDirection * ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo].GetDirX();
	float currentPathLinkForwardY = pVehicle->AutoPilot.m_nCurrentDirection * ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo].GetDirY();
	float nextPathLinkForwardX = pVehicle->AutoPilot.m_nNextDirection * ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo].GetDirX();
	float nextPathLinkForwardY = pVehicle->AutoPilot.m_nNextDirection * ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo].GetDirY();

#ifdef FIX_BUGS
	CCarPathLink* pCurrentLink;
	CCarPathLink* pNextLink;
	CVector positionOnCurrentLinkIncludingLane;
	CVector positionOnNextLinkIncludingLane;
	float directionCurrentLinkX;
	float directionCurrentLinkY;
	float directionNextLinkX;
	float directionNextLinkY;
	if (positionBetweenNodes < 0.5f) {
		pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
		pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
		positionOnCurrentLinkIncludingLane = CVector(
			pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
			pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
			0.0f);
		positionOnNextLinkIncludingLane = CVector(
			pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
			pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
			0.0f);
		directionCurrentLinkX = pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
		directionCurrentLinkY = pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
		directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
		directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
		/* We want to make a path between two links that may not have the same forward directions a curve. */
		pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = CCurves::CalcSpeedScaleFactor(
			&positionOnCurrentLinkIncludingLane,
			&positionOnNextLinkIncludingLane,
			directionCurrentLinkX, directionCurrentLinkY,
			directionNextLinkX, directionNextLinkY
		) * (1000.0f / pVehicle->AutoPilot.m_fMaxTrafficSpeed);
		pVehicle->AutoPilot.m_nTimeEnteredCurve = CTimer::GetTimeInMilliseconds() -
			(uint32)((0.5f + positionBetweenNodes) * pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve);
	}
	else {
		PickNextNodeRandomly(pVehicle);
		pVehicle->AutoPilot.m_nTimeEnteredCurve = CTimer::GetTimeInMilliseconds() -
			(uint32)((positionBetweenNodes - 0.5f) * pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve);

		pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
		pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
		positionOnCurrentLinkIncludingLane = CVector(
			pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
			pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
			0.0f);
		positionOnNextLinkIncludingLane = CVector(
			pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
			pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
			0.0f);
		directionCurrentLinkX = pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
		directionCurrentLinkY = pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
		directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
		directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	}
#else

	CCarPathLink* pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink* pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector positionOnCurrentLinkIncludingLane(
		pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
		pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
		0.0f);
	CVector positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
		0.0f);
	float directionCurrentLinkX = pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionCurrentLinkY = pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	/* We want to make a path between two links that may not have the same forward directions a curve. */
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = CCurves::CalcSpeedScaleFactor(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		directionCurrentLinkX, directionCurrentLinkY,
		directionNextLinkX, directionNextLinkY
	) * (1000.0f / pVehicle->AutoPilot.m_fMaxTrafficSpeed);
	pVehicle->AutoPilot.m_nTimeEnteredCurve = CTimer::GetTimeInMilliseconds() -
		(0.5f + positionBetweenNodes) * pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
#endif

	CVector directionCurrentLink(directionCurrentLinkX, directionCurrentLinkY, 0.0f);
	CVector directionNextLink(directionNextLinkX, directionNextLinkY, 0.0f);
	CVector positionIncludingCurve;
	CVector directionIncludingCurve;
	CCurves::CalcCurvePoint(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		&directionCurrentLink,
		&directionNextLink,
		GetPositionAlongCurrentCurve(pVehicle),
		pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve,
		&positionIncludingCurve,
		&directionIncludingCurve
	);
	CVector vectorBetweenNodes = pCurNode->GetPosition() - pNextNode->GetPosition();
	CVector finalPosition = positionIncludingCurve + vectorBetweenNodes * 2.0f / vectorBetweenNodes.Magnitude();
	finalPosition.z = positionBetweenNodes * pNextNode->GetZ() +
		(1.0f - positionBetweenNodes) * pCurNode->GetZ();
	float groundZ = INFINITE_Z;
	CColPoint colPoint;
	CEntity* pEntity;
	if (bBoatGenerated) {
		if (!CWaterLevel::GetWaterLevel(finalPosition, &groundZ, true)) {
			delete pVehicle;
			return;
		}
	}
	else {
		if (CWorld::ProcessVerticalLine(finalPosition, 1000.0f, colPoint, pEntity, true, false, false, false, true, false, nil))
			groundZ = colPoint.point.z;
		if (CWorld::ProcessVerticalLine(finalPosition, -1000.0f, colPoint, pEntity, true, false, false, false, true, false, nil)) {
			if (ABS(colPoint.point.z - finalPosition.z) < ABS(groundZ - finalPosition.z))
				groundZ = colPoint.point.z;
		}
	}
	if (groundZ == INFINITE_Z || ABS(groundZ - finalPosition.z) > 7.0f) {
		/* Failed to find ground or too far from expected position. */
		delete pVehicle;
		return;
	}
	if (CModelInfo::IsBoatModel(carModel)) {
		finalPosition.z = groundZ;
		pVehicle->bExtendedRange = true;
	}
	else
		finalPosition.z = groundZ + pVehicle->GetHeightAboveRoad();
	pVehicle->SetPosition(finalPosition);
	pVehicle->SetMoveSpeed(directionIncludingCurve / GAME_SPEED_TO_CARAI_SPEED);
	CVector2D speedDifferenceWithTarget = (CVector2D)pVehicle->GetMoveSpeed() - vecPlayerSpeed;
	CVector2D distanceToTarget = positionIncludingCurve - vecTargetPos;
	switch (carClass) {
	case COPS:
		pVehicle->SetStatus((pVehicle->AutoPilot.m_nCarMission == MISSION_CRUISE) ? STATUS_SIMPLE : STATUS_PHYSICS);
		pVehicle->ChangeLawEnforcerState(1);
		break;
	case COPS_BOAT:
		pVehicle->ChangeLawEnforcerState(1);
		pVehicle->SetStatus(STATUS_PHYSICS);
		break;
	default:
		bBoatGenerated ? pVehicle->SetStatus(STATUS_PHYSICS) : pVehicle->SetStatus(STATUS_SIMPLE);
		break;
	}
	CVisibilityPlugins::SetClumpAlpha(pVehicle->GetClump(), 0);
	if (!pVehicle->GetIsOnScreen()){
		if ((vecTargetPos - pVehicle->GetPosition()).Magnitude2D() > OFFSCREEN_DESPAWN_RANGE * (pVehicle->bExtendedRange ? EXTENDED_RANGE_DESPAWN_MULTIPLIER : 1.0f)) {
			/* Too far away cars that are not visible aren't needed. */
			delete pVehicle;
			return;
		}
	}else{
		if ((vecTargetPos - pVehicle->GetPosition()).Magnitude2D() > TheCamera.GenerationDistMultiplier * (pVehicle->bExtendedRange ? EXTENDED_RANGE_DESPAWN_MULTIPLIER : 1.0f) * ONSCREEN_DESPAWN_RANGE ||
			(vecTargetPos - pVehicle->GetPosition()).Magnitude2D() < TheCamera.GenerationDistMultiplier * MINIMAL_DISTANCE_TO_SPAWN_ONSCREEN) {
			delete pVehicle;
			return;
		}
		if ((TheCamera.GetPosition() - pVehicle->GetPosition()).Magnitude2D() < 82.5f * TheCamera.GenerationDistMultiplier || bTopDownCamera) {
			delete pVehicle;
			return;
		}
		if (pVehicle->GetModelIndex() == MI_MARQUIS) { // so marquis can only spawn if player doesn't see it?
			delete pVehicle;
			return;
		}
	}
	CVehicleModelInfo* pVehicleModel = pVehicle->GetModelInfo();
	float radiusToTest = pVehicleModel->GetColModel()->boundingSphere.radius;
	if (testForCollision){
		CWorld::FindObjectsKindaColliding(pVehicle->GetPosition(), radiusToTest + 20.0f, true, &colliding, 2, nil, false, true, false, false, false);
		if (colliding){
			delete pVehicle;
			return;
		}
	}
	CWorld::FindObjectsKindaColliding(pVehicle->GetPosition(), radiusToTest, true, &colliding, 2, nil, false, true, false, false, false);
	if (colliding){
		delete pVehicle;
		return;
	}
	if (speedDifferenceWithTarget.x * distanceToTarget.x +
		speedDifferenceWithTarget.y * distanceToTarget.y >= 0.0f){
		delete pVehicle;
		return;
	}
	pVehicleModel->AvoidSameVehicleColour(&pVehicle->m_currentColour1, &pVehicle->m_currentColour2);
	CWorld::Add(pVehicle);
	if (carClass == COPS || carClass == COPS_BOAT)
		CCarAI::AddPoliceCarOccupants(pVehicle);
	else {
		pVehicle->SetUpDriver();
		int32 passengers = 0;
		for (int i = 0; i < pVehicle->m_nNumMaxPassengers; i++)
			passengers += (CGeneral::GetRandomNumberInRange(0.0f, 1.0f) < PROBABILITY_OF_PASSENGER_IN_VEHICLE) ? 1 : 0;
		if (CModelInfo::IsCarModel(carModel) && (CModelInfo::GetModelInfo(carModel)->GetAnimFileIndex() == CAnimManager::GetAnimationBlockIndex("van") && passengers >= 1))
			passengers = 1;
		for (int i = 0; i < passengers; i++) {
			CPed* pPassenger = pVehicle->SetupPassenger(i);
			if (pPassenger) {
				++CPopulation::ms_nTotalCarPassengerPeds;
				pPassenger->bCarPassenger = true;
			}
		}
	}
	int nMadDrivers;
	switch (pVehicle->GetVehicleAppearance()) {
	case VEHICLE_APPEARANCE_BIKE:
		nMadDrivers = 30;
		break;
	case VEHICLE_APPEARANCE_BOAT:
		nMadDrivers = 40;
		break;
	default:
		nMadDrivers = 6;
		break;
	}
	if ((CGeneral::GetRandomNumber() & 0x7F) < nMadDrivers || bMadDriversCheat) {
		pVehicle->SetStatus(STATUS_PHYSICS);
		pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
		pVehicle->AutoPilot.m_nCruiseSpeed += 10;
	}
	if (carClass == COPS)
		LastTimeLawEnforcerCreated = CTimer::GetTimeInMilliseconds();
	if (pVehicle->GetModelIndex() == MI_CADDY) {
		pVehicle->SetStatus(STATUS_PHYSICS);
		pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
	}
	if (carClass == COPS && pVehicle->GetModelIndex() == MI_VICECHEE) {
		CVehicleModelInfo* pVehicleModel = (CVehicleModelInfo*)CModelInfo::GetModelInfo(MI_VICECHEE);
		switch (MiamiViceCycle) {
		case 0:
			pVehicleModel->SetVehicleColour(53, 77);
			break;
		case 1:
			pVehicleModel->SetVehicleColour(15, 77);
			break;
		case 2:
			pVehicleModel->SetVehicleColour(41, 77);
			break;
		case 3:
			pVehicleModel->SetVehicleColour(61, 77);
			break;
		default:
			break;
		}
	}
	if (CGeneral::GetRandomNumberInRange(0.0f, 1.0f) >= (1 - PROBABILITY_OF_DEAD_PED_ACCIDENT)) {
		if (CModelInfo::IsCarModel(pVehicle->GetModelIndex()) && !pVehicle->bIsLawEnforcer) {
			if (CPopulation::AddDeadPedInFrontOfCar(pVehicle->GetPosition() + pVehicle->GetForward() * DISTANCE_BETWEEN_CAR_AND_DEAD_PED, pVehicle)) {
				pVehicle->AutoPilot.m_nCruiseSpeed = 0;
				pVehicle->SetMoveSpeed(0.0f, 0.0f, 0.0f);
				for (int i = 0; i < pVehicle->m_nNumPassengers; i++) {
					if (pVehicle->pPassengers[i]) {
						pVehicle->pPassengers[i]->SetObjective(OBJECTIVE_LEAVE_CAR, pVehicle);
						pVehicle->pPassengers[i]->m_nLastPedState = PED_WANDER_PATH;
						pVehicle->pPassengers[i]->m_vehicleInAccident = pVehicle;
						pVehicle->pPassengers[i]->bDeadPedInFrontOfCar = true;
						pVehicle->RegisterReference((CEntity**)&pVehicle->pPassengers[i]->m_vehicleInAccident);
					}
				}
				if (pVehicle->pDriver) {
					pVehicle->pDriver->SetObjective(OBJECTIVE_LEAVE_CAR, pVehicle);
					pVehicle->pDriver->m_nLastPedState = PED_WANDER_PATH;
					pVehicle->pDriver->m_vehicleInAccident = pVehicle;
					pVehicle->pDriver->bDeadPedInFrontOfCar = true;
					pVehicle->RegisterReference((CEntity**)&pVehicle->pDriver->m_vehicleInAccident);
				}
			}
		}
	}
}

bool
CCarCtrl::BoatWithTallMast(int32 mi)
{
	return mi == MI_RIO || mi == MI_TROPIC || mi == MI_MARQUIS;
}

int32
CCarCtrl::ChooseBoatModel(int32 rating)
{
	++NumRequestsOfCarRating[rating];
	return ChooseCarModel(rating);
}

int32
CCarCtrl::ChooseBoatRating(CZoneInfo* pZoneInfo)
{
	int rnd = CGeneral::GetRandomNumberInRange(0, 1000);
	for (int i = 0; i < NUM_BOAT_CLASSES - 1; i++) {
		if (rnd < pZoneInfo->boatThreshold[i])
			return FIRST_BOAT_RATING + i;
	}
	return FIRST_BOAT_RATING + NUM_BOAT_CLASSES - 1;
}

int32
CCarCtrl::ChooseCarRating(CZoneInfo* pZoneInfo)
{
	int rnd = CGeneral::GetRandomNumberInRange(0, 1000);
	for (int i = 0; i < NUM_CAR_CLASSES - 1; i++) {
		if (rnd < pZoneInfo->carThreshold[i])
			return i;
	}
	return FIRST_CAR_RATING + NUM_CAR_CLASSES - 1;
}

int32
CCarCtrl::ChooseModel(CZoneInfo* pZone, int* pClass) {
	int32 model = -1;
	int32 i;
	for (i = 10; i > 0 && (model == -1 || !CStreaming::HasModelLoaded(model)); i--) {
		int rnd = CGeneral::GetRandomNumberInRange(0, 1000);

		if (rnd < pZone->copThreshold) {
			*pClass = COPS; 
			model = ChoosePoliceCarModel();
			continue;
		}

		int32 j;
		for (j = 0; j < NUM_GANG_CAR_CLASSES; j++) {
			if (rnd < pZone->gangThreshold[j]) {
				*pClass = j + FIRST_GANG_CAR_RATING;
				model = ChooseGangCarModel(j);
				break;
			}
		}

		if (j != NUM_GANG_CAR_CLASSES)
			continue;

		*pClass = ChooseCarRating(pZone);
		model = ChooseCarModel(*pClass);
	}
	if (i == 0)
		return -1;
	return model;
}

int32
CCarCtrl::ChooseCarModel(int32 vehclass)
{
	int32 model = -1;
	++NumRequestsOfCarRating[vehclass];
	if (NumOfLoadedCarsOfRating[vehclass] == 0)
		return -1;
	int32 rnd = CGeneral::GetRandomNumberInRange(0, CarFreqArrays[vehclass][NumOfLoadedCarsOfRating[vehclass] - 1]);
	int32 index = 0;
	while (rnd > CarFreqArrays[vehclass][index])
		index++;
	assert(LoadedCarsArray[vehclass][index]);
	return LoadedCarsArray[vehclass][index];
}

void
CCarCtrl::AddToLoadedVehicleArray(int32 mi, int32 rating, int32 freq)
{
	LoadedCarsArray[rating][NumOfLoadedCarsOfRating[rating]] = mi;
	assert(mi >= 130);
	CarFreqArrays[rating][NumOfLoadedCarsOfRating[rating]] = freq;
	if (NumOfLoadedCarsOfRating[rating])
		CarFreqArrays[rating][NumOfLoadedCarsOfRating[rating]] += CarFreqArrays[rating][NumOfLoadedCarsOfRating[rating] - 1];
	NumOfLoadedCarsOfRating[rating]++;
}

void
CCarCtrl::RemoveFromLoadedVehicleArray(int mi, int32 rating)
{
	int index = 0;
	while (LoadedCarsArray[rating][index] != -1) {
		if (LoadedCarsArray[rating][index] == mi)
			break;
		index++;
	}
	assert(LoadedCarsArray[rating][index] == mi);
	int32 freq = CarFreqArrays[rating][index];
	if (index > 0)
		freq -= CarFreqArrays[rating][index - 1];
	while (LoadedCarsArray[rating][index + 1] != -1) {
		LoadedCarsArray[rating][index] = LoadedCarsArray[rating][index + 1];
		CarFreqArrays[rating][index] = CarFreqArrays[rating][index + 1] - freq;
		index++;
	}
	--NumOfLoadedCarsOfRating[rating];
}

int32
CCarCtrl::ChooseCarModelToLoad(int rating)
{
	return CarArrays[rating][CGeneral::GetRandomNumberInRange(0, TotalNumOfCarsOfRating[rating])];
}

int32
CCarCtrl::ChoosePoliceCarModel(void)
{
	if (FindPlayerPed()->m_pWanted->AreMiamiViceRequired() &&
#ifdef FIX_BUGS
		(CTimer::GetTimeInMilliseconds() > LastTimeMiamiViceGenerated + 120000 || LastTimeMiamiViceGenerated == 0) &&
#else
		CTimer::GetTimeInMilliseconds() > LastTimeMiamiViceGenerated + 120000 &&
#endif
		CStreaming::HasModelLoaded(MI_VICECHEE)) {
		switch (MiamiViceCycle) {
		case 0:
			if (CStreaming::HasModelLoaded(MI_VICE1) && CStreaming::HasModelLoaded(MI_VICE2))
				return MI_VICECHEE;
			break;
		case 1:
			if (CStreaming::HasModelLoaded(MI_VICE3) && CStreaming::HasModelLoaded(MI_VICE4))
				return MI_VICECHEE;
			break;
		case 2:
			if (CStreaming::HasModelLoaded(MI_VICE5) && CStreaming::HasModelLoaded(MI_VICE6))
				return MI_VICECHEE;
			break;
		case 3:
			if (CStreaming::HasModelLoaded(MI_VICE7) && CStreaming::HasModelLoaded(MI_VICE8))
				return MI_VICECHEE;
			break;
		default:
			break;
		}
	}
	if (FindPlayerPed()->m_pWanted->AreSwatRequired() &&
		CStreaming::HasModelLoaded(MI_ENFORCER) &&
		CStreaming::HasModelLoaded(MI_POLICE))
		return ((CGeneral::GetRandomNumber() & 0xF) == 0) ? MI_ENFORCER : MI_POLICE;
	if (FindPlayerPed()->m_pWanted->AreFbiRequired() &&
		CStreaming::HasModelLoaded(MI_FBIRANCH) &&
		CStreaming::HasModelLoaded(MI_FBI))
		return MI_FBIRANCH;
	if (FindPlayerPed()->m_pWanted->AreArmyRequired() &&
		CStreaming::HasModelLoaded(MI_RHINO) &&
		CStreaming::HasModelLoaded(MI_BARRACKS) &&
		CStreaming::HasModelLoaded(MI_ARMY))
		return CGeneral::GetRandomTrueFalse() ? MI_BARRACKS : MI_RHINO;
	return MI_POLICE;
}

int32
CCarCtrl::ChooseGangCarModel(int32 gang)
{
	if (CGangs::HaveGangModelsLoaded(gang))
		return CGangs::GetGangVehicleModel(gang);
	return -1;
}

void
CCarCtrl::AddToCarArray(int32 id, int32 vehclass)
{
	assert(TotalNumOfCarsOfRating[vehclass] < MAX_CAR_MODELS_IN_ARRAY);
	CarArrays[vehclass][TotalNumOfCarsOfRating[vehclass]++] = id;
}

void
CCarCtrl::RemoveDistantCars()
{
	for (int i = CPools::GetVehiclePool()->GetSize()-1; i >= 0; i--) {
		CVehicle* pVehicle = CPools::GetVehiclePool()->GetSlot(i);
		if (!pVehicle)
			continue;
		PossiblyRemoveVehicle(pVehicle);
		if (pVehicle->bCreateRoadBlockPeds){
			if ((pVehicle->GetPosition() - FindPlayerCentreOfWorld(CWorld::PlayerInFocus)).Magnitude2D() < DISTANCE_TO_SPAWN_ROADBLOCK_PEDS) {
				CRoadBlocks::GenerateRoadBlockCopsForCar(pVehicle, pVehicle->m_nRoadblockType);
				pVehicle->bCreateRoadBlockPeds = false;
			}
		}
	}
}

void
CCarCtrl::RemoveCarsIfThePoolGetsFull(void)
{
	if ((CTimer::GetFrameCounter() & 7) != 3)
		return;
	if (CPools::GetVehiclePool()->GetNoOfFreeSpaces() >= 8)
		return;
	int i = CPools::GetVehiclePool()->GetSize();
	float md = 10000000.f;
	CVehicle* pClosestVehicle = nil;
	while (i--) {
		CVehicle* pVehicle = CPools::GetVehiclePool()->GetSlot(i);
		if (!pVehicle)
			continue;
		if (IsThisVehicleInteresting(pVehicle) || pVehicle->bIsLocked)
			continue;
		if (!pVehicle->CanBeDeleted() || CCranes::IsThisCarBeingTargettedByAnyCrane(pVehicle))
			continue;
		float distance = (TheCamera.GetPosition() - pVehicle->GetPosition()).Magnitude();
		if (distance < md) {
			md = distance;
			pClosestVehicle = pVehicle;
		}
	}
	if (pClosestVehicle) {
		CWorld::Remove(pClosestVehicle);
		delete pClosestVehicle;
	}
}

void
CCarCtrl::PossiblyRemoveVehicle(CVehicle* pVehicle)
{
#ifdef FIX_BUGS
	if (pVehicle->bIsLocked)
		return;
#endif
	CVector vecPlayerPos = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	/* BUG: this variable is initialized only in if-block below but can be used outside of it. */
	if (!IsThisVehicleInteresting(pVehicle) && !pVehicle->bIsLocked &&
		pVehicle->CanBeDeleted() && !CCranes::IsThisCarBeingTargettedByAnyCrane(pVehicle)){
		if (pVehicle->bFadeOut && CVisibilityPlugins::GetClumpAlpha(pVehicle->GetClump()) == 0){
			CWorld::Remove(pVehicle);
			delete pVehicle;
			return;
		}
		float distanceToPlayer = (pVehicle->GetPosition() - vecPlayerPos).Magnitude2D();
		float threshold = OFFSCREEN_DESPAWN_RANGE;
#ifndef EXTENDED_OFFSCREEN_DESPAWN_RANGE
		if (pVehicle->GetIsOnScreen() ||
			TheCamera.Cams[TheCamera.ActiveCam].LookingLeft ||
			TheCamera.Cams[TheCamera.ActiveCam].LookingRight ||
			TheCamera.Cams[TheCamera.ActiveCam].LookingBehind ||
			TheCamera.GetLookDirection() == 0 ||
			pVehicle->VehicleCreatedBy == PARKED_VEHICLE ||
			pVehicle->GetModelIndex() == MI_AMBULAN ||
			pVehicle->GetModelIndex() == MI_FIRETRUCK ||
			pVehicle->bIsLawEnforcer ||
			pVehicle->bIsCarParkVehicle ||
			CTimer::GetTimeInMilliseconds() < pVehicle->m_nSetPieceExtendedRangeTime
			)
#endif
		{
			threshold = ONSCREEN_DESPAWN_RANGE * TheCamera.GenerationDistMultiplier;
		}
#ifndef EXTENDED_OFFSCREEN_DESPAWN_RANGE
		if (TheCamera.GetForward().z < -0.9f)
			threshold = 70.0f;
#endif
		if (pVehicle->bExtendedRange)
			threshold *= EXTENDED_RANGE_DESPAWN_MULTIPLIER;
		if (distanceToPlayer > threshold && !CGarages::IsPointWithinHideOutGarage(pVehicle->GetPosition())){
			if (pVehicle->GetIsOnScreen()){
				pVehicle->bFadeOut = true;
			}else{
				CWorld::Remove(pVehicle);
				delete pVehicle;
			}
			return;
		}
	}
	if ((pVehicle->GetStatus() == STATUS_SIMPLE || pVehicle->GetStatus() == STATUS_PHYSICS &&
		(pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS || pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS_IGNORE_LIGHTS)) &&
		CTimer::GetTimeInMilliseconds() - pVehicle->AutoPilot.m_nTimeToStartMission > 5000 &&
		!pVehicle->GetIsOnScreen() &&
		(pVehicle->GetPosition() - vecPlayerPos).Magnitude2D() > 22.0f &&
		!IsThisVehicleInteresting(pVehicle) &&
		!pVehicle->bIsLocked &&
		pVehicle->CanBeDeleted() &&
		!CTrafficLights::ShouldCarStopForLight(pVehicle, true) &&
		!CTrafficLights::ShouldCarStopForBridge(pVehicle) &&
		!CGarages::IsPointWithinHideOutGarage(pVehicle->GetPosition())){
		CWorld::Remove(pVehicle);
		delete pVehicle;
		return;
	}
	if (pVehicle->GetStatus() == STATUS_WRECKED) {
		if (pVehicle->m_nTimeOfDeath != 0) {
			if (CTimer::GetTimeInMilliseconds() > pVehicle->m_nTimeOfDeath + 60000 &&
				CTimer::GetTimeInMilliseconds() > pVehicle->m_nSetPieceExtendedRangeTime &&
				!(pVehicle->GetIsOnScreen())) {
				if ((pVehicle->GetPosition() - vecPlayerPos).MagnitudeSqr() > SQR(6.5f)) {
					if (!CGarages::IsPointWithinHideOutGarage(pVehicle->GetPosition())) {
						CWorld::Remove(pVehicle);
						delete pVehicle;
					}
				}
			}
		}
	}
}

int32
CCarCtrl::CountCarsOfType(int32 mi)
{
	int32 total = 0;
	for (int i = CPools::GetVehiclePool()->GetSize()-1; i >= 0; i--) {
		CVehicle* pVehicle = CPools::GetVehiclePool()->GetSlot(i);
		if (!pVehicle)
			continue;
		if (pVehicle->GetModelIndex() == mi)
			total++;
	}
	return total;
}

static CVector GetRandomOffsetForVehicle(CVehicle* pVehicle, bool bNext)
{
	CVector offset;
	int32 seed = ((bNext ? pVehicle->AutoPilot.m_nNextPathNodeInfo : pVehicle->AutoPilot.m_nCurrentPathNodeInfo) + pVehicle->m_randomSeed) & 7;
	offset.x = (seed - 3) * 0.009f;
	offset.y = ((seed >> 3) - 3) * 0.009f;
	offset.z = 0.0f;
	return offset;
}

void
CCarCtrl::UpdateCarOnRails(CVehicle* pVehicle)
{
	if (pVehicle->AutoPilot.m_nTempAction == TEMPACT_WAIT){
		pVehicle->SetMoveSpeed(0.0f, 0.0f, 0.0f);
		pVehicle->AutoPilot.ModifySpeed(0.0f);
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTempAction){
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
			pVehicle->AutoPilot.m_nAntiReverseTimer = CTimer::GetTimeInMilliseconds();
			pVehicle->AutoPilot.m_nTimeToStartMission = CTimer::GetTimeInMilliseconds();
		}
		return;
	}
	SlowCarOnRailsDownForTrafficAndLights(pVehicle);
	if (pVehicle->AutoPilot.m_nTimeEnteredCurve + pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve <= CTimer::GetTimeInMilliseconds())
		PickNextNodeAccordingStrategy(pVehicle);
	if (pVehicle->GetStatus() == STATUS_PHYSICS)
		return;
	CCarPathLink* pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink* pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	float currentPathLinkForwardX = pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
	float currentPathLinkForwardY = pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
	float nextPathLinkForwardX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float nextPathLinkForwardY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	CVector positionOnCurrentLinkIncludingLane(
		pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
		pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
		0.0f);
	CVector positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
		0.0f);
	CVector directionCurrentLink = GetRandomOffsetForVehicle(pVehicle, false);
	directionCurrentLink += CVector(currentPathLinkForwardX, currentPathLinkForwardY, 0.0f);
	directionCurrentLink.Normalise();
	CVector directionNextLink = GetRandomOffsetForVehicle(pVehicle, true);
	directionNextLink += CVector(nextPathLinkForwardX, nextPathLinkForwardY, 0.0f);
	directionNextLink.Normalise();
	CVector positionIncludingCurve;
	CVector directionIncludingCurve;
	CCurves::CalcCurvePoint(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		&directionCurrentLink,
		&directionNextLink,
		GetPositionAlongCurrentCurve(pVehicle),
		pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve,
		&positionIncludingCurve,
		&directionIncludingCurve
	);
	positionIncludingCurve.z = 15.0f;
	DragCarToPoint(pVehicle, &positionIncludingCurve);
	pVehicle->SetMoveSpeed(directionIncludingCurve / GAME_SPEED_TO_CARAI_SPEED);
}

float
CCarCtrl::FindMaximumSpeedForThisCarInTraffic(CVehicle* pVehicle)
{
	if (pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_AVOID_CARS ||
		pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_PLOUGH_THROUGH)
		return pVehicle->AutoPilot.GetCruiseSpeed();
	float left = pVehicle->GetPosition().x - DISTANCE_TO_SCAN_FOR_DANGER;
	float right = pVehicle->GetPosition().x + DISTANCE_TO_SCAN_FOR_DANGER;
	float top = pVehicle->GetPosition().y - DISTANCE_TO_SCAN_FOR_DANGER;
	float bottom = pVehicle->GetPosition().y + DISTANCE_TO_SCAN_FOR_DANGER;
	int xstart = Max(0, CWorld::GetSectorIndexX(left));
	int xend = Min(NUMSECTORS_X - 1, CWorld::GetSectorIndexX(right));
	int ystart = Max(0, CWorld::GetSectorIndexY(top));
	int yend = Min(NUMSECTORS_Y - 1, CWorld::GetSectorIndexY(bottom));
#if REAL_GAMECUBE
	if(xstart > xend || ystart > yend){
		if(!gLoggedBadTrafficScan){
			const CVector &pos = pVehicle->GetPosition();
			printf("[VEH-GUARD] bad traffic scan: veh=%p model=%d pos=(%f,%f,%f) rect=(%f,%f,%f,%f) sectors=(%d,%d,%d,%d)\n",
			       pVehicle, pVehicle ? pVehicle->GetModelIndex() : -1,
			       pos.x, pos.y, pos.z, left, top, right, bottom,
			       xstart, xend, ystart, yend);
			gLoggedBadTrafficScan = true;
		}
		return pVehicle->AutoPilot.GetCruiseSpeed();
	}
#endif
	assert(xstart <= xend);
	assert(ystart <= yend);

	float maxSpeed = pVehicle->AutoPilot.GetCruiseSpeed();

	CWorld::AdvanceCurrentScanCode();

	for (int y = ystart; y <= yend; y++){
		for (int x = xstart; x <= xend; x++){
			CSector* s = CWorld::GetSector(x, y);
			SlowCarDownForCarsSectorList(s->m_lists[ENTITYLIST_VEHICLES], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.GetCruiseSpeed());
			SlowCarDownForCarsSectorList(s->m_lists[ENTITYLIST_VEHICLES_OVERLAP], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.GetCruiseSpeed());
			SlowCarDownForPedsSectorList(s->m_lists[ENTITYLIST_PEDS], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.GetCruiseSpeed());
			SlowCarDownForPedsSectorList(s->m_lists[ENTITYLIST_PEDS_OVERLAP], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.GetCruiseSpeed());
		}
	}
	pVehicle->bWarnedPeds = true;
	if (pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS || pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS_IGNORE_LIGHTS)
		return maxSpeed;
	return (maxSpeed + pVehicle->AutoPilot.GetCruiseSpeed()) / 2;
}

void
CCarCtrl::ScanForPedDanger(CVehicle* pVehicle)
{
	bool storedSlowDownFlag = pVehicle->AutoPilot.m_bSlowedDownBecauseOfPeds;
	float left = pVehicle->GetPosition().x - DISTANCE_TO_SCAN_FOR_PED_DANGER;
	float right = pVehicle->GetPosition().x + DISTANCE_TO_SCAN_FOR_PED_DANGER;
	float top = pVehicle->GetPosition().y - DISTANCE_TO_SCAN_FOR_PED_DANGER;
	float bottom = pVehicle->GetPosition().y + DISTANCE_TO_SCAN_FOR_PED_DANGER;
	int xstart = Max(0, CWorld::GetSectorIndexX(left));
	int xend = Min(NUMSECTORS_X - 1, CWorld::GetSectorIndexX(right));
	int ystart = Max(0, CWorld::GetSectorIndexY(top));
	int yend = Min(NUMSECTORS_Y - 1, CWorld::GetSectorIndexY(bottom));
#if REAL_GAMECUBE
	if(xstart > xend || ystart > yend){
		if(!gLoggedBadPedDangerScan){
			const CVector &pos = pVehicle->GetPosition();
			printf("[VEH-GUARD] bad ped scan: veh=%p model=%d pos=(%f,%f,%f) rect=(%f,%f,%f,%f) sectors=(%d,%d,%d,%d)\n",
			       pVehicle, pVehicle ? pVehicle->GetModelIndex() : -1,
			       pos.x, pos.y, pos.z, left, top, right, bottom,
			       xstart, xend, ystart, yend);
			gLoggedBadPedDangerScan = true;
		}
		return;
	}
#endif
	assert(xstart <= xend);
	assert(ystart <= yend);

	float maxSpeed = pVehicle->AutoPilot.m_nCruiseSpeed;

	CWorld::AdvanceCurrentScanCode();

	for (int y = ystart; y <= yend; y++) {
		for (int x = xstart; x <= xend; x++) {
			CSector* s = CWorld::GetSector(x, y);
			SlowCarDownForPedsSectorList(s->m_lists[ENTITYLIST_PEDS], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.m_nCruiseSpeed);
			SlowCarDownForPedsSectorList(s->m_lists[ENTITYLIST_PEDS_OVERLAP], pVehicle, left, top, right, bottom, &maxSpeed, pVehicle->AutoPilot.m_nCruiseSpeed);
		}
	}
	pVehicle->bWarnedPeds = true;
	pVehicle->AutoPilot.m_bSlowedDownBecauseOfPeds = storedSlowDownFlag;
}

void
CCarCtrl::SlowCarOnRailsDownForTrafficAndLights(CVehicle* pVehicle)
{
	float maxSpeed;
	if (CTrafficLights::ShouldCarStopForLight(pVehicle, false) || CTrafficLights::ShouldCarStopForBridge(pVehicle)){
		CCarAI::CarHasReasonToStop(pVehicle);
		maxSpeed = 0.0f;
	}else{
		maxSpeed = FindMaximumSpeedForThisCarInTraffic(pVehicle);
	}
	float curSpeed = pVehicle->AutoPilot.m_fMaxTrafficSpeed;
	if (maxSpeed >= curSpeed){
		if (maxSpeed > curSpeed)
			pVehicle->AutoPilot.ModifySpeed(Min(maxSpeed, curSpeed + 0.05f * CTimer::GetTimeStep()));
	}else if (curSpeed != 0.0f) {
		if (curSpeed < 0.1f)
			pVehicle->AutoPilot.ModifySpeed(0.0f);
		else
			pVehicle->AutoPilot.ModifySpeed(Max(maxSpeed, curSpeed - 0.7f * CTimer::GetTimeStep()));
	}
}

void CCarCtrl::SlowCarDownForPedsSectorList(CPtrList& lst, CVehicle* pVehicle, float x_inf, float y_inf, float x_sup, float y_sup, float* pSpeed, float curSpeed)
{
	float frontOffset = pVehicle->GetModelInfo()->GetColModel()->boundingBox.max.y;
	float frontSafe = frontOffset + SAFE_DISTANCE_TO_PED;
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next){
		CPed* pPed = (CPed*)pNode->item;
		if (pPed->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pPed->bUsesCollision)
			continue;
		pPed->m_scanCode = CWorld::GetCurrentScanCode();
		CVector vecPedPos = pPed->GetPosition();
		if (vecPedPos.x < x_inf || vecPedPos.x > x_sup)
			continue;
		if (vecPedPos.y < y_inf || vecPedPos.y > y_sup)
			continue;
		if (ABS(vecPedPos.z - pVehicle->GetPosition().z) >= 4.0f)
			continue;
		CVector vecToPed = vecPedPos - pVehicle->GetPosition();
		float dotDirection = DotProduct(pVehicle->GetForward(), vecToPed);
		float dotVelocity = DotProduct(pVehicle->GetForward(), pVehicle->GetMoveSpeed());
		if (dotDirection <= frontOffset) /* If already run him over, don't care */
			continue;
		float distanceUntilHit = dotDirection - frontOffset;
		float movementTowardsPedPerSecond = GAME_SPEED_TO_METERS_PER_SECOND * dotVelocity;
		if (4 * movementTowardsPedPerSecond <= distanceUntilHit)
			/* If car isn't projected to hit a ped in 4 seconds, don't care */
			continue;
		float sidewaysDistance = ABS(DotProduct(pVehicle->GetRight(), vecToPed));
		float sideLength = pVehicle->GetModelInfo()->GetColModel()->boundingBox.max.x;
		if (pVehicle->m_vehType == VEHICLE_TYPE_BIKE)
			sideLength *= 1.6f;
		if (sideLength + 0.5f < sidewaysDistance)
			/* If car is far enough taking side into account, don't care */
			continue;
		if (pPed->IsPed()){ /* ...how can it not be? */
			if (pPed->GetPedState() != PED_STEP_AWAY && pPed->GetPedState() != PED_DIVE_AWAY){
				if (distanceUntilHit < movementTowardsPedPerSecond){
					/* Very close. Time to evade. */
					if (pVehicle->GetModelIndex() == MI_RCBANDIT){
						if (dotVelocity * GAME_SPEED_TO_METERS_PER_SECOND / 2 > distanceUntilHit)
							pPed->SetEvasiveStep(pVehicle, 0);
					}else if (dotVelocity > 0.3f) {
						if (sideLength + 0.1f < sidewaysDistance)
							pPed->SetEvasiveStep(pVehicle, 0);
						else
							pPed->SetEvasiveDive(pVehicle, 0);
					}else if (dotVelocity > 0.1f) {
						if (sideLength - 0.5f < sidewaysDistance)
							pPed->SetEvasiveStep(pVehicle, 0);
						else
							pPed->SetEvasiveDive(pVehicle, 0);
					}
				}else{
					/* Relatively safe but annoying. */
					if (pVehicle->GetStatus() == STATUS_PLAYER &&
					  pPed->GetPedState() != PED_FLEE_ENTITY &&
					  pPed->CharCreatedBy == RANDOM_CHAR){
						float angleCarToPed = CGeneral::GetRadianAngleBetweenPoints(
							pVehicle->GetPosition().x, pVehicle->GetPosition().y,
							pPed->GetPosition().x, pPed->GetPosition().y
						);
						angleCarToPed = CGeneral::LimitRadianAngle(angleCarToPed);
						pPed->m_headingRate = CGeneral::LimitRadianAngle(pPed->m_headingRate);
						float visibilityAngle = ABS(angleCarToPed - pPed->m_headingRate);
						if (visibilityAngle > PI)
							visibilityAngle = TWOPI - visibilityAngle;
						if (visibilityAngle < HALFPI || pVehicle->m_nCarHornTimer){
							/* if ped sees the danger or if car horn is on */
							pPed->SetFlee(pVehicle, 2000);
							pPed->bUsePedNodeSeek = false;
							pPed->SetMoveState(PEDMOVE_RUN);
						}
					}else{
						CPlayerPed* pPlayerPed = (CPlayerPed*)pPed;
						if (pPlayerPed->IsPlayer() && dotDirection < frontSafe &&
						  pPlayerPed->IsPedInControl() &&
						  pPlayerPed->m_fMoveSpeed < 1.0f && !pPlayerPed->bIsLooking &&
						  CTimer::GetTimeInMilliseconds() > pPlayerPed->m_lookTimer) {
							pPlayerPed->AnnoyPlayerPed(false);
							pPlayerPed->SetLookFlag(pVehicle, true);
							pPlayerPed->SetLookTimer(1500);
							if (pPlayerPed->GetWeapon()->m_eWeaponType == WEAPONTYPE_UNARMED ||
								pPlayerPed->GetWeapon()->m_eWeaponType == WEAPONTYPE_BASEBALLBAT ||
								pPlayerPed->GetWeapon()->m_eWeaponType == WEAPONTYPE_COLT45 ||
								pPlayerPed->GetWeapon()->m_eWeaponType == WEAPONTYPE_UZI) {
								pPlayerPed->bShakeFist = true;
							}
						}
					}
				}
			}
		}
		/* Ped stuff done. Now vehicle stuff. */
		if (distanceUntilHit < 10.0f){
			if (pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_STOP_FOR_CARS ||
			  pVehicle->AutoPilot.m_nDrivingStyle == DRIVINGSTYLE_SLOW_DOWN_FOR_CARS){
				*pSpeed = Min(*pSpeed, ABS(distanceUntilHit - 1.0f) / 10.0f * curSpeed);
				pVehicle->AutoPilot.m_bSlowedDownBecauseOfPeds = true;
				if (distanceUntilHit < 2.0f){
					pVehicle->AutoPilot.m_nTempAction = TEMPACT_WAIT;
					pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 3000;
				}
			}
		}
	}
}

void CCarCtrl::SlowCarDownForCarsSectorList(CPtrList& lst, CVehicle* pVehicle, float x_inf, float y_inf, float x_sup, float y_sup, float* pSpeed, float curSpeed)
{
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next){
		CVehicle* pTestVehicle = (CVehicle*)pNode->item;
		if (pVehicle == pTestVehicle)
			continue;
		if (pTestVehicle->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pTestVehicle->bUsesCollision)
			continue;
		pTestVehicle->m_scanCode = CWorld::GetCurrentScanCode();
		CVector boundCenter = pTestVehicle->GetBoundCentre();
		if (boundCenter.x < x_inf || boundCenter.x > x_sup)
			continue;
		if (boundCenter.y < y_inf || boundCenter.y > y_sup)
			continue;
		if (Abs(boundCenter.z - pVehicle->GetPosition().z) < 5.0f)
			SlowCarDownForOtherCar(pTestVehicle, pVehicle, pSpeed, curSpeed);
	}
}

void CCarCtrl::SlowCarDownForOtherCar(CEntity* pOtherEntity, CVehicle* pVehicle, float* pSpeed, float curSpeed)
{
	CVector forwardA = pVehicle->GetForward();
	((CVector2D)forwardA).Normalise();
	if (DotProduct2D(pOtherEntity->GetPosition() - pVehicle->GetPosition(), forwardA) < 0.0f)
		return;
	CVector forwardB = pOtherEntity->GetForward();
	((CVector2D)forwardB).Normalise();
	forwardA.z = forwardB.z = 0.0f;
	CVehicle* pOtherVehicle = (CVehicle*)pOtherEntity;
	/* why is the argument CEntity if it's always CVehicle anyway and is casted? */
	float speedOtherX = GAME_SPEED_TO_CARAI_SPEED * pOtherVehicle->GetMoveSpeed().x;
	float speedOtherY = GAME_SPEED_TO_CARAI_SPEED * pOtherVehicle->GetMoveSpeed().y;
	float projectionX = speedOtherX - forwardA.x * curSpeed;
	float projectionY = speedOtherY - forwardA.y * curSpeed;
	float proximityA = TestCollisionBetween2MovingRects(pOtherVehicle, pVehicle, projectionX, projectionY, &forwardA, &forwardB, 0);
	float proximityB = TestCollisionBetween2MovingRects(pVehicle, pOtherVehicle, -projectionX, -projectionY, &forwardB, &forwardA, 1);
	float minProximity = Min(proximityA, proximityB);
	if (minProximity >= 0.0f && minProximity < 1.5f){
		minProximity = Max(0.0f, (minProximity - 0.2f) / 1.3f);
		pVehicle->AutoPilot.m_bSlowedDownBecauseOfCars = true;
		*pSpeed = Min(*pSpeed, minProximity * curSpeed);
	}
	if (minProximity >= 0.0f && minProximity < 0.5f && pOtherEntity->IsVehicle() &&
	  CTimer::GetTimeInMilliseconds() - pVehicle->AutoPilot.m_nTimeToStartMission > 15000 &&
	  CTimer::GetTimeInMilliseconds() - pOtherVehicle->AutoPilot.m_nTimeToStartMission > 15000){
		/* If cars are standing for 15 seconds, annoy one of them and make avoid cars. */
		if (pOtherEntity != FindPlayerVehicle() &&
		  DotProduct2D(pVehicle->GetForward(), pOtherVehicle->GetForward()) < -0.5f &&
		  pVehicle < pOtherVehicle){ /* that comparasion though... */
			*pSpeed = Max(curSpeed / 5, *pSpeed);
			if (pVehicle->GetStatus() == STATUS_SIMPLE){
				pVehicle->SetStatus(STATUS_PHYSICS);
				SwitchVehicleToRealPhysics(pVehicle);
			}
			pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
			pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 1000;
		}
	}
}

float CCarCtrl::TestCollisionBetween2MovingRects(CVehicle* pVehicleA, CVehicle* pVehicleB, float projectionX, float projectionY, CVector* pForwardA, CVector* pForwardB, uint8 id)
{
	CVector2D vecBToA = pVehicleA->GetPosition() - pVehicleB->GetPosition();
	float lenB = pVehicleB->GetModelInfo()->GetColModel()->boundingBox.max.y;
	float widthB = pVehicleB->GetModelInfo()->GetColModel()->boundingBox.max.x;
	float backLenB = -pVehicleB->GetModelInfo()->GetColModel()->boundingBox.min.y;
	float lenA = pVehicleA->GetModelInfo()->GetColModel()->boundingBox.max.y;
	float widthA = pVehicleA->GetModelInfo()->GetColModel()->boundingBox.max.x;
	float backLenA = -pVehicleA->GetModelInfo()->GetColModel()->boundingBox.min.y;
	float proximity = 1.0f;
	float fullWidthB = 2.0f * widthB;
	float fullLenB = lenB + backLenB;
	for (int i = 0; i < 4; i++){
		float testedOffsetX;
		float testedOffsetY;
		switch (i) {
		case 0: /* Front right corner */
			testedOffsetX = vecBToA.x + widthA * pForwardB->y + lenA * pForwardB->x;
			testedOffsetY = vecBToA.y + lenA * pForwardB->y - widthA * pForwardB->x;
			break;
		case 1: /* Front left corner */
			testedOffsetX = vecBToA.x + -widthA * pForwardB->x + lenA * pForwardB->x;
			testedOffsetY = vecBToA.y + lenA * pForwardB->y + widthA * pForwardB->x;
			break;
		case 2: /* Rear right corner */
			testedOffsetX = vecBToA.x + widthA * pForwardB->y - backLenA * pForwardB->x;
			testedOffsetY = vecBToA.y - backLenA * pForwardB->y - widthA * pForwardB->x;
			break;
		case 3: /* Rear left corner */
			testedOffsetX = vecBToA.x - widthA * pForwardB->y - backLenA * pForwardB->x;
			testedOffsetY = vecBToA.y - backLenA * pForwardB->y + widthA * pForwardB->x;
			break;
		default:
			break;
		}
		/* Testing width collision */
		float baseWidthProximity = 0.0f;
		float fullWidthProximity = 1.0f;
		float widthDistance = testedOffsetX * pForwardA->y - testedOffsetY * pForwardA->x;
		float widthProjection = projectionX * pForwardA->y - projectionY * pForwardA->x;
		if (widthDistance > widthB){
			if (widthProjection < 0.0f){
				float proximityWidth = -(widthDistance - widthB) / widthProjection;
				if (proximityWidth < 1.0f){
					baseWidthProximity = proximityWidth;
					fullWidthProximity = Min(1.0f, proximityWidth - fullWidthB / widthProjection);
				}else{
					baseWidthProximity = 1.0f;
				}
			}else{
				baseWidthProximity = 1.0f;
				fullWidthProximity = 1.0f;
			}
		}else if (widthDistance < -widthB){
			if (widthProjection > 0.0f) {
				float proximityWidth = -(widthDistance + widthB) / widthProjection;
				if (proximityWidth < 1.0f) {
					baseWidthProximity = proximityWidth;
					fullWidthProximity = Min(1.0f, proximityWidth + fullWidthB / widthProjection);
				}
				else {
					baseWidthProximity = 1.0f;
				}
			}
			else {
				baseWidthProximity = 1.0f;
				fullWidthProximity = 1.0f;
			}
		}else if (widthProjection > 0.0f){
			fullWidthProximity = (widthB - widthDistance) / widthProjection;
		}else if (widthProjection < 0.0f){
			fullWidthProximity = -(widthB + widthDistance) / widthProjection;
		}
		/* Testing length collision */
		float baseLengthProximity = 0.0f;
		float fullLengthProximity = 1.0f;
		float lenDistance = testedOffsetX * pForwardA->x + testedOffsetY * pForwardA->y;
		float lenProjection = projectionX * pForwardA->x + projectionY * pForwardA->y;
		if (lenDistance > lenB) {
			if (lenProjection < 0.0f) {
				float proximityLength = -(lenDistance - lenB) / lenProjection;
				if (proximityLength < 1.0f) {
					baseLengthProximity = proximityLength;
					fullLengthProximity = Min(1.0f, proximityLength - fullLenB / lenProjection);
				}
				else {
					baseLengthProximity = 1.0f;
				}
			}
			else {
				baseLengthProximity = 1.0f;
				fullLengthProximity = 1.0f;
			}
		}
		else if (lenDistance < -backLenB) {
			if (lenProjection > 0.0f) {
				float proximityLength = -(lenDistance + backLenB) / lenProjection;
				if (proximityLength < 1.0f) {
					baseLengthProximity = proximityLength;
					fullLengthProximity = Min(1.0f, proximityLength + fullLenB / lenProjection);
				}
				else {
					baseLengthProximity = 1.0f;
				}
			}
			else {
				baseLengthProximity = 1.0f;
				fullLengthProximity = 1.0f;
			}
		}
		else if (lenProjection > 0.0f) {
			fullLengthProximity = (lenB - lenDistance) / lenProjection;
		}
		else if (lenProjection < 0.0f) {
			fullLengthProximity = -(backLenB + lenDistance) / lenProjection;
		}
		float baseProximity = Max(baseWidthProximity, baseLengthProximity);
		if (baseProximity < fullWidthProximity && baseProximity < fullLengthProximity)
			proximity = Min(proximity, baseProximity);
	}
	return proximity;
}

float CCarCtrl::FindAngleToWeaveThroughTraffic(CVehicle* pVehicle, CPhysical* pTarget, float angleToTarget, float angleForward)
{
	float distanceToTest = Min(2.0f, pVehicle->GetMoveSpeed().Magnitude2D() / 0.4f + 1.0f) * 12.0f;
	float left = pVehicle->GetPosition().x - distanceToTest;
	float right = pVehicle->GetPosition().x + distanceToTest;
	float top = pVehicle->GetPosition().y - distanceToTest;
	float bottom = pVehicle->GetPosition().y + distanceToTest;
	int xstart = Max(0, CWorld::GetSectorIndexX(left));
	int xend = Min(NUMSECTORS_X - 1, CWorld::GetSectorIndexX(right));
	int ystart = Max(0, CWorld::GetSectorIndexY(top));
	int yend = Min(NUMSECTORS_Y - 1, CWorld::GetSectorIndexY(bottom));
#if REAL_GAMECUBE
	if(xstart > xend || ystart > yend){
		if(!gLoggedBadWeaveScan){
			const CVector &pos = pVehicle->GetPosition();
			printf("[VEH-GUARD] bad weave scan: veh=%p model=%d pos=(%f,%f,%f) speed=(%f,%f,%f) rect=(%f,%f,%f,%f) sectors=(%d,%d,%d,%d) angle=%f\n",
			       pVehicle, pVehicle ? pVehicle->GetModelIndex() : -1,
			       pos.x, pos.y, pos.z, pVehicle->GetMoveSpeed().x, pVehicle->GetMoveSpeed().y, pVehicle->GetMoveSpeed().z,
			       left, top, right, bottom, xstart, xend, ystart, yend, angleToTarget);
			gLoggedBadWeaveScan = true;
		}
		return angleToTarget;
	}
#endif
	assert(xstart <= xend);
	assert(ystart <= yend);

	float angleToWeaveLeft = angleToTarget;
	float angleToWeaveRight = angleToTarget;

	CWorld::AdvanceCurrentScanCode();

	float angleToWeaveLeftLastIteration = -9999.9f;
	float angleToWeaveRightLastIteration = -9999.9f;

	while (angleToWeaveLeft != angleToWeaveLeftLastIteration ||
		   angleToWeaveRight != angleToWeaveRightLastIteration){
		angleToWeaveLeftLastIteration = angleToWeaveLeft;
		angleToWeaveRightLastIteration = angleToWeaveRight;
		for (int y = ystart; y <= yend; y++) {
			for (int x = xstart; x <= xend; x++) {
				CSector* s = CWorld::GetSector(x, y);
				WeaveThroughCarsSectorList(s->m_lists[ENTITYLIST_VEHICLES], pVehicle, pTarget,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughCarsSectorList(s->m_lists[ENTITYLIST_VEHICLES_OVERLAP], pVehicle, pTarget,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughPedsSectorList(s->m_lists[ENTITYLIST_PEDS], pVehicle, pTarget,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughPedsSectorList(s->m_lists[ENTITYLIST_PEDS_OVERLAP], pVehicle, pTarget,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughObjectsSectorList(s->m_lists[ENTITYLIST_OBJECTS], pVehicle,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
				WeaveThroughObjectsSectorList(s->m_lists[ENTITYLIST_OBJECTS_OVERLAP], pVehicle,
					left, top, right, bottom, &angleToWeaveLeft, &angleToWeaveRight);
			}
		}
	}
	float angleDiffFromActualToTarget = LimitRadianAngle(angleForward - angleToTarget);
	float angleToBisectActualToTarget = LimitRadianAngle(angleToTarget + angleDiffFromActualToTarget / 2);
	float angleDiffLeft = LimitRadianAngle(angleToWeaveLeft - angleToBisectActualToTarget);
	angleDiffLeft = ABS(angleDiffLeft);
	float angleDiffRight = LimitRadianAngle(angleToWeaveRight - angleToBisectActualToTarget);
	angleDiffRight = ABS(angleDiffRight);
	if (angleDiffLeft > HALFPI && angleDiffRight > HALFPI)
		return angleToBisectActualToTarget;
	if (ABS(angleDiffLeft - angleDiffRight) < 0.08f)
		return angleToWeaveRight;
	return angleDiffLeft < angleDiffRight ? angleToWeaveLeft : angleToWeaveRight;
}

void CCarCtrl::WeaveThroughCarsSectorList(CPtrList& lst, CVehicle* pVehicle, CPhysical* pTarget, float x_inf, float y_inf, float x_sup, float y_sup, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next) {
		CVehicle* pTestVehicle = (CVehicle*)pNode->item;
		if (pTestVehicle->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pTestVehicle->bUsesCollision)
			continue;
		if (pTestVehicle == pTarget)
			continue;
		pTestVehicle->m_scanCode = CWorld::GetCurrentScanCode();
		if (pTestVehicle->GetBoundCentre().x < x_inf || pTestVehicle->GetBoundCentre().x > x_sup)
			continue;
		if (pTestVehicle->GetBoundCentre().y < y_inf || pTestVehicle->GetBoundCentre().y > y_sup)
			continue;
		if (Abs(pTestVehicle->GetPosition().z - pVehicle->GetPosition().z) >= VEHICLE_HEIGHT_DIFF_TO_CONSIDER_WEAVING)
			continue;
		if (pTestVehicle != pVehicle && (!pVehicle->bPartOfConvoy || !pTestVehicle->bPartOfConvoy))
			WeaveForOtherCar(pTestVehicle, pVehicle, pAngleToWeaveLeft, pAngleToWeaveRight);
	}
}

void CCarCtrl::WeaveForOtherCar(CEntity* pOtherEntity, CVehicle* pVehicle, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	CVehicle* pOtherCar = (CVehicle*)pOtherEntity;
	if (pVehicle->AutoPilot.m_nCarMission == MISSION_RAMPLAYER_CLOSE && pOtherEntity == FindPlayerVehicle())
		return;
	if (pVehicle->AutoPilot.m_nCarMission == MISSION_RAMCAR_CLOSE && pOtherEntity == pVehicle->AutoPilot.m_pTargetCar)
		return;
	CVector2D vecDiff = pOtherCar->GetPosition() - pVehicle->GetPosition();
	float angleBetweenVehicles = CGeneral::GetATanOfXY(vecDiff.x, vecDiff.y);
	float distance = vecDiff.Magnitude();
	if (distance < 1.0f)
		return;
	if (DotProduct2D(pVehicle->GetMoveSpeed() - pOtherCar->GetMoveSpeed(), vecDiff) * 110.0f -
	  pOtherCar->GetColModel()->boundingSphere.radius -
	  pVehicle->GetColModel()->boundingSphere.radius < distance)
		return;
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
	float forwardAngle = CGeneral::GetATanOfXY(forward.x, forward.y);
	float angleDiff = angleBetweenVehicles - forwardAngle;
	float lenProjection = ABS(pOtherCar->GetColModel()->boundingBox.max.y * Sin(angleDiff));
	float widthProjection = ABS(pOtherCar->GetColModel()->boundingBox.max.x * Cos(angleDiff));
	float lengthToEvade = (2 * (lenProjection + widthProjection) + WIDTH_COEF_TO_WEAVE_SAFELY * 2 * pVehicle->GetColModel()->boundingBox.max.x) / distance;
	float diffToLeftAngle = LimitRadianAngle(angleBetweenVehicles - *pAngleToWeaveLeft);
	diffToLeftAngle = ABS(diffToLeftAngle);
	float angleToWeave = lengthToEvade / 2;
	if (diffToLeftAngle < angleToWeave){
		*pAngleToWeaveLeft = angleBetweenVehicles - angleToWeave;
		while (*pAngleToWeaveLeft < -PI)
			*pAngleToWeaveLeft += TWOPI;
	}
	float diffToRightAngle = LimitRadianAngle(angleBetweenVehicles - *pAngleToWeaveRight);
	diffToRightAngle = ABS(diffToRightAngle);
	if (diffToRightAngle < angleToWeave){
		*pAngleToWeaveRight = angleBetweenVehicles + angleToWeave;
		while (*pAngleToWeaveRight > PI)
			*pAngleToWeaveRight -= TWOPI;
	}
}

void CCarCtrl::WeaveThroughPedsSectorList(CPtrList& lst, CVehicle* pVehicle, CPhysical* pTarget, float x_inf, float y_inf, float x_sup, float y_sup, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next) {
		CPed* pPed = (CPed*)pNode->item;
		if (pPed->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pPed->bUsesCollision)
			continue;
		if (pPed == pTarget)
			continue;
		pPed->m_scanCode = CWorld::GetCurrentScanCode();
		if (pPed->GetPosition().x < x_inf || pPed->GetPosition().x > x_sup)
			continue;
		if (pPed->GetPosition().y < y_inf || pPed->GetPosition().y > y_sup)
			continue;
		if (Abs(pPed->GetPosition().z - pVehicle->GetPosition().z) >= PED_HEIGHT_DIFF_TO_CONSIDER_WEAVING)
			continue;
		if (pPed->m_pCurSurface != pVehicle && pPed->m_attachedTo != pVehicle)
			WeaveForPed(pPed, pVehicle, pAngleToWeaveLeft, pAngleToWeaveRight);
	}

}
void CCarCtrl::WeaveForPed(CEntity* pOtherEntity, CVehicle* pVehicle, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	if (pVehicle->AutoPilot.m_nCarMission == MISSION_RAMPLAYER_CLOSE && pOtherEntity == FindPlayerPed())
		return;
	CPed* pPed = (CPed*)pOtherEntity;
	CVector2D vecDiff = pPed->GetPosition() - pVehicle->GetPosition();
	float angleBetweenVehicleAndPed = CGeneral::GetATanOfXY(vecDiff.x, vecDiff.y);
	float distance = vecDiff.Magnitude();
	float lengthToEvade = (WIDTH_COEF_TO_WEAVE_SAFELY * 2 * pVehicle->GetColModel()->boundingBox.max.x + PED_WIDTH_TO_WEAVE) / distance;
	float diffToLeftAngle = LimitRadianAngle(angleBetweenVehicleAndPed - *pAngleToWeaveLeft);
	diffToLeftAngle = ABS(diffToLeftAngle);
	float angleToWeave = lengthToEvade / 2;
	if (diffToLeftAngle < angleToWeave) {
		*pAngleToWeaveLeft = angleBetweenVehicleAndPed - angleToWeave;
		while (*pAngleToWeaveLeft < -PI)
			*pAngleToWeaveLeft += TWOPI;
	}
	float diffToRightAngle = LimitRadianAngle(angleBetweenVehicleAndPed - *pAngleToWeaveRight);
	diffToRightAngle = ABS(diffToRightAngle);
	if (diffToRightAngle < angleToWeave) {
		*pAngleToWeaveRight = angleBetweenVehicleAndPed + angleToWeave;
		while (*pAngleToWeaveRight > PI)
			*pAngleToWeaveRight -= TWOPI;
	}
}

void CCarCtrl::WeaveThroughObjectsSectorList(CPtrList& lst, CVehicle* pVehicle, float x_inf, float y_inf, float x_sup, float y_sup, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	for (CPtrNode* pNode = lst.first; pNode != nil; pNode = pNode->next) {
		CObject* pObject = (CObject*)pNode->item;
#if REAL_GAMECUBE
		if (GcShouldIgnoreScriptedIntroObjectForAI(pVehicle, pObject))
			continue;
#endif
		if (pObject->m_scanCode == CWorld::GetCurrentScanCode())
			continue;
		if (!pObject->bUsesCollision)
			continue;
		pObject->m_scanCode = CWorld::GetCurrentScanCode();
		if (pObject->GetPosition().x < x_inf || pObject->GetPosition().x > x_sup)
			continue;
		if (pObject->GetPosition().y < y_inf || pObject->GetPosition().y > y_sup)
			continue;
		if (Abs(pObject->GetPosition().z - pVehicle->GetPosition().z) >= OBJECT_HEIGHT_DIFF_TO_CONSIDER_WEAVING)
			continue;
		if (pObject->GetUp().z > 0.9f)
			WeaveForObject(pObject, pVehicle, pAngleToWeaveLeft, pAngleToWeaveRight);
	}
}

void CCarCtrl::WeaveForObject(CEntity* pOtherEntity, CVehicle* pVehicle, float* pAngleToWeaveLeft, float* pAngleToWeaveRight)
{
	float rightCoef;
	float forwardCoef;
	if (pOtherEntity->GetModelIndex() == MI_TRAFFICLIGHTS){
		rightCoef = 2.957f;
		forwardCoef = 0.147f;
	}else if (pOtherEntity->GetModelIndex() == MI_SINGLESTREETLIGHTS1){
		rightCoef = 0.744f;
		forwardCoef = 0.0f;
	}else if (pOtherEntity->GetModelIndex() == MI_SINGLESTREETLIGHTS2){
		rightCoef = 0.043f;
		forwardCoef = 0.0f;
	}else if (pOtherEntity->GetModelIndex() == MI_SINGLESTREETLIGHTS3){
		rightCoef = 1.143f;
		forwardCoef = 0.145f;
	}else if (pOtherEntity->GetModelIndex() == MI_DOUBLESTREETLIGHTS){
		rightCoef = 0.0f;
		forwardCoef = -0.048f;
	}else if (IsTreeModel(pOtherEntity->GetModelIndex())){
		rightCoef = 0.0f;
		forwardCoef = 0.0f;
	}else if (pOtherEntity->GetModelIndex() == MI_STREETLAMP1 || pOtherEntity->GetModelIndex() == MI_STREETLAMP2){
		rightCoef = 0.0f;
		forwardCoef = 0.0f;
	}else
		return;
	CObject* pObject = (CObject*)pOtherEntity;
	CVector2D vecDiff = pObject->GetPosition() +
		rightCoef * pObject->GetRight() +
		forwardCoef * pObject->GetForward() -
		pVehicle->GetPosition();
	float angleBetweenVehicleAndObject = CGeneral::GetATanOfXY(vecDiff.x, vecDiff.y);
	float distance = vecDiff.Magnitude();
	float lengthToEvade = (WIDTH_COEF_TO_WEAVE_SAFELY * 2 * pVehicle->GetColModel()->boundingBox.max.x + OBJECT_WIDTH_TO_WEAVE) / distance;
	float diffToLeftAngle = LimitRadianAngle(angleBetweenVehicleAndObject - *pAngleToWeaveLeft);
	diffToLeftAngle = ABS(diffToLeftAngle);
	float angleToWeave = lengthToEvade / 2;
	if (diffToLeftAngle < angleToWeave) {
		*pAngleToWeaveLeft = angleBetweenVehicleAndObject - angleToWeave;
		while (*pAngleToWeaveLeft < -PI)
			*pAngleToWeaveLeft += TWOPI;
	}
	float diffToRightAngle = LimitRadianAngle(angleBetweenVehicleAndObject - *pAngleToWeaveRight);
	diffToRightAngle = ABS(diffToRightAngle);
	if (diffToRightAngle < angleToWeave) {
		*pAngleToWeaveRight = angleBetweenVehicleAndObject + angleToWeave;
		while (*pAngleToWeaveRight > PI)
			*pAngleToWeaveRight -= TWOPI;
	}
}

bool CCarCtrl::PickNextNodeAccordingStrategy(CVehicle* pVehicle)
{
	pVehicle->AutoPilot.m_nCruiseSpeedMultiplierType = ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode].speedLimit;
	switch (pVehicle->AutoPilot.m_nCarMission){
	case MISSION_RAMPLAYER_FARAWAY:
	case MISSION_BLOCKPLAYER_FARAWAY:
		PickNextNodeToChaseCar(pVehicle,
			FindPlayerCoors().x,
			FindPlayerCoors().y,
#ifdef FIX_PATHFIND_BUG
			FindPlayerCoors().z,
#endif
			FindPlayerVehicle());
		return false;
	case MISSION_GOTOCOORDS:
	case MISSION_GOTOCOORDS_ACCURATE:
		return PickNextNodeToFollowPath(pVehicle);
	case MISSION_RAMCAR_FARAWAY:
	case MISSION_BLOCKCAR_FARAWAY:
		PickNextNodeToChaseCar(pVehicle,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().x,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().y,
#ifdef FIX_PATHFIND_BUG
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().z,
#endif
			pVehicle->AutoPilot.m_pTargetCar);
		return false;
	default:
		PickNextNodeRandomly(pVehicle);
		if (ThePaths.GetNode(pVehicle->AutoPilot.m_nNextRouteNode)->bOnlySmallBoats && BoatWithTallMast(pVehicle->GetModelIndex()))
			pVehicle->AutoPilot.m_nCruiseSpeed = 0;
		return false;
	}
}

void CCarCtrl::PickNextNodeRandomly(CVehicle* pVehicle)
{
	if (pVehicle->m_nRouteSeed)
		CGeneral::SetRandomSeed(pVehicle->m_nRouteSeed);
	int32 prevNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	int32 curNode = pVehicle->AutoPilot.m_nNextRouteNode;
	uint8 totalLinks = ThePaths.m_pathNodes[curNode].numLinks;
	CCarPathLink* pCurLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	uint8 lanesOnCurrentPath;
	bool isOnOneWayRoad;
	if (pCurLink->pathNodeIndex == curNode) {
		lanesOnCurrentPath = pCurLink->numLeftLanes;
		isOnOneWayRoad = pCurLink->numRightLanes == 0;
	}
	else {
		lanesOnCurrentPath = pCurLink->numRightLanes;
		isOnOneWayRoad = pCurLink->numLeftLanes == 0;
	}
	uint8 allowedDirections = PATH_DIRECTION_NONE;
	uint8 nextLane = pVehicle->AutoPilot.m_nNextLane;
	if (nextLane == 0)
		/* We are always allowed to turn left from  leftmost lane */
		allowedDirections |= PATH_DIRECTION_LEFT;
	if (nextLane == lanesOnCurrentPath - 1)
		/* We are always allowed to turn right from rightmost lane */
		allowedDirections |= PATH_DIRECTION_RIGHT;
	if (lanesOnCurrentPath < 3 || allowedDirections == PATH_DIRECTION_NONE)
		/* We are always allowed to go straight on one/two-laned road */
		/* or if we are in one of middle lanes of the road */
		allowedDirections |= PATH_DIRECTION_STRAIGHT;
	int attempt;
	pVehicle->AutoPilot.m_nPrevRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_nNextRouteNode;
	CPathNode* pPrevPathNode = &ThePaths.m_pathNodes[prevNode];
	CPathNode* pCurPathNode = &ThePaths.m_pathNodes[curNode];
	int16 nextLink;
	CCarPathLink* pNextLink;
	CPathNode* pNextPathNode;
	bool goingAgainstOneWayRoad;
	bool nextNodeIsOneWayRoad;
	uint8 direction;
	for(attempt = 0; attempt < ATTEMPTS_TO_FIND_NEXT_NODE; attempt++){
		if (attempt != 0){
			if (pVehicle->AutoPilot.m_nNextRouteNode != prevNode){
				if (direction & allowedDirections){
					pNextPathNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode];
					if ((!pNextPathNode->bDeadEnd || pPrevPathNode->bDeadEnd) &&
						(!pNextPathNode->bDisabled || pPrevPathNode->bDisabled) &&
						(!pNextPathNode->bBetweenLevels || pPrevPathNode->bBetweenLevels || !pVehicle->AutoPilot.m_bStayInCurrentLevel) &&
						!goingAgainstOneWayRoad && (!isOnOneWayRoad || !nextNodeIsOneWayRoad))
						break;
				}
			}
		}
		nextLink = CGeneral::GetRandomNumber() % totalLinks;
		pVehicle->AutoPilot.m_nNextRouteNode = ThePaths.ConnectedNode(nextLink + pCurPathNode->firstLink);
		direction = FindPathDirection(prevNode, curNode, pVehicle->AutoPilot.m_nNextRouteNode);
		pNextLink = &ThePaths.m_carPathLinks[ThePaths.m_carPathConnections[nextLink + pCurPathNode->firstLink]];
		goingAgainstOneWayRoad = pNextLink->pathNodeIndex == curNode ? pNextLink->numRightLanes == 0 : pNextLink->numLeftLanes == 0;
		nextNodeIsOneWayRoad = pNextLink->pathNodeIndex == curNode ? pNextLink->numLeftLanes == 0 : pNextLink->numRightLanes == 0;
	}
	if (attempt >= ATTEMPTS_TO_FIND_NEXT_NODE) {
		/* If we failed 15 times, then remove dead end, one way road and current lane limitations */
		for (attempt = 0; attempt < ATTEMPTS_TO_FIND_NEXT_NODE; attempt++) {
			if (attempt != 0) {
				if (pVehicle->AutoPilot.m_nNextRouteNode != prevNode) {
					pNextPathNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode];
					if ((!pNextPathNode->bDisabled || pPrevPathNode->bDisabled) &&
						(!pNextPathNode->bBetweenLevels || pPrevPathNode->bBetweenLevels || !pVehicle->AutoPilot.m_bStayInCurrentLevel) &&
						!goingAgainstOneWayRoad)
						break;
				}
			}
			nextLink = CGeneral::GetRandomNumber() % totalLinks;
			pVehicle->AutoPilot.m_nNextRouteNode = ThePaths.ConnectedNode(nextLink + pCurPathNode->firstLink);
			pNextLink = &ThePaths.m_carPathLinks[ThePaths.m_carPathConnections[nextLink + pCurPathNode->firstLink]];
			goingAgainstOneWayRoad = pNextLink->pathNodeIndex == curNode ? pNextLink->numRightLanes == 0 : pNextLink->numLeftLanes == 0;
		}
	}
	if (attempt >= ATTEMPTS_TO_FIND_NEXT_NODE) {
		/* If we failed again, remove no U-turn limitation and remove randomness */
		for (nextLink = 0; nextLink < totalLinks; nextLink++) {
			pVehicle->AutoPilot.m_nNextRouteNode = ThePaths.ConnectedNode(nextLink + pCurPathNode->firstLink);
			pNextLink = &ThePaths.m_carPathLinks[ThePaths.m_carPathConnections[nextLink + pCurPathNode->firstLink]];
			goingAgainstOneWayRoad = pNextLink->pathNodeIndex == curNode ? pNextLink->numRightLanes == 0 : pNextLink->numLeftLanes == 0;
			if (!goingAgainstOneWayRoad) {
				pNextPathNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode];
				if ((!pNextPathNode->bDisabled || pPrevPathNode->bDisabled) &&
					(!pNextPathNode->bBetweenLevels || pPrevPathNode->bBetweenLevels || !pVehicle->AutoPilot.m_bStayInCurrentLevel))
					/* Nice way to exit loop but this will fail because this is used for indexing! */
					nextLink = 1000;
			}
		}
		if (nextLink < 999)
			/* If everything else failed, turn vehicle around */
			pVehicle->AutoPilot.m_nNextRouteNode = prevNode;
	}
	pNextPathNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode];
	pNextLink = &ThePaths.m_carPathLinks[ThePaths.m_carPathConnections[nextLink + pCurPathNode->firstLink]];
	if (prevNode == pVehicle->AutoPilot.m_nNextRouteNode){
		/* We can no longer shift vehicle without physics if we have to turn it around. */
		pVehicle->SetStatus(STATUS_PHYSICS);
		SwitchVehicleToRealPhysics(pVehicle);
	}
	pVehicle->AutoPilot.m_nTimeEnteredCurve += pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
	pVehicle->AutoPilot.m_nPreviousPathNodeInfo = pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	pVehicle->AutoPilot.m_nPreviousDirection = pVehicle->AutoPilot.m_nCurrentDirection;
	pVehicle->AutoPilot.m_nCurrentDirection = pVehicle->AutoPilot.m_nNextDirection;
	pVehicle->AutoPilot.m_nCurrentLane = pVehicle->AutoPilot.m_nNextLane;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = ThePaths.m_carPathConnections[nextLink + pCurPathNode->firstLink];
	int8 lanesOnNextNode;
	if (curNode >= pVehicle->AutoPilot.m_nNextRouteNode){
		pVehicle->AutoPilot.m_nNextDirection = 1;
		lanesOnNextNode = pNextLink->numLeftLanes;
	}else{
		pVehicle->AutoPilot.m_nNextDirection = -1;
		lanesOnNextNode = pNextLink->numRightLanes;
	}
	float currentPathLinkForwardX = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirX();
	float nextPathLinkForwardX = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirX();
#ifdef FIX_BUGS
	float currentPathLinkForwardY = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirY();
	float nextPathLinkForwardY = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirY();
#endif
	if (lanesOnNextNode >= 0){
		if ((CGeneral::GetRandomNumber() & 0x600) == 0){
			/* 25% chance vehicle will try to switch lane */
			CVector2D dist = pNextPathNode->GetPosition() - pCurPathNode->GetPosition();
			if (dist.MagnitudeSqr() >= SQR(14.0f)){
				if (CGeneral::GetRandomTrueFalse())
					pVehicle->AutoPilot.m_nNextLane += 1;
				else
					pVehicle->AutoPilot.m_nNextLane -= 1;
			}
		}
		pVehicle->AutoPilot.m_nNextLane = Min(lanesOnNextNode - 1, pVehicle->AutoPilot.m_nNextLane);
		pVehicle->AutoPilot.m_nNextLane = Max(0, pVehicle->AutoPilot.m_nNextLane);
	}else{
		pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane;
	}
	if (pVehicle->AutoPilot.m_bStayInFastLane)
		pVehicle->AutoPilot.m_nNextLane = 0;
	CVector positionOnCurrentLinkIncludingLane(
		pCurLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH)
#ifdef FIX_BUGS
		* currentPathLinkForwardY
#endif
		,pCurLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
		0.0f);
	CVector positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH)
#ifdef FIX_BUGS
		* nextPathLinkForwardY
#endif
		,pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
		0.0f);
	float directionCurrentLinkX = pCurLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionCurrentLinkY = pCurLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	/* We want to make a path between two links that may not have the same forward directions a curve. */
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = CCurves::CalcSpeedScaleFactor(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		directionCurrentLinkX, directionCurrentLinkY,
		directionNextLinkX, directionNextLinkY
	) * (1000.0f / pVehicle->AutoPilot.m_fMaxTrafficSpeed);
	if (pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve < 10)
		/* Oh hey there Obbe */
		printf("fout\n");
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = Max(10, pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve);
}

uint8 CCarCtrl::FindPathDirection(int32 prevNode, int32 curNode, int32 nextNode)
{
	CVector2D prevToCur = ThePaths.m_pathNodes[curNode].GetPosition() - ThePaths.m_pathNodes[prevNode].GetPosition();
	CVector2D curToNext = ThePaths.m_pathNodes[nextNode].GetPosition() - ThePaths.m_pathNodes[curNode].GetPosition();
	float distPrevToCur = prevToCur.Magnitude();
	if (distPrevToCur == 0.0f)
		return PATH_DIRECTION_NONE;
	/* We are trying to determine angle between prevToCur and curToNext. */
	/* To find it, we consider a to be an angle between y axis and prevToCur */
	/* and b to be an angle between x axis and curToNext */
	/* Then the angle we are looking for is (pi/2 + a + b). */
	float sin_a = prevToCur.x / distPrevToCur;
	float cos_a = prevToCur.y / distPrevToCur;
	float distCurToNext = curToNext.Magnitude();
	if (distCurToNext == 0.0f)
		return PATH_DIRECTION_NONE;
	float sin_b = curToNext.y / distCurToNext;
	float cos_b = curToNext.x / distCurToNext;
	/* sin(a) * sin(b) - cos(a) * cos(b) = -cos(a+b) = sin(pi/2+a+b) */
	float sin_direction = sin_a * sin_b - cos_a * cos_b;
	if (sin_direction > 0.77f) /* Roughly between -50 and -130 degrees */
		return PATH_DIRECTION_LEFT;
	if (sin_direction < -0.77f) /* Roughly between 50 and 130 degrees */
		return PATH_DIRECTION_RIGHT;
	return PATH_DIRECTION_STRAIGHT;
}

#ifdef FIX_PATHFIND_BUG
void CCarCtrl::PickNextNodeToChaseCar(CVehicle* pVehicle, float targetX, float targetY, float targetZ, CVehicle* pTarget)
#else
void CCarCtrl::PickNextNodeToChaseCar(CVehicle* pVehicle, float targetX, float targetY, CVehicle* pTarget)
#endif
{
	if (pVehicle->m_nRouteSeed)
		CGeneral::SetRandomSeed(pVehicle->m_nRouteSeed);
	int prevNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	int curNode = pVehicle->AutoPilot.m_nNextRouteNode;
	CPathNode* pPrevNode = &ThePaths.m_pathNodes[prevNode];
	CPathNode* pCurNode = &ThePaths.m_pathNodes[curNode];
	CPathNode* pTargetNode[2];
	int16 numNodes;
	float distanceToTargetNode;
	ThePaths.DoPathSearch(0, pCurNode->GetPosition(), curNode,
#ifdef FIX_PATHFIND_BUG
		CVector(targetX, targetY, targetZ),
#else
		CVector(targetX, targetY, 0.0f),
#endif
		pTargetNode, &numNodes, 2, pVehicle, &distanceToTargetNode, 999999.9f, -1);

	int newNextNode;
	int nextLink;
	if (numNodes != 1 && numNodes != 2 || pTargetNode[0] == pCurNode){
		if (numNodes != 2 || pTargetNode[1] == pCurNode) {
			float currentAngle = CGeneral::GetATanOfXY(targetX - pVehicle->GetPosition().x, targetY - pVehicle->GetPosition().y);
			nextLink = 0;
			float lowestAngleChange = 10.0f;
			int numLinks = pCurNode->numLinks;
			newNextNode = 0;
			for (int i = 0; i < numLinks; i++) {
				int conNode = ThePaths.ConnectedNode(i + pCurNode->firstLink);
				if (conNode == prevNode && i > 1)
					continue;
				CPathNode* pTestNode = &ThePaths.m_pathNodes[conNode];
				float angle = CGeneral::GetATanOfXY(pTestNode->GetX() - pCurNode->GetX(), pTestNode->GetY() - pCurNode->GetY());
				angle = LimitRadianAngle(angle - currentAngle);
				angle = ABS(angle);
				if (angle < lowestAngleChange) {
					lowestAngleChange = angle;
					newNextNode = conNode;
					nextLink = i;
				}
			}
		}
		else {
			nextLink = 0;
			newNextNode = pTargetNode[1] - ThePaths.m_pathNodes;
			for (int i = pCurNode->firstLink; ThePaths.ConnectedNode(i) != newNextNode; i++, nextLink++)
				;
		}
	}
	else {
		nextLink = 0;
		newNextNode = pTargetNode[0] - ThePaths.m_pathNodes;
		for (int i = pCurNode->firstLink; ThePaths.ConnectedNode(i) != newNextNode; i++, nextLink++)
			;
	}
	CPathNode* pNextPathNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode];
	CCarPathLink* pNextLink = &ThePaths.m_carPathLinks[ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink]];
	CCarPathLink* pCurLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	pVehicle->AutoPilot.m_nPrevRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_nNextRouteNode;
	pVehicle->AutoPilot.m_nNextRouteNode = newNextNode;
	pVehicle->AutoPilot.m_nTimeEnteredCurve += pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
	pVehicle->AutoPilot.m_nPreviousPathNodeInfo = pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	pVehicle->AutoPilot.m_nPreviousDirection = pVehicle->AutoPilot.m_nCurrentDirection;
	pVehicle->AutoPilot.m_nCurrentDirection = pVehicle->AutoPilot.m_nNextDirection;
	pVehicle->AutoPilot.m_nCurrentLane = pVehicle->AutoPilot.m_nNextLane;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink];
	int8 lanesOnNextNode;
	if (curNode >= pVehicle->AutoPilot.m_nNextRouteNode) {
		pVehicle->AutoPilot.m_nNextDirection = 1;
		lanesOnNextNode = pNextLink->numRightLanes;
	}
	else {
		pVehicle->AutoPilot.m_nNextDirection = -1;
		lanesOnNextNode = pNextLink->numLeftLanes;
	}
	float currentPathLinkForwardX = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirX();
	float currentPathLinkForwardY = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirY();
	float nextPathLinkForwardX = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirX();
	float nextPathLinkForwardY = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirY();
	if (lanesOnNextNode >= 0) {
		CVector2D dist = pNextPathNode->GetPosition() - pCurNode->GetPosition();
		if (dist.MagnitudeSqr() >= SQR(7.0f)){
			/* 25% chance vehicle will try to switch lane */
			/* No lane switching if following car from far away */
			/* ...although it's always one of those. */
			if ((CGeneral::GetRandomNumber() & 0x600) == 0 &&
			  pVehicle->AutoPilot.m_nCarMission != MISSION_RAMPLAYER_FARAWAY &&
			  pVehicle->AutoPilot.m_nCarMission != MISSION_BLOCKPLAYER_FARAWAY &&
			  pVehicle->AutoPilot.m_nCarMission != MISSION_RAMCAR_FARAWAY &&
			  pVehicle->AutoPilot.m_nCarMission != MISSION_BLOCKCAR_FARAWAY){
				if (CGeneral::GetRandomTrueFalse())
					pVehicle->AutoPilot.m_nNextLane += 1;
				else
					pVehicle->AutoPilot.m_nNextLane -= 1;
			}
		}
		pVehicle->AutoPilot.m_nNextLane = Min(lanesOnNextNode - 1, pVehicle->AutoPilot.m_nNextLane);
		pVehicle->AutoPilot.m_nNextLane = Max(0, pVehicle->AutoPilot.m_nNextLane);
	}
	else {
		pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane;
	}
	if (pVehicle->AutoPilot.m_bStayInFastLane)
		pVehicle->AutoPilot.m_nNextLane = 0;
	CVector positionOnCurrentLinkIncludingLane(
		pCurLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
		pCurLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
		0.0f);
	CVector positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
		0.0f);
	float directionCurrentLinkX = pCurLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionCurrentLinkY = pCurLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	/* We want to make a path between two links that may not have the same forward directions a curve. */
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = CCurves::CalcSpeedScaleFactor(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		directionCurrentLinkX, directionCurrentLinkY,
		directionNextLinkX, directionNextLinkY
	) * (1000.0f / pVehicle->AutoPilot.m_fMaxTrafficSpeed);
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = Max(10, pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve);
}

bool CCarCtrl::PickNextNodeToFollowPath(CVehicle* pVehicle)
{
	if (pVehicle->m_nRouteSeed)
		CGeneral::SetRandomSeed(pVehicle->m_nRouteSeed);
	int curNode = pVehicle->AutoPilot.m_nNextRouteNode;
	CPathNode* pCurNode = &ThePaths.m_pathNodes[curNode];
	if (pVehicle->AutoPilot.m_nPathFindNodesCount == 0){
		ThePaths.DoPathSearch(0, pVehicle->GetPosition(), curNode,
			pVehicle->AutoPilot.m_vecDestinationCoors, pVehicle->AutoPilot.m_aPathFindNodesInfo,
			&pVehicle->AutoPilot.m_nPathFindNodesCount, NUM_PATH_NODES_IN_AUTOPILOT,
			pVehicle, nil, 999999.9f, -1);
#if REAL_GAMECUBE
		{
			int rawCount = pVehicle->AutoPilot.m_nPathFindNodesCount;
			int firstNode = rawCount > 0 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes : -1;
			int secondNode = rawCount > 1 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[1] - ThePaths.m_pathNodes : -1;
			GcTraceAdmiralPathSearch(pVehicle, "pick-search",
				curNode, rawCount, rawCount,
				pVehicle->AutoPilot.m_nCurrentRouteNode,
				pVehicle->AutoPilot.m_nNextRouteNode,
				pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
				pVehicle->AutoPilot.m_nNextPathNodeInfo,
				firstNode, secondNode);
		}
#endif
#if REAL_GAMECUBE
		if (pVehicle->AutoPilot.m_nPathFindNodesCount < 2 &&
		    GcShouldHoldScriptedIntroFinalSegment(pVehicle, curNode)) {
			GcTraceAdmiralPathSearch(pVehicle, "pick-hold-final",
				curNode, 0, 0,
				pVehicle->AutoPilot.m_nCurrentRouteNode,
				pVehicle->AutoPilot.m_nNextRouteNode,
				pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
				pVehicle->AutoPilot.m_nNextPathNodeInfo,
				-1, -1);
			return false;
		}
		if (pVehicle->AutoPilot.m_nPathFindNodesCount < 2 &&
		    !GcIsScriptedIntroTightFinalApproach(pVehicle)) {
			CVector2D segmentTarget;
			if (GcGetScriptedIntroSegmentContinuationTarget(pVehicle, &segmentTarget)) {
				GcTraceAdmiralPath(pVehicle, "pick-keep-segment",
					segmentTarget.x, segmentTarget.y);
				return false;
			}
		}
#endif
		if (pVehicle->AutoPilot.m_nPathFindNodesCount < 2)
			return true;
		pVehicle->AutoPilot.RemoveOnePathNode();
#if REAL_GAMECUBE
		{
			int finalCount = pVehicle->AutoPilot.m_nPathFindNodesCount;
			int firstNode = finalCount > 0 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes : -1;
			int secondNode = finalCount > 1 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[1] - ThePaths.m_pathNodes : -1;
			GcTraceAdmiralPathSearch(pVehicle, "pick-after-pop",
				curNode, finalCount + 1, finalCount,
				pVehicle->AutoPilot.m_nCurrentRouteNode,
				pVehicle->AutoPilot.m_nNextRouteNode,
				pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
				pVehicle->AutoPilot.m_nNextPathNodeInfo,
				firstNode, secondNode);
		}
#endif
	}
#if REAL_GAMECUBE
	if (GcShouldDeferScriptedIntroActiveSegmentAdvance(pVehicle)) {
		GcTraceAdmiralPathSearch(pVehicle, "pick-defer-active-handoff",
			curNode, pVehicle->AutoPilot.m_nPathFindNodesCount, pVehicle->AutoPilot.m_nPathFindNodesCount,
			pVehicle->AutoPilot.m_nCurrentRouteNode,
			pVehicle->AutoPilot.m_nNextRouteNode,
			pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
			pVehicle->AutoPilot.m_nNextPathNodeInfo,
			pVehicle->AutoPilot.m_nPathFindNodesCount > 0 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes : -1,
			pVehicle->AutoPilot.m_nPathFindNodesCount > 1 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[1] - ThePaths.m_pathNodes : -1);
		return false;
	}
	while (pVehicle->AutoPilot.m_nPathFindNodesCount > 0 &&
	       pVehicle->AutoPilot.m_aPathFindNodesInfo[0] != nil &&
	       pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes == pVehicle->AutoPilot.m_nNextRouteNode) {
		pVehicle->AutoPilot.RemoveOnePathNode();
		GcTraceAdmiralPathSearch(pVehicle, "pick-drop-duplicate-next",
			curNode, pVehicle->AutoPilot.m_nPathFindNodesCount + 1, pVehicle->AutoPilot.m_nPathFindNodesCount,
			pVehicle->AutoPilot.m_nCurrentRouteNode,
			pVehicle->AutoPilot.m_nNextRouteNode,
			pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
			pVehicle->AutoPilot.m_nNextPathNodeInfo,
			pVehicle->AutoPilot.m_nPathFindNodesCount > 0 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes : -1,
			pVehicle->AutoPilot.m_nPathFindNodesCount > 1 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[1] - ThePaths.m_pathNodes : -1);
	}
#endif
	if (pVehicle->AutoPilot.m_nPathFindNodesCount == 0)
		return true;
	CPathNode* pNextPathNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nNextRouteNode];
	CCarPathLink* pCurLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	pVehicle->AutoPilot.m_nPrevRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_nNextRouteNode;
	pVehicle->AutoPilot.m_nNextRouteNode = pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes;
	pVehicle->AutoPilot.RemoveOnePathNode();
#if REAL_GAMECUBE
	{
		int finalCount = pVehicle->AutoPilot.m_nPathFindNodesCount;
		int firstNode = finalCount > 0 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes : -1;
		int secondNode = finalCount > 1 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[1] - ThePaths.m_pathNodes : -1;
		GcTraceAdmiralPathSearch(pVehicle, "pick-advance",
			curNode, finalCount + 1, finalCount,
			pVehicle->AutoPilot.m_nCurrentRouteNode,
			pVehicle->AutoPilot.m_nNextRouteNode,
			pVehicle->AutoPilot.m_nCurrentPathNodeInfo,
			pVehicle->AutoPilot.m_nNextPathNodeInfo,
			firstNode, secondNode);
	}
#endif
	pVehicle->AutoPilot.m_nTimeEnteredCurve += pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve;
	pVehicle->AutoPilot.m_nPreviousPathNodeInfo = pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	pVehicle->AutoPilot.m_nPreviousDirection = pVehicle->AutoPilot.m_nCurrentDirection;
	pVehicle->AutoPilot.m_nCurrentDirection = pVehicle->AutoPilot.m_nNextDirection;
	pVehicle->AutoPilot.m_nCurrentLane = pVehicle->AutoPilot.m_nNextLane;
	int nextLink = 0;
	for (int i = pCurNode->firstLink; ThePaths.ConnectedNode(i) != pVehicle->AutoPilot.m_nNextRouteNode; i++, nextLink++)
		;
	CCarPathLink* pNextLink = &ThePaths.m_carPathLinks[ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink]];
	pVehicle->AutoPilot.m_nNextPathNodeInfo = ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink];
	int8 lanesOnNextNode;
	if (curNode >= pVehicle->AutoPilot.m_nNextRouteNode) {
		pVehicle->AutoPilot.m_nNextDirection = 1;
		lanesOnNextNode = pNextLink->numLeftLanes;
	}
	else {
		pVehicle->AutoPilot.m_nNextDirection = -1;
		lanesOnNextNode = pNextLink->numRightLanes;
	}
	float currentPathLinkForwardX = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirX();
	float currentPathLinkForwardY = pVehicle->AutoPilot.m_nCurrentDirection * pCurLink->GetDirY();
	float nextPathLinkForwardX = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirX();
	float nextPathLinkForwardY = pVehicle->AutoPilot.m_nNextDirection * pNextLink->GetDirY();
	if (lanesOnNextNode >= 0) {
		CVector2D dist = pNextPathNode->GetPosition() - pCurNode->GetPosition();
		if (dist.MagnitudeSqr() >= SQR(7.0f) && (CGeneral::GetRandomNumber() & 0x600) == 0) {
			if (CGeneral::GetRandomTrueFalse())
				pVehicle->AutoPilot.m_nNextLane += 1;
			else
				pVehicle->AutoPilot.m_nNextLane -= 1;
		}
		pVehicle->AutoPilot.m_nNextLane = Min(lanesOnNextNode - 1, pVehicle->AutoPilot.m_nNextLane);
		pVehicle->AutoPilot.m_nNextLane = Max(0, pVehicle->AutoPilot.m_nNextLane);
	}
	else {
		pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane;
	}
	if (pVehicle->AutoPilot.m_bStayInFastLane)
		pVehicle->AutoPilot.m_nNextLane = 0;
	CVector positionOnCurrentLinkIncludingLane(
		pCurLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardY,
		pCurLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForwardX,
		0.0f);
	CVector positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX,
		0.0f);
	float directionCurrentLinkX = pCurLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionCurrentLinkY = pCurLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
	float directionNextLinkX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float directionNextLinkY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	/* We want to make a path between two links that may not have the same forward directions a curve. */
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = CCurves::CalcSpeedScaleFactor(
		&positionOnCurrentLinkIncludingLane,
		&positionOnNextLinkIncludingLane,
		directionCurrentLinkX, directionCurrentLinkY,
		directionNextLinkX, directionNextLinkY
	) * (1000.0f / pVehicle->AutoPilot.m_fMaxTrafficSpeed);
	pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve = Max(10, pVehicle->AutoPilot.m_nTimeToSpendOnCurrentCurve);
	return false;
}

void CCarCtrl::Init(void)
{
	NumRandomCars = 0;
	NumLawEnforcerCars = 0;
	NumMissionCars = 0;
	NumParkedCars = 0;
	NumPermanentCars = 0;
	NumAmbulancesOnDuty = 0;
	NumFiretrucksOnDuty = 0;
	LastTimeFireTruckCreated = 0;
	LastTimeAmbulanceCreated = 0;
#ifdef FIX_BUGS
	LastTimeLawEnforcerCreated = 0;
	LastTimeMiamiViceGenerated = 0;
#endif
	bCarsGeneratedAroundCamera = false;
	CountDownToCarsAtStart = 2;
	CarDensityMultiplier = 1.0f;
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++)
		apCarsToKeep[i] = nil;
	for (int i = 0; i < TOTAL_CUSTOM_CLASSES; i++){
		for (int j = 0; j < MAX_CAR_MODELS_IN_ARRAY; j++) {
			LoadedCarsArray[i][j] = -1;
		}
		NumOfLoadedCarsOfRating[i] = 0;
		NumRequestsOfCarRating[i] = 0;
		TotalNumOfCarsOfRating[i] = 0;
	}
}

void CCarCtrl::ReInit(void)
{
	NumRandomCars = 0;
	NumLawEnforcerCars = 0;
	NumMissionCars = 0;
	NumParkedCars = 0;
	NumPermanentCars = 0;
	NumAmbulancesOnDuty = 0;
	NumFiretrucksOnDuty = 0;
#ifdef FIX_BUGS
	LastTimeFireTruckCreated = 0;
	LastTimeAmbulanceCreated = 0;
	LastTimeLawEnforcerCreated = 0;
	LastTimeMiamiViceGenerated = 0;
#endif
	CountDownToCarsAtStart = 2;
	CarDensityMultiplier = 1.0f;
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++)
		apCarsToKeep[i] = nil;
	for (int i = 0; i < TOTAL_CUSTOM_CLASSES; i++)
		NumRequestsOfCarRating[i] = 0;
}

void CCarCtrl::DragCarToPoint(CVehicle* pVehicle, CVector* pPoint)
{
	CVector2D posBehind = (CVector2D)pVehicle->GetPosition() - 3 * pVehicle->GetForward() / 2;
	CVector2D posTarget = *pPoint;
	CVector2D direction = posBehind - posTarget;
	CVector2D midPos = posTarget + direction * 3 / direction.Magnitude();
	float actualAheadZ;
	float actualBehindZ;
	CColPoint point;
	CEntity* pRoadObject;
	if (CCollision::IsStoredPolyStillValidVerticalLine(CVector(posTarget.x, posTarget.y, pVehicle->GetPosition().z - 3.0f),
	  pVehicle->GetPosition().z - 3.0f, point, &pVehicle->m_aCollPolys[0])){
		actualAheadZ = point.point.z;
	}else if (CWorld::ProcessVerticalLine(CVector(posTarget.x, posTarget.y, pVehicle->GetPosition().z + 1.5f),
	  pVehicle->GetPosition().z - 2.0f, point,
	  pRoadObject, true, false, false, false, false, false, &pVehicle->m_aCollPolys[0])){
		actualAheadZ = point.point.z;
		pVehicle->m_pCurGroundEntity = pRoadObject;
		if (ThisRoadObjectCouldMove(pRoadObject->GetModelIndex()))
			pVehicle->m_aCollPolys[0].valid = false;
	}else if (CWorld::ProcessVerticalLine(CVector(posTarget.x, posTarget.y, pVehicle->GetPosition().z + 3.0f),
	  pVehicle->GetPosition().z - 3.0f, point,
	  pRoadObject, true, false, false, false, false, false, &pVehicle->m_aCollPolys[0])) {
		actualAheadZ = point.point.z;
		pVehicle->m_pCurGroundEntity = pRoadObject;
		if (ThisRoadObjectCouldMove(pRoadObject->GetModelIndex()))
			pVehicle->m_aCollPolys[0].valid = false;
	}else{
		actualAheadZ = pVehicle->m_fMapObjectHeightAhead;
	}
	pVehicle->m_fMapObjectHeightAhead = actualAheadZ;
	if (CCollision::IsStoredPolyStillValidVerticalLine(CVector(midPos.x, midPos.y, pVehicle->GetPosition().z - 3.0f),
	  pVehicle->GetPosition().z - 3.0f, point, &pVehicle->m_aCollPolys[1])){
		actualBehindZ = point.point.z;
	}else if (CWorld::ProcessVerticalLine(CVector(midPos.x, midPos.y, pVehicle->GetPosition().z + 1.5f),
	  pVehicle->GetPosition().z - 2.0f, point,
	  pRoadObject, true, false, false, false, false, false, &pVehicle->m_aCollPolys[1])){
		actualBehindZ = point.point.z;
		pVehicle->m_pCurGroundEntity = pRoadObject;
		if (ThisRoadObjectCouldMove(pRoadObject->GetModelIndex()))
			pVehicle->m_aCollPolys[1].valid = false;
	}else if (CWorld::ProcessVerticalLine(CVector(midPos.x, midPos.y, pVehicle->GetPosition().z + 3.0f),
	  pVehicle->GetPosition().z - 3.0f, point,
	  pRoadObject, true, false, false, false, false, false, &pVehicle->m_aCollPolys[1])){
		actualBehindZ = point.point.z;
		pVehicle->m_pCurGroundEntity = pRoadObject;
		if (ThisRoadObjectCouldMove(pRoadObject->GetModelIndex()))
			pVehicle->m_aCollPolys[1].valid = false;
	}else{
		actualBehindZ = pVehicle->m_fMapObjectHeightBehind;
	}
	pVehicle->m_fMapObjectHeightBehind = actualBehindZ;
	float angleZ = Atan2((actualAheadZ - actualBehindZ) / 3, 1.0f);
	float cosZ = Cos(angleZ);
	float sinZ = Sin(angleZ);
	pVehicle->GetRight() = CVector(posTarget.y - midPos.y, -(posTarget.x - midPos.x), 0.0f) / 3;
	pVehicle->GetForward() = CVector(-cosZ * pVehicle->GetRight().y, cosZ * pVehicle->GetRight().x, sinZ);
	pVehicle->GetUp() = CrossProduct(pVehicle->GetRight(), pVehicle->GetForward());
	pVehicle->SetPosition((CVector(midPos.x, midPos.y, actualBehindZ) + CVector(posTarget.x, posTarget.y, actualAheadZ)) / 2);
	pVehicle->GetMatrix().GetPosition().z += pVehicle->GetHeightAboveRoad();
}

float CCarCtrl::FindSpeedMultiplier(float angleChange, float minAngle, float maxAngle, float coef)
{
	float angle = Abs(LimitRadianAngle(angleChange));
	float n = angle - minAngle;
	n = Max(0.0f, n);
	float d = maxAngle - minAngle;
	float mult = 1.0f - n / d * (1.0f - coef);
	if (n > d)
		return coef;
	return mult;
}

void CCarCtrl::SteerAICarWithPhysics(CVehicle* pVehicle)
{
	float swerve;
	float accel;
	float brake;
	bool handbrake;
#if REAL_GAMECUBE
	GcTraceScriptedIntroVelocitySample(pVehicle, "steer-physics-entry");
#endif
	switch (pVehicle->AutoPilot.m_nTempAction){
	case TEMPACT_WAIT:
		swerve = 0.0f;
		accel = 0.0f;
		brake = 0.2f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction){
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
			pVehicle->AutoPilot.m_nAntiReverseTimer = CTimer::GetTimeInMilliseconds();
			pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds();
		}
		break;
	case TEMPACT_REVERSE:
		SteerAICarWithPhysics_OnlyMission(pVehicle, &swerve, &accel, &brake, &handbrake);
		handbrake = false;
		swerve = -swerve;
		if (DotProduct(pVehicle->GetMoveSpeed(), pVehicle->GetForward()) > 0.04f){
			accel = 0.0f;
			brake = 0.5f;
		}else{
			accel = -0.5f;
			brake = 0.0f;
		}
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_HANDBRAKETURNLEFT:
		swerve = 1.0f;
		accel = 0.0f;
		brake = 0.0f;
		handbrake = true;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_HANDBRAKETURNRIGHT:
		swerve = -1.0f;
		accel = 0.0f;
		brake = 0.0f;
		handbrake = true;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_HANDBRAKESTRAIGHT:
		swerve = 0.0f;
		accel = 0.0f;
		brake = 0.0f;
		handbrake = true;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_TURNLEFT:
		swerve = 1.0f;
		accel = 1.0f;
		brake = 0.0f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_TURNRIGHT:
		swerve = -1.0f;
		accel = 1.0f;
		brake = 0.0f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_GOFORWARD:
		swerve = 0.0f;
		accel = 0.5f;
		brake = 0.0f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	case TEMPACT_SWERVELEFT:
	case TEMPACT_SWERVERIGHT:
		swerve = (pVehicle->AutoPilot.m_nTempAction == TEMPACT_SWERVERIGHT) ? 0.15f : -0.15f;
		accel = 0.0f;
		brake = 0.001f;
		handbrake = false;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction - 1000)
			swerve = -swerve;
		if (CTimer::GetTimeInMilliseconds() > pVehicle->AutoPilot.m_nTimeTempAction)
			pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
		break;
	default:
		SteerAICarWithPhysics_OnlyMission(pVehicle, &swerve, &accel, &brake, &handbrake);
		break;
	}
	pVehicle->m_fSteerAngle = swerve;
	pVehicle->bIsHandbrakeOn = handbrake;
	pVehicle->m_fGasPedal = accel;
	pVehicle->m_fBrakePedal = brake;
}

void CCarCtrl::SteerAICarWithPhysics_OnlyMission(CVehicle* pVehicle, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
#if REAL_GAMECUBE
	GcPreventScriptedIntroStraightMission(pVehicle);
#endif
	switch (pVehicle->AutoPilot.m_nCarMission) {
	case MISSION_NONE:
		*pSwerve = 0.0f;
		*pAccel = 0.0f;
		*pBrake = 0.5f;
		*pHandbrake = true;
		return;
	case MISSION_CRUISE:
	case MISSION_RAMPLAYER_FARAWAY:
	case MISSION_BLOCKPLAYER_FARAWAY:
	case MISSION_GOTOCOORDS:
	case MISSION_GOTOCOORDS_ACCURATE:
	case MISSION_RAMCAR_FARAWAY:
	case MISSION_BLOCKCAR_FARAWAY:
		if (pVehicle->AutoPilot.m_bIgnorePathfinding) {
			*pSwerve = 0.0f;
			*pAccel = 1.0f;
			*pBrake = 0.0f;
			*pHandbrake = false;
		}else
			SteerAICarWithPhysicsFollowPath(pVehicle, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_RAMPLAYER_CLOSE:
	{
		CVector2D targetPos = FindPlayerCoors();
		if (FindPlayerVehicle()){
			if (pVehicle->m_randomSeed & 1 && DotProduct(FindPlayerVehicle()->GetForward(), pVehicle->GetForward()) > 0.5f){
				float targetWidth = FindPlayerVehicle()->GetColModel()->boundingBox.max.x;
				float ownWidth = pVehicle->GetColModel()->boundingBox.max.x;
				if (pVehicle->m_randomSeed & 2){
					targetPos += (targetWidth + ownWidth - 0.2f) * FindPlayerVehicle()->GetRight();
				}else{
					targetPos -= (targetWidth + ownWidth - 0.2f) * FindPlayerVehicle()->GetRight();
				}
				float targetSpeed = FindPlayerVehicle()->GetMoveSpeed().Magnitude();
				float distanceToTarget = ((CVector2D)pVehicle->GetPosition() - targetPos).Magnitude();
				if (12.0f * targetSpeed + 2.0f > distanceToTarget && pVehicle->AutoPilot.m_nTempAction == TEMPACT_NONE){
					pVehicle->AutoPilot.m_nTempAction = (pVehicle->m_randomSeed & 2) ? TEMPACT_TURNLEFT : TEMPACT_TURNRIGHT;
					pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds() + 250;
				}
			}else{
				targetPos += FindPlayerVehicle()->GetRight() / 160 * ((pVehicle->m_randomSeed & 0xFF) - 128);
			}
		}
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, FindPlayerVehicle(), targetPos.x, targetPos.y, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	}
	case MISSION_BLOCKPLAYER_CLOSE:
		SteerAICarWithPhysicsTryingToBlockTarget(pVehicle, FindPlayerCoors().x, FindPlayerCoors().y,
			FindPlayerSpeed().x, FindPlayerSpeed().y, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_BLOCKPLAYER_HANDBRAKESTOP:
		SteerAICarWithPhysicsTryingToBlockTarget_Stop(pVehicle, FindPlayerCoors().x, FindPlayerCoors().y,
			FindPlayerSpeed().x, FindPlayerSpeed().y, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_WAITFORDELETION:
	case MISSION_HELI_LAND:
		return;
	case MISSION_GOTOCOORDS_STRAIGHT:
	case MISSION_GOTO_COORDS_STRAIGHT_ACCURATE:
#if REAL_GAMECUBE
	{
		CVector2D terminalCarryTarget;
		if (GcGetScriptedIntroTerminalRoadGuideTarget(pVehicle, &terminalCarryTarget)) {
			GcTraceAdmiralPath(pVehicle, "goto-straight-carry",
				terminalCarryTarget.x, terminalCarryTarget.y);
			GcTraceAdmiralTargetComparison(pVehicle, "goto-straight-carry", terminalCarryTarget);
			SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
				terminalCarryTarget.x, terminalCarryTarget.y,
				pSwerve, pAccel, pBrake, pHandbrake);
			return;
		}

		CVector2D segmentTarget;
		if (!GcIsScriptedIntroTightFinalApproach(pVehicle) &&
		    GcGetScriptedIntroSegmentContinuationTarget(pVehicle, &segmentTarget)) {
			GcTraceAdmiralPath(pVehicle, "goto-straight-segment",
				segmentTarget.x, segmentTarget.y);
			GcTraceAdmiralTargetComparison(pVehicle, "goto-straight-segment", segmentTarget);
			SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
				segmentTarget.x, segmentTarget.y,
				pSwerve, pAccel, pBrake, pHandbrake);
			return;
		}

		CVector2D approachTarget;
		if (GcGetScriptedIntroHeadingTarget(pVehicle, &approachTarget)) {
			GcTraceAdmiralPath(pVehicle, "goto-straight-approach",
				approachTarget.x, approachTarget.y);
			GcTraceAdmiralTargetComparison(pVehicle, "goto-straight-approach", approachTarget);
			SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
				approachTarget.x, approachTarget.y,
				pSwerve, pAccel, pBrake, pHandbrake);
			return;
		}
	}
#endif
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
			pVehicle->AutoPilot.m_vecDestinationCoors.x, pVehicle->AutoPilot.m_vecDestinationCoors.y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_EMERGENCYVEHICLE_STOP:
	case MISSION_STOP_FOREVER:
		*pSwerve = 0.0f;
		*pAccel = 0.0f;
		*pHandbrake = true;
		*pBrake = 0.5f;
		return;
	case MISSION_GOTOCOORDS_ASTHECROWSWIMS:
		SteerAIBoatWithPhysicsHeadingForTarget(pVehicle,
			pVehicle->AutoPilot.m_vecDestinationCoors.x, pVehicle->AutoPilot.m_vecDestinationCoors.y,
			pSwerve, pAccel, pBrake);
		*pHandbrake = false;
		return;
	case MISSION_RAMCAR_CLOSE:
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, pVehicle->AutoPilot.m_pTargetCar,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().x, pVehicle->AutoPilot.m_pTargetCar->GetPosition().y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_BLOCKCAR_CLOSE:
		SteerAICarWithPhysicsTryingToBlockTarget(pVehicle,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().x,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().y,
			pVehicle->AutoPilot.m_pTargetCar->GetMoveSpeed().x,
			pVehicle->AutoPilot.m_pTargetCar->GetMoveSpeed().y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_BLOCKCAR_HANDBRAKESTOP:
		SteerAICarWithPhysicsTryingToBlockTarget_Stop(pVehicle,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().x,
			pVehicle->AutoPilot.m_pTargetCar->GetPosition().y,
			pVehicle->AutoPilot.m_pTargetCar->GetMoveSpeed().x,
			pVehicle->AutoPilot.m_pTargetCar->GetMoveSpeed().y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_HELI_FLYTOCOORS:
		SteerAIHeliTowardsTargetCoors((CAutomobile*)pVehicle);
		return;
	case MISSION_ATTACKPLAYER:
		SteerAIBoatWithPhysicsAttackingPlayer(pVehicle, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_PLANE_FLYTOCOORS:
		SteerAIPlaneTowardsTargetCoors((CAutomobile*)pVehicle);
		return;
	case MISSION_SLOWLY_DRIVE_TOWARDS_PLAYER_1:
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
			pVehicle->AutoPilot.m_vecDestinationCoors.x, pVehicle->AutoPilot.m_vecDestinationCoors.y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_SLOWLY_DRIVE_TOWARDS_PLAYER_2:
		SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil, FindPlayerCoors().x, FindPlayerCoors().y,
			pSwerve, pAccel, pBrake, pHandbrake);
		return;
	case MISSION_BLOCKPLAYER_FORWARDANDBACK:
		SteerAICarBlockingPlayerForwardAndBack(pVehicle, pSwerve, pAccel, pBrake, pHandbrake);
		return;
	default:
		assert(0);
		return;
	}
}

void CCarCtrl::SteerAICarBlockingPlayerForwardAndBack(CVehicle* pVehicle, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	*pSwerve = 0.0f;
	*pHandbrake = false;
	CVector player = FindPlayerSpeed() + 0.1f * FindPlayerEntity()->GetForward();
	player.z = 0.0f;
	CVector right(pVehicle->GetRight().x, pVehicle->GetRight().y, 0.0f);
	right.Normalise();
	CVector forward(pVehicle->GetForward().x, pVehicle->GetForward().y, 0.0f);
	forward.Normalise();
	float dpPlayerAndRight = DotProduct(player, right);
	if (dpPlayerAndRight == 0.0f)
		dpPlayerAndRight = 0.01f;
	float dpDiffAndRight = -DotProduct((FindPlayerCoors() - pVehicle->GetPosition()), right) / dpPlayerAndRight;
	if (dpDiffAndRight < 0.0f) {
		*pAccel = 0.0f;
		*pBrake = 0.0f;
		return;
	}
	float dpSpeedAndForward = DotProduct(pVehicle->GetMoveSpeed(), forward);
	float dpPlayerAndForward = DotProduct(player, forward);
	float dpDiffAndForward = DotProduct((FindPlayerCoors() - pVehicle->GetPosition()), forward);
	float multiplier = dpPlayerAndForward * dpDiffAndRight + dpDiffAndForward - dpSpeedAndForward * dpDiffAndRight;
	if (multiplier > 0) {
		*pAccel = Min(1.0f, 0.1f * multiplier);
		*pBrake = 0.0f;
	}
	else if (dpSpeedAndForward > 0) {
		*pAccel = 0.0f;
		*pBrake = Min(1.0f, -0.1f * multiplier);
		if (*pBrake > 0.95f)
			*pHandbrake = true;
	}
	else {
		*pAccel = Max(-1.0f, 0.1f * multiplier);
		*pBrake = 0.0f;
	}
}

void CCarCtrl::SteerAIBoatWithPhysicsHeadingForTarget(CVehicle* pVehicle, float targetX, float targetY, float* pSwerve, float* pAccel, float* pBrake)
{
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
	float angleToTarget = CGeneral::GetATanOfXY(targetX - pVehicle->GetPosition().x, targetY - pVehicle->GetPosition().y);
	float angleForward = CGeneral::GetATanOfXY(forward.x, forward.y);
	float steerAngle = LimitRadianAngle(angleToTarget - angleForward);
	steerAngle = Clamp(steerAngle, -DEFAULT_MAX_STEER_ANGLE, DEFAULT_MAX_STEER_ANGLE);
#ifdef FIX_BUGS
	float speedTarget = pVehicle->AutoPilot.GetCruiseSpeed();
#else
	float speedTarget = pVehicle->AutoPilot.m_nCruiseSpeed;
#endif
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float speedDiff = speedTarget - currentSpeed;
	if (speedDiff <= 0.0f) {
		speedDiff < -5.0f ? *pAccel = -0.2f : *pAccel = -0.1f;
		steerAngle *= -1;
	}
	else if (speedDiff / currentSpeed > 0.25f) {
		*pAccel = 1.0f;
	}
	else {
		*pAccel = 1.0f - (0.25f - speedDiff / currentSpeed) * 4.0f;
	}
	*pBrake = 0.0f;
	*pSwerve = steerAngle;
}

void CCarCtrl::SteerAIBoatWithPhysicsAttackingPlayer(CVehicle* pVehicle, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	float distanceToPlayer = (FindPlayerCoors() - pVehicle->GetPosition()).Magnitude();
	float projection = Min(distanceToPlayer / 20.0f, 2.0f);
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
	CVector2D vecToProjection = FindPlayerCoors() + FindPlayerSpeed() * projection * GAME_SPEED_TO_CARAI_SPEED;
	float angleToTarget = CGeneral::GetATanOfXY(vecToProjection.x - pVehicle->GetPosition().x, vecToProjection.y - pVehicle->GetPosition().y);
	float angleForward = CGeneral::GetATanOfXY(forward.x, forward.y);
	float steerAngle = LimitRadianAngle(angleToTarget - angleForward);
#ifdef FIX_BUGS
	float speedTarget = pVehicle->AutoPilot.GetCruiseSpeed();
#else
	float speedTarget = pVehicle->AutoPilot.m_nCruiseSpeed;
#endif
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float speedDiff = speedTarget - currentSpeed;
	if (speedDiff <= 0.0f) {
		speedDiff < -5.0f ? *pAccel = -0.2f : *pAccel = -0.1f;
	}
	else if (speedDiff / currentSpeed > 0.25f) {
		*pAccel = 1.0f;
	}
	else {
		*pAccel = 1.0f - (0.25f - speedDiff / currentSpeed) * 4.0f;
	}
	*pBrake = 0.0f;
	*pSwerve = steerAngle;
	*pHandbrake = false;
	if (pVehicle->GetModelIndex() == MI_PREDATOR && distanceToPlayer < 40.0f && steerAngle < 0.15f)
		pVehicle->FireFixedMachineGuns();
}

#if REAL_GAMECUBE
static bool
GcShouldSuppressScriptedIntroHandbrake(CVehicle *pVehicle, float steerAngle)
{
	if (!GcIsScriptedIntroAdmiral(pVehicle))
		return false;
	if (Abs(steerAngle) <= MIN_ANGLE_TO_APPLY_HANDBRAKE)
		return false;

	float finalDistance = GcGetScriptedIntroDistanceToFinal(pVehicle);
	if (finalDistance <= 2.25f)
		return false;
	if (GcShouldContinueScriptedIntroTerminalRoadSegment(pVehicle))
		return true;
	if (!GcIsScriptedIntroTightFinalApproach(pVehicle))
		return true;
	return finalDistance > 4.0f;
}
#endif

float CCarCtrl::FindMaxSteerAngle(CVehicle* pVehicle)
{
	return pVehicle->GetModelIndex() == MI_ENFORCER ? 0.7f : DEFAULT_MAX_STEER_ANGLE;
}

void CCarCtrl::SteerAIHeliTowardsTargetCoors(CAutomobile* pHeli)
{
	if (pHeli->m_aWheelSpeed[1] < 0.22f)
		pHeli->m_aWheelSpeed[1] += 0.001f;
	if (pHeli->m_aWheelSpeed[1] < 0.15f)
		return;
	CVector2D vecToTarget = pHeli->AutoPilot.m_vecDestinationCoors - pHeli->GetPosition();
	float distanceToTarget = vecToTarget.Magnitude();
#ifdef FIX_BUGS
	float speed = pHeli->AutoPilot.GetCruiseSpeed() * 0.01f;
#else
	float speed = pHeli->AutoPilot.m_nCruiseSpeed * 0.01f;
#endif
	if (distanceToTarget <= 100.0f)
	{
		if (distanceToTarget > 75.0f)
			speed *= 0.7f;
		else if (distanceToTarget > 10.0f)
			speed *= 0.4f;
		else
			speed *= 0.2f;
	}
	vecToTarget.Normalise();
	CVector2D vecAdvanceThisFrame(vecToTarget * speed);
	float resistance = Pow(0.997f, CTimer::GetTimeStep());
	pHeli->m_vecMoveSpeed.x *= resistance;
	pHeli->m_vecMoveSpeed.y *= resistance;
	CVector2D vecSpeedDirection = vecAdvanceThisFrame - pHeli->m_vecMoveSpeed;
	float vecSpeedChangeLength = vecSpeedDirection.Magnitude();
	vecSpeedDirection.Normalise();
	float changeMultiplier = 0.002f * CTimer::GetTimeStep();
	if (distanceToTarget < 5.0f)
		changeMultiplier /= 5.0f;
	if (vecSpeedChangeLength < changeMultiplier)
		pHeli->SetMoveSpeed(vecAdvanceThisFrame.x, vecAdvanceThisFrame.y, pHeli->GetMoveSpeed().z);
	else
		pHeli->AddToMoveSpeed(vecSpeedDirection * changeMultiplier);
	pHeli->GetMatrix().Translate(CTimer::GetTimeStep() * pHeli->GetMoveSpeed().x, CTimer::GetTimeStep() * pHeli->GetMoveSpeed().y, 0.0f);
	float ZTarget = pHeli->AutoPilot.m_vecDestinationCoors.z;
	if (CTimer::GetTimeInMilliseconds() & 0x800) // switch every ~2 seconds
		ZTarget += 2.0f;
	float ZSpeedTarget = (ZTarget - pHeli->GetPosition().z) * 0.01f;
	float ZSpeedChangeTarget = ZSpeedTarget - pHeli->GetMoveSpeed().z;
	float ZSpeedChangeMax = 0.001f * CTimer::GetTimeStep();
	if (!pHeli->bHeliDestroyed) {
		if (Abs(ZSpeedChangeTarget) < ZSpeedChangeMax)
			pHeli->SetMoveSpeed(pHeli->GetMoveSpeed().x, pHeli->GetMoveSpeed().y, ZSpeedTarget);
		else if (ZSpeedChangeTarget < 0.0f)
			pHeli->AddToMoveSpeed(0.0f, 0.0f, -ZSpeedChangeMax);
		else
			pHeli->AddToMoveSpeed(0.0f, 0.0f, 1.5f * ZSpeedChangeMax);
	}
	pHeli->GetMatrix().Translate(0.0f, 0.0f, CTimer::GetTimeStep() * pHeli->GetMoveSpeed().z);
	pHeli->m_vecTurnSpeed.z *= Pow(0.99f, CTimer::GetTimeStep());
	float ZTurnSpeedTarget;
	if (distanceToTarget < 8.0f && pHeli->m_fHeliOrientation < 0.0f)
		ZTurnSpeedTarget = 0.0f;
	else {
		float fAngleTarget = CGeneral::GetATanOfXY(vecToTarget.x, vecToTarget.y) + PI;
		if (pHeli->m_fHeliOrientation >= 0.0f)
			fAngleTarget = pHeli->m_fHeliOrientation;
		fAngleTarget -= pHeli->m_fOrientation;
		while (fAngleTarget < -PI)
			fAngleTarget += TWOPI;
		while (fAngleTarget > PI)
			fAngleTarget -= TWOPI;
		if (Abs(fAngleTarget) <= 0.4f)
			ZTurnSpeedTarget = 0.0f;
		else if (fAngleTarget < 0.0f)
			ZTurnSpeedTarget = -0.03f;
		else
			ZTurnSpeedTarget = 0.03f;
	}
	float ZTurnSpeedChangeTarget = ZTurnSpeedTarget - pHeli->GetTurnSpeed().z;
	float ZTurnSpeedLimit = 0.0002f * CTimer::GetTimeStep();
	if (Abs(ZTurnSpeedChangeTarget) < ZTurnSpeedLimit)
		pHeli->m_vecTurnSpeed.z = ZTurnSpeedTarget;
	else if (ZTurnSpeedChangeTarget < 0.0f)
		pHeli->m_vecTurnSpeed.z -= ZTurnSpeedLimit;
	else
		pHeli->m_vecTurnSpeed.z += ZTurnSpeedLimit;
	pHeli->m_fOrientation += pHeli->GetTurnSpeed().z * CTimer::GetTimeStep();
	CVector up;
	if (pHeli->bHeliMinimumTilt)
		up = CVector(0.5f * pHeli->GetMoveSpeed().x, 0.5f * pHeli->GetMoveSpeed().y, 1.0f);
	else
		up = CVector(3.0f * pHeli->GetMoveSpeed().x, 3.0f * pHeli->GetMoveSpeed().y, 1.0f);
	up.Normalise();
	CVector forward(Cos(pHeli->m_fOrientation), Sin(pHeli->m_fOrientation), 0.0f);
	CVector right = CrossProduct(up, forward);
	forward = CrossProduct(up, right);
	pHeli->GetMatrix().GetRight() = right;
	pHeli->GetMatrix().GetForward() = forward;
	pHeli->GetMatrix().GetUp() = up;
}

void CCarCtrl::SteerAIPlaneTowardsTargetCoors(CAutomobile* pPlane)
{
	CVector2D vecToTarget = pPlane->AutoPilot.m_vecDestinationCoors - pPlane->GetPosition();
	float fForwardZ = (pPlane->AutoPilot.m_vecDestinationCoors.z - pPlane->GetPosition().z) / vecToTarget.Magnitude();
	fForwardZ = Clamp(fForwardZ, -0.3f, 0.3f);
	float angle = CGeneral::GetATanOfXY(vecToTarget.x, vecToTarget.y);
	while (angle > TWOPI)
		angle -= TWOPI;
	float difference = LimitRadianAngle(angle - pPlane->m_fOrientation);
	float steer = difference > 0.0f ? 0.04f : -0.04f;
	if (Abs(difference) < 0.2f)
		steer *= 5.0f * Abs(difference);
	pPlane->m_fPlaneSteer *= Pow(0.96f, CTimer::GetTimeStep());
	float steerChange = steer - pPlane->m_fPlaneSteer;
	float maxChange = 0.003f * CTimer::GetTimeStep();
	if (Abs(steerChange) < maxChange)
		pPlane->m_fPlaneSteer = steer;
	else if (steerChange < 0.0f)
		pPlane->m_fPlaneSteer -= maxChange;
	else
		pPlane->m_fPlaneSteer += maxChange;
	pPlane->m_fOrientation += pPlane->m_fPlaneSteer * CTimer::GetTimeStep();
	CVector up(0.0f, 0.0f, 1.0f);
	up.Normalise();
	CVector forward(Cos(pPlane->m_fOrientation), Sin(pPlane->m_fOrientation), fForwardZ);
	forward.Normalise();
	CVector right = CrossProduct(up, forward);
	right.z -= 5.0f * pPlane->m_fPlaneSteer;
	right.Normalise();
	up = CrossProduct(forward, right);
	up.Normalise();
	right = CrossProduct(forward, up);
	pPlane->GetMatrix().GetRight() = right;
	pPlane->GetMatrix().GetForward() = forward;
	pPlane->GetMatrix().GetUp() = up;
	float newSplit = 1.0f - Pow(0.95f, CTimer::GetTimeStep());
	float oldSplit = 1.0f - newSplit;
#ifdef FIX_BUGS
	pPlane->m_vecMoveSpeed = pPlane->m_vecMoveSpeed * oldSplit + pPlane->AutoPilot.GetCruiseSpeed() * 0.01f * forward * newSplit;
#else
	pPlane->m_vecMoveSpeed = pPlane->m_vecMoveSpeed * oldSplit + pPlane->AutoPilot.m_nCruiseSpeed * 0.01f * forward * newSplit;
#endif
	pPlane->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
}

void CCarCtrl::SteerAICarWithPhysicsFollowPath(CVehicle* pVehicle, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
#if REAL_GAMECUBE
	const int drivingStyle = GcGetEffectiveDrivingStyle(pVehicle);
#else
	const int drivingStyle = pVehicle->AutoPilot.m_nDrivingStyle;
#endif
	CCarPathLink* pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
	CCarPathLink* pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
	CVector2D currentPathLinkForward(pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection,
		pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection);
	float nextPathLinkForwardX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
	float nextPathLinkForwardY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	CVector2D positionOnCurrentLinkIncludingLane(
		pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.y,
		pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.x);
	CVector2D positionOnNextLinkIncludingLane(
		pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY,
		pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX);
	CVector2D distanceToNextNode = (CVector2D)pVehicle->GetPosition() - positionOnCurrentLinkIncludingLane;
	float scalarDistanceToNextNode = distanceToNextNode.Magnitude();
	CVector2D distanceBetweenNodes = positionOnNextLinkIncludingLane - positionOnCurrentLinkIncludingLane;
	float dp = DotProduct2D(distanceBetweenNodes, distanceToNextNode);
	float distanceBetweenNodesMagnitude = distanceBetweenNodes.Magnitude();
	float facingDp = 0.0f;
	if (scalarDistanceToNextNode > 0.0001f && distanceBetweenNodesMagnitude > 0.0001f)
		facingDp = dp / (scalarDistanceToNextNode * distanceBetweenNodesMagnitude);
	bool switchNearNode = scalarDistanceToNextNode < DISTANCE_TO_NEXT_NODE_TO_SELECT_NEW;
	bool switchFacingNode = dp > 0.0f && scalarDistanceToNextNode < DISTANCE_TO_FACING_NEXT_NODE_TO_SELECT_NEW;
	bool switchOvershoot = facingDp > 0.7f;
	bool switchSameLink = pVehicle->AutoPilot.m_nNextPathNodeInfo == pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	int switchReason = (switchNearNode ? 1 : 0) |
		(switchFacingNode ? 2 : 0) |
		(switchOvershoot ? 4 : 0) |
		(switchSameLink ? 8 : 0);
#if REAL_GAMECUBE
	if (switchReason != 0 &&
	    GcHasScriptedIntroActiveSegmentRoute(pVehicle) &&
	    GcHasQueuedScriptedIntroActiveTargetDuplicate(pVehicle)) {
		CVector2D segmentTarget;
		if (GcGetScriptedIntroSegmentContinuationTarget(pVehicle, &segmentTarget)) {
			GcTraceAdmiralSwitchGate(pVehicle, "follow-defer-active-handoff",
				scalarDistanceToNextNode, dp, facingDp,
				switchNearNode, switchFacingNode,
				switchOvershoot, switchSameLink);
			GcTraceAdmiralPath(pVehicle, "follow-hold-active-handoff",
				segmentTarget.x, segmentTarget.y);
			GcTraceAdmiralTargetComparison(pVehicle, "follow-hold-active-handoff", segmentTarget);
			SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
				segmentTarget.x, segmentTarget.y,
				pSwerve, pAccel, pBrake, pHandbrake);
			return;
		}
	}

	if (GcIsScriptedIntroAdmiral(pVehicle) &&
	    pVehicle->AutoPilot.m_nPathFindNodesCount == 0 &&
	    pVehicle->AutoPilot.m_nCurrentRouteNode != 0 &&
	    pVehicle->AutoPilot.m_nNextRouteNode != 0 &&
	    pVehicle->AutoPilot.m_nCurrentRouteNode != pVehicle->AutoPilot.m_nNextRouteNode) {
		CVector2D terminalCarryTarget;
		if (GcGetScriptedIntroTerminalRoadGuideTarget(pVehicle, &terminalCarryTarget)) {
			GcTraceAdmiralPath(pVehicle, "follow-terminal-carry",
				terminalCarryTarget.x, terminalCarryTarget.y);
			GcTraceAdmiralTargetComparison(pVehicle, "follow-terminal-carry", terminalCarryTarget);
			SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
				terminalCarryTarget.x, terminalCarryTarget.y,
				pSwerve, pAccel, pBrake, pHandbrake);
			return;
		}

		CVector2D approachTarget;
		if (GcGetScriptedIntroHeadingTarget(pVehicle, &approachTarget)) {
			GcTraceAdmiralPath(pVehicle, "follow-terminal-approach",
				approachTarget.x, approachTarget.y);
			GcTraceAdmiralTargetComparison(pVehicle, "follow-terminal-approach", approachTarget);
			SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
				approachTarget.x, approachTarget.y,
				pSwerve, pAccel, pBrake, pHandbrake);
			return;
		}
	}
#endif
	bool switchedNode = false;
	if (switchReason != 0){
		switchedNode = true;
		if (PickNextNodeAccordingStrategy(pVehicle)) {
			switch (pVehicle->AutoPilot.m_nCarMission){
			case MISSION_GOTOCOORDS:
#if REAL_GAMECUBE
				if (GcIsScriptedIntroAdmiral(pVehicle)) {
					if (pVehicle->AutoPilot.m_nCurrentRouteNode != 0 &&
					    pVehicle->AutoPilot.m_nNextRouteNode != 0 &&
					    pVehicle->AutoPilot.m_nCurrentRouteNode != pVehicle->AutoPilot.m_nNextRouteNode) {
						switchedNode = false;
						break;
					}
					CVector2D introTarget;
					// Keep a single steering chain through the hotel entrance.
					// Switching from follow-path to goto-straight mid-turn was
					// creating the current double-steer shape: a mild preview turn
					// followed by a second, deeper pull from mission 9.
					if (GcGetScriptedIntroHeadingTarget(pVehicle, &introTarget)) {
						GcTraceAdmiralPath(pVehicle, "follow-switch-keep-path",
							introTarget.x, introTarget.y);
						GcTraceAdmiralTargetComparison(pVehicle, "follow-switch-keep-path", introTarget);
						SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
							introTarget.x, introTarget.y,
							pSwerve, pAccel, pBrake, pHandbrake);
						return;
					}
					GcTraceAdmiralPath(pVehicle, "follow-switch-fallback",
						pVehicle->AutoPilot.m_vecDestinationCoors.x,
						pVehicle->AutoPilot.m_vecDestinationCoors.y);
					SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
						pVehicle->AutoPilot.m_vecDestinationCoors.x,
						pVehicle->AutoPilot.m_vecDestinationCoors.y,
						pSwerve, pAccel, pBrake, pHandbrake);
					return;
				}
#endif
				pVehicle->AutoPilot.m_nCarMission = MISSION_GOTOCOORDS_STRAIGHT;
#if REAL_GAMECUBE
			{
				CVector2D finalApproachTarget;
				if (GcGetScriptedIntroHeadingTarget(pVehicle, &finalApproachTarget)) {
					GcTraceAdmiralPath(pVehicle, "follow-switch-approach",
						finalApproachTarget.x, finalApproachTarget.y);
					GcTraceAdmiralTargetComparison(pVehicle, "follow-switch-approach", finalApproachTarget);
					SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
						finalApproachTarget.x, finalApproachTarget.y,
						pSwerve, pAccel, pBrake, pHandbrake);
					return;
				}
				GcTraceAdmiralPath(pVehicle, "follow-switch-straight",
					pVehicle->AutoPilot.m_vecDestinationCoors.x,
					pVehicle->AutoPilot.m_vecDestinationCoors.y);
			}
#endif
				SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil, pVehicle->AutoPilot.m_vecDestinationCoors.x,
					pVehicle->AutoPilot.m_vecDestinationCoors.y, pSwerve, pAccel, pBrake, pHandbrake);
				return;
			case MISSION_GOTOCOORDS_ACCURATE:
#if REAL_GAMECUBE
				if (GcIsScriptedIntroAdmiral(pVehicle)) {
					if (pVehicle->AutoPilot.m_nCurrentRouteNode != 0 &&
					    pVehicle->AutoPilot.m_nNextRouteNode != 0 &&
					    pVehicle->AutoPilot.m_nCurrentRouteNode != pVehicle->AutoPilot.m_nNextRouteNode) {
						switchedNode = false;
						break;
					}
					CVector2D introTarget;
					if (GcGetScriptedIntroHeadingTarget(pVehicle, &introTarget)) {
						GcTraceAdmiralPath(pVehicle, "follow-switch-keep-path-accurate",
							introTarget.x, introTarget.y);
						GcTraceAdmiralTargetComparison(pVehicle, "follow-switch-keep-path-accurate", introTarget);
						SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
							introTarget.x, introTarget.y,
							pSwerve, pAccel, pBrake, pHandbrake);
						return;
					}
					GcTraceAdmiralPath(pVehicle, "follow-switch-fallback-accurate",
						pVehicle->AutoPilot.m_vecDestinationCoors.x,
						pVehicle->AutoPilot.m_vecDestinationCoors.y);
					SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil,
						pVehicle->AutoPilot.m_vecDestinationCoors.x,
						pVehicle->AutoPilot.m_vecDestinationCoors.y,
						pSwerve, pAccel, pBrake, pHandbrake);
					return;
				}
#endif
				pVehicle->AutoPilot.m_nCarMission = MISSION_GOTO_COORDS_STRAIGHT_ACCURATE;
#if REAL_GAMECUBE
				GcTraceAdmiralPath(pVehicle, "follow-switch-accurate",
					pVehicle->AutoPilot.m_vecDestinationCoors.x,
					pVehicle->AutoPilot.m_vecDestinationCoors.y);
#endif
				SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil, pVehicle->AutoPilot.m_vecDestinationCoors.x,
					pVehicle->AutoPilot.m_vecDestinationCoors.y, pSwerve, pAccel, pBrake, pHandbrake);
				return;
			default: break;
			}
		}
		pCurrentLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nCurrentPathNodeInfo];
		scalarDistanceToNextNode = CVector2D(
			pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.y - pVehicle->GetPosition().x,
			pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.x - pVehicle->GetPosition().y).Magnitude();		
		pNextLink = &ThePaths.m_carPathLinks[pVehicle->AutoPilot.m_nNextPathNodeInfo];
		currentPathLinkForward.x = pCurrentLink->GetDirX() * pVehicle->AutoPilot.m_nCurrentDirection;
		currentPathLinkForward.y = pCurrentLink->GetDirY() * pVehicle->AutoPilot.m_nCurrentDirection;
		nextPathLinkForwardX = pNextLink->GetDirX() * pVehicle->AutoPilot.m_nNextDirection;
		nextPathLinkForwardY = pNextLink->GetDirY() * pVehicle->AutoPilot.m_nNextDirection;
	}
	positionOnCurrentLinkIncludingLane.x = pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.y;
	positionOnCurrentLinkIncludingLane.y = pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.x;
	positionOnNextLinkIncludingLane.x = pNextLink->GetX() + ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardY;
	positionOnNextLinkIncludingLane.y = pNextLink->GetY() - ((pVehicle->AutoPilot.m_nNextLane + pNextLink->OneWayLaneOffset()) * LANE_WIDTH) * nextPathLinkForwardX;
	distanceToNextNode = (CVector2D)pVehicle->GetPosition() - positionOnCurrentLinkIncludingLane;
	scalarDistanceToNextNode = distanceToNextNode.Magnitude();
	distanceBetweenNodes = positionOnNextLinkIncludingLane - positionOnCurrentLinkIncludingLane;
	dp = DotProduct2D(distanceBetweenNodes, distanceToNextNode);
	distanceBetweenNodesMagnitude = distanceBetweenNodes.Magnitude();
	facingDp = 0.0f;
	if (scalarDistanceToNextNode > 0.0001f && distanceBetweenNodesMagnitude > 0.0001f)
		facingDp = dp / (scalarDistanceToNextNode * distanceBetweenNodesMagnitude);
	CVector2D projectedPosition = positionOnCurrentLinkIncludingLane - currentPathLinkForward * scalarDistanceToNextNode * 0.4f;
#if REAL_GAMECUBE
	bool gcOverrideProjectedPosition = false;
	CVector2D gcProjectedPosition;
	const char *gcProjectedStage = nil;

	if (GcGetScriptedIntroActiveSegmentHandoffTarget(pVehicle, &gcProjectedPosition))
		gcProjectedStage = "follow-hold-active-segment";
	else if (pVehicle->AutoPilot.m_nPathFindNodesCount == 0 &&
	         !GcIsScriptedIntroTightFinalApproach(pVehicle) &&
	         GcGetScriptedIntroSegmentContinuationTarget(pVehicle, &gcProjectedPosition))
		gcProjectedStage = "follow-segment-continue";
	else if (GcIsHoldingScriptedIntroFinalSegment(pVehicle) &&
	         GcGetScriptedIntroHeadingTarget(pVehicle, &gcProjectedPosition))
		gcProjectedStage = "follow-held-approach";

	if (gcProjectedStage != nil) {
		projectedPosition = gcProjectedPosition;
		gcOverrideProjectedPosition = true;
		GcTraceAdmiralPath(pVehicle, gcProjectedStage,
			gcProjectedPosition.x, gcProjectedPosition.y);
		GcTraceAdmiralTargetComparison(pVehicle, gcProjectedStage, gcProjectedPosition);
	}

	if (!gcOverrideProjectedPosition &&
	    GcIsScriptedIntroAdmiral(pVehicle) &&
	    facingDp < 0.0f &&
	    scalarDistanceToNextNode > DISTANCE_TO_NEXT_NODE_TO_SELECT_NEW) {
		// The generic AI intentionally aims slightly behind the current path node to smooth traffic turns.
		// For the scripted intro Admiral this cuts the corner too early and produces the observed yaw drift.
		projectedPosition = positionOnCurrentLinkIncludingLane;
	}
#endif
	if (scalarDistanceToNextNode > DISTANCE_TO_NEXT_NODE_TO_CONSIDER_SLOWING_DOWN){
		projectedPosition.x = positionOnCurrentLinkIncludingLane.x;
		projectedPosition.y = positionOnCurrentLinkIncludingLane.y;
	}
	CVector2D distanceToProjectedPosition = projectedPosition - pVehicle->GetPosition();
	float angleCurrentLink = CGeneral::GetATanOfXY(distanceToProjectedPosition.x, distanceToProjectedPosition.y);
	float angleForward = CGeneral::GetATanOfXY(forward.x, forward.y);
	bool useTrafficAvoidance = drivingStyle == DRIVINGSTYLE_AVOID_CARS;
#if REAL_GAMECUBE
	if (useTrafficAvoidance && GcShouldBypassScriptedIntroTrafficAvoidance(pVehicle))
		useTrafficAvoidance = false;
#endif
	if (useTrafficAvoidance)
		angleCurrentLink = FindAngleToWeaveThroughTraffic(pVehicle, nil, angleCurrentLink, angleForward);
	float steerAngle = LimitRadianAngle(angleCurrentLink - angleForward);
	float maxAngle = FindMaxSteerAngle(pVehicle);
	steerAngle = Min(maxAngle, Max(-maxAngle, steerAngle));
	if (pVehicle->GetMoveSpeed().Magnitude() > MIN_SPEED_TO_START_LIMITING_STEER)
		steerAngle = Min(MAX_ANGLE_TO_STEER_AT_HIGH_SPEED, Max(-MAX_ANGLE_TO_STEER_AT_HIGH_SPEED, steerAngle));
	float currentForwardSpeed = DotProduct(pVehicle->GetMoveSpeed(), pVehicle->GetForward()) * GAME_SPEED_TO_CARAI_SPEED;
	float speedStyleMultiplier;
	switch (drivingStyle) {
	case DRIVINGSTYLE_STOP_FOR_CARS:
	case DRIVINGSTYLE_SLOW_DOWN_FOR_CARS:
	case DRIVINGSTYLE_STOP_FOR_CARS_IGNORE_LIGHTS:
		speedStyleMultiplier = FindMaximumSpeedForThisCarInTraffic(pVehicle);
#ifdef FIX_BUGS
		if (pVehicle->AutoPilot.GetCruiseSpeed() != 0)
			speedStyleMultiplier /= pVehicle->AutoPilot.GetCruiseSpeed();
#else
		speedStyleMultiplier /= pVehicle->AutoPilot.m_nCruiseSpeed;
#endif
		break;
	default:
		speedStyleMultiplier = 1.0f;
		break;
	}
	switch (drivingStyle) {
	case DRIVINGSTYLE_STOP_FOR_CARS:
	case DRIVINGSTYLE_SLOW_DOWN_FOR_CARS:
		if (CTrafficLights::ShouldCarStopForLight(pVehicle, false)){
			CCarAI::CarHasReasonToStop(pVehicle);
			speedStyleMultiplier = 0.0f;
		}
		break;
	default:
		break;
	}
	if (CTrafficLights::ShouldCarStopForBridge(pVehicle)){
		CCarAI::CarHasReasonToStop(pVehicle);
		speedStyleMultiplier = 0.0f;
	}
	CVector2D trajectory(pCurrentLink->GetX() + ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.y,
		pCurrentLink->GetY() - ((pVehicle->AutoPilot.m_nCurrentLane + pCurrentLink->OneWayLaneOffset()) * LANE_WIDTH) * currentPathLinkForward.x);
	trajectory -= pVehicle->GetPosition();
	float speedAngleMultiplier = FindSpeedMultiplier(
		CGeneral::GetATanOfXY(trajectory.x, trajectory.y) - angleForward,
		MIN_ANGLE_FOR_SPEED_LIMITING, MAX_ANGLE_FOR_SPEED_LIMITING, MIN_LOWERING_SPEED_COEFFICIENT);
	float tmpWideMultiplier = FindSpeedMultiplier(
		CGeneral::GetATanOfXY(currentPathLinkForward.x, currentPathLinkForward.y) -
		CGeneral::GetATanOfXY(nextPathLinkForwardX, nextPathLinkForwardY),
		MIN_ANGLE_FOR_SPEED_LIMITING_BETWEEN_NODES, MAX_ANGLE_FOR_SPEED_LIMITING, MIN_LOWERING_SPEED_COEFFICIENT);
	float speedNodesMultiplier;
	if (scalarDistanceToNextNode > DISTANCE_TO_NEXT_NODE_TO_CONSIDER_SLOWING_DOWN || pVehicle->AutoPilot.m_nCruiseSpeed < 12)
		speedNodesMultiplier = 1.0f;
	else
		speedNodesMultiplier = 1.0f -
			(1.0f - scalarDistanceToNextNode / DISTANCE_TO_NEXT_NODE_TO_CONSIDER_SLOWING_DOWN) *
			(1.0f - tmpWideMultiplier);
	float speedMultiplier = Min(speedStyleMultiplier, Min(speedAngleMultiplier, speedNodesMultiplier));
	float speed = pVehicle->AutoPilot.m_nCruiseSpeed * speedMultiplier;
	float speedDifference = speed - currentForwardSpeed;
	if (speed < 0.05f && speedDifference < 0.03f){
		*pBrake = 1.0f;
		*pAccel = 0.0f;
	}else if (speedDifference <= 0.0f){
		*pBrake = Min(0.5f, -speedDifference * 0.05f);
		*pAccel = 0.0f;
	}else if (currentForwardSpeed < 2.0f){
		*pBrake = 0.0f;
		*pAccel = Min(1.0f, speedDifference * 0.25f);
	}else{
		*pBrake = 0.0f;
		*pAccel = Min(1.0f, speedDifference * 0.125f);
	}
	*pSwerve = steerAngle;
	*pHandbrake = false;
#if REAL_GAMECUBE
	GcTraceAdmiralPathDecision(pVehicle, "follow-path",
		positionOnCurrentLinkIncludingLane.x, positionOnCurrentLinkIncludingLane.y,
		positionOnNextLinkIncludingLane.x, positionOnNextLinkIncludingLane.y,
		projectedPosition.x, projectedPosition.y,
		scalarDistanceToNextNode, distanceBetweenNodesMagnitude,
		dp, facingDp, switchReason, switchedNode,
		angleForward, angleCurrentLink, steerAngle, maxAngle,
		currentForwardSpeed, speed,
		speedStyleMultiplier, speedAngleMultiplier, speedNodesMultiplier,
		*pHandbrake);
	GcTraceAdmiralPath(pVehicle, "follow-path", projectedPosition.x, projectedPosition.y,
		*pSwerve, *pAccel, *pBrake, *pHandbrake);
#endif
}

void CCarCtrl::SteerAICarWithPhysicsHeadingForTarget(CVehicle* pVehicle, CPhysical* pTarget, float targetX, float targetY, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	*pHandbrake = false;
	CVector2D forward = pVehicle->GetForward();
	forward.Normalise();
#if REAL_GAMECUBE
	const int drivingStyle = GcGetEffectiveDrivingStyle(pVehicle);
#else
	const int drivingStyle = pVehicle->AutoPilot.m_nDrivingStyle;
#endif
	float angleToTarget = CGeneral::GetATanOfXY(targetX - pVehicle->GetPosition().x, targetY - pVehicle->GetPosition().y);
	float angleForward = CGeneral::GetATanOfXY(forward.x, forward.y);
	bool useTrafficAvoidance = drivingStyle == DRIVINGSTYLE_AVOID_CARS;
#if REAL_GAMECUBE
	if (useTrafficAvoidance && GcShouldBypassScriptedIntroTrafficAvoidance(pVehicle))
		useTrafficAvoidance = false;
#endif
	if (useTrafficAvoidance)
		angleToTarget = FindAngleToWeaveThroughTraffic(pVehicle, pTarget, angleToTarget, angleForward);
	float steerAngle = LimitRadianAngle(angleToTarget - angleForward);
	if (pVehicle->GetMoveSpeed().Magnitude() > MIN_SPEED_TO_APPLY_HANDBRAKE)
		if (ABS(steerAngle) > MIN_ANGLE_TO_APPLY_HANDBRAKE)
			*pHandbrake = true;
#if REAL_GAMECUBE
	if (*pHandbrake && GcShouldSuppressScriptedIntroHandbrake(pVehicle, steerAngle))
		*pHandbrake = false;
#endif
	float maxAngle = FindMaxSteerAngle(pVehicle);
	steerAngle = Min(maxAngle, Max(-maxAngle, steerAngle));
	float speedMultiplier = FindSpeedMultiplier(CGeneral::GetATanOfXY(targetX - pVehicle->GetPosition().x, targetY - pVehicle->GetPosition().y) - angleForward,
		MIN_ANGLE_FOR_SPEED_LIMITING, MAX_ANGLE_FOR_SPEED_LIMITING, MIN_LOWERING_SPEED_COEFFICIENT);
	float speedTarget = pVehicle->AutoPilot.m_nCruiseSpeed * speedMultiplier;
#if REAL_GAMECUBE
	speedTarget = GcLimitScriptedIntroApproachSpeed(pVehicle, speedTarget);
#endif
	float currentSpeed = pVehicle->GetMoveSpeed().Magnitude() * GAME_SPEED_TO_CARAI_SPEED;
	float speedDiff = speedTarget - currentSpeed;
	if (speedDiff <= 0.0f){
		*pAccel = 0.0f;
		*pBrake = Min(0.5f, -speedDiff / 20.0f);
	}else if (currentSpeed < 25.0f){
		*pAccel = Min(1.0f, speedDiff * 0.1f);
		*pBrake = 0.0f;
	}else{
		*pAccel = 1.0f;
		*pBrake = 0.0f;
	}
	*pSwerve = steerAngle;
#if REAL_GAMECUBE
	GcTraceAdmiralHeadingDecision(pVehicle, targetX, targetY,
		angleForward, angleToTarget, steerAngle, maxAngle,
		currentSpeed, speedTarget, speedMultiplier, *pHandbrake);
	GcTraceAdmiralPath(pVehicle, "heading-target", targetX, targetY,
		*pSwerve, *pAccel, *pBrake, *pHandbrake);
#endif
}

void CCarCtrl::SteerAICarWithPhysicsTryingToBlockTarget(CVehicle* pVehicle, float targetX, float targetY, float targetSpeedX, float targetSpeedY, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	CVector2D targetPos(targetX, targetY);
	CVector2D offset(targetSpeedX, targetSpeedY);
	float trajectoryLen = offset.Magnitude();
	if (trajectoryLen > MAX_SPEED_TO_ACCOUNT_IN_INTERCEPTING)
		offset *= MAX_SPEED_TO_ACCOUNT_IN_INTERCEPTING / trajectoryLen;
	targetPos += offset * GAME_SPEED_TO_CARAI_SPEED;
	pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
	SteerAICarWithPhysicsHeadingForTarget(pVehicle, nil, targetPos.x, targetPos.y, pSwerve, pAccel, pBrake, pHandbrake);
	if ((targetPos - pVehicle->GetPosition()).MagnitudeSqr() < SQR(DISTANCE_TO_SWITCH_FROM_BLOCK_TO_STOP))
		pVehicle->AutoPilot.m_nCarMission = (pVehicle->AutoPilot.m_nCarMission == MISSION_BLOCKCAR_CLOSE) ?
			MISSION_BLOCKCAR_HANDBRAKESTOP : MISSION_BLOCKPLAYER_HANDBRAKESTOP;
}

void CCarCtrl::SteerAICarWithPhysicsTryingToBlockTarget_Stop(CVehicle* pVehicle, float targetX, float targetY, float targetSpeedX, float targetSpeedY, float* pSwerve, float* pAccel, float* pBrake, bool* pHandbrake)
{
	*pSwerve = 0.0f;
	*pAccel = 0.0f;
	*pBrake = 1.0f;
	*pHandbrake = true;
	float distanceToTargetSqr = (CVector2D(targetX, targetY) - pVehicle->GetPosition()).MagnitudeSqr();
	if (distanceToTargetSqr > SQR(DISTANCE_TO_SWITCH_FROM_STOP_TO_BLOCK)){
		pVehicle->AutoPilot.m_nCarMission = (pVehicle->AutoPilot.m_nCarMission == MISSION_BLOCKCAR_HANDBRAKESTOP) ?
			MISSION_BLOCKCAR_CLOSE : MISSION_BLOCKPLAYER_CLOSE;
		return;
	}
	if (pVehicle->AutoPilot.m_nCarMission == MISSION_BLOCKCAR_HANDBRAKESTOP){
		if (((CVector2D)pVehicle->GetMoveSpeed()).MagnitudeSqr() < SQR(0.01f) &&
		  CVector2D(targetSpeedX, targetSpeedY).MagnitudeSqr() < SQR(0.02f) &&
		  pVehicle->bIsLawEnforcer){
			CCarAI::TellOccupantsToLeaveCar(pVehicle);
			pVehicle->AutoPilot.m_nCruiseSpeed = 0;
			pVehicle->AutoPilot.m_nCarMission = MISSION_NONE;
		}
	}else{
		if (FindPlayerVehicle() && FindPlayerVehicle()->GetMoveSpeed().Magnitude() < 0.05f)
#ifdef FIX_BUGS
			pVehicle->m_nTimeBlocked += CTimer::GetTimeStepInMilliseconds();
#else
			pVehicle->m_nTimeBlocked += 1000.0f / 60.0f * CTimer::GetTimeStep(); // very doubtful constant
#endif
		else
			pVehicle->m_nTimeBlocked = 0;
		if (FindPlayerVehicle() == nil || FindPlayerVehicle()->IsUpsideDown() ||
		  FindPlayerVehicle()->GetMoveSpeed().Magnitude() < 0.05f &&
		  pVehicle->m_nTimeBlocked > TIME_COPS_WAIT_TO_EXIT_AFTER_STOPPING){
			if (pVehicle->bIsLawEnforcer && distanceToTargetSqr < SQR(DISTANCE_TO_SWITCH_FROM_STOP_TO_BLOCK)){
				CCarAI::TellOccupantsToLeaveCar(pVehicle);
				pVehicle->AutoPilot.m_nCruiseSpeed = 0;
				pVehicle->AutoPilot.m_nCarMission = MISSION_NONE;
			}
		}
	}
}

void
CCarCtrl::RegisterVehicleOfInterest(CVehicle* pVehicle)
{
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (apCarsToKeep[i] == pVehicle) {
			aCarsToKeepTime[i] = CTimer::GetTimeInMilliseconds();
			return;
		}
	}
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (!apCarsToKeep[i]) {
			apCarsToKeep[i] = pVehicle;
			aCarsToKeepTime[i] = CTimer::GetTimeInMilliseconds();
			return;
		}
	}
	uint32 oldestCarWeKeepTime = UINT32_MAX;
	int oldestCarWeKeepIndex = 0;
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (apCarsToKeep[i] && aCarsToKeepTime[i] < oldestCarWeKeepTime) {
			oldestCarWeKeepTime = aCarsToKeepTime[i];
			oldestCarWeKeepIndex = i;
		}
	}
	apCarsToKeep[oldestCarWeKeepIndex] = pVehicle;
	aCarsToKeepTime[oldestCarWeKeepIndex] = CTimer::GetTimeInMilliseconds();
}

bool
CCarCtrl::IsThisVehicleInteresting(CVehicle* pVehicle)
{
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (apCarsToKeep[i] == pVehicle)
			return true;
	}
	return false;
}

void CCarCtrl::RemoveFromInterestingVehicleList(CVehicle* pVehicle)
{
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		if (apCarsToKeep[i] == pVehicle)
			apCarsToKeep[i] = nil;
	}
}

void CCarCtrl::ClearInterestingVehicleList()
{
	for (int i = 0; i < MAX_CARS_TO_KEEP; i++) {
		apCarsToKeep[i] = nil;
	}
}

void CCarCtrl::SwitchVehicleToRealPhysics(CVehicle* pVehicle)
{
#if REAL_GAMECUBE
	GcTraceAdmiralPath(pVehicle, "switch-real-before");
	const bool keepScriptedGotoMission = GcShouldKeepScriptedGotoMission(pVehicle);
	if (!keepScriptedGotoMission)
#endif
	pVehicle->AutoPilot.m_nCarMission = MISSION_CRUISE;
	pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
	pVehicle->AutoPilot.m_nAntiReverseTimer = CTimer::GetTimeInMilliseconds();
	pVehicle->AutoPilot.m_nTimeTempAction = CTimer::GetTimeInMilliseconds();
#if REAL_GAMECUBE
	if (keepScriptedGotoMission)
		GcTraceAdmiralPath(pVehicle, "switch-real-keep-mission");
	GcTraceAdmiralPath(pVehicle, "switch-real-after");
#endif
}

void CCarCtrl::JoinCarWithRoadSystem(CVehicle* pVehicle)
{
	pVehicle->AutoPilot.m_nPrevRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_nNextRouteNode = 0;
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = pVehicle->AutoPilot.m_nPreviousPathNodeInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo = 0;
	pVehicle->AutoPilot.m_nPathFindNodesCount = 0;
	int nodeId = ThePaths.FindNodeClosestToCoorsFavourDirection(pVehicle->GetPosition(), 0, pVehicle->GetForward().x, pVehicle->GetForward().y);
	CPathNode* pNode = &ThePaths.m_pathNodes[nodeId];
	int prevNodeId = -1;
	float minDistance = 999999.9f;
	for (int i = 0; i < pNode->numLinks; i++){
		int candidateId = ThePaths.ConnectedNode(i + pNode->firstLink);
		CPathNode* pCandidateNode = &ThePaths.m_pathNodes[candidateId];
		float distance = (pCandidateNode->GetPosition() - pNode->GetPosition()).Magnitude2D();
		if (distance < minDistance){
			minDistance = distance;
			prevNodeId = candidateId;
		}
	}
	if (prevNodeId < 0)
		return;
	CVector2D forward = pVehicle->GetForward();
	CPathNode* pPrevNode = &ThePaths.m_pathNodes[prevNodeId];
	if (forward.x == 0.0f && forward.y == 0.0f)
		forward.x = 1.0f;
	if (DotProduct2D(pNode->GetPosition() - pPrevNode->GetPosition(), forward) < 0.0f){
		int tmp;
		tmp = prevNodeId;
		prevNodeId = nodeId;
		nodeId = tmp;
	}
	pVehicle->AutoPilot.m_nPrevRouteNode = 0;
	pVehicle->AutoPilot.m_nCurrentRouteNode = prevNodeId;
	pVehicle->AutoPilot.m_nNextRouteNode = nodeId;
	pVehicle->AutoPilot.m_nPathFindNodesCount = 0;
	FindLinksToGoWithTheseNodes(pVehicle);
	pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane = 0;
}

bool CCarCtrl::JoinCarWithRoadSystemGotoCoors(CVehicle* pVehicle, CVector vecTarget, bool isProperNow)
{
#if REAL_GAMECUBE
	const int32 savedCurrentRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode;
	const int32 savedNextRouteNode = pVehicle->AutoPilot.m_nNextRouteNode;
	const uint32 savedCurrentPathInfo = pVehicle->AutoPilot.m_nCurrentPathNodeInfo;
	const uint32 savedNextPathInfo = pVehicle->AutoPilot.m_nNextPathNodeInfo;
	GcSavedIntroRouteState savedIntroRouteState;
	GcSaveIntroRouteState(pVehicle, &savedIntroRouteState);
	const bool hadActiveRoute = savedCurrentRouteNode != savedNextRouteNode ||
		savedCurrentPathInfo != savedNextPathInfo;
	const char *joinPathStage = "join-goto-path";
	bool usedSavedNextRouteStart = false;
#endif
	pVehicle->AutoPilot.m_vecDestinationCoors = vecTarget;
	ThePaths.DoPathSearch(0, pVehicle->GetPosition(), -1, vecTarget, pVehicle->AutoPilot.m_aPathFindNodesInfo,
		&pVehicle->AutoPilot.m_nPathFindNodesCount, NUM_PATH_NODES_IN_AUTOPILOT, pVehicle, nil, 999999.9f, -1);
	int16 rawPathCount = pVehicle->AutoPilot.m_nPathFindNodesCount;
	CPathNode *rawFirstNode = rawPathCount > 0 ? pVehicle->AutoPilot.m_aPathFindNodesInfo[0] : nil;
#if REAL_GAMECUBE
	GcTraceAdmiralNodeSequence(pVehicle, "join-raw",
		pVehicle->GetPosition(), pVehicle->AutoPilot.m_aPathFindNodesInfo, rawPathCount);
#endif
#if REAL_GAMECUBE
	GcRemoveBadStartNodeForScriptedIntro(pVehicle, &savedIntroRouteState,
		pVehicle->AutoPilot.m_aPathFindNodesInfo, &pVehicle->AutoPilot.m_nPathFindNodesCount,
		"join-preserve-start");
#else
	ThePaths.RemoveBadStartNode(pVehicle->GetPosition(),
		pVehicle->AutoPilot.m_aPathFindNodesInfo, &pVehicle->AutoPilot.m_nPathFindNodesCount);
#endif
#if REAL_GAMECUBE
	GcTraceAdmiralNodeSequence(pVehicle, "join-trimmed",
		pVehicle->GetPosition(), pVehicle->AutoPilot.m_aPathFindNodesInfo,
		pVehicle->AutoPilot.m_nPathFindNodesCount);
#endif
#if REAL_GAMECUBE
		if (GcIsScriptedIntroAdmiral(pVehicle) &&
		    pVehicle->AutoPilot.m_nPathFindNodesCount < 2 &&
		    !GcIsScriptedIntroTightFinalApproach(pVehicle)) {
			CVector2D segmentTarget;
			if (GcGetScriptedIntroSegmentContinuationTarget(pVehicle, &segmentTarget)) {
				GcResumeScriptedGotoPathMission(pVehicle);
				pVehicle->AutoPilot.m_nPathFindNodesCount = 0;
				GcTraceAdmiralPath(pVehicle, "join-goto-keep-segment",
					segmentTarget.x, segmentTarget.y);
				return false;
			}
	}
	if (GcIsScriptedIntroAdmiral(pVehicle) &&
	    pVehicle->AutoPilot.m_nPathFindNodesCount < 2 &&
	    hadActiveRoute) {
		CPathNode *candidateNodes[NUM_PATH_NODES_IN_AUTOPILOT];
		int16 candidateCount = 0;
		int16 candidateRawCount = 0;
		CPathNode *candidateRawFirstNode = nil;

		ThePaths.DoPathSearch(0, pVehicle->GetPosition(), savedCurrentRouteNode, vecTarget,
			candidateNodes, &candidateCount, NUM_PATH_NODES_IN_AUTOPILOT, pVehicle, nil, 999999.9f, -1);
		candidateRawCount = candidateCount;
		candidateRawFirstNode = candidateRawCount > 0 ? candidateNodes[0] : nil;
#if REAL_GAMECUBE
		GcTraceAdmiralNodeSequence(pVehicle, "join-candidate-current-raw",
			pVehicle->GetPosition(), candidateNodes, candidateRawCount);
#endif
#if REAL_GAMECUBE
		GcRemoveBadStartNodeForScriptedIntro(pVehicle, &savedIntroRouteState,
			candidateNodes, &candidateCount, "join-candidate-current-preserve");
#else
		ThePaths.RemoveBadStartNode(pVehicle->GetPosition(), candidateNodes, &candidateCount);
#endif
#if REAL_GAMECUBE
		GcTraceAdmiralNodeSequence(pVehicle, "join-candidate-current-trimmed",
			pVehicle->GetPosition(), candidateNodes, candidateCount);
#endif

		if (candidateCount < 2 &&
		    savedNextRouteNode != savedCurrentRouteNode) {
			CPathNode *nextCandidateNodes[NUM_PATH_NODES_IN_AUTOPILOT];
			int16 nextCandidateCount = 0;
			int16 nextCandidateRawCount = 0;
			CPathNode *nextCandidateRawFirstNode = nil;

			ThePaths.DoPathSearch(0, pVehicle->GetPosition(), savedNextRouteNode, vecTarget,
				nextCandidateNodes, &nextCandidateCount, NUM_PATH_NODES_IN_AUTOPILOT, pVehicle, nil, 999999.9f, -1);
			nextCandidateRawCount = nextCandidateCount;
			nextCandidateRawFirstNode = nextCandidateRawCount > 0 ? nextCandidateNodes[0] : nil;
#if REAL_GAMECUBE
			GcTraceAdmiralNodeSequence(pVehicle, "join-candidate-next-raw",
				pVehicle->GetPosition(), nextCandidateNodes, nextCandidateRawCount);
#endif
#if REAL_GAMECUBE
			GcRemoveBadStartNodeForScriptedIntro(pVehicle, &savedIntroRouteState,
				nextCandidateNodes, &nextCandidateCount, "join-candidate-next-preserve");
#else
			ThePaths.RemoveBadStartNode(pVehicle->GetPosition(), nextCandidateNodes, &nextCandidateCount);
#endif
#if REAL_GAMECUBE
			GcTraceAdmiralNodeSequence(pVehicle, "join-candidate-next-trimmed",
				pVehicle->GetPosition(), nextCandidateNodes, nextCandidateCount);
#endif

			if (nextCandidateCount > candidateCount ||
			    (nextCandidateCount == candidateCount && nextCandidateRawCount > candidateRawCount)) {
				candidateCount = nextCandidateCount;
				candidateRawCount = nextCandidateRawCount;
				candidateRawFirstNode = nextCandidateRawFirstNode;
				for (int i = 0; i < candidateCount; i++)
					candidateNodes[i] = nextCandidateNodes[i];
				joinPathStage = "join-goto-path-next";
				usedSavedNextRouteStart = true;
			}
		}

		const bool candidateIsAdjacent =
			candidateCount == 1 &&
			candidateRawCount >= 2 &&
			candidateRawFirstNode != nil &&
			candidateRawFirstNode != candidateNodes[0];

		if (candidateCount >= 2 || candidateIsAdjacent) {
			pVehicle->AutoPilot.m_nPathFindNodesCount = candidateCount;
			rawPathCount = candidateRawCount;
			rawFirstNode = candidateRawFirstNode;
			for (int i = 0; i < candidateCount; i++)
				pVehicle->AutoPilot.m_aPathFindNodesInfo[i] = candidateNodes[i];
			if (!usedSavedNextRouteStart)
				joinPathStage = "join-goto-path-current";
		}
	}
#endif
	if (pVehicle->AutoPilot.m_nPathFindNodesCount == 1 &&
	    rawPathCount >= 2 &&
	    rawFirstNode != nil &&
	    rawFirstNode != pVehicle->AutoPilot.m_aPathFindNodesInfo[0]
#if REAL_GAMECUBE
	    && (!GcIsScriptedIntroAdmiral(pVehicle) || GcGetScriptedIntroDistanceToFinal(pVehicle) <= 14.0f)
#endif
	    ) {
		// RemoveBadStartNode can drop the node behind the vehicle when we are already on
		// the final road segment. That leaves a single valid node ahead, which is still a
		// usable path for the intro Admiral and should not degrade into straight-to-target.
		pVehicle->AutoPilot.m_nPrevRouteNode = 0;
		pVehicle->AutoPilot.m_nCurrentRouteNode = rawFirstNode - ThePaths.m_pathNodes;
		pVehicle->AutoPilot.m_nNextRouteNode = pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes;
		pVehicle->AutoPilot.m_nPathFindNodesCount = 0;
		FindLinksToGoWithTheseNodes(pVehicle);
		pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane = 0;
#if REAL_GAMECUBE
		GcResumeScriptedGotoPathMission(pVehicle);
		GcTraceAdmiralPath(pVehicle, "join-goto-adjacent", vecTarget.x, vecTarget.y);
#endif
		return false;
	}
	if (pVehicle->AutoPilot.m_nPathFindNodesCount < 2){
#if REAL_GAMECUBE
		if (GcIsScriptedIntroAdmiral(pVehicle) && hadActiveRoute) {
			if (GcHasUsableIntroRouteState(&savedIntroRouteState))
				GcRestoreIntroRouteState(pVehicle, &savedIntroRouteState);
			else
				GcRebuildIntroRouteFromSavedNodes(pVehicle, &savedIntroRouteState);
			GcResumeScriptedGotoPathMission(pVehicle);
			GcTraceAdmiralPath(pVehicle, "join-goto-restore", vecTarget.x, vecTarget.y);
			return false;
		}
#endif
		pVehicle->AutoPilot.m_nPrevRouteNode = pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_nNextRouteNode = 0;
		pVehicle->AutoPilot.m_nCurrentPathNodeInfo = 0;
		pVehicle->AutoPilot.m_nPreviousPathNodeInfo = 0;
		pVehicle->AutoPilot.m_nNextPathNodeInfo = 0;
		pVehicle->AutoPilot.m_nPathFindNodesCount = 0;
#if REAL_GAMECUBE
		GcTraceAdmiralPath(pVehicle, "join-goto-straight", vecTarget.x, vecTarget.y);
#endif
		return true;
	}
#if REAL_GAMECUBE
	if (GcTryContinueScriptedIntroAlongActiveSegment(pVehicle, &savedIntroRouteState,
	    pVehicle->AutoPilot.m_aPathFindNodesInfo, &pVehicle->AutoPilot.m_nPathFindNodesCount)) {
		GcResumeScriptedGotoPathMission(pVehicle);
		GcTraceAdmiralNodeSequence(pVehicle, "join-keep-active-segment",
			pVehicle->GetPosition(), pVehicle->AutoPilot.m_aPathFindNodesInfo,
			pVehicle->AutoPilot.m_nPathFindNodesCount);
		GcTraceAdmiralPath(pVehicle, "join-goto-keep-active-segment",
			vecTarget.x, vecTarget.y);
		return false;
	}
#endif
	pVehicle->AutoPilot.m_nPrevRouteNode = 0;
	pVehicle->AutoPilot.m_nCurrentRouteNode = pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes;
	pVehicle->AutoPilot.RemoveOnePathNode();
	pVehicle->AutoPilot.m_nNextRouteNode = pVehicle->AutoPilot.m_aPathFindNodesInfo[0] - ThePaths.m_pathNodes;
	pVehicle->AutoPilot.RemoveOnePathNode();
#if REAL_GAMECUBE
	GcTraceAdmiralNodeSequence(pVehicle, "join-selected",
		pVehicle->GetPosition(), pVehicle->AutoPilot.m_aPathFindNodesInfo,
		pVehicle->AutoPilot.m_nPathFindNodesCount);
#endif
	FindLinksToGoWithTheseNodes(pVehicle);
	pVehicle->AutoPilot.m_nNextLane = pVehicle->AutoPilot.m_nCurrentLane = 0;
#if REAL_GAMECUBE
	GcResumeScriptedGotoPathMission(pVehicle);
	GcTraceScriptedIntroVelocitySample(pVehicle, "join-goto-exit");
	GcTraceAdmiralPath(pVehicle, joinPathStage, vecTarget.x, vecTarget.y);
#endif
	return false;
}

void CCarCtrl::FindLinksToGoWithTheseNodes(CVehicle* pVehicle)
{
	if (pVehicle->m_nRouteSeed)
		CGeneral::SetRandomSeed(pVehicle->m_nRouteSeed);
	int nextLink;
	CPathNode* pCurNode = &ThePaths.m_pathNodes[pVehicle->AutoPilot.m_nCurrentRouteNode];
	for (nextLink = 0; nextLink < 12; nextLink++)
		if (ThePaths.ConnectedNode(nextLink + pCurNode->firstLink) == pVehicle->AutoPilot.m_nNextRouteNode)
			break;
	pVehicle->AutoPilot.m_nNextPathNodeInfo = ThePaths.m_carPathConnections[nextLink + pCurNode->firstLink];
	pVehicle->AutoPilot.m_nNextDirection = (pVehicle->AutoPilot.m_nCurrentRouteNode >= pVehicle->AutoPilot.m_nNextRouteNode) ? 1 : -1;
	int curLink;
	int chosenCurLink = 0;
	int curConnection;
	float chosenAltLinkDist = 0.0f;
	if (pCurNode->numLinks == 1) {
		chosenCurLink = 0;
		curConnection = ThePaths.m_carPathConnections[pCurNode->firstLink];
	}else{
		int closestLink = -1;
		float md = 999999.9f;

		for (curLink = 0; curLink < pCurNode->numLinks; curLink++) {
			int node = ThePaths.ConnectedNode(curLink + pCurNode->firstLink);
			CPathNode* pNode = &ThePaths.m_pathNodes[node];
			if (node == pVehicle->AutoPilot.m_nNextRouteNode)
				continue;
			CVector vCurPos = pCurNode->GetPosition();
			CVector vNextPos = pNode->GetPosition();
			float dist = CCollision::DistToLine(&vCurPos, &vNextPos, &pVehicle->GetPosition());
			if (dist < md) {
				md = dist;
				closestLink = curLink;
			}
		}
		if (closestLink < 0)
			closestLink = 0;
		chosenCurLink = closestLink;
		chosenAltLinkDist = md;
		curConnection = ThePaths.m_carPathConnections[closestLink + pCurNode->firstLink];
	}
	pVehicle->AutoPilot.m_nCurrentPathNodeInfo = curConnection;
	pVehicle->AutoPilot.m_nCurrentDirection =
		(ThePaths.ConnectedNode(chosenCurLink + pCurNode->firstLink) >= pVehicle->AutoPilot.m_nCurrentRouteNode) ? 1 : -1;
#if REAL_GAMECUBE
	GcTraceAdmiralLinkChoice(pVehicle, "find-links",
		pVehicle->AutoPilot.m_nCurrentRouteNode,
		pVehicle->AutoPilot.m_nNextRouteNode,
		chosenCurLink, nextLink,
		curConnection,
		pVehicle->AutoPilot.m_nNextPathNodeInfo,
		chosenAltLinkDist);
#endif
}

void CCarCtrl::GenerateEmergencyServicesCar(void)
{
	if (FindPlayerPed()->m_pWanted->GetWantedLevel() > 3)
		return;
	if (CGame::IsInInterior())
		return;
	if (NumFiretrucksOnDuty + NumAmbulancesOnDuty + NumParkedCars + NumMissionCars +
		NumLawEnforcerCars + NumRandomCars > MaxNumberOfCarsInUse)
		return;
	if (NumAmbulancesOnDuty == 0){
		if (gAccidentManager.CountActiveAccidents() < 2){
			if (CStreaming::HasModelLoaded(MI_AMBULAN))
				CStreaming::SetModelIsDeletable(MI_MEDIC);
		}else{
			float distance = 30.0f;
			CAccident* pNearestAccident = gAccidentManager.FindNearestAccident(FindPlayerCoors(), &distance);
			if (pNearestAccident){
				if (CountCarsOfType(MI_AMBULAN) < 2 && CTimer::GetTimeInMilliseconds() > LastTimeAmbulanceCreated + 30000){
					CStreaming::RequestModel(MI_AMBULAN, STREAMFLAGS_DEPENDENCY);
					CStreaming::RequestModel(MI_MEDIC, STREAMFLAGS_DONT_REMOVE);
					if (CStreaming::HasModelLoaded(MI_AMBULAN) && CStreaming::HasModelLoaded(MI_MEDIC)){
						if (GenerateOneEmergencyServicesCar(MI_AMBULAN, pNearestAccident->m_pVictim->GetPosition())){
							LastTimeAmbulanceCreated = CTimer::GetTimeInMilliseconds();
						}
					}
				}
			}
		}
	}
	if (NumFiretrucksOnDuty == 0){
		if (gFireManager.GetTotalActiveFires() < 3){
			if (CStreaming::HasModelLoaded(MI_FIRETRUCK))
				CStreaming::SetModelIsDeletable(MI_FIREMAN);
		}else{
			float distance = 30.0f;
			CFire* pNearestFire = gFireManager.FindNearestFire(FindPlayerCoors(), &distance);
			if (pNearestFire) {
				if (CountCarsOfType(MI_FIRETRUCK) < 2 && CTimer::GetTimeInMilliseconds() > LastTimeFireTruckCreated + 35000){
					CStreaming::RequestModel(MI_FIRETRUCK, STREAMFLAGS_DEPENDENCY);
					CStreaming::RequestModel(MI_FIREMAN, STREAMFLAGS_DONT_REMOVE);
					if (CStreaming::HasModelLoaded(MI_FIRETRUCK) && CStreaming::HasModelLoaded(MI_FIREMAN)){
						if (GenerateOneEmergencyServicesCar(MI_FIRETRUCK, pNearestFire->m_vecPos)){
							LastTimeFireTruckCreated = CTimer::GetTimeInMilliseconds();
#ifdef SECUROM
							if ((myrand() & 7) == 5){
								// if pirated game
								CPickups::Init();
							}
#endif
						}
					}
				}
			}
		}
	}
}

bool CCarCtrl::GenerateOneEmergencyServicesCar(uint32 mi, CVector vecPos)
{
	CVector pPlayerPos = FindPlayerCentreOfWorld(CWorld::PlayerInFocus);
	bool created = false;
	int attempts = 0;
	CVector spawnPos;
	int curNode, nextNode;
	float posBetweenNodes;
	while (!created && attempts < 5){
		if (ThePaths.GenerateCarCreationCoors(pPlayerPos.x, pPlayerPos.y, 0.707f, 0.707f,
			REQUEST_ONSCREEN_DISTANCE, -1.0f, true, &spawnPos, &curNode, &nextNode, &posBetweenNodes, false)){
			int16 colliding[2];
			if (!ThePaths.GetNode(curNode)->bWaterPath) {
				CWorld::FindObjectsKindaColliding(spawnPos, 10.0f, true, colliding, 2, nil, false, true, true, false, false);
				if (colliding[0] == 0)
					created = true;
			}
		}
		attempts += 1;
	}
	if (attempts >= 5)
		return false;
	CAutomobile* pVehicle = new CAutomobile(mi, RANDOM_VEHICLE);
	pVehicle->AutoPilot.m_vecDestinationCoors = vecPos;
	pVehicle->SetPosition(spawnPos);
	pVehicle->AutoPilot.m_nCarMission = (JoinCarWithRoadSystemGotoCoors(pVehicle, vecPos, false)) ? MISSION_GOTOCOORDS_STRAIGHT : MISSION_GOTOCOORDS;
	pVehicle->AutoPilot.m_fMaxTrafficSpeed = pVehicle->AutoPilot.m_nCruiseSpeed = 25;
	pVehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
	pVehicle->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
	CVector2D direction = vecPos - spawnPos;
	direction.Normalise();
	pVehicle->GetForward() = CVector(direction.x, direction.y, 0.0f);
	pVehicle->GetRight() = CVector(direction.y, -direction.x, 0.0f);
	pVehicle->GetUp() = CVector(0.0f, 0.0f, 1.0f);
	spawnPos.z = posBetweenNodes * ThePaths.m_pathNodes[curNode].GetZ() + (1.0f - posBetweenNodes) * ThePaths.m_pathNodes[nextNode].GetZ();
	float groundZ = INFINITE_Z;
	CColPoint colPoint;
	CEntity* pEntity;
	if (CWorld::ProcessVerticalLine(spawnPos, 1000.0f, colPoint, pEntity, true, false, false, false, true, false, nil))
		groundZ = colPoint.point.z;
	if (CWorld::ProcessVerticalLine(spawnPos, -1000.0f, colPoint, pEntity, true, false, false, false, true, false, nil)) {
		if (ABS(colPoint.point.z - spawnPos.z) < ABS(groundZ - spawnPos.z))
			groundZ = colPoint.point.z;
	}
	if (groundZ == INFINITE_Z) {
		delete pVehicle;
		return false;
	}
	spawnPos.z = groundZ + pVehicle->GetDistanceFromCentreOfMassToBaseOfModel();
	pVehicle->SetPosition(spawnPos);
	pVehicle->SetMoveSpeed(CVector(0.0f, 0.0f, 0.0f));
	pVehicle->SetStatus(STATUS_PHYSICS);
	switch (mi){
	case MI_FIRETRUCK:
		pVehicle->bIsFireTruckOnDuty = true;
		++NumFiretrucksOnDuty;
		CCarAI::AddFiretruckOccupants(pVehicle);
		break;
	case MI_AMBULAN:
		pVehicle->bIsAmbulanceOnDuty = true;
		++NumAmbulancesOnDuty;
		CCarAI::AddAmbulanceOccupants(pVehicle);
		break;
	}
	pVehicle->m_bSirenOrAlarm = true;
	CWorld::Add(pVehicle);
	printf("CREATED EMERGENCY VEHICLE\n");
	return true;
}

void CCarCtrl::UpdateCarCount(CVehicle* pVehicle, bool remove)
{
	if (remove){
		switch (pVehicle->VehicleCreatedBy){
		case RANDOM_VEHICLE:
			if (pVehicle->bIsLawEnforcer) {
				if (--NumLawEnforcerCars < 0)
					NumLawEnforcerCars = 0;
			}
			if (--NumRandomCars < 0)
				NumRandomCars = 0;
			return;
		case MISSION_VEHICLE:
			if (--NumMissionCars < 0)
				NumMissionCars = 0;
			return;
		case PARKED_VEHICLE:
			if (--NumParkedCars < 0)
				NumParkedCars = 0;
			return;
		case PERMANENT_VEHICLE:
			if (--NumPermanentCars < 0)
				NumPermanentCars = 0;
			return;
		}
	}
	else{
		switch (pVehicle->VehicleCreatedBy){
		case RANDOM_VEHICLE:
			if (pVehicle->bIsLawEnforcer)
				++NumLawEnforcerCars;
			++NumRandomCars;
			return;
		case MISSION_VEHICLE:
			++NumMissionCars;
			return;
		case PARKED_VEHICLE:
			++NumParkedCars;
			return;
		case PERMANENT_VEHICLE:
			++NumPermanentCars;
			return;
		}
	}
}

bool CCarCtrl::ThisRoadObjectCouldMove(int16 mi)
{
#ifdef GTA_BRIDGE
	return mi == MI_BRIDGELIFT || mi == MI_BRIDGEROADSEGMENT;
#else
	return false;
#endif
}

bool CCarCtrl::MapCouldMoveInThisArea(float x, float y)
{
#ifdef GTA_BRIDGE	// actually they forgot that in VC...
	// bridge moves up and down
	return x > -342.0f && x < -219.0f &&
		y > -677.0f && y < -580.0f;
#else
	return false;
#endif
}

float CCarCtrl::FindSpeedMultiplierWithSpeedFromNodes(int8 type)
{
	switch (type)
	{
	case 1: return 1.5f;
	case 2: return 2.0f;
	}
	return 1.0f;
}
