#include "machine.hpp"
#include "native_heap.hpp"
#include "rv32i_instr.hpp"
#include "threads.hpp"
#include "util/auxvec.hpp"
#include <time.h>
#include <random>

namespace riscv
{
	template <int W>
	typename Machine<W>::printer_func Machine<W>::m_default_printer =
		[] (const char* buffer, size_t len) {
			printf("%.*s", (int)len, buffer);
		};

	template <int W>
	inline Machine<W>::Machine(std::string_view binary, const MachineOptions<W>& options)
		: cpu(*this), memory(*this, binary, options), m_arena{nullptr}, m_mt{nullptr}
	{
		cpu.reset();
	}
	template <int W>
	inline Machine<W>::Machine(const Machine& other, const MachineOptions<W>& options)
		: cpu(*this, other), memory(*this, other, options), m_arena{nullptr}, m_mt{nullptr}
	{
		this->m_counter = other.m_counter;
		this->m_max_counter = other.m_max_counter;
	}

	template <int W>
	inline Machine<W>::Machine(const std::vector<uint8_t>& bin, const MachineOptions<W>& opts)
		: Machine(std::string_view{(char*) bin.data(), bin.size()}, opts) {}

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

	template <int W> __attribute__((cold))
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
		return elf_offset<W, typename riscv::Elf<W>::Ehdr> (machine, 0);
	}


	template <int W> static inline
	void push_arg(Machine<W>& m, std::vector<address_type<W>>& vec, address_type<W>& dst, const std::string& str)
	{
		dst -= str.size();
		dst &= ~(address_type<W>)(W-1); // maintain alignment
		vec.push_back(dst);
		m.copy_to_guest(dst, str.data(), str.size()+1);
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
		auto gen = std::default_random_engine(time(0));
		std::uniform_int_distribution<int> rand(0,256);

		std::array<uint8_t, 16> canary;
		std::generate(canary.begin(), canary.end(), [&] { return rand(gen); });
		push_down(*this, dst, canary.data(), canary.size());
		const auto canary_addr = dst;

		const char* platform = (W == 4) ? "RISC-V 32-bit" : "RISC-V 64-bit";
		push_down(*this, dst, platform, strlen(platform)+1);
		const auto platform_addr = dst;

		// Program headers
		const auto* binary_ehdr = elf_header<W> (*this);
		const auto* binary_phdr = elf_offset<W, typename riscv::Elf<W>::Phdr> (*this, binary_ehdr->e_phoff);
		const unsigned phdr_count = binary_ehdr->e_phnum;
		for (unsigned i = 0; i < phdr_count; i++)
		{
			const auto* phd = &binary_phdr[i];
			push_down(*this, dst, phd, sizeof(typename riscv::Elf<W>::Phdr));
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
		push_aux<W>(argv, {AT_PHENT, sizeof(*binary_phdr)});
		push_aux<W>(argv, {AT_PHDR,  phdr_location});
		push_aux<W>(argv, {AT_PHNUM, phdr_count});

		// Misc
		push_aux<W>(argv, {AT_BASE, 0});
		push_aux<W>(argv, {AT_FLAGS, 0});
		push_aux<W>(argv, {AT_ENTRY, this->memory.start_address()});
		push_aux<W>(argv, {AT_HWCAP, 0});
		push_aux<W>(argv, {AT_UID, 0});
		push_aux<W>(argv, {AT_EUID, 0});
		push_aux<W>(argv, {AT_GID, 0});
		push_aux<W>(argv, {AT_EGID, 0});
		push_aux<W>(argv, {AT_SECURE, 1}); // indeed ;)

		push_aux<W>(argv, {AT_PLATFORM, platform_addr});

		// supplemental randomness
		push_aux<W>(argv, {AT_RANDOM, canary_addr});
		push_aux<W>(argv, {AT_NULL, 0});

		// from this point on the stack is starting, pointing @ argc
		// install the arg vector
		const size_t argsize = argv.size() * sizeof(argv[0]);
		dst -= argsize;
		dst &= ~0xF; // mandated 16-byte stack alignment
		this->copy_to_guest(dst, argv.data(), argsize);
		// re-initialize machine stack-pointer
		this->cpu.reg(REG_SP) = dst;
	}

	uint64_t u64_monotonic_time()
	{
		struct timespec tp;
		clock_gettime(CLOCK_MONOTONIC, &tp);
		return tp.tv_sec * 1000000000ull + tp.tv_nsec;
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
			case 0x7FF: // Stop machine
				this->stop();
				return;
			}
			break;
		case 0x1: // CSRRW
		case 0x2: // CSRRS
			// if destination is x0, then we do not write to rd
			bool rd = instr.Itype.rd != 0;
			bool wr = instr.Itype.rs1 != 0;
			switch (instr.Itype.imm)
			{
			case 0x001: // fflags (accrued exceptions)
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().fflags;
				if (wr) cpu.registers().fcsr().fflags = cpu.reg(instr.Itype.rs1);
				return;
			case 0x002: // frm (rounding-mode)
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().frm;
				if (wr) cpu.registers().fcsr().frm = cpu.reg(instr.Itype.rs1);
				return;
			case 0x003: // fcsr (control and status register)
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().whole;
				if (wr) cpu.registers().fcsr().whole = cpu.reg(instr.Itype.rs1);
				return;
			case 0xC00: // CSR RDCYCLE (lower)
			case 0xC02: // RDINSTRET (lower)
				if (rd) cpu.reg(instr.Itype.rd) = this->instruction_counter();
				return;
			case 0xC80: // CSR RDCYCLE (upper)
			case 0xC82: // RDINSTRET (upper)
				if (rd) cpu.reg(instr.Itype.rd) = this->instruction_counter() >> 32u;
				return;
			case 0xC01: // CSR RDTIME (lower)
				if (rd) cpu.reg(instr.Itype.rd) = u64_monotonic_time();
				return;
			case 0xC81: // CSR RDTIME (upper)
				if (rd) cpu.reg(instr.Itype.rd) = u64_monotonic_time() >> 32u;
				return;
			default:
				on_unhandled_csr(*this, instr.Itype.imm, instr.Itype.rd, instr.Itype.rs1);
			}
			break;
		}
		// if we got here, its an illegal operation!
		cpu.trigger_exception(ILLEGAL_OPERATION);
	}

	template struct Machine<4>;
	template struct Machine<8>;
	template struct Machine<16>;
}
