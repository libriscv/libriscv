#include "common.hpp"
#include <libtcc.h>

namespace riscv
{
	void* libtcc_compile(const std::string& code,
		int arch, uint64_t arena_size, uint64_t arena_roend, const std::string& libtcc1)
	{
		TCCState* state = tcc_new();
		tcc_set_output_type(state, TCC_OUTPUT_MEMORY);

		tcc_define_symbol(state, "RISCV_TRANSLATION_DYLIB", arch == 4 ? "4" : "8");
		tcc_define_symbol(state, "RISCV_MAX_SYSCALLS", std::to_string(RISCV_SYSCALLS_MAX).c_str());
		tcc_define_symbol(state, "RISCV_ARENA_END",    std::to_string(arena_size).c_str());
		tcc_define_symbol(state, "RISCV_ARENA_ROEND",  std::to_string(arena_roend).c_str());
#ifdef RISCV_EXT_VECTOR
		tcc_define_symbol(state, "RISCV_EXT_VECTOR",  std::to_string(RISCV_EXT_VECTOR).c_str());
#endif

		tcc_define_symbol(state, "ARCH", "HOST_UNKNOWN");
		tcc_set_options(state, "-std=c99 -O2");

		if (!libtcc1.empty())
			tcc_add_library_path(state, libtcc1.c_str());

		if (tcc_compile_string(state, code.c_str()) < 0)
			return nullptr;

		if (tcc_relocate(state, TCC_RELOCATE_AUTO) < 0)
			return nullptr;

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
