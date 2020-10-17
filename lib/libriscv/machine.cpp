#include "machine.hpp"
#include "util/auxvec.hpp"
#include <time.h>
#include <random>

namespace riscv
{
	template <int W>
	long Machine<W>::unknown_syscall_handler(Machine<W>& machine)
	{
		const auto syscall_number = machine.cpu.reg(RISCV::REG_ECALL);
		if (UNLIKELY(machine.m_on_unhandled_syscall != nullptr)) {
			machine.m_on_unhandled_syscall(syscall_number);
		}
#ifndef RISCV_EBREAK_MEANS_STOP
		// EBREAK should not modify registers
		if (syscall_number != SYSCALL_EBREAK) {
			return -38; // -ENOSYS
		}
		return machine.cpu.reg(RISCV::REG_RETVAL);
#else
		return -38;
#endif
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
			const auto sp = stack_push(string.data(), string.size());
			argv.push_back(sp);
		}
		argv.push_back(0x0);
		for (const auto& string : env) {
			const auto sp = stack_push(string.data(), string.size());
			argv.push_back(sp);
		}
		argv.push_back(0x0);

		// Extra aligned SP and copy the arguments over
		auto& sp = cpu.reg(RISCV::REG_SP);
		const size_t argsize = argv.size() * sizeof(argv[0]);
		sp -= argsize;
		sp &= ~0xF; // mandated 16-byte stack alignment

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
		dst &= ~(W-1); // maintain alignment
		vec.push_back(dst);
		m.copy_to_guest(dst, (const uint8_t*) str.data(), str.size());
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
		dst &= ~(W-1); // maintain alignment
		m.copy_to_guest(dst, data, size);
	}

	template <int W>
	void Machine<W>::setup_linux(
		const std::vector<std::string>& args,
		const std::vector<std::string>& env)
	{
		// start installing at near-end of address space, leaving room on both sides
		// stack below and installation above
		auto dst = this->cpu.reg(RISCV::REG_SP);

		// inception :)
		auto gen = std::default_random_engine(time(0));
		std::uniform_int_distribution<int> rand(0,256);

		std::array<uint8_t, 16> canary;
		std::generate(canary.begin(), canary.end(), [&] { return rand(gen); });
		push_down(*this, dst, canary.data(), canary.size());
		const auto canary_addr = dst;

		const std::string platform = (W == 4) ? "RISC-V 32-bit" : "RISC-V 64-bit";
		push_down(*this, dst, platform.data(), platform.size());
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
		this->cpu.reg(RISCV::REG_SP) = dst;
	}

	uint64_t u64_monotonic_time()
	{
		struct timespec tp;
		clock_gettime(CLOCK_MONOTONIC, &tp);
		return tp.tv_sec * 1000000000ull + tp.tv_nsec;
	}

	template struct Machine<4>;
	template struct Machine<8>;
}
