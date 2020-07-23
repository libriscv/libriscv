#pragma once

#define ATOMIC_INSTR(x, ...) INSTRUCTION(x, __VA_ARGS__)
#define FLOAT_INSTR(x, ...) INSTRUCTION(x, __VA_ARGS__)
#define COMPRESSED_INSTR(x, ...) INSTRUCTION(x, __VA_ARGS__)

#define DECODED_ATOMIC(x) DECODED_INSTR(x)
#define DECODED_FLOAT(x) DECODED_INSTR(x)
#define DECODED_COMPR(x) DECODED_INSTR(x)

#define CI_CODE(x, y) ((x << 13) | (y))
#define CIC2_CODE(x, y) ((x << 12) | (y))

#define RVREGTYPE(x) typename std::remove_reference_t<decltype(x)>::address_t
#define RVIS64BIT(x) (sizeof(RVREGTYPE(x)) == 8)

#include <time.h>

inline uint64_t u64_monotonic_time()
{
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return tp.tv_sec * 1000000000ull + tp.tv_nsec;
}
