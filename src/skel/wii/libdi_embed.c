#include <gccore.h>
#include "di_compat.h"

/*
 * libogc2 ships libdi source/headers here, but this environment does not
 * install a separate libdi.a into lib/wii. Embed the source directly so the
 * Wii disc-partition path can be used without changing the user's toolchain.
 */
#define static
#define DI_Init revc_original_DI_Init
#include "C:/devkitPro/libogc2/libdi/di.c"
#undef DI_Init
#undef static

int DI_Init(void)
{
    if (di_fd >= 0)
        return 1;

    state = DVD_INIT | DVD_NO_DISC;
    have_ahbprot = 1;

    if (di_fd < 0)
        di_fd = IOS_Open(di_path, 2);

    if (di_fd < 0)
        return di_fd;

    if (!bufferMutex)
        LWP_MutexInit(&bufferMutex, false);

    if (use_dvd_cache)
        CreateDVDCache();

    return 0;
}
