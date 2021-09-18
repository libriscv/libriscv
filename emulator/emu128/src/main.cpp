#include <libriscv/machine.hpp>
#include "program.h" /* program_bin */
using namespace riscv;
static std::string load_file(const std::string&);

static void init_program_at(Machine<RISCV128>& machine,
	__uint128_t base_addr, const uint8_t* bin, size_t bin_len)
{
	machine.memory.set_page_attr(base_addr, 0xA000, {.read = true, .write = false, .exec = true});
	machine.copy_to_guest(base_addr, bin, bin_len);
	machine.cpu.init_execute_area(bin, base_addr, bin_len);
	machine.cpu.jump(base_addr);
}

int main(int argc, const char** argv)
{
	Machine<RISCV128> machine { std::string_view{} };

	static const __uint128_t BASE_ADDRESS = 0x100000;
	if (argc == 1) {
		init_program_at(machine, BASE_ADDRESS,
			_tmp_program_bin, _tmp_program_bin_len);
	} else {
		static auto vec = load_file(argv[1]);
		init_program_at(machine, BASE_ADDRESS,
			(const uint8_t*)vec.data(), vec.size());
	}

	/* Install a system call handler that stops the machine. */
	Machine<RISCV128>::install_syscall_handler(1,
	 [] (Machine<RISCV128>& machine) {
		 const auto [code] = machine.sysargs <int> ();
		 printf(">>> Program exited with code: %d\n", code);
		 machine.stop();
	 });

	 /* Install a system call handler that prints something. */
	 Machine<RISCV128>::install_syscall_handler(2,
 	 [] (Machine<RISCV128>& machine) {
 		 const auto [str] = machine.sysargs <address_type<RISCV128>> ();
 		 printf(">>> Program says: %s\n", machine.memory.memstring(str).c_str());
 	 });

	/* Add program arguments on the stack. */
	machine.setup_argv({"emu128", "Hello World"});

	/* This function will run until the exit syscall has stopped the
	   machine, an exception happens which stops execution, or the
	   instruction counter reaches the given limit (1M): */
	try {
#ifdef RISCV_DEBUG
		machine.verbose_instructions = true;
		//machine.verbose_registers = true;
#endif
		machine.simulate(1'000'000);
	} catch (const std::exception& e) {
		fprintf(stderr, ">>> Runtime exception: %s\n", e.what());
	}

	printf("\n\nFinal machine registers:\n%s\n",
		machine.cpu.registers().to_string().c_str());
}

#include <stdexcept>
#include <unistd.h>
std::string load_file(const std::string& filename)
{
    size_t size = 0;
    FILE* f = fopen(filename.c_str(), "rb");
    if (f == NULL) throw std::runtime_error("Could not open file: " + filename);

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string result;
	result.resize(size);
    if (size != fread(result.data(), 1, size, f))
    {
        fclose(f);
        throw std::runtime_error("Error when reading from file: " + filename);
    }
    fclose(f);
    return result;
}
