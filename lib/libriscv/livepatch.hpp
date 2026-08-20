#pragma once
#include "decoded_exec_segment.hpp"
#include "decoder_cache.hpp"
#include "threaded_bytecodes.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

// Shared machinery for activating native code (binary translation or asmjit)
// on an execute segment whose decoder cache is already realized, which is what
// happens when the translation ran in the background.
//
// The activation cannot patch the live decoder cache in place: another thread
// may be executing out of it right now. Instead a full copy is patched, the
// segment is pointed at the copy, and every patched address gets a LIVEPATCH
// bytecode written atomically into the *original* cache. A thread that reaches
// one of those addresses rewinds to the start of its block, switches to the
// patched cache and continues there.

#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)

namespace riscv
{
	template <int W>
	RISCV_ALWAYS_INLINE DecoderData<W>& livepatch_entry_at(DecoderData<W>* cache, address_type<W> addr) noexcept
	{
		return cache[addr / DecoderData<W>::DIVISOR];
	}

	/// @brief Make `addr` the last instruction of its block in a patched copy of
	/// the decoder cache, by correcting block_bytes() for every entry that
	/// precedes it in the block.
	/// @param exec The execute segment being patched.
	/// @param patched_decoder Base-address-relative pointer into the patched cache.
	/// @param decoder_begin First valid entry of the patched cache.
	/// @param addr The address that native code is about to claim.
	/// @param verbose Print diagnostics before throwing.
	/// @return The patched entry, for the caller to overwrite with its own bytecode.
	/// @note The caller is expected to finish the job by setting the entry to a
	/// block-ending native code bytecode (idxend = 0, icount = 0).
	template <int W>
	inline DecoderData<W>& livepatch_make_block_ending(
		const DecodedExecuteSegment<W>& exec,
		DecoderData<W>* patched_decoder, DecoderData<W>* decoder_begin,
		address_type<W> addr, bool verbose)
	{
		auto& entry = livepatch_entry_at(patched_decoder, addr);
		// Already the last instruction of its block: nothing precedes it that
		// could have a stale block length.
		if (entry.block_bytes() == 0)
			return entry;

		// 1. The claimed instruction becomes the last one of the block, so look
		//    back to find where the block begins. Later instructions are already
		//    correct, as their block_bytes() do not reach across this entry.
		auto* last    = &entry;
		auto* current = &entry;
		auto last_block_bytes = entry.block_bytes();
		while (current > decoder_begin) {
			if ((current-1)->block_bytes() == 0) {
				// We may have reached the middle of an instruction, which has an
				// invalid entry. To validate this, step one more entry back, if
				// possible, and check whether it matches the current block_bytes
				// plus one 4-byte instruction.
				if (current-1 == decoder_begin) {
					// Beginning of the decoder cache, cannot step back further.
					break;
				}
				auto* prev = current-2;
				if (prev->block_bytes() == last_block_bytes + 4) {
					// Step over the invalid entry and continue with the previous one.
					current = prev;
					last_block_bytes = prev->block_bytes();
					continue;
				} else {
					// The end of the block, so stop here.
					break;
				}
			}
			if ((current-1)->block_bytes() < last_block_bytes)
				break; // We have reached another previous block
			current--;
			last_block_bytes = current->block_bytes();
		}
		int block_bytes = last_block_bytes - entry.block_bytes();

		const auto block_begin_addr = addr - block_bytes;
		if (block_begin_addr < exec.exec_begin() || block_begin_addr >= exec.exec_end()) {
			if (verbose)
				fprintf(stderr, "libriscv: Patched address 0x%lX outside execute area 0x%lX-0x%lX\n",
					(long)block_begin_addr, (long)exec.exec_begin(), (long)exec.exec_end());
			throw MachineException(INVALID_PROGRAM, "Native code mapping outside execute area");
		}

		// 2. Correct block_bytes() for all entries in the block
		auto patched_addr = block_begin_addr;
		if (current + block_bytes / (compressed_enabled ? 2 : 4) != last) {
			throw MachineException(INVALID_PROGRAM, "Native code mapping block bytes mismatch");
		}
		for (auto* dd = current; dd < last; dd++) {
			auto& p = livepatch_entry_at(patched_decoder, patched_addr);
		#ifdef RISCV_EXT_C
			if (p.get_bytecode() != 0) { // Avoid invalid entries
				p.icount = last - dd + 1; // This is inexact, but works for now
				p.idxend = block_bytes / 2;
			} else {
				// Setting icount and idxend to 0 on an invalid instruction will
				// improve exception handling/information, if jumped to.
				p.icount = 0; // Invalid entry, no instruction count
				p.idxend = 0; // No index end
			}
		#else
			p.idxend = last - dd;
		#endif
			patched_addr += (compressed_enabled ? 2 : 4);
			block_bytes -= (compressed_enabled ? 2 : 4);
		}
		if (compressed_enabled && block_bytes != 0) {
			if (verbose)
				fprintf(stderr, "libriscv: Patched block bytes mismatch at 0x%lX: %u != 0\n",
					(long)block_begin_addr, block_bytes);
			throw MachineException(INVALID_PROGRAM, "Native code mapping block bytes mismatch");
		}

		return entry;
	}

	/// @brief A patched copy of an execute segments decoder cache.
	/// @details Native code activation fills this in, hands it to the segment and
	/// then live-patches the original entries so running threads switch over.
	template <int W>
	struct LivePatchedDecoderCache
	{
		using address_t = address_type<W>;

		explicit LivePatchedDecoderCache(DecodedExecuteSegment<W>& exec, size_t reserve_patches)
			: m_exec(exec)
		{
#ifdef __cpp_lib_smart_ptr_for_overwrite // C++20 feature
			m_cache = std::make_unique_for_overwrite<DecoderData<W>[]>(exec.decoder_cache_size());
#else
			m_cache = std::make_unique<DecoderData<W>[]>(exec.decoder_cache_size());
#endif
			std::memcpy(m_cache.get(), exec.decoder_cache_base(),
				exec.decoder_cache_size() * sizeof(DecoderData<W>));
			// Base-address-relative pointer into the patched decoder cache
			m_decoder = m_cache.get() - exec.exec_begin() / DecoderData<W>::DIVISOR;
			m_begin   = &livepatch_entry_at(m_decoder, exec.exec_begin());
			m_patched.reserve(reserve_patches);
		}

		/// @brief Prepare `addr` in the copy and record it for live-patching.
		/// @return The patched entry, for the caller to overwrite.
		DecoderData<W>& claim(address_t addr, bool verbose)
		{
			auto& entry = livepatch_make_block_ending<W>(m_exec, m_decoder, m_begin, addr, verbose);
			m_patched.push_back(&livepatch_entry_at(m_exec.decoder_cache(), addr));
			return entry;
		}

		/// @brief Hand the copy to the segment and switch running threads over to it.
		/// @param enable_live_patching When false, the copy becomes the segments
		/// decoder cache but running threads keep using the original one.
		void activate(bool enable_live_patching)
		{
			m_exec.set_patched_decoder_cache(std::move(m_cache), m_decoder);
			m_exec.set_decoder(m_decoder);

			if (!enable_live_patching)
				return;

			// Memory fence to ensure that the patched decoder is visible to all threads
#ifndef __COSMOCC__
			std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
			for (auto* dd : m_patched)
				dd->set_atomic_bytecode_and_handler(RV32I_BC_LIVEPATCH, 0);
		}

		size_t patch_count() const noexcept { return m_patched.size(); }

	private:
		DecodedExecuteSegment<W>& m_exec;
		std::unique_ptr<DecoderData<W>[]> m_cache;
		DecoderData<W>* m_decoder = nullptr;
		DecoderData<W>* m_begin   = nullptr;
		std::vector<DecoderData<W>*> m_patched;
	};

} // riscv

#endif // RISCV_BINARY_TRANSLATION || RISCV_ASMJIT
