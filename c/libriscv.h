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

#define RISCV_ERROR_TYPE_GENERAL_EXCEPTION  1
#define RISCV_ERROR_TYPE_MACHINE_EXCEPTION  2
#define RISCV_ERROR_TYPE_MACHINE_TIMEOUT    3
typedef void (*riscv_error_func_t)(void *opaque, int type, const char *msg, long data);

typedef void (*riscv_stdout_func_t)(void *opaque, const char *msg, unsigned size);

typedef struct {
	uint64_t max_memory;
	uint64_t stack_size;
	int      argc; /* Program arguments */
	char**   argv;
	riscv_error_func_t error; /* Error callback */
	riscv_stdout_func_t stdout; /* Stdout callback */
	void* opaque;             /* User-provided pointer */
} RISCVOptions;

/* Fill out default values. */
LIBRISCVAPI void libriscv_set_defaults(RISCVOptions *options);

/* Create a new 64-bit RISC-V machine from a RISC-V ELF binary. */
LIBRISCVAPI RISCVMachine *libriscv_new(const void *elf_prog, unsigned elf_length, RISCVOptions *o);

/* Free a RISC-V machine created using libriscv_new. */
LIBRISCVAPI int libriscv_delete(RISCVMachine *m);


/* Start execution at current PC, with the given instruction limit. 0 on success. */
LIBRISCVAPI int libriscv_run(RISCVMachine *m, uint64_t instruction_limit);

/* Return current value of the return value register A0. */
LIBRISCVAPI int64_t libriscv_return_value(RISCVMachine *m);

/* Return current instruction counter value. */
LIBRISCVAPI uint64_t libriscv_instruction_counter(RISCVMachine *m);

/* Return symbol address or NULL if not found. */
LIBRISCVAPI long libriscv_address_of(RISCVMachine *m, const char *name);

/* Return the opaque value provided from options. */
LIBRISCVAPI void * libriscv_opaque(RISCVMachine *m);

#ifdef __cplusplus
}
#endif

#endif // LIBRISCV_H
