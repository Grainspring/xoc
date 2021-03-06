/*Copyright 2011 Alibaba Group*/
#include <sys/time.h>
#include <time.h>
#include "time/ctimeport.h"

static Float32 _sysTimeZone = 8.00;

UInt32 cTimeTicksPort(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((UInt64)tv.tv_sec)*1000+tv.tv_usec/1000;
}

//get time in milliseconds since GMT 1970-01-01 00:00:00 000
CTimeT cTimeGetTimePort(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((UInt64)tv.tv_sec)*1000+tv.tv_usec/1000;
}

CTimeT cTimeGetNanoTimePort(void) {
#ifdef HAVE_POSIX_CLOCKS
    struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return (UInt64)now.tv_sec*1000000000LL + now.tv_nsec;
#else
    CTimeT t;
    t = cTimeGetTimePort();
    return ((UInt64)t)*1000000;
#endif
}

//from -12.00 to +12.00
void cTimeSetTimeZonePort(Float32 tz) {
    _sysTimeZone = tz;
}

Float32 cTimeGetTimeZonePort(void) {
    return _sysTimeZone;
}

void cStopSystemTimerPort() {
}

void cStartSystemTimerPort(Int32 pre, void* args) {
}
