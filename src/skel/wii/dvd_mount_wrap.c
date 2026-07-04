#include <gccore.h>
#include <ogc/dvd.h>

#ifdef WII
/*
 * Wii disc titles arrive here after the apploader has already opened the game
 * partition. Re-running DVD_Mount() from main() resets that state, and later
 * DVD reads no longer see the partition-relative header/FST that reVC expects.
 */
s32 __wrap_DVD_Mount(void)
{
    SYS_Report("[reVC-WII] Skipping redundant DVD_Mount(); apploader already mounted partition.\n");
    return 0;
}
#endif
