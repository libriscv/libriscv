#include <string>

namespace riscv {
extern const std::string bintr_code =
R"123(#include <stdint.h>
#include <stdbool.h>
#define LIKELY(x) __builtin_expect((x), 1)
#define UNLIKELY(x) __builtin_expect((x), 0)
#define ILLEGAL_OPCODE  0

#if RISCV_TRANSLATION_DYLIB == 4
	typedef uint32_t address_t;
	typedef int32_t saddress_t;
#else
	typedef uint64_t address_t;
	typedef int64_t saddress_t;
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

static inline void load_float(fp64reg* reg, uint32_t fv) {
	reg->i32[0] = fv;
	reg->i32[1] = 0xFFFFFFFF;
}
static inline void load_double(fp64reg* reg, uint64_t dv) {
	reg->i64 = dv;
}
static inline void set_float(fp64reg* reg, float f) {
	reg->f32[0] = f;
	reg->i32[1] = 0xFFFFFFFF;
}
static inline void set_double(fp64reg* reg, double d) {
	reg->f64 = d;
}

// Thin variant of CPU for higher compilation speed
typedef struct {
	address_t  pc;
	address_t  regs[32];
	fp64reg    fpreg[32];
} ThinCPU;

static struct CallbackTable {
	uint8_t  (*mem_read8)(ThinCPU*, address_t addr);
	uint16_t (*mem_read16)(ThinCPU*, address_t addr);
	uint32_t (*mem_read32)(ThinCPU*, address_t addr);
	uint64_t (*mem_read64)(ThinCPU*, address_t addr);
	void (*mem_write8) (ThinCPU*, address_t addr, uint8_t);
	void (*mem_write16)(ThinCPU*, address_t addr, uint16_t);
	void (*mem_write32)(ThinCPU*, address_t addr, uint32_t);
	void (*mem_write64)(ThinCPU*, address_t addr, uint64_t);
	void (*finish_block)(ThinCPU*, address_t, uint64_t);
	void (*jump)(ThinCPU*, address_t, uint64_t);
	void (*increment_counter)(ThinCPU*, uint64_t);
	void (*trigger_exception)(ThinCPU*, int);
} api;

static inline uint32_t SRA32(bool is_signed, uint32_t shifts, uint32_t value)
{
	const uint32_t sign_bits = -is_signed ^ 0x0;
	const uint32_t sign_shifted = sign_bits << (32 - shifts);
	return (value >> shifts) | sign_shifted;
}
static inline uint64_t SRA64(bool is_signed, uint32_t shifts, uint64_t value)
{
	const uint64_t sign_bits = -is_signed ^ 0x0;
	const uint64_t sign_shifted = sign_bits << (64 - shifts);
	return (value >> shifts) | sign_shifted;
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

extern void init(struct CallbackTable* table) {
	api = *table;
};
)123";
}
