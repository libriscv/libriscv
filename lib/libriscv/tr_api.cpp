#include <string>

namespace riscv {
	extern const std::string bintr_code =
		R"123(
#if (defined(__TINYC__) && defined(__FreeBSD__))
#define int8_t   char
#define uint8_t  unsigned char
#define int16_t  short
#define uint16_t unsigned short
#define int32_t  int
#define uint32_t unsigned int
#define int64_t  long long
#define uint64_t unsigned long long
#define uintptr_t unsigned long long
#elif defined(__TINYC__) && defined(_WIN32)
#define int8_t   char
#define uint8_t  unsigned char
#define int16_t  short
#define uint16_t unsigned short
#define int32_t  int
#define uint32_t unsigned int
#define int64_t  long long
#define uint64_t unsigned long long
#define uintptr_t unsigned long long
#elif defined(__TINYC__) && defined(__ANDROID__)
#define uintptr_t unsigned long
#elif defined(__TINYC__)
#define int8_t   signed char
#define uint8_t  unsigned char
#define int16_t  short
#define uint16_t unsigned short
#define int32_t  int
#define uint32_t unsigned int
#define int64_t  long
#define uint64_t unsigned long
#define uintptr_t unsigned long
#else
#include <stdint.h>
#endif
#define LIKELY(x) __builtin_expect((x), 1)
#define UNLIKELY(x) __builtin_expect((x), 0)
#define ILLEGAL_OPCODE  0
#define MISALIGNED_INSTRUCTION 4
#define VISIBLE  __attribute__((visibility("default")))
#define INTERNAL __attribute__((visibility("hidden")))

#if RISCV_TRANSLATION_DYLIB == 4
	typedef uint32_t addr_t;
	typedef int32_t saddr_t;
#  define XLEN  32
#elif RISCV_TRANSLATION_DYLIB == 8
	typedef uint64_t addr_t;
	typedef int64_t saddr_t;
#  define XLEN  64
#elif RISCV_TRANSLATION_DYLIB == 16
	typedef __uint128_t addr_t;
	typedef __int128_t saddr_t;
#endif
#ifdef RISCV_EXT_C
#define RISCV_ALIGN_MASK 0x1
#else
#define RISCV_ALIGN_MASK 0x3
#endif

#define HOST_UNKNOWN 0
#define HOST_AMD64   1

#ifdef __TINYC__
#define UNREACHABLE() /**/
static inline float fminf(float x, float y) {
	return (x < y) ? x : y;
}
static inline double fmin(double x, double y) {
	return (x < y) ? x : y;
}
static inline float fmaxf(float x, float y) {
	return (x >= y) ? x : y;
}
static inline double fmax(double x, double y) {
	return (x >= y) ? x : y;
}
static inline float truncf(float x) {
	union { float f; uint32_t u; } v;
	v.f = x;
	int exp = ((v.u >> 23) & 0xFF) - 127;
	if (exp < 0) {
		v.u &= 0x80000000u;
	} else if (exp < 23) {
		v.u &= ~((1u << (23 - exp)) - 1u);
	}
	return v.f;
}
static inline float floorf(float x) {
	float t = truncf(x);
	return (t > x) ? t - 1.0f : t;
}
static inline double trunc(double x) {
	union { double f; uint64_t u; } v;
	v.f = x;
	int exp = (int)((v.u >> 52) & 0x7FF) - 1023;
	if (exp < 0) {
		v.u &= 0x8000000000000000ull;
	} else if (exp < 52) {
		v.u &= ~((1ull << (52 - exp)) - 1ull);
	}
	return v.f;
}
static inline double floor(double x) {
	double t = trunc(x);
	return (t > x) ? t - 1.0 : t;
}
static inline float ceilf(float x) {
	float t = truncf(x);
	return (t < x) ? t + 1.0f : t;
}
static inline double ceil(double x) {
	double t = trunc(x);
	return (t < x) ? t + 1.0 : t;
}
static inline float roundf(float x) { /* nearest, ties away from zero */
	float t = truncf(x);
	float d = x - t;
	if (d >= 0.5f) return t + 1.0f;
	if (d <= -0.5f) return t - 1.0f;
	return t;
}
static inline double round(double x) {
	double t = trunc(x);
	double d = x - t;
	if (d >= 0.5) return t + 1.0;
	if (d <= -0.5) return t - 1.0;
	return t;
}
static inline float nearbyintf(float x) { /* nearest, ties to even */
	float t = truncf(x);
	float d = x - t;
	if (d > 0.5f) return t + 1.0f;
	if (d < -0.5f) return t - 1.0f;
	if (d == 0.5f || d == -0.5f) {
		long long it = (long long)t;
		if (it & 1LL) return t + ((d > 0.0f) ? 1.0f : -1.0f);
	}
	return t;
}
static inline double nearbyint(double x) {
	double t = trunc(x);
	double d = x - t;
	if (d > 0.5) return t + 1.0;
	if (d < -0.5) return t - 1.0;
	if (d == 0.5 || d == -0.5) {
		long long it = (long long)t;
		if (it & 1LL) return t + ((d > 0.0) ? 1.0 : -1.0);
	}
	return t;
}
#ifndef __builtin_inff
#define __builtin_inff() (1.0f / 0.0f)
#define __builtin_inf() (1.0 / 0.0)
#endif
#ifndef __builtin_fabsl
#define __builtin_fabsl(x) ((x) < 0.0L ? -(x) : (x))
#endif
/* Minimal nextafter for the TinyCC path. Only ever called on a finite
   non-zero x by the FCVT int->float rounding-mode correction, so the
   zero-crossing and NaN/inf edge cases are deliberately not handled. */
static inline float nextafterf(float x, float y) {
	if (x == y) return y;
	union { float f; uint32_t u; } v = { x };
	if (v.u == 0x80000000u) v.u = 0;
	uint32_t sign = v.u & 0x80000000u;
	if (((y > x) != 0) ^ (sign != 0)) v.u++;
	else v.u--;
	return v.f;
}
static inline double nextafter(double x, double y) {
	if (x == y) return y;
	union { double f; uint64_t u; } v = { x };
	if (v.u == 0x8000000000000000ull) v.u = 0;
	uint64_t sign = v.u & 0x8000000000000000ull;
	if (((y > x) != 0) ^ (sign != 0)) v.u++;
	else v.u--;
	return v.f;
}
static inline uint32_t do_bswap32(uint32_t x) {
	return (x << 24 | (x & 0xFF00) << 8 | (x & 0xFF0000) >> 8 | x >> 24);
}
#define do_bswap64(x) ((uint64_t)do_bswap32((uint64_t)(x) >> 32) | ((uint64_t)do_bswap32((uint32_t)(x)) << 32))
#define do_clz(x) api.clz(x)
#define do_clzl(x) api.clzl(x)
#define do_ctz(x) api.ctz(x)
#define do_ctzl(x) api.ctzl(x)
#define do_cpop(x) api.cpop(x)
#define do_cpopl(x) api.cpopl(x)
#else
#define UNREACHABLE() __builtin_unreachable()
#define do_bswap32(x) __builtin_bswap32(x)
#define do_bswap64(x) __builtin_bswap64(x)
#define do_clz(x) __builtin_clz(x)
#define do_clzl(x) __builtin_clzl(x)
#define do_ctz(x) __builtin_ctz(x)
#define do_ctzl(x) __builtin_ctzl(x)
#define do_cpop(x) __builtin_popcount(x)
#define do_cpopl(x) __builtin_popcountl(x)
#define fminf(x, y) __builtin_fminf(x, y)
#define fmin(x, y) __builtin_fmin(x, y)
#define fmaxf(x, y) __builtin_fmaxf(x, y)
#define fmax(x, y) __builtin_fmax(x, y)
#define truncf(x) __builtin_truncf(x)
#define trunc(x)  __builtin_trunc(x)
#define floorf(x) __builtin_floorf(x)
#define floor(x)  __builtin_floor(x)
#define ceilf(x)  __builtin_ceilf(x)
#define ceil(x)   __builtin_ceil(x)
#define roundf(x) __builtin_roundf(x)
#define round(x)  __builtin_round(x)
#define nearbyintf(x) __builtin_nearbyintf(x)
#define nearbyint(x)  __builtin_nearbyint(x)
#define nextafterf(x, y) __builtin_nextafterf(x, y)
#define nextafter(x, y)  __builtin_nextafter(x, y)
#endif

static inline uint64_t do_brev8(uint64_t x) {
	x = ((x & 0xF0F0F0F0F0F0F0F0ull) >> 4) | ((x & 0x0F0F0F0F0F0F0F0Full) << 4);
	x = ((x & 0xCCCCCCCCCCCCCCCCull) >> 2) | ((x & 0x3333333333333333ull) << 2);
	x = ((x & 0xAAAAAAAAAAAAAAAAull) >> 1) | ((x & 0x5555555555555555ull) << 1);
	return x;
}
static inline uint32_t do_zip32(uint32_t x) {
	uint32_t lo = x & 0xFFFF;
	uint32_t hi = x >> 16;
	lo = (lo | (lo << 8)) & 0x00FF00FF;
	lo = (lo | (lo << 4)) & 0x0F0F0F0F;
	lo = (lo | (lo << 2)) & 0x33333333;
	lo = (lo | (lo << 1)) & 0x55555555;
	hi = (hi | (hi << 8)) & 0x00FF00FF;
	hi = (hi | (hi << 4)) & 0x0F0F0F0F;
	hi = (hi | (hi << 2)) & 0x33333333;
	hi = (hi | (hi << 1)) & 0x55555555;
	return lo | (hi << 1);
}
static inline uint32_t do_unzip32(uint32_t x) {
	uint32_t lo = x & 0x55555555;
	uint32_t hi = (x >> 1) & 0x55555555;
	lo = (lo | (lo >> 1)) & 0x33333333;
	lo = (lo | (lo >> 2)) & 0x0F0F0F0F;
	lo = (lo | (lo >> 4)) & 0x00FF00FF;
	lo = (lo | (lo >> 8)) & 0x0000FFFF;
	hi = (hi | (hi >> 1)) & 0x33333333;
	hi = (hi | (hi >> 2)) & 0x0F0F0F0F;
	hi = (hi | (hi >> 4)) & 0x00FF00FF;
	hi = (hi | (hi >> 8)) & 0x0000FFFF;
	return lo | (hi << 16);
}

#ifdef __HAVE_BUILTIN_SPECULATION_SAFE_VALUE
#define SPECSAFE(x) __builtin_speculation_safe_value(x)
#else
#define SPECSAFE(x) (x)
#endif

#ifdef RISCV_EXT_VECTOR
typedef union {
	uint8_t  u8 [RISCV_EXT_VECTOR];
	uint16_t u16[RISCV_EXT_VECTOR / 2];
	uint32_t u32[RISCV_EXT_VECTOR / 4];
	uint64_t u64[RISCV_EXT_VECTOR / 8];
	float    f32[RISCV_EXT_VECTOR / 4];
	double   f64[RISCV_EXT_VECTOR / 8];
} VectorLane __attribute__ ((aligned (RISCV_EXT_VECTOR)));

typedef struct {
	uint8_t b[RISCV_EXT_VECTOR];
} VectorLaneBytes;

/* Mirror of VectorRegisters<W> (rvv_registers.hpp), field for field. The
   translator emits _Static_asserts on every offset, so a stale mirror is a
   compile error instead of a silently disabled inline fast path. */
typedef struct {
	VectorLane  lane[32];
	addr_t      vl;      /* active vector length */
	addr_t      vstart;  /* first active element */
	uint32_t    vsew;    /* log2(SEW / 8) */
	uint32_t    vtype;   /* the raw vtype encoding */
	int32_t     lmul;    /* log2(LMUL) */
	uint8_t     vxrm, vxsat, vta, vma, vill;
} RVV __attribute__ ((aligned (RISCV_EXT_VECTOR)));
#endif

typedef union {
	int32_t i32[2];
	float   f32[2];
	int64_t i64;
	double  f64;
	struct {
		uint32_t bits  : 31;
		uint32_t sign  : 1;
		uint32_t upper;
	} lsign;
	struct {
		uint64_t bits  : 63;
		uint64_t sign  : 1;
	} usign;
} fp64reg;

// Each of these is a single expression in every configuration, so that emitted
// code such as "if (x) load_fl(...); else set_fl(...);" compiles identically
// with and without NaN-boxing.
#ifdef RISCV_NANBOXING
#define load_fl(reg, iv) ((reg)->i32[0] = (iv), (reg)->i32[1] = ~0)
#define set_fl(reg, fv)  ((reg)->f32[0] = (fv), (reg)->i32[1] = ~0)
#else
#define load_fl(reg, iv) ((reg)->i32[0] = (iv))
#define set_fl(reg, fv)  ((reg)->f32[0] = (fv))
#endif

#define load_dbl(reg, dv) ((reg)->i64 = (dv))
#define set_dbl(reg, dv)  ((reg)->f64 = (dv))

// Thin variant of CPU for higher compilation speed
__attribute__((aligned(RISCV_MACHINE_ALIGNMENT)))
typedef struct {
	addr_t  r[32];
	addr_t  pc;
	uint32_t fcsr;
	fp64reg fr[32];
#ifdef RISCV_EXT_VECTOR
	RVV rvv;
#endif
} CPU;

#define PAGENO(x) ((addr_t)(x) >> 12)
#define PAGEOFF(x) ((addr_t)(x) & 0xFFF)
typedef void (*syscall_t) (CPU*);
typedef void (*handler) (CPU*, uint32_t);

static struct CallbackTable {
	uint8_t (*mem_ld8) (const CPU*, addr_t);
	uint16_t (*mem_ld16) (const CPU*, addr_t);
	uint32_t (*mem_ld32) (const CPU*, addr_t);
	uint64_t (*mem_ld64) (const CPU*, addr_t);
	void (*mem_st8) (const CPU*, addr_t, uint8_t);
	void (*mem_st16) (const CPU*, addr_t, uint16_t);
	void (*mem_st32) (const CPU*, addr_t, uint32_t);
	void (*mem_st64) (const CPU*, addr_t, uint64_t);
	void (*vec_load)(const CPU*, int, addr_t);
	void (*vec_store)(const CPU*, addr_t, int);
	syscall_t* syscalls;
	uint64_t (*system_call)(CPU*, addr_t, uint64_t, uint64_t, int);
	void (*unknown_syscall)(CPU*, addr_t);
	int  (*system)(CPU*, uint32_t);
	unsigned (*execute)(CPU*, uint32_t);
	unsigned (*execute_handler)(CPU*, uint32_t, uintptr_t);
	handler* handlers;
	void (*exception) (CPU*, addr_t, int);
	void (*trace) (CPU*, const char*, addr_t, uint32_t);
	float  (*sqrtf32)(float);
	double (*sqrtf64)(double);
	// Fused multiply-add (single rounding) — required by RISC-V spec
	// §11.6 for FMADD/FMSUB/FNMADD/FNMSUB. TCC compiles emitted `a*b+c`
	// as two roundings, violating the spec; emitted code routes through
	// these api entries to std::fma for correct behavior.
	float  (*fmaf32)(float, float, float);
	double (*fmaf64)(double, double, double);
	// The same for the vector FMA family, which does not canonicalize NaNs.
	float  (*vfmaf32)(float, float, float);
	double (*vfmaf64)(double, double, double);
	// FMIN/FMAX with RISC-V's -0.0 < +0.0 convention.
	float  (*fmin32_rv)(float, float);
	float  (*fmax32_rv)(float, float);
	double (*fmin64_rv)(double, double);
	double (*fmax64_rv)(double, double);
	int (*clz) (uint32_t);
	int (*clzl) (uint64_t);
	int (*ctz) (uint32_t);
	int (*ctzl) (uint64_t);
	int (*cpop) (uint32_t);
	int (*cpopl) (uint64_t);
} api;
#define ARENA_READ_BOUNDARY  (RISCV_ARENA_END - 0x1000)
#define ARENA_WRITE_BOUNDARY (RISCV_ARENA_END - RISCV_ARENA_ROEND)
#define ARENA_READABLE(x) ((x) - 0x1000 < ARENA_READ_BOUNDARY)
#define ARENA_WRITABLE(x) ((x) - RISCV_ARENA_ROEND < ARENA_WRITE_BOUNDARY)

INTERNAL static int32_t arena_offset;
//#define ARENA_AT(cpu, x)  (arena_ptr + (x))
#ifdef RISCV_ARENA_OFFSET
// The offset of the arena pointer inside the machine is fixed, so the translator
// hands it to us as a constant. That removes a load of the offset global from
// every arena access, and shortens the address dependency chain by one step.
// The offset is part of the translation hash, so a cached or embedded object
// built against a different machine layout is already rejected by hash; this
// symbol lets the loader verify it directly rather than rely on that.
VISIBLE const int32_t arena_offset_constant = RISCV_ARENA_OFFSET;
#define ARENA_AT(cpu, x)  (*(char **)((uintptr_t)cpu + RISCV_ARENA_OFFSET) + (x))
#else
#define ARENA_AT(cpu, x)  (*(char **)((uintptr_t)cpu + arena_offset) + (x))
#endif

INTERNAL static int32_t ic_offset;
#define INS_COUNTER(cpu) (*(uint64_t *)((uintptr_t)cpu + ic_offset))
#define MAX_COUNTER(cpu) (*(uint64_t *)((uintptr_t)cpu + ic_offset + 8))

typedef struct {
	addr_t pageno;
	uint8_t *data;
} CachedPage;
INTERNAL static int32_t rdcache_offset;
#define RD_CACHE(cpu) ((CachedPage *)((uintptr_t)cpu + rdcache_offset))
#define WR_CACHE(cpu) ((CachedPage *)((uintptr_t)cpu + rdcache_offset + sizeof(CachedPage)))

#ifdef __TINYC__
// Use the API directly as TCC doesn't optimize well
#define rd8  api.mem_ld8
#define rd16 api.mem_ld16
#define rd32 api.mem_ld32
#define rd64 api.mem_ld64
#define wr8  api.mem_st8
#define wr16 api.mem_st16
#define wr32 api.mem_st32
#define wr64 api.mem_st64
#else
static uint8_t rd8(CPU* cpu, addr_t addr) {
	const addr_t pageno = addr >> 12;
	if (RD_CACHE(cpu)->pageno != pageno) {
		return api.mem_ld8(cpu, addr);
	}
	return RD_CACHE(cpu)->data[PAGEOFF(addr)];
}
static uint16_t rd16(CPU* cpu, addr_t addr) {
	const addr_t pageno = addr >> 12;
	if (RD_CACHE(cpu)->pageno != pageno) {
		return api.mem_ld16(cpu, addr);
	}
	return *(uint16_t *)&RD_CACHE(cpu)->data[PAGEOFF(addr)];
}
static uint32_t rd32(CPU* cpu, addr_t addr) {
	const addr_t pageno = addr >> 12;
	if (RD_CACHE(cpu)->pageno != pageno) {
		return api.mem_ld32(cpu, addr);
	}
	return *(uint32_t *)&RD_CACHE(cpu)->data[PAGEOFF(addr)];
}
static uint64_t rd64(CPU* cpu, addr_t addr) {
	const addr_t pageno = addr >> 12;
	if (RD_CACHE(cpu)->pageno != pageno) {
		return api.mem_ld64(cpu, addr);
	}
	return *(uint64_t *)&RD_CACHE(cpu)->data[PAGEOFF(addr)];
}
static void wr8(CPU* cpu, addr_t addr, uint8_t value) {
	if (WR_CACHE(cpu)->pageno == addr >> 12) {
		WR_CACHE(cpu)->data[PAGEOFF(addr)] = value;
		return;
	}
	api.mem_st8(cpu, addr, value);
}
static void wr16(CPU* cpu, addr_t addr, uint16_t value) {
	if (WR_CACHE(cpu)->pageno == addr >> 12) {
		*(uint16_t *)&WR_CACHE(cpu)->data[PAGEOFF(addr)] = value;
		return;
	}
	api.mem_st16(cpu, addr, value);
}
static void wr32(CPU* cpu, addr_t addr, uint32_t value) {
	if (WR_CACHE(cpu)->pageno == addr >> 12) {
		*(uint32_t *)&WR_CACHE(cpu)->data[PAGEOFF(addr)] = value;
		return;
	}
	api.mem_st32(cpu, addr, value);
}
static void wr64(CPU* cpu, addr_t addr, uint64_t value) {
	if (WR_CACHE(cpu)->pageno == addr >> 12) {
		*(uint64_t *)&WR_CACHE(cpu)->data[PAGEOFF(addr)] = value;
		return;
	}
	api.mem_st64(cpu, addr, value);
}
#endif

static inline int do_syscall(CPU* cpu, uint64_t counter, uint64_t max_counter, addr_t sysno)
{
	INS_COUNTER(cpu) = counter; // Reveal instruction counters
	MAX_COUNTER(cpu) = max_counter;
	addr_t old_pc = cpu->pc;
	if (LIKELY(sysno < RISCV_MAX_SYSCALLS))
		api.syscalls[SPECSAFE(sysno)](cpu);
	else
		api.unknown_syscall(cpu, sysno);
	// Resume if the system call did not modify PC, or hit a limit
	return (cpu->pc != old_pc || counter >= MAX_COUNTER(cpu));
}

/* JALR clears bit 0 unconditionally; with C disabled a target that still has bit
   1 set is an instruction-address-misaligned trap. RISCV_ALIGN_MASK is 1 when C
   is enabled, so the test folds away to nothing there. Exits through RETURN_VALUES
   (see the note on that macro) rather than a compound literal. */
#define JUMP_TO(addr) do { \
	const addr_t aligned = (addr) & ~(addr_t)1; \
	if (UNLIKELY(aligned & (addr_t)RISCV_ALIGN_MASK)) { \
		api.exception(cpu, aligned, MISALIGNED_INSTRUCTION); \
		RETURN_VALUES(0, 0); \
	} \
	pc = aligned; \
} while (0)

// https://stackoverflow.com/questions/28868367/getting-the-high-part-of-64-bit-integer-multiplication
// As written by catid
static inline uint64_t MUL128(
	uint64_t* r_hi,
	const uint64_t x,
	const uint64_t y)
{
	const uint64_t x0 = (uint32_t)x, x1 = x >> 32;
	const uint64_t y0 = (uint32_t)y, y1 = y >> 32;
	const uint64_t p11 = x1 * y1, p01 = x0 * y1;
	const uint64_t p10 = x1 * y0, p00 = x0 * y0;

	// 64-bit product + two 32-bit values
	const uint64_t middle = p10 + (p00 >> 32) + (uint32_t)p01;

	// 64-bit product + two 32-bit values
	*r_hi = p11 + (middle >> 32) + (p01 >> 32);

	// Add LOW PART and lower half of MIDDLE PART
	return (middle << 32) | (uint32_t)p00;
}

#ifdef EMBEDDABLE_CODE
static
#else
extern VISIBLE
#endif
void init(struct CallbackTable* table, int32_t arena_off, int32_t ins_counter_off, int32_t rdcache_off)
{
	api = *table;
	arena_offset = arena_off;
	ic_offset = ins_counter_off;
	//max_ic_offset = ins_counter_off + sizeof(uint64_t);
	rdcache_offset = rdcache_off;
}

typedef struct {
	uint64_t ic;
	uint64_t max_ic;
} ReturnValues;

/* NOTE: Exit through the function-local `retvals`, never a compound literal:
   TCC gives each literal its own stack slot and never reuses it, so hundreds of
   exit points meant a frame of tens of kilobytes. On Windows that frame is taken
   by a bare sub rsp (no __chkstk), stepping over the stack guard page. */
#define RETURN_VALUES(a, b) do { retvals.ic = (a); retvals.max_ic = (b); return retvals; } while (0)
)123";
}
