#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/rsp_server.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 32ul << 20;
static const uint16_t RSP_PORT = 21593;
using namespace riscv;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

static const std::string test_program = R"M(
volatile int counter = 0;

__attribute__((noinline))
int add(int value)
{
	counter += value;
	return counter;
}

int main()
{
	for (int i = 0; i < 3; i++)
		add(i);
	__asm__ volatile("ebreak");
	return counter + 10;
})M";

// A minimal GDB: speaks the Remote Serial Protocol over a socket
struct GdbClient
{
	int fd = -1;
	bool noack = false;
	std::string input;

	explicit GdbClient(uint16_t port) {
		for (int attempt = 0; attempt < 100; attempt++) {
			fd = socket(AF_INET, SOCK_STREAM, 0);
			struct sockaddr_in addr {};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(port);
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
				break;
			close(fd);
			fd = -1;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		REQUIRE(fd >= 0);
		struct timeval tv {};
		tv.tv_sec = 30;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	~GdbClient() { if (fd >= 0) close(fd); }

	void send_raw(const std::string& data) {
		REQUIRE(::write(fd, data.data(), data.size()) == ssize_t(data.size()));
	}
	void send(const std::string& payload) {
		uint8_t csum = 0;
		for (const char c : payload) csum += uint8_t(c);
		char tail[4];
		snprintf(tail, sizeof(tail), "#%02x", csum);
		send_raw("$" + payload + tail);
	}
	// Returns the next packet payload, or "<closed>"
	std::string receive() {
		while (true) {
			// Drop acknowledgements
			while (!input.empty() && (input[0] == '+' || input[0] == '-'))
				input.erase(0, 1);
			const size_t start = input.find('$');
			const size_t end = input.find('#', start);
			if (start != std::string::npos && end != std::string::npos && end + 2 < input.size()) {
				std::string payload;
				for (size_t i = start + 1; i < end; i++) {
					if (input[i] == '}') payload.push_back(input[++i] ^ 0x20);
					else payload.push_back(input[i]);
				}
				input.erase(0, end + 3);
				if (!noack) send_raw("+");
				return payload;
			}
			char buf[4096];
			const ssize_t len = ::read(fd, buf, sizeof(buf));
			if (len <= 0)
				return "<closed>";
			input.append(buf, len);
		}
	}
	std::string request(const std::string& payload) {
		send(payload);
		return receive();
	}
};

static std::string hex64(uint64_t value) {
	std::string out;
	char buf[4];
	for (int i = 0; i < 8; i++) {
		snprintf(buf, sizeof(buf), "%02x", unsigned(value >> (i * 8)) & 0xFF);
		out += buf;
	}
	return out;
}
static uint64_t unhex64(const std::string& hex) {
	uint64_t value = 0;
	for (int i = 7; i >= 0; i--)
		value = (value << 8) | std::stoul(hex.substr(i * 2, 2), nullptr, 16);
	return value;
}
static std::string addr_hex(uint64_t addr) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%lx", (unsigned long)addr);
	return buf;
}

struct RspSession
{
	Machine<RISCV64> machine;
	std::thread server_thread;
	std::unique_ptr<GdbClient> gdb;

	RspSession(const std::vector<uint8_t>& binary)
		: machine { binary, { .memory_max = MAX_MEMORY } }
	{
		machine.setup_linux_syscalls();
		machine.setup_linux({"rsp"}, {"LC_ALL=C"});
		machine.set_printer([] (const auto&, const char*, size_t) {});

		server_thread = std::thread([this] {
			RSP<RISCV64> server { machine, RSP_PORT };
			auto client = server.accept(10);
			if (client) {
				while (client->process_one());
			}
		});
		gdb = std::make_unique<GdbClient>(RSP_PORT);
	}
	~RspSession() {
		gdb.reset(); // Disconnect, ending the session
		server_thread.join();
	}
};

TEST_CASE("RSP: Breakpoints, stepping, registers and exit", "[RSP]")
{
	const auto binary = build_and_load(test_program, "-O1 -static");
	RspSession s { binary };
	auto& gdb = *s.gdb;
	auto& machine = s.machine;
	const auto add_addr = machine.address_of("add");
	const auto counter_addr = machine.address_of("counter");
	REQUIRE(add_addr != 0);
	REQUIRE(counter_addr != 0);

	auto features = gdb.request("qSupported:multiprocess+;swbreak+;hwbreak+;vContSupported+");
	REQUIRE_THAT(features, ContainsSubstring("qXfer:features:read+"));
	REQUIRE_THAT(features, ContainsSubstring("QStartNoAckMode+"));
	REQUIRE_THAT(features, ContainsSubstring("qXfer:auxv:read+"));

	REQUIRE(gdb.request("QStartNoAckMode") == "OK");
	gdb.noack = true;

	// Target description
	auto xml = gdb.request("qXfer:features:read:target.xml:0,3fff");
	REQUIRE_THAT(xml, StartsWith("l"));
	REQUIRE_THAT(xml, ContainsSubstring("<architecture>riscv:rv64</architecture>"));
	REQUIRE_THAT(xml, ContainsSubstring("org.gnu.gdb.riscv.cpu"));
	REQUIRE_THAT(xml, ContainsSubstring("org.gnu.gdb.riscv.fpu"));
	// Reading in chunks
	auto xml_chunk = gdb.request("qXfer:features:read:target.xml:0,10");
	REQUIRE(xml_chunk.size() == 0x11);
	REQUIRE(xml_chunk[0] == 'm');

	// The auxiliary vector ends with AT_NULL
	auto auxv = gdb.request("qXfer:auxv:read::0,1000");
	REQUIRE(auxv[0] == 'l');
	REQUIRE((auxv.size() - 1) % 16 == 0);

	REQUIRE_THAT(gdb.request("?"), StartsWith("T05thread:1;"));
	REQUIRE(gdb.request("vCont?") == "vCont;c;C;s;S;r;t");

	// Registers: PC is at the entry point
	REQUIRE(unhex64(gdb.request("p20")) == machine.memory.start_address());
	REQUIRE(gdb.request("g").size() == (33 + 32) * 16);

	// Breakpoint at add()
	REQUIRE(gdb.request("Z0," + addr_hex(add_addr) + ",2") == "OK");
	auto stop = gdb.request("vCont;c");
	REQUIRE_THAT(stop, StartsWith("T05swbreak:;thread:1;"));
	REQUIRE(machine.cpu.pc() == add_addr);
	REQUIRE(unhex64(gdb.request("p20")) == add_addr);
	REQUIRE(unhex64(gdb.request("pa")) == 0); // add(0)

	// Single-step away from the breakpoint
	REQUIRE_THAT(gdb.request("vCont;s:1"), StartsWith("T05thread:1;"));
	REQUIRE(machine.cpu.pc() != add_addr);

	// Continue to the next call
	REQUIRE_THAT(gdb.request("vCont;c"), StartsWith("T05swbreak:;"));
	REQUIRE(unhex64(gdb.request("pa")) == 1); // add(1)

	// Write a register and read it back
	REQUIRE(gdb.request("Pb=" + hex64(0x1234)) == "OK");
	REQUIRE(machine.cpu.reg(REG_ARG1) == 0x1234);
	REQUIRE(unhex64(gdb.request("pb")) == 0x1234);

	// Memory reads, and binary writes (with escaped characters)
	auto mem = gdb.request("m" + addr_hex(counter_addr) + ",4");
	REQUIRE(mem == "00000000");
	REQUIRE(gdb.request("X" + addr_hex(counter_addr) + ",1:}\x03") == "OK"); // '#' escaped
	REQUIRE(machine.memory.read<uint8_t>(counter_addr) == '#');
	REQUIRE(gdb.request("M" + addr_hex(counter_addr) + ",4:00000000") == "OK");
	REQUIRE(machine.memory.read<uint32_t>(counter_addr) == 0);
	// Unreadable memory
	REQUIRE_THAT(gdb.request("m0,4"), StartsWith("E"));

	// A write watchpoint stops *before* the store completes
	REQUIRE(gdb.request("z0," + addr_hex(add_addr) + ",2") == "OK");
	REQUIRE(gdb.request("Z2," + addr_hex(counter_addr) + ",4") == "OK");
	stop = gdb.request("vCont;c");
	REQUIRE_THAT(stop, StartsWith("T05watch:" + addr_hex(counter_addr) + ";"));
	REQUIRE(machine.memory.read<uint32_t>(counter_addr) == 0);
	// Stepping completes the store
	REQUIRE(gdb.request("z2," + addr_hex(counter_addr) + ",4") == "OK");
	REQUIRE_THAT(gdb.request("vCont;s"), StartsWith("T05"));
	REQUIRE(machine.memory.read<uint32_t>(counter_addr) == 1);

	// Read watchpoint triggers on the next load of counter
	REQUIRE(gdb.request("Z3," + addr_hex(counter_addr) + ",4") == "OK");
	REQUIRE_THAT(gdb.request("vCont;c"), StartsWith("T05rwatch:" + addr_hex(counter_addr) + ";"));
	REQUIRE(gdb.request("z3," + addr_hex(counter_addr) + ",4") == "OK");

	// The ebreak in the program stops with PC after the instruction
	stop = gdb.request("vCont;c");
	REQUIRE_THAT(stop, StartsWith("T05thread:1;"));
	const auto pc = machine.cpu.pc();
	const auto prev = machine.memory.read<uint16_t>(pc - 2);
	const bool after_ebreak = prev == 0x9002 || machine.memory.read<uint32_t>(pc - 4) == 0x00100073;
	REQUIRE(after_ebreak);

	// Exit status: 0 + 1 + 2 + 10
	REQUIRE(gdb.request("vCont;c") == "W0d");
}

TEST_CASE("RSP: Faults are reported as signals at the faulting instruction", "[RSP]")
{
	const auto binary = build_and_load(R"M(
	__attribute__((noinline))
	void crash(volatile int* ptr) { *ptr = 1234; }
	int main() {
		crash((volatile int*)0x16);
		return 0;
	})M", "-O1 -static");
	RspSession s { binary };
	auto& gdb = *s.gdb;
	gdb.request("QStartNoAckMode");
	gdb.noack = true;

	// The exception text is printed on the GDB console first
	auto console = gdb.request("vCont;c");
	REQUIRE(console[0] == 'O');
	auto stop = gdb.receive();
	REQUIRE_THAT(stop, StartsWith("T0bthread:1;")); // SIGSEGV
	const auto crash_addr = s.machine.address_of("crash");
	REQUIRE(s.machine.cpu.pc() >= crash_addr);
	REQUIRE(s.machine.cpu.pc() < crash_addr + 16);
	REQUIRE(s.machine.memory.read<uint16_t>(s.machine.cpu.pc()) != 0);

	// Continuing with the signal terminates the program, like on Linux
	REQUIRE(gdb.request("vCont;C0b:1") == "X0b");
	REQUIRE(s.machine.stopped());
}

TEST_CASE("RSP: Interrupting a running program", "[RSP]")
{
	const auto binary = build_and_load(R"M(
	volatile int sink = 0;
	int main() {
		for (;;) sink++;
	})M", "-O1 -static");
	RspSession s { binary };
	auto& gdb = *s.gdb;

	gdb.send("vCont;c");
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	gdb.send_raw("\x03");
	auto stop = gdb.receive();
	REQUIRE_THAT(stop, StartsWith("T02thread:1;")); // SIGINT
	REQUIRE(s.machine.memory.read<int>(s.machine.address_of("sink")) > 0);

	// Detaching leaves the program as-is
	REQUIRE(gdb.request("D") == "OK");
	REQUIRE(!s.machine.stopped());
}
