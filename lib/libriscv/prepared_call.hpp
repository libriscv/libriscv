#include "machine.hpp"

namespace riscv
{
	/**
	 * A prepared vmcall stores string and struct arguments on the
	 * machine stack, and then moves the stack pointer. This leaves
	 * the data written on the stack stuck permanently, unless the
	 * stack pointer is manually restored.
	 * 
	 * 1. Initialization:
	 * riscv::PreparedCall<RISCV32> prepper;
	 * prepper.prepare(machine, "my_function",
	 * 		"This is a string", test,
	 * 		333, 444, 555, 666, 777, 888);
	 * 
	 * 2. Make call:
	 * prepper.vmcall();
	 * 
	 * A prepared call can drastically reduce the call overhead, when
	 * needed, at the cost of stack space taken by the arguments up-front.
	 * Prepared calls make use of prefetching in order to load content
	 * pushed to the stack just before it is needed by the guest function.
	 * 
	 * It would in theory be possible to have a destructor attempt to
	 * restore the stack pointer if everything is destructed in the
	 * right order, however most likely it is simpler to just remove
	 * all instances and move the pointer back.
	 * We could also experiment with using a custom arena, but this simpler
	 * solution is easier to grasp and manage. There should be relatively few
	 * prepared calls in general, and they should not hog a lot of stack space.
	 * 
	 * libriscv: many arguments 	median 13ns		lowest: 12ns    highest: 17ns
	 * libriscv: prepared arguments	median 3ns 		lowest: 3ns     highest: 3ns
	 * luajit: many arguments		median 184ns  	lowest: 181ns   highest: 194ns
	 * 
	**/
	template <int W>
	struct PreparedCall
	{
		using address_t = address_type<W>;
		using saddr_t = signed_address_type<W>;

		saddr_t vmcall(uint64_t imax = UINT64_MAX);

		template <typename... Args>
		void prepare(Machine<W>&, const std::string& func, Args&&...);

		template <typename... Args>
		void prepare(Machine<W>&, address_t call_addr, Args&&...);

		bool is_prepared() const noexcept {
			return this->m_func != nullptr;
		}

		PreparedCall() = default;
		~PreparedCall() = default;
		void reset() { m_func = nullptr; }

	private:
		std::function<saddr_t(uint64_t)> m_func;
	};

	template <int W>
	inline signed_address_type<W> PreparedCall<W>::vmcall(uint64_t imax)
	{
		if (UNLIKELY(!is_prepared()))
			throw MachineException(ILLEGAL_OPERATION,
				"The call was not prepared", 0x0);

		return m_func(imax);
	}

	template <int W>
	template <typename... Args>
	void PreparedCall<W>::prepare(Machine<W>& m, address_t call_addr, Args&&... args)
	{
		if (UNLIKELY(call_addr == 0x0))
			throw MachineException(ILLEGAL_OPERATION,
				"The prepared call address was zero (0x0)", call_addr);
		// Temporary jump to call address in order to validate it
		// This allows using cpu.aligned_jump() for the prepared call later on
		const auto pc = m.cpu.pc();
		m.cpu.jump(call_addr);
		m.cpu.aligned_jump(pc);

		m.cpu.reset_stack_pointer();

		std::array<address_t, 8> gpr;
		unsigned iarg = 0;
		([&] {
			if constexpr (std::is_integral_v<remove_cvref<Args>>) {
				gpr[iarg++] = args;
				if constexpr (sizeof(Args) > W) // upper 32-bits for 64-bit integers
					gpr[iarg++] = args >> 32;
			}
			else if constexpr (is_stdstring<Args>::value)
				gpr[iarg++] = m.stack_push(args.data(), args.size()+1);
			else if constexpr (is_string<Args>::value)
				gpr[iarg++] = m.stack_push(args, strlen(args)+1);
			else if constexpr (std::is_floating_point_v<remove_cvref<Args>>) {
			}
			else if constexpr (std::is_standard_layout_v<remove_cvref<Args>>)
				gpr[iarg++] = m.stack_push(&args, sizeof(args));
			else
				static_assert(always_false<decltype(args)>, "Unknown type");
		}(), ...);
		m.realign_stack();

		// Move VMCALL initial stack address to new position
		m.memory.set_stack_initial(m.cpu.reg(REG_SP));

		this->m_func =
		[=] (uint64_t imax) mutable -> saddr_t
		{
			auto& cpu = m.cpu;
			auto& regs = cpu.registers();

			// 1. Jump to address
			cpu.aligned_jump(call_addr);
			// 2. Set return address (exit function)
			cpu.reg(REG_RA) = m.memory.exit_address();
			// 3. Reset stack pointer to current baseline
			cpu.reset_stack_pointer();
			// 4. Re-construct registers from parameter pack
			// but re-use stack for stack-stored arguments
			unsigned iarg = 0;
			unsigned farg = 0;
			([&] {
				if constexpr (std::is_integral_v<remove_cvref<Args>>) {
					regs.get(REG_ARG0 + iarg++) = args;
					if constexpr (sizeof(Args) > W) // upper 32-bits for 64-bit integers
						regs.get(REG_ARG0 + iarg++) = args >> 32;
				}
				else if constexpr (is_stdstring<Args>::value)
					regs.get(REG_ARG0 + iarg++) = gpr[iarg];
				else if constexpr (is_string<Args>::value)
					regs.get(REG_ARG0 + iarg++) = gpr[iarg];
				else if constexpr (std::is_floating_point_v<remove_cvref<Args>>)
					regs.getfl(REG_FA0 + farg++).set_float(args);
				else if constexpr (std::is_standard_layout_v<remove_cvref<Args>>)
					regs.get(REG_ARG0 + iarg++) = gpr[iarg];
				else
					static_assert(always_false<decltype(args)>, "Unknown type");
			}(), ...);

			// Execute vmcall
			m.simulate(imax);

			return saddr_t(cpu.reg(REG_RETVAL));
		};
	}

	template <int W>
	template <typename... Args>
	void PreparedCall<W>::prepare(Machine<W>& m, const std::string& func, Args&&... args)
	{
		this->prepare(m, m.address_of(func), std::forward<Args>(args)...);
	}

} // riscv
