#include "common.hpp"

#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

static std::string compiler()
{
	const char* cc = getenv("CC");
	if (cc) return std::string(cc);
	return "gcc";
}
static std::string extra_cflags()
{
	const char* cflags = getenv("CFLAGS");
	if (cflags) return std::string(cflags);
	return "";
}
static bool keep_code()
{
	return getenv("KEEPCODE") != nullptr;
}
static bool verbose()
{
	return getenv("VERBOSE") != nullptr;
}
static std::string host_arch()
{
#ifdef __x86_64__
	return "HOST_AMD64";
#else
	return "HOST_UNKNOWN";
#endif
}

namespace riscv
{
	std::string compile_command(int /*arch*/, const std::unordered_map<std::string, std::string>& defines)
	{
		std::string defstr;
		for (auto pair : defines) {
			defstr += " -D" + pair.first + "=" + pair.second;
		}

		return compiler() + " -O2 -s -std=c99 -fPIC -shared -rdynamic -x c "
			" -fexceptions" +
#ifdef RISCV_EXT_VECTOR
			" -march=native" +
#endif
			defstr +
			" -DARCH=" + host_arch() + ""
			" -pipe " + extra_cflags();
	}

	void*
	compile(const std::string& code, int arch, const std::unordered_map<std::string, std::string>& defines, const char* outfile)
	{
		// create temporary filename
		char namebuffer[64];
		strncpy(namebuffer, "/tmp/rvtrcode-XXXXXX", sizeof(namebuffer));
		// open a temporary file with owner privs
		const int fd = mkstemp(namebuffer);
		if (fd < 0) {
			return nullptr;
		}
		// write translated code to temp file
		ssize_t len = write(fd, code.c_str(), code.size());
		if (len < (ssize_t) code.size()) {
			unlink(namebuffer);
			return nullptr;
		}
		// system compiler invocation
		const std::string command =
			compile_command(arch, defines) + " "
			 + " -o " + std::string(outfile) + " "
			 + std::string(namebuffer) + " 2>&1"; // redirect stderr

		// compile the translated code
		if (verbose()) {
			printf("Command: %s\n", command.c_str());
		}
		FILE* f = popen(command.c_str(), "r");
		if (f == nullptr) {
			unlink(namebuffer);
			return nullptr;
		}
		// get compiler output
		char buffer[2048];
		while (fgets(buffer, sizeof(buffer), f) != NULL) {
			if (verbose())
				fprintf(stderr, "%s", buffer);
		}
		pclose(f);

		if (!keep_code()) {
			// delete temporary code file
			unlink(namebuffer);
		}

		return dlopen(outfile, RTLD_LAZY);
	}

	void* dylib_lookup(void* dylib, const char* symbol)
	{
		return dlsym(dylib, symbol);
	}

	void dylib_close(void* dylib)
	{
		dlclose(dylib);
	}
}
