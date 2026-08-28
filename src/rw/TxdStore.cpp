#include "common.h"

#include "templates.h"
#include "General.h"
#include "Streaming.h"
#include "RwHelper.h"
#include "TxdStore.h"

CPool<TxdDef,TxdDef> *CTxdStore::ms_pTxdPool;
RwTexDictionary *CTxdStore::ms_pStoredTxd;
static int g_loadingTxdSlot = -1;
static char g_loadingTxdName[20];

#ifdef WII
bool
RwWiiIsLoadingSplashTxd(void)
{
	return strcmp(g_loadingTxdName, "splash") == 0;
}
#endif

// Define GX_MISSING_TEXTURE_DIAGNOSTICS for targeted alias/TXD lifetime probes.

static void
WiiReportAliasLifetime(const char *action, int slot, const TxdDef *def)
{
#if defined(WII) && defined(GX_MISSING_TEXTURE_DIAGNOSTICS)
	static int s_aliasLifetimeLogCount = 0;
	if(def && s_aliasLifetimeLogCount < 64){
		SYS_Report("[TXD-ALIAS-PIN] action=%s slot=%d name='%s' refs=%d pins=%d dict=%p\n",
		           action ? action : "unknown", slot, def->name, def->refCount,
		           def->aliasPinCount, (void*)def->texDict);
		s_aliasLifetimeLogCount++;
	}
#else
	(void)action;
	(void)slot;
	(void)def;
#endif
}

#if defined(WII) && defined(GX_MISSING_TEXTURE_DIAGNOSTICS)
struct WiiTxdLiveTextureDiag
{
	int totalTextures;
	int externallyReferencedTextures;
	int loggedTextures;
	RwTexture *sampleTextures[4];
};

static RwTexture *
WiiCountTxdLiveTexture(RwTexture *texture, void *data)
{
	WiiTxdLiveTextureDiag *diag = (WiiTxdLiveTextureDiag*)data;
	diag->totalTextures++;
	if(texture->refCount <= 1)
		return texture;

	diag->externallyReferencedTextures++;
	if(diag->loggedTextures < 4)
		diag->sampleTextures[diag->loggedTextures++] = texture;
	return texture;
}

static void
WiiReportTxdLiveTexturesBeforeDestroy(int slot, TxdDef *def, const char *reason)
{
	static int s_liveTxdReportCount = 0;
	if(def == nil || def->texDict == nil)
		return;

	WiiTxdLiveTextureDiag diag = { 0, 0, 0, { nil, nil, nil, nil } };
	RwTexDictionaryForAllTextures(def->texDict, WiiCountTxdLiveTexture, &diag);
	if(diag.externallyReferencedTextures > 0 && s_liveTxdReportCount < 48){
		SYS_Report("[TXD-EVICT-LIVE-TEX] reason=%s slot=%d name='%s' refs=%d dict=%p textures=%d external=%d\n",
		           reason ? reason : "unknown", slot, def->name, def->refCount,
		           (void*)def->texDict, diag.totalTextures,
		           diag.externallyReferencedTextures);
		for(int i = 0; i < diag.loggedTextures; i++){
			RwTexture *texture = diag.sampleTextures[i];
			SYS_Report("[TXD-EVICT-LIVE-TEX-ITEM] tex=%p name='%s' mask='%s' refs=%d raster=%p\n",
			           (void*)texture, texture->name, texture->mask,
			           texture->refCount, (void*)texture->raster);
		}
		s_liveTxdReportCount++;
	}
}
#endif

void
CTxdStore::Initialise(void)
{
	if(ms_pTxdPool == nil)
		ms_pTxdPool = new CPool<TxdDef,TxdDef>(TXDSTORESIZE, "TexDictionary");
	rw::Texture::setAliasLifetimeCallbacks(CTxdStore::PinAliasTexture,
	                                       CTxdStore::ReleaseAliasTexture);
}

void
CTxdStore::Shutdown(void)
{
	rw::Texture::setAliasLifetimeCallbacks(nil, nil);
	if(ms_pTxdPool)
		delete ms_pTxdPool;
	ms_pTxdPool = nil;
}

void
CTxdStore::GameShutdown(void)
{
	int i;

	for(i = 0; i < TXDSTORESIZE; i++){
		TxdDef *def = GetSlot(i);
		if(def && GetNumRefs(i) == 0)
			RemoveTxdSlot(i);
	}
}

int
CTxdStore::AddTxdSlot(const char *name)
{
	int existing = FindTxdSlot(name);
	if(existing >= 0){
		TxdDef *existingDef = GetSlot(existing);
		if(existingDef && existingDef->aliasPinCount > 0){
			WiiReportAliasLifetime("add-slot-reuse", existing, existingDef);
			return existing;
		}
	}
	TxdDef *def = ms_pTxdPool->New();
	assert(def);
	def->texDict = nil;
	def->refCount = 0;
	def->aliasPinCount = 0;
	strcpy(def->name, name);
	return ms_pTxdPool->GetJustIndex(def);
}

bool
CTxdStore::RemoveTxdSlot(int slot)
{
	TxdDef *def = GetSlot(slot);
	if(def->aliasPinCount > 0){
		WiiReportAliasLifetime("remove-slot-blocked", slot, def);
		return false;
	}
	if(def->texDict)
		RwTexDictionaryDestroy(def->texDict);
	ms_pTxdPool->Delete(def);
	return true;
}

int
CTxdStore::FindTxdSlot(const char *name)
{
	int size = ms_pTxdPool->GetSize();
	for(int i = 0; i < size; i++){
		TxdDef *def = GetSlot(i);
		if(def && !CGeneral::faststricmp(def->name, name))
			return i;
	}
	return -1;
}

char*
CTxdStore::GetTxdName(int slot)
{
	return GetSlot(slot)->name;
}

void
CTxdStore::PushCurrentTxd(void)
{
	ms_pStoredTxd = RwTexDictionaryGetCurrent();
}

void
CTxdStore::PopCurrentTxd(void)
{
	RwTexDictionarySetCurrent(ms_pStoredTxd);
	ms_pStoredTxd = nil;
}

void
CTxdStore::SetCurrentTxd(int slot)
{
	RwTexDictionarySetCurrent(GetSlot(slot)->texDict);
}

bool
CTxdStore::Create(int slot)
{
	TxdDef *def = GetSlot(slot);
	if(def->aliasPinCount > 0){
		WiiReportAliasLifetime("create-blocked", slot, def);
		return false;
	}
	def->texDict = RwTexDictionaryCreate();
	return def->texDict != nil;
}

int
CTxdStore::GetNumRefs(int slot)
{
	return GetSlot(slot)->refCount;
}

void
CTxdStore::AddRef(int slot)
{
	GetSlot(slot)->refCount++;
}

void
CTxdStore::RemoveRef(int slot)
{
	if(--GetSlot(slot)->refCount <= 0)
		CStreaming::RemoveTxd(slot);
}

void
CTxdStore::RemoveRefWithoutDelete(int slot)
{
	GetSlot(slot)->refCount--;
}

int
CTxdStore::FindTxdSlotByDictionary(RwTexDictionary *dict)
{
	if(dict == nil || ms_pTxdPool == nil)
		return -1;
	int size = ms_pTxdPool->GetSize();
	for(int i = 0; i < size; i++){
		TxdDef *def = GetSlot(i);
		if(def && def->texDict == dict)
			return i;
	}
	return -1;
}

rw::bool32
CTxdStore::PinAliasTexture(RwTexDictionary *dict)
{
	int slot = FindTxdSlotByDictionary(dict);
	if(slot < 0)
		return true;	// The global/initial dictionary is not stream-managed.
	TxdDef *def = GetSlot(slot);
	if(def == nil || def->texDict != dict)
		return false;
	def->aliasPinCount++;
	def->refCount++;
	WiiReportAliasLifetime("acquire", slot, def);
	return true;
}

void
CTxdStore::ReleaseAliasTexture(RwTexDictionary *dict)
{
	int slot = FindTxdSlotByDictionary(dict);
	if(slot < 0)
		return;
	TxdDef *def = GetSlot(slot);
	if(def == nil || def->aliasPinCount <= 0)
		return;
	def->aliasPinCount--;
	if(def->refCount > 0)
		def->refCount--;
	// Do not remove the dictionary from this Texture::destroy callback. The
	// pin is part of refCount, so streaming will retry it on the next trim once
	// the donor becomes unreferenced without re-entering dictionary destruction.
	WiiReportAliasLifetime("release", slot, def);
}

bool
CTxdStore::IsTxdAliasPinned(int slot)
{
	if(ms_pTxdPool == nil || slot < 0 || slot >= ms_pTxdPool->GetSize())
		return false;
	TxdDef *def = GetSlot(slot);
	return def != nil && def->aliasPinCount > 0;
}

bool
CTxdStore::LoadTxd(int slot, RwStream *stream)
{
	TxdDef *def = GetSlot(slot);

	if(RwStreamFindChunk(stream, rwID_TEXDICTIONARY, nil, nil)){
		if(def->texDict){
			if(def->aliasPinCount > 0){
				WiiReportAliasLifetime("load-replace-blocked", slot, def);
				return false;
			}
			printf("[TXD-RELOAD] replacing loaded txd '%s' slot=%d refs=%d\n",
			       def->name, slot, def->refCount);
		#if defined(WII) && defined(GX_MISSING_TEXTURE_DIAGNOSTICS)
			WiiReportTxdLiveTexturesBeforeDestroy(slot, def, "load-replace");
		#endif
			RwTexDictionaryDestroy(def->texDict);
			def->texDict = nil;
		}
		SetLoadingTxdContext(slot, def->name);
		def->texDict = RwTexDictionaryGtaStreamRead(stream);
		ClearLoadingTxdContext();
			((void)0); // [GC-DEBUG-DISABLED]
		return def->texDict != nil;
	}
	printf("Failed to load TXD\n");
	return false;
}

bool
CTxdStore::LoadTxd(int slot, const char *filename)
{
	RwStream *stream;
	bool ret;

	ret = false;
#ifdef GTA_PC
	_rwD3D8TexDictionaryEnableRasterFormatConversion(true);
#endif
	do
		stream = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, filename);
	while(stream == nil);
	ret = LoadTxd(slot, stream);
	RwStreamClose(stream, nil);
	return ret;
}

bool
CTxdStore::StartLoadTxd(int slot, RwStream *stream)
{
	TxdDef *def = GetSlot(slot);
	if(RwStreamFindChunk(stream, rwID_TEXDICTIONARY, nil, nil)){
		if(def->texDict){
			if(def->aliasPinCount > 0){
				WiiReportAliasLifetime("start-load-replace-blocked", slot, def);
				return false;
			}
			printf("[TXD-RELOAD] replacing started txd '%s' slot=%d refs=%d\n",
			       def->name, slot, def->refCount);
		#if defined(WII) && defined(GX_MISSING_TEXTURE_DIAGNOSTICS)
			WiiReportTxdLiveTexturesBeforeDestroy(slot, def, "start-load-replace");
		#endif
			RwTexDictionaryDestroy(def->texDict);
			def->texDict = nil;
		}
		SetLoadingTxdContext(slot, def->name);
		def->texDict = RwTexDictionaryGtaStreamRead1(stream);
		ClearLoadingTxdContext();
		return def->texDict != nil;
	}else{
		printf("Failed to load TXD\n");
		return false;
	}
}

bool
CTxdStore::FinishLoadTxd(int slot, RwStream *stream)
{
	TxdDef *def = GetSlot(slot);
	SetLoadingTxdContext(slot, def->name);
	def->texDict = RwTexDictionaryGtaStreamRead2(stream, def->texDict);
	ClearLoadingTxdContext();
	return def->texDict != nil;
}

bool
CTxdStore::RemoveTxd(int slot)
{
	TxdDef *def = GetSlot(slot);
	if(def->aliasPinCount > 0){
	#if WII_STREAM_LIFECYCLE_AUDIT
		printf("[WII-LIFE] event=txd_remove_blocked txd='%s' slot=%d refs=%d alias_pins=%d reason=alias_pin\n",
		       def->name, slot, def->refCount, def->aliasPinCount);
	#endif
		WiiReportAliasLifetime("stream-remove-blocked", slot, def);
		return false;
	}
	#if defined(WII) && defined(GX_MISSING_TEXTURE_DIAGNOSTICS)
	WiiReportTxdLiveTexturesBeforeDestroy(slot, def, "stream-remove");
	#endif
	if(def->texDict)
		RwTexDictionaryDestroy(def->texDict);
	def->texDict = nil;
	return true;
}

void
CTxdStore::SetLoadingTxdContext(int slot, const char *name)
{
	g_loadingTxdSlot = slot;
	if(name){
		strncpy(g_loadingTxdName, name, sizeof(g_loadingTxdName) - 1);
		g_loadingTxdName[sizeof(g_loadingTxdName) - 1] = '\0';
	}else
		g_loadingTxdName[0] = '\0';
}

void
CTxdStore::ClearLoadingTxdContext(void)
{
	g_loadingTxdSlot = -1;
	g_loadingTxdName[0] = '\0';
}

int
CTxdStore::GetLoadingTxdSlot(void)
{
	return g_loadingTxdSlot;
}

const char*
CTxdStore::GetLoadingTxdName(void)
{
	return g_loadingTxdName[0] ? g_loadingTxdName : "<none>";
}
