#include <include/libc.hpp>
#include <stdio.h>
#include <assert.h>

static inline
char* int32_to_str(char* b, int val)
{
	// negation
	if (val < 0) { *b++ = '-'; val = -val; }
	// move to end of repr.
	int tmp = val;
	do { ++b; tmp /= 10;  } while (tmp);
	char* end = b;
	*end = '\0';
	// move back and insert as we go
	do {
		*--b = '0' + (val % 10);
		val /= 10;
	} while (val);
	return end;
}

int testval = 0;

extern "C"
__attribute__((constructor))
void test_constructor() {
	static const char hello[] = "Hello, Global Constructor!\n";
	write(STDOUT, hello, sizeof(hello)-1);
	testval = 22;
}

int main(int, char**)
{
	assert(testval == 22);

	int len = printf("Hello RISC-V World!\n");
	assert(len > 0);

	asm("nop");
	return 666;
}
