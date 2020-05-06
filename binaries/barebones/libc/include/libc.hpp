#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((noreturn))
void panic(const char* reason);

extern void* memset(void* dest, int ch, size_t size);
extern void* memcpy(void* dest, const void* src, size_t size);
extern void* memmove(void* dest, const void* src, size_t size);
extern int   memcmp(const void* ptr1, const void* ptr2, size_t n);
extern char*  strcpy(char* dst, const char* src);
extern size_t strlen(const char* str);
extern int    strcmp(const char* str1, const char* str2);
extern char*  strcat(char* dest, const char* src);

extern int   write(int, const void*, size_t);

extern void* malloc(size_t) _NOTHROW;
extern void* calloc(size_t, size_t) _NOTHROW;
extern void  free(void*) _NOTHROW;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifdef __cplusplus
}
#endif

#include "syscall.hpp"

inline int sys_write(const void* data, size_t len) {
	return syscall(SYSCALL_WRITE, 0, (long) data, len);
}

inline void put_string(const char* string)
{
	(void) sys_write(string, __builtin_strlen(string));
}
