#pragma once
/**
 * The host functions, as plain syscalls. Both send the same attribute tree to
 * the host; they differ only in what crosses the boundary.
 */
#include <cstddef>
#include <cstdint>

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

#define SYSCALL_OUT_FLAT 480
#define SYSCALL_OUT_OBJ  481
#define SYSCALL_PRINT    482

extern "C" {
	/// @brief Send a flat HostAttr array. Returns the host-side checksum.
	long sys_attr_out_flat(const void* nodes, size_t count);
	/// @brief Send the address of the guest's own Attributes object.
	long sys_attr_out_obj(const void* attrs);
	void sys_print(const char* text);
}

#define GENERATE_SYSCALL_WRAPPER(name, number) \
	asm(".pushsection .text, \"ax\", @progbits\n" \
		".global " #name "\n" #name ":\n  li a7, " STRINGIFY(number) "\n  ecall\n  ret\n" \
		".popsection\n");
