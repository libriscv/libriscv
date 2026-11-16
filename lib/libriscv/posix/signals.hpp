#pragma once
#include <map>
#include <set>
#include "../types.hpp"

namespace riscv {
template <int W> struct Machine;
template <int W> struct Registers;

template <int W>
struct SignalStack {
	address_type<W> ss_sp = 0x0;
	int             ss_flags = 0x0;
	address_type<W> ss_size = 0;
};

template <int W>
struct SignalAction {
	static constexpr address_type<W> SIG_UNSET = ~(address_type<W>)0x0;
	bool is_unset() const noexcept {
		return handler == 0x0 || handler == SIG_UNSET;
	}
	address_type<W> handler = SIG_UNSET;
	bool altstack = false;
	unsigned mask = 0x0;
};

template <int W>
struct SignalReturn {
	Registers<W> regs;
};

template <int W>
struct SignalPerThread {
	SignalStack<W> stack;
	SignalReturn<W> sigret;
};

template <int W>
struct Signals {
	SignalAction<W>& get(int sig);
	/// @brief Deliver a signal: build a signal frame and jump to the handler
	void enter(Machine<W>&, int sig);
	/// @brief Return from a signal handler, restoring the interrupted state
	void leave(Machine<W>&);

	/// @brief The signal state of a thread, created on first use
	auto& per_thread(int tid) { return m_per_thread[tid]; }
	/// @brief The signal state of a thread, or nullptr if it has none.
	const SignalPerThread<W>* find_per_thread(int tid) const {
		auto it = m_per_thread.find(tid);
		return (it != m_per_thread.end()) ? &it->second : nullptr;
	}
	/// @brief Forget the signal state of a thread that exited.
	void erase_per_thread(int tid) { m_per_thread.erase(tid); }
	size_t per_thread_count() const noexcept { return m_per_thread.size(); }

	Signals();
	~Signals();
private:
	std::array<SignalAction<W>, 64> signals {};
	std::map<int, SignalPerThread<W>> m_per_thread;
};

} // riscv
