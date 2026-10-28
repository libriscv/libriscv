#include "common.hpp"

#include <cstring>
#include <libtcc.h>
#include <unordered_map>
#if defined(__aarch64__) || defined(__riscv)
#include <lib-arm64.h> // lib_lib_arm64_c and lib_lib_arm64_c_len
#elif !defined(__arm__)
#include <libtcc1.h> // libtcc1_c and libtcc1_c_len
#endif
#ifdef __ANDROID__
#include <android/log.h>
#endif

#if defined(__arm__)
// 32-bit ARM: TCC calls the EABI run-time helpers, which the host already links
#define RISCV_AEABI_HELPERS(X) \
	X(__aeabi_d2lz) X(__aeabi_d2ulz) X(__aeabi_f2lz) X(__aeabi_f2ulz) \
	X(__aeabi_l2d) X(__aeabi_l2f) X(__aeabi_ul2d) X(__aeabi_ul2f) \
	X(__aeabi_lasr) X(__aeabi_llsl) X(__aeabi_llsr) \
	X(__aeabi_idiv) X(__aeabi_idivmod) X(__aeabi_uidiv) X(__aeabi_uidivmod) \
	X(__aeabi_ldivmod) X(__aeabi_uldivmod) \
	X(__aeabi_memcpy) X(__aeabi_memmove) X(__aeabi_memmove4) X(__aeabi_memmove8) X(__aeabi_memset)
#define RISCV_AEABI_DECLARE(name) void name();
extern "C" { RISCV_AEABI_HELPERS(RISCV_AEABI_DECLARE) }
#endif

namespace riscv
{
	void* libtcc_compile(const std::string& code,
		int, const std::unordered_map<std::string, std::string>& cflags, const std::string& libtcc1)
	{
		(void)libtcc1;

		TCCState* state = tcc_new();
		if (!state)
			return nullptr;

		tcc_set_output_type(state, TCC_OUTPUT_MEMORY);

		for (const auto& pair : cflags) {
			tcc_define_symbol(state, pair.first.c_str(), pair.second.c_str());
		}

		tcc_define_symbol(state, "ARCH", "HOST_UNKNOWN");
		tcc_set_options(state, "-std=c99 -O2 -nostdlib");

#if defined(_WIN32)
		// Look for some headers in the win32 directory
		tcc_add_include_path(state, "win32");
#elif defined(__linux__) && defined(__x86_64__)
		tcc_add_include_path(state, "/usr/include/x86_64-linux-gnu");
#elif defined(__linux__) && defined(__i386__)
		tcc_add_include_path(state, "/usr/include/i386-linux-gnu");
#elif defined(__linux__) && defined(__aarch64__)
		tcc_add_include_path(state, "/usr/include/aarch64-linux-gnu");
#elif defined(__linux__) && defined(__riscv)
		tcc_add_include_path(state, "/usr/include/riscv64-linux-gnu");
#endif

#ifdef __ANDROID__
		tcc_define_symbol(state, "__ANDROID__", "1");
		tcc_set_error_func(state, nullptr, [](void*, const char* msg) {
			// Android does not support stderr, so we use logcat
			__android_log_print(ANDROID_LOG_ERROR, "libtcc", "%s", msg);
		});
#endif

		tcc_add_symbol(state, "memset", (void*)memset);
		tcc_add_symbol(state, "memcpy", (void*)memcpy);
		tcc_add_symbol(state, "memcmp", (void*)memcmp);
		tcc_add_symbol(state, "memmove", (void*)memmove);
#if defined(__arm__)
#define RISCV_AEABI_ADD(name) tcc_add_symbol(state, #name, (void*)&name);
		RISCV_AEABI_HELPERS(RISCV_AEABI_ADD)
#endif

		std::string code1;
#if defined(__aarch64__) || defined(__riscv)
		code1 = std::string((const char*)lib_lib_arm64_c, lib_lib_arm64_c_len) + code;
#elif defined(__arm__)
		code1 = code;
#else
		code1 = std::string((const char*)lib_libtcc1_c, lib_libtcc1_c_len) + code;
#endif

		if (tcc_compile_string(state, code1.c_str()) < 0) {
			tcc_delete(state);
			return nullptr;
		}

#if defined(TCC_RELOCATE_AUTO)
		if (tcc_relocate(state, TCC_RELOCATE_AUTO) < 0)
#else
		if (tcc_relocate(state) < 0)
#endif
		{
			tcc_delete(state);
			return nullptr;
		}

		return state;
	}

	void* tcc_lookup(void* state, const char* symbol)
	{
		return tcc_get_symbol((TCCState *)state, symbol);
	}

	void tcc_close(void* state)
	{
		tcc_delete((TCCState *)state);
	}
}
