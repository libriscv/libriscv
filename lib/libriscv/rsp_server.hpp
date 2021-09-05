#pragma once
#include "machine.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <stdarg.h>
#include <unistd.h>

/**
  The ‘org.gnu.gdb.riscv.cpu’ feature is required
  for RISC-V targets. It should contain the registers
  ‘x0’ through ‘x31’, and ‘pc’. Either the
  architectural names (‘x0’, ‘x1’, etc) can be used,
  or the ABI names (‘zero’, ‘ra’, etc).

  The ‘org.gnu.gdb.riscv.fpu’ feature is optional.
  If present, it should contain registers ‘f0’ through
  ‘f31’, ‘fflags’, ‘frm’, and ‘fcsr’. As with the cpu
  feature, either the architectural register names,
  or the ABI names can be used.

  The ‘org.gnu.gdb.riscv.virtual’ feature is optional.
  If present, it should contain registers that are not
  backed by real registers on the target, but are
  instead virtual, where the register value is
  derived from other target state. In many ways these
  are like GDBs pseudo-registers, except implemented
  by the target. Currently the only register expected
  in this set is the one byte ‘priv’ register that
  contains the target’s privilege level in the least
  significant two bits.

  The ‘org.gnu.gdb.riscv.csr’ feature is optional.
  If present, it should contain all of the targets
  standard CSRs. Standard CSRs are those defined in
  the RISC-V specification documents. There is some
  overlap between this feature and the fpu feature;
  the ‘fflags’, ‘frm’, and ‘fcsr’ registers could
  be in either feature. The expectation is that these
  registers will be in the fpu feature if the target
  has floating point hardware, but can be moved into
  the csr feature if the target has the floating
  point control registers, but no other floating
  point hardware.
**/

namespace riscv {
template <int W> struct RSPClient;

template <int W>
struct RSP
{
	// Wait for a connection for @timeout_secs
	std::unique_ptr<RSPClient<W>> accept(int timeout_secs = 10);
	int  fd() const noexcept { return server_fd; }

	RSP(riscv::Machine<W>&, uint16_t);
	~RSP();

private:
	riscv::Machine<W>& m_machine;
	int server_fd;
};
template <int W>
struct RSPClient
{
	using StopFunc = riscv::Function<void(RSPClient<W>&)>;
	bool is_closed() const noexcept { return m_closed; }

	bool process_one();
	bool send(const char* str);
	bool sendf(const char* fmt, ...);
	void reply_ack();
	void reply_ok();
	void interrupt();
	void kill();

	auto& machine() { return *m_machine; }
	void set_machine(Machine<W>& m) { m_machine = &m; }
	void set_instruction_limit(uint64_t limit) { m_ilimit = limit; }
	void set_verbose(bool v) { m_verbose = v; }
	void on_stopped(StopFunc f) { m_on_stopped = f; }

	RSPClient(riscv::Machine<W>& m, int fd);
	~RSPClient();

private:
	static constexpr char lut[] = "0123456789abcdef";
	static const int PACKET_SIZE = 1200;
	template <typename T>
	inline void putreg(char*& d, const char* end, const T& reg);
	int forge_packet(char* dst, size_t dstlen, const char*, int);
	int forge_packet(char* dst, size_t dstlen, const char*, va_list);
	void process_data();
	void handle_query();
	void handle_breakpoint();
	void handle_continue();
	void handle_step();
	void handle_executing();
	void handle_multithread();
	void handle_readmem();
	void handle_writereg();
	void handle_writemem();
	void report_gprs();
	void report_status();
	void close_now();
	riscv::Machine<W>* m_machine;
	uint64_t m_ilimit = 100'000;
	int  sockfd;
	bool m_closed  = false;
	bool m_verbose = false;
	std::string buffer;
	riscv::address_type<W> m_bp = 0;
	StopFunc m_on_stopped = nullptr;
};

template <int W>
RSP<W>::RSP(riscv::Machine<W>& m, uint16_t port)
	: m_machine{m}
{
	this->server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

	int opt = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
		&opt, sizeof(opt))) {
		close(server_fd);
		throw std::runtime_error("Failed to enable REUSEADDR/PORT");
	}
	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);
	if (bind(server_fd, (struct sockaddr*) &address,
			sizeof(address)) < 0) {
		close(server_fd);
		throw std::runtime_error("GDB listener failed to bind to port");
	}
	if (listen(server_fd, 2) < 0) {
		close(server_fd);
		throw std::runtime_error("GDB listener failed to listen on port");
	}
}
template <int W>
std::unique_ptr<RSPClient<W>> RSP<W>::accept(int timeout_secs)
{
	struct timeval tv {
		.tv_sec = timeout_secs,
		.tv_usec = 0
	};
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(server_fd, &fds);

	const int ret = select(server_fd + 1, &fds, NULL, NULL, &tv);
	if (ret <= 0) {
		return nullptr;
	}

	struct sockaddr_in address;
	int addrlen = sizeof(address);
	int sockfd = ::accept(server_fd, (struct sockaddr*) &address,
			(socklen_t*) &addrlen);
	if (sockfd < 0) {
		return nullptr;
	}
	// Disable Nagle
	int opt = 1;
	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
		close(sockfd);
		return nullptr;
	}
	// Enable receive and send timeouts
	tv = {
		.tv_sec = 60,
		.tv_usec = 0
	};
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO | SO_SNDTIMEO, &tv, sizeof(tv))) {
		close(sockfd);
		return nullptr;
	}
	return std::make_unique<RSPClient<W>>(m_machine, sockfd);
}
template <int W> inline
RSP<W>::~RSP() {
	close(server_fd);
}

template <int W> inline
RSPClient<W>::RSPClient(riscv::Machine<W>& m, int fd)
	: m_machine{&m}, sockfd(fd)  {}
template <int W> inline
RSPClient<W>::~RSPClient() {
	if (!is_closed())
		close(this->sockfd);
}

template <int W> inline
void RSPClient<W>::close_now() {
	this->m_closed = true;
	close(this->sockfd);
}
template <int W>
int RSPClient<W>::forge_packet(
	char* dst, size_t dstlen, const char* data, int datalen)
{
	char* d = dst;
	*d++ = '$';
	uint8_t csum = 0;
	for (int i = 0; i < datalen; i++) {
		uint8_t c = data[i];
		if (c == '$' || c == '#' || c == '*' || c == '}') {
			c ^= 0x20;
			csum += '}';
			*d++ = '}';
		}
		*d++ = c;
		csum += c;
	}
	*d++ = '#';
	*d++ = lut[(csum >> 4) & 0xF];
	*d++ = lut[(csum >> 0) & 0xF];
	return d - dst;
}
template <int W>
int RSPClient<W>::forge_packet(
	char* dst, size_t dstlen, const char* fmt, va_list args)
{
	char data[4 + 2*PACKET_SIZE];
	int datalen = vsnprintf(data, sizeof(data), fmt, args);
	return forge_packet(dst, dstlen, data, datalen);
}
template <int W>
bool RSPClient<W>::sendf(const char* fmt, ...)
{
	char buffer[PACKET_SIZE];
	va_list args;
	va_start(args, fmt);
	int plen = forge_packet(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	if (UNLIKELY(m_verbose)) {
		printf("TX >>> %.*s\n", plen, buffer);
	}
	int len = ::write(sockfd, buffer, plen);
	if (len <= 0) {
		this->close_now();
		return false;
	}
	// Acknowledgement
	int rlen = ::read(sockfd, buffer, 1);
	if (rlen <= 0) {
		this->close_now();
		return false;
	}
	return (buffer[0] == '+');
}
template <int W>
bool RSPClient<W>::send(const char* str)
{
	char buffer[PACKET_SIZE];
	int plen = forge_packet(buffer, sizeof(buffer), str, strlen(str));
	if (UNLIKELY(m_verbose)) {
		printf("TX >>> %.*s\n", plen, buffer);
	}
	int len = ::write(sockfd, buffer, plen);
	if (len <= 0) {
		this->close_now();
		return false;
	}
	// Acknowledgement
	int rlen = ::read(sockfd, buffer, 1);
	if (rlen <= 0) {
		this->close_now();
		return false;
	}
	return (buffer[0] == '+');
}
template <int W>
bool RSPClient<W>::process_one()
{
	char tmp[1024];
	int len = ::read(this->sockfd, tmp, sizeof(tmp));
	if (len <= 0) {
		this->close_now();
		return false;
	}
	if (UNLIKELY(m_verbose)) {
		printf("RX <<< %.*s\n", len, tmp);
	}
	for (int i = 0; i < len; i++)
	{
		char c = tmp[i];
		if (buffer.empty() && c == '+') {
			/* Ignore acks? */
		}
		else if (c == '$') {
			this->buffer.clear();
		}
		else if (c == '#') {
			reply_ack();
			process_data();
			this->buffer.clear();
			i += 2;
		}
		else {
			this->buffer.append(&c, 1);
			if (buffer.size() >= PACKET_SIZE)
				break;
		}
	}
	return true;
}
template <int W>
void RSPClient<W>::process_data()
{
	switch (buffer[0]) {
	case 'q':
		handle_query();
		break;
	case 'c':
		handle_continue();
		break;
	case 's':
		handle_step();
		break;
	case 'g':
		report_gprs();
		break;
	case 'D':
	case 'k':
		kill();
		return;
	case 'H':
		handle_multithread();
		break;
	case 'm':
		handle_readmem();
		break;
	case 'P':
		handle_writereg();
		break;
	case 'v':
		handle_executing();
		break;
	case 'X':
		handle_writemem();
		break;
	case 'Z':
	case 'z':
		handle_breakpoint();
		break;
	case '?':
		report_status();
		break;
	default:
		if (UNLIKELY(m_verbose)) {
			fprintf(stderr, "Unhandled packet: %c\n",
				buffer[0]);
		}
	}
}
template <int W>
void RSPClient<W>::handle_query()
{
	if (strncmp("qSupported", buffer.data(), strlen("qSupported")) == 0)
	{
		sendf("PacketSize=%x;swbreak-;hwbreak+", PACKET_SIZE);
	}
	else if (strncmp("qAttached", buffer.data(), strlen("qC")) == 0)
	{
		send("1");
	}
	else if (strncmp("qC", buffer.data(), strlen("qC")) == 0)
	{
		// Current thread ID
		send("QC0");
	}
	else if (strncmp("qOffsets", buffer.data(), strlen("qOffsets")) == 0)
	{
		// Section relocation offsets
		send("Text=0;Data=0;Bss=0");
	}
	else if (strncmp("qfThreadInfo", buffer.data(), strlen("qfThreadInfo")) == 0)
	{
		// Start of threads list
		send("m0");
	}
	else if (strncmp("qsThreadInfo", buffer.data(), strlen("qfThreadInfo")) == 0)
	{
		// End of threads list
		send("l");
	}
	else if (strncmp("qSymbol::", buffer.data(), strlen("qSymbol::")) == 0)
	{
		send("OK");
	}
	else if (strncmp("qTStatus", buffer.data(), strlen("qTStatus")) == 0)
	{
		send("");
	}
	else {
		if (UNLIKELY(m_verbose)) {
			fprintf(stderr, "Unknown query: %s\n",
				buffer.data());
		}
		send("");
	}
}
template <int W>
void RSPClient<W>::handle_continue()
{
	try {
		if (m_bp == m_machine->cpu.pc()) {
			send("S05");
			return;
		}
		uint64_t n = m_ilimit;
		while (!m_machine->stopped()) {
			m_machine->cpu.simulate();
			m_machine->increment_counter(1);
			// Breakpoint
			if (m_machine->cpu.pc() == this->m_bp)
				break;
			// Instruction limit
			if (n-- == 0)
				break;
		}
	} catch (const std::exception& e) {
		fprintf(stderr, "Exception: %s\n", e.what());
		send("S01");
		return;
	}
	report_status();
}
template <int W>
void RSPClient<W>::handle_step()
{
	try {
		if (!m_machine->stopped()) {
			m_machine->cpu.simulate();
			m_machine->increment_counter(1);
		} else {
			send("S00");
			return;
		}
	} catch (const std::exception& e) {
		fprintf(stderr, "Exception: %s\n", e.what());
		send("S01");
		return;
	}
	report_status();
}
template <int W>
void RSPClient<W>::handle_breakpoint()
{
	uint32_t type = 0;
	uint64_t addr = 0;
	sscanf(&buffer[1], "%x,%lx", &type, &addr);
	if (buffer[0] == 'Z') {
		this->m_bp = addr;
	} else {
		this->m_bp = 0;
	}
	reply_ok();
}
template <int W>
void RSPClient<W>::handle_executing()
{
	if (strncmp("vCont?", buffer.data(), strlen("vCont?")) == 0)
	{
		send("vCont;c;s");
	}
	else if (strncmp("vCont;c", buffer.data(), strlen("vCont;c")) == 0)
	{
		this->handle_continue();
	}
	else if (strncmp("vCont;s", buffer.data(), strlen("vCont;s")) == 0)
	{
		this->handle_step();
	}
	else if (strncmp("vKill", buffer.data(), strlen("vKill")) == 0)
	{
		this->kill();
	}
	else if (strncmp("vMustReplyEmpty", buffer.data(), strlen("vMustReplyEmpty")) == 0)
	{
		send("");
	}
	else {
		if (UNLIKELY(m_verbose)) {
			fprintf(stderr, "Unknown executor: %s\n",
				buffer.data());
		}
		send("");
	}
}
template <int W>
void RSPClient<W>::handle_multithread() {
	reply_ok();
}
template <int W>
void RSPClient<W>::handle_readmem()
{
	uint64_t addr = 0;
	uint32_t len = 0;
	sscanf(buffer.c_str(), "m%lx,%x", &addr, &len);
	if (len >= 500) {
		send("E01");
		return;
	}

	char data[1024];
	char* d = data;
	try {
		for (unsigned i = 0; i < len; i++) {
			uint8_t val =
			m_machine->memory.template read<uint8_t> (addr + i);
			*d++ = lut[(val >> 4) & 0xF];
			*d++ = lut[(val >> 0) & 0xF];
		}
	} catch (...) {
		send("E01");
		return;
	}
	*d++ = 0;
	send(data);
}
template <int W>
void RSPClient<W>::handle_writemem()
{
	uint64_t addr = 0;
	uint32_t len = 0;
	int ret = sscanf(buffer.c_str(), "X%lx,%x:", &addr, &len);
	if (ret <= 0) {
		send("E01");
		return;
	}
	char* bin = (char*)
		memchr(buffer.data(), ':', buffer.size());
	if (bin == nullptr) {
		send("E01");
		return;
	}
	bin += 1; // Move past colon
	const char* end = buffer.c_str() + buffer.size();
	uint32_t rlen = std::min(len, (uint32_t) (end - bin));
	try {
		for (auto i = 0u; i < rlen; i++) {
			char data = bin[i];
			if (data == '{' && i+1 < rlen) {
				data = bin[++i] ^ 0x20;
			}
			m_machine->memory.template write<uint8_t> (addr+i, data);
		}
		reply_ok();
	} catch (...) {
		send("E01");
	}
}
template <int W>
void RSPClient<W>::report_status()
{
	if (!m_machine->stopped())
		send("S05"); /* Just send TRAP */
	else {
		if (m_on_stopped != nullptr) {
			m_on_stopped(*this);
		} else {
			//send("vStopped");
			send("S05"); /* Just send TRAP */
		}
	}
}
template <int W>
template <typename T>
void RSPClient<W>::putreg(char*& d, const char* end, const T& reg)
{
	for (auto j = 0u; j < sizeof(reg) && d < end; j++) {
		*d++ = lut[(reg >> (j*8+4)) & 0xF];
		*d++ = lut[(reg >> (j*8+0)) & 0xF];
	}
}

template <int W>
void RSPClient<W>::handle_writereg()
{
	uint64_t value = 0;
	uint32_t idx = 0;
	sscanf(buffer.c_str(), "P%x=%lx", &idx, &value);
	value = __builtin_bswap64(value);

	if (idx < 32) {
		m_machine->cpu.reg(idx) = value;
		send("OK");
	} else if (idx == 32) {
		m_machine->cpu.jump(value);
		send("OK");
	} else {
		send("E01");
	}
}

template <int W>
void RSPClient<W>::report_gprs()
{
	auto& regs = m_machine->cpu.registers();
	char data[1024];
	char* d = data;
	/* GPRs */
	for (int i = 0; i < 32; i++) {
		putreg(d, &data[sizeof(data)], regs.get(i));
	}
	/* PC */
	putreg(d, &data[sizeof(data)], regs.pc);
	*d++ = 0;
	send(data);
}

template <int W> inline
void RSPClient<W>::reply_ack() {
	write(sockfd, "+", 1);
}
template <int W> inline
void RSPClient<W>::reply_ok() {
	send("OK");
}
template <int W>
void RSPClient<W>::interrupt() {
	send("S05");
}
template <int W>
void RSPClient<W>::kill() {
	close(sockfd);
}

} // riscv
