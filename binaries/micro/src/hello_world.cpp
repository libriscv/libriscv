#include "syscall.hpp"

extern "C"
__attribute__((constructor))
void test_constructor() {
	// static storage const string
	static const char hello[] = "Hello, Global Constructor!\n";
	write(0, hello, sizeof(hello)-1);
}

int main(int, char**)
{
	// NOTE: can't use automatic storage here, because we would need memcpy()
	static const char hello[] = "Hello Micro World!\n";
	write(0, hello, sizeof(hello)-1);
	return 666;
}
