#include "machine.hpp"
#include <functional>
#include <unordered_map>

namespace riscv
{
	template <int W>
	struct DebugMachine
	{
		using address_t = address_type<W>;
		using breakpoint_t = std::function<void(DebugMachine<W> &)>;
		struct Watchpoint {
			address_t addr;
			size_t    len;
			address_t last_value;
			breakpoint_t callback;
		};

		void simulate(uint64_t max = UINT64_MAX);
		void simulate(breakpoint_t callback, uint64_t max = UINT64_MAX);
		void print(const std::string& label = "Breakpoint", address_t pc = 0);
		void print_and_pause();

		// Immediately block execution, print registers and current instruction.
		bool verbose_instructions = false;
		bool verbose_jumps = false;
		bool verbose_registers = false;
		bool verbose_fp_registers = false;

		void breakpoint(address_t address, breakpoint_t = default_pausepoint);
		void erase_breakpoint(address_t address) { breakpoint(address, nullptr); }
		auto& breakpoints() { return this->m_breakpoints; }
		void break_on_steps(int steps);
		void break_checks();
		static void default_pausepoint(DebugMachine<W>&);

		void watchpoint(address_t address, size_t len, breakpoint_t = default_pausepoint);
		void erase_watchpoint(address_t address) { watchpoint(address, 0, nullptr); }

		Machine<W>& machine;
		DebugMachine(Machine<W>& m) : machine(m) {}
	private:
		// instruction step & breakpoints
		mutable int32_t m_break_steps = 0;
		mutable int32_t m_break_steps_cnt = 0;
		std::unordered_map<address_t, breakpoint_t> m_breakpoints;
		std::vector<Watchpoint> m_watchpoints;
		bool break_time() const;
		void register_debug_logging() const;
	};

	template <int W>
	inline void DebugMachine<W>::breakpoint(address_t addr, breakpoint_t func)
	{
		if (func)
			this->m_breakpoints[addr] = func;
		else
			this->m_breakpoints.erase(addr);
	}

	template <int W>
	inline void DebugMachine<W>::watchpoint(address_t addr, size_t len, breakpoint_t func)
	{
		if (func) {
			this->m_watchpoints.push_back(Watchpoint{
				.addr = addr,
				.len  = len,
				.last_value = 0,
				.callback = func,
			});
		} else {
			for (auto it = m_watchpoints.begin(); it != m_watchpoints.end();) {
				if (it->addr == addr) {
					m_watchpoints.erase(it);
					return;
				} else ++it;
			}
		}
	}

	template <int W>
	inline void DebugMachine<W>::default_pausepoint(DebugMachine& debug)
	{
		debug.print_and_pause();
	}

} // riscv
