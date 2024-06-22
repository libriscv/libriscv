#include "common.hpp"

#ifdef RISCV_LIBTCC_PACKAGE
# include <libtcc.h>
#else
# include <tcc/libtcc.h>
# ifndef LIBTCC_LIBRARY_PATH
#  define LIBTCC_LIBRARY_PATH "."
# endif
#endif

namespace riscv
{
	void* libtcc_compile(const std::string& code,
		int, const std::unordered_map<std::string, std::string>& cflags, const std::string& libtcc1)
	{
		TCCState* state = tcc_new();
		if (!state)
			return nullptr;

		tcc_set_output_type(state, TCC_OUTPUT_MEMORY);

		for (const auto& pair : cflags) {
			tcc_define_symbol(state, pair.first.c_str(), pair.second.c_str());
		}

		tcc_define_symbol(state, "ARCH", "HOST_UNKNOWN");
		tcc_set_options(state, "-std=c99 -O2");

#ifdef _WIN32
		// Look for some headers in the win32 directory
		tcc_add_include_path(state, "win32");
#endif

		if (!libtcc1.empty())
			tcc_add_library_path(state, libtcc1.c_str());
#ifdef LIBTCC_LIBRARY_PATH
		tcc_add_library_path(state, LIBTCC_LIBRARY_PATH);
#endif

		if (tcc_compile_string(state, code.c_str()) < 0) {
			tcc_delete(state);
			return nullptr;
		}

#if defined(TCC_RELOCATE_AUTO)
		if (tcc_relocate(state, TCC_RELOCATE_AUTO) < 0) {
#else
		if (tcc_relocate(state) < 0) {
#endif
			tcc_delete(state);
			return nullptr;
		}

		return state;
	}

	void* dylib_lookup(void* state, const char* symbol)
	{
		return tcc_get_symbol((TCCState *)state, symbol);
	}

	void dylib_close(void* state)
	{
		tcc_delete((TCCState *)state);
	}
}
