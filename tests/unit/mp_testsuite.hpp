#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
static constexpr size_t MP_WORKERS = 4;
static constexpr size_t MP_STACK_SIZE = 512 * 1024u;

template <size_t SIZE>
struct MultiprocessWork {
	unsigned workers = 1;
	std::array<float, SIZE> data_a;
	std::array<float, SIZE> data_b;
	float result[16] = {0};
	int counter = 0;

	inline size_t work_size() const noexcept {
		return SIZE / workers;
	}
	float final_sum() const noexcept {
		float sum = 0.0f;
		for (size_t i = 0; i < this->workers; i++) sum += this->result[i];
		return sum;
	}
};
static constexpr size_t WORK_SIZE = 16384;
static MultiprocessWork<WORK_SIZE> mp_work;

template <size_t SIZE>
static void initialize_work(MultiprocessWork<SIZE>& work) {
	for (size_t i = 0; i < SIZE; i++) {
		work.data_a[i] = 1.0;
		work.data_b[i] = 1.0;
	}
}

template <size_t SIZE>
static void multiprocessing_function(int cpu, void* vdata)
{
	auto& work = *(MultiprocessWork<SIZE> *)vdata;
	const size_t start = (cpu + 0) * work.work_size();
	const size_t end   = (cpu + 1) * work.work_size();

	float sum = 0.0f;
	for (size_t i = start; i < end; i++) {
		sum += work.data_a[i] * work.data_b[i];
	}

	work.result[cpu] = sum;
	__sync_fetch_and_add(&work.counter, 1);
}

static void multiprocessing_forever(int, void*)
{
	while (1);
}

using multiprocess_func_t = void(*)(int, void*);

extern "C" long sys_multiprocess(unsigned, void*, size_t, multiprocess_func_t, void*);
extern "C" long sys_multiprocess_fork(unsigned);
extern "C" long sys_multiprocess_wait();

static struct {
	uint64_t* stacks = nullptr;
	unsigned  cpus = 0;
} mp_data;
inline unsigned multiprocess(unsigned cpus, multiprocess_func_t func, void* data)
{
	if (mp_data.cpus < cpus) {
		mp_data.cpus = cpus;
		delete[] mp_data.stacks;
		mp_data.stacks = new uint64_t[(MP_STACK_SIZE * cpus) / sizeof(uint64_t)];
	}

	return sys_multiprocess(cpus, mp_data.stacks, MP_STACK_SIZE, func, data);
}
__attribute__((always_inline))
inline unsigned multiprocess(unsigned cpus)
{
	register unsigned a0 asm("a0") = cpus;
	register int     sid asm("a7") = 1; // multiprocess_fork

	asm volatile ("ecall" : "+r"(a0) : "r"(sid));
	return a0;
}
inline long multiprocess_wait()
{
	return sys_multiprocess_wait();
}

asm(".global sys_multiprocess\n"
"sys_multiprocess:\n"
"	li a7, 0\n"
"	ecall\n"
"   beqz a0, sys_multiprocess_ret\n" // Early return for vCPU 0
// Otherwise, create a function call
"   addi a0, a0, -1\n" // Subtract 1 from vCPU ID, making it 0..N-1
"   mv a1, a4\n"       // Move work data to argument 1
"   jalr zero, a3\n"   // Direct jump to work function
"sys_multiprocess_ret:\n"
"   ret\n");           // Return to caller

asm(".global sys_multiprocess_wait\n"
"sys_multiprocess_wait:\n"
"	li a7, 2\n"
"	ecall\n"
"   ret\n");
