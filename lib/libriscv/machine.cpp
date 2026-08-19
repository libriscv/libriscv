#include "machine.hpp"
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

namespace riscv
{
#if defined(__linux__) && !defined(RISCV_DISABLE_URANDOM)
	static std::random_device rd("/dev/urandom");
#else
	static std::random_device rd{};
#endif

	template <int W>
	inline Machine<W>::Machine(std::string_view binary, const MachineOptions<W>& options)
		: cpu(*this),
		  memory(*this, binary, options),
		  m_arena(nullptr)
	{
		cpu.reset();
	}
	template <int W>
	inline Machine<W>::Machine(const Machine& other, const MachineOptions<W>& options)
		: cpu(*this, other, options),
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
		: Machine(std::string_view{(const char*) bin.data(), bin.size()}, opts) {}

#if RISCV_SPAN_AVAILABLE
	template <int W>
	inline Machine<W>::Machine(std::span<const uint8_t> binary, const MachineOptions<W>& options)
		: Machine(std::string_view{(const char*) binary.data(), binary.size()}, options) {}
#endif

	template <int W>
	inline Machine<W>::Machine(const MachineOptions<W>& opts)
		: Machine(std::string_view{}, opts){}

	template <int W>
	Machine<W>::~Machine()
	{
	}

	template <int W>
	typename Registers<W>::Options Machine<W>::register_copy_options() const noexcept
	{
		if (!has_options() || options().preserve_vector_registers)
			return Registers<W>::Options::Everything;
		else
			return Registers<W>::Options::NoVectors;
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
		machine.print(txt.c_str(), txt.size());
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
	void Machine<W>::penalize(uint64_t val)
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

		// preserve argc/argv/envp and the auxiliary vector for the program lifetime
		this->memory.set_stack_initial(sp);
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
		const int phdr_count = int(binary_ehdr->e_phnum);
		// Check if we have a PT_PHDR program header already loaded into memory
		address_t phdr_location = 0;
		for (int i = 0; i < phdr_count; i++)
		{
			if (binary_phdr[i].p_type == Elf<W>::PT_PHDR) {
				phdr_location = this->memory.elf_base_address(binary_phdr[i].p_vaddr);
				break;
			}
		}
		if (phdr_location == 0) {
			for (int i = phdr_count-1; i >= 0; i--)
			{
				const auto* phd = &binary_phdr[i];
				push_down(*this, dst, phd, sizeof(typename Elf<W>::ProgramHeader));
			}
			phdr_location = dst;
		} else {
			// Verify that the PT_PHDR is loaded at the correct address
			if (memory.memcmp(binary_phdr, phdr_location, phdr_count * sizeof(*binary_phdr)) != 0) {
				throw MachineException(INVALID_PROGRAM, "PT_PHDR program header is not loaded at the correct address");
			}
		}

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
		push_aux<W>(argv, {AT_PHNUM, unsigned(phdr_count)});

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
		// preserve argc/argv/envp and the auxiliary vector for the program lifetime
		this->memory.set_stack_initial(dst);
	}

	template <int W>
	static void handle_unhandled_csr(Machine<W>& machine, uint32_t csr, int rd, int rs1)
	{
		if ((csr & 0x300) != 0) {
			machine.cpu.trigger_exception(ILLEGAL_OPERATION, csr);
			return;
		}
		Machine<W>::on_unhandled_csr(machine, csr, rd, rs1);
	}

#ifdef RISCV_EXT_VECTOR
	/**
	 * The vector CSRs, which all six CSR instructions reach the same way:
	 * read the old value into rd, then write back the value the operation
	 * computes from it. Returns false for a CSR that is not one of ours, so
	 * the caller can carry on with its own table.
	 *
	 * vl, vtype and vlenb are read-only -- only vsetvl writes them -- so a
	 * CSR instruction that would write one raises illegal-operation. The
	 * set/clear forms only count as a write when their source is nonzero,
	 * which is what makes `csrr rd, vl` (a CSRRS with rs1=x0) legal.
	 */
	template <int W>
	static bool handle_vector_csr(Machine<W>& machine, union rv32i_instruction instr)
	{
		const uint32_t csr = instr.Itype.imm;
		switch (csr) {
			case 0x008: case 0x009: case 0x00A: case 0x00F:
			case 0xC20: case 0xC21: case 0xC22:
				break;
			default:
				return false;
		}
		auto& cpu = machine.cpu;
		auto& rvv = cpu.registers().rvv();
		using register_t = typename Machine<W>::address_t;

		const unsigned funct3 = instr.Itype.funct3;
		const bool immediate = funct3 >= 0x5;
		// CSRRWI and friends take a 5-bit unsigned immediate where the
		// register forms take rs1.
		const register_t src = immediate
			? register_t(instr.Itype.rs1) : cpu.reg(instr.Itype.rs1);
		// A swap always writes; set and clear only when they have bits to
		// set or clear, which is how a plain read is spelled. Both forms
		// name their source in rs1, so one test covers them.
		const bool writes = (funct3 == 0x1 || funct3 == 0x5)
			|| instr.Itype.rs1 != 0;

		register_t old;
		switch (csr) {
			case 0x008: old = rvv.vstart(); break;
			case 0x009: old = rvv.vxsat();  break;
			case 0x00A: old = rvv.vxrm();   break;
			case 0x00F: old = rvv.vcsr();   break;
			case 0xC20: old = rvv.vl();     break;
			case 0xC21: old = rvv.vtype();  break;
			default:    old = rvv.vlenb();  break; // 0xC22
		}

		if (writes && csr >= 0xC20) {
			cpu.trigger_exception(ILLEGAL_OPERATION, csr);
			return true;
		}
		if (instr.Itype.rd != 0)
			cpu.reg(instr.Itype.rd) = old;
		if (!writes)
			return true;

		const register_t value =
			(funct3 == 0x1 || funct3 == 0x5) ? src :
			(funct3 == 0x2 || funct3 == 0x6) ? register_t(old | src) :
			register_t(old & ~src);

		switch (csr) {
			case 0x008: rvv.set_vstart(value); break;
			case 0x009: rvv.set_vxsat(value & 1); break;
			case 0x00A: rvv.set_vxrm((unsigned)value); break;
			default:    rvv.set_vcsr((unsigned)value); break; // 0x00F
		}
		return true;
	}
#endif

	template <int W>
	void Machine<W>::system(union rv32i_instruction instr)
	{
#ifdef RISCV_EXT_VECTOR
		// The vector CSRs are shared by all six CSR instructions, so they
		// are handled once here rather than in each of the tables below.
		if (instr.Itype.funct3 != 0x0 && instr.Itype.funct3 != 0x4) {
			if (handle_vector_csr(*this, instr))
				return;
		}
#endif
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
			case 0x00D: // Zawrs: WRS.NTO
			case 0x01D: // Zawrs: WRS.STO
				// Wait-on-reservation-set -> spurious wakeup
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
			case 0xC01: // CSR RDTIME (lower) is read-only
				cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
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
				if (instr.Itype.rs1 != 0) {
					cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
					return;
				}
				if (rd) cpu.reg(instr.Itype.rd) = m_rdtime(*this);
				return;
			case 0xC81: // CSR RDTIME (upper)
				if (rd) cpu.reg(instr.Itype.rd) = m_rdtime(*this) >> 32u;
				return;
			case 0xF11: // CSR marchid
				// Machine-level CSRs are not accessible to the U-mode guest
				// this emulator models; matching the handle_unhandled_csr
				// privilege gate, accessing them must raise illegal-operation.
				cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
				return;
			case 0xF12: // CSR mvendorid
				cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
				return;
			case 0xF13: // CSR mimpid
				cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
				return;
			case 0xF14: // CSR mhartid
				cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
				return;
			default:
				handle_unhandled_csr(*this, instr.Itype.imm, instr.Itype.rd, instr.Itype.rs1);
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
			case 0xC01: // CSR RDTIME (lower)
				if (instr.Itype.rs1 != 0) {
					cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
					return;
				}
				if (rd) cpu.reg(instr.Itype.rd) = m_rdtime(*this);
				return;
			}
			break;
		}
		case 0x4: { // Zimop: the may-be-operations
			//   MOP.R.n    1 n4 00 n3 n2 0111 n1 n0 | rs1 | 100 | rd
			//   MOP.RR.n   1 n2 00 n1 n0 1 | rs2  | rs1 | 100 | rd
			const uint32_t hi = instr.whole >> 25;
			const bool wellformed = (hi & 0b1011000) == 0b1000000
				&& ((hi & 1) != 0 || (instr.whole & (0b111 << 22)) == (0b111 << 22));
			if (wellformed) {
				if (instr.Itype.rd != 0)
					cpu.reg(instr.Itype.rd) = 0;
				return;
			}
			break;
		} // Zimop
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
			case 0xC01: // CSR RDTIME (lower) is read-only
				cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
				return;
			default:
				handle_unhandled_csr(*this, instr.Itype.imm, instr.Itype.rd, instr.Itype.rs1);
				return;
			}
		} // CSRWI
		case 0x6: { // CSRRSI: Atomically read and set bit mask using immediate
			const bool rd = instr.Itype.rd != 0;
			const uint32_t imm = instr.Itype.rs1;
			switch (instr.Itype.imm)
			{
			case 0x001: // fflags (accrued exceptions)
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().fflags;
				cpu.registers().fcsr().fflags |= imm;
				return;
			case 0x002: // frm (rounding-mode)
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().frm;
				cpu.registers().fcsr().frm |= imm;
				return;
			case 0x003: // fcsr (control and status register)
				if (rd) cpu.reg(instr.Itype.rd) = cpu.registers().fcsr().whole;
				cpu.registers().fcsr().whole |= imm & 0xFF;
				return;
			case 0xC00: // CSR RDCYCLE (lower)
			case 0xC02: // RDINSTRET (lower)
				if (rd) {
					cpu.reg(instr.Itype.rd) = this->instruction_counter();
					return;
				} else {
					if (imm == 0) // UNIMP instruction
						cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION, instr.Itype.imm);
					else // CYCLE is not writable
						cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
				}
			case 0xC80: // CSR RDCYCLE (upper)
			case 0xC82: // RDINSTRET (upper)
				if (rd) cpu.reg(instr.Itype.rd) = this->instruction_counter() >> 32u;
				return;
			case 0xC01: // CSR RDTIME (lower)
				if (imm != 0) {
					cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
					return;
				}
				if (rd) cpu.reg(instr.Itype.rd) = m_rdtime(*this);
				return;
			case 0xC81: // CSR RDTIME (upper)
				if (rd) cpu.reg(instr.Itype.rd) = m_rdtime(*this) >> 32u;
				return;
			default:
				handle_unhandled_csr(*this, instr.Itype.imm, instr.Itype.rd, instr.Itype.rs1);
				return;
			}
		} // CSRRSI
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
			case 0xC01: // CSR RDTIME (lower)
				if (imm != 0) {
					cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.imm);
					return;
				}
				if (rd) cpu.reg(instr.Itype.rd) = m_rdtime(*this);
				return;
			default:
				handle_unhandled_csr(*this, instr.Itype.imm, instr.Itype.rd, instr.Itype.rs1);
				return;
			}
			break;
		} // CSRRCI
		}
		// if we got here, its an illegal operation!
		cpu.trigger_exception(ILLEGAL_OPERATION, instr.Itype.funct3);
	}

	INSTANTIATE_32_IF_ENABLED(Machine);
	INSTANTIATE_64_IF_ENABLED(Machine);
	INSTANTIATE_128_IF_ENABLED(Machine);
}
