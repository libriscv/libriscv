#include "aj_api.hpp"
#include "../machine.hpp"
#include "../internal_common.hpp"

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
