#include "syscall.hpp"
// static storage writable int
int testval = 0;

extern "C"
__attribute__((constructor))
void test_constructor() {
	// static storage const string
	static const char hello[] = "Hello, Global Constructor!\n";
	write(0, hello, sizeof(hello)-1);
	testval = 22;
}

int main(int, char**)
{
	if (testval != 22) return -1;
	// automatic storage const string
	const char hello[] = "Hello Micro World!\n";
	write(0, hello, sizeof(hello)-1);
	return 666;
}
