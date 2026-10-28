/* Clocks and sleeping. */
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "sys.h"

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    struct espix_timespec e;
    const long r = espix_sys2(ESPIX_SYS_CLOCK_GETTIME, clk, &e);
    if (r < 0) {
        return espix_ret(r);
    }
    ts->tv_sec  = (time_t)e.sec;
    ts->tv_nsec = e.nsec;
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }
    tv->tv_sec  = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    const struct espix_timespec e = { .sec = req->tv_sec, .nsec = (int32_t)req->tv_nsec };
    struct espix_timespec left = { 0 };
    const long r = espix_sys2(ESPIX_SYS_NANOSLEEP, &e, &left);
    if (rem != NULL) {
        rem->tv_sec  = (time_t)left.sec;
        rem->tv_nsec = left.nsec;
    }
    return espix_ret(r);
}

unsigned sleep(unsigned secs)
{
    struct timespec req = { .tv_sec = secs }, rem = { 0 };
    return (nanosleep(&req, &rem) == 0) ? 0 : (unsigned)rem.tv_sec;
}

int usleep(useconds_t us)
{
    const struct timespec req = { .tv_sec = us / 1000000, .tv_nsec = (long)(us % 1000000) * 1000 };
    return nanosleep(&req, NULL);
}
