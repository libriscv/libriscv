#include <catch2/catch_test_macros.hpp>
#include <libriscv/machine.hpp>
#include <cstdlib>
#include <unistd.h>

extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);

static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 16'000'000ul;
/* Result slots in the guest's out[] array. */
static const unsigned RESULTS = 32;

static std::vector<uint8_t> build_zcb(const std::string& code)
{
	// The guest writes every Zcb instruction as a raw `.insn`, so the
	// assembler never needs to know the extension -- only to emit the
	// halfword we hand it. That also pins the encoding under test to
	// exactly the one meant, rather than whatever the assembler picks.
	// -mno-relax: no crt0 sets up gp in a freestanding guest.
	const char* cc = getenv("RVVCC");
	setenv("RCC", cc ? cc : "riscv64-linux-gnu-gcc-14", 1);
	return build_and_load(code,
		"-std=gnu11 -O1 -static -march=rv64gc -mabi=lp64d -nostdlib "
		"-ffreestanding -mno-relax -Wl,-z,separate-code");
}

/* Zcb names only the eight compressed registers, so the operands are pinned:
 * rs1' is x9 (s1) and rd'/rs2' is x10 (a0). The second source, where there is
 * one, is x11 (a1). */
static const char *GUEST = R"M(
typedef unsigned long ulong;

/* Result words, checked by the host. */
volatile ulong out[32];
static ulong idx = 0;
static void put(ulong v) { out[idx++] = v; }

/* Quadrant 0, funct3=100: the byte and halfword memory ops. `sub` selects
 * among them; ub6 and ub5 are the two offset bits, of which the halfword
 * forms use only ub5 and spend ub6 on picking c.lh over c.lhu. */
#define ZCB_MEM(sub, ub6, ub5) \
	(0x8000u | ((sub) << 10) | (1u << 7) | ((ub6) << 6) | ((ub5) << 5) | (2u << 2))

/* Quadrant 1, funct3=100 with bit 12 set: c.mul and the unary row. */
#define ZCB_ALU(funct2, sel) \
	(0x9c01u | (2u << 7) | ((funct2) << 5) | ((sel) << 2))

static ulong mem_load(unsigned enc, const void *base)
{
	register const void *s1 asm("s1") = base;
	register ulong a0 asm("a0");
	asm volatile(".insn 2, %2" : "=r"(a0) : "r"(s1), "i"(enc));
	return a0;
}
static void mem_store(unsigned enc, void *base, ulong value)
{
	register void *s1 asm("s1") = base;
	register ulong a0 asm("a0") = value;
	asm volatile(".insn 2, %2" :: "r"(s1), "r"(a0), "i"(enc) : "memory");
}
static ulong alu(unsigned enc, ulong dst, ulong src)
{
	register ulong a0 asm("a0") = dst;
	register ulong a1 asm("a1") = src;
	asm volatile(".insn 2, %2" : "+r"(a0) : "r"(a1), "i"(enc));
	return a0;
}

static const unsigned char  bytes[8]  = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
static const unsigned short halves[2] = {0x8001, 0x7ffe};
static volatile unsigned char  st8[4];
static volatile unsigned short st16[2];

/* 1. c.lbu, whose offset spans both immediate bits */
static void t_lbu(void)
{
	put(mem_load(ZCB_MEM(0, 0, 0), bytes));                                 /* 0 */
	put(mem_load(ZCB_MEM(0, 1, 0), bytes));                                 /* 1 */
	put(mem_load(ZCB_MEM(0, 0, 1), bytes));                                 /* 2 */
	put(mem_load(ZCB_MEM(0, 1, 1), bytes));                                 /* 3 */
}

/* 2. c.lhu and c.lh, which share a code point */
static void t_lh(void)
{
	put(mem_load(ZCB_MEM(1, 0, 0), halves));                                /* 4 */
	put(mem_load(ZCB_MEM(1, 0, 1), halves));                                /* 5 */
	put(mem_load(ZCB_MEM(1, 1, 0), halves));  /* signed, so sign-extends */ /* 6 */
	put(mem_load(ZCB_MEM(1, 1, 1), halves));                                /* 7 */
}

/* 3. c.sb and c.sh must write their width and no more */
static void t_stores(void)
{
	mem_store(ZCB_MEM(2, 1, 1), (void *)st8, 0xAABBCCDDul);
	put(st8[3]);                                                            /* 8 */
	put(st8[0] | st8[1] | st8[2]);                                          /* 9 */

	mem_store(ZCB_MEM(3, 0, 1), (void *)st16, 0x1234FFEEul);
	put(st16[1]);                                                          /* 10 */
	put(st16[0]);                                                          /* 11 */
}

/* 4. c.mul is a full XLEN multiply, keeping the low half */
static void t_mul(void)
{
	put(alu(ZCB_ALU(2, 3), 0x1234567890ABCDEFul, 3));                      /* 12 */
	put(alu(ZCB_ALU(2, 3), 0ul - 7ul, 6));                                 /* 13 */
}

/* 5. the unary row, where the source field is an opcode extension */
static void t_unary(void)
{
	const ulong v = 0xFEDCBA9887654321ul;
	put(alu(ZCB_ALU(3, 0), v, 0));            /* c.zext.b */               /* 14 */
	put(alu(ZCB_ALU(3, 1), v, 0));            /* c.sext.b */               /* 15 */
	put(alu(ZCB_ALU(3, 2), v, 0));            /* c.zext.h */               /* 16 */
	put(alu(ZCB_ALU(3, 3), v, 0));            /* c.sext.h */               /* 17 */
	put(alu(ZCB_ALU(3, 4), v, 0));            /* c.zext.w */               /* 18 */
	put(alu(ZCB_ALU(3, 5), v, 0));            /* c.not    */               /* 19 */
	/* Sign extension has to widen, so feed it a negative byte and half. */
	put(alu(ZCB_ALU(3, 1), 0x11223344556677FFul, 0));                      /* 20 */
	put(alu(ZCB_ALU(3, 3), 0x1122334455FFFFFFul, 0));                      /* 21 */
}

void _start(void)
{
	t_lbu();
	t_lh();
	t_stores();
	t_mul();
	t_unary();
	asm volatile("li a7, 93; li a0, 0; ecall");
	__builtin_unreachable();
}
)M";

TEST_CASE("Zcb compressed operations", "[zcb]")
{
	const auto binary = build_zcb(GUEST);
	// The freestanding toolchain links a single RWX segment.
	riscv::Machine<riscv::RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY,
		.allow_write_exec_segment = true,
	} };
	machine.setup_linux_syscalls();
	machine.setup_linux({"zcb"}, {"LC_ALL=C"});
	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.instruction_limit_reached() == false);

	const auto out = machine.address_of("out");
	REQUIRE(out != 0x0);
	std::vector<uint64_t> r;
	for (unsigned i = 0; i < RESULTS; i++)
		r.push_back(machine.memory.read<uint64_t>(out + 8 * i));

	unsigned n = 0;
	/* t_lbu: the offset is uimm[0] at bit 6 and uimm[1] at bit 5 */
	REQUIRE(r[n++] == 0x11);
	REQUIRE(r[n++] == 0x22);
	REQUIRE(r[n++] == 0x33);
	REQUIRE(r[n++] == 0x44);

	/* t_lh */
	REQUIRE(r[n++] == 0x8001);              /* c.lhu zero-extends */
	REQUIRE(r[n++] == 0x7ffe);
	REQUIRE(r[n++] == 0xFFFFFFFFFFFF8001ul); /* c.lh sign-extends */
	REQUIRE(r[n++] == 0x7ffe);

	/* t_stores */
	REQUIRE(r[n++] == 0xDD);                /* c.sb wrote the 4th byte */
	REQUIRE(r[n++] == 0);                   /* ... and no other byte */
	REQUIRE(r[n++] == 0xFFEE);              /* c.sh wrote the 2nd halfword */
	REQUIRE(r[n++] == 0);                   /* ... and not the first */

	/* t_mul */
	REQUIRE(r[n++] == 0x1234567890ABCDEFul * 3);
	REQUIRE(r[n++] == uint64_t(-7) * 6);

	/* t_unary */
	REQUIRE(r[n++] == 0x21);
	REQUIRE(r[n++] == 0x21);                /* 0x21 is positive as a byte */
	REQUIRE(r[n++] == 0x4321);
	REQUIRE(r[n++] == 0x4321);
	REQUIRE(r[n++] == 0x87654321ul);
	REQUIRE(r[n++] == ~0xFEDCBA9887654321ul);
	REQUIRE(r[n++] == 0xFFFFFFFFFFFFFFFFul); /* c.sext.b of 0xFF */
	REQUIRE(r[n++] == 0xFFFFFFFFFFFFFFFFul); /* c.sext.h of 0xFFFF */

	REQUIRE(n <= RESULTS);
}

/* The same guest through the precise simulator must produce the same
 * results: the two paths share handlers, but decoding differs. */
TEST_CASE("Zcb fast-path parity", "[zcb]")
{
	const auto binary = build_zcb(GUEST);
	riscv::Machine<riscv::RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY,
		.allow_write_exec_segment = true,
	} };
	machine.setup_linux_syscalls();
	machine.setup_linux({"zcb"}, {"LC_ALL=C"});
	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.instruction_limit_reached() == false);

	const auto out = machine.address_of("out");
	REQUIRE(out != 0x0);

	std::vector<uint64_t> fast;
	for (unsigned i = 0; i < RESULTS; i++)
		fast.push_back(machine.memory.read<uint64_t>(out + 8 * i));

	riscv::Machine<riscv::RISCV64> precise { binary, {
		.memory_max = MAX_MEMORY,
		.allow_write_exec_segment = true,
	} };
	precise.setup_linux_syscalls();
	precise.setup_linux({"zcb"}, {"LC_ALL=C"});
	precise.set_max_instructions(MAX_INSTRUCTIONS);
	precise.cpu.simulate_precise();
	REQUIRE(precise.instruction_limit_reached() == false);

	for (unsigned i = 0; i < RESULTS; i++)
		REQUIRE(fast[i] == precise.memory.read<uint64_t>(out + 8 * i));
}
