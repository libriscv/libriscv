#include <libriscv/machine.hpp>
#include <chrono>
#include <iostream>
static constexpr size_t N = 2'500'000u; // Number of instructions to execute

// A list of repeatable instructions used for benchmarking
std::unordered_map<std::string, uint32_t> instruction_map = {
	{"mv", 0x00058613}, // mv a2, a1
	{"li", 0x00000793}, // li a5, 0
	{"nop", 0x00000013}, // nop is a pseudo-instruction, equivalent to addi x0, x0, 0
	{"j", 0x6f}, // j is a pseudo-instruction for jal x0, offset
	{"jal", 0x6f},
	{"jalr", 0x67},
	{"beq", 0x63},
	{"bne", 0x1063},
	{"blt", 0x4063},
	{"bge", 0x5063},
	{"bltu", 0x6063},
	{"bgeu", 0x7063},
	{"lb", 0x3}, // Load byte
	{"lh", 0x1003}, // Load halfword
	{"lw", 0x2003}, // Load word
	{"lbu", 0x4003}, // Load byte unsigned
	{"lhu", 0x5003}, // Load halfword unsigned
	{"sb", 0x23}, // Store byte
	{"sh", 0x1023}, // Store halfword
	{"sw", 0x2023}, // Store word
	{"addi", 0x81010113}, // Addi sp, sp - 2032
	{"slti", 0x2013}, // Set less than immediate
	{"sltiu", 0x3013}, // Set less than immediate unsigned
	{"andi", 0x7013}, // And immediate
	{"ori", 0x6013}, // Or immediate
	{"xori", 0x5013}, // Xor immediate
	{"slli", 0x1013}, // Shift left logical immediate
	{"srli", 0x5013}, // Shift right logical immediate
	{"srai", 0x4013}, // Shift right arithmetic immediate
	{"lui", 0x37}, // Load upper immediate
	{"auipc", 0x17}, // Add upper immediate to PC
	{"fence", 0x0f}, // Fence instruction
	{"fence_i", 0x001f}, // Fence instruction with immediate
	{"ecall", 0x73}, // Environment call
	{"ebreak", 0x100073}, // Environment break
	{"csrrw", 0x1003}, // CSR read/write
	{"csrrs", 0x2003}, // CSR read/set
	{"csrrc", 0x3003}, // CSR read/clear
	{"csrrwi", 0x4003}, // CSR read/write immediate
	{"csrrsi", 0x5003}, // CSR read/set immediate
	{"csrrci", 0x6003}, // CSR read/clear immediate
	{"lwu", 0x2003}, // Load word unsigned
	{"ld", 0x3003}, // Load doubleword
	{"sd", 0x3023}, // Store doubleword
	{"addiw", 0x1b}, // Add immediate word
	{"slliw", 0x101b}, // Shift left logical immediate word
	{"srliw", 0x501b}, // Shift right logical immediate word
	{"sraiw", 0x401b}, // Shift right arithmetic immediate word
	{"addw", 0x1b}, // Add word
	{"subw", 0x4000001b}, // Subtract word
	{"mulw", 0x2000001b}, // Multiply word
	{"divw", 0x2000001b}, // Divide word
	{"andw", 0x7000001b}, // And word
	{"orw", 0x6000001b}, // Or word
	{"xorw", 0x5000001b}, // Xor word
	{"sllw", 0x1000001b}, // Shift left logical word
	{"srlw", 0x1010001b}, // Shift right logical word
	{"sraw", 0x1020001b}, // Shift right arithmetic word
	{"add", 0x33},
	{"sub", 0x40000033},
	{"mul", 0x20000033},
	{"div", 0x20000033},
	{"and", 0x70000033},
	{"or", 0x60000033},
	{"xor", 0x50000033},
	{"sll", 0x10000033},
	{"srl", 0x10100033},
	{"sra", 0x10200033},
	// Add more instructions as needed
};
static constexpr uint32_t STOP_INSTRUCTION = 0x7ff00073; // system (stop)
static const std::vector<uint8_t> empty;
static constexpr uint32_t V = 0x2000;

int main(int argc, char** argv) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <instruction_name>" << std::endl;
		return 1;
	}
	const char* instr_name = argv[1];
	const uint32_t instr_code = instruction_map[instr_name];
	if (instr_code == 0) {
		std::cerr << "Unknown instruction: " << instr_name << std::endl;
		return 1;
	}
	std::cout << "Benchmarking instruction: " << instr_name << std::endl;

	std::vector<uint32_t> instructions;
	instructions.reserve(N + 1);
	for (size_t i = 0; i < N; ++i) {
		instructions.push_back(instr_code);
	}
	// End the sequence with a STOP instruction
	instructions.push_back(STOP_INSTRUCTION);

	riscv::Machine<riscv::RISCV64> machine {empty, riscv::MachineOptions<riscv::RISCV64>{
		.allow_write_exec_segment = true,
		.use_memory_arena = false
	}};

	// Load the benchmark stream
	machine.cpu.init_execute_area(instructions.data(), V, instructions.size() * sizeof(uint32_t));
	// Set the initial program counter to the start of the instructions
	machine.cpu.jump(V);
	machine.simulate<false>(1600);
	machine.cpu.jump(V);

	using time_point = std::chrono::high_resolution_clock::time_point;
	using duration = std::chrono::duration<double, std::micro>;

	// Start the timer
	auto t0 = std::chrono::high_resolution_clock::now();

	// Run the benchmark
	machine.simulate(500'000'000ull);

	auto t1 = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> runtime = t1 - t0;

	const uint64_t instructions_executed = machine.instruction_counter();
	std::cout << "Elapsed time: " << runtime.count()*1000.0 << " millis"
			<< ", "
			<< (instructions_executed / (runtime.count() * 1e6))
			<< " MI/s" << std::endl;

	return 0;
}
