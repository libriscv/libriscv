#include "machine.hpp"
#include "multiprocessing.hpp"
#include "internal_common.hpp"
#include "native_heap.hpp"
#include "rv32i_instr.hpp"
#include "threads.hpp"
#include "util/auxvec.hpp"
#include <algorithm>
#include <errno.h> // Used by emulated POSIX system calls
#include <random>
#ifdef __GNUG__ /* Workaround for GCC bug */
#include "machine_defaults.cpp"
#endif
#ifdef RISCV_TIMED_VMCALLS
#include <signal.h>
#include <sys/timerfd.h>
extern "C" int gettid();
#endif

namespace riscv
{
#if defined(__linux__) && !defined(RISCV_DISABLE_URANDOM)
	static std::random_device rd("/dev/urandom");
#else
	static std::random_device rd{};
#endif

	template <int W>
	inline Machine<W>::Machine(std::string_view binary, const MachineOptions<W>& options)
		: cpu(*this, options.cpu_id),
		  memory(*this, binary, options),
		  m_arena(nullptr)
	{
		cpu.reset();
	}
	template <int W>
	inline Machine<W>::Machine(const Machine& other, const MachineOptions<W>& options)
		: cpu(*this, options.cpu_id, other),
		  memory(*this, other, options),
		  m_arena(nullptr)
	{
		this->m_counter = other.m_counter;
		this->m_max_counter = other.m_max_counter;
		if (other.m_mt) {
			m_mt.reset(new MultiThreading {*this, *other.m_mt});
		}
		// TODO: transfer arena?
	}

	template <int W>
	inline Machine<W>::Machine(const std::vector<uint8_t>& bin, const MachineOptions<W>& opts)
		: Machine(std::string_view{(char*) bin.data(), bin.size()}, opts) {}

	template <int W>
	inline Machine<W>::Machine(const MachineOptions<W>& opts)
		: Machine(std::string_view{}, opts){}

	template <int W>
	Machine<W>::~Machine()
	{
	}

	template <int W>
	void Machine<W>::unknown_syscall_handler(Machine<W>& machine)
	{
		const auto syscall_number = machine.cpu.reg(REG_ECALL);
		machine.on_unhandled_syscall(machine, syscall_number);
	}

	template <int W>
	void Machine<W>::default_unknown_syscall_no(Machine<W>& machine, size_t num)
	{
		auto txt = "Unhandled system call: " + std::to_string(num) + "\n";
		machine.debug_print(txt.c_str(), txt.size());
	}

	template <int W>
	void Machine<W>::set_result_or_error(int result)
	{
		if (result >= 0)
			set_result(result);
		else
			set_result(-errno);
	}

	template <int W>
	void Machine<W>::penalize(uint32_t val)
	{
		m_counter += val;
	}

	template <int W> RISCV_COLD_PATH()
	void Machine<W>::timeout_exception(uint64_t max_instr)
	{
		throw MachineTimeoutException(MAX_INSTRUCTIONS_REACHED,
			"Instruction count limit reached", max_instr);
	}

	template <int W>
	void Machine<W>::setup_argv(
		const std::vector<std::string>& args,
		const std::vector<std::string>& env)
	{
		// Arguments to main()
		std::vector<address_t> argv;
		argv.push_back(args.size()); // argc
		for (const auto& string : args) {
			const auto sp = stack_push(string);
			argv.push_back(sp);
		}
		argv.push_back(0x0);
		for (const auto& string : env) {
			const auto sp = stack_push(string);
			argv.push_back(sp);
		}
		argv.push_back(0x0);

		// Extra aligned SP and copy the arguments over
		auto& sp = cpu.reg(REG_SP);
		const size_t argsize = argv.size() * sizeof(argv[0]);
		sp -= argsize;
		sp &= ~(address_t)0xF; // mandated 16-byte stack alignment

		this->copy_to_guest(sp, argv.data(), argsize);
	}

	template <int W, typename T>
	const T* elf_offset(riscv::Machine<W>& machine, intptr_t ofs) {
		return (const T*) &machine.memory.binary().at(ofs);
	}
	template <int W>
	inline const auto* elf_header(riscv::Machine<W>& machine) {
		return elf_offset<W, typename riscv::Elf<W>::Header> (machine, 0);
	}


	template <int W> static inline
	void push_arg(Machine<W>& m, std::vector<address_type<W>>& vec, address_type<W>& dst, const std::string& str)
	{
		const size_t size = str.size()+1;
		dst -= size;
		dst &= ~(address_type<W>)(W-1); // maintain alignment
		vec.push_back(dst);
		m.copy_to_guest(dst, str.data(), size);
	}
	template <int W> static inline
	void push_aux(std::vector<address_type<W>>& vec, AuxVec<address_type<W>> aux)
	{
		vec.push_back(aux.a_type);
		vec.push_back(aux.a_val);
	}
	template <int W> static inline
	void push_down(Machine<W>& m, address_type<W>& dst, const void* data, size_t size)
	{
		dst -= size;
		dst &= ~(address_type<W>)(W-1); // maintain alignment
		m.copy_to_guest(dst, data, size);
	}

	template <int W>
	void Machine<W>::setup_linux(
		const std::vector<std::string>& args,
		const std::vector<std::string>& env)
	{
		// start installing at near-end of address space, leaving room on both sides
		// stack below and installation above
		auto dst = this->cpu.reg(REG_SP);

		// inception :)
		std::uniform_int_distribution<int> rand(0,256);

		std::array<uint8_t, 16> canary;
		std::generate(canary.begin(), canary.end(), [&] { return rand(rd); });
		push_down(*this, dst, canary.data(), canary.size());
		const auto canary_addr = dst;

		const char* platform = (W == 4) ? "RISC-V 32-bit" : "RISC-V 64-bit";
		push_down(*this, dst, platform, strlen(platform)+1);
		const auto platform_addr = dst;

		// Program headers
		const auto* binary_ehdr = elf_header<W> (*this);
		const auto* binary_phdr = elf_offset<W, typename Elf<W>::ProgramHeader> (*this, binary_ehdr->e_phoff);
		const unsigned phdr_count = binary_ehdr->e_phnum;
		for (unsigned i = 0; i < phdr_count; i++)
		{
			const auto* phd = &binary_phdr[i];
			push_down(*this, dst, phd, sizeof(typename Elf<W>::ProgramHeader));
		}
		const auto phdr_location = dst;

		// Arguments to main()
		std::vector<address_type<W>> argv;
		argv.push_back(args.size()); // argc
		for (const auto& string : args) {
			push_arg(*this, argv, dst, string);
		}
		argv.push_back(0x0);

		// Environment vars
		for (const auto& string : env) {
			push_arg(*this, argv, dst, string);
		}
		argv.push_back(0x0);

		// Auxiliary vector
		push_aux<W>(argv, {AT_PAGESZ, Page::size()});
		push_aux<W>(argv, {AT_CLKTCK, 100});

		// ELF related
		push_aux<W>(argv, {AT_PHDR,  phdr_location});
		push_aux<W>(argv, {AT_PHENT, sizeof(*binary_phdr)});
		push_aux<W>(argv, {AT_PHNUM, phdr_count});

		// Misc
		push_aux<W>(argv, {AT_BASE, address_type<W>(this->memory.start_address() & ~0xFFFFFFLL)});
		push_aux<W>(argv, {AT_ENTRY, this->memory.start_address()});
		push_aux<W>(argv, {AT_HWCAP, 0});
		push_aux<W>(argv, {AT_HWCAP2, 0});
		push_aux<W>(argv, {AT_UID, 1000});
		push_aux<W>(argv, {AT_EUID, 0});
		push_aux<W>(argv, {AT_GID, 0});
		push_aux<W>(argv, {AT_EGID, 0});
		push_aux<W>(argv, {AT_SECURE, 0});

		push_aux<W>(argv, {AT_PLATFORM, platform_addr});

		// supplemental randomness
		push_aux<W>(argv, {AT_RANDOM, canary_addr});
		push_aux<W>(argv, {AT_NULL, 0});

		// from this point on the stack is starting, pointing @ argc
		// install the arg vector
		const size_t argsize = argv.size() * sizeof(argv[0]);
		dst -= argsize;
		dst &= ~0xFLL; // mandated 16-byte stack alignment
		this->copy_to_guest(dst, argv.data(), argsize);
		// re-initialize machine stack-pointer
		this->cpu.reg(REG_SP) = dst;
	}

	template <int W>
	void Machine<W>::system(union rv32i_instruction instr)
	{
		switch (instr.Itype.funct3) {
		case 0x0: // SYSTEM functions
			switch (instr.Itype.imm)
			{
			case 0: // ECALL
				this->system_call(cpu.reg(REG_ECALL));
				return;
			case 1: // EBREAK
				this->ebreak();
				return;
			case 0x105: // WFI
				this->stop();
				return;
			case 0x7FF: // Stop machine
				this->stop();
				return;
			}
			break;
		case 0x1: { // CSRRW: Atomically swap CSR and integer register
			const bool rd = instr.Itype.rd != 0;
			switch (instr.Itype.imm)
			{
			case 0x001: // fflags: accrued exceptions
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().fflags;
				cpu.registers().fcsr().fflags = cpu.reg(instr.Itype.rs1);
				return;
			case 0x002: // frm: rounding-mode
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().frm;
				cpu.registers().fcsr().frm = cpu.reg(instr.Itype.rs1);
				return;
			case 0x003: // fcsr: control and status register
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().whole;
				cpu.registers().fcsr().whole = cpu.reg(instr.Itype.rs1) & 0xFF;
				return;
			}
			[[fallthrough]];
		}
		case 0x2: { // CSRRS: Atomically read and set bit mask
			// if destination is x0, then we do not write to rd
			const bool rd = instr.Itype.rd != 0;
			switch (instr.Itype.imm)
			{
			case 0x001: // fflags (accrued exceptions)
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().fflags;
				cpu.registers().fcsr().fflags |= cpu.reg(instr.Itype.rs1);
				return;
			case 0x002: // frm (rounding-mode)
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().frm;
				cpu.registers().fcsr().frm |= cpu.reg(instr.Itype.rs1);
				return;
			case 0x003: // fcsr (control and status register)
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().whole;
				cpu.registers().fcsr().whole |= cpu.reg(instr.Itype.rs1) & 0xFF;
				return;
			case 0xC00: // CSR RDCYCLE (lower)
			case 0xC02: // RDINSTRET (lower)
				if (rd) {
					cpu.reg(instr.Itype.rd) = this->instruction_counter();
					return;
				} else {
					if (instr.Itype.rs1 == 0) // UNIMP instruction
						cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION, instr.Itype.imm);
					else // CYCLE is not writable
						cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
				}
			case 0xC80: // CSR RDCYCLE (upper)
			case 0xC82: // RDINSTRET (upper)
				if (rd) cpu.reg(instr.Itype.rd) = this->instruction_counter() >> 32u;
				return;
			case 0xC01: // CSR RDTIME (lower)
				if (rd) cpu.reg(instr.Itype.rd) = m_rdtime(*this);
				return;
			case 0xC81: // CSR RDTIME (upper)
				if (rd) cpu.reg(instr.Itype.rd) = m_rdtime(*this) >> 32u;
				return;
			case 0xF11: // CSR marchid
				if (rd) cpu.reg(instr.Itype.rd) = 0;
				return;
			case 0xF12: // CSR mvendorid
				if (rd) cpu.reg(instr.Itype.rd) = 0;
				return;
			case 0xF13: // CSR mimpid
				if (rd) cpu.reg(instr.Itype.rd) = 1;
				return;
			case 0xF14: // CSR mhartid
				if (rd) cpu.reg(instr.Itype.rd) = cpu.cpu_id();
				return;
			default:
				on_unhandled_csr(*this, instr.Itype.imm, instr.Itype.rd, instr.Itype.rs1);
				return;
			}
			} break;
		case 0x3: { // CSRRC: Atomically read and clear CSR
			const bool rd = instr.Itype.rd != 0;
			switch (instr.Itype.imm)
			{
			case 0x001: // fflags: accrued exceptions
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().fflags;
				cpu.registers().fcsr().fflags &= ~cpu.reg(instr.Itype.rs1);
				return;
			case 0x002: // frm: rounding-mode
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().frm;
				cpu.registers().fcsr().frm &= ~cpu.reg(instr.Itype.rs1);
				return;
			case 0x003: // fcsr: control and status register
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().whole;
				cpu.registers().fcsr().whole &= ~(cpu.reg(instr.Itype.rs1) & 0xFF);
				return;
			}
			break;
		}
		case 0x5: { // CSRWI: CSRW from uimm[4:0] in RS1
			const bool rd = instr.Itype.rd != 0;
			const uint32_t imm = instr.Itype.rs1;
			switch (instr.Itype.imm)
			{
			case 0x001: // fflags: accrued exceptions
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().fflags;
				cpu.registers().fcsr().fflags = imm;
				return;
			case 0x002: // frm: rounding-mode
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().frm;
				cpu.registers().fcsr().frm = imm;
				return;
			case 0x003: // fcsr: control and status register
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().whole;
				cpu.registers().fcsr().whole = imm & 0xFF;
				return;
			default:
				on_unhandled_csr(*this, instr.Itype.imm, instr.Itype.rd, instr.Itype.rs1);
				return;
			}
		} // CSRWI
		case 0x7: { // CSRRCI: Atomically read and clear CSR using immediate
			const bool rd = instr.Itype.rd != 0;
			const uint32_t imm = instr.Itype.rs1;
			switch (instr.Itype.imm)
			{
			case 0x001: // fflags: accrued exceptions
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().fflags;
				cpu.registers().fcsr().fflags &= ~imm;
				return;
			case 0x002: // frm: rounding-mode
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().frm;
				cpu.registers().fcsr().frm &= ~imm;
				return;
			case 0x003: // fcsr: control and status register
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().whole;
				cpu.registers().fcsr().whole &= ~(imm & 0xFF);
				return;
			default:
				on_unhandled_csr(*this, instr.Itype.imm, instr.Itype.rd, instr.Itype.rs1);
				return;
			}
			break;
		} // CSRRCI
		}
		// if we got here, its an illegal operation!
		cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.funct3);
	}

#ifdef RISCV_TIMED_VMCALLS
	struct ksigevent
	{
		union sigval sigev_value;
		int sigev_signo;
		int sigev_notify;
		int sigev_tid;
	};

	extern "C"
	void timed_vmcall_sighandler(int sig, siginfo_t* si, void* usr)
	{
		(void)si; (void)usr;
		if (sig == SIGUSR2) {
			throw MachineTimeoutException(MAX_INSTRUCTIONS_REACHED, "Timed out", 0);
		}
	}

	template <int W>
	void Machine<W>::execute_with_timeout(float timeout, address_t pc)
	{
		if (this->m_timer_id == nullptr)
		{
			// Establish handler for timer signal
			struct sigaction sa;
			sa.sa_flags = SA_SIGINFO | SA_NODEFER;
			sa.sa_sigaction = timed_vmcall_sighandler;
			sigemptyset(&sa.sa_mask);
			if (::sigaction(SIGUSR2, &sa, NULL) < 0) {
				throw MachineException(ILLEGAL_OPERATION, "sigaction failed");
			}

			// Create the timer
			struct ksigevent sev {};
			sev.sigev_signo = SIGUSR2;
			sev.sigev_notify = SIGEV_THREAD_ID;
			sev.sigev_tid = ::gettid();

			if (timer_create(CLOCK_MONOTONIC, (struct sigevent *)&sev, &m_timer_id) < 0) {
				throw MachineException(ILLEGAL_OPERATION, "timer_create failed");
			}
		}

		// Start the timer
		const struct itimerspec its {
			/* Interrupt every 50ms after timeout. This makes sure
			that we will eventually exit all blocking calls. If
			there is a blocking loop that doesn't exit properly,
			the 50ms recurring interruption should not cause too
			much wasted CPU-time. */
			.it_interval = {
				.tv_sec = 0, .tv_nsec = 50'000'000L
			},
			/* The execution timeout. */
			.it_value = {
				.tv_sec = (time_t) timeout,
				.tv_nsec = (long) ((timeout - (time_t) timeout) * 1'000'000'000L)
			}
		};

		if (timer_settime(m_timer_id, 0, &its, NULL) < 0) {
			throw MachineException(SYSTEM_CALL_FAILED, "timer_settime failed");
		}

		// Execute the VM call
		try {
			cpu.simulate_inaccurate(pc);
		} catch (...) {
			// Stop the timer
			disable_timer();
			throw;
		}
		// Stop the timer
		disable_timer();
	}

	template <int W>
	void Machine<W>::disable_timer()
	{
		struct itimerspec its {};
		timer_settime(this->m_timer_id, 0, &its, nullptr);
	}
#endif

	INSTANTIATE_32_IF_ENABLED(Machine);
	INSTANTIATE_64_IF_ENABLED(Machine);
	INSTANTIATE_128_IF_ENABLED(Machine);
}
