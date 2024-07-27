#pragma once

#ifdef __APPLE__
#include "TargetConditionals.h" // TARGET_* macros
#endif

#ifdef __GNUG__
#define RISCV_NOINLINE __attribute__((noinline))
#define RISCV_UNREACHABLE() __builtin_unreachable()
#else
#define RISCV_NOINLINE    /* */
#define RISCV_UNREACHABLE()  /* */
#endif

#ifdef RISCV_32I
#define INSTANTIATE_32_IF_ENABLED(x) template struct x<4>
#else
#define INSTANTIATE_32_IF_ENABLED(x) /* */
#endif

#ifdef RISCV_64I
#define INSTANTIATE_64_IF_ENABLED(x) template struct x<8>
#else
#define INSTANTIATE_64_IF_ENABLED(x) /* */
#endif

#ifdef RISCV_128I
#define INSTANTIATE_128_IF_ENABLED(x) template struct x<16>
#else
#define INSTANTIATE_128_IF_ENABLED(x) /* */
#endif

#ifndef ANTI_FINGERPRINTING_MASK_MICROS
#define ANTI_FINGERPRINTING_MASK_MICROS()  ~0x3FFLL
#endif
#ifndef ANTI_FINGERPRINTING_MASK_NANOS
#define ANTI_FINGERPRINTING_MASK_NANOS()   ~0xFFFFFLL
#endif
