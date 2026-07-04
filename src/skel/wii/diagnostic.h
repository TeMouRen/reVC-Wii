// diagnostic.h â€?Frame-counter-gated logging for Wii boot diagnostics
//
// Usage:
//   DIAG_LOG(fmt, ...)    â€?prints only in first g_diagVerbose frames
//   DIAG_NOTE(fmt, ...)   â€?prints every 60 frames
//   DIAG_ERROR(fmt, ...)  â€?always prints
//
// To enable in any .cpp file:
//   #if defined(WII) || defined(GAMECUBE)
//   #include "../skel/wii/diagnostic.h"
//   #endif

#ifndef GC_DIAGNOSTIC_H
#define GC_DIAGNOSTIC_H

#if defined(WII) || defined(GAMECUBE)
#include <gccore.h>

extern int  g_diagFrame;
extern int  g_diagVerbose;

#define DIAG_LOG(fmt, ...) \
    do { if (g_diagFrame <= g_diagVerbose) \
        printf("[D%02d] " fmt, g_diagFrame, ##__VA_ARGS__); } while(0)
#define DIAG_NOTE(fmt, ...) \
    do { if (g_diagFrame <= g_diagVerbose || g_diagFrame % 60 == 0) \
        printf("[D%04d] " fmt, g_diagFrame, ##__VA_ARGS__); } while(0)
#define DIAG_ERROR(fmt, ...) \
    printf("[ERR D%04d] " fmt, g_diagFrame, ##__VA_ARGS__)

#endif // WII || GAMECUBE
#endif // GC_DIAGNOSTIC_H
