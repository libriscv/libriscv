#pragma once
// The host's dynamic loader, which loads translations compiled to shared objects.
// Hosts without one (e.g. Nintendo Switch homebrew) can still activate embedded
// translations; loading and compiling at runtime always fail there.
#if defined(__MINGW32__) || defined(__MINGW64__) || defined(_MSC_VER)
# include "win32/dlfcn.h"
# define RISCV_HAVE_DLOPEN 1
#elif __has_include(<dlfcn.h>)
# include <dlfcn.h>
# define RISCV_HAVE_DLOPEN 1
#else
# define RISCV_HAVE_DLOPEN 0
# ifndef RTLD_LAZY
#  define RTLD_LAZY 0
# endif
namespace riscv {
	inline void* dlopen(const char*, int) { return nullptr; }
	inline void* dlsym(void*, const char*) { return nullptr; }
	inline int dlclose(void*) { return 0; }
}
#endif
