#include "script.hpp"

#include "attributes.hpp"
#include "flat.hpp"
#include "rustattrs.hpp"

#include <fstream>

using namespace bench;

static std::vector<uint8_t> load_file(const std::string& filename)
{
	std::ifstream stream(filename, std::ios::in | std::ios::binary);
	if (!stream)
		throw std::runtime_error("Could not open file: " + filename);
	return {
		std::istreambuf_iterator<char>(stream),
		std::istreambuf_iterator<char>()
	};
}

Script::Script(const std::string& name, const std::string& filename)
	: m_binary(load_file(filename)), m_name(name)
{
	static bool installed = false;
	if (!installed) {
		installed = true;
		install_host_functions();
	}

	riscv::MachineOptions<MARCH> options {
		.memory_max = MAX_MEMORY,
		.stack_size = STACK_SIZE,
		.default_exit_function = "fast_exit",
	};
	m_machine = std::make_unique<machine_t>(m_binary, options);
	this->machine_setup();
	machine().simulate(MAX_BOOT_INSTR);
}

void Script::machine_setup()
{
	machine().set_userdata<Script>(this);
	machine().set_printer(
		(machine_t::printer_func)[](const machine_t&, const char* p, size_t len) {
			printf("%.*s", (int)len, p);
		});
	machine().on_unhandled_syscall = [](machine_t& m, size_t num) {
		auto& script = *m.get_userdata<Script>();
		fprintf(stderr, "%s: Unhandled system call: %zu\n", script.name().c_str(), num);
	};

	// The guest's #[global_allocator] allocates out of this arena, and so does
	// the host -- which is what lets either side own what the other built
	m_heap_area = machine().memory.mmap_allocate(MAX_HEAP);
	machine().setup_native_heap(HEAP_SYSCALLS_BASE, m_heap_area, MAX_HEAP);
	// memcpy, memset, memmove and memcmp, done natively by the host instead of
	// emulated. The C++ guest in examples/attribute_bench has always had these,
	// so the Rust guest needs them for the comparison to mean anything.
	machine().setup_native_memory(MEMORY_SYSCALLS_BASE);

	machine().setup_linux_syscalls();
	// The Rust standard library locks with futexes even when single-threaded
	machine().setup_posix_threads();
	machine().setup_linux({name()}, {"LC_CTYPE=C", "LC_ALL=C"});
}

/// @brief The two host functions the guest calls with an attribute tree, one per
/// marshalling strategy. Both build the same host-side tree and return a checksum
/// of it, which is what proves the two paths deliver the same data.
void Script::install_host_functions()
{
	machine_t::install_syscall_handler(SYSCALL_OUT_FLAT,
		[] (machine_t& machine) {
			auto& script = *machine.get_userdata<Script>();
			const auto [nodes, count] = machine.sysargs<gaddr_t, gaddr_t>();

			HostAttributes attrs;
			readFlatAttributes(script, nodes, count, attrs);
			machine.set_result(int64_t(checksum(attrs)));
		});

	machine_t::install_syscall_handler(SYSCALL_OUT_OBJ,
		[] (machine_t& machine) {
			auto& script = *machine.get_userdata<Script>();
			const auto [addr] = machine.sysargs<gaddr_t>();

			HostAttributes attrs;
			readGuestAttributes(script, addr, attrs);
			machine.set_result(int64_t(checksum(attrs)));
		});

	// A Rust &str is an address and a length, not a NUL-terminated string
	machine_t::install_syscall_handler(SYSCALL_PRINT,
		[] (machine_t& machine) {
			const auto [addr, len] = machine.sysargs<gaddr_t, gaddr_t>();
			const auto text = machine.memory.memview(addr, len);
			printf("%.*s", int(text.size()), text.data());
			machine.set_result(0);
		});
}

Script::gaddr_t Script::address_of(const std::string& name) const
{
	return machine().address_of(name.c_str());
}

Script::gaddr_t Script::guest_alloc(gaddr_t bytes)
{
	return machine().arena().malloc(bytes);
}

bool Script::guest_free(gaddr_t addr)
{
	return machine().arena().free(addr) == 0x0;
}
