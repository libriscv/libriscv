static eastl::hash_set<uint32_t> good_insn
{
	RV32I_LOAD,
	RV32I_STORE,
	RV32I_OP_IMM,
	RV32I_OP,
	RV32I_LUI,
	RV32I_AUIPC,
	//RV64I_OP_IMM32,
	//RV64I_OP32,
};

template <int W>
inline uint32_t opcode(const typename CPU<W>::instr_pair& ip) {
	return ip.second.opcode();
}
template <int W>
inline bool gucci(const typename CPU<W>::instr_pair& ip) {
	return good_insn.count(opcode<W>(ip)) > 0;
}

template <typename ... Args>
inline void add_code(std::string& code, rv32i_instruction instr, Args&& ... addendum) {
	code += "{\nconstexpr rv32i_instruction instr {" + std::to_string(instr.whole) + "u};\n";
	([&] {
		code += "\t" + std::string(addendum) + "\n";
	}(), ...);
	code += "}\n";
}
#define IS_HANDLER(ip, instr) ((ip).first == DECODED_INSTR(instr).handler)

template <int W>
void CPU<W>::emit(std::string& code, const std::string& func, instr_pair* ip, size_t len) const
{
	code += "extern \"C\" void " + func + "(CPU<" + std::to_string(W) + ">& cpu, rv32i_instruction) {\n";
	for (size_t i = 0; i < len; i++) {
		const auto& instr = ip[i].second;
		if (IS_HANDLER(ip[i], LOAD_I8_DUMMY)) {
			add_code(code, instr,
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"api.mem_read8(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I8)) {
			add_code(code, instr,
				"auto& reg = cpu.reg(instr.Itype.rd);",
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"reg = (RVSIGNTYPE(cpu)) (int8_t) api.mem_read8(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I16_DUMMY)) {
			add_code(code, instr,
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"api.mem_read16(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I16)) {
			add_code(code, instr,
				"auto& reg = cpu.reg(instr.Itype.rd);",
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"reg = (RVSIGNTYPE(cpu)) (int16_t) api.mem_read16(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I32_DUMMY)) {
			add_code(code, instr,
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"api.mem_read32(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I32)) {
			add_code(code, instr,
				"auto& reg = cpu.reg(instr.Itype.rd);",
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"reg = (RVSIGNTYPE(cpu)) (int32_t) api.mem_read32(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_U8)) {
			add_code(code, instr,
				"auto& reg = cpu.reg(instr.Itype.rd);",
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"reg = api.mem_read8(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_U16)) {
			add_code(code, instr,
				"auto& reg = cpu.reg(instr.Itype.rd);",
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"reg = api.mem_read16(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_U32)) {
			add_code(code, instr,
				"auto& reg = cpu.reg(instr.Itype.rd);",
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"reg = api.mem_read32(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_U64)) {
			add_code(code, instr,
				"auto& reg = cpu.reg(instr.Itype.rd);",
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"reg = api.mem_read64(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_U64_DUMMY)) {
			add_code(code, instr,
				"const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();",
				"api.mem_read64(cpu, addr);"
			);
		}
		else if (IS_HANDLER(ip[i], STORE_I8) || IS_HANDLER(ip[i], STORE_I8_IMM)) {
			add_code(code, instr,
				"const auto& value = cpu.reg(instr.Stype.rs2);",
				"const auto addr  = cpu.reg(instr.Stype.rs1) + instr.Stype.signed_imm();",
				"api.mem_write8(cpu, addr, value);"
			);
		}
		else if (IS_HANDLER(ip[i], STORE_I16_IMM)) {
			add_code(code, instr,
				"const auto& value = cpu.reg(instr.Stype.rs2);",
				"const auto addr  = cpu.reg(instr.Stype.rs1) + instr.Stype.signed_imm();",
				"api.mem_write16(cpu, addr, value);"
			);
		}
		else if (IS_HANDLER(ip[i], STORE_I32_IMM)) {
			add_code(code, instr,
				"const auto& value = cpu.reg(instr.Stype.rs2);",
				"const auto addr  = cpu.reg(instr.Stype.rs1) + instr.Stype.signed_imm();",
				"api.mem_write32(cpu, addr, value);"
			);
		}
		else if (IS_HANDLER(ip[i], STORE_I64_IMM)) {
			add_code(code, instr,
				"const auto& value = cpu.reg(instr.Stype.rs2);",
				"const auto addr  = cpu.reg(instr.Stype.rs1) + instr.Stype.signed_imm();",
				"api.mem_write64(cpu, addr, value);"
			);
		}
		else if (IS_HANDLER(ip[i], OP_IMM)
			|| IS_HANDLER(ip[i], OP_IMM_ADDI)
			|| IS_HANDLER(ip[i], OP_IMM_ORI)
			|| IS_HANDLER(ip[i], OP_IMM_ANDI)
			|| IS_HANDLER(ip[i], OP_IMM_LI)
			|| IS_HANDLER(ip[i], OP_IMM_SLLI)) {
			add_code(code, instr,
				R"V0G0N(
				auto& dst = cpu.reg(instr.Itype.rd);
				const auto src = cpu.reg(instr.Itype.rs1);
				switch (instr.Itype.funct3) {
				case 0x0: // ADDI
					dst = src + instr.Itype.signed_imm();
					break;
				case 0x1: // SLLI
					// SLLI: Logical left-shift 5/6-bit immediate
					if constexpr (RVIS64BIT(cpu))
						dst = src << instr.Itype.shift64_imm();
					else
						dst = src << instr.Itype.shift_imm();
					break;
				case 0x2: // SLTI:
					dst = (RVTOSIGNED(src) < instr.Itype.signed_imm()) ? 1 : 0;
					break;
				case 0x3: // SLTU:
					dst = (src < (unsigned) instr.Itype.signed_imm()) ? 1 : 0;
					break;
				case 0x4: // XORI:
					dst = src ^ instr.Itype.signed_imm();
					break;
				case 0x5: // SRLI / SRAI:
					if (LIKELY(!instr.Itype.is_srai())) {
						if constexpr (RVIS64BIT(cpu))
							dst = src >> instr.Itype.shift64_imm();
						else
							dst = src >> instr.Itype.shift_imm();
					} else { // SRAI: preserve the sign bit
						constexpr auto bit = 1ul << (sizeof(src) * 8 - 1);
						const bool is_signed = (src & bit) != 0;
						if constexpr (RVIS64BIT(cpu)) {
							const uint32_t shifts = instr.Itype.shift64_imm();
							dst = RV64I::SRA(is_signed, shifts, src);
						} else {
							const uint32_t shifts = instr.Itype.shift_imm();
							dst = RV32I::SRA(is_signed, shifts, src);
						}
					}
					break;
				case 0x6: // ORI
					dst = src | instr.Itype.signed_imm();
					break;
				case 0x7: // ANDI
					dst = src & instr.Itype.signed_imm();
					break;
				})V0G0N"
			);
		}
		else if (IS_HANDLER(ip[i], OP)
			|| IS_HANDLER(ip[i], OP_ADD)
			|| IS_HANDLER(ip[i], OP_SUB)) {
			add_code(code, instr,
				R"V0G0N(
					auto& dst = cpu.reg(instr.Rtype.rd);
					const auto src1 = cpu.reg(instr.Rtype.rs1);
					const auto src2 = cpu.reg(instr.Rtype.rs2);

					switch (instr.Rtype.jumptable_friendly_op()) {
					case 0x0: // ADD / SUB
						dst = src1 + (!instr.Rtype.is_f7() ? src2 : -src2);
						break;
					case 0x1: // SLL
						if constexpr (RVIS64BIT(cpu)) {
							dst = src1 << (src2 & 0x3F);
						} else {
							dst = src1 << (src2 & 0x1F);
						}
						break;
					case 0x2: // SLT
						dst = (RVTOSIGNED(src1) < RVTOSIGNED(src2)) ? 1 : 0;
						break;
					case 0x3: // SLTU
						dst = (src1 < src2) ? 1 : 0;
						break;
					case 0x4: // XOR
						dst = src1 ^ src2;
						break;
					case 0x5: // SRL / SRA
						if (!instr.Rtype.is_f7()) { // SRL
							if constexpr (RVIS64BIT(cpu)) {
								dst = src1 >> (src2 & 0x3F); // max 63 shifts!
							} else {
								dst = src1 >> (src2 & 0x1F); // max 31 shifts!
							}
						} else { // SRA
							constexpr auto bit = 1ul << (sizeof(src1) * 8 - 1);
							const bool is_signed = (src1 & bit) != 0;
							if constexpr (RVIS64BIT(cpu)) {
								const uint32_t shifts = src2 & 0x3F; // max 63 shifts!
								dst = RV64I::SRA(is_signed, shifts, src1);
							} else {
								const uint32_t shifts = src2 & 0x1F; // max 31 shifts!
								dst = RV32I::SRA(is_signed, shifts, src1);
							}
						}
						break;
					case 0x6: // OR
						dst = src1 | src2;
						break;
					case 0x7: // AND
						dst = src1 & src2;
						break;
					// extension RV32M / RV64M
					case 0x10: // MUL
						dst = RVTOSIGNED(src1) * RVTOSIGNED(src2);
						break;
					case 0x11: // MULH (signed x signed)
						)V0G0N",
							((W == 4) ?
							"dst = ((int64_t) src1 * (int64_t) src2) >> 32u;" :
							"RV64I::MUL128(dst, src1, src2);"),
						R"V0G0N(
						break;
					case 0x12: // MULHSU (signed x unsigned)
						)V0G0N",
						((W == 4) ?
							"dst = ((int64_t) src1 * (uint64_t) src2) >> 32u;" :
							"RV64I::MUL128(dst, src1, src2);"),
						R"V0G0N(
						break;
					case 0x13: // MULHU (unsigned x unsigned)
						)V0G0N",
						((W == 4) ?
							"dst = ((uint64_t) src1 * (uint64_t) src2) >> 32u;" :
							"RV64I::MUL128(dst, src1, src2);"),
						R"V0G0N(
						break;
					case 0x14: // DIV
						// division by zero is not an exception
						if (LIKELY(RVTOSIGNED(src2) != 0)) {
							if constexpr (RVIS64BIT(cpu)) {
								// vi_instr.cpp:444:2: runtime error:
								// division of -9223372036854775808 by -1 cannot be represented in type 'long'
								if (LIKELY(!(src1 == -9223372036854775808ull && src2 == -1ull)))
									dst = RVTOSIGNED(src1) / RVTOSIGNED(src2);
							} else {
								// rv32i_instr.cpp:301:2: runtime error:
								// division of -2147483648 by -1 cannot be represented in type 'int'
								if (LIKELY(!(src1 == 2147483648 && src2 == 4294967295)))
									dst = RVTOSIGNED(src1) / RVTOSIGNED(src2);
							}
						}
						break;
					case 0x15: // DIVU
						if (LIKELY(src2 != 0)) dst = src1 / src2;
						break;
					case 0x16: // REM
						if (LIKELY(src2 != 0)) {
							if constexpr (RVIS64BIT(cpu)) {
								if (LIKELY(!(src1 == -9223372036854775808ull && src2 == -1ull)))
									dst = RVTOSIGNED(src1) % RVTOSIGNED(src2);
							} else {
							if (LIKELY(!(src1 == 2147483648 && src2 == 4294967295)))
								dst = RVTOSIGNED(src1) % RVTOSIGNED(src2);
							}
						}
						break;
					case 0x17: // REMU
						if (LIKELY(src2 != 0)) {
							dst = src1 % src2;
						}
						break;
				})V0G0N"
			);
		}
		else if (IS_HANDLER(ip[i], LUI)) {
			add_code(code, instr,
				"cpu.reg(instr.Utype.rd) = (int32_t) instr.Utype.upper_imm();"
			);
		}
		else if (IS_HANDLER(ip[i], AUIPC)) {
			add_code(code, instr,
				"cpu.reg(instr.Utype.rd) = cpu.pc() + instr.Utype.upper_imm();"
			);
		}
		else {
			throw std::runtime_error("Unhandled instruction in code emitter");
		}
	}
	code +=
		"\tcpu.machine().increment_counter(" + std::to_string(len) + ");\n"
		"\tcpu.increment_pc(" + std::to_string(4 * (len-1)) + ");\n"
		"}\n\n";
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
	std::vector<instr_pair>& ipairs) const
{
	std::vector<instr_pair*> candidates;
	std::vector<NamedIPair<W>> dlmappings;
	std::string code =
		"#include <libriscv/machine.hpp>\n"
		"#include <libriscv/instr_helpers.hpp>\n"
		"#include <libriscv/rv32i_instr.hpp>\n"
		"#include <libriscv/rv32i.hpp>\n"
		"#include <libriscv/rv64i.hpp>\n"
		"#include <libriscv/tr_api.hpp>\n"
		"using namespace riscv;\n\n"
		"static CallbackTable<" + std::to_string(W) + "> api;\n\n"
		"extern \"C\"\n"
		"void init(const CallbackTable<" + std::to_string(W) + ">& table) {\n"
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
				if (gucci<W>(*it) == 0)
					break;
			}
			size_t length = it - block;
			//printf("Block found. Length: %zu\n", length);
			const std::string func =
				"func" + std::to_string(dlmappings.size());
			emit(code, func, &*block, length);
			dlmappings.push_back({*block, func});
			icounter += length;
		}
		else {
			++it;
		}
	}
	// nothing to compile without mappings
	printf("Emitted %zu accelerated instructions!\n", icounter);
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
