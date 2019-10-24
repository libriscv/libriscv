#include <include/libc.hpp>

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

int main(int, char**)
{
	const char hello_str[] = "Just before printf\n";
	const int hello_str_len = sizeof(hello_str)-1;
	write(STDOUT, hello_str, hello_str_len);

	int bytes = printf("Hello RISC-V World!\n");
	return bytes;

	const char write_str[] = "write: ";
	const int write_str_len = sizeof(write_str)-1;
	char* buffer = (char*) malloc(512 * 1024);
	memcpy(buffer, write_str, write_str_len);

	char* bend = int32_to_str(&buffer[write_str_len], bytes);
	bend[0] = '\n';
	write(STDOUT, buffer, bend - buffer + 1);

	free(buffer);
	asm("nop");
	return 666;
}
