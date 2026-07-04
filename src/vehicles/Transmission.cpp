#include "common.h"

#include "Timer.h"
#include "HandlingMgr.h"
#include "Transmission.h"

#if REAL_GAMECUBE
static bool gLoggedBadTransmissionGear;
static bool gLoggedBadTransmissionConfig;
static bool gLoggedBadTransmissionRatios;

static bool GcTransmissionFinite(float f);

static bool
GcTransmissionRatiosLookValid(const cTransmission *tr)
{
    if(tr == nil)
        return false;
    if(tr->nNumberOfGears < 1 || tr->nNumberOfGears > 5)
        return false;
    if(!GcTransmissionFinite(tr->Gears[0].fMaxVelocity) ||
       !GcTransmissionFinite(tr->Gears[0].fShiftUpVelocity) ||
       !GcTransmissionFinite(tr->Gears[0].fShiftDownVelocity))
        return false;
    if(tr->Gears[0].fMaxVelocity >= -0.0001f || tr->Gears[0].fShiftUpVelocity >= 0.0f)
        return false;

    float prevMax = 0.0f;
    for(int i = 1; i <= tr->nNumberOfGears; i++){
        const tGear &g = tr->Gears[i];
        if(!GcTransmissionFinite(g.fMaxVelocity) ||
           !GcTransmissionFinite(g.fShiftUpVelocity) ||
           !GcTransmissionFinite(g.fShiftDownVelocity))
            return false;
        if(g.fMaxVelocity <= prevMax + 0.00001f)
            return false;
        if(g.fShiftDownVelocity >= g.fShiftUpVelocity)
            return false;
        if(g.fShiftUpVelocity > g.fMaxVelocity + 0.0001f)
            return false;
        prevMax = g.fMaxVelocity;
    }

    return Abs(tr->Gears[tr->nNumberOfGears].fMaxVelocity - tr->fMaxVelocity) <=
        Max(0.001f, tr->fMaxVelocity * 0.25f);
}

static void
GcEnsureTransmissionRatios(cTransmission *tr, uint8 gear, float velocity, float gasPedal)
{
    if(GcTransmissionRatiosLookValid(tr))
        return;

    if(!gLoggedBadTransmissionRatios){
        int32 topGear = tr && tr->nNumberOfGears >= 0 && tr->nNumberOfGears < 6 ? tr->nNumberOfGears : 0;
        printf("[VEH-TRANS-REINIT] gear=%u gears=%d vel=%f gas=%f maxVel=%f revVel=%f accel=%f g0=(%f,%f,%f) g1=(%f,%f,%f) gN=(%f,%f,%f)\n",
               (uint32)gear, tr ? (int)tr->nNumberOfGears : -1, velocity, gasPedal,
               tr ? tr->fMaxVelocity : 0.0f, tr ? tr->fMaxReverseVelocity : 0.0f,
               tr ? tr->fEngineAcceleration : 0.0f,
               tr ? tr->Gears[0].fMaxVelocity : 0.0f,
               tr ? tr->Gears[0].fShiftDownVelocity : 0.0f,
               tr ? tr->Gears[0].fShiftUpVelocity : 0.0f,
               tr ? tr->Gears[1].fMaxVelocity : 0.0f,
               tr ? tr->Gears[1].fShiftDownVelocity : 0.0f,
               tr ? tr->Gears[1].fShiftUpVelocity : 0.0f,
               tr ? tr->Gears[topGear].fMaxVelocity : 0.0f,
               tr ? tr->Gears[topGear].fShiftDownVelocity : 0.0f,
               tr ? tr->Gears[topGear].fShiftUpVelocity : 0.0f);
        gLoggedBadTransmissionRatios = true;
    }

    tr->InitGearRatios();
}

static bool
GcTransmissionFinite(float f)
{
	return f == f && f > -1.0e20f && f < 1.0e20f;
}
#endif

void
cTransmission::InitGearRatios(void)
{
	static tGear *pGearRatio0 = nil;
	static tGear *pGearRatio1 = nil;
	int i;
	float velocityDiff;

	memset(Gears, 0, sizeof(Gears));

#if REAL_GAMECUBE
	if(nNumberOfGears < 1){
		if(!gLoggedBadTransmissionConfig){
			printf("[VEH-GUARD] bad transmission init: gears=%d maxVel=%f revVel=%f accel=%f\n",
			       (int)nNumberOfGears, fMaxVelocity, fMaxReverseVelocity, fEngineAcceleration);
			gLoggedBadTransmissionConfig = true;
		}
		nNumberOfGears = 1;
	}
	if(!GcTransmissionFinite(fMaxVelocity) || fMaxVelocity <= 0.0f){
		if(!gLoggedBadTransmissionConfig){
			printf("[VEH-GUARD] bad transmission init: gears=%d maxVel=%f revVel=%f accel=%f\n",
			       (int)nNumberOfGears, fMaxVelocity, fMaxReverseVelocity, fEngineAcceleration);
			gLoggedBadTransmissionConfig = true;
		}
		fMaxVelocity = 0.01f;
	}
	if(!GcTransmissionFinite(fMaxReverseVelocity) || fMaxReverseVelocity >= -0.0001f)
		fMaxReverseVelocity = -0.01f;
	if(!GcTransmissionFinite(fEngineAcceleration) || fEngineAcceleration < 0.0f)
		fEngineAcceleration = 0.0f;
#endif

	for(i = 1; i <= nNumberOfGears; i++){
		pGearRatio0 = &Gears[i-1];
		pGearRatio1 = &Gears[i];

		pGearRatio1->fMaxVelocity = (float)i / nNumberOfGears * fMaxVelocity;

		velocityDiff = pGearRatio1->fMaxVelocity - pGearRatio0->fMaxVelocity;

		if(i >= nNumberOfGears){
			pGearRatio1->fShiftUpVelocity = fMaxVelocity;
		}else{
			Gears[i+1].fShiftDownVelocity = velocityDiff*0.42f + pGearRatio0->fMaxVelocity;
			pGearRatio1->fShiftUpVelocity = velocityDiff*0.6667f + pGearRatio0->fMaxVelocity;
		}
	}

	// Reverse gear
	Gears[0].fMaxVelocity = fMaxReverseVelocity;
	Gears[0].fShiftUpVelocity = -0.01f;
	Gears[0].fShiftDownVelocity = fMaxReverseVelocity;

	Gears[1].fShiftDownVelocity = -0.01f;
}

void
cTransmission::CalculateGearForSimpleCar(float speed, uint8 &gear)
{
	static tGear *pGearRatio;

#if REAL_GAMECUBE
	if(nNumberOfGears < 1){
		gear = 1;
		fCurVelocity = 0.0f;
		return;
	}
	if(gear > nNumberOfGears)
		gear = nNumberOfGears;
	if(!GcTransmissionFinite(speed)){
		gear = 1;
		fCurVelocity = 0.0f;
		return;
	}
	GcEnsureTransmissionRatios(this, gear, speed, 0.0f);
#endif

	pGearRatio = &Gears[gear];
	fCurVelocity = speed;
	if(speed > pGearRatio->fShiftUpVelocity)
		gear++;
	else if(speed < pGearRatio->fShiftDownVelocity){
		if(gear - 1 < 0)
			gear = 0;
		else
			gear--;
	}
}

float
cTransmission::CalculateDriveAcceleration(const float &gasPedal, uint8 &gear, float &time, const float &velocity, bool cheat)
{
	static float fAcceleration = 0.0f;
	static float fVelocity;
	static float fCheat;
	static tGear *pGearRatio;

	fVelocity = velocity;
#if REAL_GAMECUBE
	if(!GcTransmissionFinite(fVelocity)){
		if(!gLoggedBadTransmissionConfig){
			printf("[VEH-GUARD] bad transmission input: gear=%u gears=%u vel=%f gas=%f maxVel=%f revVel=%f\n",
			       (uint32)gear, (uint32)nNumberOfGears, fVelocity, gasPedal,
			       fMaxVelocity, fMaxReverseVelocity);
			gLoggedBadTransmissionConfig = true;
		}
		gear = nNumberOfGears > 0 ? 1 : 0;
		fCurVelocity = 0.0f;
		return 0.0f;
	}
	if(nNumberOfGears < 1 || !GcTransmissionFinite(fMaxVelocity) || fMaxVelocity <= 0.0f ||
	   !GcTransmissionFinite(fMaxReverseVelocity) || !GcTransmissionFinite(fEngineAcceleration) ||
	   fEngineAcceleration < 0.0f){
		if(!gLoggedBadTransmissionConfig){
			printf("[VEH-GUARD] bad transmission config: gear=%u gears=%u vel=%f gas=%f maxVel=%f revVel=%f accel=%f\n",
			       (uint32)gear, (uint32)nNumberOfGears, fVelocity, gasPedal,
			       fMaxVelocity, fMaxReverseVelocity, fEngineAcceleration);
			gLoggedBadTransmissionConfig = true;
		}
		gear = nNumberOfGears > 0 ? 1 : 0;
		fCurVelocity = 0.0f;
		return 0.0f;
	}
	GcEnsureTransmissionRatios(this, gear, fVelocity, gasPedal);
#endif
	if(fVelocity < fMaxReverseVelocity){
		fVelocity = fMaxReverseVelocity;
		return 0.0f;
	}
	if(fVelocity > fMaxVelocity){
		fVelocity = fMaxVelocity;
		return 0.0f;
	}
	fCurVelocity = fVelocity;

#if REAL_GAMECUBE
	if(gear > nNumberOfGears){
		if(!gLoggedBadTransmissionGear){
			printf("[VEH-GUARD] bad gear: gear=%u gears=%u vel=%f gas=%f maxVel=%f revVel=%f\n",
			       (uint32)gear, (uint32)nNumberOfGears, fVelocity, gasPedal,
			       fMaxVelocity, fMaxReverseVelocity);
			gLoggedBadTransmissionGear = true;
		}
		gear = nNumberOfGears;
	}
#endif
	assert(gear <= nNumberOfGears);

	pGearRatio = &Gears[gear];
	if(fVelocity > pGearRatio->fShiftUpVelocity){
		if(gear != 0 || gasPedal > 0.0f){
			gear++;
			return CalculateDriveAcceleration(gasPedal, gear, time, fVelocity, false);
		}
	}else if(fVelocity < pGearRatio->fShiftDownVelocity && gear != 0){
		if(gear != 1 || gasPedal < 0.0f){
			gear--;
			return CalculateDriveAcceleration(gasPedal, gear, time, fVelocity, false);
		}
	}

	float speedMul, accelMul;

	if(gear < 1){
		// going reverse
		accelMul = (Flags & HANDLING_2G_BOOST) ? 2.0f : 1.0f;
		speedMul = -1.0f;
	}else if(nNumberOfGears == 1){
		accelMul = 1.0f;
		speedMul = 1.0f;
	}else{
		// BUG or not? this is 1.0 normally but 0.0 in the highest gear
		float f = 1.0f - (gear-1)/(nNumberOfGears-1);
		speedMul = 3.0f*sq(f) + 1.0f;
		// This is pretty ugly, could be written more clearly
		if(Flags & HANDLING_2G_BOOST){
			if(gear == 1)
				accelMul = (Flags & HANDLING_1G_BOOST) ? 2.0f : 1.6f;
			else if(gear == 2)
				accelMul = 1.3f;
			else
				accelMul = 1.0f;
		}else if(Flags & HANDLING_1G_BOOST && gear == 1){
			accelMul = 2.0f;
		}else
			accelMul = 1.0f;
	}

	if(cheat)
		fCheat = 1.2f;
	else
		fCheat = 1.0f;
	float targetVelocity = Gears[gear].fMaxVelocity*speedMul*fCheat;
#if REAL_GAMECUBE
	if(!GcTransmissionFinite(targetVelocity) || Abs(targetVelocity) < 0.0001f){
		fCurVelocity = fVelocity;
		return 0.0f;
	}
#endif
	float accel = (targetVelocity - fVelocity) * (fEngineAcceleration*accelMul) / Abs(targetVelocity);
	if(Abs(fVelocity) < Abs(Gears[gear].fMaxVelocity*fCheat))
		fAcceleration = gasPedal * accel * CTimer::GetTimeStep();
	else
		fAcceleration = 0.0f;
	return fAcceleration;
}
