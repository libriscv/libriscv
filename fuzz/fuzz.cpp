#include <libriscv/machine.hpp>
#include "helpers.cpp"

static const std::vector<uint8_t> empty;
static constexpr int W = RISCV_ARCH;
static constexpr uint32_t MAX_CYCLES = 5'000;

// In order to be able to inspect a coredump we want to
// crash on every ASAN error.
extern "C" void __asan_on_error()
{
	abort();
}
extern "C" void __msan_on_error()
{
	abort();
}

static void fuzz_instruction_set(const uint8_t* data, size_t len)
{
	constexpr uint32_t S = 0x1000;
	constexpr uint32_t V = 0x2000;

	try
	{
		riscv::Machine<W> machine { empty };
		machine.memory.set_page_attr(S, 0x1000, {.read = true, .write = true});
		machine.memory.set_page_attr(V, 0x1000, {.read = true, .exec = true});
		machine.on_unhandled_syscall = [] (auto&, size_t) {};
		machine.cpu.init_execute_area(data, V, len);
		//machine.cpu.reg(riscv::REG_RA) = 0xffffffff;
		//machine.cpu.reg(riscv::REG_SP) = 0x1;
		machine.cpu.jump(V);
		// Let's avoid loops
		machine.reset_instruction_counter();
		machine.simulate(MAX_CYCLES);
	}
	catch (const std::exception &e)
	{
		//printf(">>> Exception: %s\n", e.what());
	}
}

static void fuzz_elf_loader(const uint8_t* data, size_t len)
{
	using namespace riscv;
	const std::string_view bin {(const char*) data, len};
	try {
		const MachineOptions<W> options {
			.allow_write_exec_segment = true,
			.use_memory_arena = false
		};
		Machine<W> machine { bin, options };
		machine.on_unhandled_syscall = [] (auto&, size_t) {};
		machine.simulate(MAX_CYCLES);
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}

extern "C"
void LLVMFuzzerTestOneInput(const uint8_t* data, size_t len)
{
#if defined(FUZZ_ELF)
	fuzz_elf_loader(data, len);
#elif defined(FUZZ_VM)
	fuzz_instruction_set(data, len);
#else
	#error "Unknown fuzzing mode"
#endif
}
