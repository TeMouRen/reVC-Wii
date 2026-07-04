#ifndef __GTA_MEMORYMGR_H__
#define __GTA_MEMORYMGR_H__

extern RwMemoryFunctions memFuncs;
void InitMemoryMgr(void);

void *MemoryMgrMalloc(size_t size);
void *MemoryMgrRealloc(void *ptr, size_t size);
void *MemoryMgrCalloc(size_t num, size_t size);
void MemoryMgrFree(void *ptr);

void *RwMallocAlign(RwUInt32 size, RwUInt32 align);
void RwFreeAlign(void *mem);

#ifdef WII
void *MemoryMgrMallocMem2(size_t size, size_t align);
void MemoryMgrFreeMem2(void *ptr);
void *MemoryMgrMallocMem2Strict(size_t size, size_t align);
void *MemoryMgrMallocAlignMem2(size_t size, size_t align);
void MemoryMgrFreeAlignMem2(void *mem);
void *MemoryMgrMallocAlignMem2Strict(size_t size, size_t align);
#endif

#endif // __GTA_MEMORYMGR_H__
