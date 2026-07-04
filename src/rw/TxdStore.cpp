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

void
CTxdStore::Initialise(void)
{
	if(ms_pTxdPool == nil)
		ms_pTxdPool = new CPool<TxdDef,TxdDef>(TXDSTORESIZE, "TexDictionary");
}

void
CTxdStore::Shutdown(void)
{
	if(ms_pTxdPool)
		delete ms_pTxdPool;
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
	TxdDef *def = ms_pTxdPool->New();
	assert(def);
	def->texDict = nil;
	def->refCount = 0;
	strcpy(def->name, name);
	return ms_pTxdPool->GetJustIndex(def);
}

void
CTxdStore::RemoveTxdSlot(int slot)
{
	TxdDef *def = GetSlot(slot);
	if(def->texDict)
		RwTexDictionaryDestroy(def->texDict);
	ms_pTxdPool->Delete(def);
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

void
CTxdStore::Create(int slot)
{
	GetSlot(slot)->texDict = RwTexDictionaryCreate();
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

bool
CTxdStore::LoadTxd(int slot, RwStream *stream)
{
	TxdDef *def = GetSlot(slot);

	if(RwStreamFindChunk(stream, rwID_TEXDICTIONARY, nil, nil)){
		if(def->texDict){
			printf("[TXD-RELOAD] replacing loaded txd '%s' slot=%d refs=%d\n",
			       def->name, slot, def->refCount);
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
			printf("[TXD-RELOAD] replacing started txd '%s' slot=%d refs=%d\n",
			       def->name, slot, def->refCount);
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

void
CTxdStore::RemoveTxd(int slot)
{
	TxdDef *def = GetSlot(slot);
	if(def->texDict)
		RwTexDictionaryDestroy(def->texDict);
	def->texDict = nil;
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
