#include "../cpu.hpp"
#include "../decoded_exec_segment.hpp"
#include "../decoder_cache.hpp"
#include "../machine.hpp"
#include "../livepatch.hpp"
#include "../threaded_bytecodes.hpp"
#include "aj_emit.hpp"
#include "aj_runtime.hpp"

#include <chrono>
#include <cstdio>
#include <set>
#include <variant>
#include <vector>

namespace riscv
{
	template <int W>
	static inline DecoderData<W>& aj_decoder_entry_at(DecoderData<W>* cache, address_type<W> addr) noexcept
	{
		return cache[addr / DecoderData<W>::DIVISOR];
	}

	// Walks the segment linearly using real instruction lengths, exactly the way
	// the decoder cache does, so that an address is only ever treated as an
	// instruction boundary when the interpreter agrees that it is one.
	template <int W>
	struct AjSegmentMap
	{
		using address_t = address_type<W>;

		address_t begin, end;
		std::vector<bool> valid_start;
		std::set<address_t> entries;
		/// @brief Addresses that must stay with the interpreter no matter what
		/// instruction they hold, because something patched their decoder entry.
		const std::set<address_t>& blocked;

		bool is_instruction(address_t pc) const noexcept {
			return pc >= begin && pc < end && (pc & 1) == 0
				&& valid_start[(pc - begin) / 2];
		}
		/// @brief Emittability as region discovery sees it: an instruction the
		/// emitter handles, at an address nobody else has claimed.
		bool is_emittable_at(address_t pc, const AjDecoded& d) const noexcept {
			return aj_is_emittable<W>(d) && blocked.count(pc) == 0;
		}
		void consider_entry(address_t pc) {
			if (is_instruction(pc) && blocked.count(pc) == 0) entries.insert(pc);
		}

		AjSegmentMap(const uint8_t* seg, address_t b, address_t e,
			const std::set<address_t>& blk)
			: begin(b), end(e), valid_start((e - b + 1) / 2, false), blocked(blk)
		{
			std::vector<address_t> candidates;
			for (address_t pc = begin; pc + 2 <= end; )
			{
				const auto d = aj_decode<W>(seg, pc, end);
				if (d.length == 0 || pc + d.length > end)
					break;
				valid_start[(pc - begin) / 2] = true;

				// Every basic-block leader becomes an entry point, not just branch
				// targets: without the address *after* a call, a returning function
				// would land in the middle of a region and fall back to the
				// interpreter for the rest of its caller.
				if (is_emittable_at(pc, d)) {
					switch (d.instr.opcode()) {
					case RV32I_JAL:
						candidates.push_back(pc + d.instr.Jtype.jump_offset());
						if (d.instr.Jtype.rd != 0)
							candidates.push_back(pc + d.length);   // return address
						break;
					case RV32I_BRANCH:
						candidates.push_back(pc + d.instr.Btype.signed_imm());
						break;
					case RV32I_JALR:
						candidates.push_back(pc + d.length);
						break;
					default:
						break;
					}
				} else {
					// The interpreter handles this one; pick the trace back up after it.
					candidates.push_back(pc + d.length);
				}
				pc += d.length;
			}

			consider_entry(begin);
			for (const address_t c : candidates)
				consider_entry(c);
		}
	};

	// Collects every address reachable from `entry` by fall-through and by direct
	// branches, stopping at indirect jumps and at anything the emitter cannot
	// handle. Bounding the region this way (rather than "everything up to the
	// first terminator") is what keeps a region from swallowing the unreachable
	// tail of the segment and duplicating it into every other region.
	template <int W>
	static std::vector<address_type<W>> aj_discover_region(const uint8_t* seg,
		const AjSegmentMap<W>& map, address_type<W> entry, size_t max_instructions)
	{
		using address_t = address_type<W>;
		std::set<address_t> seen;
		std::vector<address_t> work { entry };

		while (!work.empty())
		{
			const address_t pc = work.back();
			work.pop_back();
			if (!map.is_instruction(pc) || seen.count(pc))
				continue;
			if (seen.size() >= max_instructions)
				continue;   // capped: anything left over becomes a region exit
			const auto d = aj_decode<W>(seg, pc, map.end);
			if (!map.is_emittable_at(pc, d))
				continue;
			seen.insert(pc);

			switch (d.instr.opcode()) {
			case RV32I_JAL:
				work.push_back(pc + d.instr.Jtype.jump_offset());
				break;
			case RV32I_JALR:
				break;   // indirect: always a region exit
			case RV32I_BRANCH:
				work.push_back(pc + d.instr.Btype.signed_imm());
				[[fallthrough]];
			default:
				work.push_back(pc + d.length);
				break;
			}
		}
		return { seen.begin(), seen.end() };
	}

	template <int W>
	static AjInfo<W> aj_machine_info(const CPU<W>& cpu)
	{
		// Displacements are measured from a live CPU instance, which is robust
		// against any CPU<W>/Machine<W> layout change and avoids offsetof() on a
		// non-standard-layout type.
		const auto& mem = cpu.machine().memory;
		const auto cpu_addr = uintptr_t(&cpu);

		AjInfo<W> info;
		info.reg_offset    = int32_t(uintptr_t(&cpu.registers().get(0)) - cpu_addr);
		info.fpreg_offset  = int32_t(uintptr_t(&cpu.registers().getfl(0)) - cpu_addr);
		info.arena_ptr     = int32_t(uintptr_t(&mem.memory_arena_ptr_ref()) - cpu_addr);
		info.arena_rdbound = int32_t(uintptr_t(&mem.memory_arena_read_boundary_ref()) - cpu_addr);
		info.arena_wrbound = int32_t(uintptr_t(&mem.memory_arena_write_boundary_ref()) - cpu_addr);
		info.arena_roend   = int32_t(uintptr_t(&mem.initial_rodata_end_ref()) - cpu_addr);
		// Inlined arena access needs the plain flat arena: the N-bit encompassing
		// arena and the unaligned slow paths both change what a valid access is.
		info.inline_memory = riscv::flat_readwrite_arena
			&& !riscv::unaligned_memory_slowpaths
			&& riscv::encompassing_Nbit_arena == 0
			&& mem.uses_flat_memory_arena();
		info.cb = &aj_callbacks<W>();
		return info;
	}

	// RV32 and RV64. RV128 and hosts without a code generator fall back to the
	// interpreter.
	template <int W>
	static void aj_translate_segment(const CPU<W>& cpu,
		const MachineOptions<W>& options, DecodedExecuteSegment<W>& exec,
		bool live_patch)
	{
		using address_t = address_type<W>;

		if (!options.asmjit_enabled || exec.empty())
			return;

		const address_t begin = exec.exec_begin();
		const address_t end   = exec.exec_end();
		if (end <= begin || end - begin < 2)
			return;
		const auto* seg = exec.exec_data();

		const auto t0 = std::chrono::steady_clock::now();

		// --- 0. Addresses the emitter must not swallow ----------------------------
		// A breakpoint is installed by rewriting one decoder entry, which a region
		// that inlined that address never consults. Regions are emitted before the
		// breakpoints are installed, so the addresses are resolved here the same
		// way decoder_cache.cpp resolves them, and made to terminate a region.
		std::set<address_t> blocked;
		for (const auto& loc : options.ebreak_locations) {
			const address_t addr = std::holds_alternative<address_t>(loc)
				? std::get<address_t>(loc)
				: cpu.machine().address_of(std::get<std::string>(loc));
			if (addr >= begin && addr < end)
				blocked.insert(addr);
		}

		// --- 1. Instruction boundaries and entry points ---------------------------
		const AjSegmentMap<W> map { seg, begin, end, blocked };

		// --- 2. One region per entry point ---------------------------------------
		struct Region {
			address_t entry;
			std::vector<address_t> instrs;   // ascending; may start below `entry`
		};
		std::vector<Region> regions;
		size_t emitted_instrs = 0;
		for (const address_t entry : map.entries)
		{
			if (regions.size() >= options.asmjit_blocks_max)
				break;
			if (emitted_instrs >= options.asmjit_instr_max)
				break;
			auto instrs = aj_discover_region<W>(seg, map, entry, options.asmjit_region_instr_max);
			if (instrs.empty())
				continue;   // nothing emittable at this address
			emitted_instrs += instrs.size();
			regions.push_back({entry, std::move(instrs)});
		}
		if (regions.empty())
			return;

		// --- 3. Emit --------------------------------------------------------------
		const AjInfo<W> info = aj_machine_info<W>(cpu);

		auto ajcode = std::make_shared<AjCode>();
		auto& mappings = exec.create_asmjit_mappings(regions.size());

		unsigned live = 0;
		for (size_t i = 0; i < regions.size(); i++) {
			mappings[i] = aj_emit_region<W>(*ajcode, options, exec, info,
				regions[i].entry, regions[i].instrs);
			if (mappings[i]) live++;
		}
		if (live == 0) {
			exec.create_asmjit_mappings(0);
			return;
		}
		exec.set_asmjit_code(std::move(ajcode));

		// --- 4. Claim decoder entries ---------------------------------------------
		// Running synchronously, the decoder cache has not been generated yet: the
		// claimed entries are skipped by the generator and realize_fastsim() ends
		// the surrounding block at them. A background translation arrives long
		// after that, so it patches a copy of the finished cache instead and
		// live-patches running threads over to it.
		std::unique_ptr<LivePatchedDecoderCache<W>> patched;
		if (live_patch) {
			// The decoder cache is finished by the thread that started us, and
			// activation both reads and patches it. Wait for it to be complete.
			exec.wait_for_decoder_cache_ready();
			patched = std::make_unique<LivePatchedDecoderCache<W>>(exec, regions.size());
		}

		unsigned claimed = 0;
		for (size_t i = 0; i < regions.size(); i++) {
			if (mappings[i] == nullptr)
				continue;
		#ifdef RISCV_BINARY_TRANSLATION
			// Binary translation ran first and already owns this entry.
			// (When asmjit_override_bintr is set, asmjit runs first instead and
			// the symmetric check in tr_translate.cpp keeps bintr off these.)
			if (aj_decoder_entry_at(exec.decoder_cache(), regions[i].entry)
				.get_bytecode() == RV32I_BC_TRANSLATOR)
				continue;
		#endif
			auto& entry = live_patch
				? patched->claim(regions[i].entry, options.verbose_loader)
				: aj_decoder_entry_at(exec.decoder_cache(), regions[i].entry);
			entry.set_bytecode(RV32I_BC_ASMJIT);
			entry.set_invalid_handler();
			entry.instr  = unsigned(i);
			entry.idxend = 0;
		#ifdef RISCV_EXT_C
			entry.icount = 0;
		#endif
			claimed++;
		}

		if (live_patch) {
			// Hand the patched decoder cache to the execute segment, and switch
			// any thread that is running inside a claimed block over to it.
			patched->activate(true);
		}

		if (options.asmjit_verbose || options.asmjit_timing) {
			const auto ms = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - t0).count();
			printf("libriscv: asmjit emitted %u/%zu regions (%u claimed), "
				"%zu instructions in %.2f ms\n",
				live, regions.size(), claimed, emitted_instrs, ms);
		}
	}

	template <int W>
	void CPU<W>::asmjit_translate(const MachineOptions<W>& options,
		std::shared_ptr<DecodedExecuteSegment<W>>& shared_segment) const
	{
#if RISCV_ASMJIT_HAS_BACKEND
		if constexpr (W == 4 || W == 8) {
			// Emitting takes long enough on a large program to be worth moving off
			// the calling thread. The user decides how (and whether) that happens,
			// exactly like binary translation does.
			const bool live_patch = options.asmjit_background_callback != nullptr;
			if (!live_patch) {
				aj_translate_segment<W>(*this, options, *shared_segment, false);
				return;
			}

			// The options are copied, as the caller is free to let its own copy go
			// out of scope while we are still translating.
			std::function<void()> translation_step =
			[this, options, shared_segment = shared_segment] () mutable
			{
				try {
					aj_translate_segment<W>(*this, options, *shared_segment, true);
				} catch (const std::exception& e) {
					if (options.verbose_loader) {
						fprintf(stderr, "libriscv: asmjit translation failed: %s\n", e.what());
					}
					shared_segment->set_background_compiling(false);
					throw;
				}
				shared_segment->set_background_compiling(false);
			};

			shared_segment->set_background_compiling(true);
			try {
				options.asmjit_background_callback(translation_step);
			} catch (...) {
				// If the callback failed to take ownership of the translation step,
				// nobody will ever clear the flag, and the execute segment would
				// block forever on destruction. Clear it here instead.
				shared_segment->set_background_compiling(false);
				throw;
			}
			return;
		}
#endif
		// RV128, and hosts asmjit has no code generator for, stay interpreted.
		(void)options; (void)shared_segment;
	}

#ifdef RISCV_32I
	template void CPU<4>::asmjit_translate(const MachineOptions<4>&, std::shared_ptr<DecodedExecuteSegment<4>>&) const;
#endif
#ifdef RISCV_64I
	template void CPU<8>::asmjit_translate(const MachineOptions<8>&, std::shared_ptr<DecodedExecuteSegment<8>>&) const;
#endif
#ifdef RISCV_128I
	template void CPU<16>::asmjit_translate(const MachineOptions<16>&, std::shared_ptr<DecodedExecuteSegment<16>>&) const;
#endif
}
