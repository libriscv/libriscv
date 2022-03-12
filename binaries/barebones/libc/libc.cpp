#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef NATIVE_MEM_SYSCALLS
#define NATIVE_MEM_FUNCATTR __attribute__((noinline))
#include <include/syscall.hpp>
#define SYS_IARRAY *(const char(*)[size])
#define SYS_OARRAY *(char(*)[size])
#define SYS_IUARRAY *(const char*)
#define SYS_OUARRAY *(char*)
#else
#define NATIVE_MEM_FUNCATTR /* */
#endif
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

#ifndef USE_NEWLIB
extern "C" struct _reent* _impure_ptr;
void* __dso_handle;

extern "C" int* __errno() {
	static int errno_value = 0;
	return &errno_value;
}
#endif

#ifndef NATIVE_MEM_SYSCALLS
extern "C"
void* memset(void* vdest, int ch, size_t size)
{
	char* dest = (char*) vdest;
	for (size_t i = 0; i < size; i++)
		dest[i] = ch;
	return dest;
}
void* memcpy(void* vdest, const void* vsrc, size_t size)
{
	const char* src = (const char*) vsrc;
	char* dest = (char*) vdest;
	for (size_t i = 0; i < size; i++)
		dest[i] = src[i];
	return vdest;
}
void* memmove(void* vdest, const void* vsrc, size_t size)
{
	const char* src = (const char*) vsrc;
	char* dest = (char*) vdest;
	if (dest <= src)
	{
		for (size_t i = 0; i < size; i++)
			dest[i] = src[i];
	}
	else
	{
		for (int i = size-1; i >= 0; i--)
			dest[i] = src[i];
	}
	return dest;
}

#else

#ifdef USE_NEWLIB
#define memset  "__wrap_memset"
#define memcpy  "__wrap_memcpy"
#define memmove "__wrap_memmove"
#define memcmp  "__wrap_memcmp"
#define memmove "__wrap_memmove"

#define strlen  "__wrap_strlen"
#define strcmp  "__wrap_strcmp"
#define strncmp "__wrap_strncmp"
#else
#define memset  "memset"
#define memcpy  "memcpy"
#define memmove "memmove"
#define memcmp  "memcmp"
#define memmove "memmove"

#define strlen  "strlen"
#define strcmp  "strcmp"
#define strncmp "strncmp"
#endif

asm(".global " memset "\n"
".type " memset ", @function\n"
memset ":\n"
"li a7, " STRINGIFY(SYSCALL_MEMSET) "\n"
"ecall\n"
"ret\n");

asm(".global " memcpy "\n"
".type " memcpy ", @function\n"
memcpy ":\n"
"li a7, " STRINGIFY(SYSCALL_MEMCPY) "\n"
"ecall\n"
"ret\n");

asm(".global " memmove "\n"
".type " memmove ", @function\n"
memmove ":\n"
"li a7, " STRINGIFY(SYSCALL_MEMMOVE) "\n"
"ecall\n"
"ret\n");

asm(".global " memcmp "\n"
".type " memcmp ", @function\n"
memcmp ":\n"
"li a7, " STRINGIFY(SYSCALL_MEMCMP) "\n"
"ecall\n"
"ret\n");

asm(".global " strlen "\n"
".type " strlen ", @function\n"
strlen ":\n"
"li a7, " STRINGIFY(SYSCALL_STRLEN) "\n"
"ecall\n"
"ret\n");

asm(".global " strcmp "\n"
".type " strcmp ", @function\n"
strcmp ":\n"
"li a2, 4096\n"
"li a7, " STRINGIFY(SYSCALL_STRCMP) "\n"
"ecall\n"
"ret\n");

asm(".global " strncmp "\n"
".type " strncmp ", @function\n"
strncmp ":\n"
"li a7, " STRINGIFY(SYSCALL_STRCMP) "\n"
"ecall\n"
"ret\n");

#endif


#ifndef USE_NEWLIB

extern "C"
wchar_t* wmemcpy(wchar_t* wto, const wchar_t* wfrom, size_t size)
{
	return (wchar_t *) memcpy (wto, wfrom, size * sizeof (wchar_t));
}

extern "C"
void* memchr(const void *s, int c, size_t n)
{
    if (n != 0) {
        const auto* p = (const unsigned char*) s;

        do {
            if (*p++ == c)
                return ((void *)(p - 1));
        } while (--n != 0);
    }
    return nullptr;
}

extern "C"
char* strcpy(char* dst, const char* src)
{
	while ((*dst++ = *src++));
	*dst = 0;
	return dst;
}
#ifndef NATIVE_MEM_SYSCALLS
extern "C"
size_t strlen(const char* str)
{
	const char* iter;
	for (iter = str; *iter; ++iter);
	return iter - str;
}
extern "C"
int strcmp(const char* str1, const char* str2)
{
	while (*str1 != 0 && *str2 != 0  && *str1 == *str2) {
      str1++;
      str2++;
   }
   return *str1 - *str2;
}
extern "C"
int strncmp(const char* s1, const char* s2, size_t n)
{
    while ( n && *s1 && ( *s1 == *s2 ) )
    {
        ++s1;
        ++s2;
        --n;
    }
    if ( n == 0 )
    {
        return 0;
    }
    else
    {
        return ( *(unsigned char *)s1 - *(unsigned char *)s2 );
    }
}
#endif
extern "C"
char* strcat(char* dest, const char* src)
{
	strcpy(dest + strlen(dest), src);
	return dest;
}

extern "C"
void* __memcpy_chk(void* dest, const void* src, size_t len, size_t destlen)
{
  assert (len <= destlen);
  return memcpy(dest, src, len);
}
extern "C"
void* __memset_chk(void* dest, int c, size_t len, size_t destlen)
{
  assert (len <= destlen);
  return memset(dest, c, len);
}
extern "C"
char* __strcat_chk(char* dest, const char* src, size_t destlen)
{
  size_t len = strlen(dest) + strlen(src) + 1;
  assert (len <= destlen);
  return strcat(dest, src);
}

extern "C"
int abs(int value)
{
	return (value >= 0) ? value : -value;
}

#else

extern "C" __attribute__((noreturn))
void _exit(int code)
{
	register long a0 asm("a0") = code;

	asm volatile("r%=: .insn i SYSTEM, 0, %0, x0, 0x7ff \nj r%=\n" :: "r"(a0));
	__builtin_unreachable();
}

extern "C" __attribute__((noreturn))
void exit(int code)
{
	_exit(code);
}

#endif
