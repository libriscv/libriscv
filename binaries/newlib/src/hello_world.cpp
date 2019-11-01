#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <stdexcept>

int main (int argc, char *argv[])
{
	for (int i = 0; i < argc; i++) {
		printf("arg%d: %s\n", i, argv[0]);
	}

	for (int i = 0; i < 1000; i++)
	{
		try {
			throw std::runtime_error("Oh god!");
		}
		catch (std::exception& e) {
			//printf("Error: %s\n", e.what());
			write(5, e.what(), strlen(e.what()));
		}
		const char hello_void[] = "Hello void\n";
		write(5, hello_void, sizeof(hello_void));
	}
	return 666;
}
