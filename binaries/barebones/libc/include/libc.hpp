#include <stddef.h>
#include <stdint.h>
#include <include/printf.h>

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((noreturn))
void panic(const char* reason);

void* memset(void* dest, int ch, size_t size);
void* memcpy(void* dest, const void* src, size_t size);
void* memmove(void* dest, const void* src, size_t size);
int   memcmp(const void* ptr1, const void* ptr2, size_t n);
char*  strcpy(char* dst, const char* src);
size_t strlen(const char* str);
int    strcmp(const char* str1, const char* str2);
char*  strcat(char* dest, const char* src);

int   write(int, const void*, size_t);

void* malloc(size_t) _NOTHROW;
void* calloc(size_t, size_t) _NOTHROW;
void  free(void*) _NOTHROW;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifdef __cplusplus
}
#endif
