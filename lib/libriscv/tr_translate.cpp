#include <EASTL/hash_set.h>
#include <dlfcn.h>
#include <unistd.h>
#include "machine.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "tr_api.hpp"
#include "util/crc32.hpp"

namespace riscv
{
	static constexpr int  TRANSLATION_TRESHOLD = 6;
	static constexpr int  INSTRUCTIONS_MAX = 64'000;
	static constexpr int  TRANSLATIONS_MAX = 4000;
	static constexpr int  LOOP_OFFSET_MAX = 80;
	static constexpr bool SCAN_FOR_GP = true;

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
	RV32I_SYSTEM,
	RV32I_FENCE,
	RV64I_OP_IMM32,
	RV64I_OP32,
	RV32F_LOAD,
	RV32F_STORE,
	RV32F_FMADD,
	RV32F_FMSUB,
	RV32F_FNMADD,
	RV32F_FNMSUB,
	RV32F_FPFUNC,
};

template <int W>
inline uint32_t opcode(const typename CPU<W>::instr_pair& ip) {
	return ip.second.opcode();
}
template <int W>
inline bool gucci(const typename CPU<W>::instr_pair& ip) {
	return good_insn.count(opcode<W>(ip)) > 0;
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

	address_t gp = 0;
if constexpr (SCAN_FOR_GP) {
	// We assume that GP is initialized with AUIPC,
	// followed by OP_IMM (and maybe OP_IMM32)
	for (auto it = ipairs.begin(); it != ipairs.end(); ++it)
	if (it->second.opcode() == RV32I_AUIPC) {
		const auto auipc = it->second;
		if (auipc.Utype.rd == 3) { // GP
			// calculate current PC for AUIPC
			const address_t pc = basepc + 4 * (it - ipairs.begin());
			const auto addi = (it+1)->second;
			if (addi.opcode() == RV32I_OP_IMM && addi.Itype.funct3 == 0x0) {
				//printf("Found OP_IMM: ADDI  rd=%d, rs1=%d\n", addi.Itype.rd, addi.Itype.rs1);
				if (addi.Itype.rd == 3 && addi.Itype.rs1 == 3) { // GP
					gp = pc + auipc.Utype.upper_imm() + addi.Itype.signed_imm();
					break;
				}
			}
		}
	}
} // SCAN_FOR_GP

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
			bool has_branch = false;
			// measure block length
			while (++it != ipairs.end()) {
				// we can include this but not continue after
				if (it->second.opcode() == RV32I_JALR ||
					(it->second.opcode() == RV32I_SYSTEM && it->second.Itype.funct3 == 0x0 && it->second.Itype.imm == 1))
				{
					++it; break;
				}
if constexpr (LOOP_OFFSET_MAX > 0) {
				// loop detection (negative branch offsets)
				if (it->second.opcode() == RV32I_BRANCH && it->second.Btype.sign()) {
					has_branch = true;
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
				emit(code, func, &*block, length, {basepc, gp, has_branch});
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
		printf("Emitted %zu accelerated instructions and %zu functions. GP=0x%lX\n",
			icounter, dlmappings.size(), (long) gp);
	}
	// nothing to compile without mappings
	if (dlmappings.empty()) {
		return;
	}
	if (machine().memory.is_binary_translated()) {
		throw std::runtime_error("Machine already reports binary translation");
	}
	const size_t code_size = code.size();
	// add compiler arguments to make it part of checksum
	extern std::string compile_command(int arch);
	code.append(compile_command(W));
	// create cacheable filename
	const uint32_t checksum = crc32c(code.c_str(), code.size());
	char filename[1024];
	int len = snprintf(filename, sizeof(filename),
		"/tmp/rvbintr-%08X", checksum);
	if (len <= 0) {
		return;
	}
	void* dylib = nullptr;
	code.resize(code_size); // remove compiler arguments

#ifdef RISCV_TRANSLATION_CACHE
	if (access(filename, R_OK) == 0) {
		dylib = dlopen(filename, RTLD_LAZY);
	}
#endif
	// compile ourselves
	if (dylib == nullptr) {
		extern void* compile(const std::string& code, int arch, const char*);
		dylib = compile(code, W, filename);
		// check compilation result
		if (dylib == nullptr) {
			return;
		}
	}
	// map the API callback table
	auto* ptr = dlsym(dylib, "init");
	if (ptr == nullptr) {
		fprintf(stderr, "libriscv: Could not find dylib init function\n");
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
		.syscall = [] (CPU<W>& cpu, uint64_t val) -> int {
			auto old_pc = cpu.pc();
			cpu.registers().pc += val * 4;
			cpu.machine().system_call(cpu.reg(17));
			// if the system did not modify PC, return to bintr
			if (cpu.pc() - val * 4 == old_pc && !cpu.machine().stopped()) {
				cpu.registers().pc = old_pc;
				return 0;
			}
			// otherwise, update instruction counter and exit
			cpu.machine().increment_counter(val);
			return 1;
		},
		.ebreak = [] (CPU<W>& cpu, uint64_t val) {
			cpu.registers().pc += val * 4;
			cpu.machine().increment_counter(val);
			cpu.machine().ebreak();
		},
		.system = [] (CPU<W>& cpu, uint32_t instr) {
			cpu.machine().system(rv32i_instruction{instr});
		},
		.trigger_exception = [] (CPU<W>& cpu, int e) {
			cpu.trigger_exception(e);
		},
		.sqrtf32 = [] (float f) -> float {
			return std::sqrt(f);
		},
		.sqrtf64 = [] (double d) -> double {
			return std::sqrt(d);
		},
	});

	// map all the functions
	for (auto& mapping : dlmappings) {
		auto* func = dlsym(dylib, mapping.symbol.c_str());
		if (func != nullptr) {
			mapping.ipair.first = (instruction_handler<W>) func;
		}
	}

#ifndef RISCV_TRANSLATION_CACHE
	// delete program
	unlink(filename);
#endif
	// close dylib when machine is destructed
	machine().memory.set_binary_translated(dylib);
}

	template void CPU<4>::try_translate(address_t, std::vector<instr_pair>&) const;
	template void CPU<8>::try_translate(address_t, std::vector<instr_pair>&) const;
	static_assert(!compressed_enabled,
		"C-extension incompatible with binary translation");
}
