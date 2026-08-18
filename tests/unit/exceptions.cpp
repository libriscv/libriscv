#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_INSTRUCTIONS = 50'000'000ul;
using namespace riscv;

static const int HEAP_SYSCALLS_BASE	  = 470;
static const int MEMORY_SYSCALLS_BASE = 475;
static const int THREADS_SYSCALL_BASE = 490;

template <int W>
static void setup_native_system_calls(riscv::Machine<W>& machine, size_t heap_size = 16UL << 20)
{
	// Syscall-backed heap
	auto heap = machine.memory.mmap_allocate(heap_size);

	machine.setup_native_heap(HEAP_SYSCALLS_BASE, heap, heap_size);
	machine.setup_native_memory(MEMORY_SYSCALLS_BASE);
	machine.setup_native_threads(THREADS_SYSCALL_BASE);
}

// Collects everything the guest writes to stdout/stderr. Note that libriscv
// also reports unhandled system calls through the printer, so tests count
// occurrences instead of comparing the whole stream.
struct Output {
	std::string text;

	size_t count(const std::string& needle) const {
		size_t n = 0;
		for (size_t pos = text.find(needle); pos != std::string::npos;
			 pos = text.find(needle, pos + needle.size())) n++;
		return n;
	}

	template <int W>
	void install(riscv::Machine<W>& machine) {
		machine.set_userdata(this);
		machine.set_printer([] (const auto& m, const char* data, size_t size) {
			m.template get_userdata<Output> ()->text.append(data, data + size);
		});
	}
};

// Replaces malloc/calloc/realloc/free with the arena-backed system calls, the
// way sandboxed guest programs do. musl's own allocator is never pulled out of
// the archive, because these symbols are already defined.
//
// aligned_alloc() *must* be overridden along with them: musl implements it on
// top of the internal __libc_malloc_impl() rather than the public malloc()
// symbol, so a guest that only replaces malloc/free ends up allocating
// exceptions from mallocng and releasing them into the arena.
// __cxa_allocate_exception() reaches the allocator through aligned_alloc().
static const std::string NATIVE_HEAP_PROLOGUE = R"M(
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)
#define CREATE_SYSCALL(name, syscall_id) \
	__asm__(".pushsection .text\n" \
			".global " #name "\n" \
			".type " #name ", @function\n" \
			"" #name ":\n" \
			"	li a7, " STRINGIFY(syscall_id) "\n" \
			"	ecall\n" \
			"	ret\n" \
			".popsection")

CREATE_SYSCALL(malloc,  470);
CREATE_SYSCALL(calloc,  471);
CREATE_SYSCALL(realloc, 472);
CREATE_SYSCALL(free,    473);

extern "C" void *memalign(size_t alignment, size_t size) {
	if (alignment <= 16)
		return malloc(size);
	void* list[16];
	size_t i = 0;
	void* result = nullptr;
	for (i = 0; i < 16; i++) {
		result = malloc(size);
		list[i] = result;
		if (result == nullptr) break;
		if (((uintptr_t)result % alignment) == 0) break;
		free(result);
		list[i] = malloc(16);
	}
	for (size_t j = 0; j < i; j++) free(list[j]);
	return result;
}
extern "C" void *aligned_alloc(size_t alignment, size_t size) {
	return memalign(alignment, size);
}
extern "C" int posix_memalign(void **memptr, size_t alignment, size_t size) {
	void* result = memalign(alignment, size);
	if (result == nullptr) return 1;
	*memptr = result;
	return 0;
}
void *operator new(size_t size) { return malloc(size); }
void *operator new[](size_t size) { return malloc(size); }
void operator delete(void *ptr) noexcept { free(ptr); }
void operator delete[](void *ptr) noexcept { free(ptr); }
void operator delete(void *ptr, size_t) noexcept { free(ptr); }
void operator delete[](void *ptr, size_t) noexcept { free(ptr); }
)M";

// The smallest possible program that throws and catches a C++ exception.
// Unwinding has to walk out of thrower() and find the landing pad, which
// requires locating PT_GNU_EH_FRAME. LLVM libunwind does that through
// dl_iterate_phdr(), and musl answers it by re-reading the auxiliary vector
// on the stack -- so the auxiliary vector has to survive for the lifetime of
// the program. (libgcc is unaffected: static GNU binaries register .eh_frame
// up front via __register_frame_info in crtbegin.)
static const std::string THROW_CATCH_PROGRAM = R"M(
#include <cstdio>
#include <stdexcept>

__attribute__((noinline))
static void thrower(int value)
{
	if (value > 0)
		throw std::runtime_error("thrown");
}

extern "C" __attribute__((used, retain))
int throw_and_catch(int value)
{
	try {
		thrower(value);
		return -1;
	} catch (const std::exception& e) {
		printf("caught %s\n", e.what());
		fflush(stdout);
		return 666;
	}
}

int main()
{
	printf("main %d\n", throw_and_catch(1));
	fflush(stdout);
	// Stop without running libc teardown, the way sandboxed programs do,
	// so that vmcalls can be made afterwards.
	__asm__(".insn i SYSTEM, 0, x0, x0, 0x7ff");
	return 0;
}
)M";

TEST_CASE("Throw and catch a C++ exception in main()", "[Exceptions]")
{
	const auto binary = build_and_load(THROW_CATCH_PROGRAM, "-O2 -static", true);

	riscv::Machine<RISCV64> machine { binary };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"exceptions"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	Output output;
	output.install(machine);

	REQUIRE_NOTHROW(machine.simulate(MAX_INSTRUCTIONS));

	REQUIRE(output.count("caught thrown\nmain 666\n") == 1);
}

TEST_CASE("Throw and catch a C++ exception in a vmcall", "[Exceptions]")
{
	// vmcall() resets the stack pointer, which must not place the guest stack
	// above argc/argv/envp and the auxiliary vector: growing down through them
	// destroys the ELF program headers that the unwinder needs.
	const auto binary = build_and_load(THROW_CATCH_PROGRAM, "-O2 -static", true);

	riscv::Machine<RISCV64> machine { binary };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"exceptions"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	Output output;
	output.install(machine);

	// Run main() first, so that libc is fully initialized
	REQUIRE_NOTHROW(machine.simulate(MAX_INSTRUCTIONS));
	output.text.clear();

	const auto func = machine.address_of("throw_and_catch");
	REQUIRE(func != 0x0);

	// Repeatedly, because a failed unwind terminates into the guests
	// terminate handler, which throws again -- and each round leaks an
	// exception allocation until the heap is exhausted.
	for (size_t i = 0; i < 16; i++)
	{
		REQUIRE_NOTHROW(machine.vmcall<MAX_INSTRUCTIONS>(func, 1));
		REQUIRE(machine.return_value() == 666);
	}
	REQUIRE(output.count("caught thrown\n") == 16);
}

TEST_CASE("Throw and catch a C++ exception using the native heap", "[Exceptions]")
{
	// Same as above, but every allocation goes through the arena, which is how
	// sandboxes run guest programs. Each vmcall must return the arena to the
	// same state, or a long-running sandbox eventually runs out of memory.
	const auto binary = build_and_load(NATIVE_HEAP_PROLOGUE + THROW_CATCH_PROGRAM,
		"-O2 -static", true);

	riscv::Machine<RISCV64> machine { binary };
	setup_native_system_calls(machine);
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"exceptions"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	Output output;
	output.install(machine);

	REQUIRE_NOTHROW(machine.simulate(MAX_INSTRUCTIONS));
	output.text.clear();

	const auto func = machine.address_of("throw_and_catch");
	REQUIRE(func != 0x0);

	// One warm-up call, then the arena must stop growing
	REQUIRE_NOTHROW(machine.vmcall<MAX_INSTRUCTIONS>(func, 1));
	const size_t used_after_first = machine.arena().bytes_used();

	for (size_t i = 0; i < 16; i++)
	{
		REQUIRE_NOTHROW(machine.vmcall<MAX_INSTRUCTIONS>(func, 1));
		REQUIRE(machine.return_value() == 666);
		REQUIRE(machine.arena().bytes_used() == used_after_first);
	}
	REQUIRE(output.count("caught thrown\n") == 17);
}
