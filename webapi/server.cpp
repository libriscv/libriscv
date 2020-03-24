#include "server.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

static const char* ADDRESS = "localhost";
static const uint16_t PORT = 1234;

int main(void)
{
    using namespace httplib;
    Server svr;

    svr.Post("/compile", compile);
	svr.Post("/execute", execute);

	printf("Listening on %s:%u\n", ADDRESS, PORT);
    svr.listen(ADDRESS, PORT);
}

void common_response_fields(httplib::Response& res, int status)
{
	res.status = status;
	res.set_header("Access-Control-Allow-Origin", "*");
	res.set_header("Access-Control-Expose-Headers", "*");
}

void load_file(const std::string& filename, std::vector<uint8_t>& result)
{
    size_t size = 0;
    FILE* f = fopen(filename.c_str(), "rb");
    if (f == NULL) {
		result.clear();
		return;
	}

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    result.resize(size);
    if (size != fread(result.data(), 1, size, f))
    {
		result.clear();
    }
    fclose(f);
}
