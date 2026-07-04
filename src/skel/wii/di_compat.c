#include <ogc/lwp_watchdog.h>

int usleep(unsigned int usec)
{
    u64 start = gettime();
    u64 delta = ticks_to_microsecs(gettime() - start);
    while (delta < (u64)usec) {
        delta = ticks_to_microsecs(gettime() - start);
    }
    return 0;
}
