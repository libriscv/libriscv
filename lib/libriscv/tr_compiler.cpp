#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>

static std::string compiler()
{
	const char* cc = getenv("CC");
	if (cc) return std::string(cc);
	return "gcc";
}
static std::string cflags()
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

namespace riscv
{
	std::pair<std::string, void*>
	compile(const std::string& code, int arch)
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
			compiler() + " -O0 -s -std=c99 -fPIC -shared -x c "
			" -ffreestanding -nostdlib "
			 + "-DRISCV_TRANSLATION_DYLIB=" + std::to_string(arch)
			 + " " + cflags() + " -o " + outfile + " "
			 + std::string(namebuffer) + " 2>&1"; // redirect stderr

		// compile the translated code
		if (verbose()) {
			printf("Command: %s\n", command.c_str());
		}
		FILE* f = popen(command.c_str(), "r");
		if (f == nullptr) {
			unlink(namebuffer);
			return {"", nullptr};
		}
		if (verbose()) {
			// get compiler output
			char buffer[1024];
			while (fgets(buffer, sizeof(buffer), f) != NULL) {
				fprintf(stderr, "%s", buffer);
			}
		}
		pclose(f);

		if (!keep_code()) {
			// delete temporary code file
			unlink(namebuffer);
		}

		return {outfile, dlopen(outfile.c_str(), RTLD_LAZY)};
	}
}
