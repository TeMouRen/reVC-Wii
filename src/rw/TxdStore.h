#ifndef __GTA_TXDSTORE_H__
#define __GTA_TXDSTORE_H__

#include "templates.h"

struct TxdDef {
	RwTexDictionary *texDict;
	int refCount;
	char name[20];
};

class CTxdStore
{
	static CPool<TxdDef,TxdDef> *ms_pTxdPool;
	static RwTexDictionary *ms_pStoredTxd;
public:
	static void Initialise(void);
	static void Shutdown(void);
	static void GameShutdown(void);
	static int AddTxdSlot(const char *name);
	static void RemoveTxdSlot(int slot);
	static int FindTxdSlot(const char *name);
	static char *GetTxdName(int slot);
	static void PushCurrentTxd(void);
	static void PopCurrentTxd(void);
	static void SetCurrentTxd(int slot);
	static void Create(int slot);
	static int GetNumRefs(int slot);
	static void AddRef(int slot);
	static void RemoveRef(int slot);
	static void RemoveRefWithoutDelete(int slot);
	static bool LoadTxd(int slot, RwStream *stream);
	static bool LoadTxd(int slot, const char *filename);
	static bool StartLoadTxd(int slot, RwStream *stream);
	static bool FinishLoadTxd(int slot, RwStream *stream);
	static void RemoveTxd(int slot);
	static void SetLoadingTxdContext(int slot, const char *name);
	static void ClearLoadingTxdContext(void);
	static int GetLoadingTxdSlot(void);
	static const char *GetLoadingTxdName(void);

	static TxdDef *GetSlot(int slot) {
		if(slot < 0) {
			printf("[TXD-ASSERT] GetSlot slot=%d caller=0x%08X\n", slot, (uint32)__builtin_return_address(0));
			return nil;
		}
		assert(ms_pTxdPool);
		assert(slot < ms_pTxdPool->GetSize());
		return ms_pTxdPool->GetSlot(slot);
	}
	static bool isTxdLoaded(int slot);
};

#endif // __GTA_TXDSTORE_H__
