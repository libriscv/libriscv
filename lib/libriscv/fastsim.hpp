#pragma once
#include "machine.hpp"
#include <inttypes.h>

namespace riscv
{
	[[maybe_unused]] static constexpr bool VERBOSE_FASTSIM = false;

	template <int W>
	struct QCData {
		instruction_handler<W> handler;
		uint32_t instr;
		uint16_t idxend;
		uint8_t  original_opcode;
		uint8_t  reserved;
	};
	template <int W>
	struct QCVec {
		address_type<W> base_pc;
		address_type<W> end_pc;
#ifdef RISCV_EXT_COMPRESSED
		uint16_t  incrementor;
#endif
		std::vector<QCData<W>> data;
	};

	template <int W> inline
	void CPU<W>::add_qc(QCVec<W> data) {
		if (this->m_qcdata == nullptr)
			this->m_qcdata = std::make_shared<std::vector<QCVec<W>>> ();
		data.data.shrink_to_fit();
		this->m_qcdata->push_back(std::move(data));
	}
	template <int W> inline
	void CPU<W>::finish_qc() {
		this->m_qcdata->shrink_to_fit();
		this->m_fastsim_vector = this->m_qcdata->data();
	}

	template <int W> inline
	void verbose_fast_sim(CPU<W>& cpu, instruction_handler<W> handler, rv32i_instruction instruction)
	{
		const auto string = [&] () -> std::string {
			if (handler != &CPU<W>::fast_simulator)
				return isa_type<W>::to_string(cpu, instruction, cpu.decode(instruction)) + "\n";
			char buffer[1024];
			return std::string(buffer, snprintf(buffer, sizeof(buffer),
				"[0x%" PRIX64 "] %08" PRIx32 " Fast simulator index (%u)\n",
				(uint64_t)cpu.pc(), instruction.whole, instruction.half[0]));
			}();
		cpu.machine().print(string.c_str(), string.size());
	}
}
