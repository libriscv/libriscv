#include "../asm_dispatch_glue.hpp"
#include <vector>

// Both RV32 and RV64 dispatches are written in assembly: amd64/dispatch_rv.nasm,
// assembled once per width and mode. The counting dispatch (prefix rvXXasmc_)
// is CPU::simulate and follows cpu_dispatch.cpp, and the inaccurate dispatch
// (prefix rvXXasm_) is CPU::simulate_inaccurate. The C++ threaded dispatch is
// still built, as CPU::simulate_cpp, for 128-bit and for host CPUs without
// the extensions that the assembly uses.

// Handlers in dispatch_rv.nasm. Each handler symbol is an array with the
// address of every copy (see handler replication in the assembly).
#define ASMDISPATCH_HANDLERS(X) \
	X(fallback) X(stop) \
	X(addi) X(li) X(mv) X(slli) X(srli) X(srai) X(slti) X(sltiu) \
	X(xori) X(ori) X(andi) X(sext_b) X(sext_h) X(bseti) X(bexti) \
	X(lui) X(auipc) \
	X(op_add) X(op_sub) X(op_sll) X(op_srl) X(op_sra) X(op_slt) X(op_sltu) \
	X(op_xor) X(op_or) X(op_and) X(op_mul) X(op_zext_h) \
	X(op_div) X(op_divu) X(op_rem) X(op_remu) \
	X(op_sh1add) X(op_sh2add) X(op_sh3add) \
	X(function) X(funcblock) X(syscall) X(dyncall) X(system) X(step) \
	X(translator) X(asmjit) X(livepatch) \
	X(beq) X(bne) X(beq_fw) X(bne_fw) X(blt) X(bge) X(bltu) X(bgeu) \
	X(jal) X(fast_jal) X(fast_call) X(jalr) \
	X(ldw) X(ldh) X(ldhu) X(ldb) X(ldbu) X(stw) X(sth) X(stb) \
	X(flw) X(fld) X(fsw) X(fsd) X(fadd) X(fsub) X(fmul) X(fdiv) X(fmadd)
#define ASMDISPATCH_RV64_HANDLERS(X) \
	X(addiw) X(slliw) X(srliw) X(sraiw) \
	X(op_addw) X(op_subw) X(op_mulw) X(op_add_uw) X(op_sh1add_uw) X(op_sh2add_uw) \
	X(ldwu) X(ldd) X(std)

// The compressed bytecodes (dispatch_rv.nasm assembled with -DCOMPRESSED=1)
#ifdef RISCV_EXT_COMPRESSED
#define ASMDISPATCH_C_HANDLERS(X) \
	X(c_addi) X(c_mv) X(c_slli) X(c_srli) X(c_andi) X(c_add) X(c_xor) X(c_or) \
	X(c_bnez) X(c_beqz) X(c_jmp) X(c_jal_addiw) X(c_jr) X(c_jalr) \
	X(c_function) X(c_ldw) X(c_stw) X(c_lui) X(c_fld) X(c_fsd) X(c_step) \
	X(c_srai) X(c_sub) X(c_and)
#define ASMDISPATCH_RV32_C_HANDLERS(X) X(c_flw) X(c_fsw)
#define ASMDISPATCH_RV64_C_HANDLERS(X) X(c_ldd) X(c_std) X(c_subw) X(c_addw)
#else
#define ASMDISPATCH_C_HANDLERS(X) /* */
#define ASMDISPATCH_RV32_C_HANDLERS(X) /* */
#define ASMDISPATCH_RV64_C_HANDLERS(X) /* */
#endif

namespace riscv {
	struct AsmDispatchContext;
	template <int W> struct DecoderData;
}
using riscv::AsmDispatchContext;
using riscv::DecoderData;

extern "C" {
#define ASMDISPATCH_DECLARE_ONE(P, name) extern void* const P##name[];
#ifdef RISCV_32I
	uint32_t rv32asm_dispatch(AsmDispatchContext* ctx);
	uint32_t rv32asmc_dispatch(AsmDispatchContext* ctx);
	extern const uint32_t rv32asm_ncopies;
	extern const uint32_t rv32asmc_ncopies;
#define RV32ASM_DECLARE(name)  ASMDISPATCH_DECLARE_ONE(rv32asm_, name)
#define RV32ASMC_DECLARE(name) ASMDISPATCH_DECLARE_ONE(rv32asmc_, name)
	ASMDISPATCH_HANDLERS(RV32ASM_DECLARE)
	ASMDISPATCH_HANDLERS(RV32ASMC_DECLARE)
	ASMDISPATCH_C_HANDLERS(RV32ASM_DECLARE)
	ASMDISPATCH_C_HANDLERS(RV32ASMC_DECLARE)
	ASMDISPATCH_RV32_C_HANDLERS(RV32ASM_DECLARE)
	ASMDISPATCH_RV32_C_HANDLERS(RV32ASMC_DECLARE)
#endif
#ifdef RISCV_64I
	uint32_t rv64asm_dispatch(AsmDispatchContext* ctx);
	uint32_t rv64asmc_dispatch(AsmDispatchContext* ctx);
	extern const uint32_t rv64asm_ncopies;
	extern const uint32_t rv64asmc_ncopies;
#define RV64ASM_DECLARE(name)  ASMDISPATCH_DECLARE_ONE(rv64asm_, name)
#define RV64ASMC_DECLARE(name) ASMDISPATCH_DECLARE_ONE(rv64asmc_, name)
	ASMDISPATCH_HANDLERS(RV64ASM_DECLARE)
	ASMDISPATCH_RV64_HANDLERS(RV64ASM_DECLARE)
	ASMDISPATCH_HANDLERS(RV64ASMC_DECLARE)
	ASMDISPATCH_RV64_HANDLERS(RV64ASMC_DECLARE)
	ASMDISPATCH_C_HANDLERS(RV64ASM_DECLARE)
	ASMDISPATCH_C_HANDLERS(RV64ASMC_DECLARE)
	ASMDISPATCH_RV64_C_HANDLERS(RV64ASM_DECLARE)
	ASMDISPATCH_RV64_C_HANDLERS(RV64ASMC_DECLARE)
#endif
}

namespace riscv {

// The CTX_* offsets in dispatch_rv.nasm
static_assert(offsetof(AsmDispatchContext, read_boundary) == 40);
static_assert(offsetof(AsmDispatchContext, table) == 64);
static_assert(offsetof(AsmDispatchContext, pc) == 72);
static_assert(offsetof(AsmDispatchContext, cpu) == 80);
static_assert(offsetof(AsmDispatchContext, counter) == 88);
static_assert(offsetof(AsmDispatchContext, max_counter) == 96);
static_assert(offsetof(AsmDispatchContext, enter_count) == 104);
static_assert(sizeof(DecoderData<4>) == 8 && sizeof(DecoderData<8>) == 8);

// table[copy * 256 + bytecode] = copy `copy` of the handler
#define H(bc, sym) for (unsigned c = 0; c < copies; c++) t[c * 256 + (bc)] = sym[c];

#define ASMDISPATCH_FILL(P) \
	for (unsigned c = 0; c < copies; c++) \
		for (unsigned bc = 0; bc < 256; bc++) \
			t[c * 256 + bc] = P##fallback[c]; \
	H(RV32I_BC_STOP, P##stop) \
	H(RV32I_BC_ADDI, P##addi)   H(RV32I_BC_LI, P##li)       H(RV32I_BC_MV, P##mv) \
	H(RV32I_BC_SLLI, P##slli)   H(RV32I_BC_SRLI, P##srli)   H(RV32I_BC_SRAI, P##srai) \
	H(RV32I_BC_SLTI, P##slti)   H(RV32I_BC_SLTIU, P##sltiu) H(RV32I_BC_XORI, P##xori) \
	H(RV32I_BC_ORI, P##ori)     H(RV32I_BC_ANDI, P##andi) \
	H(RV32I_BC_SEXT_B, P##sext_b) H(RV32I_BC_SEXT_H, P##sext_h) \
	H(RV32I_BC_BSETI, P##bseti) H(RV32I_BC_BEXTI, P##bexti) \
	H(RV32I_BC_LUI, P##lui)     H(RV32I_BC_AUIPC, P##auipc) \
	H(RV32I_BC_OP_ADD, P##op_add) H(RV32I_BC_OP_SUB, P##op_sub) \
	H(RV32I_BC_OP_SLL, P##op_sll) H(RV32I_BC_OP_SRL, P##op_srl) H(RV32I_BC_OP_SRA, P##op_sra) \
	H(RV32I_BC_OP_SLT, P##op_slt) H(RV32I_BC_OP_SLTU, P##op_sltu) \
	H(RV32I_BC_OP_XOR, P##op_xor) H(RV32I_BC_OP_OR, P##op_or) H(RV32I_BC_OP_AND, P##op_and) \
	H(RV32I_BC_OP_MUL, P##op_mul) H(RV32I_BC_OP_ZEXT_H, P##op_zext_h) \
	H(RV32I_BC_OP_DIV, P##op_div) H(RV32I_BC_OP_DIVU, P##op_divu) \
	H(RV32I_BC_OP_REM, P##op_rem) H(RV32I_BC_OP_REMU, P##op_remu) \
	H(RV32I_BC_OP_SH1ADD, P##op_sh1add) H(RV32I_BC_OP_SH2ADD, P##op_sh2add) \
	H(RV32I_BC_OP_SH3ADD, P##op_sh3add) \
	H(RV32I_BC_FUNCTION, P##function) H(RV32I_BC_FUNCBLOCK, P##funcblock) \
	H(RV32I_BC_SYSCALL, P##syscall) H(RV32I_BC_DYNCALL, P##dyncall) \
	H(RV32I_BC_SYSTEM, P##system) H(RV32I_BC_LIVEPATCH, P##livepatch) \
	ASMDISPATCH_FILL_NATIVE(P) \
	H(RV32I_BC_BEQ, P##beq)     H(RV32I_BC_BNE, P##bne) \
	H(RV32I_BC_BEQ_FW, P##beq_fw) H(RV32I_BC_BNE_FW, P##bne_fw) \
	H(RV32I_BC_BLT, P##blt)     H(RV32I_BC_BGE, P##bge) \
	H(RV32I_BC_BLTU, P##bltu)   H(RV32I_BC_BGEU, P##bgeu) \
	H(RV32I_BC_JAL, P##jal)     H(RV32I_BC_FAST_JAL, P##fast_jal) \
	H(RV32I_BC_FAST_CALL, P##fast_call) H(RV32I_BC_JALR, P##jalr) \
	H(RV32I_BC_LDW, P##ldw)     H(RV32I_BC_LDH, P##ldh)     H(RV32I_BC_LDHU, P##ldhu) \
	H(RV32I_BC_LDB, P##ldb)     H(RV32I_BC_LDBU, P##ldbu) \
	H(RV32I_BC_STW, P##stw)     H(RV32I_BC_STH, P##sth)     H(RV32I_BC_STB, P##stb) \
	H(RV32F_BC_FLD, P##fld)     H(RV32F_BC_FSW, P##fsw)     H(RV32F_BC_FSD, P##fsd) \
	/* NaN-boxing and FCSR emulation are left to C++ (CPU::step_one) */ \
	if constexpr (!nanboxing) { H(RV32F_BC_FLW, P##flw) } \
	else { H(RV32F_BC_FLW, P##step) } \
	if constexpr (!nanboxing && !fcsr_emulation) { \
		H(RV32F_BC_FADD, P##fadd) H(RV32F_BC_FSUB, P##fsub) \
		H(RV32F_BC_FMUL, P##fmul) H(RV32F_BC_FDIV, P##fdiv) \
		H(RV32F_BC_FMADD, P##fmadd) \
	} else { \
		H(RV32F_BC_FADD, P##step) H(RV32F_BC_FSUB, P##step) \
		H(RV32F_BC_FMUL, P##step) H(RV32F_BC_FDIV, P##step) \
		H(RV32F_BC_FMADD, P##step) \
	} \
	ASMDISPATCH_FILL_C(P)
#ifdef RISCV_EXT_COMPRESSED
#define ASMDISPATCH_FILL_C(P) \
	H(RV32C_BC_ADDI, P##c_addi) H(RV32C_BC_LI, P##c_addi) H(RV32C_BC_MV, P##c_mv) \
	H(RV32C_BC_SLLI, P##c_slli) H(RV32C_BC_SRLI, P##c_srli) H(RV32C_BC_ANDI, P##c_andi) \
	H(RV32C_BC_ADD, P##c_add)   H(RV32C_BC_XOR, P##c_xor)   H(RV32C_BC_OR, P##c_or) \
	H(RV32C_BC_BNEZ, P##c_bnez) H(RV32C_BC_BEQZ, P##c_beqz) H(RV32C_BC_JMP, P##c_jmp) \
	H(RV32C_BC_JAL_ADDIW, P##c_jal_addiw) \
	H(RV32C_BC_JR, P##c_jr)     H(RV32C_BC_JALR, P##c_jalr) \
	H(RV32C_BC_FUNCTION, P##c_function) \
	H(RV32C_BC_LDW, P##c_ldw)   H(RV32C_BC_STW, P##c_stw) \
	H(RV32C_BC_LUI, P##c_lui)   H(RV32C_BC_FLD, P##c_fld)   H(RV32C_BC_FSD, P##c_fsd) \
	H(RV32C_BC_SRAI, P##c_srai) H(RV32C_BC_SUB, P##c_sub)   H(RV32C_BC_AND, P##c_and)
#define ASMDISPATCH_FILL_RV32_C(P) \
	if constexpr (!nanboxing) { H(RV32C_BC_FLW, P##c_flw) } \
	else { H(RV32C_BC_FLW, P##c_step) } \
	H(RV32C_BC_FSW, P##c_fsw)
#define ASMDISPATCH_FILL_RV64_C(P) \
	H(RV32C_BC_LDD, P##c_ldd)   H(RV32C_BC_STD, P##c_std) \
	H(RV32C_BC_SUBW, P##c_subw) H(RV32C_BC_ADDW, P##c_addw)
#else
#define ASMDISPATCH_FILL_C(P) /* */
#define ASMDISPATCH_FILL_RV32_C(P) /* */
#define ASMDISPATCH_FILL_RV64_C(P) /* */
#endif

#ifdef RISCV_BINARY_TRANSLATION
#define ASMDISPATCH_FILL_TRANSLATOR(P) H(RV32I_BC_TRANSLATOR, P##translator)
#else
#define ASMDISPATCH_FILL_TRANSLATOR(P) /* */
#endif
#ifdef RISCV_ASMJIT
#define ASMDISPATCH_FILL_ASMJIT(P) H(RV32I_BC_ASMJIT, P##asmjit)
#else
#define ASMDISPATCH_FILL_ASMJIT(P) /* */
#endif
#define ASMDISPATCH_FILL_NATIVE(P) ASMDISPATCH_FILL_TRANSLATOR(P) ASMDISPATCH_FILL_ASMJIT(P)
#define ASMDISPATCH_FILL_RV64(P) \
	H(RV64I_BC_ADDIW, P##addiw) \
	H(RV64I_BC_SLLIW, P##slliw) H(RV64I_BC_SRLIW, P##srliw) H(RV64I_BC_SRAIW, P##sraiw) \
	H(RV64I_BC_OP_ADDW, P##op_addw) H(RV64I_BC_OP_SUBW, P##op_subw) \
	H(RV64I_BC_OP_MULW, P##op_mulw) H(RV64I_BC_OP_ADD_UW, P##op_add_uw) \
	H(RV64I_BC_OP_SH1ADD_UW, P##op_sh1add_uw) H(RV64I_BC_OP_SH2ADD_UW, P##op_sh2add_uw) \
	H(RV32I_BC_LDWU, P##ldwu) H(RV32I_BC_LDD, P##ldd) H(RV32I_BC_STD, P##std) \
	ASMDISPATCH_FILL_RV64_C(P)

template <int W, bool Counting>
struct AsmDispatch;

#define ASMDISPATCH_DEFINE(W, COUNTING, P, RV64FILL) \
template <> \
struct AsmDispatch<W, COUNTING> { \
	static uint32_t dispatch(AsmDispatchContext* ctx) { return P##dispatch(ctx); } \
	static void prepare(AsmDispatchContext& ctx) { ctx.table = table(); } \
	static void on_yield(AsmDispatchContext&) {} \
	static void** table() { \
		static std::vector<void*> t = [] { \
			const unsigned copies = P##ncopies; \
			std::vector<void*> t(copies * 256); \
			ASMDISPATCH_FILL(P) \
			RV64FILL \
			return t; \
		}(); \
		return t.data(); \
	} \
}; \
extern "C" uint32_t P##call(AsmDispatchContext* ctx, DecoderData<W>* entry, uint32_t kind) noexcept \
{ \
	return asm_call<W, COUNTING>(ctx, entry, kind); \
}

#ifdef RISCV_32I
ASMDISPATCH_DEFINE(4, false, rv32asm_, ASMDISPATCH_FILL_RV32_C(rv32asm_))
ASMDISPATCH_DEFINE(4, true, rv32asmc_, ASMDISPATCH_FILL_RV32_C(rv32asmc_))
#endif
#ifdef RISCV_64I
ASMDISPATCH_DEFINE(8, false, rv64asm_, ASMDISPATCH_FILL_RV64(rv64asm_))
ASMDISPATCH_DEFINE(8, true, rv64asmc_, ASMDISPATCH_FILL_RV64(rv64asmc_))
#endif
#undef H

// The assembly uses BMI2, AVX and FMA3
static bool asm_dispatch_supported()
{
	static const bool supported = __builtin_cpu_supports("bmi2")
		&& __builtin_cpu_supports("avx") && __builtin_cpu_supports("fma");
	return supported;
}

template <int W>
bool CPU<W>::simulate(address_t pc, uint64_t inscounter, uint64_t maxcounter)
{
	if constexpr (W == 4 || W == 8) {
		if (LIKELY(asm_dispatch_supported()))
			return asm_simulate<W, true, AsmDispatch<W, true>>(*this, pc, inscounter, maxcounter);
	}
	return this->simulate_cpp(pc, inscounter, maxcounter);
}

template <int W>
void CPU<W>::simulate_inaccurate(address_t pc)
{
	if constexpr (W == 4 || W == 8) {
		machine().set_instruction_counter(0);
		machine().set_max_instructions(UINT64_MAX);
		if (UNLIKELY(!asm_dispatch_supported())) {
			// The accurate dispatch, with a counter that never runs out
			this->simulate_cpp(pc, 0, UINT64_MAX);
			return;
		}
		asm_simulate<W, false, AsmDispatch<W, false>>(*this, pc, 0, UINT64_MAX);
	} else {
		// For 128-bit, we don't have an inaccurate dispatch
		throw MachineException(FEATURE_DISABLED, "Inaccurate dispatch is not implemented for this CPU width");
	}
}

	INSTANTIATE_32_IF_ENABLED(CPU);
	INSTANTIATE_64_IF_ENABLED(CPU);
#ifdef RISCV_128I
	// The rest of CPU<16> is instantiated with the C++ dispatch
	template bool CPU<16>::simulate(address_type<16>, uint64_t, uint64_t);
#endif
} // riscv
