#pragma once
#include <memory>
#include "types.hpp"

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
			address_t addr_end;
#ifdef _MSC_VER
			addr_end = addr + len;
			return addr >= m_vaddr_begin && addr_end <= m_vaddr_end && (addr_end > addr);
#else
			if (!__builtin_add_overflow(addr, len, &addr_end))
				return addr >= m_vaddr_begin && addr_end <= m_vaddr_end;
#endif
			return false;
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

		DecodedExecuteSegment() = default;
		DecodedExecuteSegment(address_t pbase, size_t len, address_t vaddr, size_t exlen);
		DecodedExecuteSegment(DecodedExecuteSegment&&);
		~DecodedExecuteSegment();

		size_t threaded_rewrite(size_t bytecode, address_t pc, rv32i_instruction& instr);

		uint32_t crc32c_hash() const noexcept { return m_crc32c_hash; }
		void set_crc32c_hash(uint32_t hash) { m_crc32c_hash = hash; }

#ifdef RISCV_BINARY_TRANSLATION
		bool is_binary_translated() const noexcept { return !m_translator_mappings.empty(); }
		bool is_libtcc() const noexcept { return m_is_libtcc; }
		void* binary_translation_so() const { return m_bintr_dl; }
		void set_binary_translated(void* dl, bool is_libtcc) const { m_bintr_dl = dl; m_is_libtcc = is_libtcc; }
		uint32_t translation_hash() const { return m_bintr_hash; }
		void set_translation_hash(uint32_t hash) { m_bintr_hash = hash; }
		void reserve_mappings(size_t mappings) { m_translator_mappings.reserve(mappings); }
		void add_mapping(bintr_block_func<W> handler) { m_translator_mappings.push_back(handler); }
		bintr_block_func<W> mapping_at(unsigned i) const { return m_translator_mappings.at(i); }
		bintr_block_func<W> unchecked_mapping_at(unsigned i) const { return m_translator_mappings[i]; }
		size_t translator_mappings() const noexcept { return m_translator_mappings.size(); }
#else
		bool is_binary_translated() const noexcept { return false; }
#endif

		bool is_execute_only() const noexcept { return m_is_execute_only; }
		void set_execute_only(bool is_xo) { m_is_execute_only = is_xo; }

	private:
		address_t m_vaddr_begin = 0;
		address_t m_vaddr_end   = 0;
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

#ifdef RISCV_BINARY_TRANSLATION
		std::vector<bintr_block_func<W>> m_translator_mappings;
		mutable void* m_bintr_dl = nullptr;
		uint32_t m_bintr_hash = 0x0; // CRC32-C of the execute segment + compiler options
#endif
		uint32_t m_crc32c_hash = 0x0; // CRC32-C of the execute segment
		bool m_is_execute_only = false;
#ifdef RISCV_BINARY_TRANSLATION
		mutable bool m_is_libtcc = false;
#endif
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

	template <int W>
	inline DecodedExecuteSegment<W>::DecodedExecuteSegment(DecodedExecuteSegment&& other)
	{
		m_vaddr_begin = other.m_vaddr_begin;
		m_vaddr_end   = other.m_vaddr_end;
		m_exec_decoder = other.m_exec_decoder;
		other.m_exec_decoder = nullptr;

		m_exec_pagedata_size = other.m_exec_pagedata_size;
		m_exec_pagedata_base = other.m_exec_pagedata_base;
		m_exec_pagedata = std::move(other.m_exec_pagedata);

		m_decoder_cache_size = other.m_decoder_cache_size;
		m_decoder_cache = std::move(other.m_decoder_cache);

#ifdef RISCV_BINARY_TRANSLATION
		m_translator_mappings = std::move(other.m_translator_mappings);
		m_bintr_dl = other.m_bintr_dl;
		other.m_bintr_dl = nullptr;
#endif
	}

	template <int W>
	inline DecodedExecuteSegment<W>::~DecodedExecuteSegment()
	{
#ifdef RISCV_BINARY_TRANSLATION
		extern void  dylib_close(void* dylib, bool is_libtcc);
		if (m_bintr_dl)
			dylib_close(m_bintr_dl, m_is_libtcc);
#endif
	}

} // riscv
