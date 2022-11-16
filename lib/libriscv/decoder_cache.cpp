#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvc.hpp"

namespace riscv
{
	union UnalignedLoad32 {
		uint16_t data[2];
		operator uint32_t() {
			return data[0] | uint32_t(data[1]) << 16;
		}
	};

#ifdef RISCV_FAST_SIMULATOR
	template <int W>
	static bool is_regular_compressed(uint16_t instr) {
		const rv32c_instruction ci { instr };
		#define CI_CODE(x, y) ((x << 13) | (y))
		switch (ci.opcode()) {
		case CI_CODE(0b001, 0b01):
			if constexpr (W >= 8) return true; // C.ADDIW
			return false; // C.JAL 32-bit
		case CI_CODE(0b101, 0b01): // C.JMP
		case CI_CODE(0b110, 0b01): // C.BEQZ
		case CI_CODE(0b111, 0b01): // C.BNEZ
			return false;
		case CI_CODE(0b100, 0b10): { // VARIOUS
				const bool topbit = ci.whole & (1 << 12);
				if (!topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0) {
					return false; // C.JR rd
				} else if (topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0) {
					return false; // C.JALR ra, rd+0
				} // TODO: Handle C.EBREAK
				return true;
			}
		default:
			return true;
		}
	}

	// While we do somewhat care about the precise amount of instructions per block,
	// there is never really going to be any one block with more than 255 raw instructions.
	// Still, we do care about making progress towards the instruction limits.
	inline uint8_t overflow_checked_instr_count(size_t count) {
		return (count > 255) ? 255 : count;
	}

	template <int W>
	static void realize_fastsim(
		address_type<W> base_pc, address_type<W> last_pc,
		const uint8_t* exec_segment, DecoderData<W>* exec_decoder)
	{
		if constexpr (compressed_enabled)
		{
			// Go through entire executable segment and measure lengths
			// Record entries while looking for jumping instruction, then
			// fill out data and opcode lengths previous instructions.
			std::vector<DecoderData<W>*> data;
			address_type<W> pc = base_pc;
			while (pc < last_pc) {
				size_t datalength = 0;
				while (pc < last_pc) {
					auto& entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];
					data.push_back(&entry);

					const rv32i_instruction instruction{*(UnalignedLoad32 *)&exec_segment[pc]};
					const auto opcode = instruction.opcode();
					const auto length = instruction.length();
					pc += length;
					datalength += length / 2;

					// All opcodes that can modify PC
					if (length == 2)
					{
						if (!is_regular_compressed<W>(instruction.half[0]))
							break;
					} else {
						if (opcode == RV32I_BRANCH || opcode == RV32I_SYSTEM
							|| opcode == RV32I_JAL || opcode == RV32I_JALR
							|| opcode == RV32I_AUIPC)
							break;
					}
				}
				for (size_t i = 0; i < data.size(); i++) {
					auto* entry = data[i];
					const auto length = rv32i_instruction{entry->original_opcode}.length();
					// Ends at *last instruction*
					entry->idxend = datalength;
					// XXX: original_opcode gets overwritten here by opcode_length
					// which simplifies future simulation by simplifying length.
					entry->opcode_length = length;
					// XXX: We have to pack the instruction count by combining it with the cb length
					// in order to avoid overflows on large code blocks. The code block length
					// has been sufficiently large to avoid overflows in all executables tested.
					entry->instr_count = overflow_checked_instr_count(datalength - (data.size() - i));
					datalength -= length / 2;
				}
				data.clear();
			}
		} else { // !compressed_enabled
			// Count distance to next branching instruction backwards
			// and fill in idxend for all entries along the way.
			unsigned idxend = 0;
			address_type<W> pc = last_pc - 4;
			while (pc >= base_pc)
			{
				const rv32i_instruction instruction{*(UnalignedLoad32 *)&exec_segment[pc]};
				auto& entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];
				const auto opcode = instruction.opcode();

				// All opcodes that can modify PC and stop the machine
				if (opcode == RV32I_BRANCH || opcode == RV32I_SYSTEM
					|| opcode == RV32I_JAL || opcode == RV32I_JALR
					|| opcode == RV32I_AUIPC)
					idxend = 0;
				// Ends at *one instruction before* the block ends
				entry.idxend = idxend;
				// Increment after, idx becomes block count - 1
				idxend ++;

				pc -= 4;
			}
		}
	}
#else
#define VERBOSE_FASTSIM  false
#endif // RISCV_FAST_SIMULATOR

	template <int W> RISCV_INTERNAL
	void Memory<W>::generate_decoder_cache(
		[[maybe_unused]] const MachineOptions<W>& options,
		address_t pbase, address_t addr, size_t len)
	{
		constexpr size_t PMASK = Page::size()-1;
		const size_t prelen  = addr - pbase;
		const size_t midlen  = len + prelen;
		const size_t plen = (midlen + PMASK) & ~PMASK;

		const size_t n_pages = plen / Page::size();
		auto* decoder_array = new DecoderCache<W> [n_pages];
		this->m_exec_decoder =
			decoder_array[0].get_base() - pbase / decoder_array->DIVISOR;
		// there could be an old cache from a machine reset
		delete[] this->m_decoder_cache;
		this->m_decoder_cache = &decoder_array[0];

		// Avoid using Memory::m_exec_pagedata here.
		// We choose to use the CPU execute segment,
		// because it is more authoritative.
		auto* exec_segment = this->machine().cpu.exec_seg_data();
		assert(exec_segment != nullptr && "Must have set CPU execute segment");
		auto* exec_decoder = this->m_exec_decoder;

	#ifdef RISCV_BINARY_TRANSLATION
		/* We do not support binary translation for RV128I */
		if constexpr (W != 16) {
			// Attempt to load binary translation
			// Also, fill out the binary translation SO filename for later
			std::string bintr_filename;
			machine().cpu.load_translation(options, &bintr_filename);

			if (!machine().is_binary_translated())
			{
				// This can be improved somewhat, by fetching them on demand
				// instead of building a vector of the whole execute segment.
				std::vector<TransInstr<W>> ipairs;
				ipairs.reserve(len / 4);

				for (address_t dst = addr; dst < addr + len; dst += 4)
				{
					// Load unaligned instruction from execute segment
					const rv32i_instruction instruction { *(UnalignedLoad32*) &exec_segment[dst] };
					ipairs.push_back({instruction.whole});
				}
				machine().cpu.try_translate(
					options, bintr_filename, addr, std::move(ipairs));
			}
		} // W != 16
	#endif

		// When compressed instructions are enabled, many decoder
		// entries are illegal because they between instructions.
		bool was_full_instruction = true;

		/* Generate all instruction pointers for executable code.
		   Cannot step outside of this area when pregen is enabled,
		   so it's fine to leave the boundries alone. */
		address_t dst = addr;
		for (; dst < addr + len;)
		{
			auto& entry = exec_decoder[dst / DecoderCache<W>::DIVISOR];

			// Load unaligned instruction from execute segment
			const rv32i_instruction instruction { *(UnalignedLoad32*) &exec_segment[dst] };
			rv32i_instruction rewritten = instruction;

#ifdef RISCV_BINARY_TRANSLATION
			if (machine().is_binary_translated()) {
				if (entry.isset()) {
					// With fastsim we pretend the original opcode is JAL,
					// which breaks the fastsim loop. In all cases, continue.
					#ifdef RISCV_FAST_SIMULATOR
					entry.original_opcode = RV32I_JAL;
					#endif
					dst += 4;
					continue;
				}
			}
#endif // RISCV_BINARY_TRANSLATION

#ifdef RISCV_FAST_SIMULATOR
			// Help the fastsim determine the real opcodes
			// Also, put whole 16-bit instructions there.
			// NOTE: Gets overwritten for opcode_length later.
			entry.original_opcode = instruction.half[0];
			entry.idxend = 0;
#endif

			// Insert decoded instruction into decoder cache
			Instruction<W> decoded;
			// The rewriter can rewrite full instructions, so lets only
			// invoke it when we have a decoder cache with full instructions.
			// TODO: Allow disabling at run-time
			if (decoder_rewriter_enabled) {
				// Improve many instruction handlers by rewriting instructions
				decoded = machine().cpu.decode_rewrite(dst, rewritten);
			} else {
				decoded = machine().cpu.decode(instruction);
			}
			entry.set_handler(decoded);

#ifdef RISCV_FAST_SIMULATOR
			// Cache the (modified) instruction bits
			entry.instr = rewritten.whole;
#endif

			// Increment PC after everything
			if constexpr (compressed_enabled) {
				// With compressed we always step forward 2 bytes at a time
				dst += 2;
				if (was_full_instruction) {
					// For it to be a full instruction again,
					// the length needs to match.
					was_full_instruction = (instruction.length() == 2);
				} else {
					// If it wasn't a full instruction last time, it
					// will for sure be one now.
					was_full_instruction = true;
				}
			} else
				dst += 4;
		}

#ifdef RISCV_FAST_SIMULATOR
		realize_fastsim<W>(addr, dst, exec_segment, exec_decoder);
#endif
	}

	template struct Memory<4>;
	template struct Memory<8>;
	template struct Memory<16>;
} // riscv
