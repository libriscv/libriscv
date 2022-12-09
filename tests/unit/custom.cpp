#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/rv32i_instr.hpp>
#include "custom.hpp"
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
static const std::string cwd {SRCDIR};
using namespace riscv;

/** The new custom instruction **/
static const Instruction<RISCV64> custom {
	[] (auto& cpu, rv32i_instruction instr) {
		printf("Hello custom instruction World!\n");
		REQUIRE(instr.opcode() == 0b1010111);
		cpu.reg(riscv::REG_ARG0) = 0xDEADB33F;
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) {
		return snprintf(buffer, len, "CUSTOM: 4-byte 0x%X (0x%X)",
						instr.opcode(), instr.whole);
	}
};

TEST_CASE("Custom instruction", "[Custom]")
{
	const auto binary = build_and_load(R"M(
int main()
{
	__asm__(".word 0b1010111");
	__asm__("ret");
}
)M");

	CPU<RISCV64>::on_unimplemented_instruction =
	[] (rv32i_instruction instr) -> const Instruction<RISCV64>& {
		if (instr.opcode() == 0b1010111) {
			return custom;
		}
		return CPU<RISCV64>::get_unimplemented_instruction();
	};

	// Normal (fastest) simulation
	{
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		// We need to install Linux system calls for maximum gucciness
		machine.setup_linux_syscalls();
		// We need to create a Linux environment for runtimes to work well
		machine.setup_linux(
			{"va_exec"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		// Run for at most X instructions before giving up
		machine.simulate(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value() == 0xDEADB33F);
	}
	// Precise (step-by-step) simulation
	{
		riscv::Machine<RISCV64> machine{binary, { .memory_max = MAX_MEMORY }};
		machine.setup_linux_syscalls();
		machine.setup_linux(
			{"va_exec"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		// Verify step-by-step simulation
		machine.cpu.simulate_precise(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value() == 0xDEADB33F);
	}
}

#include <map>
struct SystemFunctionHandler {
	std::function<SystemArg(Machine<RISCV64>&, const SystemFunctionArgs&)> handler;
	size_t arguments = 0;
};
static std::map<std::string, SystemFunctionHandler> sf_handlers;

static void add_system_functions()
{
	sf_handlers["AddTwoFloats"].handler =
		[] (Machine<RISCV64>&, const SystemFunctionArgs& args) -> SystemArg {
			// TODO: Check arguments
			return {
				.f32 = args.arg[0].f32 + args.arg[1].f32,
				.type = FLOAT_32,
			};
		};
	sf_handlers["AddTwoFloats"].arguments = 2;

	sf_handlers["Print"].handler =
		[] (Machine<RISCV64>&, const SystemFunctionArgs& args) -> SystemArg {
			// TODO: Check arguments
			std::string str { args.arg[0].string };
			printf("Print: %s\n", str.c_str());
			REQUIRE(str == "Hello World!");
			return {
				.u32 = (unsigned)str.size(),
				.type = UNSIGNED_INT,
			};
		};
	sf_handlers["Print"].arguments = 1;
}

static SystemArg perform_system_function(Machine<RISCV64>& machine,
	const std::string& name, size_t argc, SystemFunctionArgs& args)
{
	printf("System function: %s\n", name.c_str());

	auto it = sf_handlers.find(name);
	if (it == sf_handlers.end())
	{
		return {
			.u32 = ERROR_NO_SUCH_FUNCTION,
			.type = ERROR,
		};
	}
	auto& handler = it->second;

	if (argc < handler.arguments)
	{
		return {
			.u32 = ERROR_MISSING_ARGUMENTS,
			.type = ERROR
		};
	}

	// Zero-terminate all strings (set the last char to zero)
	for (size_t i = 0; i < argc; i++) {
		if (args.arg[i].type == STRING)
			args.arg[i].string[STRING_BUFFER_SIZE-1] = 0;
	}

	return handler.handler(machine, args);
}

TEST_CASE("Take custom system arguments", "[Custom]")
{
	const auto binary = build_and_load(R"M(
		#include "custom.hpp"
		#include <stdio.h>
		#include <string.h>
static long syscall(long n, long arg0, long arg1, long arg2, long arg3)
{
	register long a0 __asm__("a0") = arg0;
	register long a1 __asm__("a1") = arg1;
	register long a2 __asm__("a2") = arg2;
	register long a3 __asm__("a3") = arg3;
	register long syscall_id __asm__("a7") = n;

	__asm__ volatile ("scall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(syscall_id));
	return a0;
}

	static void system_function(
		const char *name,
		size_t n, struct SystemFunctionArgs *args,
		struct SystemArg *result)
	{
		__asm__("" ::: "memory");
		syscall(500, (long)name, n, (long)args, (long)result);
	}

	static void print_arg(struct SystemArg *arg)
	{
		switch (arg->type) {
			case SIGNED_INT:
				printf("32-bit signed integer: %d\n", arg->i32);
				break;
			case UNSIGNED_INT:
				printf("32-bit unsigned integer: %d\n", arg->u32);
				break;
			case FLOAT_32:
				printf("32-bit floating-point: %f\n", arg->f32);
				break;
			case FLOAT_64:
				printf("64-bit floating-point: %f\n", arg->f64);
				break;
			case STRING:
				printf("String: %s\n", arg->string);
				break;
			case ERROR:
				printf("Error code: 0x%X\n", arg->u32);
				break;
			default:
				printf("Unknown value: 0x%X\n", arg->u32);
		}
	}

	int main() {
		// Setup system function "AddTwoFloats"
		struct SystemFunctionArgs sfa;
		sfa.arg[0].type = FLOAT_32;
		sfa.arg[0].f32  = 64.0f;
		sfa.arg[1].type = FLOAT_32;
		sfa.arg[1].f32  = 32.0f;

		// Perform 'AddTwoFloats' system function
		struct SystemArg result;
		system_function("AddTwoFloats", 2, &sfa, &result);

		// Result should be a 32-bit FP value
		print_arg(&result);

		// Perform 'Print'
		sfa.arg[0].type = STRING;
		strcpy(sfa.arg[0].string, "Hello World!");
		system_function("Print", 1, &sfa, &result);

		return 0x1234;
	})M", "-O2 -static -I" + cwd);

	Machine<RISCV64> machine{binary};
	machine.setup_linux(
		{"myprogram"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.setup_linux_syscalls();

	// Add our system functions
	add_system_functions();

	Machine<RISCV64>::install_syscall_handler(500,
	[] (Machine<RISCV64>& machine) {
		// Retrieve name (string), argument count (32-bit unsigned)
		// and the whole SystemFunctionArgs structure.
		auto [name, argc, args] =
			machine.sysargs <std::string, unsigned, SystemFunctionArgs> ();
		// The address of the result
		auto g_result = machine.sysarg(3);
		// A little bounds-checking
		const size_t count = std::min(argc, 4u);
		auto result =
			perform_system_function(machine, name, count, args);
		machine.copy_to_guest(g_result, &result, sizeof(result));
		machine.set_result(0);
	});

	machine.set_printer([] (const auto&, const char* data, size_t size) {
		std::string text{data, data + size};
		REQUIRE(text == "32-bit floating-point: 96.000000\n");
	});

	machine.simulate();

	REQUIRE(machine.return_value() == 0x1234);
}
