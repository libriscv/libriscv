#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>

static std::string compiler()
{
	const char* cxx = getenv("CXX");
	if (cxx) return std::string(cxx);
	return "g++";
}

static std::string libriscv_path()
{
	const char* p = getenv("RISCV_INC");
	if (p) return std::string(p);
	return "/home/gonzo/github/libriscv/lib";
}
static std::string eastl_path()
{
	const char* p = getenv("EASTL_INC");
	if (p) return std::string(p);
	return "/home/gonzo/github/libriscv/lib/EASTL/include";
}

namespace riscv
{
	std::pair<std::string, void*>
	compile(const std::string& code)
	{
		// create temporary filename
		char namebuffer[64];
		strncpy(namebuffer, "/tmp/rvtrcode-XXXXXX", sizeof(namebuffer));
		// open a temporary file with owner privs
		int fd = mkstemp(namebuffer);
		if (fd < 0) {
			return {"", nullptr};
		}
		// write translated code to temp file
		ssize_t len = write(fd, code.c_str(), code.size());
		if (len < (ssize_t) code.size()) {
			unlink(namebuffer);
			return {"", nullptr};
		}
		const auto outfile = std::string(namebuffer) + ".elf";
		// system compiler invocation
		const std::string command =
			compiler() + " -std=c++17 -shared -x c++ -fPIC -O "
			" -fno-exceptions -fno-rtti -fuse-ld=lld "
			 + "-I\"" + libriscv_path() + "\" -I\"" + eastl_path() + "\" -I\"" + eastl_path() + "/../test/packages/EABase/include/Common"
			 + "\" -o " + outfile + " "
			 + std::string(namebuffer) + " 2>&1"; // redirect stderr

		// compile the translated code
		printf("Command: %s\n", command.c_str());
		FILE* f = popen(command.c_str(), "r");
		if (f == nullptr) {
			unlink(namebuffer);
			return {"", nullptr};
		}
		// get compiler output
		char buffer[1024];
		while (fgets(buffer, sizeof(buffer), f) != NULL) {
			fprintf(stderr, "%s", buffer);
		}
		pclose(f);
		// delete temporary code file
		//unlink(namebuffer);

		return {outfile, dlopen(outfile.c_str(), RTLD_LAZY)};
	}
}
