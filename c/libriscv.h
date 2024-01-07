#ifndef LIBRISCV_H
#define LIBRISCV_H

#ifndef LIBRISCVAPI
#define LIBRISCVAPI
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct RISCVMachine;
typedef struct RISCVMachine RISCVMachine;

#define RISCV_ERROR_TYPE_GENERAL_EXCEPTION  -1
#define RISCV_ERROR_TYPE_MACHINE_EXCEPTION  -2
#define RISCV_ERROR_TYPE_MACHINE_TIMEOUT    -3
typedef void (*riscv_error_func_t)(void *opaque, int type, const char *msg, long data);

typedef void (*riscv_stdout_func_t)(void *opaque, const char *msg, unsigned size);

typedef struct {
	uint64_t max_memory;
	uint32_t stack_size;
	int      strict_sandbox;  /* No file or socket permissions */
	unsigned     argc;        /* Program arguments */
	const char **argv;
	riscv_error_func_t error; /* Error callback */
	riscv_stdout_func_t stdout; /* Stdout callback */
	void *opaque;             /* User-provided pointer */
} RISCVOptions;

/* Fill out default values. */
LIBRISCVAPI void libriscv_set_defaults(RISCVOptions *options);

/* Create a new 64-bit RISC-V machine from an ELF binary. The binary must out-live the machine. */
LIBRISCVAPI RISCVMachine *libriscv_new(const void *elf_prog, unsigned elf_size, RISCVOptions *o);

/* Free a RISC-V machine created using libriscv_new. */
LIBRISCVAPI int libriscv_delete(RISCVMachine *m);


/* Start execution at current PC, with the given instruction limit. 0 on success.
   When an error occurs, the negative value is one of the RISCV_ERROR_ enum values. */
LIBRISCVAPI int libriscv_run(RISCVMachine *m, uint64_t instruction_limit);

/* Returns a string describing a negative return value. */
LIBRISCVAPI const char * libriscv_strerror(int return_value);

/* Return current value of the return value register A0. */
LIBRISCVAPI int64_t libriscv_return_value(RISCVMachine *m);

/* Return symbol address or NULL if not found. */
LIBRISCVAPI uint64_t libriscv_address_of(RISCVMachine *m, const char *name);

/* Return the opaque value provided during machine creation. */
LIBRISCVAPI void * libriscv_opaque(RISCVMachine *m);

/*** View and modify the RISC-V emulator state ***/

typedef union {
	float   f32[2];
	double  f64;
} RISCVFloat;

typedef struct {
	uint64_t  pc;
	uint64_t  r[32];
	uint32_t  fcsr;
	RISCVFloat fr[32];
} RISCVRegisters;

/* Retrieve the internal registers of the RISC-V machine. Changing PC is dangerous. */
LIBRISCVAPI RISCVRegisters * libriscv_get_registers(RISCVMachine *m);

/* Change the PC register safely. PC can be changed before running and during system calls. */
LIBRISCVAPI int libriscv_jump(RISCVMachine *m, uint64_t address);

/* Copy memory in and out of the RISC-V machine. */
LIBRISCVAPI int libriscv_copy_to_guest(RISCVMachine *m, uint64_t dst, const void *src, unsigned len);
LIBRISCVAPI int libriscv_copy_from_guest(RISCVMachine *m, void *dst, uint64_t src, unsigned len);

/* Read a zero-terminated string from memory into a heap-allocated string of at most maxlen length.
   On success, set *length and return a pointer to the new string. Otherwise, return null. */
LIBRISCVAPI char * libriscv_memstring(RISCVMachine *m, uint64_t src, unsigned maxlen, unsigned *length);

/* View a slice of readable memory from src to src + length.
   On success, return a pointer to the memory. Otherwise, return null. */
LIBRISCVAPI const char * libriscv_memview(RISCVMachine *m, uint64_t src, unsigned length);

/* Stops execution normally. Only possible from a system call and EBREAK. */
LIBRISCVAPI void libriscv_stop(RISCVMachine *m);

/* Return current instruction counter value. */
LIBRISCVAPI uint64_t libriscv_instruction_counter(RISCVMachine *m);

/* Return a *pointer* to the instruction max counter. */
LIBRISCVAPI uint64_t * libriscv_max_counter_pointer(RISCVMachine *m);

/*** RISC-V system call handling ***/

typedef void (*riscv_syscall_handler_t)(RISCVMachine *m);

/* Install a custom system call handler. */
LIBRISCVAPI int libriscv_set_syscall_handler(unsigned num, riscv_syscall_handler_t);

/* Triggers a CPU exception. Only safe to call from a system call. Will end execution. */
LIBRISCVAPI void libriscv_trigger_exception(RISCVMachine *m, unsigned exception, uint64_t data);

/*** RISC-V VM function calls ***/

/* Make preparations for a VM function call. Returns 0 on success. */
LIBRISCVAPI int libriscv_setup_vmcall(RISCVMachine *m, uint64_t address);

/* Stack realignment helper. */
#define LIBRISCV_REALIGN_STACK(regs)  ((regs)->r[2] & ~0xFLL)

/* Register function or system call argument helper. */
#define LIBRISCV_ARG_REGISTER(regs, n)  (regs)->r[10 + (n)]

/* Put data on the current stack, with maintained 16-byte alignment. */
static inline uint64_t libriscv_stack_push(RISCVMachine *m, RISCVRegisters *regs, const char *data, unsigned len) {
	regs->r[2] -= len;
	LIBRISCV_REALIGN_STACK(regs);
	libriscv_copy_to_guest(m, regs->r[2], data, len);
	return regs->r[2];
}

#ifdef __cplusplus
}
#endif

#endif // LIBRISCV_H
