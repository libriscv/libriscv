#include <libriscv/machine.hpp>
#include "helpers.cpp"

static const std::vector<uint8_t> empty;
static bool init = false;
static constexpr int W = RISCV_ARCH;
static constexpr uint32_t MAX_CYCLES = 5'000;

static void fuzz_instruction_set(const uint8_t* data, size_t len)
{
	static riscv::Machine<W> machine { empty };
	static std::vector<uint8_t> vec;
	constexpr uint32_t S = 0x1000;
	constexpr uint32_t V = 0x2000;

	if (UNLIKELY(len == 0 || init == false)) {
		init = true;
		machine.memory.set_page_attr(S, 0x1000, {.read = true, .write = true});
		machine.on_unhandled_syscall = [] (auto&, int) {};
		vec.resize(riscv::Page::size());
		return;
	}

	memcpy(vec.data(), data, std::min(len, vec.size()));
	try
	{
		machine.cpu.init_execute_area(vec.data(), V, vec.size());
		machine.cpu.reg(riscv::REG_SP) = V;
		machine.cpu.jump(V);
		// Let's avoid loops
		machine.simulate<false>(MAX_CYCLES);
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
		const MachineOptions<W> options { .allow_write_exec_segment = true };
		Machine<W> machine { bin, options };
		machine.on_unhandled_syscall = [] (auto&, int) {};
		// This crashes when there is an empty
		// or no execute section:
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
