#include <cstdio>

extern void test_custom_machine();
extern void test_crashes();
extern void test_rv32i();
extern void test_rv32c();

int main()
{
	test_custom_machine();

	test_crashes();
	test_rv32i();
	test_rv32c();
	printf("Tests passed!\n");
	return 0;
}
