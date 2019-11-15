#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
static inline std::vector<uint8_t> load_file(const std::string&);
static void test_rtti();

int testval = 0;

__attribute__((constructor))
void test_constructor() {
	static const char hello[] = "Hello, Global Constructor!\n";
	printf("%s", hello);
	testval = 22;
}

#include <exception>
class IdioticException : public std::exception
{
    const char* oh_god;
public:
	IdioticException(const char* reason) : oh_god(reason) {}
    const char* what() const noexcept override
    {
        return oh_god;
    }
};

int main (int argc, char *argv[], char *envp[])
{
	// heap test
	auto b = std::unique_ptr<std::string> (new std::string(""));
	assert(b != nullptr);
	// copy into string
	static const char* hello = "Hello %s World v%d.%d!\n";
	*b = hello;
	assert(*b == hello);
	// va_list & stdarg test
	int len = printf(b->c_str(), "RISC-V", 1, 0);
	assert(len > 0);
	printf("* printf(), stdarg and va_lists seem to be working!\n");
	// global constructors
	assert(testval == 22);
	printf("* Global ctors seem to be working!\n");
	// auxvec, arguments to main():
	assert(argc > 0 && std::string("hello_world") == argv[0]);
	for (int i = 0; i < argc; i++) {
		printf("arg%d: %s\n", i, argv[i]);
	}
	printf("* Arguments seem to be working!\n");
	// environ tests
	assert(*envp != nullptr);
	for (char** env = envp; *env != 0; env++) {
		printf("env: %s\n", *env);
	}
	printf("* Environment variables seem to be working!\n");
	// C++ tests
	test_rtti();
	printf("* C++ RTTI seems to be working!\n");
	// unfortunately, exceptions are not initialized (probably no unwinder also)
	// so this throw will just call abort()
	try {
		throw IdioticException("Oh god!");
		assert(0 && "Exception was not thrown!");
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

struct A {
	static int A_called;
	static int B_called;
	virtual void f() { A_called++; }
};
struct B : public A {
	void f() override { B_called++; }
};
int A::A_called = 0, A::B_called = 0;

void test_rtti()
{
	A a;
	B b;
	a.f();        // A::f()
	b.f();        // B::f()

	A *pA = &a;
	A *pB = &b;
	pA->f();      // A::f()
	pB->f();      // B::f()

	pA = &b;
	// pB = &a;      // not allowed
	pB = dynamic_cast<B*>(&a); // allowed but it returns NULL
	assert(pB == nullptr);
	assert(A::A_called == 2 && A::B_called == 2);
}
