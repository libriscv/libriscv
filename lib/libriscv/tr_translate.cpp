#include <EASTL/hash_set.h>
#include <dlfcn.h>
#include <unistd.h>
#include "machine.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "tr_api.hpp"

namespace riscv
{
	static constexpr int TRANSLATION_TRESHOLD = 6;
	static constexpr int INSTRUCTIONS_MAX = 64'000;
	static constexpr int TRANSLATIONS_MAX = 4000;
	static constexpr int LOOP_OFFSET_MAX = 80;

static eastl::hash_set<uint32_t> good_insn
{
	RV32I_LOAD,
	RV32I_STORE,
	RV32I_BRANCH,
	RV32I_JAL,
	RV32I_OP_IMM,
	RV32I_OP,
	RV32I_LUI,
	RV32I_AUIPC,
	RV32I_FENCE,
	RV64I_OP_IMM32,
	RV64I_OP32,
	RV32F_LOAD,
	RV32F_STORE,
	RV32F_FMADD,
	RV32F_FMSUB,
	RV32F_FNMADD,
	RV32F_FNMSUB,
};

template <int W>
inline uint32_t opcode(const typename CPU<W>::instr_pair& ip) {
	return ip.second.opcode();
}
template <int W>
inline bool gucci(const typename CPU<W>::instr_pair& ip) {
	if (good_insn.count(opcode<W>(ip)) > 0) return true;
	// we support some FP functions
	if (ip.second.opcode() == RV32F_FPFUNC) {
		if (ip.second.fpfunc() == RV32F__FADD || ip.second.fpfunc() == RV32F__FSUB ||
			ip.second.fpfunc() == RV32F__FMUL || ip.second.fpfunc() == RV32F__FDIV ||
			ip.second.fpfunc() == RV32F__FCVT_SD_DS || ip.second.fpfunc() == RV32F__FCVT_SD_W ||
			ip.second.fpfunc() == RV32F__FSGNJ_NX) {
			return true;
		}
	}
	return false;
}

template <int W>
struct NamedIPair {
	using instr_pair = typename CPU<W>::instr_pair;
	instr_pair& ipair;
	std::string symbol;
};

template <int W>
void CPU<W>::try_translate(
	address_t basepc, std::vector<instr_pair>& ipairs) const
{
	std::vector<instr_pair*> candidates;
	std::vector<NamedIPair<W>> dlmappings;
	extern std::string bintr_code;
	std::string code = bintr_code;

	size_t icounter = 0;
	auto it = ipairs.begin();
	std::vector<std::pair<decltype(it), address_t>> loops;
	eastl::hash_set<address_t> already_generated;
	eastl::hash_set<address_t> already_looped;

	while (it != ipairs.end() && icounter < INSTRUCTIONS_MAX)
	{
		if (!loops.empty()) {
			it = loops.back().first;
			basepc = loops.back().second;
			loops.pop_back();
		}
		if (gucci<W>(*it))
		{
			auto block = it;
			// measure block length
			while (++it != ipairs.end()) {
				// we can include this but not continue after
				if (it->second.opcode() == RV32I_JALR ||
					(it->second.opcode() == RV32I_SYSTEM && it->second.Itype.funct3 == 0x0)) {
					++it; break;
				}
if constexpr (LOOP_OFFSET_MAX > 0) {
				// loop detection (negative branch offsets)
				if (it->second.opcode() == RV32I_BRANCH && it->second.Btype.sign()) {
					// detect jump location
					const auto& instr = it->second;
					const size_t length = it - block;
					const auto offset = instr.Btype.signed_imm();
					const auto dst = basepc + (4 * length) + offset;
					if (offset > -LOOP_OFFSET_MAX && already_looped.count(dst) == 0) {
						loops.push_back({it + offset / 4, dst});
						already_looped.insert(dst);
					}
				}
}
				// we can accelerate these and continue
				if (gucci<W>(*it) == false) {
					// must exit native, ending block
					break;
				}
			}
			const size_t length = it - block;
			if (length >= TRANSLATION_TRESHOLD && icounter + length < INSTRUCTIONS_MAX
				&& already_generated.count(basepc) == 0)
			{
				already_generated.insert(basepc);
				//printf("Block found at %#lX. Length: %zu\n", (long) basepc, length);
				std::string func =
					"f" + std::to_string(dlmappings.size());
				emit(code, func, basepc, &*block, length);
				dlmappings.push_back({*block, std::move(func)});
				icounter += length;
				// we can't translate beyond this estimate, otherwise
				// the compiler will never finish code generation
				if (dlmappings.size() >= TRANSLATIONS_MAX)
					break;
			}
			basepc += 4 * length;
		}
		else {
			basepc += 4;
			++it;
		}
	}
	// run with VERBOSE=1 to see command and output
	if (getenv("VERBOSE")) {
		printf("Emitted %zu accelerated instructions and %zu functions!\n",
			icounter, dlmappings.size());
	}
	// nothing to compile without mappings
	if (dlmappings.empty())
		return;

	extern std::pair<std::string, void*> compile(const std::string& code, int arch);
	auto [filename, dylib] = compile(code, W);
	if (dylib) {
		// map the API callback table
		auto* ptr = dlsym(dylib, "init");
		if (ptr == nullptr) {
			fprintf(stderr, "Could not find init function\n");
			dlclose(dylib);
			return;
		}
		auto func = (void (*)(const CallbackTable<W>&)) ptr;
		func(CallbackTable<W>{
			.mem_read8 = [] (CPU<W>& cpu, address_type<W> addr) -> uint8_t {
				return cpu.machine().memory.template read<uint8_t> (addr);
			},
			.mem_read16 = [] (CPU<W>& cpu, address_type<W> addr) -> uint16_t {
				return cpu.machine().memory.template read<uint16_t> (addr);
			},
			.mem_read32 = [] (CPU<W>& cpu, address_type<W> addr) -> uint32_t {
				return cpu.machine().memory.template read<uint32_t> (addr);
			},
			.mem_read64 = [] (CPU<W>& cpu, address_type<W> addr) -> uint64_t {
				return cpu.machine().memory.template read<uint64_t> (addr);
			},
			.mem_write8 = [] (CPU<W>& cpu, address_type<W> addr, uint8_t val) {
				cpu.machine().memory.template write<uint8_t> (addr, val);
			},
			.mem_write16 = [] (CPU<W>& cpu, address_type<W> addr, uint16_t val) {
				cpu.machine().memory.template write<uint16_t> (addr, val);
			},
			.mem_write32 = [] (CPU<W>& cpu, address_type<W> addr, uint32_t val) {
				cpu.machine().memory.template write<uint32_t> (addr, val);
			},
			.mem_write64 = [] (CPU<W>& cpu, address_type<W> addr, uint64_t val) {
				cpu.machine().memory.template write<uint64_t> (addr, val);
			},
			.finish_block = [] (CPU<W>& cpu, address_type<W> addr, uint64_t val) {
				cpu.registers().pc = addr;
				cpu.machine().increment_counter(val);
			},
			.jump = [] (CPU<W>& cpu, address_type<W> addr, uint64_t val) {
				cpu.jump(addr);
				cpu.machine().increment_counter(val);
			},
			.syscall = [] (CPU<W>& cpu, uint64_t val) {
				cpu.registers().pc += val * 4;
				cpu.machine().increment_counter(val);
				cpu.machine().system_call(cpu.reg(17));
			},
			.ebreak = [] (CPU<W>& cpu, uint64_t val) {
				cpu.registers().pc += val * 4;
				cpu.machine().increment_counter(val);
				cpu.machine().ebreak();
			},
			.trigger_exception = [] (CPU<W>& cpu, int e) {
				cpu.trigger_exception(e);
			},
		});

		// map all the functions
		for (auto& mapping : dlmappings) {
			auto* func = dlsym(dylib, mapping.symbol.c_str());
			if (func != nullptr) {
				mapping.ipair.first = (instruction_handler<W>) func;
			}
		}

		// delete program
		unlink(filename.c_str());
		// close dylib when machine is destructed
		/*machine().add_destructor_callback(
			[dylib] {
				dlclose(dylib);
			});*/
	}

}

	template void CPU<4>::try_translate(address_t, std::vector<instr_pair>&) const;
	template void CPU<8>::try_translate(address_t, std::vector<instr_pair>&) const;
}
