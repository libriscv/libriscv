#include <include/libc.hpp>
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

#include <microthread.hpp>

int testval = 0;

extern "C"
__attribute__((constructor))
void test_constructor() {
	static const char hello[] = "Hello, Global Constructor!\n";
	write(STDOUT_FILENO, hello, sizeof(hello)-1);
	testval = 22;
}

int main(int argc, char** argv)
{
	printf("Arguments: %d\n", argc);
	for (int i = 0; i < argc; i++) {
		printf("Arg %d: %s\n", i, argv[i]);
	}
	printf("Note: If you see only garbage here, activate the native-heap "
			"system calls in the emulator.\n");
	static const char* hello = "Hello %s World v%d.%d!\n";
	assert(testval == 22);
	// heap test
	auto b = std::unique_ptr<std::string> (new std::string(""));
	assert(b != nullptr);
	// copy into string
	*b = hello;
	// va_list & stdarg test
	int len = printf(b->c_str(), "RISC-V", 1, 0);
	assert(len > 0);

	auto* thread = microthread::create(
		[] (int a, int b, int c) -> long {
			printf("Hello from microthread!\n"
					"a = %d, b = %d, c = %d\n",
					a, b, c);
			long rv = microthread::join(microthread::create([] () -> long {
				printf("Recursive thread!\n");
				microthread::exit(222);
			}));
			return rv;
		}, 111, 222, 333);
	long retval = microthread::join(thread);
	printf("microthread returned %ld\n", retval);

	return 666;
}

// this function can be called using Machine::vmcall()
extern "C" void public_function()
{
	printf("Test!!\n");
}
