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
