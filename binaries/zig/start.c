__attribute__((noreturn)) void _exit(int exitval);

__attribute__((visibility("hidden"), used))
void libc_start(int argc, char** argv)
{
	// zero-initialize .bss section
	extern char __bss_start;
	extern char _end;
	for (char* bss = &__bss_start; bss < &_end; bss++) {
		*bss = 0;
	}
	asm volatile("" ::: "memory");

	// call global constructors
	extern void(*__init_array_start [])();
	extern void(*__init_array_end [])();
	int count = __init_array_end - __init_array_start;
	for (int i = 0; i < count; i++) {
		__init_array_start[i]();
	}

	// call main() :)
	extern int main(int, char**);
	_exit(main(argc, argv));
}

// 1. wrangle with argc and argc
// 2. initialize the global pointer to __global_pointer
// NOTE: have to disable relaxing first
asm
("   .global _start             \t\n\
_start:                         \t\n\
     lw   a0, 0(sp) 			\t\n\
	 addi a1, sp, 4		 		\t\n\
	 andi sp, sp, -16 /* not needed */\t\n\
     .option push 				\t\n\
	 .option norelax 			\t\n\
	 1:auipc gp, %pcrel_hi(__global_pointer$) \t\n\
	 addi  gp, gp, %pcrel_lo(1b) \t\n\
	.option pop					\t\n\
	call libc_start				\t\n\
");
