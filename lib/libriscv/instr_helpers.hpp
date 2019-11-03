#pragma once
#include <time.h>

inline uint64_t u64_monotonic_time()
{
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return tp.tv_sec * 1000000000ull + tp.tv_nsec;
}
