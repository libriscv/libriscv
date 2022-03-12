#include "multiprocessing.hpp"

#include "machine.hpp"

namespace riscv {

struct Latch {
	std::size_t remaining;
	std::mutex mtx;
	std::condition_variable cv;

	Latch(std::size_t s) : remaining(s) {}

	void arrive()
	{
		auto lock = std::unique_lock(mtx);
		remaining--;
		if (remaining == 0) cv.notify_all();
	}
	void wait()
	{
		auto lock = std::unique_lock(mtx);
		cv.wait(lock,
			[&] { return remaining == 0; }
		);
	}
};

template <int W>
Multiprocessing<W>& Machine<W>::smp()
{
	if (UNLIKELY(m_smp == nullptr))
		m_smp.reset(new Multiprocessing<W> (m_multiprocessing_workers));
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
void Multiprocessing<W>::async_work(std::function<void()>&& wrk)
{
	m_threadpool.enqueue(std::move(wrk));
	this->processing = true;
}
template <int W>
long Multiprocessing<W>::wait()
{
	m_threadpool.wait_until_nothing_in_flight();
	this->processing = false;
	return this->failures ? -1 : 0;
}

template <int W>
bool Machine<W>::multiprocess(unsigned num_cpus, uint64_t maxi,
	address_t stack, address_t stksize, bool do_fork)
{
	if (UNLIKELY(is_multiprocessing()))
		return false;

	const address_t stackpage = Memory<W>::page_number(stack);
	const address_t stackendpage = Memory<W>::page_number(stack + stksize);
	smp().failures = false;
	Latch latch{num_cpus};

	// Create worker 0...N
	for (unsigned id = 1; id <= num_cpus; id++)
	{
		const address_t sp = stack + stksize * id;

		if (do_fork == false) {
			smp().async_work(
			[=, &latch] {

				Machine<W> fork { *this, { .cpu_id = id } };
				// TODO: threads need to be accessed through the main VM
				latch.arrive();

				fork.set_userdata(this->get_userdata<void>());
				fork.set_printer([] (const char*, size_t) {});
				fork.set_max_instructions(maxi);
				fork.cpu.increment_pc(4); // Step over current ECALL
				fork.cpu.reg(REG_SP) = sp; // Per-CPU stack
				fork.cpu.reg(REG_ARG0) = id; // Return value

				if (smp().shared_page_faults) {
					fork.memory.set_page_fault_handler(
					[this] (auto& mem, const address_t pageno) -> Page& {
						std::lock_guard<std::mutex> lk(this->smp().m_lock);
						auto& master_page = this->memory.create_writable_pageno(pageno);
						// Install the page as non-owned (loaned) memory
						return mem.install_shared_page(pageno, master_page);
					});
				}
				if (smp().shared_read_faults) {
					fork.memory.set_page_readf_handler(
					[this] (const auto&, address_t pageno) -> const Page& {
						std::lock_guard<std::mutex> lk(this->smp().m_lock);
						return this->memory.get_pageno(pageno);
					});
				}

				// For most workloads, we will only need a copy-on-write handler
				fork.memory.set_page_write_handler(
				[this] (auto&, address_t pageno, Page& page) -> void {
					// Release old page if non-owned
					if (page.attr.non_owning && page.m_page.get() != nullptr)
						page.m_page.release();

					std::lock_guard<std::mutex> lk(this->m_smp->m_lock);
					// Create new page in master VM
					auto& master_page = this->memory.create_writable_pageno(pageno);
					// Return back page with memory loaned from master VM
					page.attr = master_page.attr;
					page.attr.non_owning = true;
					page.m_page.reset(master_page.m_page.get());
				});

				try {
					fork.simulate<true> (maxi);
				} catch (...) {
					smp().failures = true;
				}
			});
		} else {
			// Fork variant
			smp().async_work(
			[=] {
				Machine<W> fork { *this, { .cpu_id = id } };

				fork.set_userdata(this->get_userdata<void>());
				fork.set_printer([] (const char*, size_t) {});
				fork.set_max_instructions(maxi);
				fork.cpu.increment_pc(4); // Step over current ECALL
				fork.cpu.reg(REG_ARG0) = id; // Return value

				// For most workloads, we will only need a copy-on-write handler
				fork.memory.set_page_write_handler(
				[this, stackpage, stackendpage] (auto&, address_t pageno, Page& page) -> void
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
					page.attr = master_page.attr;
					page.attr.non_owning = true;
					page.m_page.reset(master_page.m_page.get());
				});

				try {
					fork.simulate<true> (maxi);
				} catch (...) {
					smp().failures = true;
				}
			});
		}
	} // foreach CPU

	if (do_fork == false) {
		// When not forking, we will only wait for the Machine forks
		// to complete.
		latch.wait();
	} else {
		// Immediately wait if we are forking everything
		// We don't want the main vCPU to trample the stack that the workers
		// may be relying on. It's perfectly safe to immediately wait.
		multiprocess_wait();
	}

	return true;
}
template <int W>
long Machine<W>::multiprocess_wait()
{
	return smp().wait();
}

#else // RISCV_MULTIPROCESS

template <int W>
Multiprocessing<W>::Multiprocessing(size_t) {}

template <int W>
bool Machine<W>::multiprocess(unsigned, uint64_t, address_t, address_t, bool) {
	return false;
}
template <int W>
long Machine<W>::multiprocess_wait() { return -1; }

#endif // RISCV_MULTIPROCESS

template struct Machine<4>;
template struct Machine<8>;
template struct Machine<16>;
template struct Multiprocessing<4>;
template struct Multiprocessing<8>;
template struct Multiprocessing<16>;
} // riscv
