#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
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

	template <int W>
	struct Instruction {
		using isa_t     = isa_type<W>;              // 32- or 64-bit architecture
		using format_t  = typename isa_t::format_t; // one machine instruction

		using handler_t = void (*)(CPU<W>&, format_t);
		using printer_t = int  (*)(char*, size_t, CPU<W>&, format_t);

		const handler_t handler; // callback for executing one instruction
		const printer_t printer; // callback for logging one instruction
	};

	enum trapmode {
		TRAP_READ  = 0x0,
		TRAP_WRITE = 0x1000,
		TRAP_EXEC  = 0x2000,
	};
}
