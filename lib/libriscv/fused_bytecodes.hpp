#pragma once
#include "decoder_cache.hpp"
#include "threaded_bytecodes.hpp"

namespace riscv {

/// @brief A fused pair executes two decoder entries, and is
/// only valid as long as the entry is a fused bytecode
template <int W>
inline bool is_fused_bytecode(const DecoderData<W>& entry) noexcept
{
	const unsigned bc = entry.get_bytecode();
	return bc >= RV32I_BC_FUSED_FIRST && bc < BYTECODES_MAX;
}

/// @brief Restore the decoder entry to its unfused bytecode
template <int W>
inline void unfuse_entry(DecoderData<W>& entry) noexcept
{
	if (is_fused_bytecode(entry))
		entry.set_bytecode(fused_pairs[entry.get_bytecode() - RV32I_BC_FUSED_FIRST].first);
}

/// @brief The length of the first instruction in a fused pair (compressed 2, otherwise 4)
template <int W>
inline unsigned fused_first_insn_length(const DecoderData<W>& entry) noexcept
{
	const unsigned first = fused_pairs[entry.get_bytecode() - RV32I_BC_FUSED_FIRST].first;
	return (first >= RV32C_BC_FIRST && first <= RV32C_BC_LAST) ? 2 : 4;
}

/// @brief Called before modifying a decoder entry at `addr` (after decoding)
/// @param exec_decoder Base-address-relative decoder pointer
/// @param begin The first address of the execute segment
template <int W>
inline void unfuse_entry_before(DecoderData<W>* exec_decoder, address_type<W> begin, address_type<W> addr) noexcept
{
	for (const unsigned len : {2u, 4u}) {
		if (len == 2 && !compressed_enabled)
			continue;
		if (addr < begin + len)
			continue;
		auto& prev = exec_decoder[(addr - len) / DecoderData<W>::DIVISOR];
		if (is_fused_bytecode(prev) && fused_first_insn_length(prev) == len)
			unfuse_entry(prev);
	}
}

} // riscv
