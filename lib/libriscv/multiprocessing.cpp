#include "multiprocessing.hpp"

#include "machine.hpp"
#include "internal_common.hpp"

namespace riscv {

template <int W>
Multiprocessing<W>& Machine<W>::smp(unsigned workers)
{
	if (UNLIKELY(m_smp == nullptr))
		m_smp.reset(new Multiprocessing<W> (workers));
	return *m_smp;
}

template <int W>
bool Machine<W>::is_multiprocessing() const noexcept
{
	if (m_smp == nullptr) return false;
	return m_smp->is_multiprocessing();
}

#ifdef RISCV_MULTIPROCESS

template <int W>
Multiprocessing<W>::Multiprocessing(size_t workers)
	: m_threadpool { workers }  {}

template <int W>
void Multiprocessing<W>::async_work(std::vector<std::function<void()>>&& wrk)
{
	m_threadpool.enqueue(std::move(wrk));
	this->processing = true;
}
template <int W>
typename Multiprocessing<W>::failure_bits_t Multiprocessing<W>::wait()
{
	if (this->processing) {
		m_threadpool.wait_until_empty();
		m_threadpool.wait_until_nothing_in_flight();
		this->processing = false;
	}
	return this->failures;
}

template <int W>
bool Machine<W>::multiprocess(unsigned num_cpus, uint64_t maxi,
	address_t stack, address_t stksize, std::function<void(Machine&)> setup_cb)
{
	if (UNLIKELY(is_multiprocessing()))
		return false;

	const uint64_t stackpage = Memory<W>::page_number(stack);
	const uint64_t stackendpage = Memory<W>::page_number(stack + stksize);
	smp().failures = 0x0;

	// Create worker 1...N
	std::vector<std::function<void()>> tasks;
	tasks.reserve(num_cpus);

	for (unsigned id = 1; id <= num_cpus; id++)
	{
		// Fork variant
		tasks.push_back(
		[=, this] {
			try {
				// NOTE: minimal_fork causes a ton of contention. Avoid!
				// NOTE: cannot use arena as it can fail to read the origin stack
				Machine<W> fork { *this, { .use_memory_arena = false } };

				fork.set_userdata(this->get_userdata<void>());
				fork.set_printer([] (const auto&, const char*, size_t) {});
				//NOTE: fork.set_stdin(...) unnecessary due to default disallow.
				fork.cpu.increment_pc(4); // Step over current ECALL
				fork.cpu.reg(REG_ARG0) = id; // Return value

				// For most workloads, we will only need a copy-on-write handler
				fork.memory.set_page_write_handler(
				[=, this] (auto&, address_t pageno, Page& page)
				{
					if (pageno >= stackpage && pageno < stackendpage) {
						page.make_writable();
						return;
					}
					// Release old page if non-owned
					if (page.attr.non_owning && page.m_page.get() != nullptr)
						page.m_page.release();

					std::lock_guard<std::mutex> lk(this->m_smp->m_lock);
					// Retrieve writable page in main VM
					auto& master_page = this->memory.create_writable_pageno(pageno);
					// Return back page with memory loaned from master VM
					page.loan(master_page);
				});
				fork.memory.set_page_readf_handler(
				[this] (auto&, address_t pageno) -> const Page& {
					std::lock_guard<std::mutex> lk(this->smp().m_lock);
					return this->memory.get_pageno(pageno);
				});
				fork.memory.set_page_fault_handler(
				[=, this] (auto& mem, const address_t pageno, bool init) -> Page& {
					if (pageno >= stackpage && pageno < stackendpage) {
						return mem.create_writable_pageno(pageno, init);
					}
					std::lock_guard<std::mutex> lk(this->smp().m_lock);
					return this->memory.create_writable_pageno(pageno, init);
				});

				if (setup_cb != nullptr)
					setup_cb(fork);

				fork.simulate<true> (maxi);
			} catch (...) {
				__sync_fetch_and_or(&smp().failures, 1u << id);
			}
		});
	} // foreach CPU

	smp().async_work(std::move(tasks));

	// Immediately wait if we are forking everything
	// We don't want the main vCPU to trample the stack that the workers
	// may be relying on. It's perfectly safe to immediately wait.
	multiprocess_wait();

	return true;
}
template <int W>
uint32_t Machine<W>::multiprocess_wait()
{
	return smp().wait();
}

#else // RISCV_MULTIPROCESS

template <int W>
Multiprocessing<W>::Multiprocessing(size_t) {}

template <int W>
bool Machine<W>::multiprocess(unsigned, uint64_t, address_t, address_t, std::function<void(Machine&)>) {
	return false;
}
template <int W>
uint32_t Machine<W>::multiprocess_wait() { return -1; }

#endif // RISCV_MULTIPROCESS

INSTANTIATE_32_IF_ENABLED(Machine);
INSTANTIATE_32_IF_ENABLED(Multiprocessing);
INSTANTIATE_64_IF_ENABLED(Machine);
INSTANTIATE_64_IF_ENABLED(Multiprocessing);
INSTANTIATE_128_IF_ENABLED(Machine);
INSTANTIATE_128_IF_ENABLED(Multiprocessing);
} // riscv
