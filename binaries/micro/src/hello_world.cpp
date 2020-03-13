#include "syscall.hpp"

extern "C"
__attribute__((constructor))
void test_constructor() {
	// static storage const string
	static const char hello[] = "Hello, Global Constructor!\n";
	write(0, hello, sizeof(hello)-1);
}

int main(int, char** argv)
{
	// calculate length of argv[0]
	int alen = 0; while(argv[0][alen] != 0) alen++;
	// NOTE: can't use automatic storage here, because we would need memcpy()
	static const char hello[] = "Hello World from ";
	write(0, hello, sizeof(hello)-1);
	write(0, argv[0], alen);
	write(0, "!\n", 2);
	return 666;
}
