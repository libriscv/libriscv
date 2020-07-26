#pragma once
#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>

namespace riscv
{
	struct RV32I;
	struct RV64I;
	template <int W> struct CPU;

	template <int N>
	using address_type = typename std::conditional<(N == 4), uint32_t, uint64_t>::type;

	template <int N>
	using isa_type = typename std::conditional<(N == 4), RV32I, RV64I>::type;

	using interrupt_t = uint8_t;

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
		UNKNOWN_EXCEPTION
	};

	class MachineException : public std::exception {
	public:
	    explicit MachineException(const int type, const char* message, const int data = 0)
			: m_type {type}, m_data { data }, m_msg { message }   {}

	    virtual ~MachineException() throw() {}

		int  type() const throw() { return m_type; }
	    const char* what() const throw() override { return m_msg; }
		int  data() const throw() { return m_data; }
	protected:
	    const int   m_type;
		const int   m_data;
		const char* m_msg;
	};

	class MachineTimeoutException : public MachineException {
		using MachineException::MachineException;
	};

	template <int W>
	using instruction_format  = union rv32i_instruction;
	template <int W>
	using instruction_handler = void (*)(CPU<W>&, instruction_format<W>);
	template <int W>
	using instruction_printer = int  (*)(char*, size_t, CPU<W>&, instruction_format<W>);

	template <int W>
	struct Instruction {
		using isa_t     = isa_type<W>;           // 32- or 64-bit architecture
		using format_t  = instruction_format<W>; // one machine instruction

		const instruction_handler<W> handler; // callback for executing one instruction
		const instruction_printer<W> printer; // callback for logging one instruction
	};

	enum trapmode {
		TRAP_READ  = 0x0,
		TRAP_WRITE = 0x1000,
		TRAP_EXEC  = 0x2000,
	};
}
