#include "../common.hpp"

#include <cstring>
#include "dlfcn.h"

namespace riscv
{
	void* compile(const std::string& code, int arch, const std::string& cflags, const std::string& outfile)
	{
		(void)arch;
		(void)cflags;
		(void)outfile;
		(void)code;

		return nullptr;
	}

	void* dylib_lookup(void* dylib, const char* symbol, bool)
	{
		return dlsym(dylib, symbol);
	}

	void dylib_close(void* dylib, bool)
	{
		dlclose(dylib);
	}
}
