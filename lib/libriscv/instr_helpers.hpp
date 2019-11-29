#pragma once

#ifdef RISCV_DEBUG
#define INSTRUCTION(x, ...) static CPU<4>::instruction_t instr32i_##x { __VA_ARGS__ }
#define ATOMIC_INSTR(x, ...) INSTRUCTION(x, __VA_ARGS__)
#define FLOAT_INSTR(x, ...) INSTRUCTION(x, __VA_ARGS__)
#define COMPRESSED_INSTR(x, ...) INSTRUCTION(x, __VA_ARGS__)
#else
#define INSTRUCTION(x, a, ...) static constexpr Instruction<4>::handler_t instr32i_##x {a}
#define ATOMIC_INSTR(x, a, b) INSTRUCTION(x, a, nullptr)
#define FLOAT_INSTR(x, a, ...) INSTRUCTION(x, a, nullptr)
#define COMPRESSED_INSTR(x, a, ...) INSTRUCTION(x, a, nullptr)
#endif

#define DECODED_INSTR(x) instr32i_##x
#define DECODED_ATOMIC(x) DECODED_INSTR(x)
#define DECODED_FLOAT(x) DECODED_INSTR(x)
#define DECODED_COMPR(x) DECODED_INSTR(x)

#define CI_CODE(x, y) ((x << 13) | (y))
#define CIC2_CODE(x, y) ((x << 12) | (y))

#include <time.h>

inline uint64_t u64_monotonic_time()
{
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return tp.tv_sec * 1000000000ull + tp.tv_nsec;
}
