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
