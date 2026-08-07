#include "aj_emit.hpp"
#include "aj_runtime.hpp"
#include "../cpu.hpp"
#include "../decoded_exec_segment.hpp"
#include "../memory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <bitset>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

namespace riscv
{
#if RISCV_ASMJIT_HAS_BACKEND

using namespace asmjit;

template <int W>
struct AjEmitter
{
	using address_t = address_type<W>;
	static_assert(W == 4, "The asmjit backend is RV32-only in v1");
	static constexpr int RVLEN = sizeof(address_t);

	x86::Compiler& cc;
	const uint8_t* seg;   // PC-relative pointer to the execute segment
	address_t entry;      // the guest address this function is entered at
	const std::vector<address_t>& instrs;   // reachable addresses, ascending
	address_t seg_end;    // execute segment end, for instruction reads
	const AjInfo<W>& info;

	x86::Gp cpu;          // arg0: CPU<W>*
	x86::Gp st;           // arg1: AjState<W>*
	x86::Gp counter;      // live instruction counter
	x86::Gp zero;         // a constant zero, standing in for x0
	x86::Gp arena;        // base of the flat memory arena, when inlining

	std::array<x86::Gp, 32> vreg {};
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

	AjEmitter(x86::Compiler& c, const uint8_t* s, address_t en,
		const std::vector<address_t>& list, address_t se, const AjInfo<W>& in)
		: cc(c), seg(s), entry(en), instrs(list), seg_end(se), info(in) {}

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

	x86::Mem reg_mem(unsigned i) const {
		return x86::ptr(cpu, info.reg_offset + int32_t(i * RVLEN), RVLEN);
	}

	// --- register cache ---
	// Registers are preloaded eagerly in the prologue, NOT lazily on first use.
	// A lazy load emitted mid-region would sit after a bound label, so a back-edge
	// jumping to that label would re-execute the load and clobber the loop-carried
	// value with stale memory. Preloading puts every load ahead of every label.
	void emit_prologue_loads() {
		if (needs_zero) {
			zero = cc.new_gp32("zero");
			cc.xor_(zero, zero);
		}
		if (needs_arena) {
			arena = cc.new_gpz("arena");
			cc.mov(arena, x86::qword_ptr(cpu, info.arena_ptr));
		}
		for (unsigned i = 1; i < 32; i++) {
			if (!readset[i]) continue;
			vreg[i] = cc.new_gp32("x%u", i);
			cc.mov(vreg[i], reg_mem(i));
		}
	}
	x86::Gp get(unsigned i) {          // source register
		if (i == 0) return zero;       // never written, so it can be shared
		return vreg[i];                // guaranteed loaded by the prologue
	}
	x86::Gp def(unsigned i) {          // destination register
		if (i == 0) return cc.new_gp32("sink"); // writes to x0 are discarded
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
		if (pending) { cc.add(counter, imm(pending)); pending = 0; }
	}

	// --- exits ---
	// Stores the region's whole static write-set, not an incrementally tracked
	// dirty set. An exit emitted early in a loop body would otherwise miss
	// registers written later in the body, which the *previous* iteration did
	// execute. Exits are cold, so the extra stores cost nothing.
	void flush_regs() {
		for (unsigned i = 1; i < 32; i++)
			if (writeset[i]) cc.mov(reg_mem(i), vreg[i]);
	}
	void store_counter(uint32_t pend) {
		if (pend == 0) {
			cc.mov(x86::qword_ptr(st, offsetof(AjState<W>, counter)), counter);
		} else {
			auto total = cc.new_gp64("total");
			cc.lea(total, x86::ptr(counter, int32_t(pend)));
			cc.mov(x86::qword_ptr(st, offsetof(AjState<W>, counter)), total);
		}
	}
	// Writes back registers, counter and PC, then returns to dispatch.
	// Must not mutate `counter` or `pending`: an exit is emitted on one side of a
	// branch while the fall-through path continues with the same state.
	void emit_exit(address_t next_pc, uint32_t pend) {
		flush_regs();
		store_counter(pend);
		cc.mov(x86::dword_ptr(st, offsetof(AjState<W>, pc)), imm(uint32_t(next_pc)));
		cc.ret();
	}
	// The JALR form: the next PC is only known at run time.
	void emit_exit_reg(const x86::Gp& next_pc, uint32_t pend) {
		flush_regs();
		store_counter(pend);
		cc.mov(x86::dword_ptr(st, offsetof(AjState<W>, pc)), next_pc);
		cc.ret();
	}
	// Emitted on the taken path of a backward branch, after flush_counter().
	// Reloads max from memory on every check so that a faulting helper (which
	// zeroes it) breaks the loop rather than spinning to the instruction limit.
	void emit_backedge_check(address_t target) {
		Label ok = cc.new_label();
		cc.cmp(counter, x86::qword_ptr(st, offsetof(AjState<W>, max_counter)));
		cc.jb(ok);
		emit_exit(target, 0);
		cc.bind(ok);
	}

	// --- ALU helpers ---
	enum Op { OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR };

	void alu_rr(Op op, const x86::Gp& d, const x86::Gp& s) {
		switch (op) {
		case OP_ADD: cc.add (d, s); break;
		case OP_SUB: cc.sub (d, s); break;
		case OP_AND: cc.and_(d, s); break;
		case OP_OR:  cc.or_ (d, s); break;
		case OP_XOR: cc.xor_(d, s); break;
		}
	}
	void alu_ri(Op op, const x86::Gp& d, int32_t v) {
		// Identity immediates: ADDI/ORI/XORI with 0 (which is how MV is encoded)
		// and ANDI with -1 leave the destination alone.
		if ((v == 0 && (op == OP_ADD || op == OP_SUB || op == OP_OR || op == OP_XOR))
			|| (v == -1 && op == OP_AND))
			return;
		switch (op) {
		case OP_ADD: cc.add (d, imm(v)); break;
		case OP_SUB: cc.sub (d, imm(v)); break;
		case OP_AND: cc.and_(d, imm(v)); break;
		case OP_OR:  cc.or_ (d, imm(v)); break;
		case OP_XOR: cc.xor_(d, imm(v)); break;
		}
	}
	// rd = rs1 <op> rs2, taking care of the case where the destination virtual
	// register is the same one as a source that is still needed.
	void binop_rr(Op op, bool commutative, unsigned rd, unsigned rs1, unsigned rs2) {
		const auto a = get(rs1), b = get(rs2);
		const auto d = def(rd);
		if (d.id() == b.id()) {
			if (commutative) { alu_rr(op, d, a); return; }
			auto t = cc.new_gp32("tmp");
			cc.mov(t, a);
			alu_rr(op, t, b);
			cc.mov(d, t);
			return;
		}
		if (d.id() != a.id()) cc.mov(d, a);
		alu_rr(op, d, b);
	}
	void binop_ri(Op op, unsigned rd, unsigned rs1, int32_t v) {
		const auto a = get(rs1);
		const auto d = def(rd);
		if (d.id() != a.id()) cc.mov(d, a);
		alu_ri(op, d, v);
	}
	// 0 = shl, 1 = shr, 2 = sar. x86 masks the count by 0x1F for 32-bit
	// operands, which is exactly the RISC-V rule for RV32.
	void shift_rr(int kind, unsigned rd, unsigned rs1, unsigned rs2) {
		const auto a = get(rs1), b = get(rs2);
		const auto d = def(rd);
		auto c = cc.new_gp32("shcnt");
		cc.mov(c, b);                          // copy first: d may alias b
		if (d.id() != a.id()) cc.mov(d, a);
		switch (kind) {
		case 0: cc.shl(d, c.r8()); break;
		case 1: cc.shr(d, c.r8()); break;
		case 2: cc.sar(d, c.r8()); break;
		}
	}
	void shift_ri(int kind, unsigned rd, unsigned rs1, uint32_t shamt) {
		const auto a = get(rs1);
		const auto d = def(rd);
		if (d.id() != a.id()) cc.mov(d, a);
		switch (kind) {
		case 0: cc.shl(d, imm(shamt)); break;
		case 1: cc.shr(d, imm(shamt)); break;
		case 2: cc.sar(d, imm(shamt)); break;
		}
	}
	void setcc_rr(bool is_unsigned, unsigned rd, unsigned rs1, unsigned rs2) {
		auto t = cc.new_gp8("cmp");
		cc.cmp(get(rs1), get(rs2));
		if (is_unsigned) cc.setb(t); else cc.setl(t);
		cc.movzx(def(rd).r32(), t.r8());
	}
	void setcc_ri(bool is_unsigned, unsigned rd, unsigned rs1, int32_t v) {
		auto t = cc.new_gp8("cmp");
		cc.cmp(get(rs1), imm(v));
		if (is_unsigned) cc.setb(t); else cc.setl(t);
		cc.movzx(def(rd).r32(), t.r8());
	}

	// --- memory ---
	// The effective address lives in a 64-bit virtual register that is only ever
	// written through its 32-bit half, so the upper half is implicitly zeroed and
	// the register can be used directly as an index into the arena.
	x86::Gp address_of(unsigned rs1, int32_t simm) {
		auto a = cc.new_gp64("addr");
		if (rs1 == 0) {
			cc.mov(a.r32(), imm(uint32_t(simm)));
		} else {
			cc.mov(a.r32(), get(rs1));
			if (simm != 0) cc.add(a.r32(), imm(simm));
		}
		return a;
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
		Label ok = cc.new_label();
		cc.cmp(x86::qword_ptr(st, offsetof(AjState<W>, max_counter)), imm(0));
		cc.jne(ok);
		emit_exit(pc, pend);
		cc.bind(ok);
	}
	void call_load_helper(address_t pc, unsigned funct3, const x86::Gp& dst,
		const x86::Gp& addr, uint32_t pend)
	{
		const auto* cb = info.cb;
		uint64_t fn = 0;
		switch (funct3) {
		case 0x0: fn = uint64_t(uintptr_t(cb->load_i8));  break;
		case 0x1: fn = uint64_t(uintptr_t(cb->load_i16)); break;
		case 0x2: fn = uint64_t(uintptr_t(cb->load_i32)); break;
		case 0x4: fn = uint64_t(uintptr_t(cb->load_u8));  break;
		case 0x5: fn = uint64_t(uintptr_t(cb->load_u16)); break;
		}
		InvokeNode* node;
		cc.invoke(Out(node), fn, load_signature());
		node->set_arg(0, cpu);
		node->set_arg(1, st);
		node->set_arg(2, addr.r32());
		node->set_arg(3, imm(uint32_t(pc)));
		node->set_ret(0, dst);
		emit_fault_check(pc, pend);
	}
	void call_store_helper(address_t pc, unsigned funct3, const x86::Gp& value,
		const x86::Gp& addr, uint32_t pend)
	{
		const auto* cb = info.cb;
		uint64_t fn = 0;
		switch (funct3) {
		case 0x0: fn = uint64_t(uintptr_t(cb->store_8));  break;
		case 0x1: fn = uint64_t(uintptr_t(cb->store_16)); break;
		case 0x2: fn = uint64_t(uintptr_t(cb->store_32)); break;
		}
		InvokeNode* node;
		cc.invoke(Out(node), fn, store_signature());
		node->set_arg(0, cpu);
		node->set_arg(1, st);
		node->set_arg(2, addr.r32());
		node->set_arg(3, value);
		node->set_arg(4, imm(uint32_t(pc)));
		emit_fault_check(pc, pend);
	}
	// (addr - RWREAD_BEGIN) < read_boundary, the same single-sided check the
	// interpreter uses. Both bounds are read from the machine rather than baked
	// in, so the code stays valid for every machine sharing this execute segment.
	void emit_arena_check(const x86::Gp& addr, bool is_write, const Label& slow) {
		auto t = cc.new_gp32("bchk");
		cc.mov(t, addr.r32());
		if (is_write)
			cc.sub(t, x86::dword_ptr(cpu, info.arena_roend));
		else
			cc.sub(t, imm(uint32_t(Memory<W>::RWREAD_BEGIN)));
		cc.cmp(t, x86::dword_ptr(cpu, is_write ? info.arena_wrbound : info.arena_rdbound));
		cc.jae(slow);
	}
	void emit_load(address_t pc, unsigned funct3, unsigned rd, unsigned rs1, int32_t simm)
	{
		auto addr = address_of(rs1, simm);
		const auto dst = def(rd);
		if (!info.inline_memory) {
			call_load_helper(pc, funct3, dst, addr, pending - 1);
			return;
		}
		Label slow = cc.new_label(), done = cc.new_label();
		emit_arena_check(addr, false, slow);
		switch (funct3) {
		case 0x0: cc.movsx(dst, x86::byte_ptr(arena, addr));  break; // LB
		case 0x1: cc.movsx(dst, x86::word_ptr(arena, addr));  break; // LH
		case 0x2: cc.mov  (dst, x86::dword_ptr(arena, addr)); break; // LW
		case 0x4: cc.movzx(dst, x86::byte_ptr(arena, addr));  break; // LBU
		case 0x5: cc.movzx(dst, x86::word_ptr(arena, addr));  break; // LHU
		}
		cc.bind(done);
		deferred.push_back([=, this, pend = pending - 1] {
			cc.bind(slow);
			call_load_helper(pc, funct3, dst, addr, pend);
			cc.jmp(done);
		});
	}
	void emit_store(address_t pc, unsigned funct3, unsigned rs1, unsigned rs2, int32_t simm)
	{
		auto addr = address_of(rs1, simm);
		const auto src = get(rs2);
		if (!info.inline_memory) {
			call_store_helper(pc, funct3, src, addr, pending - 1);
			return;
		}
		Label slow = cc.new_label(), done = cc.new_label();
		emit_arena_check(addr, true, slow);
		switch (funct3) {
		case 0x0: cc.mov(x86::byte_ptr(arena, addr),  src.r8());  break; // SB
		case 0x1: cc.mov(x86::word_ptr(arena, addr),  src.r16()); break; // SH
		case 0x2: cc.mov(x86::dword_ptr(arena, addr), src);       break; // SW
		}
		cc.bind(done);
		deferred.push_back([=, this, pend = pending - 1] {
			cc.bind(slow);
			call_store_helper(pc, funct3, src, addr, pend);
			cc.jmp(done);
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
			case RV32I_OP:
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
			labels.emplace(t, cc.new_label());
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
			cc.jmp(label_at(entry));
		address_t fallthrough_pc = entry_is_first() ? instrs.front() : 0;
		for (const address_t pc : instrs)
		{
			const bool is_target = branch_targets.count(pc) != 0;
			if (is_target) {   // merge point: settle the counter first
				if (pc == fallthrough_pc) flush_counter();
				else pending = 0;   // only reachable through the label
				cc.bind(label_at(pc));
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
				cc.mov(def(i.Utype.rd), imm(uint32_t(i.Utype.upper_imm())));
				break;

			case RV32I_AUIPC:
				cc.mov(def(i.Utype.rd), imm(uint32_t(pc + i.Utype.upper_imm())));
				break;

			case RV32I_FENCE:
				// The interpreter makes every FENCE form a full barrier; match it
				// rather than reason about which guest orderings x86-TSO covers.
				cc.mfence();
				break;

			case RV32I_OP_IMM: {
				const unsigned rd = i.Itype.rd, rs1 = i.Itype.rs1;
				const int32_t simm = i.Itype.signed_imm();
				switch (i.Itype.funct3) {
				case 0x0: // ADDI
					if (rs1 == 0) cc.mov(def(rd), imm(uint32_t(simm)));
					else          binop_ri(OP_ADD, rd, rs1, simm);
					break;
				case 0x1: shift_ri(0, rd, rs1, i.Itype.shift_imm()); break; // SLLI
				case 0x2: setcc_ri(false, rd, rs1, simm); break;            // SLTI
				case 0x3: setcc_ri(true,  rd, rs1, simm); break;            // SLTIU
				case 0x4: binop_ri(OP_XOR, rd, rs1, simm); break;           // XORI
				case 0x5: // SRLI / SRAI
					shift_ri((i.Itype.imm & 0xFE0) == 0x400 ? 2 : 1, rd, rs1, i.Itype.shift_imm());
					break;
				case 0x6: binop_ri(OP_OR,  rd, rs1, simm); break;           // ORI
				case 0x7: binop_ri(OP_AND, rd, rs1, simm); break;           // ANDI
				}
				} break;

			case RV32I_OP: {
				const unsigned rd = i.Rtype.rd, rs1 = i.Rtype.rs1, rs2 = i.Rtype.rs2;
				const bool alt = (i.Rtype.funct7 == 0b0100000);
				switch (i.Rtype.funct3) {
				case 0x0: // ADD / SUB
					if (alt) binop_rr(OP_SUB, false, rd, rs1, rs2);
					else     binop_rr(OP_ADD, true,  rd, rs1, rs2);
					break;
				case 0x1: shift_rr(0, rd, rs1, rs2); break;              // SLL
				case 0x2: setcc_rr(false, rd, rs1, rs2); break;          // SLT
				case 0x3: setcc_rr(true,  rd, rs1, rs2); break;          // SLTU
				case 0x4: binop_rr(OP_XOR, true, rd, rs1, rs2); break;   // XOR
				case 0x5: shift_rr(alt ? 2 : 1, rd, rs1, rs2); break;    // SRL / SRA
				case 0x6: binop_rr(OP_OR,  true, rd, rs1, rs2); break;   // OR
				case 0x7: binop_rr(OP_AND, true, rd, rs1, rs2); break;   // AND
				}
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
				Label notaken = cc.new_label();
				if (i.Btype.rs2 == 0)
					cc.cmp(get(i.Btype.rs1), imm(0));
				else
					cc.cmp(get(i.Btype.rs1), get(i.Btype.rs2));
				// Jump *around* the taken path, so the taken path stays inline.
				switch (i.Btype.funct3) {
				case 0x0: cc.jne(notaken); break;   // BEQ
				case 0x1: cc.je (notaken); break;   // BNE
				case 0x4: cc.jge(notaken); break;   // BLT
				case 0x5: cc.jl (notaken); break;   // BGE
				case 0x6: cc.jae(notaken); break;   // BLTU
				case 0x7: cc.jb (notaken); break;   // BGEU
				}
				if (in_region(target)) {
					if (target <= pc) emit_backedge_check(target); // bound the loop
					cc.jmp(label_at(target));   // vregs stay live across the back-edge
				} else {
					emit_exit(target, 0);
				}
				cc.bind(notaken);
				} break;

			case RV32I_JAL: {
				const address_t target = pc + i.Jtype.jump_offset();
				if (i.Jtype.rd != 0)
					cc.mov(def(i.Jtype.rd), imm(uint32_t(next)));
				flush_counter();
				if (in_region(target)) {
					if (target <= pc) emit_backedge_check(target);
					cc.jmp(label_at(target));
				} else {
					emit_exit(target, 0);
				}
				fallthrough_pc = 0;   // nothing falls through an unconditional jump
				} break;

			case RV32I_JALR: {
				// Read rs1 into a temporary before writing rd: they may be the same
				// register, and RISC-V takes the old value of rs1 as the target.
				auto target = cc.new_gp32("jalr");
				const int32_t simm = i.Itype.signed_imm();
				if (i.Itype.rs1 == 0) {
					cc.mov(target, imm(uint32_t(simm) & ~uint32_t(1)));
				} else {
					cc.mov(target, get(i.Itype.rs1));
					if (simm != 0) cc.add(target, imm(simm));
					cc.and_(target, imm(~uint32_t(1)));
				}
				if (i.Itype.rd != 0)
					cc.mov(def(i.Itype.rd), imm(uint32_t(next)));
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

	x86::Compiler cc(&code);
	FuncNode* fn = cc.add_func(FuncSignature::build<void, void*, void*>());
	if (fn == nullptr)
		return nullptr;

	AjEmitter<W> e { cc, exec.exec_data(), entry, instrs, exec.exec_end(), info };
	e.cpu     = cc.new_gpz("cpu");
	e.st      = cc.new_gpz("state");
	e.counter = cc.new_gp64("counter");
	fn->set_arg(0, e.cpu);
	fn->set_arg(1, e.st);

	e.prepass();
	cc.mov(e.counter, x86::qword_ptr(e.st, offsetof(AjState<W>, counter)));
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
	return nullptr;   // v1 targets x86-64 only
}

#endif

#ifdef RISCV_32I
	template aj_block_func<4> aj_emit_region<4>(AjCode&, const MachineOptions<4>&,
		const DecodedExecuteSegment<4>&, const AjInfo<4>&,
		address_type<4>, const std::vector<address_type<4>>&);
#endif
} // riscv
