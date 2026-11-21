#pragma once
// The bodies of the bytecodes that used to be in bytecode_impl.cpp,
// now separated for sharing with fused bytecode handlers as well
#include "threaded_bytecodes.hpp"

namespace riscv
{
	template <unsigned Bytecode> struct BcTag {};

#pragma push_macro("DECODER")
#undef DECODER
#pragma push_macro("CPU")
#undef CPU
#pragma push_macro("REG")
#undef REG
#pragma push_macro("REGISTERS")
#undef REGISTERS
#pragma push_macro("VECTORS")
#undef VECTORS
#pragma push_macro("MACHINE")
#undef MACHINE
#pragma push_macro("VIEW_INSTR")
#undef VIEW_INSTR
#pragma push_macro("VIEW_INSTR_AS")
#undef VIEW_INSTR_AS
#pragma push_macro("NEXT_INSTR")
#undef NEXT_INSTR
#pragma push_macro("NEXT_C_INSTR")
#undef NEXT_C_INSTR
#pragma push_macro("UNUSED_FUNCTION")
#undef UNUSED_FUNCTION
#define DECODER()   (decoder)
#define CPU()       (cpu)
#define REG(x)      cpu.registers().get()[x]
#define REGISTERS() cpu.registers()
#define VECTORS()   cpu.registers().rvv()
#define MACHINE()   cpu.machine()
#define VIEW_INSTR() \
	auto instr = *(rv32i_instruction *)&decoder.instr;
#define VIEW_INSTR_AS(name, x) \
	auto &&name = *(x *)&decoder.instr;
#define NEXT_INSTR()   return
#define NEXT_C_INSTR() return
#define UNUSED_FUNCTION() cpu.trigger_exception(ILLEGAL_OPCODE)
#define BYTECODE_BODY_PROLOGUE() \
	using addr_t  [[maybe_unused]] = address_type<W>; \
	using saddr_t [[maybe_unused]] = signed_address_type<W>; \
	[[maybe_unused]] static constexpr uint32_t XLEN = W * 8;

#ifdef RISCV_EXT_COMPRESSED
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_ADDI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) = REG(fi.get_rs2()) + fi.signed_imm();
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_MV>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterMove);
	REG(fi.get_rd()) = REG(fi.get_rs1());
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_SLLI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) <<= fi.imm;
	NEXT_C_INSTR();
}
#endif
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_ADDI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) =
		REG(fi.get_rs2()) + fi.signed_imm();
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_LI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterImmediate);
	REG(fi.get_rd()) = fi.signed_imm();
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_MV>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterMove);
	REG(fi.get_rd()) = REG(fi.get_rs1());
	NEXT_INSTR();
}
#ifdef RISCV_64I
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_ADDIW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		REG(fi.get_rs1()) = (int32_t)
			((uint32_t)REG(fi.get_rs2()) + fi.signed_imm());
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
#endif // RISCV_64I
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_SLLI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// SLLI: Logical left-shift 5/6/7-bit immediate
	REG(fi.get_rs1()) =
		REG(fi.get_rs2()) << fi.unsigned_imm();
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_SLTI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// SLTI: Set less than immediate
	REG(fi.get_rs1()) = (saddr_t(REG(fi.get_rs2())) < fi.signed_imm());
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_SLTIU>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// SLTIU: Sign-extend, then treat as unsigned
	REG(fi.get_rs1()) = (REG(fi.get_rs2()) < addr_t(fi.signed_imm()));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_XORI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// XORI
	REG(fi.get_rs1()) = REG(fi.get_rs2()) ^ fi.signed_imm();
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_SRLI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// SRLI: Shift-right logical 5/6/7-bit immediate
	REG(fi.get_rs1()) = REG(fi.get_rs2()) >> fi.unsigned_imm();
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_SRAI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// SRAI: Shift-right arithmetical (preserve the sign bit)
	REG(fi.get_rs1()) = saddr_t(REG(fi.get_rs2())) >> fi.unsigned_imm();
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_ORI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// ORI: Or sign-extended 12-bit immediate
	REG(fi.get_rs1()) = REG(fi.get_rs2()) | fi.signed_imm();
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_ANDI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// ANDI: And sign-extended 12-bit immediate
	REG(fi.get_rs1()) = REG(fi.get_rs2()) & fi.signed_imm();
	NEXT_INSTR();
}
#ifdef RISCV_EXT_COMPRESSED
#endif // RISCV_EXT_COMPRESSED
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_LDW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		(int32_t)CPU().memory().template read<uint32_t>(addr);
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_STW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr  = REG(fi.get_rs1()) + fi.signed_imm();
	CPU().memory().template write<uint32_t>(addr, REG(fi.get_rs2()));
	NEXT_INSTR();
}
#ifdef RISCV_64I
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_LDD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
		REG(fi.get_rs1()) =
			(int64_t)CPU().memory().template read<uint64_t>(addr);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_STD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr  = REG(fi.get_rs1()) + fi.signed_imm();
		CPU().memory().template write<uint64_t>(addr, REG(fi.get_rs2()));
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
#endif // RISCV_64I
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32F_BC_FLW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	auto addr = REG(fi.rs2) + fi.signed_imm();
	auto& dst = REGISTERS().getfl(fi.rs1);
	dst.load_u32(CPU().memory().template read<uint32_t> (addr));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32F_BC_FLD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	auto addr = REG(fi.rs2) + fi.signed_imm();
	auto& dst = REGISTERS().getfl(fi.rs1);
	dst.load_u64(CPU().memory().template read<uint64_t> (addr));
	NEXT_INSTR();
}
#ifdef RISCV_EXT_COMPRESSED
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_LDD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
		REG(fi.get_rs1()) =
			(int64_t)CPU().memory().template read<uint64_t>(addr);
		NEXT_C_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_STD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
		CPU().memory().template write<uint64_t>(addr, REG(fi.get_rs2()));
		NEXT_C_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_LDW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		(int32_t)CPU().memory().template read<uint32_t>(addr);
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_STW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
	CPU().memory().template write<uint32_t>(addr, REG(fi.get_rs2()));
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_FLD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.rs2) + fi.signed_imm();
	REGISTERS().getfl(fi.rs1).load_u64(CPU().memory().template read<uint64_t>(addr));
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_FSD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.rs1) + fi.signed_imm();
	CPU().memory().template write<uint64_t>(addr, REGISTERS().getfl(fi.rs2).i64);
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_FLW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.rs2) + fi.signed_imm();
	REGISTERS().getfl(fi.rs1).load_u32(CPU().memory().template read<uint32_t>(addr));
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_FSW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.rs1) + fi.signed_imm();
	CPU().memory().template write<uint32_t>(addr, REGISTERS().getfl(fi.rs2).i32[0]);
	NEXT_C_INSTR();
}
#endif // RISCV_EXT_COMPRESSED
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_AUIPC>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterJtype);
	// AUIPC using re-constructed PC
	//REG(instr.Utype.rd) = (pc - DECODER().block_bytes()) + instr.Utype.upper_imm();
	REG(fi.rd) = (pc - DECODER().block_bytes()) + fi.upper_imm();
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_LUI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterJtype);
	REG(fi.rd) = fi.upper_imm();
	NEXT_INSTR();
}
#ifdef RISCV_EXT_COMPRESSED
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_LUI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterJtype);
	REG(fi.rd) = fi.upper_imm();
	NEXT_C_INSTR();
}
#endif
#define OP_INSTR()                       \
	VIEW_INSTR_AS(fi, FasterOpType);     \
	auto& dst = REG(fi.get_rd());        \
	const auto src1 = REG(fi.get_rs1()); \
	const auto src2 = REG(fi.get_rs2());
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_ADD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src1 + src2;
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_SUB>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src1 - src2;
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_SLL>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src1 << (src2 & (XLEN - 1));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_SLT>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = (saddr_t(src1) < saddr_t(src2));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_SLTU>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = (src1 < src2);
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_XOR>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src1 ^ src2;
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_SRL>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src1 >> (src2 & (XLEN - 1));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_OR>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src1 | src2;
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_AND>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src1 & src2;
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_MUL>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	// MUL keeps the low XLEN bits, which is the same product either way, but
	// only the unsigned multiplication is defined when it overflows
	dst = addr_t(src1) * addr_t(src2);
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_SH1ADD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src2 + (src1 << 1);
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_SH2ADD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src2 + (src1 << 2);
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_SH3ADD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = src2 + (src1 << 3);
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_SRA>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = saddr_t(src1) >> (src2 & (XLEN-1));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_ZEXT_H>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = uint16_t(src1);
	(void)src2;
	NEXT_INSTR();
}
#ifdef RISCV_64I
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_OP_ADDW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = int32_t(uint32_t(src1) + uint32_t(src2));
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_OP_SUBW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = int32_t(uint32_t(src1) - uint32_t(src2));
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_OP_MULW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = int32_t(uint32_t(src1) * uint32_t(src2));
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_OP_ADD_UW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = uint32_t(src1) + src2;
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_SLLIW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		REG(fi.get_rs1()) = (int32_t)
			((uint32_t)REG(fi.get_rs2()) << fi.imm);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_SRLIW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		REG(fi.get_rs1()) = (int32_t)
			((uint32_t)REG(fi.get_rs2()) >> fi.imm);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
#endif // RISCV_64I
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_SEXT_B>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) = saddr_t(int8_t(REG(fi.get_rs2())));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_SEXT_H>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) = saddr_t(int16_t(REG(fi.get_rs2())));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_BSETI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// BSETI: Bit-set immediate
	REG(fi.get_rs1()) =
		REG(fi.get_rs2()) | (addr_t(1) << fi.unsigned_imm());
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_BEXTI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	// BEXTI: Single-bit Extract
	REG(fi.get_rs1()) =
		(REG(fi.get_rs2()) >> fi.unsigned_imm()) & 1;
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32F_BC_FSW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto& src = REGISTERS().getfl(fi.rs2);
	auto addr = REG(fi.rs1) + fi.signed_imm();
	CPU().memory().template write<uint32_t> (addr, src.i32[0]);
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32F_BC_FSD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto& src = REGISTERS().getfl(fi.rs2);
	auto addr = REG(fi.rs1) + fi.signed_imm();
	CPU().memory().template write<uint64_t> (addr, src.i64);
	NEXT_INSTR();
}
// Spec §11.3: an FP operation that produces a NaN must deliver the canonical
// quiet NaN, not the payload-propagating one a host FPU hands us. Guests that
// care are the ones that also want fflags, so the canonicalization rides along
// with FCSR emulation: without it a scripting guest would have to read the raw
// bits back as an integer to tell the difference, and the branch is not worth
// paying for on every single FP instruction.
#define SET_FLOAT_CANON(dst, expr) \
	if constexpr (fcsr_emulation) { \
		const float fcanon_r = (expr); \
		if (UNLIKELY(std::isnan(fcanon_r))) dst.load_u32(0x7FC00000u); \
		else dst.set_float(fcanon_r); \
	} else { \
		dst.set_float(expr); \
	}
#define SET_DOUBLE_CANON(dst, expr) \
	if constexpr (fcsr_emulation) { \
		const double dcanon_r = (expr); \
		if (UNLIKELY(std::isnan(dcanon_r))) dst.load_u64(0x7FF8000000000000ull); \
		else dst.f64 = dcanon_r; \
	} else { \
		dst.f64 = (expr); \
	}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32F_BC_FADD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterFloatType);
	#define FLREGS() \
		auto& dst = REGISTERS().getfl(fi.get_rd()); \
		const auto& rs1 = REGISTERS().getfl(fi.get_rs1()); \
		const auto& rs2 = REGISTERS().getfl(fi.get_rs2());
	FLREGS();
	if (fi.func == 0x0)
	{ // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		if constexpr (fcsr_emulation) {
			// The operands are read out before the store, because rd is
			// allowed to alias rs1 or rs2. NX when the exact sum (computed on
			// operands widened to double) does not round-trip to the single-
			// precision result.
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			const double exact = (double)fa + (double)fb;
			SET_FLOAT_CANON(dst, fa + fb);
			if ((double)(fa + fb) != exact) REGISTERS().fcsr().fflags |= 1;
		} else {
			SET_FLOAT_CANON(dst, rs1.f32[0] + rs2.f32[0]);
		}
	}
	else
	{ // float64
		SET_DOUBLE_CANON(dst, rs1.f64 + rs2.f64);
	}
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32F_BC_FSUB>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterFloatType);
	FLREGS();
	if (fi.func == 0x0)
	{ // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		if constexpr (fcsr_emulation) {
			// Operands read out before the store: rd may alias rs1 or rs2.
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			const double exact = (double)fa - (double)fb;
			SET_FLOAT_CANON(dst, fa - fb);
			if ((double)(fa - fb) != exact) REGISTERS().fcsr().fflags |= 1;
		} else {
			SET_FLOAT_CANON(dst, rs1.f32[0] - rs2.f32[0]);
		}
	}
	else
	{ // float64
		SET_DOUBLE_CANON(dst, rs1.f64 - rs2.f64);
	}
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32F_BC_FMUL>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterFloatType);
	FLREGS();
	if (fi.func == 0x0)
	{ // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		if constexpr (fcsr_emulation) {
			// The operands are read out before the store, because rd is
			// allowed to alias rs1 or rs2.
			const uint32_t ia = rs1.i32[0], ib = rs2.i32[0];
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			SET_FLOAT_CANON(dst, fa * fb);
			// Finite inputs that produce an infinity overflowed; finite inputs
			// that produce an inexact subnormal underflowed. Both imply NX.
			if ((ia & 0x7f800000u) != 0x7f800000u
				&& (ib & 0x7f800000u) != 0x7f800000u) {
				const uint32_t result = dst.i32[0] & 0x7fffffffu;
				if (result == 0x7f800000u) {
					CPU().registers().fcsr().fflags |= 5; // OF | NX
					// The overflow value depends on the rounding mode (IEEE 754
					// §7.4): RTZ saturates to the largest finite number for
					// either sign, RDN only for a positive result and RUP only
					// for a negative one. RNE and RMM keep the host infinity.
					const unsigned rm = CPU().registers().fcsr().frm;
					const bool neg = (dst.i32[0] & 0x80000000u) != 0;
					if (rm == 0x1 || (rm == 0x2 && !neg) || (rm == 0x3 && neg))
						dst.i32[0] = (dst.i32[0] & 0x80000000u) | 0x7F7FFFFFu;
				}
				else if (result < 0x00800000u
					&& (double)fa * (double)fb != dst.f32[0])
					CPU().registers().fcsr().fflags |= 3; // UF | NX
			}
		} else {
			dst.set_float(rs1.f32[0] * rs2.f32[0]);
		}
	}
	else
	{ // float64
		SET_DOUBLE_CANON(dst, rs1.f64 * rs2.f64);
	}
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32F_BC_FDIV>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterFloatType);
	FLREGS();
	if (fi.func == 0x0)
	{ // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		if constexpr (fcsr_emulation) {
			// Operands read out before the store: rd may alias rs1 or rs2.
			// DZ is only for a finite non-zero numerator over zero: 0/0 is NV
			// and inf/0 is exact.
			const uint32_t ia = rs1.i32[0], ib = rs2.i32[0];
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			const float fr = fa / fb;
			const double exact = (double)fa / (double)fb;
			SET_FLOAT_CANON(dst, fr);
			if (fr != fr) {
				if ((ia & 0x7fffffffu) == 0 && (ib & 0x7fffffffu) == 0)
					CPU().registers().fcsr().fflags |= 16; // NV
				else if ((ia & 0x7f800000u) == 0x7f800000u
					&& (ib & 0x7f800000u) == 0x7f800000u)
					CPU().registers().fcsr().fflags |= 16; // NV
			} else {
				if ((ia & 0x7fffffffu) != 0 && (ia & 0x7f800000u) != 0x7f800000u
					&& (ib & 0x7fffffffu) == 0)
					CPU().registers().fcsr().fflags |= 8; // DZ
				if ((double)fr != exact)
					CPU().registers().fcsr().fflags |= 1; // NX
				const uint32_t ir = dst.i32[0] & 0x7fffffffu;
				if ((ib & 0x7fffffffu) != 0 && (ia & 0x7f800000u) != 0x7f800000u
					&& ir == 0x7f800000u)
					CPU().registers().fcsr().fflags |= 5; // OF | NX
				else if (ir < 0x00800000u && (double)fr != exact)
					CPU().registers().fcsr().fflags |= 3; // UF | NX
			}
		} else {
			dst.set_float(rs1.f32[0] / rs2.f32[0]);
		}
	}
	else
	{ // float64
		if constexpr (fcsr_emulation) {
			// Operands read out before the store: rd may alias rs1 or rs2.
			const uint64_t ia = rs1.i64, ib = rs2.i64;
			const double da = rs1.f64, db = rs2.f64;
			const double dr = da / db;
			const long double exact = (long double)da / (long double)db;
			SET_DOUBLE_CANON(dst, dr);
			if (dr != dr) {
				if ((ia & 0x7fffffffffffffffull) == 0 && (ib & 0x7fffffffffffffffull) == 0)
					CPU().registers().fcsr().fflags |= 16; // NV
				else if ((ia & 0x7ff0000000000000ull) == 0x7ff0000000000000ull
					&& (ib & 0x7ff0000000000000ull) == 0x7ff0000000000000ull)
					CPU().registers().fcsr().fflags |= 16; // NV
			} else {
				if ((ia & 0x7fffffffffffffffull) != 0
					&& (ia & 0x7ff0000000000000ull) != 0x7ff0000000000000ull
					&& (ib & 0x7fffffffffffffffull) == 0)
					CPU().registers().fcsr().fflags |= 8; // DZ
				if ((long double)dr != exact)
					CPU().registers().fcsr().fflags |= 1; // NX
				const uint64_t ir = dst.i64 & 0x7fffffffffffffffull;
				if ((ib & 0x7fffffffffffffffull) != 0
					&& (ia & 0x7ff0000000000000ull) != 0x7ff0000000000000ull
					&& ir == 0x7ff0000000000000ull)
					CPU().registers().fcsr().fflags |= 5; // OF | NX
				else if (ir < 0x0010000000000000ull && (long double)dr != exact)
					CPU().registers().fcsr().fflags |= 3; // UF | NX
			}
		} else {
			dst.f64 = rs1.f64 / rs2.f64;
		}
	}
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32F_BC_FMADD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, rv32f_instruction);
	#define FMAREGS() \
		auto& dst = REGISTERS().getfl(fi.R4type.rd);  \
		auto& rs1 = REGISTERS().getfl(fi.R4type.rs1); \
		auto& rs2 = REGISTERS().getfl(fi.R4type.rs2); \
		auto& rs3 = REGISTERS().getfl(fi.R4type.rs3);
	FMAREGS();
	// RISC-V spec §11.6: FMA must round only once. std::fma is the
	// IEEE 754 fused multiply-add (single rounding). Prior code used
	// `a * b + c`, which does two roundings — spec violation, visible
	// as 1-ULP divergences in the fuzz oracle.
	//
	// Spec §11.3: a NaN result must be the canonical qNaN. Only under FCSR
	// emulation, same as the other arithmetic bytecodes.
	if (fi.R4type.funct2 == 0x0) { // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs3.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		SET_FLOAT_CANON(dst, std::fma(rs1.f32[0], rs2.f32[0], rs3.f32[0]));
	} else if (fi.R4type.funct2 == 0x1) { // float64
		SET_DOUBLE_CANON(dst, std::fma(rs1.f64, rs2.f64, rs3.f64));
	}
	NEXT_INSTR();
}
#ifdef RISCV_64I
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_SRAIW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		//dst = (int32_t)src >> instr.Itype.shift_imm();
		REG(fi.get_rs1()) =
			(int32_t)REG(fi.get_rs2()) >> fi.imm;
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_OP_SH1ADD_UW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = src2 + (addr_t(uint32_t(src1)) << 1);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV64I_BC_OP_SH2ADD_UW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = src2 + (addr_t(uint32_t(src1)) << 2);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
#endif // RISCV_64I
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_DIV>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = rv_div<addr_t>(src1, src2);
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_DIVU>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	if (LIKELY(src2 != 0)) {
		dst = src1 / src2;
	} else {
		dst = addr_t(-1);
	}
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_REM>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	dst = rv_rem<addr_t>(src1, src2);
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_OP_REMU>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	OP_INSTR();
	if (LIKELY(src2 != 0)) {
		dst = src1 % src2;
	} else {
		dst = src1;
	}
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_LDB>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		int8_t(CPU().memory().template read<uint8_t>(addr));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_LDBU>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		saddr_t(CPU().memory().template read<uint8_t>(addr));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_LDH>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		int16_t(CPU().memory().template read<uint16_t>(addr));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_LDHU>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		saddr_t(CPU().memory().template read<uint16_t>(addr));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_STB>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
	CPU().memory().template write<uint8_t>(addr, REG(fi.get_rs2()));
	NEXT_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_STH>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
	CPU().memory().template write<uint16_t>(addr, REG(fi.get_rs2()));
	NEXT_INSTR();
}
#ifdef RISCV_64I
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32I_BC_LDWU>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
		REG(fi.get_rs1()) =
			CPU().memory().template read<uint32_t>(addr);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
#endif // RISCV_64I
#ifdef RISCV_EXT_COMPRESSED
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_SRLI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) >>= fi.imm;
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_ADD>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) += REG(fi.get_rs2());
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_XOR>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) ^= REG(fi.get_rs2());
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_OR>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) |= REG(fi.get_rs2());
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_SRAI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) = saddr_t(REG(fi.get_rs1())) >> fi.imm;
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_SUB>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) -= REG(fi.get_rs2());
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_AND>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) &= REG(fi.get_rs2());
	NEXT_C_INSTR();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_SUBW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W == 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		REG(fi.get_rs1()) = (int32_t)
			((uint32_t)REG(fi.get_rs1()) - (uint32_t)REG(fi.get_rs2()));
		NEXT_C_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_ADDW>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	if constexpr (W == 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		REG(fi.get_rs1()) = (int32_t)
			((uint32_t)REG(fi.get_rs1()) + (uint32_t)REG(fi.get_rs2()));
		NEXT_C_INSTR();
	}
	else UNUSED_FUNCTION();
}
template <int W> static inline RISCV_ALWAYS_INLINE
void bytecode_body(BcTag<RV32C_BC_ANDI>, CPU<W>& cpu, DecoderData<W>& decoder, [[maybe_unused]] address_type<W> pc)
{
	BYTECODE_BODY_PROLOGUE();
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) &= fi.signed_imm();
	NEXT_C_INSTR();
}
#endif
#ifdef RISCV_EXT_VECTOR
#endif // RISCV_EXT_VECTOR

#undef BYTECODE_BODY_PROLOGUE
#undef OP_INSTR
#undef SET_FLOAT_CANON
#undef SET_DOUBLE_CANON
#undef FLREGS
#undef FMAREGS
#pragma pop_macro("DECODER")
#pragma pop_macro("CPU")
#pragma pop_macro("REG")
#pragma pop_macro("REGISTERS")
#pragma pop_macro("VECTORS")
#pragma pop_macro("MACHINE")
#pragma pop_macro("VIEW_INSTR")
#pragma pop_macro("VIEW_INSTR_AS")
#pragma pop_macro("NEXT_INSTR")
#pragma pop_macro("NEXT_C_INSTR")
#pragma pop_macro("UNUSED_FUNCTION")
} // riscv
