#pragma once
#include "machine.hpp"
#include "rv32i_instr.hpp"
#include "threads.hpp"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <inttypes.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/**
  A GDB Remote Serial Protocol (RSP) server for libriscv.

  Connect with: gdb-multiarch program.elf -ex "target remote :2159"

  Supported features:
  - A target description (qXfer:features:read) exposing the machine's
    integer, floating-point, vector and counter registers to GDB
  - Unlimited software breakpoints (Z0/Z1), with stop reasons (swbreak)
  - Write watchpoints (Z2), reported *before* the store completes,
    as GDB expects on RISC-V (have_nonsteppable_watchpoint)
  - Single-stepping, range-stepping (vCont;r), continuing and Ctrl-C
    interruption of a running program
  - Program exit (W) and CPU exceptions reported as signals:
    SIGSEGV, SIGILL, SIGBUS, SIGTRAP (ebreak), SIGXCPU (instruction limit)
  - Threads (when POSIX threads are enabled): thread lists, per-thread
    registers (Hg) and thread names in 'info threads'
  - The auxiliary vector (qXfer:auxv:read) for relocating PIE symbols
  - An optional executable path (qXfer:exec-file:read), allowing GDB to
    load symbols automatically
  - QStartNoAckMode, binary memory writes (X) and hex writes (M)

  Execution under the debugger is always precise: instructions execute
  individually, bypassing the decoder cache, so faults are reported at
  the exact instruction that caused them.
**/

namespace riscv {
#ifndef WIN32
	typedef int socket_fd_type;
#else
	typedef uint64_t socket_fd_type;
#endif
template <int W> struct RSPClient;

template <int W>
struct RSP
{
	// Wait for a connection for @timeout_secs
	std::unique_ptr<RSPClient<W>> accept(int timeout_secs = 30);
	socket_fd_type  fd() const noexcept { return server_fd; }

	RSP(riscv::Machine<W>&, uint16_t);
	~RSP();

private:
	riscv::Machine<W>& m_machine;
	socket_fd_type server_fd;
};

template <int W>
struct RSPClient
{
	using address_t = address_type<W>;
	using register_t = register_type<W>;
	using StopFunc = std::function<void(RSPClient<W>&)>;
	using PrinterFunc = void(*)(const Machine<W>&, const char*, size_t);
	bool is_closed() const noexcept { return m_closed; }

	/// @brief Wait for incoming packets and process them.
	/// @return False when the connection is closed.
	bool process_one();
	/// @brief Send a packet with the given payload.
	bool send(const char* str);
	bool send(std::string_view str);
	bool sendf(const char* fmt, ...);
	/// @brief Send text to be printed on the GDB console.
	bool console_print(std::string_view text);
	void reply_ack();
	void reply_ok();
	/// @brief Report that the program stopped with SIGTRAP.
	void interrupt();
	/// @brief Close the connection.
	void kill();

	auto& machine() { return *m_machine; }
	void set_machine(Machine<W>& m) { m_machine = &m; }
	/// @brief Limit the number of instructions executed during a single
	/// continue command. Reaching the limit stops the program with SIGXCPU.
	/// No limit is set by default. Use Ctrl-C in GDB to interrupt execution.
	void set_instruction_limit(uint64_t limit) { m_ilimit = limit; }
	void set_verbose(bool v) { m_verbose = v; }
	/// @brief Set the callback invoked when the program stops (e.g., exits)
	/// and GDB expects a stop reply. The default behavior reports program
	/// termination with the value in A0 as the exit status.
	void on_stopped(StopFunc f) { m_on_stopped = f; }
	/// @brief Provide the executable path so GDB can load symbols automatically.
	void set_exec_file(std::string path) { m_exec_file = std::move(path); }

	// Debug printer (for printing exceptions)
	void debug_print(const char*, size_t) const;
	auto& get_debug_printer() const noexcept { return m_debug_printer; }
	void set_debug_printer(PrinterFunc pf) noexcept { m_debug_printer = pf; }

	RSPClient(riscv::Machine<W>& m, socket_fd_type fd);
	~RSPClient();

private:
	static constexpr char lut[] = "0123456789abcdef";
	static constexpr size_t PACKET_SIZE = 0x4000;
	// GDB register numbers (see gdb/riscv-tdep.h and features/riscv/*.xml)
	static constexpr unsigned REG_PC_NUM     = 32;
	static constexpr unsigned REG_FP0_NUM    = 33;
	static constexpr unsigned REG_CSR0_NUM   = 65;
	static constexpr unsigned REG_FFLAGS_NUM = REG_CSR0_NUM + 0x001;
	static constexpr unsigned REG_FRM_NUM    = REG_CSR0_NUM + 0x002;
	static constexpr unsigned REG_FCSR_NUM   = REG_CSR0_NUM + 0x003;
	static constexpr unsigned REG_VSTART_NUM = REG_CSR0_NUM + 0x008;
	static constexpr unsigned REG_VXSAT_NUM  = REG_CSR0_NUM + 0x009;
	static constexpr unsigned REG_VXRM_NUM   = REG_CSR0_NUM + 0x00A;
	static constexpr unsigned REG_VCSR_NUM   = REG_CSR0_NUM + 0x00F;
	static constexpr unsigned REG_CYCLE_NUM  = REG_CSR0_NUM + 0xC00;
	static constexpr unsigned REG_INSTRET_NUM = REG_CSR0_NUM + 0xC02;
	static constexpr unsigned REG_VL_NUM     = REG_CSR0_NUM + 0xC20;
	static constexpr unsigned REG_VTYPE_NUM  = REG_CSR0_NUM + 0xC21;
	static constexpr unsigned REG_VLENB_NUM  = REG_CSR0_NUM + 0xC22;
	static constexpr unsigned REG_V0_NUM     = REG_CSR0_NUM + 4096 + 1;
	// Signals as GDB numbers them (gdb/signals.def)
	static constexpr int GDB_SIGINT  = 2;
	static constexpr int GDB_SIGILL  = 4;
	static constexpr int GDB_SIGTRAP = 5;
	static constexpr int GDB_SIGABRT = 6;
	static constexpr int GDB_SIGBUS  = 10;
	static constexpr int GDB_SIGSEGV = 11;
	static constexpr int GDB_SIGXCPU = 24;

	enum class ResumeMode { Continue, Step, Range };
	enum WatchType : int { WATCH_WRITE = 2, WATCH_READ = 3, WATCH_ACCESS = 4 };
	struct Watchpoint {
		int       type;
		address_t addr;
		size_t    len;
		std::vector<uint8_t> value; // Write watchpoints only
	};
	struct MemoryAccess {
		address_t addr = 0;
		unsigned  len = 0;
		bool      read = false;
		bool      write = false;
	};

	// OS-specific I/O
	bool write_raw(const char* data, size_t len);
	// Return the number of bytes read, 0 on timeout, or -1 on error or disconnect
	int  read_raw(char* buffer, size_t len, int timeout_ms);
	void close_now();

	bool process_buffered();
	void process_data();
	void handle_query();
	void handle_set_query();
	void handle_breakpoint();
	void handle_continue();
	void handle_step();
	void handle_executing();
	void handle_vcont();
	void handle_multithread();
	void handle_thread_alive();
	void handle_readmem();
	void handle_readreg();
	void handle_writereg();
	void handle_writeregs();
	void handle_writemem_binary();
	void handle_writemem_hex();
	void handle_detach();
	void handle_kill();
	void handle_qxfer(std::string_view object, std::string_view annex, size_t offset, size_t length);
	void report_gprs();
	void report_status();
	void report_stopped();
	void send_stop_reply(int signal, const std::string& reason = {});
	void resume(ResumeMode, address_t range_begin = 0, address_t range_end = 0);
	bool deliver_signal(uint64_t signal);
	void handle_exception(const std::exception&);
	bool poll_interrupt();

	int  current_tid() const;
	Registers<W>* registers_for(int tid);
	bool read_register(Registers<W>&, unsigned regno, std::string& hex);
	bool write_register(Registers<W>&, unsigned regno, std::string_view hex);
	size_t read_memory(address_t addr, uint8_t* dst, size_t len);
	bool watchpoint_changed(const Watchpoint&);
	static bool decode_memory_access(rv32i_instruction, const Registers<W>&, MemoryAccess&);
	std::string target_description() const;
	bool build_auxv(std::string& out);
	void qxfer_reply(const std::string& doc, size_t offset, size_t length);

	riscv::Machine<W>* m_machine;
	uint64_t m_ilimit = UINT64_MAX;
	socket_fd_type  sockfd;
	bool m_closed  = false;
	bool m_verbose = false;
	bool m_noack   = false;
	int  m_gthread = 0; // Thread selected by Hg, 0 = the current thread
	std::string m_input;
	std::string m_packet;
	std::string m_last_packet;
	std::string m_exec_file;
	std::unordered_set<address_t> m_bp;
	std::vector<Watchpoint> m_watchpoints;
	StopFunc m_on_stopped = nullptr;
	mutable PrinterFunc m_debug_printer = [](const Machine<W>&, const char*, size_t) {};
};
} // riscv

// Socket operations for RSP<W> and RSPClient<W> have OS-specific implementations
#ifndef WIN32
#include "linux/rsp_server.hpp"
#else
#include "win32/rsp_server.hpp"
#endif

namespace riscv {
namespace rsp_detail {
	inline int unhex(char c) noexcept {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}
	inline void append_hex(std::string& out, const void* vdata, size_t len) {
		static constexpr char lut[] = "0123456789abcdef";
		const auto* data = (const uint8_t*)vdata;
		for (size_t i = 0; i < len; i++) {
			out.push_back(lut[data[i] >> 4]);
			out.push_back(lut[data[i] & 0xF]);
		}
	}
	// Decode hex into exactly len bytes at vout
	inline bool parse_hex_bytes(std::string_view hex, void* vout, size_t len) {
		if (hex.size() < len * 2) return false;
		auto* out = (uint8_t*)vout;
		for (size_t i = 0; i < len; i++) {
			const int hi = unhex(hex[i*2]), lo = unhex(hex[i*2+1]);
			if (hi < 0 || lo < 0) return false;
			out[i] = uint8_t(hi << 4 | lo);
		}
		return true;
	}
	// Parse a big-endian hex number and remove it from sv
	inline bool parse_hex_number(std::string_view& sv, uint64_t& out) {
		out = 0;
		size_t i = 0;
		for (; i < sv.size() && i < 16; i++) {
			const int v = unhex(sv[i]);
			if (v < 0) break;
			out = (out << 4) | v;
		}
		sv.remove_prefix(i);
		return i > 0;
	}
	inline bool starts_with(std::string_view sv, std::string_view prefix) {
		return sv.substr(0, prefix.size()) == prefix;
	}
	inline bool consume(std::string_view& sv, std::string_view prefix) {
		if (!starts_with(sv, prefix)) return false;
		sv.remove_prefix(prefix.size());
		return true;
	}
} // rsp_detail

template <int W> inline
RSPClient<W>::RSPClient(riscv::Machine<W>& m, socket_fd_type fd)
	: m_machine{&m}, sockfd(fd)
{
	// Clear the machine's stopped state so GDB can resume execution
	m_machine->set_max_instructions(UINT64_MAX);
}

template <int W>
bool RSPClient<W>::send(std::string_view data)
{
	if (m_closed) return false;
	std::string pkt;
	pkt.reserve(data.size() + 8);
	pkt.push_back('$');
	uint8_t csum = 0;
	for (const char cc : data) {
		char c = cc;
		if (c == '$' || c == '#' || c == '*' || c == '}') {
			pkt.push_back('}');
			csum += '}';
			c ^= 0x20;
		}
		pkt.push_back(c);
		csum += uint8_t(c);
	}
	pkt.push_back('#');
	pkt.push_back(lut[(csum >> 4) & 0xF]);
	pkt.push_back(lut[(csum >> 0) & 0xF]);
	if (UNLIKELY(m_verbose)) {
		printf("TX >>> %.*s\n", (int)std::min(pkt.size(), size_t(512)), pkt.c_str());
	}
	const bool ok = write_raw(pkt.data(), pkt.size());
	if (!m_noack)
		m_last_packet = std::move(pkt);
	return ok;
}
template <int W>
bool RSPClient<W>::send(const char* str)
{
	return send(std::string_view(str));
}
template <int W>
bool RSPClient<W>::sendf(const char* fmt, ...)
{
	char data[4096];
	va_list args;
	va_start(args, fmt);
	const int len = vsnprintf(data, sizeof(data), fmt, args);
	va_end(args);
	if (len < 0) return false;
	return send(std::string_view(data, std::min(size_t(len), sizeof(data)-1)));
}
template <int W>
bool RSPClient<W>::console_print(std::string_view text)
{
	std::string pkt = "O";
	rsp_detail::append_hex(pkt, text.data(), text.size());
	return send(pkt);
}
template <int W> inline
void RSPClient<W>::reply_ack() {
	if (!m_noack)
		write_raw("+", 1);
}
template <int W> inline
void RSPClient<W>::reply_ok() {
	send("OK");
}
template <int W>
void RSPClient<W>::interrupt() {
	send_stop_reply(GDB_SIGTRAP);
}
template <int W>
void RSPClient<W>::kill() {
	if (!m_closed)
		close_now();
}

template <int W>
bool RSPClient<W>::process_one()
{
	if (m_closed) return false;
	// Packets may already be buffered (e.g., received during program execution)
	if (process_buffered())
		return !m_closed;

	char tmp[4096];
	const int len = read_raw(tmp, sizeof(tmp), -1);
	if (len <= 0) {
		if (len < 0) this->close_now();
		return !m_closed;
	}
	if (UNLIKELY(m_verbose)) {
		printf("RX <<< %.*s\n", std::min(len, 512), tmp);
	}
	m_input.append(tmp, len);
	process_buffered();
	return !m_closed;
}

template <int W>
bool RSPClient<W>::process_buffered()
{
	bool processed = false;
	while (!m_closed && !m_input.empty())
	{
		// Skip acknowledgements, and handle out-of-band bytes
		size_t i = 0;
		while (i < m_input.size() && m_input[i] != '$') {
			const char c = m_input[i++];
			if (c == '\x03') {
				// Interrupt while already stopped: nothing to do
			} else if (c == '-' && !m_last_packet.empty()) {
				write_raw(m_last_packet.data(), m_last_packet.size());
			}
		}
		m_input.erase(0, i);
		if (m_input.empty())
			break;

		const size_t end = m_input.find('#');
		if (end == std::string::npos || end + 2 >= m_input.size()) {
			// Incomplete packet. Guard against unbounded growth.
			if (m_input.size() > 4 * PACKET_SIZE) {
				this->close_now();
			}
			break;
		}
		// Verify checksum and unescape
		uint8_t csum = 0;
		std::string payload;
		payload.reserve(end);
		for (size_t j = 1; j < end; j++) {
			const char c = m_input[j];
			csum += uint8_t(c);
			if (c == '}' && j + 1 < end) {
				csum += uint8_t(m_input[++j]);
				payload.push_back(m_input[j] ^ 0x20);
			} else {
				payload.push_back(c);
			}
		}
		const int hi = rsp_detail::unhex(m_input[end+1]);
		const int lo = rsp_detail::unhex(m_input[end+2]);
		m_input.erase(0, end + 3);
		if (!m_noack && (hi < 0 || lo < 0 || uint8_t(hi << 4 | lo) != csum)) {
			write_raw("-", 1);
			continue;
		}
		reply_ack();
		m_packet = std::move(payload);
		processed = true;
		if (!m_packet.empty()) {
			try {
				process_data();
			} catch (const std::exception& e) {
				if (UNLIKELY(m_verbose))
					fprintf(stderr, "Exception while handling packet %s: %s\n", m_packet.c_str(), e.what());
				send("E01");
			}
		}
	}
	return processed;
}

template <int W>
void RSPClient<W>::process_data()
{
	switch (m_packet[0]) {
	case 'q':
		handle_query();
		break;
	case 'Q':
		handle_set_query();
		break;
	case 'c':
	case 'C':
		handle_continue();
		break;
	case 's':
	case 'S':
		handle_step();
		break;
	case 'g':
		report_gprs();
		break;
	case 'G':
		handle_writeregs();
		break;
	case 'D':
		handle_detach();
		return;
	case 'k':
		handle_kill();
		return;
	case 'H':
		handle_multithread();
		break;
	case 'T':
		handle_thread_alive();
		break;
	case 'm':
		handle_readmem();
		break;
	case 'M':
		handle_writemem_hex();
		break;
	case 'p':
		handle_readreg();
		break;
	case 'P':
		handle_writereg();
		break;
	case 'v':
		handle_executing();
		break;
	case 'X':
		handle_writemem_binary();
		break;
	case 'Z':
	case 'z':
		handle_breakpoint();
		break;
	case '?':
		report_status();
		break;
	case '!': // Extended mode is not supported
	default:
		if (UNLIKELY(m_verbose)) {
			fprintf(stderr, "Unhandled packet: %s\n", m_packet.c_str());
		}
		send("");
	}
}

template <int W>
void RSPClient<W>::handle_query()
{
	using namespace rsp_detail;
	std::string_view q = m_packet;
	if (starts_with(q, "qSupported"))
	{
		std::string features = "PacketSize=" + std::to_string(PACKET_SIZE);
		features += ";QStartNoAckMode+;swbreak+;hwbreak+;vContSupported+";
		features += ";qXfer:features:read+";
		std::string auxv;
		if (build_auxv(auxv))
			features += ";qXfer:auxv:read+";
		if (!m_exec_file.empty())
			features += ";qXfer:exec-file:read+";
		send(features);
	}
	else if (starts_with(q, "qAttached"))
	{
		send("1");
	}
	else if (q == "qC")
	{
		sendf("QC%x", current_tid());
	}
	else if (starts_with(q, "qfThreadInfo"))
	{
		std::string list = "m";
		if (m_machine->has_threads()) {
			std::vector<int> tids;
			for (const auto& it : m_machine->threads().m_threads)
				tids.push_back(it.first);
			std::sort(tids.begin(), tids.end());
			for (size_t i = 0; i < tids.size(); i++) {
				char buf[16];
				snprintf(buf, sizeof(buf), "%s%x", (i > 0) ? "," : "", tids[i]);
				list += buf;
			}
		} else {
			list += "1";
		}
		send(list);
	}
	else if (starts_with(q, "qsThreadInfo"))
	{
		send("l");
	}
	else if (consume(q, "qThreadExtraInfo,"))
	{
		uint64_t tid = 0;
		parse_hex_number(q, tid);
		std::string info;
		if (m_machine->has_threads()) {
			auto& mt = m_machine->threads();
			if (int(tid) == current_tid())
				info = "active";
			else {
				info = "suspended";
				for (const auto* t : mt.blocked_threads())
					if (t->tid == int(tid)) info = "blocked";
			}
		}
		std::string reply;
		append_hex(reply, info.data(), info.size());
		send(reply);
	}
	else if (consume(q, "qXfer:"))
	{
		// qXfer:object:read:annex:offset,length
		const size_t c1 = q.find(':');
		if (c1 == q.npos) { send("E00"); return; }
		const auto object = q.substr(0, c1);
		q.remove_prefix(c1 + 1);
		if (!consume(q, "read:")) { send(""); return; }
		const size_t c2 = q.find(':');
		if (c2 == q.npos) { send("E00"); return; }
		const auto annex = q.substr(0, c2);
		q.remove_prefix(c2 + 1);
		uint64_t offset = 0, length = 0;
		if (!parse_hex_number(q, offset) || !consume(q, ",") || !parse_hex_number(q, length)) {
			send("E00");
			return;
		}
		handle_qxfer(object, annex, offset, length);
	}
	else if (starts_with(q, "qSymbol::"))
	{
		send("OK");
	}
	else {
		if (UNLIKELY(m_verbose)) {
			fprintf(stderr, "Unknown query: %s\n", m_packet.c_str());
		}
		send("");
	}
}
template <int W>
void RSPClient<W>::handle_set_query()
{
	if (m_packet == "QStartNoAckMode") {
		reply_ok();
		m_noack = true;
		m_last_packet.clear();
	} else {
		send("");
	}
}

template <int W>
void RSPClient<W>::qxfer_reply(const std::string& doc, size_t offset, size_t length)
{
	if (offset >= doc.size()) {
		send("l");
		return;
	}
	length = std::min(length, PACKET_SIZE - 16);
	const size_t n = std::min(length, doc.size() - offset);
	std::string reply;
	reply.reserve(n + 1);
	reply.push_back((offset + n < doc.size()) ? 'm' : 'l');
	reply.append(doc, offset, n);
	send(reply);
}

template <int W>
void RSPClient<W>::handle_qxfer(std::string_view object, std::string_view annex, size_t offset, size_t length)
{
	if (object == "features") {
		if (annex != "target.xml") {
			send("E00");
			return;
		}
		qxfer_reply(target_description(), offset, length);
	}
	else if (object == "auxv") {
		std::string auxv;
		if (!build_auxv(auxv)) {
			send("E01");
			return;
		}
		qxfer_reply(auxv, offset, length);
	}
	else if (object == "exec-file" && !m_exec_file.empty()) {
		qxfer_reply(m_exec_file, offset, length);
	}
	else {
		send("");
	}
}

template <int W>
std::string RSPClient<W>::target_description() const
{
	const unsigned xlen = (W == 4) ? 32 : 64;
	static const char* gpr_names[32] = {
		"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
		"fp", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
		"a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
		"s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };
	static const char* fpr_names[32] = {
		"ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
		"fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
		"fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
		"fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11" };
	std::string x;
	char buf[512];
	x += "<?xml version=\"1.0\"?>\n<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n<target version=\"1.0\">\n";
	x += (W == 4) ? "<architecture>riscv:rv32</architecture>\n" : "<architecture>riscv:rv64</architecture>\n";
	x += "<osabi>GNU/Linux</osabi>\n";

	x += "<feature name=\"org.gnu.gdb.riscv.cpu\">\n";
	for (unsigned i = 0; i < 32; i++) {
		const char* type = "int";
		if (i == 1) type = "code_ptr";
		else if (i == 2 || i == 3 || i == 4 || i == 8) type = "data_ptr";
		snprintf(buf, sizeof(buf), "  <reg name=\"%s\" bitsize=\"%u\" type=\"%s\" regnum=\"%u\"/>\n",
			gpr_names[i], xlen, type, i);
		x += buf;
	}
	snprintf(buf, sizeof(buf), "  <reg name=\"pc\" bitsize=\"%u\" type=\"code_ptr\" regnum=\"%u\"/>\n", xlen, REG_PC_NUM);
	x += buf;
	x += "</feature>\n";

	// libriscv always has 64-bit floating-point registers (F+D)
	x += "<feature name=\"org.gnu.gdb.riscv.fpu\">\n";
	x += "  <union id=\"riscv_double\">\n"
		 "    <field name=\"float\" type=\"ieee_single\"/>\n"
		 "    <field name=\"double\" type=\"ieee_double\"/>\n"
		 "  </union>\n";
	for (unsigned i = 0; i < 32; i++) {
		snprintf(buf, sizeof(buf), "  <reg name=\"%s\" bitsize=\"64\" type=\"riscv_double\" regnum=\"%u\"/>\n",
			fpr_names[i], REG_FP0_NUM + i);
		x += buf;
	}
	snprintf(buf, sizeof(buf),
		"  <reg name=\"fflags\" bitsize=\"32\" type=\"int\" regnum=\"%u\" group=\"float\"/>\n"
		"  <reg name=\"frm\" bitsize=\"32\" type=\"int\" regnum=\"%u\" group=\"float\"/>\n"
		"  <reg name=\"fcsr\" bitsize=\"32\" type=\"int\" regnum=\"%u\" group=\"float\"/>\n",
		REG_FFLAGS_NUM, REG_FRM_NUM, REG_FCSR_NUM);
	x += buf;
	x += "</feature>\n";

	x += "<feature name=\"org.gnu.gdb.riscv.csr\">\n";
	snprintf(buf, sizeof(buf),
		"  <reg name=\"cycle\" bitsize=\"%u\" type=\"uint%u\" regnum=\"%u\" group=\"system\"/>\n"
		"  <reg name=\"instret\" bitsize=\"%u\" type=\"uint%u\" regnum=\"%u\" group=\"system\"/>\n",
		xlen, xlen, REG_CYCLE_NUM, xlen, xlen, REG_INSTRET_NUM);
	x += buf;
#ifdef RISCV_EXT_VECTOR
	snprintf(buf, sizeof(buf),
		"  <reg name=\"vstart\" bitsize=\"%u\" type=\"int\" regnum=\"%u\" group=\"vector\"/>\n"
		"  <reg name=\"vxsat\" bitsize=\"%u\" type=\"int\" regnum=\"%u\" group=\"vector\"/>\n"
		"  <reg name=\"vxrm\" bitsize=\"%u\" type=\"int\" regnum=\"%u\" group=\"vector\"/>\n",
		xlen, REG_VSTART_NUM, xlen, REG_VXSAT_NUM, xlen, REG_VXRM_NUM);
	x += buf;
	snprintf(buf, sizeof(buf),
		"  <reg name=\"vcsr\" bitsize=\"%u\" type=\"int\" regnum=\"%u\" group=\"vector\"/>\n"
		"  <reg name=\"vl\" bitsize=\"%u\" type=\"int\" regnum=\"%u\" group=\"vector\"/>\n"
		"  <reg name=\"vtype\" bitsize=\"%u\" type=\"int\" regnum=\"%u\" group=\"vector\"/>\n"
		"  <reg name=\"vlenb\" bitsize=\"%u\" type=\"int\" regnum=\"%u\" group=\"vector\"/>\n",
		xlen, REG_VCSR_NUM, xlen, REG_VL_NUM, xlen, REG_VTYPE_NUM, xlen, REG_VLENB_NUM);
	x += buf;
#endif
	x += "</feature>\n";

#ifdef RISCV_EXT_VECTOR
	{
		const unsigned vlenb = VectorLane::size();
		x += "<feature name=\"org.gnu.gdb.riscv.vector\">\n";
		snprintf(buf, sizeof(buf),
			"  <vector id=\"bytes\" type=\"uint8\" count=\"%u\"/>\n"
			"  <vector id=\"shorts\" type=\"uint16\" count=\"%u\"/>\n"
			"  <vector id=\"words\" type=\"uint32\" count=\"%u\"/>\n"
			"  <vector id=\"longs\" type=\"uint64\" count=\"%u\"/>\n",
			vlenb, vlenb / 2, vlenb / 4, vlenb / 8);
		x += buf;
		snprintf(buf, sizeof(buf),
			"  <vector id=\"floats\" type=\"ieee_single\" count=\"%u\"/>\n"
			"  <vector id=\"doubles\" type=\"ieee_double\" count=\"%u\"/>\n",
			vlenb / 4, vlenb / 8);
		x += buf;
		x += "  <union id=\"riscv_vector\">\n"
			 "    <field name=\"b\" type=\"bytes\"/>\n"
			 "    <field name=\"s\" type=\"shorts\"/>\n"
			 "    <field name=\"w\" type=\"words\"/>\n"
			 "    <field name=\"l\" type=\"longs\"/>\n"
			 "    <field name=\"f\" type=\"floats\"/>\n"
			 "    <field name=\"d\" type=\"doubles\"/>\n"
			 "  </union>\n";
		for (unsigned i = 0; i < 32; i++) {
			snprintf(buf, sizeof(buf), "  <reg name=\"v%u\" bitsize=\"%u\" type=\"riscv_vector\" regnum=\"%u\" group=\"vector\"/>\n",
				i, vlenb * 8, REG_V0_NUM + i);
			x += buf;
		}
		x += "</feature>\n";
	}
#endif
	x += "</target>\n";
	return x;
}

template <int W>
bool RSPClient<W>::build_auxv(std::string& out)
{
	// The Linux ABI stack: argc, argv[], NULL, envp[], NULL, auxv[] ending with AT_NULL
	try {
		const address_t sp = m_machine->memory.stack_initial();
		if (sp == 0) return false;
		auto& mem = m_machine->memory;
		const address_t argc = mem.template read<address_t>(sp);
		if (argc > 4096) return false;
		address_t p = sp + sizeof(address_t) * (argc + 2); // Skip argc, argv and NULL
		for (unsigned i = 0; mem.template read<address_t>(p) != 0; i++) {
			if (i > 4096) return false;
			p += sizeof(address_t);
		}
		p += sizeof(address_t); // Skip NULL
		out.clear();
		for (unsigned i = 0; i < 64; i++) {
			const address_t type = mem.template read<address_t>(p);
			const address_t value = mem.template read<address_t>(p + sizeof(address_t));
			if (type > 64) return false; // Not an auxiliary vector
			out.append((const char*)&type, sizeof(type));
			out.append((const char*)&value, sizeof(value));
			p += 2 * sizeof(address_t);
			if (type == 0) // AT_NULL
				return true;
		}
	} catch (...) {}
	return false;
}

template <int W>
int RSPClient<W>::current_tid() const
{
	if (m_machine->has_threads())
		return m_machine->threads().get_tid();
	return 1;
}
template <int W>
Registers<W>* RSPClient<W>::registers_for(int tid)
{
	if (tid <= 0 || tid == current_tid())
		return &m_machine->cpu.registers();
	if (m_machine->has_threads()) {
		if (auto* t = m_machine->threads().get_thread(tid))
			return &t->stored_regs;
	}
	return nullptr;
}

template <int W>
void RSPClient<W>::send_stop_reply(int signal, const std::string& reason)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "T%02x", signal);
	std::string reply = buf;
	reply += reason;
	snprintf(buf, sizeof(buf), "thread:%x;", current_tid());
	reply += buf;
	// Include the registers GDB needs to display the current location immediately
	auto& regs = m_machine->cpu.registers();
	static constexpr unsigned expedite[] = { REG_PC_NUM, 1, 2, 8 };
	for (const unsigned regno : expedite) {
		snprintf(buf, sizeof(buf), "%02x:", regno);
		reply += buf;
		read_register(regs, regno, reply);
		reply += ';';
	}
	m_gthread = 0;
	send(reply);
}

template <int W>
void RSPClient<W>::report_stopped()
{
	if (m_on_stopped != nullptr) {
		m_on_stopped(*this);
	} else {
		sendf("W%02x", unsigned(m_machine->template return_value<int>()) & 0xFF);
	}
}

template <int W>
void RSPClient<W>::report_status()
{
	if (m_machine->stopped())
		report_stopped();
	else
		send_stop_reply(GDB_SIGTRAP);
}

template <int W>
bool RSPClient<W>::poll_interrupt()
{
	char tmp[256];
	const int len = read_raw(tmp, sizeof(tmp), 0);
	if (len < 0) {
		this->close_now();
		return true;
	}
	bool interrupted = false;
	for (int i = 0; i < len; i++) {
		if (tmp[i] == '\x03')
			interrupted = true;
		else
			m_input.push_back(tmp[i]);
	}
	return interrupted;
}

template <int W>
bool RSPClient<W>::watchpoint_changed(const Watchpoint& wp)
{
	uint8_t tmp[256];
	size_t done = 0;
	while (done < wp.len) {
		const size_t chunk = std::min(wp.len - done, sizeof(tmp));
		if (read_memory(wp.addr + done, tmp, chunk) != chunk)
			return false;
		if (std::memcmp(tmp, &wp.value[done], chunk) != 0)
			return true;
		done += chunk;
	}
	return false;
}

template <int W>
bool RSPClient<W>::decode_memory_access(rv32i_instruction instr, const Registers<W>& regs, MemoryAccess& out)
{
	// Scalar loads, stores and atomics, including the compressed forms.
	// Vector memory operations are not decoded.
	const uint32_t bits = instr.whole;
	if ((bits & 3) != 3) {
		if constexpr (!compressed_enabled) return false;
		const uint32_t c = bits & 0xFFFF;
		const unsigned quadrant = c & 3, funct3 = c >> 13;
		unsigned offset = 0;
		if (quadrant == 0) {
			// c.fld, c.lw, c.ld/c.flw, c.fsd, c.sw, c.sd/c.fsw
			switch (funct3) {
			case 2: case 6: // Word
				offset = ((c >> 7) & 0x38) | ((c >> 4) & 0x4) | ((c << 1) & 0x40);
				out.len = 4;
				break;
			case 1: case 5: // Double
				offset = ((c >> 7) & 0x38) | ((c << 1) & 0xC0);
				out.len = 8;
				break;
			case 3: case 7: // RV64: c.ld/c.sd, RV32: c.flw/c.fsw
				if (W == 4) {
					offset = ((c >> 7) & 0x38) | ((c >> 4) & 0x4) | ((c << 1) & 0x40);
					out.len = 4;
				} else {
					offset = ((c >> 7) & 0x38) | ((c << 1) & 0xC0);
					out.len = 8;
				}
				break;
			default:
				return false;
			}
			out.addr = regs.get(8 + ((c >> 7) & 7)) + offset;
		} else if (quadrant == 2) {
			// Loads and stores relative to the stack pointer
			switch (funct3) {
			case 2: // c.lwsp
				offset = ((c >> 7) & 0x20) | ((c >> 2) & 0x1C) | ((c << 4) & 0xC0);
				out.len = 4;
				break;
			case 1: // c.fldsp
				offset = ((c >> 7) & 0x20) | ((c >> 2) & 0x18) | ((c << 4) & 0x1C0);
				out.len = 8;
				break;
			case 3: // RV64: c.ldsp, RV32: c.flwsp
				if (W == 4) {
					offset = ((c >> 7) & 0x20) | ((c >> 2) & 0x1C) | ((c << 4) & 0xC0);
					out.len = 4;
				} else {
					offset = ((c >> 7) & 0x20) | ((c >> 2) & 0x18) | ((c << 4) & 0x1C0);
					out.len = 8;
				}
				break;
			case 6: // c.swsp
				offset = ((c >> 7) & 0x3C) | ((c >> 1) & 0xC0);
				out.len = 4;
				break;
			case 5: // c.fsdsp
				offset = ((c >> 7) & 0x38) | ((c >> 1) & 0x1C0);
				out.len = 8;
				break;
			case 7: // RV64: c.sdsp, RV32: c.fswsp
				if (W == 4) {
					offset = ((c >> 7) & 0x3C) | ((c >> 1) & 0xC0);
					out.len = 4;
				} else {
					offset = ((c >> 7) & 0x38) | ((c >> 1) & 0x1C0);
					out.len = 8;
				}
				break;
			default:
				return false;
			}
			out.addr = regs.get(2) + offset;
		} else {
			return false;
		}
		out.read  = funct3 < 4;
		out.write = funct3 >= 5;
		return true;
	}
	const unsigned opcode = bits & 0x7F;
	const unsigned funct3 = (bits >> 12) & 7;
	const address_t rs1 = regs.get((bits >> 15) & 31);
	const int32_t imm_i = int32_t(bits) >> 20;
	const int32_t imm_s = ((int32_t(bits) >> 25) << 5) | ((bits >> 7) & 0x1F);
	// FP loads and stores share their opcode with vector loads and stores
	static constexpr unsigned fp_len[8] = { 0, 2, 4, 8, 16, 0, 0, 0 };
	switch (opcode) {
	case 0b0000011: // LOAD
		out = { address_t(rs1 + imm_i), 1u << (funct3 & 3), true, false };
		return true;
	case 0b0100011: // STORE
		out = { address_t(rs1 + imm_s), 1u << (funct3 & 3), false, true };
		return true;
	case 0b0000111: // LOAD-FP
		if (fp_len[funct3] == 0) return false;
		out = { address_t(rs1 + imm_i), fp_len[funct3], true, false };
		return true;
	case 0b0100111: // STORE-FP
		if (fp_len[funct3] == 0) return false;
		out = { address_t(rs1 + imm_s), fp_len[funct3], false, true };
		return true;
	case 0b0101111: { // AMO
		const unsigned funct5 = bits >> 27;
		out.addr = rs1;
		out.len  = 1u << (funct3 & 7);
		out.read  = funct5 != 0b00011; // Not SC
		out.write = funct5 != 0b00010; // Not LR
		return true;
	}
	default:
		return false;
	}
}

template <int W>
void RSPClient<W>::resume(ResumeMode mode, address_t range_begin, address_t range_end)
{
	auto& m = *m_machine;
	auto& cpu = m.cpu;
	if (m.max_instructions() == 0) {
		report_stopped();
		return;
	}
	m.set_max_instructions(UINT64_MAX);
	bool watch_writes = false, watch_access = false;
	for (const auto& wp : m_watchpoints) {
		if (wp.type == WATCH_WRITE) watch_writes = true;
		else watch_access = true;
	}
	std::array<register_t, 32> saved_regs {};
	address_t saved_pc = 0;
	uint64_t budget = m_ilimit;

	try {
		for (uint64_t n = 0; ; n++)
		{
			const address_t pc = cpu.pc();
			const auto instr = cpu.read_next_instruction();
			const bool is_ebreak = instr.whole == 0x00100073
				|| (compressed_enabled && (instr.whole & 0xFFFF) == 0x9002);
			if (UNLIKELY(is_ebreak)) {
				// A breakpoint compiled into the program. Like int3 on x86,
				// the program stops with PC pointing to the next instruction
				// so execution can resume. If resuming from this breakpoint
				// (e.g., GDB connected in an EBREAK system call handler), skip it.
				cpu.registers().pc = pc + instr.length();
				m.increment_counter(1);
				if (n != 0 && mode != ResumeMode::Step) {
					send_stop_reply(GDB_SIGTRAP);
					return;
				}
			} else {
				// Read and access watchpoints trigger before the instruction
				MemoryAccess access;
				if (watch_access && decode_memory_access(instr, cpu.registers(), access)) {
					for (const auto& wp : m_watchpoints) {
						if (wp.type == WATCH_WRITE)
							continue;
						if (access.addr < wp.addr + wp.len && wp.addr < access.addr + access.len
							&& (access.read || wp.type == WATCH_ACCESS)) {
							char reason[64];
							snprintf(reason, sizeof(reason), "%s:%" PRIx64 ";",
								(wp.type == WATCH_READ) ? "rwatch" : "awatch", uint64_t(wp.addr));
							send_stop_reply(GDB_SIGTRAP, reason);
							return;
						}
					}
				}
				if (watch_writes) {
					saved_regs = cpu.registers().get();
					saved_pc = pc;
				}
				cpu.execute(instr);
				if constexpr (compressed_enabled)
					cpu.registers().pc += instr.length();
				else
					cpu.registers().pc += 4;
				m.increment_counter(1);

				// The program exited (or the machine was stopped)
				if (UNLIKELY(m.max_instructions() == 0)) {
					report_stopped();
					return;
				}

				if (watch_writes) {
					Watchpoint* hit = nullptr;
					for (auto& wp : m_watchpoints) {
						if (wp.type == WATCH_WRITE && watchpoint_changed(wp)) {
							hit = &wp;
							break;
						}
					}
					if (hit != nullptr) {
						// GDB expects RISC-V watchpoints to trigger *before* the
						// store completes, then single-steps over it. Undo
						// a plain store, but not system calls or atomics, which
						// may have side effects outside of the watched memory.
						const unsigned opcode = instr.whole & 0x7F;
						if ((instr.whole & 3) != 3 || (opcode != 0b1110011 && opcode != 0b0101111)) {
							cpu.registers().get() = saved_regs;
							cpu.registers().pc = saved_pc;
							m.set_instruction_counter(m.instruction_counter() - 1);
							// Restore the previous values for all changed watchpoints
							for (auto& wp : m_watchpoints) {
								if (wp.type == WATCH_WRITE && watchpoint_changed(wp))
									m.memory.memcpy(wp.addr, wp.value.data(), wp.len);
							}
						} else {
							for (auto& wp : m_watchpoints) {
								if (wp.type == WATCH_WRITE)
									read_memory(wp.addr, wp.value.data(), wp.len);
							}
						}
						char reason[64];
						snprintf(reason, sizeof(reason), "watch:%" PRIx64 ";", uint64_t(hit->addr));
						send_stop_reply(GDB_SIGTRAP, reason);
						return;
					}
				}
			}

			const address_t new_pc = cpu.pc();
			if (mode == ResumeMode::Step) {
				send_stop_reply(GDB_SIGTRAP);
				return;
			}
			if (mode == ResumeMode::Range && (new_pc < range_begin || new_pc >= range_end)) {
				send_stop_reply(GDB_SIGTRAP);
				return;
			}
			if (!m_bp.empty() && m_bp.count(new_pc)) {
				send_stop_reply(GDB_SIGTRAP, "swbreak:;");
				return;
			}
			if (UNLIKELY(--budget == 0)) {
				console_print("Instruction limit reached\n");
				send_stop_reply(GDB_SIGXCPU);
				return;
			}
			if (UNLIKELY((n & 0xFFFF) == 0xFFFF)) {
				if (poll_interrupt()) {
					if (!m_closed)
						send_stop_reply(GDB_SIGINT);
					return;
				}
			}
		}
	} catch (const std::exception& e) {
		handle_exception(e);
	}
}

template <int W>
void RSPClient<W>::handle_exception(const std::exception& e)
{
	int signal = GDB_SIGABRT;
	std::string text = "Exception: ";
	if (auto* me = dynamic_cast<const MachineException*>(&e)) {
		switch (me->type()) {
		case PROTECTION_FAULT:
		case EXECUTION_SPACE_PROTECTION_FAULT:
		case OUT_OF_MEMORY:
			signal = GDB_SIGSEGV;
			break;
		case ILLEGAL_OPCODE:
		case ILLEGAL_OPERATION:
		case UNIMPLEMENTED_INSTRUCTION:
		case UNIMPLEMENTED_INSTRUCTION_LENGTH:
		case FEATURE_DISABLED:
			signal = GDB_SIGILL;
			break;
		case MISALIGNED_INSTRUCTION:
		case INVALID_ALIGNMENT:
			signal = GDB_SIGBUS;
			break;
		case MAX_INSTRUCTIONS_REACHED:
			signal = GDB_SIGXCPU;
			break;
		default:
			break;
		}
		char buf[256];
		snprintf(buf, sizeof(buf), "%s (data: 0x%" PRIX64 ")\n", me->what(), uint64_t(me->data()));
		text += buf;
	} else {
		text += e.what();
		text += "\n";
	}
	this->debug_print(text.data(), text.size());
	console_print(text);
	send_stop_reply(signal);
}

template <int W>
bool RSPClient<W>::deliver_signal(uint64_t signal)
{
	// GDB forwards the signal when resuming after a signal stop (e.g., SIGSEGV).
	// No signal handlers run under the debugger, so the default action is
	// to terminate the program, as on Linux.
	if (signal == 0 || signal == GDB_SIGINT || signal == GDB_SIGTRAP)
		return false;
	m_machine->stop();
	sendf("X%02x", unsigned(signal) & 0xFF);
	return true;
}

template <int W>
void RSPClient<W>::handle_continue()
{
	// c[addr] or C sig[;addr]
	std::string_view sv = m_packet;
	const bool with_signal = sv[0] == 'C';
	sv.remove_prefix(1);
	if (with_signal) {
		uint64_t signal = 0;
		rsp_detail::parse_hex_number(sv, signal);
		if (deliver_signal(signal))
			return;
		const size_t semi = sv.find(';');
		sv = (semi != sv.npos) ? sv.substr(semi + 1) : std::string_view{};
	}
	uint64_t addr = 0;
	if (rsp_detail::parse_hex_number(sv, addr))
		m_machine->cpu.jump(addr);
	resume(ResumeMode::Continue);
}
template <int W>
void RSPClient<W>::handle_step()
{
	std::string_view sv = m_packet;
	const bool with_signal = sv[0] == 'S';
	sv.remove_prefix(1);
	if (with_signal) {
		uint64_t signal = 0;
		rsp_detail::parse_hex_number(sv, signal);
		if (deliver_signal(signal))
			return;
		const size_t semi = sv.find(';');
		sv = (semi != sv.npos) ? sv.substr(semi + 1) : std::string_view{};
	}
	uint64_t addr = 0;
	if (rsp_detail::parse_hex_number(sv, addr))
		m_machine->cpu.jump(addr);
	resume(ResumeMode::Step);
}

template <int W>
void RSPClient<W>::handle_vcont()
{
	using namespace rsp_detail;
	// vCont[;action[:thread-id]]... Execute the first action that applies
	// to the current thread.
	std::string_view sv = m_packet;
	sv.remove_prefix(5); // vCont
	const int tid = current_tid();
	while (consume(sv, ";")) {
		const size_t next = sv.find(';');
		std::string_view action = sv.substr(0, next);
		sv = (next != sv.npos) ? sv.substr(next) : std::string_view{};

		bool applies = true;
		const size_t colon = action.find(':');
		if (colon != action.npos) {
			std::string_view thr = action.substr(colon + 1);
			action = action.substr(0, colon);
			if (thr.find('.') != thr.npos) // Multiprocess pPID.TID
				thr = thr.substr(thr.find('.') + 1);
			if (thr != "-1") {
				uint64_t t = 0;
				parse_hex_number(thr, t);
				applies = (int(t) == tid || t == 0);
			}
		}
		if (!applies || action.empty())
			continue;
		if (action[0] == 'C' || action[0] == 'S') {
			std::string_view sig = action.substr(1);
			uint64_t signal = 0;
			parse_hex_number(sig, signal);
			if (deliver_signal(signal))
				return;
		}
		switch (action[0]) {
		case 'c':
		case 'C':
			resume(ResumeMode::Continue);
			return;
		case 's':
		case 'S':
			resume(ResumeMode::Step);
			return;
		case 'r': {
			action.remove_prefix(1);
			uint64_t begin = 0, end = 0;
			if (!parse_hex_number(action, begin) || !consume(action, ",") || !parse_hex_number(action, end)) {
				send("E01");
				return;
			}
			resume(ResumeMode::Range, begin, end);
			return;
		}
		case 't':
			send_stop_reply(GDB_SIGINT);
			return;
		}
	}
	send("E01");
}

template <int W>
void RSPClient<W>::handle_executing()
{
	using namespace rsp_detail;
	std::string_view sv = m_packet;
	if (sv == "vCont?")
	{
		send("vCont;c;C;s;S;r;t");
	}
	else if (starts_with(sv, "vCont;"))
	{
		handle_vcont();
	}
	else if (starts_with(sv, "vKill"))
	{
		reply_ok();
		handle_kill();
	}
	else if (starts_with(sv, "vCtrlC"))
	{
		reply_ok();
	}
	else {
		// Includes vMustReplyEmpty, vFile (GDB falls back to local files) and vRun
		if (UNLIKELY(m_verbose && !starts_with(sv, "vMustReplyEmpty"))) {
			fprintf(stderr, "Unknown executor: %s\n", m_packet.c_str());
		}
		send("");
	}
}

template <int W>
void RSPClient<W>::handle_detach()
{
	// The program continues running after GDB detaches
	reply_ok();
	m_bp.clear();
	m_watchpoints.clear();
	this->close_now();
}
template <int W>
void RSPClient<W>::handle_kill()
{
	m_machine->stop();
	this->close_now();
}

template <int W>
void RSPClient<W>::handle_breakpoint()
{
	using namespace rsp_detail;
	// [Zz]type,addr,kind
	std::string_view sv = m_packet;
	const bool insert = sv[0] == 'Z';
	sv.remove_prefix(1);
	uint64_t type = 0, addr = 0, kind = 0;
	if (!parse_hex_number(sv, type) || !consume(sv, ",") || !parse_hex_number(sv, addr)
		|| !consume(sv, ",") || !parse_hex_number(sv, kind)) {
		send("E01");
		return;
	}
	switch (type) {
	case 0: // Software breakpoint
	case 1: // Hardware breakpoint
		if (insert)
			m_bp.insert(addr);
		else
			m_bp.erase(addr);
		reply_ok();
		return;
	case WATCH_WRITE:
	case WATCH_READ:
	case WATCH_ACCESS: {
		if (insert) {
			if (kind == 0 || kind > 4096) {
				send("E01");
				return;
			}
			Watchpoint wp { int(type), address_t(addr), size_t(kind), {} };
			if (wp.type == WATCH_WRITE) {
				// Write watchpoints compare the previous and current values
				// to detect changes, including those made by system calls.
				wp.value.resize(wp.len);
				if (read_memory(wp.addr, wp.value.data(), wp.len) != wp.len) {
					send("E01");
					return;
				}
			}
			m_watchpoints.push_back(std::move(wp));
		} else {
			for (auto it = m_watchpoints.begin(); it != m_watchpoints.end(); ++it) {
				if (it->type == int(type) && it->addr == addr && it->len == kind) {
					m_watchpoints.erase(it);
					break;
				}
			}
		}
		reply_ok();
		return;
	}
	default:
		send("");
	}
}

template <int W>
void RSPClient<W>::handle_multithread()
{
	// Hg selects the thread for register access; Hc selects it for resuming (ignored)
	std::string_view sv = m_packet;
	if (sv.size() >= 2 && sv[1] == 'g') {
		sv.remove_prefix(2);
		if (sv == "-1" || sv == "0") {
			m_gthread = 0;
		} else {
			uint64_t tid = 0;
			rsp_detail::parse_hex_number(sv, tid);
			if (registers_for(tid) == nullptr) {
				send("E01");
				return;
			}
			m_gthread = tid;
		}
	}
	reply_ok();
}
template <int W>
void RSPClient<W>::handle_thread_alive()
{
	std::string_view sv = m_packet;
	sv.remove_prefix(1);
	uint64_t tid = 0;
	rsp_detail::parse_hex_number(sv, tid);
	if (registers_for(tid) != nullptr)
		reply_ok();
	else
		send("E01");
}

template <int W>
size_t RSPClient<W>::read_memory(address_t addr, uint8_t* dst, size_t len)
{
	auto& mem = m_machine->memory;
	try {
		mem.memcpy_out(dst, addr, len);
		return len;
	} catch (...) {}
	// Read one byte at a time, stopping at the first unreadable byte.
	// Read execute-only code from the execute segment so GDB can disassemble it.
	for (size_t i = 0; i < len; i++) {
		const address_t a = addr + i;
		try {
			dst[i] = mem.template read<uint8_t>(a);
			continue;
		} catch (...) {}
		auto& seg = mem.exec_segment_for(a);
		if (seg != nullptr && !seg->empty() && seg->is_within(a, 1)) {
			dst[i] = *(const uint8_t*)seg->exec_data(a);
			continue;
		}
		return i;
	}
	return len;
}

template <int W>
void RSPClient<W>::handle_readmem()
{
	std::string_view sv = m_packet;
	sv.remove_prefix(1);
	uint64_t addr = 0, len = 0;
	if (!rsp_detail::parse_hex_number(sv, addr) || !rsp_detail::consume(sv, ",")
		|| !rsp_detail::parse_hex_number(sv, len)) {
		send("E01");
		return;
	}
	len = std::min(len, uint64_t(PACKET_SIZE / 2 - 8));
	std::vector<uint8_t> data(len);
	const size_t got = read_memory(addr, data.data(), len);
	if (got == 0 && len > 0) {
		send("E14"); // EFAULT
		return;
	}
	std::string reply;
	rsp_detail::append_hex(reply, data.data(), got);
	send(reply);
}

template <int W>
void RSPClient<W>::handle_writemem_binary()
{
	// Xaddr,length:XX... (the payload has already been unescaped)
	std::string_view sv = m_packet;
	sv.remove_prefix(1);
	uint64_t addr = 0, len = 0;
	if (!rsp_detail::parse_hex_number(sv, addr) || !rsp_detail::consume(sv, ",")
		|| !rsp_detail::parse_hex_number(sv, len) || !rsp_detail::consume(sv, ":")
		|| sv.size() < len) {
		send("E01");
		return;
	}
	try {
		if (len > 0)
			m_machine->memory.memcpy(addr, sv.data(), len);
		reply_ok();
	} catch (...) {
		send("E14");
	}
}
template <int W>
void RSPClient<W>::handle_writemem_hex()
{
	std::string_view sv = m_packet;
	sv.remove_prefix(1);
	uint64_t addr = 0, len = 0;
	if (!rsp_detail::parse_hex_number(sv, addr) || !rsp_detail::consume(sv, ",")
		|| !rsp_detail::parse_hex_number(sv, len) || !rsp_detail::consume(sv, ":")) {
		send("E01");
		return;
	}
	std::vector<uint8_t> data(len);
	if (!rsp_detail::parse_hex_bytes(sv, data.data(), len)) {
		send("E01");
		return;
	}
	try {
		if (len > 0)
			m_machine->memory.memcpy(addr, data.data(), len);
		reply_ok();
	} catch (...) {
		send("E14");
	}
}

template <int W>
bool RSPClient<W>::read_register(Registers<W>& regs, unsigned regno, std::string& hex)
{
	using rsp_detail::append_hex;
	if (regno < 32) {
		const register_t val = regs.get(regno);
		append_hex(hex, &val, sizeof(val));
	} else if (regno == REG_PC_NUM) {
		const address_t val = regs.pc;
		append_hex(hex, &val, sizeof(val));
	} else if (regno >= REG_FP0_NUM && regno < REG_FP0_NUM + 32) {
		const auto& fl = regs.getfl(regno - REG_FP0_NUM);
		append_hex(hex, &fl.i64, sizeof(fl.i64));
	} else if (regno == REG_FFLAGS_NUM || regno == REG_FRM_NUM || regno == REG_FCSR_NUM) {
		uint32_t val = 0;
		if (regno == REG_FFLAGS_NUM) val = regs.fcsr().fflags;
		else if (regno == REG_FRM_NUM) val = regs.fcsr().frm;
		else val = regs.fcsr().whole;
		append_hex(hex, &val, sizeof(val));
	} else if (regno == REG_CYCLE_NUM || regno == REG_INSTRET_NUM) {
		const register_t val = m_machine->instruction_counter();
		append_hex(hex, &val, sizeof(val));
	}
#ifdef RISCV_EXT_VECTOR
	else if (regno >= REG_V0_NUM && regno < REG_V0_NUM + 32) {
		const auto& lane = regs.rvv().get(regno - REG_V0_NUM);
		append_hex(hex, lane.u8.data(), lane.u8.size());
	} else if (regno == REG_VSTART_NUM || regno == REG_VXSAT_NUM || regno == REG_VXRM_NUM
		|| regno == REG_VCSR_NUM || regno == REG_VL_NUM || regno == REG_VTYPE_NUM || regno == REG_VLENB_NUM) {
		const auto& rvv = regs.rvv();
		register_t val = 0;
		switch (regno) {
		case REG_VSTART_NUM: val = rvv.vstart(); break;
		case REG_VXSAT_NUM:  val = rvv.vxsat(); break;
		case REG_VXRM_NUM:   val = rvv.vxrm(); break;
		case REG_VCSR_NUM:   val = rvv.vcsr(); break;
		case REG_VL_NUM:     val = rvv.vl(); break;
		case REG_VTYPE_NUM:  val = rvv.vtype(); break;
		case REG_VLENB_NUM:  val = rvv.vlenb(); break;
		}
		append_hex(hex, &val, sizeof(val));
	}
#endif
	else {
		return false;
	}
	return true;
}

template <int W>
bool RSPClient<W>::write_register(Registers<W>& regs, unsigned regno, std::string_view hex)
{
	using rsp_detail::parse_hex_bytes;
	if (regno < 32) {
		register_t val = 0;
		if (!parse_hex_bytes(hex, &val, sizeof(val))) return false;
		if (regno != 0)
			regs.get(regno) = val;
	} else if (regno == REG_PC_NUM) {
		address_t val = 0;
		if (!parse_hex_bytes(hex, &val, sizeof(val))) return false;
		if (&regs == &m_machine->cpu.registers())
			m_machine->cpu.jump(val);
		else
			regs.pc = val;
	} else if (regno >= REG_FP0_NUM && regno < REG_FP0_NUM + 32) {
		auto& fl = regs.getfl(regno - REG_FP0_NUM);
		if (!parse_hex_bytes(hex, &fl.i64, sizeof(fl.i64))) return false;
	} else if (regno == REG_FFLAGS_NUM || regno == REG_FRM_NUM || regno == REG_FCSR_NUM) {
		uint32_t val = 0;
		if (!parse_hex_bytes(hex, &val, sizeof(val))) return false;
		if (regno == REG_FFLAGS_NUM) regs.fcsr().fflags = val;
		else if (regno == REG_FRM_NUM) regs.fcsr().frm = val;
		else regs.fcsr().whole = val;
	} else if (regno == REG_CYCLE_NUM || regno == REG_INSTRET_NUM) {
		// Read-only, ignore
	}
#ifdef RISCV_EXT_VECTOR
	else if (regno >= REG_V0_NUM && regno < REG_V0_NUM + 32) {
		auto& lane = regs.rvv().get(regno - REG_V0_NUM);
		if (!parse_hex_bytes(hex, lane.u8.data(), lane.u8.size())) return false;
	} else if (regno == REG_VSTART_NUM || regno == REG_VXSAT_NUM || regno == REG_VXRM_NUM
		|| regno == REG_VCSR_NUM) {
		register_t val = 0;
		if (!parse_hex_bytes(hex, &val, sizeof(val))) return false;
		auto& rvv = regs.rvv();
		switch (regno) {
		case REG_VSTART_NUM: rvv.set_vstart(val); break;
		case REG_VXSAT_NUM:  rvv.set_vxsat(val & 1); break;
		case REG_VXRM_NUM:   rvv.set_vxrm(val); break;
		case REG_VCSR_NUM:   rvv.set_vcsr(val); break;
		}
	} else if (regno == REG_VL_NUM || regno == REG_VTYPE_NUM || regno == REG_VLENB_NUM) {
		// Read-only, ignore
	}
#endif
	else {
		return false;
	}
	return true;
}

template <int W>
void RSPClient<W>::handle_readreg()
{
	std::string_view sv = m_packet;
	sv.remove_prefix(1);
	uint64_t regno = 0;
	auto* regs = registers_for(m_gthread);
	if (!rsp_detail::parse_hex_number(sv, regno) || regs == nullptr) {
		send("E01");
		return;
	}
	std::string reply;
	if (read_register(*regs, regno, reply))
		send(reply);
	else
		send("E01");
}
template <int W>
void RSPClient<W>::handle_writereg()
{
	std::string_view sv = m_packet;
	sv.remove_prefix(1);
	uint64_t regno = 0;
	auto* regs = registers_for(m_gthread);
	if (!rsp_detail::parse_hex_number(sv, regno) || !rsp_detail::consume(sv, "=") || regs == nullptr) {
		send("E01");
		return;
	}
	if (write_register(*regs, regno, sv))
		reply_ok();
	else
		send("E01");
}

template <int W>
void RSPClient<W>::report_gprs()
{
	auto* regs = registers_for(m_gthread);
	if (regs == nullptr) {
		send("E01");
		return;
	}
	// x0-x31, pc, f0-f31. GDB fetches the rest with 'p'.
	std::string reply;
	reply.reserve(65 * 16 + 1);
	for (unsigned i = 0; i < REG_FP0_NUM + 32; i++)
		read_register(*regs, i, reply);
	send(reply);
}
template <int W>
void RSPClient<W>::handle_writeregs()
{
	auto* regs = registers_for(m_gthread);
	if (regs == nullptr) {
		send("E01");
		return;
	}
	std::string_view sv = m_packet;
	sv.remove_prefix(1);
	for (unsigned i = 0; i < REG_FP0_NUM + 32 && !sv.empty(); i++) {
		const size_t size = (i < REG_FP0_NUM) ? sizeof(register_t) : 8;
		if (!write_register(*regs, i, sv)) {
			send("E01");
			return;
		}
		sv.remove_prefix(std::min(sv.size(), size * 2));
	}
	reply_ok();
}

template <int W>
inline void RSPClient<W>::debug_print(const char* buffer, size_t len) const
{
	this->m_debug_printer(*m_machine, buffer, len);
}

} // riscv
