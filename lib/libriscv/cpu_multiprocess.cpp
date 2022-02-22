#include "machine.hpp"

namespace riscv
{
	// We will not use read caching during multiprocessing,
	// as its simply cheaper to look up the pages using a mutex.
	// We can still use write cache, but we have to promise that
	// we will not remove the writable page.
	template <int W>
	const Page& CPU<W>::get_readable_page(address_t address)
	{
		// TODO: Cache pages that are read-only, but not CoW

		std::lock_guard<std::mutex> lk(machine().multiprocessing_lock);
		return machine().memory.get_readable_page(address);
	}

	template <int W>
	Page& CPU<W>::get_writable_page(address_t address)
	{
		std::lock_guard<std::mutex> lk(machine().multiprocessing_lock);
		return machine().memory.get_writable_page(address);
	}

	template <int W>
	void Machine<W>::begin_multiprocessing()
	{
		if (m_threadpool == nullptr)
			m_threadpool.reset(new ThreadPool());

		for (auto& vcpu : m_vcpus) {
			m_threadpool->enqueue([&vcpu] {
				vcpu.simulate(vcpu.max_instructions());
			});
		}
	}
	template <int W>
	void Machine<W>::multiprocess_wait()
	{
		if (m_threadpool == nullptr)
			return;
		m_threadpool->wait_until_nothing_in_flight();
		// While we could record the final registers, we
		// don't actually care that much. Instead, we will
		// use the vector to determine if we are multiprocessing.
		m_vcpus.clear();
	}

	template <int W>
	void Machine<W>::multiprocess(unsigned num_cpus,
		address_t func, uint64_t maxi, address_t stack, size_t stack_size,
		address_t data)
	{
		if (UNLIKELY(is_multiprocessing()))
			throw MachineException(ILLEGAL_OPERATION, "Multiprocessing already active");
		// Create vCPU 1...N
		for (size_t i = 1; i < num_cpus; i++)
		{
			m_vcpus.emplace_back(this->cpu, i);
			auto& vcpu = m_vcpus.back();
			vcpu.reg(REG_SP) = stack + i * stack_size;
			vcpu.set_max_instructions(maxi);
			setup_call(vcpu, func, (int)i, (address_t)data);
		}
		// Send work to thread pool
		this->begin_multiprocessing();
	}

	template struct CPU<4>;
	template struct CPU<8>;
	template struct CPU<16>;
	template struct Machine<4>;
	template struct Machine<8>;
	template struct Machine<16>;
}
