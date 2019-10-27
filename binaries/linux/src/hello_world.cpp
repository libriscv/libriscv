#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
static inline std::vector<uint8_t> load_file(const std::string&);

int testval = 0;

__attribute__((constructor))
void test_constructor() {
	static const char hello[] = "Hello, Global Constructor!\n";
	printf("%s", hello);
	testval = 22;
}

int main(int argc, char** argv)
{
	printf("Argc: %d  Argv 0: %s\n", argc, argv[0]);
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
	// test fopen, fseek, fread, fclose
	try {
		auto vec = load_file("test.txt");
		assert(vec.empty()); // sadly not implemented these syscalls :(
	}
	catch (std::exception& e) {
		printf("Error: %s\n", e.what());
	}
	return 666;
}

#include <unistd.h>
std::vector<uint8_t> load_file(const std::string& filename)
{
    size_t size = 0;
    FILE* f = fopen(filename.c_str(), "rb");
    if (f == NULL) throw std::runtime_error("Could not open file: " + filename);

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> result(size);
    if (size != fread(result.data(), 1, size, f))
    {
        fclose(f);
        throw std::runtime_error("Error when reading from file: " + filename);
    }
    fclose(f);
    return result;
}
