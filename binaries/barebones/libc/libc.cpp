#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#ifdef NATIVE_MEM_SYSCALLS
#include <include/syscall.hpp>
#endif
#ifndef USE_NEWLIB
extern "C" struct _reent* _impure_ptr;
void* __dso_handle;

extern "C" int* __errno() {
	static int errno_value = 0;
	return &errno_value;
}
#endif

extern "C"
void* memset(void* vdest, int ch, size_t size)
{
#ifndef NATIVE_MEM_SYSCALLS
	char* dest = (char*) vdest;
	for (size_t i = 0; i < size; i++)
		dest[i] = ch;
	return dest;
#else
	return (void*) syscall(SYSCALL_MEMSET, (long) vdest, ch, size);
#endif
}
extern "C"
void* memcpy(void* vdest, const void* vsrc, size_t size)
{
#ifndef NATIVE_MEM_SYSCALLS
	const char* src = (const char*) vsrc;
	char* dest = (char*) vdest;
	for (size_t i = 0; i < size; i++)
		dest[i] = src[i];
	return dest;
#else
	return (void*) syscall(SYSCALL_MEMCPY, (long) vdest, (long) vsrc, size);
#endif
}
extern "C"
void* memmove(void* vdest, const void* vsrc, size_t size)
{
#ifndef NATIVE_MEM_SYSCALLS
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
#else
	return (void*) syscall(SYSCALL_MEMMOVE, (long) vdest, (long) vsrc, size);
#endif
}

#ifdef NATIVE_MEM_SYSCALLS

extern "C"
int memcmp(const void* ptr1, const void* ptr2, size_t n)
{
	return syscall(SYSCALL_MEMCMP, (long) ptr1, (long) ptr2, n);
}

extern "C"
size_t strlen(const char* str)
{
	return syscall(SYSCALL_STRLEN, (long) str);
}
extern "C"
int strcmp(const char* str1, const char* str2)
{
	return syscall(SYSCALL_STRCMP, (long) str1, (long) str2, 4096);
}
extern "C"
int strncmp(const char* s1, const char* s2, size_t n)
{
	return syscall(SYSCALL_STRCMP, (long) s1, (long) s2, n);
}

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

	asm("ebreak" : : "r"(a0));
	__builtin_unreachable();
}

extern "C" __attribute__((noreturn))
void exit(int code)
{
	_exit(code);
}

#endif
