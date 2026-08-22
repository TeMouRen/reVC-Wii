#include "common.h"

#if defined _WIN32 && !defined __MINGW32__
#if defined __MWERKS__
#include <wctype.h>
#else
#include "ctype.h"
#endif
#else
#include <cwctype>
#endif

#include "General.h"
#include "RwHelper.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "AnimManager.h"
#include "RpAnimBlend.h"
#include "AnimBlendAssociation.h"
#include "AnimBlendAssocGroup.h"

static bool
IsUsableAnimAssociation(const CAnimBlendAssociation *assoc)
{
	return assoc != nil &&
		assoc->nodes != nil &&
		assoc->hierarchy != nil &&
		assoc->hierarchy->name[0] != '\0';
}

CAnimBlendAssocGroup::CAnimBlendAssocGroup(void)
{
	animBlock = nil;
	assocList = nil;
	numAssociations = 0;
	firstAnimId = 0;
	groupId = -1;
}

CAnimBlendAssocGroup::~CAnimBlendAssocGroup(void)
{
	DestroyAssociations();
}

void
CAnimBlendAssocGroup::DestroyAssociations(void)
{
	if(assocList){
		delete[] assocList;
		assocList = nil;
	}
	numAssociations = 0;
	animBlock = nil;
}

CAnimBlendAssociation*
CAnimBlendAssocGroup::GetAnimation(uint32 id)
{
	int32 directIndex;

	if(assocList == nil || numAssociations <= 0){
		printf("[ANIM-GROUP] GetAnimation(id=%u) on empty group=%d block=%p\n",
		       (unsigned)id, (int)groupId, (void*)animBlock);
		return nil;
	}
	directIndex = (int32)id - firstAnimId;
	if(directIndex < 0 || directIndex >= numAssociations ||
	   !IsUsableAnimAssociation(&assocList[directIndex]) ||
	   (uint32)assocList[directIndex].animId != id){
		printf("[ANIM-GROUP] Missing animation id=%u group=%d count=%d\n",
		       (unsigned)id, (int)groupId, (int)numAssociations);
		return nil;
	}
	return &assocList[directIndex];
}

CAnimBlendAssociation*
CAnimBlendAssocGroup::GetAnimation(const char *name)
{
	int i;
	int invalidSlots;

	if(name == nil || name[0] == '\0'){
		printf("[ANIM-GROUP] GetAnimation(name) received empty name for group=%d\n",
		       (int)groupId);
		return nil;
	}

	if(assocList == nil || numAssociations <= 0){
		printf("[ANIM-GROUP] GetAnimation(%s) on empty group=%d block=%p\n",
		       name, (int)groupId, (void*)animBlock);
		return nil;
	}

	invalidSlots = 0;
	for(i = 0; i < numAssociations; i++){
		if(!IsUsableAnimAssociation(&assocList[i])){
			invalidSlots++;
			continue;
		}
		if(!CGeneral::faststricmp(assocList[i].hierarchy->name, name))
			return &assocList[i];
	}
	if(invalidSlots > 0){
		printf("[ANIM-GROUP] GetAnimation(%s) skipped %d invalid associations in group=%d\n",
		       name, invalidSlots, (int)groupId);
	}
	debug("\n\nCan't find the fucking animation %s\n\n\n", name);
	return nil;
}


CAnimBlendAssociation*
CAnimBlendAssocGroup::CopyAnimation(uint32 id)
{
	CAnimBlendAssociation *anim = GetAnimation(id);
	if(anim == nil)
		return nil;
	CAnimManager::UncompressAnimation(anim->hierarchy);
	return new CAnimBlendAssociation(*anim);
}

CAnimBlendAssociation*
CAnimBlendAssocGroup::CopyAnimation(const char *name)
{
	CAnimBlendAssociation *anim = GetAnimation(name);
	if(anim == nil)
		return nil;
	CAnimManager::UncompressAnimation(anim->hierarchy);
	return new CAnimBlendAssociation(*anim);
}

bool
strcmpIgnoringDigits(const char *s1, const char *s2)
{
	char c1, c2;

	for(;;){
		c1 = *s1;
		c2 = *s2;
		if(c1) s1++;
		if(c2) s2++;
		if(c1 == '\0' && c2 == '\0') return true;
#ifndef ASCII_STRCMP
		if(isdigit(c1) && isdigit(c2))
#else
		if(__ascii_isdigit(c1) && __ascii_isdigit(c2))
#endif
			continue;
#ifndef ASCII_STRCMP
		c1 = toupper(c1);
		c2 = toupper(c2);
#else
		c1 = __ascii_toupper(c1);
		c2 = __ascii_toupper(c2);
#endif

		if(c1 && c2 && c1 != c2)
			return false;
	}
}

CBaseModelInfo*
GetModelFromName(const char *name)
{
	int i;
	CBaseModelInfo *mi;
	char playername[32];

	if(strncasecmp(name, "CSplay", 6) == 0 &&
	   strncasecmp(CModelInfo::GetModelInfo(MI_PLAYER)->GetModelName(), "ig", 2) == 0){
		strcpy(playername, CModelInfo::GetModelInfo(MI_PLAYER)->GetModelName());
		playername[0] = 'C';
		playername[1] = 'S';
		name = playername;
	}

	for(i = 0; i < MODELINFOSIZE; i++){
		mi = CModelInfo::GetModelInfo(i);
		if(mi && mi->GetRwObject() && RwObjectGetType(mi->GetRwObject()) == rpCLUMP &&
		   strcmpIgnoringDigits(mi->GetModelName(), name))
			return mi;
	}
	return nil;
}

void
CAnimBlendAssocGroup::CreateAssociations(const char *name)
{
	int i;
	int skippedAssociations;

	DestroyAssociations();

	animBlock = CAnimManager::GetAnimationBlock(name);
	if(animBlock == nil){
		printf("[ANIM-GROUP] Missing animation block %s\n", name ? name : "<null>");
		return;
	}

	assocList = new CAnimBlendAssociation[animBlock->numAnims];
	numAssociations = 0;
	skippedAssociations = 0;

	for(i = 0; i < animBlock->numAnims; i++){
		CAnimBlendHierarchy *anim = CAnimManager::GetAnimation(animBlock->firstIndex + i);
		if(anim == nil){
			printf("[ANIM-GROUP] Null hierarchy block=%s index=%d/%d\n",
			       name, i, (int)animBlock->numAnims);
			skippedAssociations++;
			continue;
		}

		CBaseModelInfo *model = GetModelFromName(anim->name);
		if(model){
			debug("Associated anim %s with model %s\n", anim->name, model->GetModelName());
			RpClump *clump = (RpClump*)model->CreateInstance();
			if(clump == nil){
				printf("[ANIM-GROUP] CreateInstance failed block=%s anim=%s model=%s\n",
				       name, anim->name, model->GetModelName());
				skippedAssociations++;
				continue;
			}
			RpAnimBlendClumpInit(clump);
			assocList[numAssociations].Init(clump, anim);
			if(IsClumpSkinned(clump))
				RpClumpForAllAtomics(clump, AtomicRemoveAnimFromSkinCB, nil);
			RpClumpDestroy(clump);
			if(!IsUsableAnimAssociation(&assocList[numAssociations])){
				printf("[ANIM-GROUP] Init failed block=%s anim=%s model=%s\n",
				       name, anim->name, model->GetModelName());
				skippedAssociations++;
				continue;
			}
			assocList[numAssociations].animId = firstAnimId + i;
			assocList[numAssociations].groupId = groupId;
			numAssociations++;
		}else{
			printf("[ANIM-GROUP] Missing model for block=%s anim=%s\n", name, anim->name);
			debug("\n\nCANNOT FIND MODELINFO WITH NAME %s\n\n\n", anim->name);
			skippedAssociations++;
		}
	}
	if(skippedAssociations > 0){
		printf("[ANIM-GROUP] Created %d/%d associations for block=%s (skipped=%d)\n",
		       (int)numAssociations, (int)animBlock->numAnims, name, skippedAssociations);
	}
}

// Create associations from hierarchies for a given clump
void
CAnimBlendAssocGroup::CreateAssociations(const char *blockName, RpClump *clump, const char **animNames, int numAssocs)
{
	int i;
	bool failed;

	DestroyAssociations();

	animBlock = CAnimManager::GetAnimationBlock(blockName);
	if(animBlock == nil){
		printf("[ANIM-GROUP] Missing animation block %s\n", blockName ? blockName : "<null>");
		return;
	}
	if(clump == nil){
		printf("[ANIM-GROUP] CreateAssociations received nil clump for block=%s\n", blockName);
		return;
	}
	assocList = new CAnimBlendAssociation[numAssocs];

	failed = false;
	for(i = 0; i < numAssocs; i++){
		CAnimBlendHierarchy *anim = CAnimManager::GetAnimation(animNames[i], animBlock);
		if(anim == nil){
			printf("[ANIM-GROUP] Missing hierarchy block=%s anim=%s\n",
			       blockName, animNames[i] ? animNames[i] : "<null>");
			failed = true;
			break;
		}
		assocList[i].Init(clump, anim);
		if(!IsUsableAnimAssociation(&assocList[i])){
			printf("[ANIM-GROUP] Init failed block=%s anim=%s\n",
			       blockName, animNames[i] ? animNames[i] : "<null>");
			failed = true;
			break;
		}
		assocList[i].animId = firstAnimId + i;
		assocList[i].groupId = groupId;
	}
	if(failed){
		printf("[ANIM-GROUP] Rejecting incomplete group=%d block=%s\n", (int)groupId, blockName);
		DestroyAssociations();
		return;
	}
	numAssociations = numAssocs;
}
