#include "aj_emit.hpp"
#include "aj_runtime.hpp"
#include "../cpu.hpp"
#include "../decoded_exec_segment.hpp"
#include "../memory.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

namespace riscv
{
#if RISCV_ASMJIT_HAS_BACKEND

using namespace asmjit;
using namespace asmjit::ujit;

// --- host escapes ---------------------------------------------------------
// Everything else in this file is emitted through asmjit's universal compiler,
// which serves x86-64 and AArch64 from one source. These two operations have no
// universal spelling, and together they are the entire host-specific surface.

/// @brief A full memory barrier, matching what the interpreter gives FENCE.
static inline void host_fence(BackendCompiler& cc)
{
#if defined(ASMJIT_UJIT_X86)
	cc.mfence();
#else
	cc.dmb(a64::Predicate::DB::kSY);
#endif
}

/// @brief dst = sign_extend_32_to_64(src).
/// @details RV64 defines every *W instruction as a 32-bit operation whose
/// result is sign-extended into the full destination register.
static inline void host_sign_extend_word(BackendCompiler& cc, const Gp& dst, const Gp& src)
{
#if defined(ASMJIT_UJIT_X86)
	cc.movsxd(dst, src);
#else
	cc.sxtw(dst, src);
#endif
}

template <int W>
struct AjEmitter
{
	using address_t = address_type<W>;
	static_assert(W == 4 || W == 8, "The asmjit backend supports RV32 and RV64");
	static constexpr int  RVLEN = sizeof(address_t);
	static constexpr bool RV64  = (W == 8);

	UniCompiler& uc;
	BackendCompiler& cc;  // == *uc.cc, used only by the host escapes above
	const uint8_t* seg;   // PC-relative pointer to the execute segment
	address_t entry;      // the guest address this function is entered at
	const std::vector<address_t>& instrs;   // reachable addresses, ascending
	address_t seg_end;    // execute segment end, for instruction reads
	const AjInfo<W>& info;

	Gp cpu;               // arg0: CPU<W>*
	Gp st;                // arg1: AjState<W>*
	Gp counter;           // live instruction counter, always 64-bit
	Gp zero;              // a constant zero, standing in for x0
	Gp arena;             // base of the flat memory arena, when inlining

	std::array<Gp, 32> vreg {};
	std::bitset<32> writeset;   // static: every rd the region writes
	std::bitset<32> readset;    // static: writeset + every rs1/rs2 it reads
	bool needs_zero = false;
	bool needs_arena = false;

	std::set<address_t> branch_targets;         // in-region targets
	std::unordered_map<address_t, Label> labels;
	uint32_t pending = 0;       // instructions retired since the last counter flush

	// Cold paths (helper calls) are emitted after the region body, so that the
	// hot path stays contiguous. Each one captures the counter state that was
	// live where it branched off, because `pending` has moved on by then.
	std::vector<std::function<void()>> deferred;

	AjEmitter(UniCompiler& u, const uint8_t* s, address_t en,
		const std::vector<address_t>& list, address_t se, const AjInfo<W>& in)
		: uc(u), cc(*u.cc), seg(s), entry(en), instrs(list), seg_end(se), info(in) {}

	// A region is entered at `entry`, but emitted in address order. The two differ
	// whenever a back-edge reaches below the entry, which is the normal shape of a
	// loop entered from its middle.
	bool entry_is_first() const noexcept { return entry == instrs.front(); }
	// A branch may only become an internal jump when its target is an address the
	// region actually emits. Discovery caps region size, so a target inside
	// [begin, end) is not by itself proof of that.
	bool in_region(address_t pc) const noexcept {
		return std::binary_search(instrs.begin(), instrs.end(), pc);
	}
	Label label_at(address_t pc) { return labels.at(pc); }

	// --- width-parametric primitives ---
	// A guest register is a host register of exactly the guest width, so that
	// every operation on it carries RISC-V wrapping and shift-masking semantics
	// for free, on both 32- and 64-bit guests.
	Gp new_ireg(const char* name) {
		if constexpr (RV64) return uc.new_gp64(name); else return uc.new_gp32(name);
	}
	Gp new_ireg(const char* fmt, unsigned i) {
		if constexpr (RV64) return uc.new_gp64(fmt, i); else return uc.new_gp32(fmt, i);
	}
	/// @brief A full-width guest constant: an address, a link target, LUI's result.
	static Imm rvimm(address_t v) {
		if constexpr (RV64) return Imm(uint64_t(v)); else return Imm(uint32_t(v));
	}
	/// @brief A sign-extended constant, which is what every I-type immediate is.
	static Imm sximm(int32_t v) { return Imm(int64_t(v)); }

	Mem reg_mem(unsigned i) const {
		return mem_ptr(cpu, info.reg_offset + int32_t(i * RVLEN));
	}
	static constexpr int32_t off_counter() { return int32_t(offsetof(AjState<W>, counter)); }
	static constexpr int32_t off_max()     { return int32_t(offsetof(AjState<W>, max_counter)); }
	static constexpr int32_t off_pc()      { return int32_t(offsetof(AjState<W>, pc)); }

	// --- register cache ---
	// Registers are preloaded eagerly in the prologue, NOT lazily on first use.
	// A lazy load emitted mid-region would sit after a bound label, so a back-edge
	// jumping to that label would re-execute the load and clobber the loop-carried
	// value with stale memory. Preloading puts every load ahead of every label.
	void emit_prologue_loads() {
		if (needs_zero) {
			zero = new_ireg("zero");
			uc.mov(zero, Imm(0));
		}
		if (needs_arena) {
			arena = uc.new_gp_ptr("arena");
			uc.load(arena, mem_ptr(cpu, info.arena_ptr));
		}
		for (unsigned i = 1; i < 32; i++) {
			if (!readset[i]) continue;
			vreg[i] = new_ireg("x%u", i);
			uc.load(vreg[i], reg_mem(i));
		}
	}
	Gp get(unsigned i) {               // source register
		if (i == 0) return zero;       // never written, so it can be shared
		return vreg[i];                // guaranteed loaded by the prologue
	}
	Gp def(unsigned i) {               // destination register
		if (i == 0) return new_ireg("sink"); // writes to x0 are discarded
		return vreg[i];                // in writeset, so the prologue loaded it
	}

	// --- counter accounting ---
	// `pending` is a compile-time count of instructions retired since the last
	// flush into the `counter` register. It must be zero at every point where
	// control flow can merge, otherwise two predecessors of a label disagree
	// about how much the register still owes. Two flush points guarantee that:
	//   1. immediately before binding a label that is a branch target
	//   2. immediately before emitting a BRANCH, JAL or JALR
	// Both sit on linear control flow, so flushing never diverges between paths.
	void flush_counter() {
		if (pending) { uc.add(counter, counter, Imm(pending)); pending = 0; }
	}

	// --- exits ---
	// Stores the region's whole static write-set, not an incrementally tracked
	// dirty set. An exit emitted early in a loop body would otherwise miss
	// registers written later in the body, which the *previous* iteration did
	// execute. Exits are cold, so the extra stores cost nothing.
	void flush_regs() {
		for (unsigned i = 1; i < 32; i++)
			if (writeset[i]) uc.store(reg_mem(i), vreg[i]);
	}
	void store_counter(uint32_t pend) {
		if (pend == 0) {
			uc.store(mem_ptr(st, off_counter()), counter);
		} else {
			Gp total = uc.new_gp64("total");
			uc.add(total, counter, Imm(pend));
			uc.store(mem_ptr(st, off_counter()), total);
		}
	}
	void store_pc(address_t next_pc) {
		Gp t = new_ireg("nextpc");
		uc.mov(t, rvimm(next_pc));
		uc.store(mem_ptr(st, off_pc()), t);
	}
	// Writes back registers, counter and PC, then returns to dispatch.
	// Must not mutate `counter` or `pending`: an exit is emitted on one side of a
	// branch while the fall-through path continues with the same state.
	void emit_exit(address_t next_pc, uint32_t pend) {
		flush_regs();
		store_counter(pend);
		store_pc(next_pc);
		uc.ret();
	}
	// The JALR form: the next PC is only known at run time.
	void emit_exit_reg(const Gp& next_pc, uint32_t pend) {
		flush_regs();
		store_counter(pend);
		uc.store(mem_ptr(st, off_pc()), next_pc);
		uc.ret();
	}
	// Emitted on the taken path of a backward branch, after flush_counter().
	// Reloads max from memory on every check so that a faulting helper (which
	// zeroes it) breaks the loop rather than spinning to the instruction limit.
	void emit_backedge_check(address_t target) {
		Label ok = uc.new_label();
		uc.j(ok, ucmp_lt(counter, mem_ptr(st, off_max())));
		emit_exit(target, 0);
		uc.bind(ok);
	}

	// --- ALU helpers ---
	// The universal compiler takes three operands and resolves dst/src aliasing
	// itself, so RISC-V's two-address forms map straight onto it. In particular
	// `and rd, rs1, rd` -- a destination that is also the second source -- needs
	// nothing from the emitter.
	enum Op { OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR };

	void alu_rr(Op op, const Gp& d, const Gp& a, const Gp& b) {
		switch (op) {
		case OP_ADD: uc.add (d, a, b); break;
		case OP_SUB: uc.sub (d, a, b); break;
		case OP_AND: uc.and_(d, a, b); break;
		case OP_OR:  uc.or_ (d, a, b); break;
		case OP_XOR: uc.xor_(d, a, b); break;
		}
	}
	void alu_ri(Op op, const Gp& d, const Gp& a, int32_t v) {
		// Identity immediates: ADDI/ORI/XORI with 0 (which is how MV is encoded)
		// and ANDI with -1 are pure moves.
		if ((v == 0 && (op == OP_ADD || op == OP_SUB || op == OP_OR || op == OP_XOR))
			|| (v == -1 && op == OP_AND)) {
			if (d.id() != a.id()) uc.mov(d, a);
			return;
		}
		switch (op) {
		case OP_ADD: uc.add (d, a, sximm(v)); break;
		case OP_SUB: uc.sub (d, a, sximm(v)); break;
		case OP_AND: uc.and_(d, a, sximm(v)); break;
		case OP_OR:  uc.or_ (d, a, sximm(v)); break;
		case OP_XOR: uc.xor_(d, a, sximm(v)); break;
		}
	}
	// 0 = shift left, 1 = shift right logical, 2 = shift right arithmetic.
	// Both hosts mask a register shift count by the operand width minus one,
	// which is exactly the RISC-V rule for the matching XLEN.
	void shift_rr(int kind, const Gp& d, const Gp& a, const Gp& b) {
		switch (kind) {
		case 0: uc.shl(d, a, b); break;
		case 1: uc.shr(d, a, b); break;
		case 2: uc.sar(d, a, b); break;
		}
	}
	void shift_ri(int kind, const Gp& d, const Gp& a, uint32_t shamt) {
		switch (kind) {
		case 0: uc.shl(d, a, Imm(shamt)); break;
		case 1: uc.shr(d, a, Imm(shamt)); break;
		case 2: uc.sar(d, a, Imm(shamt)); break;
		}
	}
	void setcc_rr(bool is_unsigned, unsigned rd, unsigned rs1, unsigned rs2) {
		const Gp a = get(rs1), b = get(rs2);
		uc.select(def(rd), Imm(1), Imm(0),
			is_unsigned ? ucmp_lt(a, b) : scmp_lt(a, b));
	}
	void setcc_ri(bool is_unsigned, unsigned rd, unsigned rs1, int32_t v) {
		const Gp a = get(rs1);
		// SLTIU compares against the *sign-extended* immediate, unsigned.
		uc.select(def(rd), Imm(1), Imm(0),
			is_unsigned ? ucmp_lt(a, sximm(v)) : scmp_lt(a, sximm(v)));
	}

	// --- RV64 word operations ---
	// Every *W instruction computes on the low 32 bits and sign-extends the
	// result, so it is emitted as a 32-bit operation into a scratch register
	// followed by one widening move.
	Gp word_of(unsigned reg) { return get(reg).r32(); }
	void finish_word(unsigned rd, const Gp& tmp32) {
		host_sign_extend_word(cc, def(rd), tmp32);
	}

	// --- memory ---
	// The effective address lives in a host pointer-sized register. On RV32 it is
	// only ever written through its 32-bit half, so the upper half is implicitly
	// zeroed and the register can be used directly as an index into the arena.
	Gp address_of(unsigned rs1, int32_t simm) {
		Gp a = uc.new_gp_ptr("addr");
		if constexpr (RV64) {
			if (rs1 == 0)        uc.mov(a, rvimm(address_t(int64_t(simm))));
			else if (simm == 0)  uc.mov(a, get(rs1));
			else                 uc.add(a, get(rs1), sximm(simm));
		} else {
			if (rs1 == 0) {
				uc.mov(a.r32(), Imm(uint32_t(simm)));
			} else {
				uc.mov(a.r32(), get(rs1));
				if (simm != 0) uc.add(a.r32(), a.r32(), sximm(simm));
			}
		}
		return a;
	}
	/// @brief The guest-width view of an address register, for bounds arithmetic
	/// and for passing the address to a helper.
	static Gp addr_value(const Gp& a) {
		if constexpr (RV64) return a; else return a.r32();
	}

	static FuncSignature load_signature() {
		return FuncSignature::build<address_t, void*, void*, address_t, address_t>();
	}
	static FuncSignature store_signature() {
		return FuncSignature::build<void, void*, void*, address_t, address_t, address_t>();
	}
	// A helper that faults records the exception and zeroes max_counter; leave the
	// region right there rather than running on with a half-executed instruction.
	// `pend` excludes the faulting instruction, which never retired.
	void emit_fault_check(address_t pc, uint32_t pend) {
		Label ok = uc.new_label();
		Gp maxc = uc.new_gp64("maxc");
		uc.load(maxc, mem_ptr(st, off_max()));
		uc.j(ok, test_nz(maxc));
		emit_exit(pc, pend);
		uc.bind(ok);
	}
	void call_load_helper(address_t pc, unsigned funct3, const Gp& dst,
		const Gp& addr, uint32_t pend)
	{
		const auto* cb = info.cb;
		uint64_t fn = 0;
		switch (funct3) {
		case 0x0: fn = uint64_t(uintptr_t(cb->load_i8));  break;
		case 0x1: fn = uint64_t(uintptr_t(cb->load_i16)); break;
		case 0x2: fn = uint64_t(uintptr_t(cb->load_i32)); break;
		case 0x3: fn = uint64_t(uintptr_t(cb->load_i64)); break;  // RV64: LD
		case 0x4: fn = uint64_t(uintptr_t(cb->load_u8));  break;
		case 0x5: fn = uint64_t(uintptr_t(cb->load_u16)); break;
		case 0x6: fn = uint64_t(uintptr_t(cb->load_u32)); break;  // RV64: LWU
		}
		InvokeNode* node;
		cc.invoke(Out(node), fn, load_signature());
		node->set_arg(0, cpu);
		node->set_arg(1, st);
		node->set_arg(2, addr_value(addr));
		node->set_arg(3, rvimm(pc));
		node->set_ret(0, dst);
		emit_fault_check(pc, pend);
	}
	void call_store_helper(address_t pc, unsigned funct3, const Gp& value,
		const Gp& addr, uint32_t pend)
	{
		const auto* cb = info.cb;
		uint64_t fn = 0;
		switch (funct3) {
		case 0x0: fn = uint64_t(uintptr_t(cb->store_8));  break;
		case 0x1: fn = uint64_t(uintptr_t(cb->store_16)); break;
		case 0x2: fn = uint64_t(uintptr_t(cb->store_32)); break;
		case 0x3: fn = uint64_t(uintptr_t(cb->store_64)); break;  // RV64: SD
		}
		InvokeNode* node;
		cc.invoke(Out(node), fn, store_signature());
		node->set_arg(0, cpu);
		node->set_arg(1, st);
		node->set_arg(2, addr_value(addr));
		node->set_arg(3, value);
		node->set_arg(4, rvimm(pc));
		emit_fault_check(pc, pend);
	}
	// (addr - RWREAD_BEGIN) < read_boundary, the same single-sided check the
	// interpreter uses. Both bounds are read from the machine rather than baked
	// in, so the code stays valid for every machine sharing this execute segment.
	void emit_arena_check(const Gp& addr, bool is_write, const Label& slow) {
		const Gp a = addr_value(addr);
		Gp t = new_ireg("bchk");
		if (is_write)
			uc.sub(t, a, mem_ptr(cpu, info.arena_roend));
		else
			uc.sub(t, a, rvimm(address_t(Memory<W>::RWREAD_BEGIN)));
		uc.j(slow, ucmp_ge(t, mem_ptr(cpu, is_write ? info.arena_wrbound : info.arena_rdbound)));
	}
	void emit_load(address_t pc, unsigned funct3, unsigned rd, unsigned rs1, int32_t simm)
	{
		auto addr = address_of(rs1, simm);
		const Gp dst = def(rd);
		if (!info.inline_memory) {
			call_load_helper(pc, funct3, dst, addr, pending - 1);
			return;
		}
		Label slow = uc.new_label(), done = uc.new_label();
		emit_arena_check(addr, false, slow);
		const Mem m = mem_ptr(arena, addr);
		switch (funct3) {
		case 0x0: uc.load_i8 (dst, m); break;                        // LB
		case 0x1: uc.load_i16(dst, m); break;                        // LH
		case 0x2: uc.load_i32(dst, m); break;                        // LW (widens on RV64)
		case 0x3: if constexpr (RV64) uc.load_i64(dst, m); break;    // LD
		case 0x4: uc.load_u8 (dst, m); break;                        // LBU
		case 0x5: uc.load_u16(dst, m); break;                        // LHU
		case 0x6: if constexpr (RV64) uc.load_u32(dst, m); break;    // LWU
		}
		uc.bind(done);
		deferred.push_back([=, this, pend = pending - 1] {
			uc.bind(slow);
			call_load_helper(pc, funct3, dst, addr, pend);
			uc.j(done);
		});
	}
	void emit_store(address_t pc, unsigned funct3, unsigned rs1, unsigned rs2, int32_t simm)
	{
		auto addr = address_of(rs1, simm);
		const Gp src = get(rs2);
		if (!info.inline_memory) {
			call_store_helper(pc, funct3, src, addr, pending - 1);
			return;
		}
		Label slow = uc.new_label(), done = uc.new_label();
		emit_arena_check(addr, true, slow);
		const Mem m = mem_ptr(arena, addr);
		switch (funct3) {
		case 0x0: uc.store_u8 (m, src); break;                        // SB
		case 0x1: uc.store_u16(m, src); break;                        // SH
		case 0x2: uc.store_u32(m, src); break;                        // SW
		case 0x3: if constexpr (RV64) uc.store_u64(m, src); break;    // SD
		}
		uc.bind(done);
		deferred.push_back([=, this, pend = pending - 1] {
			uc.bind(slow);
			call_store_helper(pc, funct3, src, addr, pend);
			uc.j(done);
		});
	}

	// --- pre-pass ---
	// Collects the read set, the write set and the in-region branch targets in a
	// single walk, so the emission loop is free of path-dependent state.
	// The read set must mirror exactly which registers the emitter calls get() on.
	void prepass() {
		for (const address_t pc : instrs) {
			const auto i = aj_decode<W>(seg, pc, seg_end).instr;
			switch (i.opcode()) {
			case RV32I_LUI:
			case RV32I_AUIPC:
				writeset.set(i.Utype.rd);
				break;
			case RV32I_OP_IMM:
				// ADDI with rs1 == x0 is folded into a plain immediate load
				if (!(i.Itype.funct3 == 0x0 && i.Itype.rs1 == 0))
					readset.set(i.Itype.rs1);
				writeset.set(i.Itype.rd);
				break;
			case RV64I_OP_IMM32:
				readset.set(i.Itype.rs1);
				writeset.set(i.Itype.rd);
				break;
			case RV32I_OP:
			case RV64I_OP32:
				readset.set(i.Rtype.rs1);
				readset.set(i.Rtype.rs2);
				writeset.set(i.Rtype.rd);
				break;
			case RV32I_LOAD:
				readset.set(i.Itype.rs1);      // x0 folds into the immediate
				writeset.set(i.Itype.rd);
				needs_arena |= info.inline_memory;
				break;
			case RV32I_STORE:
				readset.set(i.Stype.rs1);      // x0 folds into the immediate
				readset.set(i.Stype.rs2);      // ...but the stored value does not
				needs_arena |= info.inline_memory;
				break;
			case RV32I_JALR:
				readset.set(i.Itype.rs1);
				writeset.set(i.Itype.rd);
				break;
			case RV32I_BRANCH: {
				readset.set(i.Btype.rs1);
				// A comparison against x0 becomes a compare against an immediate
				if (i.Btype.rs2 != 0)
					readset.set(i.Btype.rs2);
				const address_t t = pc + i.Btype.signed_imm();
				if (in_region(t)) branch_targets.insert(t);
				} break;
			case RV32I_JAL: {
				writeset.set(i.Jtype.rd);
				const address_t t = pc + i.Jtype.jump_offset();
				if (in_region(t)) branch_targets.insert(t);
				} break;
			default:
				break;
			}
		}
		// A LOAD with rs1 == x0 reads no register, but the read set is allowed to
		// be conservative: an unused preload costs one instruction in the prologue.
		readset |= writeset;      // written registers must be loaded too, see flush_regs
		needs_zero = readset[0];
		readset.reset(0);
		writeset.reset(0);

		// When the entry is not the lowest address, the prologue has to jump to it,
		// which makes it a label like any other branch target.
		if (!entry_is_first())
			branch_targets.insert(entry);

		for (const address_t t : branch_targets)
			labels.emplace(t, uc.new_label());
	}

	// --- emission ---
	bool failed = false;

	void emit_body()
	{
		// Labels are bound only at addresses actually jumped to from within the
		// region. `begin` needs no label of its own: the prologue falls straight
		// into it, and if it is also a branch target it gets bound here, after the
		// prologue loads, which is exactly right.
		// `fallthrough_pc` is the address linear control flow has arrived at, or 0
		// when the previous instruction did not fall through at all. The prologue
		// falls into the first emitted address only when that is also the entry.
		if (!entry_is_first())
			uc.j(label_at(entry));
		address_t fallthrough_pc = entry_is_first() ? instrs.front() : 0;
		for (const address_t pc : instrs)
		{
			const bool is_target = branch_targets.count(pc) != 0;
			if (is_target) {   // merge point: settle the counter first
				if (pc == fallthrough_pc) flush_counter();
				else pending = 0;   // only reachable through the label
				uc.bind(label_at(pc));
			} else if (pc != fallthrough_pc) {
				// Discovery only produces addresses reachable from the entry, so an
				// address that neither falls through nor carries a label cannot exist.
				failed = true;
				return;
			}

			const auto d = aj_decode<W>(seg, pc, seg_end);
			const auto i = d.instr;
			const address_t next = pc + d.length;
			fallthrough_pc = next;
			pending++;

			switch (i.opcode())
			{
			case RV32I_LUI:
				// The 20-bit upper immediate is sign-extended to the register width.
				uc.mov(def(i.Utype.rd), rvimm(address_t(int64_t(i.Utype.upper_imm()))));
				break;

			case RV32I_AUIPC:
				uc.mov(def(i.Utype.rd), rvimm(address_t(pc + address_t(int64_t(i.Utype.upper_imm())))));
				break;

			case RV32I_FENCE:
				// The interpreter makes every FENCE form a full barrier; match it
				// rather than reason about which guest orderings the host covers.
				host_fence(cc);
				break;

			case RV32I_OP_IMM: {
				const unsigned rd = i.Itype.rd, rs1 = i.Itype.rs1;
				const int32_t simm = i.Itype.signed_imm();
				const uint32_t shamt = RV64 ? i.Itype.shift64_imm() : i.Itype.shift_imm();
				switch (i.Itype.funct3) {
				case 0x0: // ADDI
					if (rs1 == 0) uc.mov(def(rd), rvimm(address_t(int64_t(simm))));
					else          alu_ri(OP_ADD, def(rd), get(rs1), simm);
					break;
				case 0x1: shift_ri(0, def(rd), get(rs1), shamt); break;     // SLLI
				case 0x2: setcc_ri(false, rd, rs1, simm); break;            // SLTI
				case 0x3: setcc_ri(true,  rd, rs1, simm); break;            // SLTIU
				case 0x4: alu_ri(OP_XOR, def(rd), get(rs1), simm); break;   // XORI
				case 0x5: // SRLI / SRAI
					shift_ri((i.Itype.imm & 0x400) ? 2 : 1, def(rd), get(rs1), shamt);
					break;
				case 0x6: alu_ri(OP_OR,  def(rd), get(rs1), simm); break;   // ORI
				case 0x7: alu_ri(OP_AND, def(rd), get(rs1), simm); break;   // ANDI
				}
				} break;

			case RV32I_OP: {
				const unsigned rd = i.Rtype.rd, rs1 = i.Rtype.rs1, rs2 = i.Rtype.rs2;
				const bool alt = (i.Rtype.funct7 == 0b0100000);
				switch (i.Rtype.funct3) {
				case 0x0: // ADD / SUB
					alu_rr(alt ? OP_SUB : OP_ADD, def(rd), get(rs1), get(rs2));
					break;
				case 0x1: shift_rr(0, def(rd), get(rs1), get(rs2)); break;         // SLL
				case 0x2: setcc_rr(false, rd, rs1, rs2); break;                    // SLT
				case 0x3: setcc_rr(true,  rd, rs1, rs2); break;                    // SLTU
				case 0x4: alu_rr(OP_XOR, def(rd), get(rs1), get(rs2)); break;      // XOR
				case 0x5: shift_rr(alt ? 2 : 1, def(rd), get(rs1), get(rs2)); break; // SRL / SRA
				case 0x6: alu_rr(OP_OR,  def(rd), get(rs1), get(rs2)); break;      // OR
				case 0x7: alu_rr(OP_AND, def(rd), get(rs1), get(rs2)); break;      // AND
				}
				} break;

			case RV64I_OP_IMM32: if constexpr (RV64) {
				const unsigned rd = i.Itype.rd, rs1 = i.Itype.rs1;
				Gp t = uc.new_gp32("w");
				switch (i.Itype.funct3) {
				case 0x0: // ADDIW
					alu_ri(OP_ADD, t, word_of(rs1), i.Itype.signed_imm());
					break;
				case 0x1: // SLLIW
					shift_ri(0, t, word_of(rs1), i.Itype.shift_imm());
					break;
				case 0x5: // SRLIW / SRAIW
					shift_ri((i.Itype.imm & 0x400) ? 2 : 1, t, word_of(rs1), i.Itype.shift_imm());
					break;
				}
				finish_word(rd, t);
				} break;

			case RV64I_OP32: if constexpr (RV64) {
				const unsigned rd = i.Rtype.rd, rs1 = i.Rtype.rs1, rs2 = i.Rtype.rs2;
				const bool alt = (i.Rtype.funct7 == 0b0100000);
				Gp t = uc.new_gp32("w");
				switch (i.Rtype.funct3) {
				case 0x0: // ADDW / SUBW
					alu_rr(alt ? OP_SUB : OP_ADD, t, word_of(rs1), word_of(rs2));
					break;
				case 0x1: // SLLW
					shift_rr(0, t, word_of(rs1), word_of(rs2));
					break;
				case 0x5: // SRLW / SRAW
					shift_rr(alt ? 2 : 1, t, word_of(rs1), word_of(rs2));
					break;
				}
				finish_word(rd, t);
				} break;

			case RV32I_LOAD:
				emit_load(pc, i.Itype.funct3, i.Itype.rd, i.Itype.rs1, i.Itype.signed_imm());
				break;

			case RV32I_STORE:
				emit_store(pc, i.Stype.funct3, i.Stype.rs1, i.Stype.rs2, i.Stype.signed_imm());
				break;

			case RV32I_BRANCH: {
				const address_t target = pc + i.Btype.signed_imm();
				flush_counter();                 // settle before control flow splits
				Label notaken = uc.new_label();
				const Gp a = get(i.Btype.rs1);
				const bool vs_zero = (i.Btype.rs2 == 0);
				const Gp b = vs_zero ? a : get(i.Btype.rs2);   // unused when vs_zero
				// Jump *around* the taken path with the inverted condition, so the
				// taken path stays inline.
				switch (i.Btype.funct3) {
				case 0x0: // BEQ
					uc.j(notaken, vs_zero ? cmp_ne(a, Imm(0)) : cmp_ne(a, b)); break;
				case 0x1: // BNE
					uc.j(notaken, vs_zero ? cmp_eq(a, Imm(0)) : cmp_eq(a, b)); break;
				case 0x4: // BLT
					uc.j(notaken, vs_zero ? scmp_ge(a, Imm(0)) : scmp_ge(a, b)); break;
				case 0x5: // BGE
					uc.j(notaken, vs_zero ? scmp_lt(a, Imm(0)) : scmp_lt(a, b)); break;
				case 0x6: // BLTU
					uc.j(notaken, vs_zero ? ucmp_ge(a, Imm(0)) : ucmp_ge(a, b)); break;
				case 0x7: // BGEU
					uc.j(notaken, vs_zero ? ucmp_lt(a, Imm(0)) : ucmp_lt(a, b)); break;
				}
				if (in_region(target)) {
					if (target <= pc) emit_backedge_check(target); // bound the loop
					uc.j(label_at(target));   // vregs stay live across the back-edge
				} else {
					emit_exit(target, 0);
				}
				uc.bind(notaken);
				} break;

			case RV32I_JAL: {
				const address_t target = pc + i.Jtype.jump_offset();
				if (i.Jtype.rd != 0)
					uc.mov(def(i.Jtype.rd), rvimm(next));
				flush_counter();
				if (in_region(target)) {
					if (target <= pc) emit_backedge_check(target);
					uc.j(label_at(target));
				} else {
					emit_exit(target, 0);
				}
				fallthrough_pc = 0;   // nothing falls through an unconditional jump
				} break;

			case RV32I_JALR: {
				// Read rs1 into a temporary before writing rd: they may be the same
				// register, and RISC-V takes the old value of rs1 as the target.
				Gp target = new_ireg("jalr");
				const int32_t simm = i.Itype.signed_imm();
				if (i.Itype.rs1 == 0) {
					uc.mov(target, rvimm(address_t(int64_t(simm)) & ~address_t(1)));
				} else {
					alu_ri(OP_ADD, target, get(i.Itype.rs1), simm);
					uc.and_(target, target, sximm(-2));
				}
				if (i.Itype.rd != 0)
					uc.mov(def(i.Itype.rd), rvimm(next));
				flush_counter();
				emit_exit_reg(target, 0);
				fallthrough_pc = 0;
				} break;

			default:
				// Unreachable: discovery guaranteed emittability.
				emit_exit(pc, pending - 1);
				fallthrough_pc = 0;
				break;
			}

			// The region ends wherever linear control flow leaves the set of
			// addresses discovery reached: past the last instruction, or at a
			// fall-through that the per-region instruction cap cut off.
			if (fallthrough_pc != 0 && !in_region(fallthrough_pc)) {
				emit_exit(fallthrough_pc, pending);
				pending = 0;
				fallthrough_pc = 0;
			}
		}

		// Slow paths last, so that they do not split the hot path. New entries can
		// appear while draining (a helper call emits no further slow paths, but the
		// index-based loop keeps that from mattering either way).
		for (size_t n = 0; n < deferred.size(); n++)
			deferred[n]();
	}
};

template <int W>
aj_block_func<W> aj_emit_region(AjCode& ajcode, const MachineOptions<W>& options,
	const DecodedExecuteSegment<W>& exec, const AjInfo<W>& info,
	address_type<W> entry, const std::vector<address_type<W>>& instrs)
{
	if (instrs.empty())
		return nullptr;

	CodeHolder code;
	if (code.init(ajcode.rt.environment(), ajcode.rt.cpu_features()) != kErrorOk)
		return nullptr;

	StringLogger logger;
	if (options.asmjit_verbose)
		code.set_logger(&logger);

	BackendCompiler bc(&code);
	UniCompiler cc(&bc, ajcode.rt.cpu_features(), ajcode.rt.cpu_hints());

	FuncNode* fn = cc.add_func(FuncSignature::build<void, void*, void*>());
	if (fn == nullptr)
		return nullptr;

	AjEmitter<W> e { cc, exec.exec_data(), entry, instrs, exec.exec_end(), info };
	e.cpu     = cc.new_gp_ptr("cpu");
	e.st      = cc.new_gp_ptr("state");
	e.counter = cc.new_gp64("counter");
	fn->set_arg(0, e.cpu);
	fn->set_arg(1, e.st);

	e.prepass();
	cc.load(e.counter, mem_ptr(e.st, AjEmitter<W>::off_counter()));
	e.emit_prologue_loads();   // every load lands here, ahead of every label
	e.emit_body();
	if (e.failed)
		return nullptr;

	cc.end_func();
	if (cc.finalize() != kErrorOk)
		return nullptr;

	aj_block_func<W> out = nullptr;
	if (ajcode.rt.add(&out, &code) != kErrorOk)
		return nullptr;

	if (options.asmjit_verbose) {
		printf("libriscv: asmjit region 0x%lX-0x%lX entered at 0x%lX (%zu instructions) ->\n%s",
			long(instrs.front()), long(instrs.back()), long(entry), instrs.size(), logger.data());
	}
	return out;
}

#else // !RISCV_ASMJIT_HAS_BACKEND

template <int W>
aj_block_func<W> aj_emit_region(AjCode&, const MachineOptions<W>&,
	const DecodedExecuteSegment<W>&, const AjInfo<W>&,
	address_type<W>, const std::vector<address_type<W>>&)
{
	return nullptr;   // no code generator for this host
}

#endif

#ifdef RISCV_32I
	template aj_block_func<4> aj_emit_region<4>(AjCode&, const MachineOptions<4>&,
		const DecodedExecuteSegment<4>&, const AjInfo<4>&,
		address_type<4>, const std::vector<address_type<4>>&);
#endif
#ifdef RISCV_64I
	template aj_block_func<8> aj_emit_region<8>(AjCode&, const MachineOptions<8>&,
		const DecodedExecuteSegment<8>&, const AjInfo<8>&,
		address_type<8>, const std::vector<address_type<8>>&);
#endif
} // riscv
