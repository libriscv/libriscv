#include "aj_api.hpp"
#include "../machine.hpp"
#include "../internal_common.hpp"
#include "../rvfd_util.hpp"
#include "../rv32i_instr.hpp"

namespace riscv
{
	// Records the exception, stops the machine, and forces every subsequent
	// counter check in generated code to exit (max_counter = 0), so that a
	// faulting region terminates at its next backward branch or exit instead
	// of spinning all the way to the instruction limit.
	template <int W>
	static void aj_fault(CPU<W>& cpu, AjState<W>* st) noexcept
	{
		cpu.set_current_exception(std::current_exception());
		st->max_counter = 0;
		cpu.machine().stop();
	}

	// `result` is the C++ type the value is extended through before it lands in
	// the destination register, which is what makes LB/LH sign-extend and
	// LBU/LHU zero-extend without any further work in generated code.
#define AJ_LOAD_HELPER(name, type, result)                                        \
	template <int W>                                                              \
	static address_type<W> name(CPU<W>& cpu, AjState<W>* st,                      \
		address_type<W> addr, address_type<W> pc) noexcept                        \
	{                                                                             \
		try {                                                                     \
			return address_type<W>(result(cpu.machine().memory.template read<type> (addr))); \
		} catch (...) {                                                           \
			cpu.registers().pc = pc;                                              \
			aj_fault(cpu, st);                                                    \
			return 0;                                                             \
		}                                                                         \
	}

	AJ_LOAD_HELPER(aj_load_i8,  uint8_t,  int8_t)
	AJ_LOAD_HELPER(aj_load_u8,  uint8_t,  uint8_t)
	AJ_LOAD_HELPER(aj_load_i16, uint16_t, int16_t)
	AJ_LOAD_HELPER(aj_load_u16, uint16_t, uint16_t)
	AJ_LOAD_HELPER(aj_load_i32, uint32_t, int32_t)
	// Only ever emitted for W == 8; on RV32 the result would not fit a register,
	// but the entries exist so that the callback table has one shape.
	AJ_LOAD_HELPER(aj_load_u32, uint32_t, uint32_t)
	AJ_LOAD_HELPER(aj_load_i64, uint64_t, int64_t)

#define AJ_STORE_HELPER(name, type)                                               \
	template <int W>                                                              \
	static void name(CPU<W>& cpu, AjState<W>* st,                                 \
		address_type<W> addr, address_type<W> value, address_type<W> pc) noexcept \
	{                                                                             \
		try {                                                                     \
			cpu.machine().memory.template write<type> (addr, type(value));        \
		} catch (...) {                                                           \
			cpu.registers().pc = pc;                                              \
			aj_fault(cpu, st);                                                    \
		}                                                                         \
	}

	AJ_STORE_HELPER(aj_store_8,  uint8_t)
	AJ_STORE_HELPER(aj_store_16, uint16_t)
	AJ_STORE_HELPER(aj_store_32, uint32_t)
	AJ_STORE_HELPER(aj_store_64, uint64_t)   // RV64 only

	// --- F/D extension -----------------------------------------------------
	// Same shape as the integer accessors, but with a uint64_t payload on both
	// guest widths: RV32D moves 64-bit doubles through 32-bit registers.
#define AJ_FLOAD_HELPER(name, type)                                           \
	template <int W>                                                          \
	static uint64_t name(CPU<W>& cpu, AjState<W>* st,                         \
		address_type<W> addr, address_type<W> pc) noexcept                    \
	{                                                                         \
		try {                                                                 \
			return uint64_t(cpu.machine().memory.template read<type> (addr)); \
		} catch (...) {                                                       \
			cpu.registers().pc = pc;                                          \
			aj_fault(cpu, st);                                                \
			return 0;                                                         \
		}                                                                     \
	}

	AJ_FLOAD_HELPER(aj_load_fl,  uint32_t)
	AJ_FLOAD_HELPER(aj_load_dbl, uint64_t)

#define AJ_FSTORE_HELPER(name, type)                                          \
	template <int W>                                                          \
	static void name(CPU<W>& cpu, AjState<W>* st,                             \
		address_type<W> addr, uint64_t value, address_type<W> pc) noexcept    \
	{                                                                         \
		try {                                                                 \
			cpu.machine().memory.template write<type> (addr, type(value));    \
		} catch (...) {                                                       \
			cpu.registers().pc = pc;                                          \
			aj_fault(cpu, st);                                                \
		}                                                                     \
	}

	AJ_FSTORE_HELPER(aj_store_fl,  uint32_t)
	AJ_FSTORE_HELPER(aj_store_dbl, uint64_t)

	template <int W>
	static address_type<W> aj_fclass32(uint32_t bits) noexcept {
		return rv_fclass32(bits);
	}
	template <int W>
	static address_type<W> aj_fclass64(uint64_t bits) noexcept {
		return rv_fclass64(bits);
	}

	// The result type is what decides the extension the destination register
	// ends up with: FCVT.WU.* delivers a *sign*-extended 32-bit result on RV64,
	// which is why the unsigned word forms go through int32_t on the way out.
#define AJ_FCVT_TO_INT(name, dsttype, exttype, srctype)                       \
	template <int W>                                                          \
	static address_type<W> name(CPU<W>& cpu, srctype value, uint32_t rm) noexcept \
	{                                                                         \
		if (rm == 0x7) /* DYN: the mode comes from the fcsr */                \
			rm = cpu.registers().fcsr().frm;                                  \
		return address_type<W>(exttype(fcvt_to_integer<dsttype>(value, rm))); \
	}

	AJ_FCVT_TO_INT(aj_fcvt_w_s,  int32_t,  int32_t, float)
	AJ_FCVT_TO_INT(aj_fcvt_wu_s, uint32_t, int32_t, float)
	AJ_FCVT_TO_INT(aj_fcvt_l_s,  int64_t,  int64_t, float)
	AJ_FCVT_TO_INT(aj_fcvt_lu_s, uint64_t, uint64_t, float)
	AJ_FCVT_TO_INT(aj_fcvt_w_d,  int32_t,  int32_t, double)
	AJ_FCVT_TO_INT(aj_fcvt_wu_d, uint32_t, int32_t, double)
	AJ_FCVT_TO_INT(aj_fcvt_l_d,  int64_t,  int64_t, double)
	AJ_FCVT_TO_INT(aj_fcvt_lu_d, uint64_t, uint64_t, double)

	// Executes one instruction on the interpreter's handler. Handlers reached
	// this way never modify PC nor stop the machine, so translated code
	// continues at the next instruction unless the handler traps.
	template <int W>
	static void aj_execute(CPU<W>& cpu, AjState<W>* st, uint32_t instr,
		address_type<W> pc, const void* handler) noexcept
	{
		cpu.registers().pc = pc;
		try {
			auto fn = reinterpret_cast<instruction_handler<W>>(
				reinterpret_cast<uintptr_t>(handler));
			fn(cpu, rv32i_instruction{instr});
		} catch (...) {
			aj_fault(cpu, st);
		}
	}

	// Zbc's carry-less multiplies, mirroring rvi_instr.cpp, for hosts without
	// a carry-less multiply instruction.
	template <int W>
	static address_type<W> aj_clmul(address_type<W> a, address_type<W> b) noexcept
	{
		address_type<W> result = 0;
		for (unsigned i = 0; i < 8 * sizeof(a); i++)
			if ((b >> i) & 1) result ^= (a << i);
		return result;
	}
	template <int W>
	static address_type<W> aj_clmulh(address_type<W> a, address_type<W> b) noexcept
	{
		address_type<W> result = 0;
		for (unsigned i = 1; i < 8 * sizeof(a); i++)
			if ((b >> i) & 1) result ^= (a >> (8 * sizeof(a) - i));
		return result;
	}
	template <int W>
	static address_type<W> aj_clmulr(address_type<W> a, address_type<W> b) noexcept
	{
		address_type<W> result = 0;
		for (unsigned i = 0; i < 8 * sizeof(a); i++)
			if ((b >> i) & 1) result ^= (a >> (8 * sizeof(a) - i - 1));
		return result;
	}

	static float  aj_fcvt_s_lu(uint64_t value) noexcept { return float(value); }
	static double aj_fcvt_d_lu(uint64_t value) noexcept { return double(value); }

	template <int W>
	const AjCallbacks<W>& aj_callbacks() noexcept
	{
		static const AjCallbacks<W> table {
			.load_i8  = aj_load_i8<W>,  .load_u8  = aj_load_u8<W>,
			.load_i16 = aj_load_i16<W>, .load_u16 = aj_load_u16<W>,
			.load_i32 = aj_load_i32<W>, .load_u32 = aj_load_u32<W>,
			.load_i64 = aj_load_i64<W>,
			.store_8  = aj_store_8<W>,  .store_16 = aj_store_16<W>,
			.store_32 = aj_store_32<W>, .store_64 = aj_store_64<W>,

			.load_fl   = aj_load_fl<W>,   .load_dbl  = aj_load_dbl<W>,
			.store_fl  = aj_store_fl<W>,  .store_dbl = aj_store_dbl<W>,
			.fmin32 = rv_fmin32, .fmax32 = rv_fmax32,
			.fmin64 = rv_fmin64, .fmax64 = rv_fmax64,
			.fclass32 = aj_fclass32<W>, .fclass64 = aj_fclass64<W>,
			.fcvt_w_s  = aj_fcvt_w_s<W>,  .fcvt_wu_s = aj_fcvt_wu_s<W>,
			.fcvt_l_s  = aj_fcvt_l_s<W>,  .fcvt_lu_s = aj_fcvt_lu_s<W>,
			.fcvt_w_d  = aj_fcvt_w_d<W>,  .fcvt_wu_d = aj_fcvt_wu_d<W>,
			.fcvt_l_d  = aj_fcvt_l_d<W>,  .fcvt_lu_d = aj_fcvt_lu_d<W>,
			.execute = aj_execute<W>,
			.clmul  = aj_clmul<W>,  .clmulh = aj_clmulh<W>,
			.clmulr = aj_clmulr<W>,
			.fcvt_s_lu = aj_fcvt_s_lu,    .fcvt_d_lu = aj_fcvt_d_lu,
		};
		return table;
	}

#ifdef RISCV_32I
	template const AjCallbacks<4>& aj_callbacks<4>() noexcept;
#endif
#ifdef RISCV_64I
	template const AjCallbacks<8>& aj_callbacks<8>() noexcept;
#endif
#ifdef RISCV_128I
	template const AjCallbacks<16>& aj_callbacks<16>() noexcept;
#endif
}
