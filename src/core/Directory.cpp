#include "common.h"

#include "General.h"
#include "FileMgr.h"
#include "Directory.h"

CDirectory::CDirectory(int32 maxEntries)
 : numEntries(0), maxEntries(maxEntries)
{
	entries = new DirectoryInfo[maxEntries];
}

CDirectory::~CDirectory(void)
{
	delete[] entries;
}

void
CDirectory::ReadDirFile(const char *filename)
{
	int fd;
	DirectoryInfo dirinfo;

	fd = CFileMgr::OpenFile(filename, "rb");
	while(CFileMgr::Read(fd, (char*)&dirinfo, sizeof(dirinfo))) {
#if defined(GAMECUBE) || defined(RW_BIG_ENDIAN)
		// DIR file is little-endian (PC format), GC is big-endian
		dirinfo.offset = ((dirinfo.offset & 0xFF000000) >> 24) |
		                 ((dirinfo.offset & 0x00FF0000) >> 8) |
		                 ((dirinfo.offset & 0x0000FF00) << 8) |
		                 ((dirinfo.offset & 0x000000FF) << 24);
		dirinfo.size   = ((dirinfo.size & 0xFF000000) >> 24) |
		                 ((dirinfo.size & 0x00FF0000) >> 8) |
		                 ((dirinfo.size & 0x0000FF00) << 8) |
		                 ((dirinfo.size & 0x000000FF) << 24);
#endif
		AddItem(dirinfo);
	}
	CFileMgr::CloseFile(fd);
}

bool
CDirectory::WriteDirFile(const char *filename)
{
	int fd;
	size_t n;
	fd = CFileMgr::OpenFileForWriting(filename);
	n = CFileMgr::Write(fd, (char*)entries, numEntries*sizeof(DirectoryInfo));
	CFileMgr::CloseFile(fd);
	return n == numEntries*sizeof(DirectoryInfo);
}

void
CDirectory::AddItem(const DirectoryInfo &dirinfo)
{
	// [GC-FIX] 如果对象本身是 NULL 或底层数组未分配，
	// 绝对不可执行写入或扩容（GameCube 0x0 地址可读会误判数组有效）
	if (this == nil || entries == nil) {
		return;
	}

#ifdef FIX_BUGS
	// don't add if already exists
	uint32 offset, size;
	if(FindItem(dirinfo.name, offset, size))
		return;
#endif
	// Dynamic resize when full — prevents heap corruption from writing past array bounds.
	// Original code asserted then wrote anyway, which trashed malloc metadata on GameCube.
	if (numEntries >= maxEntries) {
		int32 newMax = maxEntries * 2;
		DirectoryInfo *newEntries = new DirectoryInfo[newMax];
		if (newEntries == nil) {
			// Can't grow — silently drop item. Better than heap corruption.
			return;
		}
		memcpy(newEntries, entries, numEntries * sizeof(DirectoryInfo));
		delete[] entries;
		entries = newEntries;
		maxEntries = newMax;
	}
	entries[numEntries++] = dirinfo;
}

void
CDirectory::AddItem(const DirectoryInfo &dirinfo, int32 imgId)
{
	DirectoryInfo di = dirinfo;
	di.offset |= imgId<<24;
	AddItem(di);
}

bool
CDirectory::FindItem(const char *name, uint32 &offset, uint32 &size)
{
	int i;

	for(i = 0; i < numEntries; i++)
		if(!CGeneral::faststricmp(entries[i].name, name)){
			offset = entries[i].offset;
			size = entries[i].size;
			return true;
		}
	return false;
}
