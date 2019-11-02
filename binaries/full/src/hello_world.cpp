#include <cstdio>
#include <thread>
static void*
thread_start(void* userdata)
{
	return nullptr;
}

int main (int argc, char *argv[], char *envp[])
{
	auto t = std::thread(thread_start, nullptr);
	printf("Test\n");
	return 666;
}
