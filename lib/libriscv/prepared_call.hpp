#pragma once
#include "machine.hpp"
#if defined(RISCV_BINARY_TRANSLATION)
#include "decoder_cache.hpp"
#include "threaded_bytecodes.hpp"
#endif

namespace riscv
{
	/**
	 * A stored vmcall stores string and struct arguments on the
	 * machine stack, and then moves the stack pointer. This leaves
	 * the data written on the stack stuck permanently, unless the
	 * stack pointer is manually restored.
	 * 
	 * 1. Initialization:
	 * riscv::StoredCall<RISCV32> stored;
	 * stored.store(machine, "my_function",
	 * 		"This is a string", test,
	 * 		333, 444, 555, 666, 777, 888);
	 * 
	 * 2. Make call:
	 * stored.vmcall();
	 * 
	 * A stored call can drastically reduce the call overhead, when
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
	 * stored calls in general, and they should not hog a lot of stack space.
	 * 
	**/
	template <int W>
	struct StoredCall
	{
		using address_t = address_type<W>;
		using saddr_t = signed_address_type<W>;

		saddr_t vmcall(uint64_t imax = UINT64_MAX);

		template <typename... Args>
		void store(Machine<W>&, const std::string& func, Args&&...);

		template <typename... Args>
		void store(Machine<W>&, address_t call_addr, Args&&...);

		bool is_stored() const noexcept {
			return this->m_func != nullptr;
		}
		operator bool() const noexcept {
			return this->is_stored();
		}

		StoredCall() = default;
		~StoredCall() = default;
		void reset() { m_func = nullptr; }

	private:
		std::function<saddr_t(uint64_t)> m_func;
	};

	template <int W>
	inline signed_address_type<W> StoredCall<W>::vmcall(uint64_t imax)
	{
		if (UNLIKELY(!is_stored()))
			throw MachineException(ILLEGAL_OPERATION,
				"The call was not prepared", 0x0);

		return m_func(imax);
	}

	template <int W>
	template <typename... Args>
	void StoredCall<W>::store(Machine<W>& m, address_t call_addr, Args&&... args)
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
			cpu.reg(REG_SP) &= ~address_t(0xF);

			// Execute vmcall
			m.simulate_with(imax, 0, call_addr);

			return cpu.reg(REG_RETVAL);
		};
	}

	template <int W>
	template <typename... Args>
	void StoredCall<W>::store(Machine<W>& m, const std::string& func, Args&&... args)
	{
		this->store(m, m.address_of(func), std::forward<Args>(args)...);
	}

	/**
	 * A prepared vmcall makes preparations for a given type of call
	 * by recording the PC, max instructions, and enforcing a function type
	 * 
	 * When binary translation is enabled, the prepared call will attempt
	 * to check if the function is binary translated, and if so, call the
	 * function directly. Work in progress.
	**/
	template <int W, typename F, uint64_t IMAX = UINT64_MAX>
	struct PreparedCall
	{
	public:
		using address_t = address_type<W>;
		using Ret = std::function<F>::result_type;

		template <typename... Args>
		auto call_with(Machine<W>& m, Args&&... args) const
		{
			static_assert(std::is_invocable_v<F, Args...>,
				"PreparedCall: Invalid argument types for function call");

#if defined(RISCV_BINARY_TRANSLATION)
			auto  exit_addr = m.memory.exit_address();
#endif
			m.cpu.reset_stack_pointer();
			m.setup_call(std::forward<Args>(args)...);

#if defined(RISCV_BINARY_TRANSLATION)
			if (m_mapping != nullptr)
			{
				auto results = m_mapping(m.cpu, 0, IMAX, m_pc);
				auto max = results.max_counter;
				if (max == 0 || m.cpu.pc() == exit_addr)
				{
					[[likely]];
					goto resolve_return_value;
				}
				else if (results.counter >= max) {
					[[unlikely]];
					throw MachineTimeoutException(MAX_INSTRUCTIONS_REACHED,
						"PreparedCall: execution timeout", max);
				}
				// Continue with normal simulation
				m.simulate_with(max, results.counter, m.cpu.pc());
			} else {
#endif
				m.simulate_with(IMAX, 0, m_pc);
#if defined(RISCV_BINARY_TRANSLATION)
			}
resolve_return_value:
#endif
			if constexpr (std::is_same_v<Ret, float>)
				return m.cpu.registers().getfl(REG_RETVAL).f32[0];
			else if constexpr (std::is_same_v<Ret, double>)
				return m.cpu.registers().getfl(REG_RETVAL).f64;
			else
				return m.cpu.reg(REG_RETVAL);
		}

		template <typename... Args>
		auto operator()(Args&&... args) const
		{
			return this->call_with(*m_machine, std::forward<Args>(args)...);
		}

		Machine<W>& machine() const noexcept { return *m_machine; }
		Machine<W>& machine() noexcept { return *m_machine; }

		address_t address() const noexcept { return m_pc; }

		constexpr uint64_t max_instructions() const noexcept { return IMAX; }

		bool is_directly_translated() const noexcept {
#if defined(RISCV_BINARY_TRANSLATION)
			return m_mapping != nullptr;
#else
			return false;
#endif
		}

		void prepare(Machine<W>& m, address_t call_addr)
		{
			if (call_addr == 0x0)
				throw MachineException(EXECUTION_SPACE_PROTECTION_FAULT,
					"Function address for PreparedCall was 0x0", call_addr);

			// Check if the jump is OK
			auto old_pc = m.cpu.pc();
			m.cpu.jump(call_addr);

			auto pc  = call_addr;

			m.cpu.aligned_jump(old_pc);

			this->m_machine = &m;
			this->m_pc = pc;

#if defined(RISCV_BINARY_TRANSLATION)
			auto& exec = m.cpu.current_execute_segment();
			// We need an execute segment matching current PC
			if (pc >= exec.exec_begin() && pc < exec.exec_end())
			{
				DecoderData<W>* exec_decoder = exec.decoder_cache();
				auto* decoder = &exec_decoder[pc >> DecoderCache<W>::SHIFT];
				// There's a very high chance that the (first) instruction is a translated function
				if (LIKELY(decoder->get_bytecode() == RV32I_BC_TRANSLATOR))
				{
					//printf("PreparedCall: Using direct translation for %s\n", m.memory.lookup(call_addr).name.c_str());
					this->m_mapping = exec.mapping_at(decoder->instr);
				} else {
					//printf("PreparedCall: No direct translation for %s\n", m.memory.lookup(call_addr).name.c_str());
					this->m_mapping = nullptr;
				}
			} else {
				this->m_mapping = nullptr;
			}
#else
			m.cpu.create_fast_path_function(pc);
#endif
		}

		void prepare(Machine<W>& m, const std::string& func)
		{
			this->prepare(m, m.address_of(func));
		}

		void prepare(Machine<W>& m)
		{
			this->m_machine = &m;
		}

		PreparedCall(Machine<W>& m, const std::string& func)
		{
			this->prepare(m, func);
		}
		PreparedCall(Machine<W>& m, address_t call_addr)
		{
			this->prepare(m, call_addr);
		}
		PreparedCall(const PreparedCall& other)
			: m_machine(other.m_machine), m_pc(other.m_pc)
		{
#if defined(RISCV_BINARY_TRANSLATION)
			this->m_mapping = other.m_mapping;
#endif
		}
		~PreparedCall() = default;

	private:
		Machine<W>* m_machine = nullptr;
		address_t   m_pc = 0;
#if defined(RISCV_BINARY_TRANSLATION)
		bintr_block_func<W> m_mapping = nullptr;
#endif
	};

} // riscv
