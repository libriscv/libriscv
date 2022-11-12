#include <libriscv/machine.hpp>
#include <libriscv/decoder_cache.hpp>
#include <libriscv/rv32i_instr.hpp>
#include <inttypes.h>
static inline std::vector<uint8_t> load_file(const std::string&);
static constexpr uint64_t MAX_MEMORY = 1024 * 1024 * 256ul;
static const std::vector<std::string> env = {
	"LC_CTYPE=C", "LC_ALL=C", "USER=groot"
};

template <int W>
static riscv::Machine<W> create_machine(
	const std::vector<uint8_t>& binary,
	const std::vector<std::string>& args)
{
	riscv::Machine<W> machine { binary, {
		.memory_max = MAX_MEMORY,
		.verbose_loader = (getenv("VERBOSE") != nullptr)
	}};

	machine.setup_linux(args, env);
	// Linux system to open files and access internet
	machine.setup_linux_syscalls();
	machine.fds().permit_filesystem = true;
	machine.fds().permit_sockets = true;
	// Only allow opening certain file paths. The void* argument is
	// the user-provided pointer set in the RISC-V machine.
	machine.fds().filter_open = [] (void* user, const std::string& path) {
		(void) user;
		if (path == "/etc/hostname")
			return true;
		if (path == "/dev/urandom")
			return true;
		return false;
	};
	// multi-threading
	machine.setup_posix_threads();
	return machine;
}

template <int W>
static void run_program(
	const std::vector<uint8_t>& binary,
	const std::vector<std::string>& args)
{
	auto m1 = create_machine<W>(binary, args);
	auto m2 = create_machine<W>(binary, args);

	// Instruction limit is used to keep running
	m1.set_max_instructions(1'000'000UL);
	m2.set_max_instructions(1'000'000UL);

	while (!m1.stopped())
	{
		auto& cpu = m1.cpu;
		// Get 32- or 16-bits instruction
		auto instr = cpu.read_next_instruction();
		// Decode instruction to get instruction info
		auto handlers = cpu.decode(instr);
		// Execute one instruction, and increment PC
		handlers.handler(cpu, instr);
		cpu.increment_pc(instr.length());

		//auto regs_str = cpu.registers().to_string();
		//printf("%s\n", regs_str.c_str());
		auto m1_instr = cpu.current_instruction_to_string();
		printf("%s\n", m1_instr.c_str());
		bool pause = false;

		auto m2_instr = cpu.current_instruction_to_string();
		if (m1_instr != m2_instr) {
			printf("Instructions diverged!\n");
			printf("M1: %s\n", m1_instr.c_str());
			printf("M2: %s\n", m2_instr.c_str());
			pause = true;
		}
		m2.cpu.step_one();

		const auto& gpr1 = m1.cpu.registers();
		const auto& gpr2 = m2.cpu.registers();
		for (size_t i = 0; i < 32; i++)
		{
			if (gpr1.get(i) != gpr2.get(i)) {
				printf("Register %zu diverged\n", i);
				printf("M1 value: 0x%lX\n", (long)gpr1.get(i));
				printf("M2 value: 0x%lX\n", (long)gpr2.get(i));
				pause = true;
			}
		}
		if (gpr1.pc != gpr2.pc) {
			printf("PC diverged!\n");
			printf("M1 PC: 0x%lX\n", (long)gpr1.pc);
			printf("M2 PC: 0x%lX\n", (long)gpr2.pc);
			pause = true;
		}
		if (pause)
			m2.print_and_pause();
	}
}

int main(int argc, const char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Provide RISC-V binary as argument!\n");
		exit(1);
	}

	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) {
		args.push_back(argv[i]);
	}
	const std::string& filename = args.front();

	const auto binary = load_file(filename);
	assert(binary.size() >= 64);

	try {
		if (binary[4] == ELFCLASS64)
			run_program<riscv::RISCV64> (binary, args);
		else
			run_program<riscv::RISCV32> (binary, args);
	} catch (const std::exception& e) {
		printf("Exception: %s\n", e.what());
	}

	return 0;
}

#include <stdexcept>
#include <unistd.h>
std::vector<uint8_t> load_file(const std::string& filename)
{
    size_t size = 0;
    FILE* f = fopen(filename.c_str(), "rb");
    if (f == NULL) throw std::runtime_error("Could not open file: " + filename);

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> result(size);
    if (size != fread(result.data(), 1, size, f))
    {
        fclose(f);
        throw std::runtime_error("Error when reading from file: " + filename);
    }
    fclose(f);
    return result;
}
