#include <stdio.h>
// A macro that creates a varadic callable host function
// Drawback: Floats are promoted to doubles
#define STR(x) #x
#define CREATE_HOST_FUNCTION(INDEX, ...) \
	__asm__(".pushsection .text\n"	\
		".global call_host_function" #INDEX "\n"	\
		"call_host_function" #INDEX ":\n"	\
		"    .insn i 0b1011011, 0, x0, x0, " STR(INDEX) "\n"	\
		"    ret\n"	\
		".popsection\n");	\
	extern long call_host_function ## INDEX(__VA_ARGS__);

// Create three host functions indexed 0, 1, and 2
struct Strings {
	unsigned long count;
	const char *strings[32];
};
CREATE_HOST_FUNCTION(0, struct Strings*);
struct Buffer {
	unsigned long count;
	char buffer[256];
	unsigned long another_count;
	char *another_buffer;
};
CREATE_HOST_FUNCTION(1, struct Buffer*);
typedef void (*HostFunction)(const char*);
CREATE_HOST_FUNCTION(2, HostFunction);

static void my_function(const char *str)
{
	printf("Host says: %s\n", str);
	fflush(stdout);
}

int main()
{
	printf("Hello, Micro RISC-V World!\n");

	// Call the host function that prints strings
	struct Strings vec = {2, {"Hello", "World"}};
	call_host_function0(&vec);

	// Call the host function that modifies a buffer
	struct Buffer buf;
	char another_buf[256];
	buf.another_count = sizeof(another_buf);
	buf.another_buffer = another_buf;
	call_host_function1(&buf);
	printf("Buffer: %s\n", buf.buffer);
	printf("Another Buffer: %s\n", buf.another_buffer);

	// Call a host function that takes a function pointer
	call_host_function2(&my_function);
}
