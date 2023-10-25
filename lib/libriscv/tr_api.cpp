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
} CPU;

#define PUREFUNC __attribute__((pure))
#define PAGENO(x) ((addr_t)(x) >> 12)
#define PAGEOFF(x) ((addr_t)(x) & 0xFFF)

typedef PUREFUNC const char* (*mem_ld_t) (const CPU*, addr_t);
typedef PUREFUNC char* (*mem_st_t) (const CPU*, addr_t);

static struct CallbackTable {
	mem_ld_t mem_ld;
	mem_st_t mem_st;
	int  (*syscall)(CPU*, addr_t);
	void (*ebreak)(CPU*);
	void (*system)(CPU*, uint32_t);
	void (*execute)(CPU*, uint32_t);
	void (*exception) (CPU*, int);
	float  (*sqrtf32)(float);
	double (*sqrtf64)(double);
} api;
static char* arena_base;
static uint64_t* cur_insn;
static uint64_t* max_insn;

static inline void jump(CPU* cpu, addr_t addr) {
	if (__builtin_expect((addr & 0x3) == 0, 1)) {
		cpu->pc = addr;
	} else {
		api.exception(cpu, MISALIGNED_INSTRUCTION);
		__builtin_unreachable();
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

extern void init(struct CallbackTable* table, char* abase, uint64_t* cur_icount, uint64_t* max_icount)
{
	api = *table;
	arena_base = abase;
	cur_insn = cur_icount;
	max_insn = max_icount;
};
)123";
}
