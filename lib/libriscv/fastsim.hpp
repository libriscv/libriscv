#pragma once
#include "machine.hpp"
#include <inttypes.h>

namespace riscv
{
	template <int W>
	struct QCData {
		uint32_t instr;
		instruction_handler<W> handler;
	};
	template <int W>
	struct QCVec {
		address_type<W> base_pc;
		address_type<W> end_pc;
		uint16_t  incrementor;
		std::vector<QCData<W>> data;
	};

	template <int W> inline
	void CPU<W>::add_qc(const QCVec<W>& data) {
		if (this->m_qcdata == nullptr)
			this->m_qcdata = std::make_shared<std::vector<QCVec<W>>> ();
		this->m_qcdata->push_back(data);
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
