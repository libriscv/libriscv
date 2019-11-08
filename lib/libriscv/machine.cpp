#include "machine.hpp"

namespace riscv
{
	template <int W>
	address_type<W> Machine<W>::stack_push(const void* data, size_t length)
	{
		auto& sp = cpu.reg(RISCV::REG_SP);
		sp = (sp - length) & ~(W-1); // maintain word alignment
		this->copy_to_guest(sp, data, length);
		return sp;
	}

	template <int W>
	void Machine<W>::setup_argv(const std::vector<std::string>& args)
	{
		// Arguments to main()
		std::vector<uint32_t> argv;
		argv.push_back(args.size()); // argc
		for (const auto& string : args) {
			const auto sp = stack_push(string.data(), string.size());
			argv.push_back(sp);
		}
		argv.push_back(0x0);
		argv.push_back(0x0);

		// Extra aligned SP and copy the arguments over
		auto& sp = cpu.reg(RISCV::REG_SP);
		const size_t argsize = argv.size() * sizeof(argv[0]);
		sp -= argsize;
		sp &= ~0xF; // mandated 16-byte stack alignment

		this->copy_to_guest(sp, argv.data(), argsize);
	}

	template class Machine<4>;
}
