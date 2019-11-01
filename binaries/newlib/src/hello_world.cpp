#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <stdexcept>

int main (int argc, char *argv[])
{
	for (int i = 0; i < argc; i++) {
		printf("arg%d: %s\n", i, argv[0]);
	}

	auto* ptr = new char[1024*1024];

	for (int i = 0; i < 1000; i++)
	{
		try {
			throw std::runtime_error("Oh god!");
		}
		catch (std::exception& e) {
			//printf("Error: %s\n", e.what());
			write(5, e.what(), strlen(e.what()));
		}
	}
	const char hello_void[] = "Hello Virtual World!\n";
	write(0, hello_void, sizeof(hello_void)-1);
	return 666;
}
