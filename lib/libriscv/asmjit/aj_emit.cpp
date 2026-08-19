#include "aj_emit.hpp"
#include "aj_runtime.hpp"
#include "../cpu.hpp"
#include "../decoded_exec_segment.hpp"
#include "../memory.hpp"
#include "../rvfd_util.hpp"

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

/// @brief True when the host's CLZ/CTZ already give RISC-V's answer for zero.
/// @details Zbb defines CLZ(0) and CTZ(0) as XLEN, which is exactly what lzcnt
/// and tzcnt return. Their pre-BMI fallbacks, bsr and bsf, leave the destination
/// undefined instead, so on such a host the zero case has to be selected in.
/// AArch64's clz -- and the rbit+clz pair standing in for ctz -- both return the
/// register width, so nothing is needed there.
static inline bool host_count_zeros_is_exact(const UniCompiler& uc, bool leading)
{
#if defined(ASMJIT_UJIT_X86)
	return leading ? uc.has_lzcnt() : uc.has_bmi();
#else
	(void)uc; (void)leading;
	return true;
#endif
}

/// @brief dst = the number of set bits in `src`, at the width of `dst`.
/// @details x86 has a single instruction for this whenever POPCNT is present,
/// which is every host made since 2008. AArch64 only grew a scalar count in
/// ARMv8.9 (CSSC), and asmjit exposes no universal op for it, so everything else
/// gets the usual SWAR reduction: pairs, then nibbles, then a folding sum whose
/// low byte holds the total.
static inline void host_popcount(UniCompiler& uc, const Gp& dst, const Gp& src)
{
#if defined(ASMJIT_UJIT_X86)
	if (uc.has_popcnt()) {
		uc.cc->popcnt(dst, src);
		return;
	}
#endif
	const uint32_t width = uint32_t(dst.size()) * 8;
	const bool wide = (width == 64);
	const uint64_t m1 = wide ? 0x5555555555555555ull : 0x55555555ull;
	const uint64_t m2 = wide ? 0x3333333333333333ull : 0x33333333ull;
	const uint64_t m4 = wide ? 0x0F0F0F0F0F0F0F0Full : 0x0F0F0F0Full;

	Gp t = uc.new_similar_reg(dst, "pcnt");
	Gp u = uc.new_similar_reg(dst, "pcnt2");
	uc.shr (t, src, Imm(1));
	uc.and_(t, t, Imm(m1));
	uc.sub (t, src, t);            // t: two-bit counts
	uc.shr (u, t, Imm(2));
	uc.and_(u, u, Imm(m2));
	uc.and_(t, t, Imm(m2));
	uc.add (t, t, u);              // t: four-bit counts
	uc.shr (u, t, Imm(4));
	uc.add (t, t, u);
	uc.and_(t, t, Imm(m4));        // t: one count per byte, each at most 8
	// Folding the bytes together can never carry out of the low byte: the total
	// is at most 64. A multiply by 0x01..01 would do the same in one step, but
	// x86 has no 64-bit immediate multiply.
	for (uint32_t s = 8; s < width; s *= 2) {
		uc.shr(u, t, Imm(s));
		uc.add(t, t, u);
	}
	uc.and_(dst, t, Imm(0xFF));
}

/// @brief dst = the high half of the 2*XLEN-bit product of `a` and `b`.
/// @details MULH and MULHU want the part of the product that a three-operand
/// multiply throws away, which no universal op exposes.
static inline void host_mul_hi(UniCompiler& uc, const Gp& dst, const Gp& a, const Gp& b, bool is_signed)
{
	BackendCompiler& cc = *uc.cc;
#if defined(ASMJIT_UJIT_X86)
	// xDX:xAX <- xAX * src. Both halves are virtual registers; the allocator
	// pins them to the physical pair the instruction requires.
	Gp lo = uc.new_similar_reg(dst, "mullo");
	Gp hi = uc.new_similar_reg(dst, "mulhi");
	cc.mov(lo, a);
	if (is_signed) cc.imul(hi, lo, b); else cc.mul(hi, lo, b);
	cc.mov(dst, hi);
#else
	if (dst.size() == 8) {
		if (is_signed) cc.smulh(dst, a, b); else cc.umulh(dst, a, b);
	} else {
		// AArch64 has no 32-bit multiply-high, but it does have a widening
		// 32x32 -> 64 multiply, whose upper half is the same thing.
		Gp t = uc.new_gp64("mulw");
		if (is_signed) cc.smull(t, a, b); else cc.umull(t, a, b);
		uc.shr(t, t, Imm(32));
		uc.mov(dst, t.r32());
	}
#endif
}

/// @brief True when the host can compute a fused multiply-add.
/// @details RISC-V requires FMADD and friends to round once, which a separate
/// multiply and add does not. asmjit will happily emit the two-instruction
/// fallback, so the FMA opcodes are only claimed when the host has the real
/// thing; elsewhere they end the region and the interpreter's std::fma runs.
static inline bool host_has_fma(const UniCompiler& uc)
{
#if defined(ASMJIT_UJIT_X86)
	return uc.has_fma();
#else
	(void)uc;
	return true;   // AArch64 has FMADD/FMSUB in the base ISA
#endif
}

/// @brief dst = a / b, signed, or a % b when `want_rem`.
/// @details The caller has already taken b == 0 and b == -1 off this path:
/// they are exactly the two inputs x86's idiv faults on, and exactly the two
/// RISC-V defines a result for instead.
static inline void host_sdiv(UniCompiler& uc, const Gp& dst, const Gp& a, const Gp& b, bool want_rem)
{
	BackendCompiler& cc = *uc.cc;
#if defined(ASMJIT_UJIT_X86)
	// idiv divides xDX:xAX, so the dividend has to be sign-extended into the
	// upper half first.
	Gp quot = uc.new_similar_reg(dst, "quot");
	Gp rem  = uc.new_similar_reg(dst, "rem");
	cc.mov(quot, a);
	if (quot.size() == 8) cc.cqo(rem, quot); else cc.cdq(rem, quot);
	cc.idiv(rem, quot, b);
	cc.mov(dst, want_rem ? rem : quot);
#else
	if (!want_rem) {
		cc.sdiv(dst, a, b);
	} else {
		Gp quot = uc.new_similar_reg(dst, "quot");
		cc.sdiv(quot, a, b);
		cc.msub(dst, quot, b, a);   // dst = a - quot * b
	}
#endif
}

bool aj_host_has_fma() noexcept
{
	// The JitRuntime derives its features from the host the same way, so this
	// answers the same question the emitter's UniCompiler would.
	static const bool has_fma = [] {
		const CpuFeatures& f = CpuInfo::host().features();
#if defined(ASMJIT_UJIT_X86)
		return f.x86().has_fma();
#else
		(void)f;
		return true;
#endif
	}();
	return has_fma;
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

	// The f-registers get exactly the same treatment as the integer ones: a
	// virtual register each, preloaded in the prologue and stored back at every
	// exit. There is no x0 equivalent here -- f0 is an ordinary register.
	std::array<Vec, 32> fvreg {};
	std::bitset<32> fp_writeset;
	std::bitset<32> fp_readset;
	Vec canon32, canon64;       // the canonical NaNs, when a conversion needs them
	bool needs_canon32 = false;
	bool needs_canon64 = false;

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
	/// @brief An f-register is 64 bits wide on both guest widths: RV32D moves
	/// doubles through a machine whose integer registers are half that size.
	Mem freg_mem(unsigned i) const {
		return mem_ptr(cpu, info.fpreg_offset + int32_t(i * 8));
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
		// The canonical NaNs are loop-invariant, so they are materialized here
		// rather than at each conversion that needs one.
		if (needs_canon32) {
			Gp t = uc.new_gp32("canon32i");
			uc.mov(t, Imm(CANONICAL_NAN_F32));
			canon32 = uc.new_vec128("canon32");
			uc.s_mov_u32(canon32, t);
		}
		if (needs_canon64) {
			Gp t = uc.new_gp64("canon64i");
			uc.mov(t, Imm(CANONICAL_NAN_F64));
			canon64 = uc.new_vec128("canon64");
			uc.s_mov_u64(canon64, t);
		}
		for (unsigned i = 1; i < 32; i++) {
			if (!readset[i]) continue;
			vreg[i] = new_ireg("x%u", i);
			uc.load(vreg[i], reg_mem(i));
		}
		for (unsigned i = 0; i < 32; i++) {
			if (!fp_readset[i]) continue;
			fvreg[i] = uc.new_vec128("f%u", i);
			uc.v_loadu64_u64(fvreg[i], freg_mem(i));
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
	// f-registers need no x0 special case, so source and destination are the
	// same lookup; both names exist to keep the emission code readable.
	Vec fget(unsigned i) { return fvreg[i]; }
	Vec fdef(unsigned i) { return fvreg[i]; }

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
		for (unsigned i = 0; i < 32; i++)
			if (fp_writeset[i]) uc.v_storeu64_u64(freg_mem(i), fvreg[i]);
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

	// --- M extension ---
	// Neither division-by-zero nor signed overflow traps in RISC-V; both have a
	// defined result. `b + 1 <= 1` unsigned holds for exactly b == 0 and
	// b == -1, so a single test moves both off the hot path -- and that pair is
	// also exactly what x86's idiv faults on.
	void emit_div(const Gp& d, const Gp& a, const Gp& b, bool want_rem, bool is_signed)
	{
		Label slow = uc.new_label(), done = uc.new_label();
		if (is_signed) {
			Gp guard = uc.new_similar_reg(d, "divguard");
			uc.add(guard, b, Imm(1));
			uc.j(slow, ucmp_le(guard, Imm(1)));
			host_sdiv(uc, d, a, b, want_rem);
		} else {
			uc.j(slow, cmp_eq(b, Imm(0)));
			if (want_rem) uc.umod(d, a, b); else uc.udiv(d, a, b);
		}
		uc.j(done);

		uc.bind(slow);
		if (is_signed) {
			Label divzero = uc.new_label();
			uc.j(divzero, cmp_eq(b, Imm(0)));
			// b == -1. a / -1 is -a, which is also the defined result of the
			// only overflowing division, MIN / -1 == MIN. a % -1 is zero.
			if (want_rem) uc.mov(d, Imm(0));
			else          uc.neg(d, a);
			uc.j(done);
			uc.bind(divzero);
		}
		// Division by zero: the quotient is all ones, the remainder is the dividend.
		if (want_rem) { if (d.id() != a.id()) uc.mov(d, a); }
		else          uc.mov(d, sximm(-1));
		uc.bind(done);
	}
	/// @brief The eight M-extension forms of OP, and the five of OP-32.
	/// @details `d`, `a` and `b` are already the right width for the caller:
	/// full guest width for OP, 32 bits for the *W forms.
	void emit_muldiv(unsigned funct3, const Gp& d, const Gp& a, const Gp& b)
	{
		switch (funct3) {
		case 0x0: uc.mul(d, a, b); break;                      // MUL / MULW
		case 0x1: host_mul_hi(uc, d, a, b, true);  break;      // MULH
		case 0x2: {                                            // MULHSU
			// Reading `a` as signed subtracts one whole `b` from the high half
			// when `a` is negative: high(a*b) - (a < 0 ? b : 0). The adjustment
			// is computed first because the product may land in `a` or `b`.
			Gp adj = uc.new_similar_reg(d, "hsu");
			uc.sar(adj, a, Imm(d.size() * 8 - 1));   // 0 or ~0
			uc.and_(adj, adj, b);
			host_mul_hi(uc, d, a, b, false);
			uc.sub(d, d, adj);
			} break;
		case 0x3: host_mul_hi(uc, d, a, b, false); break;      // MULHU
		case 0x4: emit_div(d, a, b, false, true);  break;      // DIV  / DIVW
		case 0x5: emit_div(d, a, b, false, false); break;      // DIVU / DIVUW
		case 0x6: emit_div(d, a, b, true,  true);  break;      // REM  / REMW
		case 0x7: emit_div(d, a, b, true,  false); break;      // REMU / REMUW
		}
	}

	// --- RV64 word operations ---
	// Every *W instruction computes on the low 32 bits and sign-extends the
	// result, so it is emitted as a 32-bit operation into a scratch register
	// followed by one widening move.
	Gp word_of(unsigned reg) { return get(reg).r32(); }
	void finish_word(unsigned rd, const Gp& tmp32) {
		host_sign_extend_word(cc, def(rd), tmp32);
	}

	// --- bit manipulation: Zba, Zbb, Zbs -----------------------------------
	// Every encoding reaching emit_zb() was classified by aj_zb_classify(),
	// which region discovery also consults through aj_is_emittable(). Sharing
	// the one classifier is what keeps discovery from claiming an encoding that
	// emission then falls through, which would leave rd holding a stale value
	// rather than faulting.

	static constexpr uint32_t XLEN = uint32_t(RVLEN) * 8;

	/// @brief zext32(rs1) in a full-width register, for the Zba *.UW forms.
	/// @details Writing a register through its 32-bit half zeroes the upper
	/// half on both hosts, which is the same identity address_of() relies on to
	/// use an RV32 address register as an arena index.
	Gp zext32_of(unsigned rs1) {
		Gp z = uc.new_gp64("uw");
		uc.mov(z.r32(), get(rs1).r32());
		return z;
	}

	/// @brief 1 << (amount & (XLEN-1)), the mask BSET, BCLR and BINV apply.
	/// @details Both hosts mask a register shift count by the operand width
	/// minus one, which is exactly the RISC-V rule at the matching XLEN, so the
	/// masking never has to be spelled out.
	Gp bit_mask(const Gp& amount) {
		Gp m = new_ireg("bitm");
		uc.mov(m, Imm(1));
		uc.shl(m, m, amount);
		return m;
	}
	/// @brief The same mask for an immediate bit index. It is materialized
	/// rather than folded into the OR because x86 ALU immediates are 32 bits,
	/// and the RV64 masks run up to bit 63.
	Gp bit_mask(uint32_t shamt) {
		Gp m = new_ireg("bitm");
		const uint64_t v = uint64_t(1) << (shamt & (XLEN - 1));
		uc.mov(m, RV64 ? Imm(v) : Imm(uint32_t(v)));
		return m;
	}

	/// @brief dst = the low `bits` of `src`, sign-extended.
	/// @details Neither host exposes a universal sign-extend-from-width op, and
	/// the shift pair is one instruction more than the x86 movsx it replaces.
	void emit_sign_extend(const Gp& d, const Gp& a, uint32_t bits) {
		const uint32_t up = uint32_t(a.size()) * 8 - bits;
		Gp t = uc.new_similar_reg(d, "sext");
		uc.shl(t, a, Imm(up));
		uc.sar(d, t, Imm(up));
	}

	/// @brief CLZ/CTZ, including RISC-V's XLEN answer for a zero input.
	void emit_count_zeros(const Gp& d, const Gp& a, bool leading)
	{
		if (host_count_zeros_is_exact(uc, leading)) {
			if (leading) uc.clz(d, a); else uc.ctz(d, a);
			return;
		}
		// The host left zero undefined. Everything is computed into scratch
		// registers so that a destination which is also the source stays intact
		// until the comparison has been made.
		const uint32_t width = uint32_t(a.size()) * 8;
		Gp t = uc.new_similar_reg(d, "cnt");
		Gp r = uc.new_similar_reg(d, "cntz");
		if (leading) uc.clz(t, a); else uc.ctz(t, a);
		uc.select(r, Imm(width), t, cmp_eq(a, Imm(0)));
		uc.mov(d, r);
	}

	/// @brief ORC.B: every byte holding any set bit becomes 0xFF.
	void emit_orc_b(const Gp& d, const Gp& a)
	{
		const bool wide = (XLEN == 64);
		const uint64_t low7  = wide ? 0x7F7F7F7F7F7F7F7Full : 0x7F7F7F7Full;
		const uint64_t high1 = wide ? 0x8080808080808080ull : 0x80808080ull;
		Gp t = uc.new_similar_reg(d, "orcb");
		Gp u = uc.new_similar_reg(d, "orcb2");
		Gp k = uc.new_similar_reg(d, "orcbk");
		uc.mov (k, RV64 ? Imm(low7) : Imm(uint32_t(low7)));
		uc.and_(t, a, k);
		uc.add (t, t, k);      // carries into bit 7 iff the low seven bits were set
		uc.or_ (t, t, a);      // bit 7 of each byte is now set iff the byte was
		uc.mov (k, RV64 ? Imm(high1) : Imm(uint32_t(high1)));
		uc.and_(t, t, k);
		// Only bit 7 of each byte survives, so shifting right by seven lands on
		// bit 0 of the same byte and the subtraction cannot borrow across one:
		// 0x80 - 0x01 is 0x7F, and a zero byte stays zero.
		uc.shr (u, t, Imm(7));
		uc.sub (u, t, u);
		uc.or_ (d, t, u);
	}

	/// @brief The Zba, Zbb and Zbs forms of OP, OP-IMM, OP-32 and OP-IMM-32.
	void emit_zb(AjZb zb, rv32i_instruction i)
	{
		// Every shamt-carrying encoding reads it from the same field, at the
		// width the guest allows.
		const uint32_t shamt = RV64 ? i.Itype.shift64_imm() : i.Itype.shift_imm();
		// SH1ADD/SH2ADD/SH3ADD put the shift in funct3 as 2, 4 or 6.
		const uint32_t sh_scale = 1u << (i.Rtype.funct3 >> 1);

		switch (zb)
		{
		// --- Zba: address generation ---
		case AjZb::kShAdd:      // rd = rs2 + (rs1 << n)
			uc.add_ext(def(i.Rtype.rd), get(i.Rtype.rs2), get(i.Rtype.rs1), sh_scale);
			break;
		case AjZb::kShAddUw:    // rd = rs2 + (zext32(rs1) << n)
			// The .UW forms take the low word of rs1 and produce a full 64-bit
			// result, so they are not *W operations and must not sign-extend.
			uc.add_ext(def(i.Rtype.rd), get(i.Rtype.rs2), zext32_of(i.Rtype.rs1), sh_scale);
			break;
		case AjZb::kAddUw:
			uc.add(def(i.Rtype.rd), get(i.Rtype.rs2), zext32_of(i.Rtype.rs1));
			break;
		case AjZb::kSlliUw:
			// A full six-bit shift amount, unlike every other OP-IMM-32 form.
			uc.shl(def(i.Itype.rd), zext32_of(i.Itype.rs1), Imm(i.Itype.shift64_imm()));
			break;

		// --- Zbb: logic with negate ---
		case AjZb::kAndn:
			uc.bic(def(i.Rtype.rd), get(i.Rtype.rs1), get(i.Rtype.rs2));
			break;
		case AjZb::kOrn: {
			Gp t = new_ireg("orn");
			uc.not_(t, get(i.Rtype.rs2));
			uc.or_(def(i.Rtype.rd), get(i.Rtype.rs1), t);
			} break;
		case AjZb::kXnor: {
			Gp t = new_ireg("xnor");
			uc.xor_(t, get(i.Rtype.rs1), get(i.Rtype.rs2));
			uc.not_(def(i.Rtype.rd), t);
			} break;

		// --- Zbb: min and max ---
		case AjZb::kMin:
			uc.smin(def(i.Rtype.rd), get(i.Rtype.rs1), get(i.Rtype.rs2));
			break;
		case AjZb::kMinu:
			uc.umin(def(i.Rtype.rd), get(i.Rtype.rs1), get(i.Rtype.rs2));
			break;
		case AjZb::kMax:
			uc.smax(def(i.Rtype.rd), get(i.Rtype.rs1), get(i.Rtype.rs2));
			break;
		case AjZb::kMaxu:
			uc.umax(def(i.Rtype.rd), get(i.Rtype.rs1), get(i.Rtype.rs2));
			break;

		// --- Zbb: bit counting ---
		case AjZb::kClz:
			emit_count_zeros(def(i.Itype.rd), get(i.Itype.rs1), true);
			break;
		case AjZb::kCtz:
			emit_count_zeros(def(i.Itype.rd), get(i.Itype.rs1), false);
			break;
		case AjZb::kCpop:
			host_popcount(uc, def(i.Itype.rd), get(i.Itype.rs1));
			break;
		case AjZb::kClzw:
		case AjZb::kCtzw:
		case AjZb::kCpopw: {
			// A count is at most 32, so the *W sign-extension is a no-op; going
			// through it anyway keeps every OP-IMM-32 form on one tail.
			Gp t = uc.new_gp32("w");
			if (zb == AjZb::kCpopw)
				host_popcount(uc, t, word_of(i.Itype.rs1));
			else
				emit_count_zeros(t, word_of(i.Itype.rs1), zb == AjZb::kClzw);
			finish_word(i.Itype.rd, t);
			} break;

		// --- Zbb: extend, reverse, combine ---
		case AjZb::kSextB:
			emit_sign_extend(def(i.Itype.rd), get(i.Itype.rs1), 8);
			break;
		case AjZb::kSextH:
			emit_sign_extend(def(i.Itype.rd), get(i.Itype.rs1), 16);
			break;
		case AjZb::kRev8:
			uc.bswap(def(i.Itype.rd), get(i.Itype.rs1));
			break;
		case AjZb::kOrcB:
			emit_orc_b(def(i.Itype.rd), get(i.Itype.rs1));
			break;
		case AjZb::kPack: {
			// rd = the low half of rs1, with the low half of rs2 above it. The
			// shift pair clears each upper half without needing a wide mask.
			// On RV32 with rs2 == x0 this is ZEXT.H.
			constexpr uint32_t half = XLEN / 2;
			Gp lo = new_ireg("packlo"), hi = new_ireg("packhi");
			uc.shl(lo, get(i.Rtype.rs1), Imm(half));
			uc.shr(lo, lo, Imm(half));
			uc.shl(hi, get(i.Rtype.rs2), Imm(half));
			uc.or_(def(i.Rtype.rd), lo, hi);
			} break;
		case AjZb::kPackH: {
			// rd = the low byte of rs1, with the low byte of rs2 above it.
			Gp lo = new_ireg("phlo"), hi = new_ireg("phhi");
			uc.and_(lo, get(i.Rtype.rs1), Imm(0xFF));
			uc.and_(hi, get(i.Rtype.rs2), Imm(0xFF));
			uc.shl(hi, hi, Imm(8));
			uc.or_(def(i.Rtype.rd), lo, hi);
			} break;
		case AjZb::kPackW: if constexpr (RV64) {
			// The RV64 form pairs 16-bit halves into a sign-extended word. Its
			// rs2 == x0 case is ZEXT.H, whose result is positive, so the sign
			// extension leaves it exactly as the zero-extension would.
			Gp t = uc.new_gp32("w"), h = uc.new_gp32("wh");
			uc.and_(t, word_of(i.Rtype.rs1), Imm(0xFFFF));
			uc.shl(h, word_of(i.Rtype.rs2), Imm(16));
			uc.or_(t, t, h);
			finish_word(i.Rtype.rd, t);
			} break;

		// --- Zbb: rotate ---
		case AjZb::kRol:
			uc.rol(def(i.Rtype.rd), get(i.Rtype.rs1), get(i.Rtype.rs2));
			break;
		case AjZb::kRor:
			uc.ror(def(i.Rtype.rd), get(i.Rtype.rs1), get(i.Rtype.rs2));
			break;
		case AjZb::kRori:
			// The rotate amount is masked to the register width, so a zero
			// amount is a rotate by zero rather than by the full width.
			uc.ror(def(i.Itype.rd), get(i.Itype.rs1), Imm(shamt));
			break;
		case AjZb::kRolw:
		case AjZb::kRorw: if constexpr (RV64) {
			Gp t = uc.new_gp32("w");
			if (zb == AjZb::kRolw)
				uc.rol(t, word_of(i.Rtype.rs1), word_of(i.Rtype.rs2));
			else
				uc.ror(t, word_of(i.Rtype.rs1), word_of(i.Rtype.rs2));
			finish_word(i.Rtype.rd, t);
			} break;
		case AjZb::kRoriw: if constexpr (RV64) {
			Gp t = uc.new_gp32("w");
			uc.ror(t, word_of(i.Itype.rs1), Imm(i.Itype.shift_imm()));
			finish_word(i.Itype.rd, t);
			} break;

		// --- Zbs: single-bit ---
		case AjZb::kBset:
			uc.or_(def(i.Rtype.rd), get(i.Rtype.rs1), bit_mask(get(i.Rtype.rs2)));
			break;
		case AjZb::kBclr:
			uc.bic(def(i.Rtype.rd), get(i.Rtype.rs1), bit_mask(get(i.Rtype.rs2)));
			break;
		case AjZb::kBinv:
			uc.xor_(def(i.Rtype.rd), get(i.Rtype.rs1), bit_mask(get(i.Rtype.rs2)));
			break;
		case AjZb::kBext: {
			Gp t = new_ireg("bext");
			uc.shr(t, get(i.Rtype.rs1), get(i.Rtype.rs2));
			uc.and_(def(i.Rtype.rd), t, Imm(1));
			} break;
		case AjZb::kBseti:
			uc.or_(def(i.Itype.rd), get(i.Itype.rs1), bit_mask(shamt));
			break;
		case AjZb::kBclri:
			uc.bic(def(i.Itype.rd), get(i.Itype.rs1), bit_mask(shamt));
			break;
		case AjZb::kBinvi:
			uc.xor_(def(i.Itype.rd), get(i.Itype.rs1), bit_mask(shamt));
			break;
		case AjZb::kBexti: {
			Gp t = new_ireg("bexti");
			uc.shr(t, get(i.Itype.rs1), Imm(shamt));
			uc.and_(def(i.Itype.rd), t, Imm(1));
			} break;

		case AjZb::kNone:
			// Unreachable: the caller only arrives here for a classified
			// encoding, and aj_is_emittable() rejected everything else.
			failed = true;
			break;
		}
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

	// --- F/D extension -----------------------------------------------------
	// f-registers live in vector registers of the host, one guest register per
	// virtual register, and every operation works on the low 32 or 64 bits of
	// one. The upper half of an f-register written by a single-precision
	// operation is architecturally NaN-boxed; libriscv only pays for that under
	// `nanboxing`, and otherwise leaves it as whatever the host op produced,
	// which is the same contract the interpreter documents for `set_float()`.

	/// @brief Fill the upper half of an f-register with ones, so that reading a
	/// single-precision value back as a double yields a NaN, as on hardware.
	void nanbox(const Vec& d) {
		if constexpr (riscv::nanboxing)
			uc.v_or_f64(d, d, uc.simd_const(&uc.ct().p_FFFFFFFF00000000, Bcst::k64, d));
	}
	/// @brief Finish a result of the given precision: single-precision results
	/// are the only ones that leave the upper half of the register undefined.
	void fp_result(const Vec& d, bool is_double) {
		if (!is_double) nanbox(d);
	}

	// Bitwise operations have no precision of their own, but keeping them in
	// the same domain as the surrounding arithmetic avoids the bypass penalty
	// x86 charges for moving a value between the float and double pipelines.
	void v_and_fp(bool dbl, const Vec& d, const Vec& a, const Operand& b) {
		if (dbl) uc.v_and_f64(d, a, b); else uc.v_and_f32(d, a, b);
	}
	void v_andn_fp(bool dbl, const Vec& d, const Vec& a, const Operand& b) {
		if (dbl) uc.v_andn_f64(d, a, b); else uc.v_andn_f32(d, a, b);
	}
	void v_or_fp(bool dbl, const Vec& d, const Vec& a, const Operand& b) {
		if (dbl) uc.v_or_f64(d, a, b); else uc.v_or_f32(d, a, b);
	}
	void v_xor_fp(bool dbl, const Vec& d, const Vec& a, const Operand& b) {
		if (dbl) uc.v_xor_f64(d, a, b); else uc.v_xor_f32(d, a, b);
	}
	/// @brief dst = mask ? if_set : if_clear, where `mask` came out of a scalar
	/// compare and is therefore all-ones or all-zeros in the low lane.
	void select_masked(bool dbl, const Vec& dst, const Vec& mask,
		const Vec& if_set, const Vec& if_clear)
	{
		Vec a = uc.new_vec128("selA");
		Vec b = uc.new_vec128("selB");
		v_andn_fp(dbl, a, mask, if_clear);   // a = ~mask & if_clear
		v_and_fp(dbl, b, mask, if_set);
		v_or_fp(dbl, dst, a, b);
	}

	// FLD and FSD are 64-bit accesses on both guest widths, so the FP helpers
	// carry a uint64_t payload rather than an address_t one.
	static FuncSignature fp_load_signature() {
		return FuncSignature::build<uint64_t, void*, void*, address_t, address_t>();
	}
	static FuncSignature fp_store_signature() {
		return FuncSignature::build<void, void*, void*, address_t, uint64_t, address_t>();
	}
	void call_fp_load_helper(address_t pc, bool is_double, const Vec& dst,
		const Gp& addr, uint32_t pend)
	{
		const uint64_t fn = uint64_t(uintptr_t(
			is_double ? info.cb->load_dbl : info.cb->load_fl));
		Gp bits = uc.new_gp64("fbits");
		InvokeNode* node;
		cc.invoke(Out(node), fn, fp_load_signature());
		node->set_arg(0, cpu);
		node->set_arg(1, st);
		node->set_arg(2, addr_value(addr));
		node->set_arg(3, rvimm(pc));
		node->set_ret(0, bits);
		uc.s_mov_u64(dst, bits);
		emit_fault_check(pc, pend);
	}
	void call_fp_store_helper(address_t pc, bool is_double, const Vec& value,
		const Gp& addr, uint32_t pend)
	{
		const uint64_t fn = uint64_t(uintptr_t(
			is_double ? info.cb->store_dbl : info.cb->store_fl));
		Gp bits = uc.new_gp64("fbits");
		uc.s_mov_u64(bits, value);   // the helper narrows a single-precision store
		InvokeNode* node;
		cc.invoke(Out(node), fn, fp_store_signature());
		node->set_arg(0, cpu);
		node->set_arg(1, st);
		node->set_arg(2, addr_value(addr));
		node->set_arg(3, bits);
		node->set_arg(4, rvimm(pc));
		emit_fault_check(pc, pend);
	}
	void emit_fp_load(address_t pc, unsigned funct3, unsigned rd, unsigned rs1, int32_t simm)
	{
		const bool is_double = (funct3 == 0x3);
		auto addr = address_of(rs1, simm);
		const Vec dst = fdef(rd);
		if (!info.inline_memory) {
			call_fp_load_helper(pc, is_double, dst, addr, pending - 1);
			fp_result(dst, is_double);
			return;
		}
		Label slow = uc.new_label(), done = uc.new_label();
		emit_arena_check(addr, false, slow);
		const Mem m = mem_ptr(arena, addr);
		if (is_double) uc.v_loadu64_u64(dst, m);   // FLD
		else           uc.v_loadu32_u32(dst, m);   // FLW
		uc.bind(done);
		deferred.push_back([=, this, pend = pending - 1] {
			uc.bind(slow);
			call_fp_load_helper(pc, is_double, dst, addr, pend);
			uc.j(done);
		});
		// Emitted after the merge point, so it covers both paths.
		fp_result(dst, is_double);
	}
	void emit_fp_store(address_t pc, unsigned funct3, unsigned rs1, unsigned rs2, int32_t simm)
	{
		const bool is_double = (funct3 == 0x3);
		auto addr = address_of(rs1, simm);
		const Vec src = fget(rs2);
		if (!info.inline_memory) {
			call_fp_store_helper(pc, is_double, src, addr, pending - 1);
			return;
		}
		Label slow = uc.new_label(), done = uc.new_label();
		emit_arena_check(addr, true, slow);
		const Mem m = mem_ptr(arena, addr);
		if (is_double) uc.v_storeu64_u64(m, src);  // FSD
		else           uc.v_storeu32_u32(m, src);  // FSW
		uc.bind(done);
		deferred.push_back([=, this, pend = pending - 1] {
			uc.bind(slow);
			call_fp_store_helper(pc, is_double, src, addr, pend);
			uc.j(done);
		});
	}

	/// @brief Move a 32-bit result into a guest register, widening it on RV64.
	/// @details Every FP instruction that writes an integer register produces
	/// either a sign-extended word (FMV.X.W) or a small positive number
	/// (FEQ/FLT/FLE, FCLASS), so one sign-extending path serves all of them.
	void set_word_result(unsigned rd, const Gp& t32) {
		if constexpr (RV64) host_sign_extend_word(cc, def(rd), t32);
		else                uc.mov(def(rd), t32);
	}

	/// @brief The four fused multiply-add opcodes.
	/// @details asmjit has all four shapes built in, but its negated forms
	/// negate a different operand than the interpreter's std::fma() calls do.
	/// The arithmetic is identical either way; the NaN that comes out is not,
	/// because a hardware FMA propagates a NaN operand unchanged and so records
	/// which operand was negated on the way in. Spelling every form as an
	/// explicit sign flip plus a plain multiply-add costs one xor and keeps the
	/// two backends bit-identical.
	void emit_fmadd(rv32i_instruction i)
	{
		const rv32f_instruction fi { i };
		const bool dbl = (fi.R4type.funct2 == 0x1);
		const Vec a = fget(fi.R4type.rs1), b = fget(fi.R4type.rs2);
		const Vec c = fget(fi.R4type.rs3), d = fdef(fi.R4type.rd);

		const auto negated = [&] (const Vec& src) {
			Vec t = uc.new_vec128("fneg");
			if (dbl) uc.s_neg_f64(t, src); else uc.s_neg_f32(t, src);
			return t;
		};
		const auto madd = [&] (const Vec& dst, const Vec& x, const Vec& y, const Vec& z) {
			if (dbl) uc.s_madd_f64(dst, x, y, z); else uc.s_madd_f32(dst, x, y, z);
		};
		switch (i.opcode()) {
		case RV32F_FMADD:                        // fma(rs1, rs2, rs3)
			madd(d, a, b, c);
			break;
		case RV32F_FMSUB:                        // fma(rs1, rs2, -rs3)
			madd(d, a, b, negated(c));
			break;
		case RV32F_FNMSUB:                       // fma(-rs1, rs2, rs3)
			madd(d, negated(a), b, c);
			break;
		case RV32F_FNMADD: {                     // -fma(rs1, rs2, rs3)
			Vec t = uc.new_vec128("fnmadd");
			madd(t, a, b, c);
			if (dbl) uc.s_neg_f64(d, t); else uc.s_neg_f32(d, t);
			} break;
		}
		fp_result(d, dbl);
	}

	/// @brief FMIN/FMAX, which no host instruction implements: RISC-V orders
	/// -0.0 below +0.0 and canonicalizes a pair of NaN operands.
	void emit_fminmax(const rv32f_instruction& fi, bool dbl)
	{
		const bool is_max = (fi.R4type.funct3 == 0x1);
		const void* fn = dbl
			? (is_max ? (const void*)info.cb->fmax64 : (const void*)info.cb->fmin64)
			: (is_max ? (const void*)info.cb->fmax32 : (const void*)info.cb->fmin32);
		InvokeNode* node;
		cc.invoke(Out(node), uint64_t(uintptr_t(fn)),
			dbl ? FuncSignature::build<double, double, double>()
				: FuncSignature::build<float, float, float>());
		node->set_arg(0, fget(fi.R4type.rs1));
		node->set_arg(1, fget(fi.R4type.rs2));
		node->set_ret(0, fdef(fi.R4type.rd));
		fp_result(fdef(fi.R4type.rd), dbl);
	}

	/// @brief FSGNJ, FSGNJN and FSGNJX: rs1's magnitude with a sign derived
	/// from rs2. This is also how a compiler spells FMV, FNEG and FABS.
	void emit_fsgnj(const rv32f_instruction& fi, bool dbl)
	{
		const Vec a = fget(fi.R4type.rs1), b = fget(fi.R4type.rs2);
		const Vec d = fdef(fi.R4type.rd);
		const Operand sign = dbl
			? uc.simd_const(&uc.ct().p_8000000000000000, Bcst::k64, d)
			: uc.simd_const(&uc.ct().p_8000000080000000, Bcst::k32, d);
		const Operand magnitude = dbl
			? uc.simd_const(&uc.ct().p_7FFFFFFFFFFFFFFF, Bcst::k64, d)
			: uc.simd_const(&uc.ct().p_7FFFFFFF7FFFFFFF, Bcst::k32, d);

		Vec sbit = uc.new_vec128("fsgnj");
		switch (fi.R4type.funct3) {
		case 0x0: // FSGNJ: rs2's sign
			v_and_fp(dbl, sbit, b, sign);
			break;
		case 0x1: // FSGNJN: rs2's sign, inverted
			v_andn_fp(dbl, sbit, b, sign);
			break;
		case 0x2: // FSGNJX: the two signs, xored
			v_xor_fp(dbl, sbit, a, b);
			v_and_fp(dbl, sbit, sbit, sign);
			break;
		}
		Vec mag = uc.new_vec128("fsgnjm");
		v_and_fp(dbl, mag, a, magnitude);
		v_or_fp(dbl, d, mag, sbit);
		fp_result(d, dbl);
	}

	/// @brief FEQ, FLT and FLE, whose result is an integer 0 or 1.
	/// @details All three are ordered comparisons: a NaN operand makes them
	/// false, which is exactly what the host's scalar compare already answers.
	void emit_fcmp(const rv32f_instruction& fi, bool dbl)
	{
		const Vec a = fget(fi.R4type.rs1), b = fget(fi.R4type.rs2);
		Vec m = uc.new_vec128("fcmp");
		switch (fi.R4type.funct3) {
		case 0x0: if (dbl) uc.s_cmp_le_f64(m, a, b); else uc.s_cmp_le_f32(m, a, b); break;
		case 0x1: if (dbl) uc.s_cmp_lt_f64(m, a, b); else uc.s_cmp_lt_f32(m, a, b); break;
		case 0x2: if (dbl) uc.s_cmp_eq_f64(m, a, b); else uc.s_cmp_eq_f32(m, a, b); break;
		}
		// The low lane is all ones or all zeros; one bit of it is the result.
		Gp t = uc.new_gp32("fcmpr");
		uc.s_extract_u32(t, m, 0);
		uc.and_(t, t, Imm(1));
		set_word_result(fi.R4type.rd, t);
	}

	/// @brief FCVT.S.D and FCVT.D.S.
	/// @details A NaN operand must come out as the destination format's
	/// canonical NaN, which the host conversion does not do -- it carries the
	/// payload across. Selecting the canonical NaN of the *source* format
	/// before converting gets there in one fewer domain: the two canonical NaNs
	/// convert into each other exactly.
	void emit_fcvt_sd_ds(const rv32f_instruction& fi, bool to_double)
	{
		const Vec a = fget(fi.R4type.rs1);
		const Vec d = fdef(fi.R4type.rd);
		Vec m = uc.new_vec128("fnan");
		Vec t = uc.new_vec128("fcvt");
		if (to_double) {                      // FCVT.D.S
			uc.s_cmp_unord_f32(m, a, a);
			select_masked(false, t, m, canon32, a);
			uc.s_cvt_f32_to_f64(d, t);
		} else {                              // FCVT.S.D
			uc.s_cmp_unord_f64(m, a, a);
			select_masked(true, t, m, canon64, a);
			uc.s_cvt_f64_to_f32(d, t);
			nanbox(d);
		}
	}

	/// @brief FCVT.{W,WU,L,LU}.{S,D}, all of them helper calls.
	/// @details Every host conversion instruction disagrees with RISC-V about
	/// NaN and about overflow -- x86 answers the integer indefinite for both,
	/// where RISC-V clips to the nearest representable extreme -- and the
	/// rounding mode is part of the instruction rather than of the register
	/// file, so there is little of the sequence left to inline.
	void emit_fcvt_to_int(const rv32f_instruction& fi, bool dbl)
	{
		const auto* cb = info.cb;
		const void* fn = nullptr;
		switch (fi.R4type.rs2) {
		case 0x0: fn = dbl ? (const void*)cb->fcvt_w_d  : (const void*)cb->fcvt_w_s;  break;
		case 0x1: fn = dbl ? (const void*)cb->fcvt_wu_d : (const void*)cb->fcvt_wu_s; break;
		case 0x2: fn = dbl ? (const void*)cb->fcvt_l_d  : (const void*)cb->fcvt_l_s;  break;
		case 0x3: fn = dbl ? (const void*)cb->fcvt_lu_d : (const void*)cb->fcvt_lu_s; break;
		}
		Gp rm = uc.new_gp32("frm");
		uc.mov(rm, Imm(fi.R4type.funct3));
		InvokeNode* node;
		cc.invoke(Out(node), uint64_t(uintptr_t(fn)),
			dbl ? FuncSignature::build<address_t, void*, double, uint32_t>()
				: FuncSignature::build<address_t, void*, float, uint32_t>());
		node->set_arg(0, cpu);
		node->set_arg(1, fget(fi.R4type.rs1));
		node->set_arg(2, rm);
		node->set_ret(0, def(fi.R4type.rd));
	}

	/// @brief FCVT.{S,D}.{W,WU,L,LU}.
	/// @details Without FCSR emulation these carry no inexactness reporting, so
	/// the signed forms are a single host instruction. Only a 64-bit unsigned
	/// source needs a helper: no host has an instruction for it.
	void emit_fcvt_from_int(const rv32f_instruction& fi, bool dbl)
	{
		const Vec d = fdef(fi.R4type.rd);
		const Gp src = get(fi.R4type.rs1);
		switch (fi.R4type.rs2) {
		case 0x0: // FCVT.{S,D}.W
			if (dbl) uc.s_cvt_int_to_f64(d, src.r32());
			else     uc.s_cvt_int_to_f32(d, src.r32());
			break;
		case 0x1: { // FCVT.{S,D}.WU -- widening to a signed 64-bit source is exact
			Gp z = uc.new_gp64("fzx");
			uc.mov(z.r32(), src.r32());   // writing the low half clears the upper
			if (dbl) uc.s_cvt_int_to_f64(d, z);
			else     uc.s_cvt_int_to_f32(d, z);
			} break;
		case 0x2: // FCVT.{S,D}.L
			if (dbl) uc.s_cvt_int_to_f64(d, src);
			else     uc.s_cvt_int_to_f32(d, src);
			break;
		case 0x3: { // FCVT.{S,D}.LU
			const void* fn = dbl ? (const void*)info.cb->fcvt_d_lu
								 : (const void*)info.cb->fcvt_s_lu;
			InvokeNode* node;
			cc.invoke(Out(node), uint64_t(uintptr_t(fn)),
				dbl ? FuncSignature::build<double, uint64_t>()
					: FuncSignature::build<float, uint64_t>());
			node->set_arg(0, src);
			node->set_ret(0, d);
			} break;
		}
		fp_result(d, dbl);
	}

	/// @brief The OP-FP opcode, which carries every FP instruction that is not
	/// a load, a store or a fused multiply-add.
	void emit_fpfunc(rv32i_instruction i)
	{
		const rv32f_instruction fi { i };
		const unsigned rd = fi.R4type.rd, rs1 = fi.R4type.rs1, rs2 = fi.R4type.rs2;
		const bool dbl = (fi.R4type.funct2 == 0x1);

		switch (i.fpfunc())
		{
		case RV32F__FADD:
			if (dbl) uc.s_add_f64(fdef(rd), fget(rs1), fget(rs2));
			else     uc.s_add_f32(fdef(rd), fget(rs1), fget(rs2));
			fp_result(fdef(rd), dbl);
			break;
		case RV32F__FSUB:
			if (dbl) uc.s_sub_f64(fdef(rd), fget(rs1), fget(rs2));
			else     uc.s_sub_f32(fdef(rd), fget(rs1), fget(rs2));
			fp_result(fdef(rd), dbl);
			break;
		case RV32F__FMUL:
			if (dbl) uc.s_mul_f64(fdef(rd), fget(rs1), fget(rs2));
			else     uc.s_mul_f32(fdef(rd), fget(rs1), fget(rs2));
			fp_result(fdef(rd), dbl);
			break;
		case RV32F__FDIV:
			if (dbl) uc.s_div_f64(fdef(rd), fget(rs1), fget(rs2));
			else     uc.s_div_f32(fdef(rd), fget(rs1), fget(rs2));
			fp_result(fdef(rd), dbl);
			break;
		case RV32F__FSQRT:
			if (dbl) uc.s_sqrt_f64(fdef(rd), fget(rs1));
			else     uc.s_sqrt_f32(fdef(rd), fget(rs1));
			fp_result(fdef(rd), dbl);
			break;
		case RV32F__FSGNJ_NX:
			emit_fsgnj(fi, dbl);
			break;
		case RV32F__FMIN_MAX:
			emit_fminmax(fi, dbl);
			break;
		case RV32F__FEQ_LT_LE:
			emit_fcmp(fi, dbl);
			break;
		case RV32F__FCVT_SD_DS:
			// funct2 names the *destination* format here, not the source.
			emit_fcvt_sd_ds(fi, dbl);
			break;
		case RV32F__FCVT_W_SD:
			emit_fcvt_to_int(fi, dbl);
			break;
		case RV32F__FCVT_SD_W:
			emit_fcvt_from_int(fi, dbl);
			break;
		case RV32F__FMV_X_W:
			if (fi.R4type.funct3 == 0x0) {
				if (dbl) {                       // FMV.X.D, RV64 only
					if constexpr (RV64) uc.s_mov_u64(def(rd), fget(rs1));
				} else {                         // FMV.X.W
					Gp t = uc.new_gp32("fmvx");
					uc.s_extract_u32(t, fget(rs1), 0);
					set_word_result(rd, t);
				}
			} else {                             // FCLASS
				Gp bits = dbl ? uc.new_gp64("fcls") : uc.new_gp32("fcls");
				if (dbl) uc.s_mov_u64(bits, fget(rs1));
				else     uc.s_extract_u32(bits, fget(rs1), 0);
				InvokeNode* node;
				cc.invoke(Out(node), uint64_t(uintptr_t(dbl
						? (const void*)info.cb->fclass64
						: (const void*)info.cb->fclass32)),
					dbl ? FuncSignature::build<address_t, uint64_t>()
						: FuncSignature::build<address_t, uint32_t>());
				node->set_arg(0, bits);
				node->set_ret(0, def(rd));
			}
			break;
		case RV32F__FMV_W_X:
			if (dbl) {                           // FMV.D.X, RV64 only
				if constexpr (RV64) uc.s_mov_u64(fdef(rd), get(rs1));
			} else {                             // FMV.W.X
				uc.s_mov_u32(fdef(rd), get(rs1).r32());
				nanbox(fdef(rd));
			}
			break;
		}
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

			case RV32F_LOAD:               // FLW, FLD
				readset.set(i.Itype.rs1);
				fp_writeset.set(i.Itype.rd);
				needs_arena |= info.inline_memory;
				break;
			case RV32F_STORE:              // FSW, FSD
				readset.set(i.Stype.rs1);
				fp_readset.set(i.Stype.rs2);
				needs_arena |= info.inline_memory;
				break;
			case RV32F_FMADD:
			case RV32F_FMSUB:
			case RV32F_FNMADD:
			case RV32F_FNMSUB: {
				const rv32f_instruction fi { i };
				fp_readset.set(fi.R4type.rs1);
				fp_readset.set(fi.R4type.rs2);
				fp_readset.set(fi.R4type.rs3);
				fp_writeset.set(fi.R4type.rd);
				} break;
			case RV32F_FPFUNC: {
				const rv32f_instruction fi { i };
				const unsigned rd = fi.R4type.rd, rs1 = fi.R4type.rs1, rs2 = fi.R4type.rs2;
				const bool dbl = (fi.R4type.funct2 == 0x1);
				switch (i.fpfunc()) {
				case RV32F__FADD: case RV32F__FSUB:
				case RV32F__FMUL: case RV32F__FDIV:
				case RV32F__FSGNJ_NX: case RV32F__FMIN_MAX:
					fp_readset.set(rs1); fp_readset.set(rs2); fp_writeset.set(rd);
					break;
				case RV32F__FSQRT:
					fp_readset.set(rs1); fp_writeset.set(rd);
					break;
				case RV32F__FCVT_SD_DS:
					fp_readset.set(rs1); fp_writeset.set(rd);
					// The canonical NaN is selected in the *source* format.
					if (dbl) needs_canon32 = true; else needs_canon64 = true;
					break;
				case RV32F__FEQ_LT_LE:
					fp_readset.set(rs1); fp_readset.set(rs2); writeset.set(rd);
					break;
				case RV32F__FCVT_W_SD:
					fp_readset.set(rs1); writeset.set(rd);
					break;
				case RV32F__FCVT_SD_W:
					readset.set(rs1); fp_writeset.set(rd);
					break;
				case RV32F__FMV_X_W:   // and FCLASS
					fp_readset.set(rs1); writeset.set(rd);
					break;
				case RV32F__FMV_W_X:
					readset.set(rs1); fp_writeset.set(rd);
					break;
				default:
					break;
				}
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
		// f0 is an ordinary register, so the f-register sets keep every bit.
		fp_readset |= fp_writeset;

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
				// Zb* first: BSETI and friends share funct3 with SLLI, and RORI
				// and friends share it with SRLI/SRAI.
				if (const AjZb zb = aj_zb_classify<W>(i); zb != AjZb::kNone) {
					emit_zb(zb, i);
					break;
				}
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
				// Zb* first: ANDN, ORN and XNOR share funct7 0x20 with SUB and SRA.
				if (const AjZb zb = aj_zb_classify<W>(i); zb != AjZb::kNone) {
					emit_zb(zb, i);
					break;
				}
				const unsigned rd = i.Rtype.rd, rs1 = i.Rtype.rs1, rs2 = i.Rtype.rs2;
				const bool alt = (i.Rtype.funct7 == 0b0100000);
				if (i.Rtype.funct7 == 0b0000001) {   // RV32M / RV64M
					emit_muldiv(i.Rtype.funct3, def(rd), get(rs1), get(rs2));
					break;
				}
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
				// Zb* first: SLLI.UW shares funct3 with SLLIW, and RORIW with SRLIW.
				if (const AjZb zb = aj_zb_classify<W>(i); zb != AjZb::kNone) {
					emit_zb(zb, i);
					break;
				}
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
				// Zb* first, and before word_of(): the .UW forms and ZEXT.H
				// produce full 64-bit results rather than sign-extended words.
				if (const AjZb zb = aj_zb_classify<W>(i); zb != AjZb::kNone) {
					emit_zb(zb, i);
					break;
				}
				const unsigned rd = i.Rtype.rd, rs1 = i.Rtype.rs1, rs2 = i.Rtype.rs2;
				const bool alt = (i.Rtype.funct7 == 0b0100000);
				Gp t = uc.new_gp32("w");
				if (i.Rtype.funct7 == 0b0000001) {   // MULW DIVW DIVUW REMW REMUW
					emit_muldiv(i.Rtype.funct3, t, word_of(rs1), word_of(rs2));
					finish_word(rd, t);
					break;
				}
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

			case RV32F_LOAD:
				emit_fp_load(pc, i.Itype.funct3, i.Itype.rd, i.Itype.rs1, i.Itype.signed_imm());
				break;

			case RV32F_STORE:
				emit_fp_store(pc, i.Stype.funct3, i.Stype.rs1, i.Stype.rs2, i.Stype.signed_imm());
				break;

			case RV32F_FMADD:
			case RV32F_FMSUB:
			case RV32F_FNMADD:
			case RV32F_FNMSUB:
				emit_fmadd(i);
				break;

			case RV32F_FPFUNC:
				emit_fpfunc(i);
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

bool aj_host_has_fma() noexcept
{
	return false;   // nothing is emitted on this host anyway
}

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
