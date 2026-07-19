#ifndef __WII_SAVE_H__
#define __WII_SAVE_H__

#if defined(WII)

#include <stddef.h>

bool WiiSaveSystemInit(void);
const char *WiiSaveGetDataDir(void);
bool WiiSavePathIsManaged(const char *path);
bool WiiSaveIsNoExistsError(int ret);
bool WiiSaveBuildTempPath(const char *finalPath, char *tempPath, size_t tempPathSize);
bool WiiSaveDeleteFile(const char *path);
bool WiiSaveCommitTempFile(const char *tempPath, const char *finalPath);

#endif

#endif
