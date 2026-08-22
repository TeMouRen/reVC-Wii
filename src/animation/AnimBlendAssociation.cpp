#include "common.h"

#include "AnimBlendHierarchy.h"
#include "AnimBlendClumpData.h"
#include "RpAnimBlend.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "MemoryMgr.h"

CAnimBlendAssociation::CAnimBlendAssociation(void)
{
	numNodes = 0;
	groupId = -1;
	nodes = nil;
	hierarchy = nil;
	blendAmount = 1.0f;
	blendDelta = 0.0f;
	currentTime = 0.0f;
	speed = 1.0f;
	timeStep = 0.0f;
	animId = -1;
	flags = 0;
	callbackType = CB_NONE;
	callback = nil;
	callbackArg = nil;
	link.Init();
}

CAnimBlendAssociation::CAnimBlendAssociation(CAnimBlendAssociation &other)
{
	numNodes = 0;
	groupId = -1;
	nodes = nil;
	hierarchy = nil;
	blendAmount = 1.0f;
	blendDelta = 0.0f;
	currentTime = 0.0f;
	speed = 1.0f;
	timeStep = 0.0f;
	animId = -1;
	flags = 0;
	callbackType = CB_NONE;
	callback = nil;
	callbackArg = nil;
	link.Init();
	Init(other);
}

CAnimBlendAssociation::~CAnimBlendAssociation(void)
{
	FreeAnimBlendNodeArray();
	link.Remove();
}


void
CAnimBlendAssociation::AllocateAnimBlendNodeArray(int n)
{
	int i;

	nodes = nil;
	numNodes = 0;
	if(n <= 0)
		return;

	nodes = (CAnimBlendNode*)RwMallocAlign(n*sizeof(CAnimBlendNode), 64);
	if(nodes == nil){
		printf("[ANIM-ALLOC] AllocateAnimBlendNodeArray FAILED n=%d size=%u\n",
		       n, (unsigned)(n*sizeof(CAnimBlendNode)));
		return;
	}
	numNodes = n;
	for(i = 0; i < n; i++)
		nodes[i].Init();
}

void
CAnimBlendAssociation::FreeAnimBlendNodeArray(void)
{
	if(nodes)
		RwFreeAlign(nodes);
	nodes = nil;
	numNodes = 0;
}

void
CAnimBlendAssociation::Init(RpClump *clump, CAnimBlendHierarchy *hier)
{
	int i;
	AnimBlendFrameData *frame;
	CAnimBlendClumpData *clumpData;

	FreeAnimBlendNodeArray();
	hierarchy = nil;
	if(clump == nil || hier == nil){
		printf("[ANIM-ALLOC] Init(clump,hier) rejected clump=%p hier=%p\n",
		       (void*)clump, (void*)hier);
		return;
	}
	clumpData = *RPANIMBLENDCLUMPDATA(clump);
	if(clumpData == nil || clumpData->frames == nil || clumpData->numFrames <= 0){
		printf("[ANIM-ALLOC] Init(clump,hier) missing clump data clump=%p hier=%p data=%p\n",
		       (void*)clump, (void*)hier, (void*)clumpData);
		return;
	}
	AllocateAnimBlendNodeArray(clumpData->numFrames);
	if(nodes == nil){
		printf("[ANIM-ALLOC] Init(clump,hier) aborted: nodes alloc failed clump=%p hier=%p frames=%d\n",
		       (void*)clump, (void*)hier, (int)clumpData->numFrames);
		return;
	}
	for(i = 0; i < numNodes; i++)
		nodes[i].association = this;
	hierarchy = hier;

	// Init every node from a sequence and a Clump frame
	// NB: This is where the order of nodes is defined
	for(i = 0; i < hier->numSequences; i++){
		CAnimBlendSequence *seq = &hier->sequences[i];
		if(seq->boneTag == -1)
			frame = RpAnimBlendClumpFindFrame(clump, seq->name);
		else
			frame = RpAnimBlendClumpFindBone(clump, seq->boneTag);
		if(frame && seq->numFrames > 0)
			nodes[frame - clumpData->frames].sequence = seq;
	}
}

void
CAnimBlendAssociation::Init(CAnimBlendAssociation &assoc)
{
	int i;
	int nodeCount;

	FreeAnimBlendNodeArray();
	hierarchy = nil;
	nodeCount = assoc.numNodes;
	if(assoc.nodes == nil || assoc.hierarchy == nil || nodeCount <= 0){
		printf("[ANIM-ALLOC] Init(copy) rejected assoc=%p nodes=%p hier=%p frames=%d\n",
		       (void*)&assoc, (void*)assoc.nodes, (void*)assoc.hierarchy, nodeCount);
		return;
	}
	AllocateAnimBlendNodeArray(nodeCount);
	if(nodes == nil){
		printf("[ANIM-ALLOC] Init(copy) aborted: nodes alloc failed assoc=%p frames=%d\n",
		       (void*)&assoc, nodeCount);
		return;
	}
	hierarchy = assoc.hierarchy;
	flags = assoc.flags;
	animId = assoc.animId;
	groupId = assoc.groupId;
	for(i = 0; i < numNodes; i++){
		nodes[i] = assoc.nodes[i];
		nodes[i].association = this;
	}
}

void
CAnimBlendAssociation::SetBlend(float amount, float delta)
{
	blendAmount = amount;
	blendDelta = delta;
}

void
CAnimBlendAssociation::SetFinishCallback(void (*cb)(CAnimBlendAssociation*, void*), void *arg)
{
	callbackType = CB_FINISH;
	callback = cb;
	callbackArg = arg;
}

void
CAnimBlendAssociation::SetDeleteCallback(void (*cb)(CAnimBlendAssociation*, void*), void *arg)
{
	callbackType = CB_DELETE;
	callback = cb;
	callbackArg = arg;
}

void
CAnimBlendAssociation::SetCurrentTime(float time)
{
	int i;

	for(currentTime = time; currentTime >= hierarchy->totalLength; currentTime -= hierarchy->totalLength)
		if (!IsRepeating()) {
			currentTime = hierarchy->totalLength;
			break;
		}

	CAnimManager::UncompressAnimation(hierarchy);
#ifdef ANIM_COMPRESSION
	// strangely PC has this but android doesn't
	if(hierarchy->keepCompressed){
		for(i = 0; i < numNodes; i++)
			if(nodes[i].sequence)
				nodes[i].SetupKeyFrameCompressed();
	}else
#endif
	{
		for(i = 0; i < numNodes; i++)
			if(nodes[i].sequence)
				nodes[i].FindKeyFrame(currentTime);
	}
}

void
CAnimBlendAssociation::SyncAnimation(CAnimBlendAssociation *other)
{
	SetCurrentTime(other->currentTime/other->hierarchy->totalLength * hierarchy->totalLength);
}

void
CAnimBlendAssociation::Start(float time)
{
	flags |= ASSOC_RUNNING;
	SetCurrentTime(time);
}

void
CAnimBlendAssociation::UpdateTimeStep(float timeDelta, float relSpeed)
{
	if(IsRunning())
		timeStep = (flags & ASSOC_MOVEMENT ? relSpeed*hierarchy->totalLength : speed) * timeDelta;
}

bool
CAnimBlendAssociation::UpdateTime(float timeDelta, float relSpeed)
{
	if(!IsRunning())
		return true;
	if(currentTime >= hierarchy->totalLength){
		flags &= ~ASSOC_RUNNING;
		return true;
	}

	currentTime += timeStep;

	if(currentTime >= hierarchy->totalLength){
		// Ran past end

		if(IsRepeating())
			currentTime -= hierarchy->totalLength;
		else{
			currentTime = hierarchy->totalLength;
			if(flags & ASSOC_FADEOUTWHENDONE){
				flags |= ASSOC_DELETEFADEDOUT;
				blendDelta = -4.0f;
			}
			if(callbackType == CB_FINISH){
				callbackType = CB_NONE;
				callback(this, callbackArg);
			}
		}
	}
	return true;
}

// return whether we still exist after this function
bool
CAnimBlendAssociation::UpdateBlend(float timeDelta)
{
	blendAmount += blendDelta * timeDelta;

	if(blendAmount <= 0.0f && blendDelta < 0.0f){
		// We're faded out and are not fading in
		blendAmount = 0.0f;
		blendDelta = Max(0.0f, blendDelta);
		if(flags & ASSOC_DELETEFADEDOUT){
			if(callbackType == CB_FINISH || callbackType == CB_DELETE)
				callback(this, callbackArg);
			delete this;
			return false;
		}
	}

	if(blendAmount > 1.0f){
		// Maximally faded in, clamp values
		blendAmount = 1.0f;
		blendDelta = Min(0.0f, blendDelta);
	}

	return true;
}
