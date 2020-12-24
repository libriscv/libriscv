static constexpr int TRANSLATION_TRESHOLD = 6;
static constexpr int INSTRUCTIONS_MAX = 64'000;
static constexpr int TRANSLATIONS_MAX = 4000;
}
#include <EASTL/hash_set.h>
namespace riscv {

static eastl::hash_set<uint32_t> good_insn
{
	RV32I_LOAD,
	RV32I_STORE,
	RV32I_BRANCH,
	RV32I_OP_IMM,
	RV32I_OP,
	RV32I_LUI,
	RV32I_AUIPC,
	RV32I_FENCE,
	//RV64I_OP_IMM32,
	//RV64I_OP32,
	RV32F_LOAD,
	RV32F_STORE,
};

#include "tr_emit.cpp"

template <int W>
inline uint32_t opcode(const typename CPU<W>::instr_pair& ip) {
	return ip.second.opcode();
}
template <int W>
inline bool gucci(const typename CPU<W>::instr_pair& ip) {
	return good_insn.count(opcode<W>(ip)) > 0;
}

}
#include <dlfcn.h>
#include <unistd.h>
#include "tr_api.hpp"
namespace riscv {

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
	while (it != ipairs.end() && icounter < INSTRUCTIONS_MAX)
	{
		auto block = it;
		if (gucci<W>(*block))
		{
			// measure block length
			while (++it != ipairs.end()) {
				// we can include this but not continue after
				if (it->second.opcode() == RV32I_JALR ||
					it->second.opcode() == RV32I_JAL) {
					++it; break;
				}

				// we can accelerate these and continue
				if (gucci<W>(*it) == 0)
					break;
			}
			size_t length = it - block;
			if (length >= TRANSLATION_TRESHOLD && icounter + length < INSTRUCTIONS_MAX)
			{
				//printf("Block found. Length: %zu\n", length);
				const std::string func =
					"f" + std::to_string(dlmappings.size());
				emit(code, func, basepc, &*block, length);
				dlmappings.push_back({*block, func});
				icounter += length;
				// we can't translate beyond this estimate, otherwise
				// the compiler will never finish code generation
				if (dlmappings.size() >= TRANSLATIONS_MAX)
					break;
			}
			basepc += 4 * length;
		}
		else {
			++it;
			basepc += 4;
		}
	}
	// nothing to compile without mappings
	printf("Emitted %zu accelerated instructions and %zu functions!\n",
		icounter, dlmappings.size());
	if (dlmappings.empty())
		return;

	extern std::pair<std::string, void*> compile(const std::string& code, int arch);
	auto res = compile(code, W);
	void* dylib = res.second;
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
			.jump = [] (CPU<W>& cpu, address_type<W> addr) {
				cpu.jump(addr);
			},
			.increment_counter = [] (CPU<W>& cpu, uint64_t val) {
				cpu.machine().increment_counter(val);
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
		unlink(res.first.c_str());
		// close dylib when machine is destructed
		/*machine().add_destructor_callback(
			[dylib] {
				dlclose(dylib);
			});*/
	}
}
