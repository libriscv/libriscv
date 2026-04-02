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
