#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rvc.hpp"
#include "safe_instr_loader.hpp"
#include "threaded_rewriter.cpp"
#include "threaded_bytecodes.hpp"
#include "util/crc32.hpp"
#include <mutex>

namespace riscv
{
	static constexpr bool VERBOSE_DECODER = false;

	template <int W>
	struct SharedExecuteSegments {
		SharedExecuteSegments() = default;
		SharedExecuteSegments(const SharedExecuteSegments&) = delete;
		SharedExecuteSegments& operator=(const SharedExecuteSegments&) = delete;

		struct Segment {
			std::shared_ptr<DecodedExecuteSegment<W>> segment;
			std::mutex mutex;

			std::shared_ptr<DecodedExecuteSegment<W>> get() {
				std::lock_guard<std::mutex> lock(mutex);
				return segment;
			}

			void unlocked_set(std::shared_ptr<DecodedExecuteSegment<W>> segment) {
				this->segment = std::move(segment);
			}
		};

		// Remove a segment if it is the last reference
		void remove_if_unique(uint32_t hash) {
			std::lock_guard<std::mutex> lock(mutex);
			// We are not able to remove the Segment itself, as the mutex
			// may be locked by another thread. We can, however, lock the
			// Segments mutex and set the segment to nullptr.
			auto it = m_segments.find(hash);
			if (it != m_segments.end()) {
				std::scoped_lock lock(it->second.mutex);
				if (it->second.segment.use_count() == 1)
					it->second.segment = nullptr;
			}
		}

		auto& get_segment(const uint32_t hash) {
			std::scoped_lock lock(mutex);
			auto& entry = m_segments[hash];
			return entry;
		}

	private:
		std::unordered_map<uint32_t, Segment> m_segments;
		std::mutex mutex;
	};
	template <int W>
	static SharedExecuteSegments<W> shared_execute_segments;

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
			return true;
//			return instr.Itype.funct3 == 0
//				&& (instr.Itype.imm == 0  // System call
//					|| instr.Itype.imm == 0x105   // WFI
//					|| instr.Itype.imm == 0x7ff); // STOP
		}
		return false;
	}
	static bool is_stopping_auipc(rv32i_instruction instr) {
		return (instr.opcode() == RV32I_AUIPC && instr.Utype.rd != 0);
	}

	template <int W>
	static void realize_fastsim(
		address_type<W> base_pc, address_type<W> last_pc,
		const uint8_t* exec_segment, DecoderData<W>* exec_decoder)
	{
#ifdef RISCV_BINARY_TRANSLATION
		const auto translator_op = RV32I_BC_TRANSLATOR;
#endif

		if constexpr (compressed_enabled)
		{
			if (UNLIKELY(base_pc >= last_pc))
				throw MachineException(INVALID_PROGRAM, "The execute segment has an overflow");
			if (UNLIKELY(base_pc & 0x1))
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
					if (pc + length < pc)
						throw MachineException(INVALID_PROGRAM, "PC overflow during execute segment decoding");
					pc += length;

					// If ending up crossing last_pc, it's an invalid block although
					// it could just be garbage, so let's force-end with an invalid instruction.
					if (UNLIKELY(pc > last_pc)) {
						entry.m_bytecode = 0; // Invalid instruction
						entry.m_handler = 0;
						break;
					}

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
							|| is_stopping_auipc(instruction))
							break;
					}
				#ifdef RISCV_BINARY_TRANSLATION
					if (entry.get_bytecode() == translator_op)
						break;
				#endif

					// A last test for the last instruction, which should have been a block-ending
					// instruction. Since it wasn't we must force-end the block here.
					if (UNLIKELY(pc >= last_pc)) {
						entry.m_bytecode = 0; // Invalid instruction
						entry.m_handler = 0;
						break;
					}

					// Too large blocks are likely malicious (although could be many empty pages)
					if (UNLIKELY(datalength >= 255)) {
						// NOTE: Reinsert original instruction, as long sequences will lead to
						// PC becoming desynched, as it doesn't get increased.
						// We use a new block-ending fallback function handler instead.
						entry.set_bytecode(RV32I_BC_FUNCBLOCK);
						entry.set_handler(CPU<W>::decode(instruction));
						entry.instr = instruction.whole;
						break;
					}
				}
				if constexpr (VERBOSE_DECODER) {
					fprintf(stderr, "Block 0x%lX to 0x%lX\n", block_pc, pc);
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

					if constexpr (VERBOSE_DECODER) {
						fprintf(stderr, "Block 0x%lX has %u instructions\n", block_pc, count);
					}

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
					|| is_stopping_auipc(instruction))
					idxend = 0;
			#ifdef RISCV_BINARY_TRANSLATION
				if (entry.get_bytecode() == translator_op)
					idxend = 0;
			#endif
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
		std::shared_ptr<DecodedExecuteSegment<W>>& shared_segment)
	{
		auto& exec = *shared_segment;
		if (exec.exec_end() < exec.exec_begin())
			throw MachineException(INVALID_PROGRAM, "Execute segment was invalid");
		if constexpr (W >= 8)
			exec.set_likely_jit(exec.pagedata_base() >= 0x100000000);

		const auto pbase = exec.pagedata_base();
		const auto addr  = exec.exec_begin();
		const auto len   = exec.exec_end() - exec.exec_begin();
		constexpr size_t PMASK = Page::size()-1;
		// We need to allocate room for at least one more decoder cache entry.
		// This is because jump and branch instructions don't check PC after
		// not branching. The last entry is an invalid instruction.
		const size_t prelen  = addr - pbase;
		const size_t midlen  = len + prelen + 4; // Extra entry
		const size_t plen = (midlen + PMASK) & ~PMASK;
		//printf("generate_decoder_cache: Addr 0x%X Len %zx becomes 0x%X->0x%X PRE %zx MIDDLE %zu TOTAL %zu\n",
		//	addr, len, pbase, pbase + plen, prelen, midlen, plen);

		const size_t n_pages = plen / Page::size();
		if (n_pages == 0) {
			throw MachineException(INVALID_PROGRAM,
				"Program produced empty decoder cache");
		}
		// Here we allocate the decoder cache which is page-sized
		auto* decoder_cache = exec.create_decoder_cache(
			new DecoderCache<W> [n_pages], n_pages);
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
		// Also, avoid binary translation for execute segments that are likely JIT-compiled
		if (W != 16 && !exec.is_binary_translated() && !exec.is_likely_jit()) {
			// Attempt to load binary translation
			// Also, fill out the binary translation SO filename for later
			std::string bintr_filename;
			int result = machine().cpu.load_translation(options, &bintr_filename, exec);
			const bool must_translate = result > 0;
			if (must_translate)
			{
				machine().cpu.try_translate(
					options, bintr_filename, shared_segment, addr, addr + len);
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
			entry.idxend = 0;

			// Load unaligned instruction from execute segment
			const auto instruction = read_instruction(
				exec_segment, dst, end_addr);
			rv32i_instruction rewritten = instruction;

#ifdef RISCV_BINARY_TRANSLATION
			if (entry.get_bytecode() == RV32I_BC_TRANSLATOR) {
				// Translator activation uses a special bytecode
				// but we must still validate the mapping index.
				if (entry.instr >= exec.translator_mappings())
					throw MachineException(INVALID_PROGRAM, "Invalid translator mapping index");
				if constexpr (compressed_enabled) {
					dst += 2;
					if (was_full_instruction) {
						was_full_instruction = (instruction.length() == 2);
					} else {
						was_full_instruction = true;
					}
				} else
					dst += 4;
				continue;
			}
#endif // RISCV_BINARY_TRANSLATION

			if (was_full_instruction) {
				// Insert decoded instruction into decoder cache
				Instruction<W> decoded = CPU<W>::decode(instruction);
				entry.set_handler(decoded);

				// Cache the (modified) instruction bits
				auto bytecode = CPU<W>::computed_index_for(instruction);
				// Threaded rewrites are **always** enabled
				bytecode = exec.threaded_rewrite(bytecode, dst, rewritten);
				entry.set_bytecode(bytecode);
				entry.instr = rewritten.whole;
			} else {
				// WARNING: If we don't ignore this instruction,
				// it will get *wrong* idxend values, and cause *invalid jumps*
				entry.m_handler = 0;
				entry.set_bytecode(0);
				// ^ Must be made invalid, even if technically possible to jump to!
			}
			if constexpr (VERBOSE_DECODER) {
				if (entry.get_bytecode() >= RV32I_BC_BEQ && entry.get_bytecode() <= RV32I_BC_BGEU) {
					fprintf(stderr, "Detected branch bytecode at 0x%lX\n", dst);
				}
				if (entry.get_bytecode() == RV32I_BC_BEQ_FW || entry.get_bytecode() == RV32I_BC_BNE_FW) {
					fprintf(stderr, "Detected forward branch bytecode at 0x%lX\n", dst);
				}
			}

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
		// Make sure the last entry is an invalid instruction
		// This simplifies many other sub-systems
		auto& entry = exec_decoder[(addr + len) / DecoderCache<W>::DIVISOR];
		entry.set_bytecode(0);
		entry.m_handler = 0;
		entry.idxend = 0;

		realize_fastsim<W>(addr, dst, exec_segment, exec_decoder);
	}

	template <int W> RISCV_INTERNAL
	size_t DecoderData<W>::handler_index_for(Handler new_handler)
	{
		auto it = handler_cache.find(new_handler);
		if (it != handler_cache.end())
			return it->second;

		if (UNLIKELY(handler_count >= instr_handlers.size()))
			throw MachineException(INVALID_PROGRAM, "Too many instruction handlers");
		instr_handlers[handler_count] = new_handler;
		const size_t idx = handler_count++;
		handler_cache.try_emplace(new_handler, idx);
		return idx;
	}

	// An execute segment contains a sequential array of raw instruction bits
	// belonging to a set of sequential pages with +exec permission.
	// It also contains a decoder cache that is produced from this instruction data.
	// It is not strictly necessary to store the raw instruction bits, however, it
	// enables step by step simulation as well as CLI- and remote debugging without
	// rebuilding the emulator.
	// Crucially, because of page alignments and 4 extra bytes, the necessary checks
	// when reading from the execute segment is reduced. You can always read 4 bytes
	// no matter where you are in the segment, a whole instruction unchecked.
	template <int W> RISCV_INTERNAL
	DecodedExecuteSegment<W>& Memory<W>::create_execute_segment(
		const MachineOptions<W>& options, const void *vdata, address_t vaddr, size_t exlen)
	{
		if (UNLIKELY(exlen % (compressed_enabled ? 2 : 4)))
			throw MachineException(INVALID_PROGRAM, "Misaligned execute segment length");

		constexpr address_t PMASK = Page::size()-1;
		const address_t pbase = vaddr & ~PMASK;
		const size_t prelen  = vaddr - pbase;
		// Make 4 bytes of extra room to avoid having to validate 4-byte reads
		// when reading at 2 bytes before the end of the execute segment.
		const size_t midlen  = exlen + prelen + 2; // Extra room for reads
		const size_t plen = (midlen + PMASK) & ~PMASK;
		// Because postlen uses midlen, we end up zeroing the extra 4 bytes in the end
		const size_t postlen = plen - midlen;
		//printf("Addr 0x%X Len %zx becomes 0x%X->0x%X PRE %zx MIDDLE %zu POST %zu TOTAL %zu\n",
		//	vaddr, exlen, pbase, pbase + plen, prelen, exlen, postlen, plen);
		if (UNLIKELY(prelen > plen || prelen + exlen > plen)) {
			throw MachineException(INVALID_PROGRAM, "Segment virtual base was bogus");
		}
		if (UNLIKELY(pbase + plen < pbase)) {
			throw MachineException(INVALID_PROGRAM, "Segment virtual base was bogus");
		}

		// Create the whole executable memory range
		auto current_exec = std::make_shared<DecodedExecuteSegment<W>>(pbase, plen, vaddr, exlen);

		auto* exec_data = current_exec->exec_data(pbase);
		// This is a zeroed prologue in order to be able to use whole pages
		std::memset(&exec_data[0],      0,     prelen);
		// This is the actual instruction bytes
		std::memcpy(&exec_data[prelen], vdata, exlen);
		// This memset() operation will end up zeroing the extra 4 bytes
		std::memset(&exec_data[prelen + exlen], 0,   postlen);

		// Create CRC32-C hash of the execute segment
		const uint32_t hash = crc32c(exec_data, current_exec->exec_end() - current_exec->exec_begin());

		// Get a free slot to reference the execute segment
		auto& free_slot = this->next_execute_segment();


		if (options.use_shared_execute_segments)
		{
			// In order to prevent others from creating the same execute segment
			// we need to lock the shared execute segments mutex.
			auto& segment = shared_execute_segments<W>.get_segment(hash);
			std::scoped_lock lock(segment.mutex);

			if (segment.segment != nullptr) {
				free_slot = segment.segment;
				return *free_slot;
			}

			// We need to create a new execute segment, as there is no shared
			// execute segment with the same hash.
			free_slot = std::move(current_exec);
			// Store the hash in the decoder cache
			free_slot->set_crc32c_hash(hash);

			this->generate_decoder_cache(options, free_slot);

			// Share the execute segment
			shared_execute_segments<W>.get_segment(hash).unlocked_set(free_slot);
		}
		else
		{
			free_slot = std::move(current_exec);
			// Store the hash in the decoder cache
			free_slot->set_crc32c_hash(hash);

			this->generate_decoder_cache(options, free_slot);
		}

		return *free_slot;
	}

	template <int W>
	std::shared_ptr<DecodedExecuteSegment<W>>& Memory<W>::next_execute_segment()
	{
		if (LIKELY(m_exec_segs < MAX_EXECUTE_SEGS)) {
			auto& result = this->m_exec.at(m_exec_segs);
			m_exec_segs ++;
			return result;
		}
		throw MachineException(INVALID_PROGRAM, "Max execute segments reached");
	}

	template <int W>
	std::shared_ptr<DecodedExecuteSegment<W>>& Memory<W>::exec_segment_for(address_t vaddr)
	{
		for (size_t i = 0; i < m_exec_segs; i++) {
			auto& segment = m_exec[i];
			if (segment && segment->is_within(vaddr)) return segment;
		}
		return CPU<W>::empty_execute_segment();
	}

	template <int W>
	const std::shared_ptr<DecodedExecuteSegment<W>>& Memory<W>::exec_segment_for(address_t vaddr) const
	{
		return const_cast<Memory<W>*>(this)->exec_segment_for(vaddr);
	}

	template <int W>
	void Memory<W>::evict_execute_segments()
	{
		// destructor could throw, so let's invalidate early
		machine().cpu.set_execute_segment(*CPU<W>::empty_execute_segment());

		m_exec_segs = std::min(m_exec_segs, m_exec.size());
		while (m_exec_segs > 0) {
			m_exec_segs--;

			try {
				auto& segment = m_exec.at(m_exec_segs);
				if (segment) {
					const uint32_t hash = segment->crc32c_hash();
					segment = nullptr;
					shared_execute_segments<W>.remove_if_unique(hash);
				}
			} catch (...) {
				// Ignore exceptions
			}
		}
	}

	template <int W>
	void Memory<W>::evict_execute_segment(DecodedExecuteSegment<W>& segment)
	{
		const uint32_t hash = segment.crc32c_hash();
		for (size_t i = 0; i < m_exec_segs; i++) {
			if (m_exec[i].get() == &segment) {
				m_exec[i] = nullptr;
				if (i == m_exec_segs - 1)
					m_exec_segs--;
				break;
			}
		}
		shared_execute_segments<W>.remove_if_unique(hash);
	}

	INSTANTIATE_32_IF_ENABLED(DecoderData);
	INSTANTIATE_32_IF_ENABLED(Memory);
	INSTANTIATE_64_IF_ENABLED(DecoderData);
	INSTANTIATE_64_IF_ENABLED(Memory);
	INSTANTIATE_128_IF_ENABLED(DecoderData);
	INSTANTIATE_128_IF_ENABLED(Memory);
} // riscv
