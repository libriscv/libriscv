static constexpr int TRANSLATION_TRESHOLD = 8;
}
#include <EASTL/hash_set.h>
namespace riscv {

static eastl::hash_set<uint32_t> good_insn
{
	RV32I_LOAD,
	RV32I_STORE,
	RV32I_OP_IMM,
	RV32I_OP,
	RV32I_LUI,
	RV32I_AUIPC,
	RV32I_FENCE,
	//RV64I_OP_IMM32,
	//RV64I_OP32,
	//RV32F_LOAD,
	//RV32F_STORE,
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
	std::string code =
		"#define RISCV_TRANSLATION_DYLIB " + std::to_string(W) + "\n"
		"#include <libriscv/tr_api.hpp>\n"
		"#include <libriscv/rv32i_instr.hpp>\n"
		"#include <libriscv/rv32i.hpp>\n"
		"#include <libriscv/rv64i.hpp>\n"
		"using namespace riscv;\n\n"
		"static CallbackTable api;\n\n"
		"extern \"C\"\n"
		"void init(const CallbackTable& table) {\n"
		"	api = table;\n"
		"}\n\n";

	size_t icounter = 0;
	auto it = ipairs.begin();
	while (it != ipairs.end())
	{
		auto block = it;
		if (gucci<W>(*block))
		{
			// measure block length
			while (++it != ipairs.end()) {
				// we can include this but not continue after
				if (it->second.opcode() == RV32I_JALR ||
					it->second.opcode() == RV32I_JAL
					//(it->second.opcode() == RV32I_BRANCH && it->first == DECODED_INSTR(BRANCH_NE).handler)
				) {
					++it; break;
				}
				// we can accelerate these and continue
				if (gucci<W>(*it) == 0)
					break;
			}
			size_t length = it - block;
			if (length >= TRANSLATION_TRESHOLD)
			{
				//printf("Block found. Length: %zu\n", length);
				const std::string func =
					"func" + std::to_string(dlmappings.size());
				emit(code, func, basepc, &*block, length);
				dlmappings.push_back({*block, func});
				icounter += length;
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

	extern std::pair<std::string, void*> compile(const std::string& code);
	//printf("Code:\n%s\n", code.c_str());
	auto res = compile(code);
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
