#include <include/libc.hpp>
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

int testval = 0;

extern "C"
__attribute__((constructor))
void test_constructor() {
	static const char hello[] = "Hello, Global Constructor!\n";
	write(STDOUT_FILENO, hello, sizeof(hello)-1);
	testval = 22;
}

int main(int, char**)
{
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
	return 666;
}

// this function can be called using Machine::vmcall()
extern "C" void public_function()
{
	printf("Test!!\n");
}
