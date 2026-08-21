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

	// Linear walk using real instruction lengths, matching the decoder cache's
	// boundary decisions.
	template <int W>
	struct AjSegmentMap
	{
		using address_t = address_type<W>;

		address_t begin, end;
		std::vector<bool> valid_start;
		std::set<address_t> entries;
		/// @brief Addresses excluded from JIT: their decoder entries are patched (e.g. breakpoints).
		const std::set<address_t>& blocked;

		bool is_instruction(address_t pc) const noexcept {
			return pc >= begin && pc < end && (pc & 1) == 0
				&& valid_start[(pc - begin) / 2];
		}
		/// @brief Emittable and unclaimed by breakpoints or live-patches.
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

				// Every basic-block leader is an entry point. Without the
				// address after a call, a returning callee would land mid-region
				// and fall back to the interpreter.
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
					// Non-emittable: resume after it.
					candidates.push_back(pc + d.length);
				}
				pc += d.length;
			}

			consider_entry(begin);
			for (const address_t c : candidates)
				consider_entry(c);
		}
	};

	// Discover addresses reachable from `entry` by fall-through and direct
	// branches. Stops at indirect jumps, unemittable instructions, and addresses
	// already `claimed` by earlier regions (prevents O(N²) re-emission of
	// shared tails).
	template <int W>
	static std::vector<address_type<W>> aj_discover_region(const uint8_t* seg,
		const AjSegmentMap<W>& map, address_type<W> entry, size_t max_instructions,
		const std::set<address_type<W>>& claimed)
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
			if (claimed.count(pc))
				continue;   // emitted by an earlier region: a region exit
			if (seen.size() >= max_instructions)
				continue;   // capped: anything left over becomes a region exit
			const auto d = aj_decode<W>(seg, pc, map.end);
			if (!map.is_emittable_at(pc, d))
				continue;
			seen.insert(pc);

			switch (d.instr.opcode()) {
			case RV32I_JAL:
				// A linking JAL is a call; following it would inline the callee
				// and fragment it across callers via `claimed`. End the region at
				// calls, keeping each to one function's CFG. Tail calls (rd==0)
				// are still followed.
				if (d.instr.Jtype.rd != 0)
					break;
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
		// Field offsets measured from a live instance, avoiding offsetof() on
		// non-standard-layout types.
		const auto& mem = cpu.machine().memory;
		const auto cpu_addr = uintptr_t(&cpu);

		AjInfo<W> info;
		info.reg_offset    = int32_t(uintptr_t(&cpu.registers().get(0)) - cpu_addr);
		info.fpreg_offset  = int32_t(uintptr_t(&cpu.registers().getfl(0)) - cpu_addr);
		info.arena_ptr     = int32_t(uintptr_t(&mem.memory_arena_ptr_ref()) - cpu_addr);
		info.arena_rdbound = int32_t(uintptr_t(&mem.memory_arena_read_boundary_ref()) - cpu_addr);
		info.arena_wrbound = int32_t(uintptr_t(&mem.memory_arena_write_boundary_ref()) - cpu_addr);
		info.arena_roend   = int32_t(uintptr_t(&mem.initial_rodata_end_ref()) - cpu_addr);
#ifdef RISCV_EXT_VECTOR
		// RVV register file and vl/vtype fields, same measurement method.
		const auto& rvv = cpu.registers().rvv();
		info.rvv_regs = int32_t(uintptr_t(&rvv.get(0))              - cpu_addr);
		info.rvv_vl   = int32_t(uintptr_t(&rvv.vl_ref())            - cpu_addr);
		info.rvv_vsew = int32_t(uintptr_t(&rvv.encoded_sew_ref())   - cpu_addr);
		info.rvv_lmul = int32_t(uintptr_t(&rvv.lmul_shift_ref())    - cpu_addr);
		info.rvv_vill = int32_t(uintptr_t(&rvv.vill_ref())          - cpu_addr);
#endif
		if constexpr (riscv::encompassing_Nbit_arena != 0) {
			// N-bit encompassing arena: mask only, no bounds check.
			info.inline_memory = mem.uses_Nbit_encompassing_arena();
			info.arena_mask = info.inline_memory
				? riscv::encompassing_arena_mask : 0;
		} else {
			// Flat arena: single-sided bounds check per access; disabled with
			// unaligned slow paths.
			info.inline_memory = riscv::flat_readwrite_arena
				&& !riscv::unaligned_memory_slowpaths
				&& mem.uses_flat_memory_arena();
			info.arena_mask = 0;
		}
		info.cb = &aj_callbacks<W>();
		return info;
	}

	// RV32/RV64 only. RV128 and unsupported hosts stay interpreted.
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

		// --- 0. Blocked addresses (breakpoints) -----------------------------------
		// Breakpoints rewrite decoder entries. Regions bypass the cache, so
		// blocked addresses must terminate regions explicitly.
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

		// --- 2. Partition into non-overlapping regions ----------------------------
		// Each unclaimed entry grows a region; leaders inside an existing region
		// become additional entry points rather than starting duplicates.
		struct Region {
			address_t entry;                 // the leader the region was grown from
			std::vector<address_t> instrs;   // ascending; may start below `entry`
			std::vector<address_t> entries;  // ascending; every leader inside it
		};
		std::vector<Region> regions;
		std::set<address_t> claimed_addrs;
		size_t emitted_instrs = 0;
		for (const address_t entry : map.entries)
		{
			if (regions.size() >= options.asmjit_blocks_max)
				break;
			if (emitted_instrs >= options.asmjit_instr_max)
				break;
			if (claimed_addrs.count(entry))
				continue;   // an earlier region emitted it; it becomes an entry of that one
			auto instrs = aj_discover_region<W>(seg, map, entry,
				options.asmjit_region_instr_max, claimed_addrs);
			if (instrs.empty())
				continue;   // nothing emittable at this address
			emitted_instrs += instrs.size();
			claimed_addrs.insert(instrs.begin(), instrs.end());
			regions.push_back({entry, std::move(instrs), {}});
		}
		if (regions.empty())
			return;

		// Collect entry points per region. instrs is sorted, so entries come out
		// sorted too.
		size_t total_entries = 0;
		for (auto& r : regions) {
			for (const address_t pc : r.instrs)
				if (map.entries.count(pc))
					r.entries.push_back(pc);
			total_entries += r.entries.size();
		}

		// --- 3. Emit --------------------------------------------------------------
		const AjInfo<W> info = aj_machine_info<W>(cpu);

		auto ajcode = std::make_shared<AjCode>();
		auto& mappings = exec.create_asmjit_mappings(regions.size());

		unsigned live = 0;
		for (size_t i = 0; i < regions.size(); i++) {
			mappings[i] = aj_emit_region<W>(*ajcode, options, exec, info,
				regions[i].entries, regions[i].instrs);
			if (mappings[i]) live++;
		}
		if (live == 0) {
			exec.create_asmjit_mappings(0);
			return;
		}
		exec.set_asmjit_code(std::move(ajcode));

		// --- 4. Claim decoder entries ---------------------------------------------
		// Synchronous: entries are skipped by the cache generator. Background:
		// live-patches a finished cache copy.
		std::unique_ptr<LivePatchedDecoderCache<W>> patched;
		if (live_patch) {
			// Wait for the decoder cache to be fully built before patching.
			exec.wait_for_decoder_cache_ready();
			patched = std::make_unique<LivePatchedDecoderCache<W>>(exec, total_entries);
		}

		unsigned claimed = 0;
		for (size_t i = 0; i < regions.size(); i++) {
			if (mappings[i] == nullptr)
				continue;
			// All entry points share the region's function; the prologue
			// dispatches on the entry PC stored in AjState.
			for (const address_t addr : regions[i].entries) {
			#ifdef RISCV_BINARY_TRANSLATION
				// Binary translation already owns this entry.
				if (aj_decoder_entry_at(exec.decoder_cache(), addr)
					.get_bytecode() == RV32I_BC_TRANSLATOR)
					continue;
			#endif
				auto& entry = live_patch
					? patched->claim(addr, options.verbose_loader)
					: aj_decoder_entry_at(exec.decoder_cache(), addr);
				entry.set_bytecode(RV32I_BC_ASMJIT);
				entry.set_invalid_handler();
				entry.instr  = unsigned(i);
				entry.idxend = 0;
			#ifdef RISCV_EXT_C
				entry.icount = 0;
			#endif
				claimed++;
			}
		}

		if (live_patch) {
			// Activate the patched cache and migrate running threads.
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
			// Background JIT: options captured by value, segment shared.
			const bool live_patch = options.asmjit_background_callback != nullptr;
			if (!live_patch) {
				aj_translate_segment<W>(*this, options, *shared_segment, false);
				return;
			}

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
				// Callback failed: clear the flag so the segment doesn't block
				// on destruction.
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
