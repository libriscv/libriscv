#include <cstdio>
#include <thread>
static void*
thread_start(void* userdata)
{
	return nullptr;
}

int main (int argc, char *argv[], char *envp[])
{
	//auto t = std::thread(thread_start, nullptr);
	printf("Test\n");
	for (char** env = envp; *env != 0; env++) {
		printf("Env: %s\n", *env);
	}
	return 666;
}
