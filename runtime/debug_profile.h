/* The sampling profiler behind the F6 debug key (debug_profile.c). */
#pragma once

void profile_start(double seconds);   /* sample every thread for this long, then write a report */
int profile_running(void);
