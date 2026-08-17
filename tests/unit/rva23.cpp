#include <catch2/catch_test_macros.hpp>
#include <libriscv/machine.hpp>
#include <cstdlib>
#include <unistd.h>

extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);

static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 16'000'000ul;
static const unsigned RESULTS = 48;

/* The four RVA23U64 mandatory extensions that no plain rv64gc toolchain can
 * name: Zfhmin, Zimop, Zcmop and Zawrs. Everything below is written as a raw
 * `.insn`, so the assembler only has to emit the word we hand it -- which
 * both keeps the test buildable on the CI toolchain and pins each encoding
 * to exactly the one meant, rather than whatever the assembler picks.
 *
 * The wider RVA23 coverage lives in tests/rva23, which needs a full RVA23
 * compiler; this file is what CI can still run.
 */
static std::vector<uint8_t> build_rva23(const std::string& code)
{
	const char* cc = getenv("RVVCC");
	setenv("RCC", cc ? cc : "riscv64-linux-gnu-gcc-14", 1);
	/* -mno-relax: no crt0 sets up gp in a freestanding guest. */
	return build_and_load(code,
		"-std=gnu11 -O1 -static -march=rv64gc -mabi=lp64d -nostdlib "
		"-ffreestanding -mno-relax -Wl,-z,separate-code");
}

static const char *GUEST = R"M(
typedef unsigned long ulong;

volatile ulong out[48];
static ulong idx = 0;
static void put(ulong v) { out[idx++] = v; }

/* ---- encodings -------------------------------------------------------
 *
 * Registers are pinned: a0 and a1 for the integer operands, fa0 (f10) for
 * the float. Each asm block moves its operands in and its answer out
 * itself, because a `register ... asm("a0")` variable is only guaranteed
 * live across the asm that names it, and any call in between may reuse it.
 */
#define A0 10
#define A1 11
#define FA0 10

/* Zfhmin: FLH is the LOAD-FP opcode at funct3 = 1, FSH the STORE-FP one. */
/* Every encoding is built as an unsigned long: `.insn 4, %n` takes the
 * operand as a signed value, and anything with bit 31 set would arrive
 * negative and be rejected as too wide for a four-byte instruction. */
#define ENC(x) ((unsigned long)(x) & 0xFFFFFFFFul)
#define FLH(rd, rs1, imm)  ENC(0x07u | ((rd) << 7) | (1u << 12) | ((rs1) << 15) \
                            | ((unsigned)(imm) << 20))
#define FSH(rs2, rs1, imm) ENC(0x27u | (((imm) & 0x1F) << 7) | (1u << 12) | ((rs1) << 15) \
                            | ((rs2) << 20) | ((unsigned)((imm) >> 5) << 25))
/* OP-FP: funct7 is the five-bit operation with the two format bits below it.
 * Format 0 is single, 1 double, 2 half; rs2 names the source format for a
 * conversion, and the same numbering applies there. */
#define OPFP(op, fmt, rd, rs1, rs2, rm) \
	ENC(0x53u | ((rd) << 7) | ((rm) << 12) | ((rs1) << 15) | ((rs2) << 20) \
	 | ((unsigned)(((op) << 2) | (fmt)) << 25))
#define FMV_X_H(rd, rs1)  OPFP(0x1C, 2, rd, rs1, 0, 0)
#define FMV_H_X(rd, rs1)  OPFP(0x1E, 2, rd, rs1, 0, 0)
#define FCVT_S_H(rd, rs1) OPFP(0x08, 0, rd, rs1, 2, 0) /* widening: rm must be 0 */
#define FCVT_D_H(rd, rs1) OPFP(0x08, 1, rd, rs1, 2, 0)
#define FCVT_H_S(rd, rs1) OPFP(0x08, 2, rd, rs1, 0, 7) /* narrowing: dynamic rm */
#define FCVT_H_D(rd, rs1) OPFP(0x08, 2, rd, rs1, 1, 7)

/* Zimop. MOP.R.n puts n[4] at bit 30, n[3] at 27, n[2] at 26 and n[1:0] at
 * 21:20, with bits 24:22 fixed at 111 and bit 25 clear. MOP.RR.n clears
 * those constraints by setting bit 25, and spells n across bits 30, 27
 * and 26. Both live at funct3 = 100, which the base ISA reserves. */
#define MOP_R(n, rd, rs1) \
	ENC(0x73u | ((rd) << 7) | (4u << 12) | ((rs1) << 15) \
	 | (0x1Cu << 20) | ((unsigned)(((n) >> 2) & 1) << 26) \
	 | ((unsigned)(((n) >> 3) & 1) << 27) \
	 | ((unsigned)(((n) >> 4) & 1) << 30) | (1u << 31) \
	 | ((unsigned)((n) & 3) << 20))
#define MOP_RR(n, rd, rs1, rs2) \
	ENC(0x73u | ((rd) << 7) | (4u << 12) | ((rs1) << 15) | ((rs2) << 20) \
	 | (1u << 25) | ((unsigned)((n) & 1) << 26) \
	 | ((unsigned)(((n) >> 1) & 1) << 27) \
	 | ((unsigned)(((n) >> 2) & 1) << 30) | (1u << 31))

/* Zcmop: C.MOP.n sits in the reserved C.LUI points, imm = 0 with rd = 0nnn1. */
#define C_MOP(n) ENC(0x6001u | ((n) << 7))

/* Zawrs. */
#define WRS_NTO ENC(0x00d00073u)
#define WRS_STO ENC(0x01d00073u)

static const unsigned short halves[4] = { 0x3C00, 0xBC00, 0x0001, 0x7BFF };
static volatile unsigned short st16[2];

/* Load a half and hand back the whole of what FMV.X.H produced -- which is
 * sixteen bits sign-extended across the destination, the same way FMV.X.W
 * extends a single on RV64. */
static ulong half_bits_extended(const void *p, unsigned off)
{
	ulong r;
	if (off == 0) {
		__asm__ volatile("mv a1, %1\n.insn 4, %2\n.insn 4, %3\nmv %0, a0"
			: "=r"(r) : "r"(p), "i"(FLH(FA0, A1, 0)), "i"(FMV_X_H(A0, FA0))
			: "a0", "a1", "fa0", "memory");
	} else {
		__asm__ volatile("mv a1, %1\n.insn 4, %2\n.insn 4, %3\nmv %0, a0"
			: "=r"(r) : "r"(p), "i"(FLH(FA0, A1, 2)), "i"(FMV_X_H(A0, FA0))
			: "a0", "a1", "fa0", "memory");
	}
	return r;
}

static ulong half_bits(const void *p) { return half_bits_extended(p, 0) & 0xFFFF; }

/* half -> float, as the raw f32 bits. Everything happens inside one asm:
 * fa0 is in the clobber list, so its value would not survive to a second. */
static ulong h2s(ulong hbits)
{
	ulong r;
	__asm__ volatile("mv a0, %1\n.insn 4, %2\n.insn 4, %3\n"
		"fmv.x.w a0, fa0\nmv %0, a0"
		: "=r"(r) : "r"(hbits), "i"(FMV_H_X(FA0, A0)), "i"(FCVT_S_H(FA0, FA0))
		: "a0", "fa0");
	return (unsigned)r;
}

/* half -> double, as the raw f64 bits. */
static ulong h2d(ulong hbits)
{
	ulong r;
	__asm__ volatile("mv a0, %1\n.insn 4, %2\n.insn 4, %3\n"
		"fmv.x.d a0, fa0\nmv %0, a0"
		: "=r"(r) : "r"(hbits), "i"(FMV_H_X(FA0, A0)), "i"(FCVT_D_H(FA0, FA0))
		: "a0", "fa0");
	return r;
}

/* float bits -> half bits. */
static ulong s2h(ulong sbits)
{
	ulong r;
	__asm__ volatile("mv a0, %1\nfmv.w.x fa0, a0\n.insn 4, %2\n.insn 4, %3\n"
		"mv %0, a0"
		: "=r"(r) : "r"(sbits), "i"(FCVT_H_S(FA0, FA0)), "i"(FMV_X_H(A0, FA0))
		: "a0", "fa0");
	return r & 0xFFFF;
}

/* double bits -> half bits. */
static ulong d2h(ulong dbits)
{
	ulong r;
	__asm__ volatile("mv a0, %1\nfmv.d.x fa0, a0\n.insn 4, %2\n.insn 4, %3\n"
		"mv %0, a0"
		: "=r"(r) : "r"(dbits), "i"(FCVT_H_D(FA0, FA0)), "i"(FMV_X_H(A0, FA0))
		: "a0", "fa0");
	return r & 0xFFFF;
}

/* 1. FLH reads sixteen bits from memory */
static void t_flh(void)
{
	put(half_bits(&halves[0]));                                             /*  0 */
	put(half_bits(&halves[1]));                                             /*  1 */
	put(half_bits(&halves[3]));                                             /*  2 */
	/* The offset field is a real twelve-bit displacement, and FMV.X.H
	 * sign-extends what it reads. */
	put(half_bits_extended(&halves[0], 2) & 0xFFFF);                       /*  3 */
	put(half_bits_extended(&halves[1], 0));                                /*  4 */
}

/* 2. FSH writes sixteen bits and no more */
static void t_fsh(void)
{
	ulong v = 0xBEEF;
	st16[0] = 0x1111; st16[1] = 0x2222;
	__asm__ volatile("mv a0, %0\nmv a1, %1\n.insn 4, %2\n.insn 4, %3"
		: : "r"(v), "r"(st16), "i"(FMV_H_X(FA0, A0)), "i"(FSH(FA0, A1, 2))
		: "a0", "a1", "fa0", "memory");
	put(st16[1]);                                                           /*  5 */
	put(st16[0]);                                                           /*  6 */
}

/* 3. the widening conversions, which are exact and take no rounding mode */
static void t_widen(void)
{
	put(h2s(0x3C00));   /* 1.0                    */                        /*  7 */
	put(h2s(0xBC00));   /* -1.0                   */                        /*  8 */
	put(h2s(0x0001));   /* smallest subnormal     */                        /*  9 */
	put(h2s(0x03FF));   /* largest subnormal      */                        /* 10 */
	put(h2s(0x0400));   /* smallest normal        */                        /* 11 */
	put(h2s(0x7BFF));   /* largest finite, 65504  */                       /* 12 */
	put(h2s(0x7C00));   /* infinity               */                       /* 13 */
	put(h2s(0x7E00));   /* quiet NaN              */                       /* 14 */
	put(h2d(0x3C00));                                                      /* 15 */
	put(h2d(0x0001));                                                      /* 16 */
	put(h2d(0x7BFF));                                                      /* 17 */
}

/* 4. the narrowing conversions, which round */
static void t_narrow(void)
{
	put(s2h(0x3F800000ul)); /* 1.0                          */             /* 18 */
	put(s2h(0x477FE000ul)); /* 65504, the largest finite    */             /* 19 */
	put(s2h(0x477FF000ul)); /* 65520, the tie -> infinity   */             /* 20 */
	put(s2h(0x47800000ul)); /* 65536 -> infinity            */             /* 21 */
	put(s2h(0x33000000ul)); /* 2^-25, a tie -> even -> zero */             /* 22 */
	put(s2h(0x33800000ul)); /* 2^-24, smallest subnormal    */             /* 23 */
	put(s2h(0x387FC000ul)); /* largest subnormal            */             /* 24 */

	/* Narrowing a double must round once, not twice. Each of these sits
	 * exactly where rounding through a float first would land one ulp
	 * away from the correctly-rounded answer. */
	put(d2h(0x3fd6aa000b49d2d0ul)); /* 0.3541259870756166   */             /* 25 */
	put(d2h(0x404d45ffff2cd659ul)); /* 58.5468749016697     */             /* 26 */
	put(d2h(0xbfe43a00059840f7ul)); /* -0.6320800885460177  */             /* 27 */
	put(d2h(0x40effe0000000000ul)); /* 65520 -> infinity    */             /* 28 */
	put(d2h(0x40effde000000000ul)); /* 65519 -> 65504       */             /* 29 */
	put(d2h(0x3e78000000000000ul)); /* 3*2^-25 -> 2^-23     */             /* 30 */
}

/* 5. Zimop: every may-be-operation writes zero to rd and nothing else */
static void t_zimop(void)
{
	ulong acc = 0, keep;

#define TRY_MOP_R(n) do { ulong r_; \
	__asm__ volatile("li a0, -1\nli a1, 0x5A5A\n.insn 4, %1\nmv %0, a0" \
		: "=r"(r_) : "i"(MOP_R(n, A0, A1)) : "a0", "a1"); \
	acc |= r_; } while (0)
	TRY_MOP_R(0);  TRY_MOP_R(1);  TRY_MOP_R(2);  TRY_MOP_R(3);
	TRY_MOP_R(4);  TRY_MOP_R(5);  TRY_MOP_R(6);  TRY_MOP_R(7);
	TRY_MOP_R(8);  TRY_MOP_R(9);  TRY_MOP_R(10); TRY_MOP_R(11);
	TRY_MOP_R(12); TRY_MOP_R(13); TRY_MOP_R(14); TRY_MOP_R(15);
	TRY_MOP_R(16); TRY_MOP_R(17); TRY_MOP_R(18); TRY_MOP_R(19);
	TRY_MOP_R(20); TRY_MOP_R(21); TRY_MOP_R(22); TRY_MOP_R(23);
	TRY_MOP_R(24); TRY_MOP_R(25); TRY_MOP_R(26); TRY_MOP_R(27);
	TRY_MOP_R(28); TRY_MOP_R(29); TRY_MOP_R(30); TRY_MOP_R(31);
	put(acc);                                                              /* 31 */

	acc = 0;
#define TRY_MOP_RR(n) do { ulong r_; \
	__asm__ volatile("li a0, -1\nli a1, 0x5A5A\n.insn 4, %1\nmv %0, a0" \
		: "=r"(r_) : "i"(MOP_RR(n, A0, A1, A1)) : "a0", "a1"); \
	acc |= r_; } while (0)
	TRY_MOP_RR(0); TRY_MOP_RR(1); TRY_MOP_RR(2); TRY_MOP_RR(3);
	TRY_MOP_RR(4); TRY_MOP_RR(5); TRY_MOP_RR(6); TRY_MOP_RR(7);
	put(acc);                                                              /* 32 */

	/* rd = x0 must leave x0 alone and the source register untouched. */
	__asm__ volatile("li a1, 0x1234\n.insn 4, %1\nmv %0, a1"
		: "=r"(keep) : "i"(MOP_R(7, 0, A1)) : "a1");
	put(keep);                                                             /* 33 */
}

/* 6. Zcmop: the compressed may-be-operations write no register at all */
static void t_zcmop(void)
{
	ulong keep;
	__asm__ volatile("li a0, 0x99\n"
		".insn 2, %1\n.insn 2, %2\n.insn 2, %3\n.insn 2, %4\n"
		".insn 2, %5\n.insn 2, %6\n.insn 2, %7\n.insn 2, %8\n"
		"mv %0, a0"
		: "=r"(keep)
		: "i"(C_MOP(1)),  "i"(C_MOP(3)),  "i"(C_MOP(5)),  "i"(C_MOP(7)),
		  "i"(C_MOP(9)),  "i"(C_MOP(11)), "i"(C_MOP(13)), "i"(C_MOP(15))
		: "a0");
	put(keep);                                                             /* 34 */
}

/* 7. Zawrs: both forms must retire, and may do so immediately */
static void t_zawrs(void)
{
	static volatile unsigned word = 7;
	ulong seen;
	__asm__ volatile("lr.w %0, (%1)\n.insn 4, %2\n.insn 4, %3"
		: "=&r"(seen) : "r"(&word), "i"(WRS_NTO), "i"(WRS_STO) : "memory");
	put(seen);                                                             /* 35 */
}

void _start(void)
{
	t_flh();
	t_fsh();
	t_widen();
	t_narrow();
	t_zimop();
	t_zcmop();
	t_zawrs();
	__asm__ volatile("li a7, 93; li a0, 0; ecall");
	__builtin_unreachable();
}
)M";

static std::vector<uint64_t> run_guest(const std::vector<uint8_t>& binary,
	bool precise)
{
	riscv::Machine<riscv::RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY,
		.allow_write_exec_segment = true,
	} };
	machine.setup_linux_syscalls();
	machine.setup_linux({"rva23"}, {"LC_ALL=C"});
	if (precise) {
		machine.set_max_instructions(MAX_INSTRUCTIONS);
		machine.cpu.simulate_precise();
	} else {
		machine.simulate(MAX_INSTRUCTIONS);
	}
	REQUIRE(machine.instruction_limit_reached() == false);

	const auto out = machine.address_of("out");
	REQUIRE(out != 0x0);
	std::vector<uint64_t> r;
	for (unsigned i = 0; i < RESULTS; i++)
		r.push_back(machine.memory.read<uint64_t>(out + 8 * i));
	return r;
}

TEST_CASE("Zfhmin half-precision moves and conversions", "[rva23]")
{
	const auto r = run_guest(build_rva23(GUEST), false);
	unsigned n = 0;

	/* t_flh */
	REQUIRE(r[n++] == 0x3C00);
	REQUIRE(r[n++] == 0xBC00);
	REQUIRE(r[n++] == 0x7BFF);
	REQUIRE(r[n++] == 0xBC00);              /* offset 2 reached halves[1] */
	REQUIRE(r[n++] == 0xFFFFFFFFFFFFBC00ull); /* FMV.X.H sign-extends */

	/* t_fsh: sixteen bits written, the neighbour untouched */
	REQUIRE(r[n++] == 0xBEEF);
	REQUIRE(r[n++] == 0x1111);

	/* t_widen: half -> single. Exact in every case, including both
	 * subnormal ends, and a NaN stays a NaN. */
	REQUIRE(r[n++] == 0x3F800000);
	REQUIRE(r[n++] == 0xBF800000);
	REQUIRE(r[n++] == 0x33800000);
	REQUIRE(r[n++] == 0x387FC000);
	REQUIRE(r[n++] == 0x38800000);
	REQUIRE(r[n++] == 0x477FE000);
	REQUIRE(r[n++] == 0x7F800000);
	REQUIRE(r[n++] == 0x7FC00000);
	/* half -> double */
	REQUIRE(r[n++] == 0x3FF0000000000000ull);
	REQUIRE(r[n++] == 0x3E70000000000000ull);
	REQUIRE(r[n++] == 0x40EFFC0000000000ull);

	/* t_narrow: single -> half */
	REQUIRE(r[n++] == 0x3C00);
	REQUIRE(r[n++] == 0x7BFF);
	REQUIRE(r[n++] == 0x7C00);
	REQUIRE(r[n++] == 0x7C00);
	REQUIRE(r[n++] == 0x0000);
	REQUIRE(r[n++] == 0x0001);
	REQUIRE(r[n++] == 0x03FF);
	/* double -> half, rounded once */
	REQUIRE(r[n++] == 0x35AB); /* 0x35AA if it went through a float */
	REQUIRE(r[n++] == 0x5351); /* 0x5352 if it went through a float */
	REQUIRE(r[n++] == 0xB90F); /* 0xB90E if it went through a float */
	REQUIRE(r[n++] == 0x7C00);
	REQUIRE(r[n++] == 0x7BFF);
	REQUIRE(r[n++] == 0x0002);

	REQUIRE(n <= RESULTS);
}

TEST_CASE("Zimop, Zcmop and Zawrs", "[rva23]")
{
	const auto r = run_guest(build_rva23(GUEST), false);

	/* All forty may-be-operations wrote zero over the all-ones seed. */
	REQUIRE(r[31] == 0);
	REQUIRE(r[32] == 0);
	/* rd = x0 wrote nothing, and rs1 came through untouched. */
	REQUIRE(r[33] == 0x1234);
	/* The compressed forms write no register at all. */
	REQUIRE(r[34] == 0x99);
	/* WRS retired, and the load-reserved before it read the right word. */
	REQUIRE(r[35] == 7);
}

/* The two simulators share instruction handlers but not decoding, so the
 * same guest has to come out the same way through both. */
TEST_CASE("RVA23 fast-path parity", "[rva23]")
{
	const auto binary = build_rva23(GUEST);
	const auto fast = run_guest(binary, false);
	const auto precise = run_guest(binary, true);
	for (unsigned i = 0; i < RESULTS; i++)
		REQUIRE(fast[i] == precise[i]);
}
