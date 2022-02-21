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
		const auto pageno = Memory<W>::page_number(address);
		auto& entry = m_wr_cache;
		if (entry.pageno == pageno)
			return *entry.page;

		std::lock_guard<std::mutex> lk(machine().multiprocessing_lock);
		auto& page = machine().memory.get_writable_page(address);
		entry = {pageno, &page};
		return page;
	}

	template <int W>
	void Machine<W>::begin_multiprocessing(size_t num_cpus)
	{
		for (size_t i = 1; i < num_cpus; i++) {
			m_threadpool->enqueue([&vcpu = m_vcpus[i]] {
				// We have already set max_instructions,
				// and now we will just be resuming
				vcpu.simulate(0);
			});
		}
	}
	template <int W>
	void Machine<W>::multiprocess_wait()
	{
		m_threadpool->wait_until_nothing_in_flight();
		// While we could record the final registers, we
		// don't actually care that much. Instead, we will
		// use the vector to determine if we are multiprocessing.
		m_vcpus.clear();
	}

	template struct CPU<4>;
	template struct CPU<8>;
	template struct CPU<16>;
	template struct Machine<4>;
	template struct Machine<8>;
	template struct Machine<16>;
}
