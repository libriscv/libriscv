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
		m_smp.reset(new Multiprocessing<W>);
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
void Multiprocessing<W>::async_work(std::function<void()> wrk)
{
	m_threadpool.enqueue(wrk);
	this->processing = true;
}
template <int W>
void Multiprocessing<W>::wait()
{
	m_threadpool.wait_until_nothing_in_flight();
	this->processing = false;
}

template <int W>
bool Machine<W>::multiprocess(unsigned num_cpus,
	address_t func, uint64_t maxi, address_t stack, size_t stack_size,
	address_t data)
{
	if (UNLIKELY(is_multiprocessing()))
		return false;

	Latch latch{num_cpus - 1};

	// Create vCPU 1...N
	for (unsigned i = 1; i < num_cpus; i++)
	{
		const address_t sp = stack + i * stack_size;
		smp().async_work(
		[=, &latch] () -> int {

			Machine<W> fork { *this };
			latch.arrive();

			fork.set_userdata(this->get_userdata<void>());
			fork.set_printer([] (const char*, size_t) {});
			fork.set_max_instructions(maxi);
			fork.cpu.reg(REG_SP) = sp;
			fork.setup_call(func, (unsigned)i, (address_t)data);

			if (smp().shared_page_faults) {
				fork.memory.set_page_fault_handler(
				[this] (auto& mem, const address_t pageno) -> Page& {
					std::lock_guard<std::mutex> lk(this->smp().m_lock);
					auto& master_page = this->memory.create_page(pageno);
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
				auto& master_page = this->memory.create_page(pageno);
				// Return back page with memory loaned from master VM
				page.attr = master_page.attr;
				page.attr.non_owning = true;
				page.m_page.reset(master_page.m_page.get());
			});

			try {
				fork.simulate<true> (maxi);
			} catch (...) {
				return i;
			}
			return 0;
		});
	} // foreach CPU

	latch.wait();

	return true;
}
template <int W>
void Machine<W>::multiprocess_wait()
{
	smp().wait();
}

#else // RISCV_MULTIPROCESS

template <int W>
bool Machine<W>::multiprocess(unsigned, address_t, uint64_t, address_t, size_t, address_t) {
	return false;
}
template <int W>
void Machine<W>::multiprocess_wait() { }

#endif // RISCV_MULTIPROCESS

template struct Machine<4>;
template struct Machine<8>;
template struct Machine<16>;
template struct Multiprocessing<4>;
template struct Multiprocessing<8>;
template struct Multiprocessing<16>;
} // riscv
