#include "machine.hpp"
#include <time.h>

namespace riscv
{
	template <int W>
	void Machine<W>::setup_argv(const std::vector<std::string>& args)
	{
		// Arguments to main()
		std::vector<address_t> argv;
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

	uint64_t u64_monotonic_time()
	{
		struct timespec tp;
		clock_gettime(CLOCK_MONOTONIC, &tp);
		return tp.tv_sec * 1000000000ull + tp.tv_nsec;
	}

	template struct Machine<4>;
	template struct Machine<8>;
}
