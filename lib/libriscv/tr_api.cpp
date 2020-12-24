#include <string>

namespace riscv {
extern const std::string bintr_code =
R"123(#include <cstdint>
#define LIKELY(x) __builtin_expect((x), 1)
#define UNLIKELY(x) __builtin_expect((x), 0)

#if RISCV_TRANSLATION_DYLIB == 4
	using address_t = uint32_t;
	using saddress_t = int32_t;
#else
	using address_t = uint64_t;
	using saddress_t = int64_t;
#endif

// Thin variant of CPU for higher compilation speed
struct ThinCPU {
	static constexpr int W = RISCV_TRANSLATION_DYLIB;
	using format_t  = union rv32i_instruction;  // one machine instruction

	void set_pc(address_t addr) { m_regs.pc = addr; }
	address_t& reg(uint32_t idx) { return m_regs.regs[idx]; }
	const address_t& reg(uint32_t idx) const { return m_regs.regs[idx]; }

private:
	using register_t = address_t;
	struct {
		address_t  pc;
		register_t regs[32];
	} m_regs;
};
struct CallbackTable {
	uint8_t  (*mem_read8)(ThinCPU&, address_t addr);
	uint16_t (*mem_read16)(ThinCPU&, address_t addr);
	uint32_t (*mem_read32)(ThinCPU&, address_t addr);
	uint64_t (*mem_read64)(ThinCPU&, address_t addr);
	void (*mem_write8) (ThinCPU&, address_t addr, uint8_t);
	void (*mem_write16)(ThinCPU&, address_t addr, uint16_t);
	void (*mem_write32)(ThinCPU&, address_t addr, uint32_t);
	void (*mem_write64)(ThinCPU&, address_t addr, uint64_t);
	void (*jump)(ThinCPU&, address_t);
	void (*increment_counter)(ThinCPU&, uint64_t);
	void (*trigger_exception)(ThinCPU&, int);
};

union rv32i_instruction
{
	// register format
	struct {
		uint32_t opcode : 7;
		uint32_t rd     : 5;
		uint32_t funct3 : 3;
		uint32_t rs1    : 5;
		uint32_t rs2    : 5;
		uint32_t funct7 : 7;

		bool is_f7() const noexcept {
			return funct7 == 0b0100000;
		}
		bool is_32M() const noexcept {
			return funct7 == 0b0000001;
		}
		uint32_t jumptable_friendly_op() const noexcept {
			// use bit 4 for RV32M extension
			return funct3 | ((funct7 & 1) << 4);
		}
	} Rtype;
	// immediate format
	struct {
		uint32_t opcode : 7;
		uint32_t rd     : 5;
		uint32_t funct3 : 3;
		uint32_t rs1    : 5;
		uint32_t imm    : 12;

		bool sign() const noexcept {
			return imm & 0x800;
		}
		int64_t signed_imm() const noexcept {
			const uint64_t ext = 0xFFFFFFFFFFFFF000;
			return imm | (sign() ? ext : 0);
		}
		uint32_t shift_imm() const noexcept {
			return imm & 0x1F;
		}
		uint32_t shift64_imm() const noexcept {
			return imm & 0x3F;
		}
		bool is_srai() const noexcept {
			return imm & 0x400;
		}
	} Itype;
	// store format
	struct {
		uint32_t opcode : 7;
		uint32_t imm1   : 5;
		uint32_t funct3 : 3;
		uint32_t rs1    : 5;
		uint32_t rs2    : 5;
		uint32_t imm2   : 7;

		bool sign() const noexcept {
			return imm2 & 0x40;
		}
		int64_t signed_imm() const noexcept {
			const uint64_t ext = 0xFFFFFFFFFFFFF000;
			return imm1 | (imm2 << 5) | (sign() ? ext : 0);
		}
	} Stype;
	// upper immediate format
	struct {
		uint32_t opcode : 7;
		uint32_t rd     : 5;
		uint32_t imm    : 20;

		bool sign() const noexcept {
			return imm & 0x80000;
		}
		int64_t signed_imm() const noexcept {
			const uint64_t ext = 0xFFFFFFFFFFF00000;
			return imm | (sign() ? ext : 0);
		}
		int32_t upper_imm() const noexcept {
			return imm << 12u;
		}
	} Utype;
	// branch type
	struct {
		uint32_t opcode : 7;
		uint32_t imm1   : 1;
		uint32_t imm2   : 4;
		uint32_t funct3 : 3;
		uint32_t rs1    : 5;
		uint32_t rs2    : 5;
		uint32_t imm3   : 6;
		uint32_t imm4   : 1;

		bool sign() const noexcept {
			return imm4;
		}
		int64_t signed_imm() const noexcept {
			const uint64_t ext = 0xFFFFFFFFFFFFF000;
			return (imm2 << 1) | (imm3 << 5) | (imm1 << 11) | (sign() ? ext : 0);
		}
	} Btype;
	// jump instructions
	struct {
		uint32_t opcode : 7;
		uint32_t rd     : 5;
		uint32_t imm1   : 8;
		uint32_t imm2   : 1;
		uint32_t imm3   : 10;
		uint32_t imm4   : 1;

		bool sign() const noexcept {
			return imm4;
		}
		int64_t jump_offset() const noexcept {
			const int64_t  jo  = (imm3 << 1) | (imm2 << 11) | (imm1 << 12);
			const uint64_t ext = 0xFFFFFFFFFFF00000;
			return jo | (sign() ? ext : 0);
		}
	} Jtype;
	// atomic format
	struct {
		uint32_t opcode : 7;
		uint32_t rd     : 5;
		uint32_t funct3 : 3;
		uint32_t rs1    : 5;
		uint32_t rs2    : 5;
		uint32_t rl     : 1;
		uint32_t aq     : 1;
		uint32_t funct5 : 5;
	} Atype;

	uint16_t half[2];
	uint32_t whole;
};

struct RV32I {
	static inline uint32_t SRA(bool is_signed, uint32_t shifts, uint32_t value)
	{
		const uint32_t sign_bits = -is_signed ^ 0x0;
		const uint32_t sign_shifted = sign_bits << (32 - shifts);
		return (value >> shifts) | sign_shifted;
	}
};
struct RV64I {
	static inline uint64_t SRA(bool is_signed, uint32_t shifts, uint64_t value)
	{
		const uint64_t sign_bits = -is_signed ^ 0x0;
		const uint64_t sign_shifted = sign_bits << (64 - shifts);
		return (value >> shifts) | sign_shifted;
	}

	// https://stackoverflow.com/questions/28868367/getting-the-high-part-of-64-bit-integer-multiplication
	// As written by catid
	static inline uint64_t MUL128(
		uint64_t& r_hi,
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
		r_hi = p11 + (middle >> 32) + (p01 >> 32);

		// Add LOW PART and lower half of MIDDLE PART
		return (middle << 32) | (uint32_t)p00;
	}
};

static CallbackTable api;
extern "C"
void init(const CallbackTable& table) {
	api = table;
};
)123";
}
