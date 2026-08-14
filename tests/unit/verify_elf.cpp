#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <libriscv/machine.hpp>
extern std::vector<uint8_t> load_file(const std::string& filename);
static const uint64_t MAX_MEMORY = 680ul << 20; /* 680MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
static const std::string cwd {SRCDIR};
using namespace riscv;

// Requires paging: the Go runtime maps sparse address space far beyond the arena
#ifdef RISCV_VIRTUAL_PAGING
TEST_CASE("Golang Hello World", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/golang-riscv64-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	machine.fds().permit_filesystem = true;
	machine.fds().permit_sockets = false;
	machine.fds().filter_open = [] (void* user, const std::string& path) {
		(void) user; (void) path;
		return false;
	};
	// multi-threading
	machine.setup_posix_threads();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"golang-riscv64-hello-world"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	struct State {
		bool output_is_hello_world = false;
	} state;
	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		state->output_is_hello_world = (text == "hello world");
	});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 0);
	REQUIRE(state.output_is_hello_world);
}
#endif

TEST_CASE("Zig Hello World", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/zig-riscv64-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// Install Linux system calls
	machine.setup_linux_syscalls();
	// Create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"zig-riscv64-hello-world"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	struct State {
		std::string text;
	} state;
	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		state->text.append(data, data + size);
	});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 0);
	REQUIRE(state.text == "Hello, world!\n");
}

TEST_CASE("Rust Hello World", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/rust-riscv64-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// Install Linux system calls
	machine.setup_linux_syscalls();
	// Create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"rust-riscv64-hello-world"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	struct State {
		std::string text;
	} state;
	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		state->text.append(data, data + size);
	});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 0);
	REQUIRE(state.text == "Hello World!\n");
}

TEST_CASE("RV32 Newlib with B-ext Hello World", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv32gb-hello-world");

	riscv::Machine<RISCV32> machine { binary, { .memory_max = MAX_MEMORY } };
	// Install Linux system calls
	machine.setup_linux_syscalls();
	// Create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"newlib-rv32gb-hello-world"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	struct State {
		std::string text;
	} state;
	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		state->text.append(data, data + size);
	});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	// Sadly, main() returns int which gets sign-extended to all bits set
	// which kind of defeats the purpose of testing ROL... oh well.
	REQUIRE(machine.return_value() == 666);
	REQUIRE(state.text.find("[confronted]") != std::string::npos);
	REQUIRE(state.text.find("[problem]") != std::string::npos);
	REQUIRE(state.text.find("[regular]") != std::string::npos);
	REQUIRE(state.text.find("[expressions]") != std::string::npos);
	REQUIRE(state.text.find("[problems]") != std::string::npos);
	REQUIRE(state.text.find("Caught exception: Hello Exceptions!") != std::string::npos);
}

TEST_CASE("RV64 Newlib with B-ext Hello World", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// Install Linux system calls
	machine.setup_linux_syscalls();
	// Create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"newlib-rv64gb-hello-world"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	struct State {
		std::string text;
	} state;
	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		state->text.append(data, data + size);
	});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	// Sadly, main() returns int which gets sign-extended to all bits set
	// which kind of defeats the purpose of testing ROL... oh well.
	REQUIRE(machine.return_value() == 666);
	REQUIRE(state.text.find("[confronted]") != std::string::npos);
	REQUIRE(state.text.find("[problem]") != std::string::npos);
	REQUIRE(state.text.find("[regular]") != std::string::npos);
	REQUIRE(state.text.find("[expressions]") != std::string::npos);
	REQUIRE(state.text.find("[problems]") != std::string::npos);
	REQUIRE(state.text.find("Caught exception: Hello Exceptions!") != std::string::npos);
}

TEST_CASE("TinyCC dynamic fib", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/tinycc-rv64g-fib");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// Install Linux system calls
	machine.setup_linux_syscalls();
	// Create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"tinycc-rv64g-fib"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=groot"});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 75025); // fib(25)
}

// Requires paging: enforce_exec_only needs page attributes
#ifdef RISCV_VIRTUAL_PAGING
TEST_CASE("RV32 Execute-Only Hello World", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/riscv32gb-execute-only");
	constexpr uint64_t MAX_MEMORY = 128ul << 20; /* 128MB */

	riscv::Machine<RISCV32> machine{binary, {
		.memory_max = MAX_MEMORY,
		.enforce_exec_only = true,
	}};
	machine.setup_newlib_syscalls();
	machine.setup_argv(
		{"riscv32gb-execute-only"}, {});

	machine.setup_native_heap(580,
		machine.memory.mmap_allocate(0x1800000), 0x1800000);
	machine.setup_native_memory(585);

	struct State {
		std::string text;
	} state;
	machine.set_userdata(&state);

	machine.install_syscall_handler(502,
	[] (auto& machine) {
		auto [buf, count] = machine.template sysargs<uint32_t, uint32_t>();
		auto view = machine.memory.memview(buf, count);
		printf("%.*s", (int) view.size(), view.data());

		auto* state = machine.template get_userdata<State>();
		state->text.append((const char*) view.data(), view.size());
	});

	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 123);
	REQUIRE(state.text == "Hello, World!");
}

#endif

// Returns the file offset of the named section's header within the ELF.
static size_t section_header_offset(const std::vector<uint8_t>& elf, const char* section)
{
	auto rd = [&] (auto& out, size_t off) {
		REQUIRE(off + sizeof(out) <= elf.size());
		std::memcpy(&out, elf.data() + off, sizeof(out));
	};

	REQUIRE(elf.size() > 0x34);
	const bool is64 = elf[4] == 2; // EI_CLASS: 1 = ELF32, 2 = ELF64
	const size_t sh_offset_field = is64 ? 0x18 : 0x10; // sh_offset within the section header

	uint64_t e_shoff;
	uint16_t e_shentsize, e_shnum, e_shstrndx;
	if (is64) {
		rd(e_shoff, 0x28);
		rd(e_shentsize, 0x3A); rd(e_shnum, 0x3C); rd(e_shstrndx, 0x3E);
	} else {
		uint32_t shoff32; rd(shoff32, 0x20); e_shoff = shoff32;
		rd(e_shentsize, 0x2E); rd(e_shnum, 0x30); rd(e_shstrndx, 0x32);
	}

	uint64_t shstrtab_offset;
	const size_t strtab_hdr = e_shoff + e_shstrndx * e_shentsize;
	if (is64) {
		rd(shstrtab_offset, strtab_hdr + sh_offset_field);
	} else {
		uint32_t o; rd(o, strtab_hdr + sh_offset_field); shstrtab_offset = o;
	}

	for (uint16_t i = 0; i < e_shnum; i++) {
		const size_t hdr_offset = e_shoff + i * e_shentsize;
		uint32_t sh_name; rd(sh_name, hdr_offset); // sh_name is at +0 in both classes
		const char* name = (const char*)elf.data() + shstrtab_offset + sh_name;
		if (std::strcmp(name, section) == 0)
			return hdr_offset;
	}
	FAIL("Section " << section << " not found in test binary");
	return 0;
}

static void set_section_file_offset(std::vector<uint8_t>& elf,
	const char* section, uint64_t new_offset)
{
	const bool is64 = elf[4] == 2;
	const size_t sh_offset_field = is64 ? 0x18 : 0x10; // sh_offset within the section header
	const size_t hdr_offset = section_header_offset(elf, section);
	if (is64) {
		std::memcpy(elf.data() + hdr_offset + sh_offset_field, &new_offset, 8);
	} else {
		uint32_t o = (uint32_t)new_offset;
		std::memcpy(elf.data() + hdr_offset + sh_offset_field, &o, 4);
	}
}

// sh_type is a uint32_t at +4 in both ELF32 and ELF64 section headers.
static void set_section_type(std::vector<uint8_t>& elf,
	const char* section, uint32_t sh_type)
{
	const size_t hdr_offset = section_header_offset(elf, section);
	std::memcpy(elf.data() + hdr_offset + 4, &sh_type, 4);
}

static void set_section_size(std::vector<uint8_t>& elf,
	const char* section, uint64_t new_size)
{
	const bool is64 = elf[4] == 2;
	const size_t sh_size_field = is64 ? 0x20 : 0x14; // sh_size within the section header
	const size_t hdr_offset = section_header_offset(elf, section);
	if (is64) {
		std::memcpy(elf.data() + hdr_offset + sh_size_field, &new_size, 8);
	} else {
		uint32_t s = (uint32_t)new_size;
		std::memcpy(elf.data() + hdr_offset + sh_size_field, &s, 4);
	}
}

TEST_CASE("Out-of-range .text section offset", "[Verify]")
{
	auto binary = load_file(cwd + "/elf/fib");
	set_section_file_offset(binary, ".text", 0x70000000ull);

	try {
		riscv::Machine<RISCV32> machine { binary, { .memory_max = MAX_MEMORY } };
		(void) machine;
	} catch (const riscv::MachineException&) {
		// Gucci
	}
}

TEST_CASE("Out-of-range .symtab section offset", "[Verify]")
{
	auto binary = load_file(cwd + "/elf/fib");
	set_section_file_offset(binary, ".symtab", 0x70000000ull);

	// The symbol table is parsed lazily, so loading must succeed, and the
	// best-effort symbol paths must return empty instead of throwing.
	riscv::Machine<RISCV32> machine { binary, { .memory_max = MAX_MEMORY } };
	REQUIRE(machine.address_of("main") == 0);
	REQUIRE_NOTHROW(machine.memory.all_symbols());
	REQUIRE_NOTHROW(machine.memory.lookup(0x10074));
}

TEST_CASE("Out-of-range .strtab section offset", "[Verify]")
{
	auto binary = load_file(cwd + "/elf/fib");
	set_section_file_offset(binary, ".strtab", 0x70000000ull);

	riscv::Machine<RISCV32> machine { binary, { .memory_max = MAX_MEMORY } };
	REQUIRE(machine.address_of("main") == 0);
	REQUIRE_NOTHROW(machine.memory.all_symbols());
	REQUIRE_NOTHROW(machine.memory.lookup(0x10074));
}

TEST_CASE("SHT_NOBITS .symtab cannot bypass file-range validation", "[Verify]")
{
	static constexpr uint32_t SHT_NOBITS = 8;
	auto binary = load_file(cwd + "/elf/fib");
	// Point the symbol table out of bounds and disguise it as a section
	// with no file contents, which must not be file-indexed regardless.
	set_section_file_offset(binary, ".symtab", 0x70000000ull);
	set_section_type(binary, ".symtab", SHT_NOBITS);

	riscv::Machine<RISCV32> machine { binary, { .memory_max = MAX_MEMORY } };
	REQUIRE(machine.address_of("main") == 0);
	REQUIRE_NOTHROW(machine.memory.all_symbols());
	REQUIRE_NOTHROW(machine.memory.lookup(0x10074));
}

TEST_CASE("Symbol section ending exactly at EOF is accepted", "[Verify]")
{
	// A section occupying [sh_offset, sh_offset + sh_size) is valid when it
	// ends exactly at end-of-file; reading the final byte(s) must not throw.
	SECTION(".strtab ends at EOF") {
		auto binary = load_file(cwd + "/elf/fib");
		const uint64_t at_eof = binary.size();
		binary.push_back(0x00);
		set_section_file_offset(binary, ".strtab", at_eof);
		set_section_size(binary, ".strtab", 1);

		riscv::Machine<RISCV32> machine { binary, { .memory_max = MAX_MEMORY } };
		REQUIRE_NOTHROW(machine.address_of("main"));
		REQUIRE_NOTHROW(machine.memory.all_symbols());
	}
	SECTION(".symtab ends at EOF") {
		auto binary = load_file(cwd + "/elf/fib");
		const uint64_t at_eof = binary.size();
		binary.resize(binary.size() + 16, 0x00); // one zeroed Elf32_Sym
		set_section_file_offset(binary, ".symtab", at_eof);
		set_section_size(binary, ".symtab", 16);

		riscv::Machine<RISCV32> machine { binary, { .memory_max = MAX_MEMORY } };
		REQUIRE_NOTHROW(machine.address_of("main"));
		REQUIRE_NOTHROW(machine.memory.all_symbols());
	}
}
