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

#define RISCV_PAGE_SIZE  4096

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
	/* Extended options (initialize via libriscv_set_defaults) */
	int      use_memory_arena;            /* Create linear memory arena (default: 1) */
	int      use_shared_execute_segments; /* Share execute segments across forks (default: 1) */
	const char *default_exit_function;    /* Symbol name for exit function (default: NULL) */
	int      load_program;                /* Load ELF at construction (default: 1) */
	int      protect_segments;            /* Apply ELF segment protections (default: 1) */
	unsigned native_syscall_base;         /* If non-zero, install native heap+memory syscalls at this base (needs 10 slots) */
	uint64_t arena_size;                  /* Arena size in bytes (default: 8 MiB, ignored if native_syscall_base == 0) */
	unsigned native_heap_max_chunks;      /* Host metadata cap (0: derive from arena_size) */
} RISCVOptions;

/* Fill out default values. */
LIBRISCVAPI void libriscv_set_defaults(RISCVOptions *options);

/* Create a new 64-bit RISC-V machine from an ELF binary. The binary must out-live the machine. */
LIBRISCVAPI RISCVMachine *libriscv_new(const void *elf_prog, unsigned elf_size, const RISCVOptions *o);

/* Free a RISC-V machine created using libriscv_new or libriscv_fork. */
LIBRISCVAPI int libriscv_delete(RISCVMachine *m);


/* Start execution at current PC, with the given instruction limit. 0 on success.
   When an error occurs, the negative value is one of the RISCV_ERROR_ enum values. */
LIBRISCVAPI int libriscv_run(RISCVMachine *m, uint64_t instruction_limit);

/* Step one instruction. If verbose is non-zero, print the current instruction and registers
   before the step. Returns instruction counter on success, or 0 if the machine has stopped.
   On error, returns a negative value which is one of the RISCV_ERROR_ enum values. */
LIBRISCVAPI int64_t libriscv_step_one(RISCVMachine *m, int verbose);

/* Add a host-side filepath that can be opened by the guest program. Sandbox must be disabled. */
LIBRISCVAPI void libriscv_allow_file(RISCVMachine *m, const char *path);

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
	uint64_t  r[32];
	uint64_t  pc;
	uint32_t  fcsr;
	RISCVFloat fr[32];
} RISCVRegisters;

/* Retrieve the internal registers of the RISC-V machine. Changing PC is dangerous. */
LIBRISCVAPI RISCVRegisters * libriscv_get_registers(RISCVMachine *m);

/* Set the result register A0 to a value. */
LIBRISCVAPI void libriscv_set_result_register(RISCVMachine *m, int64_t value);

/* Change the PC register safely. PC can be changed before running and during system calls. */
LIBRISCVAPI int libriscv_jump(RISCVMachine *m, uint64_t address);

/* Copy memory in and out of the RISC-V machine. */
LIBRISCVAPI int libriscv_copy_to_guest(RISCVMachine *m, uint64_t dst, const void *src, unsigned len);
LIBRISCVAPI int libriscv_copy_from_guest(RISCVMachine *m, void *dst, uint64_t src, unsigned len);

/* View a zero-terminated string from readable memory of at most maxlen length. The string is read-only.
   On success, set *length and return a pointer to the string, zero-copy. Otherwise, return null. */
LIBRISCVAPI const char * libriscv_memstring(RISCVMachine *m, uint64_t src, unsigned maxlen, unsigned *length);

/* View a slice of readable (but not guaranteed writable) memory from src to src + length.
   On success, returns a pointer to the memory. Otherwise, returns null. */
LIBRISCVAPI const char * libriscv_memview(RISCVMachine *m, uint64_t src, unsigned length);

/* View a slice of readable memory as an array of Type from src to src + sizeof(Type) * Count.
   On success, returns a pointer to the memory. Otherwise, returns null.
   Example: uint32_t *array_of_ten = LIBRISCV_VIEW_ARRAY(m, uint32_t, 0x1000, 10); */
#define LIBRISCV_VIEW_ARRAY(m, Type, Addr, Count)  ((Type*)libriscv_memview(m, Addr, sizeof(Type) * Count))

/* View a slice of readable and writable memory from src to src + length.
   On success, returns a pointer to the writable memory. Otherwise, returns null. */
LIBRISCVAPI char * libriscv_writable_memview(RISCVMachine *m, uint64_t src, unsigned length);

/* View a slice of writable memory as an array of Type from src to src + sizeof(Type) * Count.
   On success, returns a pointer to the writable memory. Otherwise, returns null.
   Example: uint32_t *array_of_ten = LIBRISCV_VIEW_WRITABLE_ARRAY(m, uint32_t, 0x1000, 10); */
#define LIBRISCV_VIEW_WRITABLE_ARRAY(m, Type, Addr, Count)  ((Type*)libriscv_writable_memview(m, Addr, sizeof(Type) * Count))

/* Stops execution normally. Only possible from a system call and EBREAK. */
LIBRISCVAPI void libriscv_stop(RISCVMachine *m);

/* Return current instruction counter value. */
LIBRISCVAPI uint64_t libriscv_instruction_counter(RISCVMachine *m);

/* Return a *pointer* to the instruction max counter. */
LIBRISCVAPI uint64_t * libriscv_max_counter_pointer(RISCVMachine *m);

/* Returns non-zero if the instruction limit has been reached. */
LIBRISCVAPI int libriscv_instruction_limit_reached(RISCVMachine *m);

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
#define LIBRISCV_FP32_ARG_REG(regs, n)  (regs)->fr[10 + (n)].f32[0]
#define LIBRISCV_FP64_ARG_REG(regs, n)  (regs)->fr[10 + (n)].f64

/* Put data on the current stack, with maintained 16-byte alignment. */
static inline uint64_t libriscv_stack_push(RISCVMachine *m, RISCVRegisters *regs, const char *data, unsigned len) {
	regs->r[2] -= len;
	regs->r[2] = LIBRISCV_REALIGN_STACK(regs);
	libriscv_copy_to_guest(m, regs->r[2], data, len);
	return regs->r[2];
}

/* Helper for viewing an argument registers as a given type or array of type */
#define LIBRISCV_VIEW_ARG(m, regs, n, Type)  ((Type*)libriscv_memview(m, LIBRISCV_ARG_REGISTER(regs, n), sizeof(Type)))
#define LIBRISCV_VIEW_ARG_ARRAY(m, regs, n, Type, Count)  ((Type*)libriscv_memview(m, LIBRISCV_ARG_REGISTER(regs, n), sizeof(Type) * Count))

#define LIBRISCV_VIEW_WRITABLE_ARG(m, regs, n, Type)  ((Type*)libriscv_writable_memview(m, LIBRISCV_ARG_REGISTER(regs, n), sizeof(Type)))
#define LIBRISCV_VIEW_WRITABLE_ARG_ARRAY(m, regs, n, Type, Count)  ((Type*)libriscv_writable_memview(m, LIBRISCV_ARG_REGISTER(regs, n), sizeof(Type) * Count))

/* Helper for loading binary files */
LIBRISCVAPI int libriscv_load_binary_file(const char *filename, char **data);


/*** Fast-fork API: create lightweight Copy-on-Write VM forks ***/

/* Page attributes for fine-grained memory control. */
typedef struct {
	int read;
	int write;
	int exec;
	int is_cow;
	int non_owning;
	int dont_fork;
	uint8_t user_defined;
} RISCVPageAttributes;

/* Create a fast fork of a parent machine. Installs default CoW page handlers and
   arena at the parent's high watermark. The parent must outlive all its forks.
   Uses opts only for error, stdout, and opaque. Returns NULL on failure. */
LIBRISCVAPI RISCVMachine *libriscv_fast_fork(const RISCVMachine *parent, RISCVOptions *opts);

/* Returns non-zero if the machine is a fork. */
LIBRISCVAPI int libriscv_is_forked(const RISCVMachine *m);

/* Retrieve a parent machine's page data and attributes for a given page number.
   Useful within page handler callbacks for implementing CoW.
   Returns pointer to RISCV_PAGE_SIZE bytes, or NULL if the page doesn't exist. */
LIBRISCVAPI const void *libriscv_get_parent_page_data(
	const RISCVMachine *parent, uint64_t pageno, RISCVPageAttributes *attr_out);


/*** Arena (native heap) management ***/

/* Arena realloc result: new pointer and old allocation size. */
typedef struct {
	uint64_t ptr;
	uint64_t old_size;
} RISCVReallocResult;

/* Called when arena.free() encounters an unknown pointer (e.g., master-owned).
   Return 0 to silently succeed, -1 for error. */
typedef int (*riscv_arena_unknown_free_t)(uint64_t ptr, void *user);

/* Called when arena.realloc() encounters an unknown pointer.
   Return new_ptr and old_size so the caller can memcpy. */
typedef RISCVReallocResult (*riscv_arena_unknown_realloc_t)(
	uint64_t ptr, uint64_t new_size, void *user);

/* Set up a native heap arena at the given guest address.
   syscall_base is the first system call number used by the arena (needs 5 slots).
   Returns 0 on success. */
LIBRISCVAPI int libriscv_setup_arena(
	RISCVMachine *m, unsigned syscall_base, uint64_t addr, uint64_t size);

/* Returns non-zero if the machine has an arena. */
LIBRISCVAPI int libriscv_has_arena(const RISCVMachine *m);

/* Allocate memory in the arena. Returns guest address, or 0 on failure. */
LIBRISCVAPI uint64_t libriscv_arena_malloc(RISCVMachine *m, uint64_t size);

/* Free an arena allocation. Returns 0 on success, -1 on failure. */
LIBRISCVAPI int libriscv_arena_free(RISCVMachine *m, uint64_t ptr);

/* Reallocate an arena allocation. Returns new pointer and old size. */
LIBRISCVAPI RISCVReallocResult libriscv_arena_realloc(
	RISCVMachine *m, uint64_t ptr, uint64_t new_size);

/* Get the size of an arena allocation. Returns 0 if pointer is invalid. */
LIBRISCVAPI uint64_t libriscv_arena_size(RISCVMachine *m, uint64_t ptr);

/* Get the highest address covered by any live allocation in the arena.
   Cache this after master initialization for O(1) fork arena setup. */
LIBRISCVAPI uint64_t libriscv_arena_high_watermark(const RISCVMachine *m);

/* Set handler for free() of unknown (e.g. master-owned) pointers. */
LIBRISCVAPI void libriscv_arena_set_unknown_free(
	RISCVMachine *m, riscv_arena_unknown_free_t handler, void *user);

/* Set handler for realloc() of unknown (e.g. master-owned) pointers. */
LIBRISCVAPI void libriscv_arena_set_unknown_realloc(
	RISCVMachine *m, riscv_arena_unknown_realloc_t handler, void *user);

/* Transfer arena state from src to dst. Returns 0 on success. */
LIBRISCVAPI int libriscv_transfer_arena(RISCVMachine *dst, const RISCVMachine *src);


/*** Memory address queries ***/

/* Return the guest heap base address. */
LIBRISCVAPI uint64_t libriscv_heap_address(const RISCVMachine *m);

/* Allocate address space from the mmap region. Returns guest address. */
LIBRISCVAPI uint64_t libriscv_mmap_allocate(RISCVMachine *m, uint64_t bytes);

/* Return the initial stack pointer address. */
LIBRISCVAPI uint64_t libriscv_stack_initial(const RISCVMachine *m);

/* Return the number of owned (non-shared) active pages. */
LIBRISCVAPI uint64_t libriscv_owned_pages_active(const RISCVMachine *m);


/*** Shared / non-owned memory ***/

/* Insert externally-owned memory into the guest address space.
   dst and size should be page-aligned. The caller must ensure src outlives the machine. */
LIBRISCVAPI int libriscv_insert_non_owned_memory(
	RISCVMachine *m, uint64_t dst, void *src, uint64_t size,
	const RISCVPageAttributes *attr);


/*** Setup helpers ***/

/* Set up Linux system calls. filesystem/sockets: 0 to disable, non-zero to enable. */
LIBRISCVAPI int libriscv_setup_linux_syscalls(RISCVMachine *m, int filesystem, int sockets);

/* Set up POSIX threads emulation. */
LIBRISCVAPI int libriscv_setup_posix_threads(RISCVMachine *m);

/* Set up native memory system calls starting at syscall_base. */
LIBRISCVAPI int libriscv_setup_native_memory(unsigned syscall_base);


#ifdef __cplusplus
}
#endif

#endif // LIBRISCV_H
