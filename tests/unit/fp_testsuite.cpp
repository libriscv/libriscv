#include <catch2/catch_test_macros.hpp>
#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
static const std::string cwd {SRCDIR};
using namespace riscv;

TEST_CASE("Verify floating point instructions", "[Verification]")
{
	const auto fpfuncfile = cwd + "/fptest/floating-point.cpp";
	const auto binary = build_and_load(R"M(
	#include "fptest/fptest.cpp"
)M", "-O2 -static -I" + cwd + " " + fpfuncfile, true);

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"compute_pi"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 0);
}

TEST_CASE("Trunc and floor", "[Verification]")
{
	const auto binary = build_and_load(R"M(
	#include <math.h>

	double test_trunc64(double val) {
		return __builtin_trunc(val);
	}
	float test_trunc32(float val) {
		return __builtin_truncf(val);
	}
	double test_floor64(double val) {
		return __builtin_floor(val);
	}
	float test_floor32(float val) {
		return __builtin_floorf(val);
	}

	int main() {
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"compute_pi"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value() == 0);

	// 64-bit truncation
	machine.vmcall("test_trunc64", 3.14);
	REQUIRE(machine.return_value<double>() == 3.0);
	machine.vmcall("test_trunc64", -3.14);
	REQUIRE(machine.return_value<double>() == -3.0);
	machine.vmcall("test_trunc64", 3.74);
	REQUIRE(machine.return_value<double>() == 3.0);
	machine.vmcall("test_trunc64", -3.74);
	REQUIRE(machine.return_value<double>() == -3.0);
	// 32-bit truncation
	machine.vmcall("test_trunc32", 3.14f);
	REQUIRE(machine.return_value<float>() == 3.0f);
	machine.vmcall("test_trunc32", -3.14f);
	REQUIRE(machine.return_value<float>() == -3.0f);
	machine.vmcall("test_trunc32", 3.74f);
	REQUIRE(machine.return_value<float>() == 3.0f);
	machine.vmcall("test_trunc32", -3.74f);
	REQUIRE(machine.return_value<float>() == -3.0f);

	// 64-bit floor
	machine.vmcall("test_floor64", 3.14);
	REQUIRE(machine.return_value<double>() == 3.0);
	machine.vmcall("test_floor64", -3.14);
	REQUIRE(machine.return_value<double>() == -4.0);
	machine.vmcall("test_floor64", 3.74);
	REQUIRE(machine.return_value<double>() == 3.0);
	machine.vmcall("test_floor64", -3.74);
	REQUIRE(machine.return_value<double>() == -4.0);
	// 32-bit floor
	machine.vmcall("test_floor32", 3.14f);
	REQUIRE(machine.return_value<float>() == 3.0f);
	machine.vmcall("test_floor32", -3.14f);
	REQUIRE(machine.return_value<float>() == -4.0f);
	machine.vmcall("test_floor32", 3.74f);
	REQUIRE(machine.return_value<float>() == 3.0f);
	machine.vmcall("test_floor32", -3.74f);
	REQUIRE(machine.return_value<float>() == -4.0f);
}

TEST_CASE("FCVT float-to-integer rounding modes", "[Verification]")
{
	// Directly emit FCVT with each static rounding mode (rtz/rdn/rup/rmm/rne)
	// via inline assembly. Regression test for the interpreter/JIT only
	// implementing RTZ + RDN (a bare C++ cast is RTZ), which produced wrong
	// results for round-up (ceil), round-nearest-ties-away (std::lround), and
	// round-nearest-ties-even. Also covers the previously-missing 64-bit
	// conversions from float32 (FCVT.L.S / FCVT.LU.S) and unsigned 32-bit
	// sign-extension (FCVT.WU.*).
	const auto binary = build_and_load(R"M(
	#define CVT(name, insn, ctype, ftype, rm) \
		ctype name(ftype x) { ctype r; \
			__asm__(insn " %0, %1, " rm : "=r"(r) : "f"(x)); return r; }

	// float32 -> int32 (FCVT.W.S)
	CVT(w_s_rtz, "fcvt.w.s", int, float, "rtz")
	CVT(w_s_rdn, "fcvt.w.s", int, float, "rdn")
	CVT(w_s_rup, "fcvt.w.s", int, float, "rup")
	CVT(w_s_rmm, "fcvt.w.s", int, float, "rmm")
	CVT(w_s_rne, "fcvt.w.s", int, float, "rne")

	// double -> int64 (FCVT.L.D)
	CVT(l_d_rtz, "fcvt.l.d", long, double, "rtz")
	CVT(l_d_rdn, "fcvt.l.d", long, double, "rdn")
	CVT(l_d_rup, "fcvt.l.d", long, double, "rup")
	CVT(l_d_rmm, "fcvt.l.d", long, double, "rmm")
	CVT(l_d_rne, "fcvt.l.d", long, double, "rne")

	// float32 -> int64 (FCVT.L.S) -- previously unimplemented
	CVT(l_s_rup, "fcvt.l.s", long, float, "rup")
	// float32 -> uint64 (FCVT.LU.S) -- previously unimplemented
	CVT(lu_s_rtz, "fcvt.lu.s", unsigned long, float, "rtz")
	// float32 -> uint32 (FCVT.WU.S), result is sign-extended to XLEN
	CVT(wu_s_rtz, "fcvt.wu.s", unsigned, float, "rtz")
	// double -> uint32 (FCVT.WU.D)
	CVT(wu_d_rtz, "fcvt.wu.d", unsigned, double, "rtz")

	// DYN: rounding mode comes from the fcsr CSR (frm field). Set it with
	// csrwi, then convert with the dynamic rounding mode.
	long l_d_dyn_rdn(double x) { long r;
		__asm__ volatile("csrwi frm, 2\n\tfcvt.l.d %0, %1, dyn" : "=r"(r) : "f"(x));
		return r; }
	long l_d_dyn_rup(double x) { long r;
		__asm__ volatile("csrwi frm, 3\n\tfcvt.l.d %0, %1, dyn" : "=r"(r) : "f"(x));
		return r; }
	long l_d_dyn_rtz(double x) { long r;
		__asm__ volatile("csrwi frm, 1\n\tfcvt.l.d %0, %1, dyn" : "=r"(r) : "f"(x));
		return r; }

	int main() { return 0; }
	)M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls();
	machine.setup_linux({"fcvt"}, {"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value() == 0);

	// Half-way values distinguish every rounding mode.
	//               2.5      3.5      -2.5
	// RTZ (trunc):    2        3        -2
	// RDN (floor):    2        3        -3
	// RUP (ceil):     3        4        -2
	// RMM (ties away):3        4        -3
	// RNE (ties even):2        4        -2
	struct { float in; int rtz, rdn, rup, rmm, rne; } cases[] = {
		{ 2.5f,  2, 2, 3, 3, 2 },
		{ 3.5f,  3, 3, 4, 4, 4 },
		{ -2.5f, -2, -3, -2, -3, -2 },
		{ 2.7f,  2, 2, 3, 3, 3 },
	};
	for (const auto& c : cases) {
		machine.vmcall("w_s_rtz", c.in);
		REQUIRE(machine.return_value<int32_t>() == c.rtz);
		machine.vmcall("w_s_rdn", c.in);
		REQUIRE(machine.return_value<int32_t>() == c.rdn);
		machine.vmcall("w_s_rup", c.in);
		REQUIRE(machine.return_value<int32_t>() == c.rup);
		machine.vmcall("w_s_rmm", c.in);
		REQUIRE(machine.return_value<int32_t>() == c.rmm);
		machine.vmcall("w_s_rne", c.in);
		REQUIRE(machine.return_value<int32_t>() == c.rne);

		// Same modes, double -> int64 path.
		machine.vmcall("l_d_rtz", (double)c.in);
		REQUIRE(machine.return_value<int64_t>() == c.rtz);
		machine.vmcall("l_d_rdn", (double)c.in);
		REQUIRE(machine.return_value<int64_t>() == c.rdn);
		machine.vmcall("l_d_rup", (double)c.in);
		REQUIRE(machine.return_value<int64_t>() == c.rup);
		machine.vmcall("l_d_rmm", (double)c.in);
		REQUIRE(machine.return_value<int64_t>() == c.rmm);
		machine.vmcall("l_d_rne", (double)c.in);
		REQUIRE(machine.return_value<int64_t>() == c.rne);
	}

	// float32 -> int64 (FCVT.L.S), rounding up: 2.7 -> 3.
	machine.vmcall("l_s_rup", 2.7f);
	REQUIRE(machine.return_value<int64_t>() == 3);

	// float32 -> uint64 (FCVT.LU.S): truncation toward zero.
	machine.vmcall("lu_s_rtz", 5.9f);
	REQUIRE(machine.return_value<uint64_t>() == 5);

	// FCVT.WU.S: a value above INT32_MAX yields a uint32 sign-extended to XLEN.
	// 3e9 -> 3000000000 (0xB2D05E00), the low 32 bits must be preserved.
	machine.vmcall("wu_s_rtz", 3000000000.0f);
	REQUIRE(machine.return_value<uint32_t>() == 3000000000u);
	machine.vmcall("wu_d_rtz", 3000000000.0);
	REQUIRE(machine.return_value<uint32_t>() == 3000000000u);

	// DYN mode must read the rounding mode from the fcsr CSR.
	machine.vmcall("l_d_dyn_rdn", 2.7);
	REQUIRE(machine.return_value<int64_t>() == 2);   // floor
	machine.vmcall("l_d_dyn_rup", 2.3);
	REQUIRE(machine.return_value<int64_t>() == 3);   // ceil
	machine.vmcall("l_d_dyn_rtz", -2.7);
	REQUIRE(machine.return_value<int64_t>() == -2);  // trunc
}

TEST_CASE("Compute PI slowly", "[Verification]")
{
	const auto binary = build_and_load(R"M(
	#include <assert.h>
	#include <float.h>

	inline int kinda64(float val, double expectation) {
		return val >= expectation-FLT_EPSILON
			&& val < expectation+FLT_EPSILON;
	}

	static struct {
		double sum;
		int counter;
		int sign;
	} pi;

	static double compute_more_pi()
	{
	    pi.sum += pi.sign / (2.0 * pi.counter + 1.0);
		pi.counter ++;
		pi.sign = -pi.sign;
	    return 4.0 * pi.sum;
	}
	int main() {
		pi.sign = 1;
		assert(kinda64(compute_more_pi(), 4.0));
		assert(kinda64(compute_more_pi(), 2.66666666666));
		assert(kinda64(compute_more_pi(), 3.46666666666));
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"compute_pi"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 0);
}

TEST_CASE("FCVT clamps out-of-range conversions", "[Verification]")
{
	// Converting an out-of-range float to an integer is undefined behavior in
	// C++, so both the interpreter and the code emitter have to range-check
	// before the cast. RISC-V pins the result to the destination's extreme:
	// NaN and positive overflow give the maximum, negative overflow the
	// minimum, and an unsigned destination gives 0 for anything negative.
	const auto binary = build_and_load(R"M(
	#define CVT(name, insn, ctype, ftype) \
		ctype name(ftype x) { ctype r; \
			__asm__ volatile(insn " %0, %1, rtz" : "=r"(r) : "f"(x)); return r; }

	CVT(w_s,  "fcvt.w.s",  int,           float)
	CVT(wu_s, "fcvt.wu.s", unsigned,      float)
	CVT(l_s,  "fcvt.l.s",  long,          float)
	CVT(lu_s, "fcvt.lu.s", unsigned long, float)
	CVT(w_d,  "fcvt.w.d",  int,           double)
	CVT(wu_d, "fcvt.wu.d", unsigned,      double)
	CVT(l_d,  "fcvt.l.d",  long,          double)
	CVT(lu_d, "fcvt.lu.d", unsigned long, double)

	int main() { return 0; }
	)M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls();
	machine.setup_linux({"fcvt"}, {"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value() == 0);

	const float  fnan = __builtin_nanf("");
	const double dnan = __builtin_nan("");

	// float32 sources
	for (float x : { 1e30f, __builtin_inff(), fnan }) {
		machine.vmcall("w_s", x);
		REQUIRE(machine.return_value<int32_t>() == INT32_MAX);
		machine.vmcall("l_s", x);
		REQUIRE(machine.return_value<int64_t>() == INT64_MAX);
		machine.vmcall("wu_s", x);
		REQUIRE(machine.return_value<uint32_t>() == UINT32_MAX);
		machine.vmcall("lu_s", x);
		REQUIRE(machine.return_value<uint64_t>() == UINT64_MAX);
	}
	for (float x : { -1e30f, -__builtin_inff() }) {
		machine.vmcall("w_s", x);
		REQUIRE(machine.return_value<int32_t>() == INT32_MIN);
		machine.vmcall("l_s", x);
		REQUIRE(machine.return_value<int64_t>() == INT64_MIN);
		machine.vmcall("wu_s", x);
		REQUIRE(machine.return_value<uint32_t>() == 0u);
		machine.vmcall("lu_s", x);
		REQUIRE(machine.return_value<uint64_t>() == 0u);
	}
	// float64 sources
	for (double x : { 1e300, __builtin_inf(), dnan }) {
		machine.vmcall("w_d", x);
		REQUIRE(machine.return_value<int32_t>() == INT32_MAX);
		machine.vmcall("l_d", x);
		REQUIRE(machine.return_value<int64_t>() == INT64_MAX);
		machine.vmcall("wu_d", x);
		REQUIRE(machine.return_value<uint32_t>() == UINT32_MAX);
		machine.vmcall("lu_d", x);
		REQUIRE(machine.return_value<uint64_t>() == UINT64_MAX);
	}
	for (double x : { -1e300, -__builtin_inf() }) {
		machine.vmcall("w_d", x);
		REQUIRE(machine.return_value<int32_t>() == INT32_MIN);
		machine.vmcall("l_d", x);
		REQUIRE(machine.return_value<int64_t>() == INT64_MIN);
		machine.vmcall("wu_d", x);
		REQUIRE(machine.return_value<uint32_t>() == 0u);
		machine.vmcall("lu_d", x);
		REQUIRE(machine.return_value<uint64_t>() == 0u);
	}
	// In-range values still convert normally.
	machine.vmcall("w_s", -2.7f);
	REQUIRE(machine.return_value<int32_t>() == -2);
	machine.vmcall("lu_d", 2.7);
	REQUIRE(machine.return_value<uint64_t>() == 2u);
}

TEST_CASE("FCSR exception flags and canonical NaNs", "[Verification]")
{
	// Everything here is behavior we only promise when built with RISCV_FCSR:
	// a scripting guest that never reads fflags should not pay for the checks,
	// so without FCSR emulation NaN payloads and flags are left to the host.
	if constexpr (!riscv::fcsr_emulation) {
		SUCCEED("FCSR emulation is disabled in this build");
		return;
	}

	static constexpr uint32_t QNAN = 0x7fc00000u; // canonical quiet NaN
	static constexpr uint32_t SNAN = 0x7f800001u; // signaling NaN
	static constexpr uint32_t ONE  = 0x3f800000u; // 1.0f
	static constexpr uint32_t MONE = 0xbf800000u; // -1.0f
	static constexpr uint32_t NV = 16, DZ = 8, NX = 1;

	// Operands are passed as raw bits so the values survive the host->guest
	// argument path exactly. Every sequence clears fflags first, since the
	// flags are accrued.
	const auto binary = build_and_load(R"M(
	#define BITS_TO_F(var, bits) float var; __builtin_memcpy(&var, &bits, 4)
	#define F_TO_BITS(bits, var) unsigned bits; __builtin_memcpy(&bits, &var, 4)

	#define BINOP(name, insn) \
		unsigned name##_bits(unsigned ab, unsigned bb) { \
			BITS_TO_F(a, ab); BITS_TO_F(b, bb); float r; \
			__asm__ volatile(insn " %0, %1, %2" : "=f"(r) : "f"(a), "f"(b)); \
			F_TO_BITS(rb, r); return rb; } \
		unsigned name##_flags(unsigned ab, unsigned bb) { \
			BITS_TO_F(a, ab); BITS_TO_F(b, bb); float r; unsigned f; \
			__asm__ volatile("csrwi fflags, 0\n\t" insn " %1, %2, %3\n\tcsrr %0, fflags" \
				: "=r"(f), "=f"(r) : "f"(a), "f"(b)); \
			return f; }

	BINOP(fadd, "fadd.s")
	BINOP(fmul, "fmul.s")
	BINOP(fdiv, "fdiv.s")
	BINOP(fmin, "fmin.s")
	BINOP(fmax, "fmax.s")

	unsigned fsqrt_bits(unsigned ab) {
		BITS_TO_F(a, ab); float r;
		__asm__ volatile("fsqrt.s %0, %1" : "=f"(r) : "f"(a));
		F_TO_BITS(rb, r); return rb; }
	unsigned fsqrt_flags(unsigned ab) {
		BITS_TO_F(a, ab); float r; unsigned f;
		__asm__ volatile("csrwi fflags, 0\n\tfsqrt.s %1, %2\n\tcsrr %0, fflags"
			: "=r"(f), "=f"(r) : "f"(a));
		return f; }
	unsigned flt_flags(unsigned ab, unsigned bb) {
		BITS_TO_F(a, ab); BITS_TO_F(b, bb); long r; unsigned f;
		__asm__ volatile("csrwi fflags, 0\n\tflt.s %1, %2, %3\n\tcsrr %0, fflags"
			: "=r"(f), "=r"(r) : "f"(a), "f"(b));
		return f; }
	unsigned fcvt_ws_flags(unsigned ab) {
		BITS_TO_F(a, ab); long r; unsigned f;
		__asm__ volatile("csrwi fflags, 0\n\tfcvt.w.s %1, %2, rtz\n\tcsrr %0, fflags"
			: "=r"(f), "=r"(r) : "f"(a));
		return f; }
	unsigned fcvt_sl_flags(long v) {
		float r; unsigned f;
		__asm__ volatile("csrwi fflags, 0\n\tfcvt.s.l %1, %2\n\tcsrr %0, fflags"
			: "=r"(f), "=f"(r) : "r"(v));
		return f; }

	int main() { return 0; }
	)M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls();
	machine.setup_linux({"fcsr"}, {"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value() == 0);

	auto call = [&] (const char* fn, uint32_t a, uint32_t b) {
		machine.vmcall(fn, a, b);
		return machine.return_value<uint32_t>();
	};
	auto call1 = [&] (const char* fn, uint32_t a) {
		machine.vmcall(fn, a);
		return machine.return_value<uint32_t>();
	};

	// Spec 11.3: a NaN result is the canonical quiet NaN, never a propagated
	// payload. A quiet NaN operand is not an invalid operation.
	REQUIRE(call("fadd_bits", QNAN, ONE) == QNAN);
	REQUIRE(call("fmul_bits", QNAN, ONE) == QNAN);
	REQUIRE(call("fdiv_bits", QNAN, ONE) == QNAN);
	REQUIRE(call("fadd_bits", SNAN, ONE) == QNAN);

	// FMIN/FMAX return the non-NaN operand, and the canonical NaN when both
	// operands are NaN. A signaling operand raises NV, a quiet one does not.
	REQUIRE(call("fmin_bits", QNAN, ONE) == ONE);
	REQUIRE(call("fmax_bits", QNAN, MONE) == MONE);
	REQUIRE(call("fmin_bits", QNAN, QNAN) == QNAN);
	REQUIRE((call("fmin_flags", SNAN, ONE) & NV) == NV);
	REQUIRE((call("fmin_flags", QNAN, ONE) & NV) == 0);

	// sqrt of a negative number is invalid; sqrt of a quiet NaN is not.
	REQUIRE(call1("fsqrt_bits", MONE) == QNAN);
	REQUIRE((call1("fsqrt_flags", MONE) & NV) == NV);
	REQUIRE(call1("fsqrt_bits", QNAN) == QNAN);
	REQUIRE((call1("fsqrt_flags", QNAN) & NV) == 0);
	REQUIRE((call1("fsqrt_flags", SNAN) & NV) == NV);

	// Division of a finite non-zero value by zero raises DZ.
	REQUIRE((call("fdiv_flags", ONE, 0u) & DZ) == DZ);
	REQUIRE((call("fdiv_flags", 0u, 0u) & DZ) == 0);
	REQUIRE((call("fdiv_flags", ONE, ONE) & DZ) == 0);

	// FLT is a signaling compare: any NaN operand raises NV.
	REQUIRE((call("flt_flags", QNAN, ONE) & NV) == NV);
	REQUIRE((call("flt_flags", ONE, ONE) & NV) == 0);

	// Out-of-range float->int is invalid, and an integer too wide for the
	// destination mantissa is inexact.
	REQUIRE((call1("fcvt_ws_flags", 0x71c37937u /* 1e30f */) & NV) == NV);
	REQUIRE((call1("fcvt_ws_flags", ONE) & NV) == 0);
	machine.vmcall("fcvt_sl_flags", int64_t((1ll << 62) + 1));
	REQUIRE((machine.return_value<uint32_t>() & NX) == NX);
	machine.vmcall("fcvt_sl_flags", int64_t(3));
	REQUIRE((machine.return_value<uint32_t>() & NX) == 0);
}

TEST_CASE("FCSR regressions from the accuracy patches", "[Verification]")
{
	// Same deal as above: these are FCSR-only promises. Each assertion in
	// fcsr_flags.c failed on at least one execution lane before the fix.
	if constexpr (!riscv::fcsr_emulation) {
		SUCCEED("FCSR emulation is disabled in this build");
		return;
	}

	const auto binary = build_and_load(R"M(
	#include "fptest/fcsr_flags.c"
)M", "-O2 -static -I" + cwd);

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls();
	machine.setup_linux({"fcsr_flags"}, {"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 0);
}
