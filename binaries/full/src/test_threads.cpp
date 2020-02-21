#include <cassert>
#include <cstdio>
#include <pthread.h>
#include <sys/types.h>
#include <stdexcept>
#include <thread> // C++ threads

struct testdata
{
  int depth     = 0;
  const int max_depth = 20;
};

extern "C" {
  static void* thread_function1(void* data)
  {
    printf("Inside thread function1, x = %d\n", *(int*) data);
    thread_local int test = 2019;
    printf("test @ %p, test = %d\n", &test, test);
    assert(test == 2019);
  }
  static void* thread_function2(void* data)
  {
    printf("Inside thread function2, x = %d\n", *(int*) data);
    thread_local int test = 2020;

    printf("Locking already locked mutex now\n");
    auto* mtx = (pthread_mutex_t*) data;
    const int res = pthread_mutex_lock(mtx);
    printf("Locking returned %d\n", res);

    printf("Yielding from thread2, expecting to be returned to main thread\n");
    sched_yield();
    printf("Returned to thread2, expecting to exit to after main thread yield\n");

    pthread_exit(NULL);
  }
  static void* recursive_function(void* tdata)
  {
    auto* data = (testdata*) tdata;
    data->depth++;
    printf("%ld: Thread depth %d / %d\n",
          pthread_self(), data->depth, data->max_depth);

    if (data->depth < data->max_depth)
    {
      pthread_t t;
      int res = pthread_create(&t, NULL, recursive_function, data);
      if (res < 0) {
        printf("Failed to create thread!\n");
        return NULL;
      }
    }
    printf("%ld: Thread yielding %d / %d\n",
           pthread_self(), data->depth, data->max_depth);
    sched_yield();

    printf("%ld: Thread exiting %d / %d\n",
           pthread_self(), data->depth, data->max_depth);
    data->depth--;
    return NULL;
  }
}

void test_threads()
{
	int x = 666;
	pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
	pthread_t t;
	int res;

	printf("*** Testing pthread_create and sched_yield...\n");
	res = pthread_create(&t, NULL, thread_function1, &x);
	if (res < 0) {
		printf("Failed to create thread!\n");
		return;
	}
	pthread_join(t, NULL);

	pthread_mutex_lock(&mtx);
	res = pthread_create(&t, NULL, thread_function2, &mtx);
	if (res < 0) {
		printf("Failed to create thread!\n");
		return;
	}
	pthread_mutex_unlock(&mtx);

	printf("Yielding from main thread, expecting to return to thread2\n");
	// return back to finish thread2
	sched_yield();
	printf("After yielding from main thread, looking good!\n");

	printf("*** Now testing recursive threads...\n");
	static testdata rdata;
	recursive_function(&rdata);
	// now we have to yield until all the detached children also exit
	printf("*** Yielding until all children are dead!\n");
	while (rdata.depth > 0) sched_yield();

	// remove the last thread?
	pthread_join(t, NULL);

    auto* cpp_thread = new std::thread(
        [] (int a, long long b, std::string c) -> void {
            printf("Hello from a C++ thread\n");
            assert(a == 1);
            assert(b == 2LL);
            assert(c == std::string("test"));
			std::this_thread::yield();
            printf("C++ thread arguments are OK, returning...\n");
        },
        1, 2L, std::string("test")
    );
	printf("Returned. Yielding back...\n");
	std::this_thread::yield();
    printf("Returned. Joining the C++ thread\n");
    cpp_thread->join();
	printf("Deleting the C++ thread\n");
    delete cpp_thread;

	printf("SUCCESS\n");
}
