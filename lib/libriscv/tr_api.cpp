#include <string>

namespace riscv {
	extern const std::string bintr_code =
		R"123(#include <stdint.h>
#define LIKELY(x) __builtin_expect((x), 1)
#define UNLIKELY(x) __builtin_expect((x), 0)
#define ILLEGAL_OPCODE  0
#define MISALIGNED_INSTRUCTION 4

#if RISCV_TRANSLATION_DYLIB == 4
	typedef uint32_t addr_t;
	typedef int32_t saddr_t;
#  define XLEN  32
#else
	typedef uint64_t addr_t;
	typedef int64_t saddr_t;
#  define XLEN  64
#endif

#define HOST_UNKNOWN 0
#define HOST_AMD64   1

#ifdef __TINYC__
#define UNREACHABLE() /**/
static inline float fminf(float x, float y) {
	return (x < y) ? x : y;
}
static inline float fmin(double x, double y) {
	return (x < y) ? x : y;
}
static inline float fmaxf(float x, float y) {
	return (x >= y) ? x : y;
}
static inline float fmax(double x, double y) {
	return (x >= y) ? x : y;
}
#define do_bswap32(x) \
	((((x) & 0xff000000) >> 24)| \
	(((x) & 0x00ff0000) >>  8) | \
	(((x) & 0x0000ff00) <<  8) | \
	(((x) & 0x000000ff) << 24))
#define do_bswap64(x) (do_bswap32(x) | ((uint64_t)do_bswap32((x) >> 32) << 32))
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
#endif

#ifdef __HAVE_BUILTIN_SPECULATION_SAFE_VALUE
#define SPECSAFE(x) __builtin_speculation_safe_value(x)
#else
#define SPECSAFE(x) (x)
#endif

#ifdef RISCV_EXT_VECTOR
typedef union {
	float  f32[RISCV_EXT_VECTOR / 4];
	double f64[RISCV_EXT_VECTOR / 8];
} VectorLane __attribute__ ((aligned (RISCV_EXT_VECTOR)));

typedef struct {
	VectorLane  lane[32];
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

static inline void load_fl(fp64reg* reg, uint32_t fv) {
	reg->i32[0] = fv;
	reg->i32[1] = 0;
}
static inline void set_fl(fp64reg* reg, float f) {
	reg->f32[0] = f;
	reg->i32[1] = 0;
}
#define load_dbl(reg, dv) (reg)->i64 = (dv)
#define set_dbl(reg, dv)  (reg)->f64 = (dv)

// Thin variant of CPU for higher compilation speed
__attribute__((aligned(32)))
typedef struct {
	addr_t  pc;
	addr_t  r[32];
	uint32_t fcsr;
	fp64reg fr[32];
#ifdef RISCV_EXT_VECTOR
	RVV rvv;
#endif
} CPU;

#define PAGENO(x) ((addr_t)(x) >> 12)
#define PAGEOFF(x) ((addr_t)(x) & 0xFFF)
typedef void (*syscall_t) (CPU*);

static struct CallbackTable {
	const char* (*mem_ld) (const CPU*, addr_t);
	char* (*mem_st) (const CPU*, addr_t);
	void (*vec_load)(const CPU*, int, addr_t);
	void (*vec_store)(const CPU*, addr_t, int);
	syscall_t* syscalls;
	void (*unknown_syscall)(CPU*, addr_t);
	void (*system)(CPU*, uint32_t);
	void (*execute)(CPU*, uint32_t);
	void (*exception) (CPU*, int);
	float  (*sqrtf32)(float);
	double (*sqrtf64)(double);
	int (*clz) (uint32_t);
	int (*clzl) (uint64_t);
	int (*ctz) (uint32_t);
	int (*ctzl) (uint64_t);
	int (*cpop) (uint32_t);
	int (*cpopl) (uint64_t);
} api;
static char* arena_base;
static uint64_t* cur_insn;
static uint64_t* max_insn;
static const addr_t ARENA_READ_BOUNDARY  = RISCV_ARENA_END - 0x1000;
static const addr_t ARENA_WRITE_BOUNDARY = RISCV_ARENA_END - RISCV_ARENA_ROEND;
#define ARENA_READABLE(x) ((x) - 0x1000 < ARENA_READ_BOUNDARY)
#define ARENA_WRITABLE(x) ((x) - RISCV_ARENA_ROEND < ARENA_WRITE_BOUNDARY)

static inline int do_syscall(CPU* cpu, uint64_t counter, uint64_t max_counter, addr_t sysno)
{
	*cur_insn = counter; // Reveal instruction counters
	*max_insn = max_counter;
	addr_t old_pc = cpu->pc;
	if (LIKELY(sysno < RISCV_MAX_SYSCALLS))
		api.syscalls[SPECSAFE(sysno)](cpu);
	else
		api.unknown_syscall(cpu, sysno);
	// Resume if the system call did not modify PC, or hit a limit
	return (cpu->pc != old_pc || counter >= *max_insn);
}

static inline void jump(CPU* cpu, addr_t addr) {
	if (__builtin_expect((addr & 0x3) == 0, 1)) {
		cpu->pc = addr;
	} else {
		api.exception(cpu, MISALIGNED_INSTRUCTION);
		UNREACHABLE();
	}
}

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

extern void init(struct CallbackTable* table,
	char*    abase,
	uint64_t* cur_icount,
	uint64_t* max_icount)
{
	api = *table;
	arena_base = abase;
	cur_insn = cur_icount;
	max_insn = max_icount;
};

typedef struct {
	uint64_t counter;
	uint64_t max_counter;
} ReturnValues;
)123";
}
