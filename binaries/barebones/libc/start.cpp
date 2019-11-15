#include <include/syscall.hpp>
#include <tinyprintf.h>
#include <stdint.h>
int __testable_global __attribute__((section(".bss"))) = 0;

extern "C" {
	__attribute__((noreturn))
	void _exit(int exitval) {
		syscall(SYSCALL_EXIT, exitval);
		__builtin_unreachable();
	}
	void __init_heap(uintptr_t free_begin, uintptr_t heapmax);
	void __print_putchr(void* file, char c);
}

static void
init_stdlib()
{
	// 1. enable printf facilities
	init_printf(NULL, __print_putchr);

	// 2. initialize heap (malloc, etc.)
	__init_heap(0xC0000000, 0xF0000000);

#ifdef EH_ENABLED
	/// 3. initialize exceptions before we run constructors
    extern char __eh_frame_start[];
    extern void __register_frame(void*);
  	__register_frame(&__eh_frame_start);
#endif

	// 4. call global C/C++ constructors
	extern void(*__init_array_start [])();
	extern void(*__init_array_end [])();
	int count = __init_array_end - __init_array_start;
	for (int i = 0; i < count; i++) {
		__init_array_start[i]();
	}
}

extern "C"
void _start()
{
	// initialize the global pointer to __global_pointer
	// NOTE: have to disable relaxing first
	asm volatile
	("   .option push 				\t\n\
		 .option norelax 			\t\n\
		 1:auipc gp, %pcrel_hi(__global_pointer$) \t\n\
  		 addi  gp, gp, %pcrel_lo(1b) \t\n\
		.option pop					\t\n\
	");
	asm volatile("" ::: "memory");
	// testable global
	__testable_global = 1;
	// zero-initialize .bss section
	extern char __bss_start;
	extern char __BSS_END__;
	for (char* bss = &__bss_start; bss < &__BSS_END__; bss++) {
		*bss = 0;
	}
	asm volatile("" ::: "memory");
	// exit out if the .bss section did not get initialized
	if (__testable_global != 0) {
		_exit(-1);
	}

	init_stdlib();

	// call main() :)
	extern int main(int, char**);
	_exit(main(0, nullptr));
}
