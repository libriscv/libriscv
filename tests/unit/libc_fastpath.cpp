#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 32ul << 20;
static const uint64_t MAX_INSTRUCTIONS = 200'000'000ul;
using namespace riscv;

/* The guest calls libc through volatile function pointers so that GCC cannot
   expand the calls into inline code: they have to reach the real symbols,
   which are the ones the fast-path replaces. Every check prints a line only
   when it fails, so a passing run produces exactly one line of output. */
static const std::string libc_exerciser = R"M(
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while (0)

static void *(*volatile p_memcpy)(void*, const void*, size_t) = memcpy;
static void *(*volatile p_memset)(void*, int, size_t) = memset;
static void *(*volatile p_memmove)(void*, const void*, size_t) = memmove;
static int   (*volatile p_memcmp)(const void*, const void*, size_t) = memcmp;
static void *(*volatile p_memchr)(const void*, int, size_t) = memchr;
#ifdef __GLIBC__ /* rawmemchr is a glibc-only extension */
static void *(*volatile p_rawmemchr)(const void*, int) = rawmemchr;
#endif
static size_t(*volatile p_strlen)(const char*) = strlen;
static size_t(*volatile p_strnlen)(const char*, size_t) = strnlen;
static int   (*volatile p_strcmp)(const char*, const char*) = strcmp;
static int   (*volatile p_strncmp)(const char*, const char*, size_t) = strncmp;
static char *(*volatile p_strcpy)(char*, const char*) = strcpy;
static char *(*volatile p_stpcpy)(char*, const char*) = stpcpy;
static char *(*volatile p_strncpy)(char*, const char*, size_t) = strncpy;
static char *(*volatile p_strchr)(const char*, int) = strchr;
static char *(*volatile p_strchrnul)(const char*, int) = strchrnul;

#define SZ 40000
static char big1[SZ];
static char big2[SZ];

static int sign(int x) { return (x > 0) - (x < 0); }

int main(void)
{
	char buf[128], buf2[128];
	const char* hello = "Hello, World!";

	/* memcpy */
	p_memset(buf, 0xAB, sizeof(buf));
	CHECK(p_memcpy(buf, hello, 14) == buf);
	CHECK(p_strcmp(buf, hello) == 0);
	CHECK((unsigned char)buf[14] == 0xAB);
	CHECK(p_memcpy(buf, hello, 0) == buf);

	/* memset */
	CHECK(p_memset(buf2, 'x', 10) == buf2);
	CHECK(buf2[0] == 'x' && buf2[9] == 'x');

	/* memmove, overlapping in both directions */
	p_strcpy(buf, "0123456789");
	CHECK(p_memmove(buf + 2, buf, 8) == buf + 2);
	CHECK(p_memcmp(buf, "0101234567", 10) == 0);
	p_strcpy(buf, "0123456789");
	CHECK(p_memmove(buf, buf + 2, 8) == buf);
	CHECK(p_memcmp(buf, "2345678989", 10) == 0);

	/* memcmp */
	CHECK(p_memcmp("abc", "abc", 3) == 0);
	CHECK(sign(p_memcmp("abc", "abd", 3)) < 0);
	CHECK(sign(p_memcmp("abd", "abc", 3)) > 0);
	CHECK(p_memcmp("abc", "xyz", 0) == 0);

	/* memchr and rawmemchr */
	CHECK(p_memchr(hello, 'W', 13) == hello + 7);
	CHECK(p_memchr(hello, 'W', 5) == NULL);
	CHECK(p_memchr(hello, 'Z', 13) == NULL);
	CHECK(p_memchr(hello, 'H', 0) == NULL);
#ifdef __GLIBC__
	CHECK(p_rawmemchr(hello, '!') == hello + 12);
	CHECK(p_rawmemchr(hello, '\0') == hello + 13);
#endif

	/* strlen and strnlen */
	CHECK(p_strlen(hello) == 13);
	CHECK(p_strlen("") == 0);
	CHECK(p_strnlen(hello, 5) == 5);
	CHECK(p_strnlen(hello, 100) == 13);

	/* strcmp and strncmp */
	CHECK(p_strcmp("abc", "abc") == 0);
	CHECK(sign(p_strcmp("abc", "abd")) < 0);
	CHECK(sign(p_strcmp("abcd", "abc")) > 0);
	CHECK(sign(p_strcmp("", "a")) < 0);
	CHECK(p_strncmp("abcXX", "abcYY", 3) == 0);
	CHECK(sign(p_strncmp("abcXX", "abcYY", 4)) < 0);
	CHECK(p_strncmp("abc", "abd", 0) == 0);
	CHECK(p_strncmp("ab", "ab", 10) == 0);

	/* strcpy, stpcpy and strncpy */
	p_memset(buf, 0x7F, sizeof(buf));
	CHECK(p_strcpy(buf, hello) == buf);
	CHECK(p_strlen(buf) == 13);
	CHECK(p_stpcpy(buf, hello) == buf + 13);
	p_memset(buf, 0x7F, sizeof(buf));
	CHECK(p_strncpy(buf, "ab", 8) == buf);
	CHECK(p_memcmp(buf, "ab\0\0\0\0\0\0", 8) == 0);
	CHECK((unsigned char)buf[8] == 0x7F);
	p_memset(buf, 0x7F, sizeof(buf));
	p_strncpy(buf, "abcdefgh", 4);
	CHECK(p_memcmp(buf, "abcd", 4) == 0);
	CHECK((unsigned char)buf[4] == 0x7F);

	/* strchr and strchrnul */
	CHECK(p_strchr(hello, 'W') == hello + 7);
	CHECK(p_strchr(hello, 'Z') == NULL);
	CHECK(p_strchr(hello, '\0') == hello + 13);
	CHECK(p_strchrnul(hello, 'W') == hello + 7);
	CHECK(p_strchrnul(hello, 'Z') == hello + 13);

	/* Buffers that span many pages */
	p_memset(big1, 'A', SZ);
	p_memset(big2, 'A', SZ);
	CHECK(p_memcmp(big1, big2, SZ) == 0);
	big2[SZ - 1] = 'B';
	CHECK(sign(p_memcmp(big1, big2, SZ)) < 0);
	p_memcpy(big2, big1, SZ);
	CHECK(p_memcmp(big1, big2, SZ) == 0);
	big1[SZ - 1] = '\0';
	CHECK(p_strlen(big1) == SZ - 1);
	CHECK(p_memchr(big1, '\0', SZ) == big1 + SZ - 1);
	big1[SZ / 2] = 'Q';
	CHECK(p_strchr(big1, 'Q') == big1 + SZ / 2);
	CHECK(p_strcmp(big1, big1) == 0);
	big2[SZ - 1] = '\0';
	CHECK(sign(p_strcmp(big1, big2)) > 0);
	p_memmove(big1 + 1000, big1, SZ - 1000);
	CHECK(big1[1000 + SZ / 2] == 'Q');

	/* Heap memory, which lives outside the initial ELF image */
	char* h = malloc(70000);
	p_memset(h, 'z', 70000);
	h[69999] = 0;
	CHECK(p_strlen(h) == 69999);
	CHECK(p_memchr(h, 0, 70000) == h + 69999);
	free(h);

	printf("libc: %d failures\n", failures);
	return 0;
})M";

struct State {
	std::string output;
	unsigned    ebreaks = 0;
};

// System call handlers and the printer are plain function pointers, so the
// test state travels through the machine's userdata pointer
static void collect_output(const riscv::Machine<RISCV64>& m, const char* data, size_t size)
{
	m.template get_userdata<State> ()->output.append(data, size);
}

static std::string run_exerciser(const std::vector<uint8_t>& binary, bool fastpath)
{
	State state;
	riscv::Machine<RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY,
		.libc_fastpath = fastpath,
	} };
	machine.set_userdata(&state);
	machine.set_printer(collect_output);
	machine.setup_linux_syscalls();
	machine.setup_linux({"libc_fastpath"}, {"LC_ALL=C"});

	machine.simulate(MAX_INSTRUCTIONS);
	return state.output;
}

TEST_CASE("Hot-patched libc functions behave like the real ones", "[LibcFastpath]")
{
	const auto binary = build_and_load(libc_exerciser);

	// The unpatched run is the reference: it executes the guest's own libc
	const std::string reference = run_exerciser(binary, false);
	REQUIRE(reference == "libc: 0 failures\n");

	const std::string patched = run_exerciser(binary, true);
	REQUIRE(patched == reference);
}

TEST_CASE("Hot-patching leaves guest memory untouched", "[LibcFastpath]")
{
	const auto binary = build_and_load(libc_exerciser);

	riscv::Machine<RISCV64> plain { binary, { .memory_max = MAX_MEMORY } };
	riscv::Machine<RISCV64> patched { binary, {
		.memory_max = MAX_MEMORY,
		.libc_fastpath = true,
	} };

	const auto memcpy_addr = plain.address_of("memcpy");
	REQUIRE(memcpy_addr != 0x0);

	// Only the decoder cache is rewritten, so the machine code a debugger,
	// a backtrace or the precise simulator would read is still the original
	std::array<uint8_t, 32> a, b;
	plain.copy_from_guest(a.data(), memcpy_addr, a.size());
	patched.copy_from_guest(b.data(), memcpy_addr, b.size());
	REQUIRE(a == b);
}

TEST_CASE("Hot-patching does not break real breakpoints", "[LibcFastpath]")
{
	const auto binary = build_and_load(libc_exerciser);

	State state;
	riscv::Machine<RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY,
		.ebreak_locations = {std::string("main")},
		.libc_fastpath = true,
	} };
	machine.set_userdata(&state);
	machine.set_printer(collect_output);
	machine.setup_linux_syscalls();
	machine.setup_linux({"libc_fastpath"}, {"LC_ALL=C"});

	// setup_linux_syscalls() installs its own EBREAK handler *after* the
	// fast-path took over EBREAK, and must end up in the chain rather than
	// replacing it. Installing one here has to work the same way.
	machine.install_syscall_handler(riscv::SYSCALL_EBREAK,
		[] (riscv::Machine<RISCV64>& m) {
			m.template get_userdata<State> ()->ebreaks ++;
			// Resume at the (unmodified) instruction the breakpoint replaced
			m.cpu.simulate_precise();
		});

	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(state.ebreaks == 1);
	REQUIRE(state.output == "libc: 0 failures\n");
}
