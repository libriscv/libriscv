#pragma once
#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>

namespace riscv
{
	template <int W> struct CPU;

#ifndef RISCV_128BIT_ISA
	template <int N>
	using address_type = typename std::conditional<(N == 4), uint32_t, uint64_t>::type;

	template <int N>
	using signed_address_type = typename std::conditional<(N == 4), int32_t, int64_t>::type;
#else
	template <int N>
	using address_type = typename std::conditional<(N == 4), uint32_t,
		typename std::conditional<(N == 8), uint64_t, __uint128_t>::type>::type;

	template <int N>
	using signed_address_type = typename std::conditional<(N == 4), int32_t,
		typename std::conditional<(N == 8), int64_t, __int128_t>::type>::type;
#endif

	enum exceptions
	{
		ILLEGAL_OPCODE,
		ILLEGAL_OPERATION,
		PROTECTION_FAULT,
		EXECUTION_SPACE_PROTECTION_FAULT,
		MISALIGNED_INSTRUCTION,
		UNIMPLEMENTED_INSTRUCTION_LENGTH,
		UNIMPLEMENTED_INSTRUCTION,
		UNHANDLED_SYSCALL,
		OUT_OF_MEMORY,
		INVALID_ALIGNMENT,
		DEADLOCK_REACHED,
		MAX_INSTRUCTIONS_REACHED,
		FEATURE_DISABLED,
		INVALID_PROGRAM,
		SYSTEM_CALL_FAILED,
		EXECUTION_LOOP_DETECTED,
		UNKNOWN_EXCEPTION
	};

	using instruction_format  = union rv32i_instruction;
	template <int W>
	using instruction_handler = void (*)(CPU<W> &, instruction_format);
	template <int W>
	using instruction_printer = int  (*)(char*, size_t, const CPU<W>&, instruction_format);
	template <int W>
	using register_type  = address_type<W>;

	template <int W>
	struct Instruction {
		instruction_handler<W> handler; // callback for executing one instruction
		instruction_printer<W> printer; // callback for logging one instruction
	};

	class MachineException : public std::exception {
	public:
		explicit MachineException(const int type, const char* text, const uint64_t data = 0)
			: m_type{type}, m_data{data}, m_msg{text} {}

		virtual ~MachineException() throw() {}

		int         type() const throw() { return m_type; }
		uint64_t    data() const throw() { return m_data; }
		const char* what() const throw() override { return m_msg; }
	protected:
		const int      m_type;
		const uint64_t m_data;
		const char*    m_msg;
	};

	class MachineTimeoutException : public MachineException {
		using MachineException::MachineException;
	};

	enum trapmode {
		TRAP_READ  = 0x0,
		TRAP_WRITE = 0x1000,
		TRAP_EXEC  = 0x2000,
	};

	template <int W>
	struct TransInfo;

	template <int W>
	struct TransInstr
	{
		uint32_t instr;
	};
}
