#include <cstddef>
#include <cstdint>

extern "C" {
	void* memset(void* dest, int ch, size_t size);
	void* memcpy(void* dest, const void* src, size_t size);
	void* memmove(void* dest, const void* src, size_t size);
	int memcmp(const void* ptr1, const void* ptr2, size_t n);
}
