#pragma once
#include <memory>

namespace riscv
{
	template<int W> struct DecoderCache;
	template<int W> struct DecoderData;

	// A fully decoded execute segment
	template <int W>
	struct DecodedExecuteSegment
	{
		using address_t = address_type<W>;

		bool is_within(address_t addr, size_t len = 2) const noexcept {
			return addr >= m_vaddr_begin && addr + len < m_vaddr_end;
		}

		auto* exec_data(address_t pc = 0) const noexcept {
			return m_exec_pagedata.get() - m_exec_pagedata_base + pc;
		}

		address_t exec_begin() const noexcept { return m_vaddr_begin; }
		address_t exec_end() const noexcept { return m_vaddr_end; }
		address_t pagedata_base() const noexcept { return m_exec_pagedata_base; }

		auto* decoder_cache() noexcept { return m_exec_decoder; }
		auto* decoder_cache() const noexcept { return m_exec_decoder; }

		auto* create_decoder_cache(DecoderCache<W>* cache, size_t size) {
			m_decoder_cache.reset(cache);
			m_decoder_cache_size = size;
			return m_decoder_cache.get();
		}
		void set_decoder(DecoderData<W>* dec) { m_exec_decoder = dec; }

		size_t size_bytes() const noexcept {
			return sizeof(*this) + m_exec_pagedata_size + m_decoder_cache_size;
		}
		bool empty() const noexcept { return m_exec_pagedata_size == 0; }

		DecodedExecuteSegment(address_t pbase, size_t len, address_t vaddr, size_t exlen);

	private:
		address_t m_vaddr_begin;
		address_t m_vaddr_end;
		DecoderData<W>* m_exec_decoder = nullptr;

		// The flat execute segment is used to execute
		// the CPU::simulate_precise function in order to
		// support debugging, as well as when producing
		// the decoder cache
		size_t    m_exec_pagedata_size = 0;
		address_t m_exec_pagedata_base = 0;
		std::unique_ptr<uint8_t[]> m_exec_pagedata = nullptr;

		// Decoder cache is used to run simulation at a
		// high speed, without resorting to JIT
		size_t          m_decoder_cache_size = 0;
		std::unique_ptr<DecoderCache<W>[]> m_decoder_cache = nullptr;
	};

	template <int W>
	inline DecodedExecuteSegment<W>::DecodedExecuteSegment(
		address_t pbase, size_t len, address_t exaddr, size_t exlen)
	{
		m_vaddr_begin = exaddr;
		m_vaddr_end   = exaddr + exlen;
		m_exec_pagedata.reset(new uint8_t[len]);
		m_exec_pagedata_size = len;
		m_exec_pagedata_base = pbase;
	}

} // riscv
