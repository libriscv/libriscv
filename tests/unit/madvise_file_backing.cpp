#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
#include <array>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace riscv;
extern std::vector<uint8_t> build_and_load(const std::string&, const std::string&, bool);

static const std::vector<uint8_t> empty_binary;
static constexpr address_type<RISCV64> guest_addr = 0x100000;

static uint8_t guest_read_byte(Machine<RISCV64>& machine, address_type<RISCV64> addr)
{
#ifdef RISCV_VIRTUAL_PAGING
	return machine.memory.read<uint8_t>(addr);
#else
	uint8_t value;
	machine.memory.memcpy_out(&value, addr, sizeof(value));
	return value;
#endif
}

static void guest_write_byte(Machine<RISCV64>& machine, address_type<RISCV64> addr,
	uint8_t value)
{
#ifdef RISCV_VIRTUAL_PAGING
	machine.memory.write<uint8_t>(addr, value);
#else
	machine.memory.memcpy(addr, &value, sizeof(value));
#endif
}

struct TemporaryFile
{
	int fd = -1;
	TemporaryFile()
	{
		char path[] = "/tmp/libriscv-madvise-XXXXXX";
		fd = ::mkstemp(path);
		if (fd >= 0)
			::unlink(path);
	}
	~TemporaryFile() { if (fd >= 0) ::close(fd); }
};

static void write_file(int fd, const uint8_t* data, size_t size)
{
	REQUIRE(::pwrite(fd, data, size, 0) == ssize_t(size));
}

static void map_test_page(Machine<RISCV64>& machine, int fd,
	const uint8_t* data, size_t size)
{
	machine.memory.memcpy(guest_addr, data, size);
	bool retention_failed = false;
	auto backing = machine.memory.retain_file_mapping(fd, retention_failed);
	REQUIRE_FALSE(retention_failed);
	REQUIRE(backing != nullptr);
	machine.memory.register_file_mapping(guest_addr, size, 0, std::move(backing));
}

static std::vector<uint8_t> build_file_refault_probe(bool store)
{
	std::string body = "li t0, 0x100000\n";
	if (store)
		body += "li t1, 69\nsb t1, 0(t0)\n";
	body += "lb a0, 0(t0)\n.word 0x7ff00073\n";
	std::string source = "__attribute__((naked)) void refault_entry(void) { __asm__ volatile(\"";
	for (char c : body) source += c == '\n' ? "\\n" : std::string(1, c);
	source += "\"); } int main(void) { return 0; }";
	return build_and_load(source, "-nostdlib -static -Wl,-e,refault_entry", false);
}

static std::vector<uint8_t> build_file_refault_fp_probe(bool store_without_load)
{
	std::string body = "li t0, 0x100000\n";
	if (store_without_load)
		body += "li t1, 0x42434445\n.word 0xf0030553\n"; // fmv.w.x fa0, t1
	else
		body += ".word 0x0002a507\n"; // flw fa0, 0(t0)
	body += ".word 0x00a2a227\n.word 0x7ff00073\n"; // fsw fa0, 4(t0); ecall
	std::string source = "__attribute__((naked)) void refault_entry(void) { __asm__ volatile(\"";
	for (char c : body) source += c == '\n' ? "\\n" : std::string(1, c);
	source += "\"); } int main(void) { return 0; }";
	return build_and_load(source, "-nostdlib -static -Wl,-e,refault_entry", false);
}

static MachineOptions<RISCV64> file_refault_options(bool translated)
{
	MachineOptions<RISCV64> options;
	options.memory_max = 16u << 20;
	options.use_shared_execute_segments = false;
#ifdef RISCV_BINARY_TRANSLATION
	options.translate_enabled = translated;
	options.translation_cache = false;
	options.translate_enable_embedded = false;
#endif
#ifdef RISCV_ASMJIT
	options.asmjit_enabled = translated;
#endif
	(void)translated;
	return options;
}

constexpr bool translated_file_refault_supported =
#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)
	true;
#else
	false;
#endif

TEST_CASE("file backing retention distinguishes unsupported fds from errors",
	"[Memory][madvise-file-backed]")
{
	Machine<RISCV64> machine { empty_binary, {.memory_max = 16u << 20} };
	bool retention_failed = false;
	CHECK(machine.memory.retain_file_mapping(-1, retention_failed) == nullptr);
	CHECK(retention_failed);

	int pipe_fds[2];
	REQUIRE(::pipe(pipe_fds) == 0);
	retention_failed = true;
	CHECK(machine.memory.retain_file_mapping(pipe_fds[0], retention_failed) == nullptr);
	CHECK_FALSE(retention_failed);
	::close(pipe_fds[0]);
	::close(pipe_fds[1]);
}

TEST_CASE("MADV_DONTNEED refaults current file data after the descriptor closes",
	"[Memory][madvise-file-backed]")
{
	TemporaryFile file;
	REQUIRE(file.fd >= 0);
	std::array<uint8_t, Page::size()> original;
	original.fill(0x7f);
	write_file(file.fd, original.data(), original.size());

	Machine<RISCV64> machine { empty_binary, {.memory_max = 16u << 20} };
	map_test_page(machine, file.fd, original.data(), original.size());
	machine.memory.madvise_dontneed(guest_addr, Page::size(), true);

	const uint8_t updated = 0x42;
	REQUIRE(::pwrite(file.fd, &updated, sizeof(updated), 0) == 1);
	::close(file.fd);
	file.fd = -1;
	CHECK(guest_read_byte(machine, guest_addr) == updated);
}

TEST_CASE("file refault does not change the host file position",
	"[Memory][madvise-file-backed]")
{
	TemporaryFile file;
	REQUIRE(file.fd >= 0);
	std::array<uint8_t, Page::size()> original;
	original.fill(0x7f);
	write_file(file.fd, original.data(), original.size());
	Machine<RISCV64> machine { empty_binary, {.memory_max = 16u << 20} };
	map_test_page(machine, file.fd, original.data(), original.size());
	machine.memory.madvise_dontneed(guest_addr, Page::size(), true);
	REQUIRE(::lseek(file.fd, 37, SEEK_SET) == 37);
	CHECK(guest_read_byte(machine, guest_addr) == 0x7f);
	CHECK(::lseek(file.fd, 0, SEEK_CUR) == 37);
}

TEST_CASE("memory buffer and move fallback paths observe a pending file refault",
	"[Memory][madvise-file-backed]")
{
	TemporaryFile file;
	REQUIRE(file.fd >= 0);
	std::array<uint8_t, Page::size()> original;
	original.fill(0x7f);
	write_file(file.fd, original.data(), original.size());
	Machine<RISCV64> machine { empty_binary, {.memory_max = 16u << 20} };
	map_test_page(machine, file.fd, original.data(), original.size());
	machine.memory.madvise_dontneed(guest_addr, Page::size(), true);

	const uint8_t updated = 0x42;
	REQUIRE(::pwrite(file.fd, &updated, sizeof(updated), 0) == 1);
	const auto destination = guest_addr + Page::size();
	if (machine.memory.try_memmove(destination, guest_addr, 1)) {
		CHECK(guest_read_byte(machine, destination) == updated);
	} else {
		uint8_t source_byte = 0;
		machine.memory.memcpy_out(&source_byte, guest_addr, sizeof(source_byte));
		machine.memory.memcpy(destination, &source_byte, sizeof(source_byte));
	}
	CHECK(guest_read_byte(machine, destination) == updated);
	machine.memory.madvise_dontneed(guest_addr, Page::size(), true);
#ifdef RISCV_VIRTUAL_PAGING
	auto buffer = machine.memory.membuffer(guest_addr, 1);
	REQUIRE(buffer.size() == 1);
	CHECK(uint8_t(buffer.data()[0]) == updated);
#else
	uint8_t after = 0;
	machine.memory.memcpy_out(&after, guest_addr, sizeof(after));
	CHECK(after == updated);
#endif
}

TEST_CASE("MADV_DONTNEED discards a dirty private page and keeps split mappings isolated",
	"[Memory][madvise-file-backed]")
{
	TemporaryFile file;
	REQUIRE(file.fd >= 0);
	std::array<uint8_t, Page::size() * 2> original;
	original.fill(0x7f);
	original[Page::size()] = 0x6b;
	write_file(file.fd, original.data(), original.size());

	Machine<RISCV64> machine { empty_binary, {.memory_max = 16u << 20} };
	map_test_page(machine, file.fd, original.data(), original.size());
	// The second page is unmapped/remapped anonymously; its old backing offset
	// must not survive in the file-map registry.
	machine.memory.remove_file_mappings(guest_addr + Page::size(), Page::size());
	guest_write_byte(machine, guest_addr, 0x55);
	machine.memory.madvise_dontneed(guest_addr, original.size(), true);
	CHECK(guest_read_byte(machine, guest_addr) == 0x7f);
	CHECK(guest_read_byte(machine, guest_addr + Page::size()) == 0);
}

TEST_CASE("translated reads refault file data after MADV_DONTNEED",
	"[Memory][madvise-file-backed][translation]")
{
	const auto binary = build_file_refault_probe(false);
	for (bool translated : {false, true}) {
		if (translated && !translated_file_refault_supported) continue;
		TemporaryFile file;
		REQUIRE(file.fd >= 0);
		std::array<uint8_t, Page::size()> original;
		original.fill(0x7f);
		write_file(file.fd, original.data(), original.size());

		Machine<RISCV64> machine(binary, file_refault_options(translated));
		map_test_page(machine, file.fd, original.data(), original.size());
		machine.memory.madvise_dontneed(guest_addr, Page::size(), true);
		const uint8_t updated = 0x42;
		REQUIRE(::pwrite(file.fd, &updated, sizeof(updated), 0) == 1);
		REQUIRE(machine.simulate<false>(10'000));
		CHECK(machine.cpu.reg(REG_ARG0) == updated);
	}
}

#ifdef RISCV_ASMJIT
TEST_CASE("inaccurate JIT reads refault file data after MADV_DONTNEED",
	"[Memory][madvise-file-backed][translation]")
{
	const auto binary = build_file_refault_probe(false);
	TemporaryFile file;
	REQUIRE(file.fd >= 0);
	std::array<uint8_t, Page::size()> original;
	original.fill(0x7f);
	write_file(file.fd, original.data(), original.size());

	Machine<RISCV64> machine(binary, file_refault_options(true));
	map_test_page(machine, file.fd, original.data(), original.size());
	machine.memory.madvise_dontneed(guest_addr, Page::size(), true);
	const uint8_t updated = 0x42;
	REQUIRE(::pwrite(file.fd, &updated, sizeof(updated), 0) == 1);
	machine.cpu.simulate_inaccurate(machine.cpu.registers().pc);
	CHECK(machine.cpu.reg(REG_ARG0) == updated);
}
#endif

TEST_CASE("translated stores refault before modifying file-backed pages",
	"[Memory][madvise-file-backed][translation]")
{
	const auto binary = build_file_refault_probe(true);
	for (bool translated : {false, true}) {
		if (translated && !translated_file_refault_supported) continue;
		TemporaryFile file;
		REQUIRE(file.fd >= 0);
		std::array<uint8_t, Page::size()> original;
		original.fill(0x7f);
		write_file(file.fd, original.data(), original.size());

		Machine<RISCV64> machine(binary, file_refault_options(translated));
		map_test_page(machine, file.fd, original.data(), original.size());
		machine.memory.madvise_dontneed(guest_addr, Page::size(), true);
		const uint8_t updated = 0x42;
		REQUIRE(::pwrite(file.fd, &updated, sizeof(updated), 0) == 1);
		REQUIRE(machine.simulate<false>(10'000));
		CHECK(machine.cpu.reg(REG_ARG0) == 69);
		uint8_t actual = 0;
		machine.memory.memcpy_out(&actual, guest_addr, sizeof(actual));
		CHECK(actual == 69);
	}
}

TEST_CASE("translated floating-point accesses refault file data",
	"[Memory][madvise-file-backed][translation]")
{
	const std::array<uint8_t, 4> updated {0x45, 0x44, 0x43, 0x42};
	for (bool store_without_load : {false, true}) {
		const auto binary = build_file_refault_fp_probe(store_without_load);
		for (bool translated : {false, true}) {
			TemporaryFile file;
			REQUIRE(file.fd >= 0);
			std::array<uint8_t, Page::size()> original;
			original.fill(0x7f);
			write_file(file.fd, original.data(), original.size());

			Machine<RISCV64> machine(binary, file_refault_options(translated));
			map_test_page(machine, file.fd, original.data(), original.size());
			machine.memory.madvise_dontneed(guest_addr, Page::size(), true);
			REQUIRE(::pwrite(file.fd, updated.data(), updated.size(), 0)
				== ssize_t(updated.size()));
			REQUIRE(machine.simulate<false>(10'000));
			for (size_t i = 0; i < updated.size(); i++)
				CHECK(guest_read_byte(machine, guest_addr + 4 + i) == updated[i]);
		}
	}
}

#ifdef RISCV_VIRTUAL_PAGING
TEST_CASE("forked file mappings keep independent pending refault pages",
	"[Memory][madvise-file-backed][fork]")
{
	TemporaryFile file;
	REQUIRE(file.fd >= 0);
	std::array<uint8_t, Page::size()> original;
	original.fill(0x7f);
	write_file(file.fd, original.data(), original.size());

	Machine<RISCV64> parent { empty_binary, {
		.memory_max = 16u << 20, .use_memory_arena = false
	} };
	map_test_page(parent, file.fd, original.data(), original.size());
	parent.memory.madvise_dontneed(guest_addr, Page::size(), true);
	Machine<RISCV64> child { parent, {.use_memory_arena = false} };

	const uint8_t updated = 0x42;
	REQUIRE(::pwrite(file.fd, &updated, sizeof(updated), 0) == 1);
	CHECK(guest_read_byte(child, guest_addr) == updated);
	child.memory.write<uint8_t>(guest_addr, 0x55);
	CHECK(guest_read_byte(parent, guest_addr) == updated);
	CHECK(guest_read_byte(child, guest_addr) == 0x55);
}
#endif

#ifdef RISCV_VIRTUAL_PAGING
TEST_CASE("read-only file pages can be refaulted without a guest write",
	"[Memory][madvise-file-backed][read-only]")
{
	TemporaryFile file;
	REQUIRE(file.fd >= 0);
	std::array<uint8_t, Page::size()> original;
	original.fill(0x7f);
	write_file(file.fd, original.data(), original.size());
	Machine<RISCV64> machine { empty_binary, {
		.memory_max = 16u << 20, .use_memory_arena = false
	} };
	map_test_page(machine, file.fd, original.data(), original.size());
	machine.memory.set_page_attr(guest_addr, Page::size(), {
		.read = true, .write = false, .exec = false
	});
	machine.memory.madvise_dontneed(guest_addr, Page::size(), true);
	CHECK(guest_read_byte(machine, guest_addr) == 0x7f);
}
#endif
