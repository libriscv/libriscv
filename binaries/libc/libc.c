#include <stddef.h>
#include <stdint.h>

__attribute__((used))
void* memset(char* dest, int ch, size_t size)
{
	for (size_t i = 0; i < size; i++)
		dest[i] = ch;
	return dest;
}
void* memcpy(char* dest, const char* src, size_t size)
{
	for (size_t i = 0; i < size; i++)
		dest[i] = src[i];
	return dest;
}
void* memmove(char* dest, const char* src, size_t size)
{
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

char* strcpy(char* dst, const char* src)
{
	while ((*dst++ = *src++));
	*dst = 0;
	return dst;
}
size_t strlen(const char* str)
{
	const char* iter;
	for (iter = str; *iter; ++iter);
	return iter - str;
}
int strcmp(const char* str1, const char* str2)
{
	while (*str1 != 0 && *str2 != 0  && *str1 == *str2) {
      str1++;
      str2++;
   }
   return *str1 - *str2;
}
char* strcat(char* dest, const char* src)
{
	strcpy(dest + strlen(dest), src);
	return dest;
}

void* __memcpy_chk(void* dest, const void* src, size_t len, size_t destlen)
{
  assert (len <= destlen);
  return memcpy(dest, src, len);
}
void* __memset_chk(void* dest, int c, size_t len, size_t destlen)
{
  assert (len <= destlen);
  return memset(dest, c, len);
}
char* __strcat_chk(char* dest, const char* src, size_t destlen)
{
  size_t len = strlen(dest) + strlen(src) + 1;
  assert (len <= destlen);
  return strcat(dest, src);
}
