#include <cstdio>
#include <stdexcept>

int main (int argc, char *argv[])
{
	printf("arg0: %s\n", argv[0]);
	printf("arg1: %s\n", argv[1]);

	try {
		throw std::runtime_error("Oh god!");
	}
	catch (std::exception& e) {
		printf("Error: %s\n", e.what());
	}
	printf("Hello world\n");
	return 666;
}
