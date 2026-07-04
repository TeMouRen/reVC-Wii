// gcm_vfs.h  —  GCM Virtual File System public interface
// Register "dvd:/" as a newlib devoptab device backed by the
// GameCube optical disc FST.  After GCM_VFS_Mount() succeeds,
// standard fopen("dvd:/path/to/file","rb") works transparently.

#ifndef GCM_VFS_H
#define GCM_VFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/**
 * Read the disc FST and register "dvd" as a newlib device.
 * Must be called AFTER DVD_Init() and DVD_Mount().
 * Safe to call multiple times (no-op if already mounted).
 * @return true on success, false on read/format error.
 */
bool GCM_VFS_Mount(void);

/**
 * Unregister the device and free FST memory.
 * Normally not needed during gameplay (FST lives forever).
 */
void GCM_VFS_Unmount(void);

#ifdef __cplusplus
}
#endif

#endif /* GCM_VFS_H */