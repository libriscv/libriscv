#include <fmt/core.h>
#include <libriscv/machine.hpp>
#include <libriscv/rv32i_instr.hpp>
static std::vector<uint8_t> load_file(const std::string& filename);
static void setup_syscall_interface();
using namespace riscv;
static constexpr int MARCH = riscv::RISCV64;
using RiscvMachine = riscv::Machine<MARCH>;
using gaddr_t = riscv::address_type<MARCH>;

// A function that can be called from inside the guest program
using HostFunction = std::function<void(RiscvMachine&)>;
// An array of host functions that can be called from the guest program
static std::array<HostFunction, 64> g_host_functions {};
static void register_function(unsigned number, HostFunction&& fn) {
	g_host_functions.at(number) = std::move(fn);
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		fmt::print("Usage: {} [program file] [arguments ...]\n", argv[0]);
		return -1;
	}
	static gaddr_t g_host_functions_addr = 0;

	// Register a host function that can be called from the guest program
	register_function(0, [](RiscvMachine& machine) {
		fmt::print("Hello from host function 0!\n");

		// Get a zero-copy view of Strings in guest memory
		struct Strings {
			gaddr_t count;
			gaddr_t strings[32];
		};
		auto [vec] = machine.sysargs<Strings*>();

		// For each string up to count, read it from guest memory and print it
		for (size_t i = 0; i < vec->count; i++) {
			std::string str = machine.memory.memstring(vec->strings[i]);
			fmt::print("  {}\n", str);
		}
	});

	// Register a two-way host function that modifies guest memory
	register_function(1, [](RiscvMachine& machine) {
		fmt::print("Hello from host function 1!\n");

		// Get a zero-copy view of Strings in guest memory
		struct Buffer {
			gaddr_t count;
			char    buffer[256];            // An inline buffer
			gaddr_t another_count;
			gaddr_t another_buffer_address; // A pointer to a buffer somewhere in guest memory
		};
		auto [buf] = machine.sysargs<Buffer*>();

		// Write a string to the buffer in guest memory
		strcpy(buf->buffer, "Hello from host function 1!");
		buf->count = strlen(buf->buffer);

		// The "another" buffer has a count and then a guest pointer to the buffer
		// In order to get a writable pointer to that buffer, we can use memarray<T>():
		char* another_buf = machine.memory.memarray<char>(buf->another_buffer_address, buf->another_count);
		// Let's check if the buffer is large enough to hold the string
		const std::string str = "Another buffer from host function 1!";
		if (str.size() > buf->another_count) {
			fmt::print("Another buffer is too small to hold the string!\n");
			return;
		}
		// Copy the string to the buffer
		strcpy(another_buf, str.c_str());
		another_buf[str.size()] = '\0';
		// Update the count of the buffer
		buf->another_count = str.size();
	});

	// Register a host function that takes a function pointer
	register_function(2, [](RiscvMachine& machine) {
		// Get the function pointer argument as a guest address
		auto [fn] = machine.sysargs<gaddr_t>();

		// Set our host function address so we can call it later
		g_host_functions_addr = fn;
	});

	// Create a new machine
	std::vector<uint8_t> program = load_file(argv[1]);
	RiscvMachine machine(program);

	// Setup the machine configuration and syscall interface
	machine.setup_linux({"program"}, {"LC_CTYPE=C", "LC_ALL=C", "USER=groot"});
	// Add POSIX system call interfaces (no filesystem or network access)
	machine.setup_linux_syscalls(false, false);
	machine.setup_posix_threads();
	setup_syscall_interface();

	// Run the machine
	try {
		machine.simulate();
	} catch (const std::exception& e) {
		fmt::print("Exception: {}\n", e.what());
	}

	// Call the host function that takes a function pointer
	if (g_host_functions_addr != 0) {
		fmt::print("Calling host function 2...\n");
		machine.vmcall(g_host_functions_addr, "Hello From A Function Callback!");
	} else {
		fmt::print("Host function 2 was not called!!?\n");
	}

	return 0;
}

void setup_syscall_interface()
{
	// A custom instruction that executes a function based on an index
	// This variant is faster than a system call, and can use 8 integers as arguments
    static const Instruction<MARCH> unchecked_dyncall_instruction {
        [](CPU<MARCH>& cpu, riscv::rv32i_instruction instr)
        {
            g_host_functions[instr.Itype.imm](cpu.machine());
        },
        [](char* buffer, size_t len, auto&, riscv::rv32i_instruction instr) -> int
        {
            return fmt::format_to_n(buffer, len,
                "DYNCALL: 4-byte idx={:x} (inline, 0x{:X})",
                uint32_t(instr.Itype.imm),
                instr.whole
            ).size;
        }};
    // Override the machines unimplemented instruction handling,
    // in order to use the custom instruction for a given opcode.
    CPU<MARCH>::on_unimplemented_instruction
        = [](riscv::rv32i_instruction instr) -> const Instruction<MARCH>&
    {
        if (instr.opcode() == 0b1011011 && instr.Itype.rs1 == 0 && instr.Itype.rd == 0)
        {
			if (instr.Itype.imm < g_host_functions.size())
				return unchecked_dyncall_instruction;
        }
        return CPU<MARCH>::get_unimplemented_instruction();
    };
}

#include <unistd.h>
std::vector<uint8_t> load_file(const std::string& filename)
{
	size_t size = 0;
	FILE* f		= fopen(filename.c_str(), "rb");
	if (f == NULL)
		throw std::runtime_error("Could not open file: " + filename);

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
