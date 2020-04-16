#include <cassert>
#include <cstddef>
#include <cstdint>
extern struct _reent* _impure_ptr;

extern "C"
void* memset(void* vdest, int ch, size_t size)
{
	char* dest = (char*) vdest;
	for (size_t i = 0; i < size; i++)
		dest[i] = ch;
	return dest;
}
extern "C" __attribute__((used))
void* memcpy(void* vdest, const void* vsrc, size_t size)
{
	const char* src = (const char*) vsrc;
	char* dest = (char*) vdest;
	for (size_t i = 0; i < size; i++)
		dest[i] = src[i];
	return dest;
}
extern "C"
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
extern "C"
int memcmp(const void* ptr1, const void* ptr2, size_t n)
{
	const uint8_t* iter1 = (const uint8_t*) ptr1;
	const uint8_t* iter2 = (const uint8_t*) ptr2;
	while (n > 0 && *iter1 == *iter2) {
		iter1++;
		iter2++;
		n--;
	}
	return n == 0 ? 0 : (*iter1 - *iter2);
}

extern "C"
char* strcpy(char* dst, const char* src)
{
	while ((*dst++ = *src++));
	*dst = 0;
	return dst;
}
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
