#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvc.hpp"
#include "threaded_rewriter.cpp"

namespace riscv
{
	struct UnalignedLoad32 {
		uint16_t data[2];
		operator uint32_t() {
			return data[0] | uint32_t(data[1]) << 16;
		}
	};
	struct AlignedLoad16 {
		uint16_t data;
		operator uint32_t() { return data; }
	};
	static inline rv32i_instruction read_instruction(
		const uint8_t* exec_segment, uint64_t pc, uint64_t end_pc)
	{
		if (pc + 4 <= end_pc)
			return {*(UnalignedLoad32 *)&exec_segment[pc]};
		else
			return {*(AlignedLoad16 *)&exec_segment[pc]};
	}

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

	static bool is_stopping_system(rv32i_instruction instr) {
		if (instr.opcode() == RV32I_SYSTEM) {
			return instr.Itype.funct3 == 0
				&& (instr.Itype.imm == 0  // System call
					|| instr.Itype.imm == 0x105   // WFI
					|| instr.Itype.imm == 0x7ff); // STOP
		}
		return false;
	}

	template <int W>
	static void realize_fastsim(
		address_type<W> base_pc, address_type<W> last_pc,
		const uint8_t* exec_segment, DecoderData<W>* exec_decoder)
	{
		if constexpr (compressed_enabled)
		{
			if (UNLIKELY(base_pc >= last_pc))
				throw MachineException(INVALID_PROGRAM, "The execute segment has an overflow");
			if (UNLIKELY(base_pc & 0x3))
				throw MachineException(INVALID_PROGRAM, "The execute segment is misaligned");

			// Go through entire executable segment and measure lengths
			// Record entries while looking for jumping instruction, then
			// fill out data and opcode lengths previous instructions.
			std::vector<DecoderData<W>*> data;
			address_type<W> pc = base_pc;
			while (pc < last_pc) {
				size_t datalength = 0;
				address_type<W> block_pc = pc;
				[[maybe_unused]] unsigned last_length = 0;
				while (true) {
					// Record the instruction
					auto& entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];
					data.push_back(&entry);

					const auto instruction = read_instruction(
						exec_segment, pc, last_pc);
					const auto opcode = instruction.opcode();
					const auto length = instruction.length();
					// Make sure PC does not overflow
					if (UNLIKELY(__builtin_add_overflow(pc, length, &pc)))
						throw MachineException(INVALID_PROGRAM, "PC overflow during execute segment decoding");
					// If ended up crossing last_pc, it's an invalid block
					if (UNLIKELY(pc > last_pc))
						throw MachineException(INVALID_PROGRAM, "Encountered invalid block");

					datalength += length / 2;
					last_length = length;

					// All opcodes that can modify PC
					if (length == 2)
					{
						if (!is_regular_compressed<W>(instruction.half[0]))
							break;
					} else {
						if (opcode == RV32I_BRANCH || is_stopping_system(instruction)
							|| opcode == RV32I_JAL || opcode == RV32I_JALR
							|| opcode == RV32I_AUIPC || entry.instr == FASTSIM_BLOCK_END)
							break;
					}
					// If we reached the end, and the opcode is not "stopping",
					// then it's an illegal block.
					if (UNLIKELY(pc >= last_pc)) {
						entry.m_bytecode = 0;
						entry.m_handler = 0;
						break;
					}
					// Too large blocks are likely malicious (although could be many empty pages)
					if (UNLIKELY(datalength >= 255)) {
						break;
					}
				}

				if (UNLIKELY(data.size() == 0))
					throw MachineException(INVALID_PROGRAM, "Encountered empty block after measuring");

				for (size_t i = 0; i < data.size(); i++) {
					auto* entry = data[i];

					const auto instruction = read_instruction(
						exec_segment, block_pc, last_pc);
					const auto length = instruction.length();

					// Ends at instruction *before* last PC
					// Subtract block PC in order to get length,
					// then store half
					auto count = (pc - last_length - block_pc) / 2;
					if (count > 255)
						throw MachineException(INVALID_PROGRAM, "Too many non-branching instructions in a row");
					entry->idxend = count;
					entry->icount = count + 1 - (data.size() - i);

					block_pc += length;
					datalength -= length / 2;
				}
				data.clear();
			}
		} else { // !compressed_enabled
			// Count distance to next branching instruction backwards
			// and fill in idxend for all entries along the way.
			// This is for uncompressed instructions, which are always
			// 32-bits in size. We can use the idxend value for
			// instruction counting.
			unsigned idxend = 0;
			address_type<W> pc = last_pc - 4;
			// NOTE: The last check avoids overflow
			while (pc >= base_pc && pc < last_pc)
			{
				const auto instruction = read_instruction(
					exec_segment, pc, last_pc);
				auto& entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];
				const auto opcode = instruction.opcode();

				// All opcodes that can modify PC and stop the machine
				if (opcode == RV32I_BRANCH || is_stopping_system(instruction)
					|| opcode == RV32I_JAL || opcode == RV32I_JALR
					|| opcode == RV32I_AUIPC || entry.instr == FASTSIM_BLOCK_END)
					idxend = 0;
				// Ends at *one instruction before* the block ends
				entry.idxend = idxend;
				// Increment after, idx becomes block count - 1
				idxend ++;

				pc -= 4;
			}
		}
	}

	// The decoder cache is a sequential array of DecoderData<W> entries
	// each of which (currently) serves a dual purpose of enabling
	// threaded dispatch (m_bytecode) and fallback to callback function
	// (m_handler). This enables high-speed emulation, precise simulation,
	// CLI debugging and remote GDB debugging without rebuilding the emulator.
	//
	// The decoder cache covers all pages that the execute segment belongs
	// in, so that all legal jumps (based on page +exec permission) will
	// result in correct execution (including invalid instructions).
	//
	// The goal of the decoder cache is to allow uninterrupted execution
	// with minimal bounds-checking, while also enabling accurate
	// instruction counting.
	template <int W> RISCV_INTERNAL
	void Memory<W>::generate_decoder_cache(
		[[maybe_unused]] const MachineOptions<W>& options,
		DecodedExecuteSegment<W>& exec)
	{
		const auto pbase = exec.pagedata_base();
		const auto addr  = exec.exec_begin();
		const auto len   = exec.exec_end() - exec.exec_begin();

		constexpr size_t PMASK = Page::size()-1;
		const size_t prelen  = addr - pbase;
		const size_t midlen  = len + prelen;
		const size_t plen = (midlen + PMASK) & ~PMASK;

		const size_t n_pages = plen / Page::size();
		if (n_pages == 0) {
			throw MachineException(INVALID_PROGRAM,
				"Program produced empty decoder cache");
		}
		// there could be an old cache from a machine reset
		auto* decoder_cache = exec.create_decoder_cache(
			new DecoderCache<W> [n_pages],
			n_pages * sizeof(DecoderCache<W>));
		auto* exec_decoder = 
			decoder_cache[0].get_base() - pbase / DecoderCache<W>::DIVISOR;
		exec.set_decoder(exec_decoder);

		DecoderData<W> invalid_op;
		invalid_op.set_handler(this->machine().cpu.decode({0}));
		if (UNLIKELY(invalid_op.m_handler != 0)) {
			throw MachineException(INVALID_PROGRAM,
				"The invalid instruction did not have the index zero", invalid_op.m_handler);
		}

		// PC-relative pointer to instruction bits
		auto* exec_segment = exec.exec_data();

#ifdef RISCV_BINARY_TRANSLATION
		// We do not support binary translation for RV128I
		// Also, don't run the translator again (for now)
		if (W != 16 && !is_binary_translated()) {
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
		const address_t end_addr = addr + len;
		for (; dst < addr + len;)
		{
			auto& entry = exec_decoder[dst / DecoderCache<W>::DIVISOR];
			entry.instr = 0x0;
			entry.idxend = 0;

			// Load unaligned instruction from execute segment
			const auto instruction = read_instruction(
				exec_segment, dst, end_addr);
			rv32i_instruction rewritten = instruction;

#ifdef RISCV_BINARY_TRANSLATION
			if (machine().is_binary_translated()) {
				if (entry.isset()) {
					// With fastsim we pretend the original opcode is JAL,
					// which breaks the fastsim loop. In all cases, continue.
					entry.instr = FASTSIM_BLOCK_END;
					entry.set_bytecode(CPU<W>::computed_index_for(entry.instr));
					dst += 4;
					continue;
				}
			}
#endif // RISCV_BINARY_TRANSLATION

			// Insert decoded instruction into decoder cache
			Instruction<W> decoded = CPU<W>::decode(instruction);
			entry.set_handler(decoded);

			// Cache the (modified) instruction bits
			auto bytecode = CPU<W>::computed_index_for(instruction);
			// Threaded rewrites are **always** enabled
			bytecode = exec.threaded_rewrite(bytecode, dst, rewritten);
			entry.set_bytecode(bytecode);
			entry.instr = rewritten.whole;

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

		realize_fastsim<W>(addr, dst, exec_segment, exec_decoder);
	}

	template <int W> RISCV_INTERNAL
	size_t DecoderData<W>::handler_index_for(Handler new_handler)
	{
		auto it = handler_cache.find(new_handler);
		if (it != handler_cache.end())
			return it->second;

		instr_handlers.push_back(new_handler);
		const size_t idx = instr_handlers.size()-1;
		handler_cache.try_emplace(new_handler, idx);
		return idx;
	}

	// An execute segment contains a sequential array of raw instruction bits
	// belonging to a set of sequential pages with +exec permission.
	// It also contains a decoder cache that is produced from this instruction data.
	// It is not strictly necessary to store the raw instruction bits, however, it
	// enables step by step simulation as well as CLI- and remote debugging without
	// rebuilding the emulator.
	// XXX: Moved here to work around a GCC bug
	template <int W> RISCV_INTERNAL
	DecodedExecuteSegment<W>& Memory<W>::create_execute_segment(
		const MachineOptions<W>& options, const void *vdata, address_t vaddr, size_t exlen)
	{
		if constexpr (compressed_enabled) {
			if (UNLIKELY(exlen % 2))
				throw MachineException(INVALID_PROGRAM, "Misaligned execute segment length");
		} else {
			if (UNLIKELY(exlen % 4))
				throw MachineException(INVALID_PROGRAM, "Misaligned execute segment length");
		}

		constexpr address_t PMASK = Page::size()-1;
		const address_t pbase = vaddr & ~PMASK;
		const size_t prelen  = vaddr - pbase;
		const size_t midlen  = exlen + prelen;
		const size_t plen = (midlen + PMASK) & ~PMASK;
		const size_t postlen = plen - midlen;
		//printf("Addr 0x%X Len %zx becomes 0x%X->0x%X PRE %zx MIDDLE %zu POST %zu TOTAL %zu\n",
		//	vaddr, exlen, pbase, pbase + plen, prelen, exlen, postlen, plen);
		if (UNLIKELY(prelen > plen || prelen + exlen > plen)) {
			throw MachineException(INVALID_PROGRAM, "Segment virtual base was bogus");
		}
		// An additional wrap-around check because we are adding 12 bytes
		// as well as additional padding to len.
		if (UNLIKELY(pbase + plen < pbase)) {
			throw MachineException(INVALID_PROGRAM, "Segment virtual base was bogus");
		}

		// Create the whole executable memory range
		m_exec.emplace_back(pbase, plen, vaddr, exlen);
		auto& current_exec = m_exec.back();

		auto* exec_data = current_exec.exec_data(pbase);
		std::memset(&exec_data[0],      0,     prelen);
		std::memcpy(&exec_data[prelen], vdata, exlen);
		std::memset(&exec_data[prelen + exlen], 0,   postlen);

		this->generate_decoder_cache(options, current_exec);

		return current_exec;
	}

	template <int W>
	DecodedExecuteSegment<W>* Memory<W>::exec_segment_for(address_t vaddr)
	{
		for (auto& segment : m_exec) {
			if (segment.is_within(vaddr)) return &segment;
		}
		return nullptr;
	}

	template <int W>
	const DecodedExecuteSegment<W>* Memory<W>::exec_segment_for(address_t vaddr) const
	{
		return const_cast<Memory<W>*>(this)->exec_segment_for(vaddr);
	}

	template <int W>
	void Memory<W>::evict_execute_segments(size_t remaining_size)
	{
		if (m_exec.size() <= remaining_size)
			return;

		while (m_exec.size() > remaining_size) {
			m_exec.pop_back();
		}
		// XXX: Should probably detect if the current execute
		// segment is already active, but this should also be OK.
		if (!m_exec.empty()) {
			machine().cpu.set_execute_segment(&m_exec[0]);
		} else {
			machine().cpu.set_execute_segment(nullptr);
		}
	}

	template struct Memory<4>;
	template struct Memory<8>;
	template struct Memory<16>;
	template struct DecoderData<4>;
	template struct DecoderData<8>;
	template struct DecoderData<16>;
} // riscv
