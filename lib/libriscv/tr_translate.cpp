#include <bit>
#include <cmath>
#include <chrono>
#if defined(__MINGW32__) || defined(__MINGW64__) || defined(_MSC_VER)
#define YEP_IS_WINDOWS 1
#include "win32/dlfcn.h"
#else
#include <dlfcn.h>
#endif
#include <unistd.h>
#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "safe_instr_loader.hpp"
#include "tr_api.hpp"
#include "tr_types.hpp"
#include "util/crc32.hpp"
#ifdef RISCV_EXT_C
#include "rvc.hpp"
#endif

namespace riscv
{
	static constexpr bool VERBOSE_BLOCKS = false;
	static constexpr bool SCAN_FOR_GP = true;

	inline timespec time_now();
	inline long nanodiff(timespec, timespec);
	#define TIME_POINT(x) \
		[[maybe_unused]] timespec x;  \
		if (options.translate_timing) { \
			asm("" : : : "memory");   \
			x = time_now();           \
			asm("" : : : "memory");   \
		}
	extern void  dylib_close(void* dylib);
	extern void* dylib_lookup(void* dylib, const char*);

template <int W>
inline uint32_t opcode(const TransInstr<W>& ti) {
	return rv32i_instruction{ti.instr}.opcode();
}

template <int W>
inline DecoderData<W>& decoder_entry_at(const DecodedExecuteSegment<W>& exec, address_type<W> addr) {
	return exec.decoder_cache()[addr / DecoderCache<W>::DIVISOR];
}

template <int W>
static std::unordered_map<std::string, std::string> create_defines_for(const Machine<W>& machine, const MachineOptions<W>& options)
{
	// Calculate offset from Machine to each counter
	auto counters = const_cast<Machine<W>&> (machine).get_counters();
	const auto ins_counter_offset = uintptr_t(&counters.first) - uintptr_t(&machine);
	const auto max_counter_offset = uintptr_t(&counters.second) - uintptr_t(&machine);

	std::unordered_map<std::string, std::string> defines;
	defines.emplace("RISCV_TRANSLATION_DYLIB", std::to_string(W));
	defines.emplace("RISCV_MAX_SYSCALLS", std::to_string(RISCV_SYSCALLS_MAX));
	defines.emplace("RISCV_ARENA_END", std::to_string(machine.memory.memory_arena_size()));
	defines.emplace("RISCV_ARENA_ROEND", std::to_string(machine.memory.initial_rodata_end()));
	defines.emplace("RISCV_INS_COUNTER_OFF", std::to_string(ins_counter_offset));
	defines.emplace("RISCV_MAX_COUNTER_OFF", std::to_string(max_counter_offset));
	if constexpr (compressed_enabled) {
		defines.emplace("RISCV_EXT_C", "1");
	}
	if constexpr (vector_extension) {
		defines.emplace("RISCV_EXT_VECTOR", std::to_string(vector_extension));
	}
	if constexpr (nanboxing) {
		defines.emplace("RISCV_NANBOXING", "1");
	}
	if (options.translate_trace) {
		// Adding this as a define will change the hash of the translation,
		// so it will be recompiled if the trace option is toggled.
		defines.emplace("RISCV_TRACING", "1");
	}
	return defines;
}

template <int W>
int CPU<W>::load_translation(const MachineOptions<W>& options,
	std::string* filename, DecodedExecuteSegment<W>& exec) const
{
	// Binary translation using libtcc doesn't use files
	if constexpr (libtcc_enabled) {
		return 1;
	}

	// Disable translator with NO_TRANSLATE=1
	// or by setting max blocks to zero.
	if (0 == options.translate_blocks_max || getenv("NO_TRANSLATE")) {
		if (options.verbose_loader) {
			printf("libriscv: Binary translation disabled\n");
		}
		exec.set_binary_translated(nullptr);
		return -1;
	}
	if (exec.is_binary_translated()) {
		throw MachineException(ILLEGAL_OPERATION, "Execute segment already binary translated");
	}

	auto* exec_data = exec.exec_data(exec.exec_begin());

	// Checksum the execute segment + compiler flags
	TIME_POINT(t5);
	extern std::string compile_command(int arch, const std::unordered_map<std::string, std::string>& cflags);
	const auto cc = compile_command(W, create_defines_for(machine(), options));
	const uint32_t checksum =
		crc32c(exec_data, exec.exec_end() - exec.exec_begin())
		^ crc32c(cc.c_str(), cc.size());
	exec.set_translation_hash(checksum);

	char filebuffer[256];
	int len = snprintf(filebuffer, sizeof(filebuffer),
		"%s%08X%s", options.translation_prefix.c_str(), checksum, options.translation_suffix.c_str());
	if (len <= 0)
		return -1;

	void* dylib = nullptr;
	if (options.translate_timing) {
		TIME_POINT(t6);
		printf(">> Execute segment hashing took %ld ns\n", nanodiff(t5, t6));
	}

	// Always check if there is an existing file
	if (access(filebuffer, R_OK) == 0) {
		TIME_POINT(t7);
		dylib = dlopen(filebuffer, RTLD_LAZY);
		if (options.translate_timing) {
			TIME_POINT(t8);
			printf(">> dlopen took %ld ns\n", nanodiff(t7, t8));
		}
	}
	bool must_compile = dylib == nullptr;

	// If MinGW compilation is enabled, we should check for the PE-dll too
	if (options.mingw_options) {
		const uint32_t hash = checksum;
		const std::string mingw_filename = MachineMingWTranslationOptions::filename(
			options.mingw_options->mingw_cross_prefix, hash, options.mingw_options->mingw_cross_suffix);
		if (access(mingw_filename.c_str(), R_OK) != 0) {
			must_compile = true;
		}
	}

	// We must compile ourselves
	if (dylib == nullptr) {
		if (filename) *filename = std::string(filebuffer);
		return 1;
	}

	this->activate_dylib(options, exec, dylib);

	if (options.translate_timing) {
		TIME_POINT(t10);
		printf(">> Total binary translation loading time %ld ns\n", nanodiff(t5, t10));
	}

	// If the mingw PE-dll is not found, we must also compile (despite activating the ELF)
	if (must_compile) {
		if (filename) *filename = std::string(filebuffer);
		return 1;
	}
	return 0;
}

static bool is_stopping_instruction(rv32i_instruction instr) {
	if (instr.opcode() == RV32I_JALR || instr.whole == RV32_INSTR_STOP
		|| (instr.opcode() == RV32I_SYSTEM && instr.Itype.funct3 == 0 && instr.Itype.imm == 261) // WFI
	) return true;

#ifdef RISCV_EXT_C
	if (instr.is_compressed()) {
		#define CI_CODE(x, y) ((x << 13) | (y))
		const rv32c_instruction ci { instr };
		if (ci.opcode() == CI_CODE(0b100, 0b10)) { // VARIOUS
			if (ci.CR.rd != 0 && ci.CR.rs2 == 0) {
				return true; // C.JR and C.JALR (aka. RET)
			}
		}
	}
#endif

	return false;
}

template <int W>
void CPU<W>::try_translate(const MachineOptions<W>& options,
	const std::string& filename,
	DecodedExecuteSegment<W>& exec, address_t basepc, address_t endbasepc) const
{
	// Run with VERBOSE=1 to see command and output
	const bool verbose = options.verbose_loader;
	const bool trace_instructions = options.translate_trace;

#ifdef YEP_IS_WINDOWS
	// Windows users don't have C compilers laying around
	if (verbose) {
		printf("Binary translation not supported on Windows\n");
		printf("The translation filename is %s\n", filename.c_str());
	}
	// TODO: Check for MinGW and other compilers instead of just disabling
	return;
#endif

	address_t gp = 0;
	TIME_POINT(t0);
if constexpr (SCAN_FOR_GP) {
	// We assume that GP is initialized with AUIPC,
	// followed by OP_IMM (and maybe OP_IMM32)
	for (address_t pc = basepc; pc < endbasepc; ) {
		const rv32i_instruction instruction
			= read_instruction(exec.exec_data(), pc, endbasepc);
		if (instruction.opcode() == RV32I_AUIPC) {
			const auto auipc = instruction;
			if (auipc.Utype.rd == 3) { // GP
				const rv32i_instruction addi
					= read_instruction(exec.exec_data(), pc + 4, endbasepc);
				if (addi.opcode() == RV32I_OP_IMM && addi.Itype.funct3 == 0x0) {
					//printf("Found OP_IMM: ADDI  rd=%d, rs1=%d\n", addi.Itype.rd, addi.Itype.rs1);
					if (addi.Itype.rd == 3 && addi.Itype.rs1 == 3) { // GP
						gp = pc + auipc.Utype.upper_imm() + addi.Itype.signed_imm();
						break;
					}
				} else {
					gp = pc + auipc.Utype.upper_imm();
					break;
				}
			}
		} // opcode

		pc += instruction.length();
	} // iterator
	if (options.translate_timing) {
		TIME_POINT(t1);
		printf(">> GP scan took %ld ns, GP=0x%lX\n", nanodiff(t0, t1), (long)gp);
	}
} // SCAN_FOR_GP

	// Code block and loop detection
	TIME_POINT(t2);
	size_t icounter = 0;
	std::unordered_set<address_type<W>> global_jump_locations;
	std::vector<TransInfo<W>> blocks;

	// Insert the ELF entry point as the first global jump location
	const auto elf_entry = machine().memory.start_address();
	if (elf_entry >= basepc && elf_entry < endbasepc)
		global_jump_locations.insert(elf_entry);

	for (address_t pc = basepc; pc < endbasepc && icounter < options.translate_instr_max; )
	{
		const auto block = pc;
		std::size_t block_insns = 0;

		for (; pc < endbasepc; ) {
			const rv32i_instruction instruction
				= read_instruction(exec.exec_data(), pc, endbasepc);
			pc += instruction.length();
			block_insns++;

			// JALR and STOP are show-stoppers / code-block enders
			if (is_stopping_instruction(instruction)) {
				break;
			}
		}

		auto block_end = pc;
		std::unordered_set<address_t> jump_locations;
		std::vector<rv32i_instruction> block_instructions;
		block_instructions.reserve(block_insns);

		// Find jump locations inside block
		for (pc = block; pc < block_end; ) {
			const rv32i_instruction instruction
				= read_instruction(exec.exec_data(), pc, endbasepc);
			const auto opcode = instruction.opcode();
			bool is_jal = false;
			bool is_branch = false;

			address_t location = 0;
			if (opcode == RV32I_JAL) {
				is_jal = true;
				const auto offset = instruction.Jtype.jump_offset();
				location = pc + offset;
			} else if (opcode == RV32I_BRANCH) {
				is_branch = true;
				const auto offset = instruction.Btype.signed_imm();
				location = pc + offset;
			}
#ifdef RISCV_EXT_C
			else if (instruction.is_compressed())
			{
				const rv32c_instruction ci { instruction };

				// Find branch and jump locations
				if (W == 4 && ci.opcode() == CI_CODE(0b001, 0b01)) { // C.JAL
					is_jal = true;
					const int32_t imm = ci.CJ.signed_imm();
					location = pc + imm;
				}
				else if (ci.opcode() == CI_CODE(0b101, 0b01)) { // C.JMP
					is_jal = true;
					const int32_t imm = ci.CJ.signed_imm();
					location = pc + imm;
				}
				else if (ci.opcode() == CI_CODE(0b110, 0b01)) { // C.BEQZ
					is_branch = true;
					const int32_t imm = ci.CB.signed_imm();
					location = pc + imm;
				}
				else if (ci.opcode() == CI_CODE(0b111, 0b01)) { // C.BNEZ
					is_branch = true;
					const int32_t imm = ci.CB.signed_imm();
					location = pc + imm;
				}
			}
#endif

			// detect far JAL, otherwise use as local jump
			if (is_jal) {
				// All JAL target addresses need to be recorded in order
				// to detect function calls
				global_jump_locations.insert(location);

				// Long jumps are considered returnable
				if (location < block || location >= block_end) {
					block_instructions.push_back(instruction);
					pc += instruction.length();
					block_end = pc;
					break;
				}
				if (location >= block && location < block_end)
					jump_locations.insert(location);
			}
			// loop detection (negative branch offsets)
			else if (is_branch) {
				// only accept branches relative to current block
				if (location >= block && location < block_end)
					jump_locations.insert(location);
			}

			// Add instruction to block
			block_instructions.push_back(instruction);
			pc += instruction.length();
		} // process block

		// Process block and add it for emission
		const size_t length = block_instructions.size();
		if (length >= options.block_size_treshold
			&& icounter + length < options.translate_instr_max)
		{
			if constexpr (VERBOSE_BLOCKS) {
				printf("Block found at %#lX -> %#lX. Length: %zu\n", long(block), long(block_end), length);
				for (auto loc : jump_locations)
					printf("-> Jump to %#lX\n", long(loc));
			}

			blocks.push_back({
				std::move(block_instructions), block, block_end, gp,
				trace_instructions,
				true,
				std::move(jump_locations),
				nullptr, // blocks
				global_jump_locations
			});
			icounter += length;
			// we can't translate beyond this estimate, otherwise
			// the compiler will never finish code generation
			if (blocks.size() >= options.translate_blocks_max)
				break;
		}

		pc = block_end;
	}

	TIME_POINT(t3);
	if (options.translate_timing) {
		printf(">> Code block detection %ld ns\n", nanodiff(t2, t3));
	}

	// Code generation
	std::vector<TransMapping<W>> dlmappings;
	extern const std::string bintr_code;
	std::string code = bintr_code;

	for (auto& block : blocks)
	{
		block.blocks = &blocks;
		auto result = emit(code, block);

		for (auto& mapping : result) {
			dlmappings.push_back(std::move(mapping));
		}
	}
	// Append all instruction handler -> dl function mappings
	code += "const uint32_t no_mappings = "
		+ std::to_string(dlmappings.size()) + ";\n";
	code += R"V0G0N(
struct Mapping {
	addr_t addr;
	ReturnValues (*handler)(CPU*, uint64_t, uint64_t, addr_t);
};
const struct Mapping mappings[] = {
)V0G0N";
	for (const auto& mapping : dlmappings)
	{
		char buffer[128];
		snprintf(buffer, sizeof(buffer), 
			"{0x%lX, %s},\n",
			(long)mapping.addr, mapping.symbol.c_str());
		code.append(buffer);
	}
	code += "};\n";

	if (options.translate_timing) {
		TIME_POINT(t4);
		printf(">> Code generation took %ld ns\n", nanodiff(t3, t4));
	}

	if (verbose) {
		printf("Emitted %zu accelerated instructions and %zu functions. GP=0x%lX\n",
			icounter, dlmappings.size(), (long) gp);
	}
	// nothing to compile without mappings
	if (dlmappings.empty()) {
		if (verbose) {
			printf("Binary translator has nothing to compile! No mappings.\n");
		}
		return;
	}

	const auto defines = create_defines_for(machine(), options);
	void* dylib = nullptr;

	TIME_POINT(t9);
	if constexpr (libtcc_enabled) {
		extern void* libtcc_compile(const std::string& code, int arch, const std::unordered_map<std::string, std::string>& defines, const std::string&);
		dylib = libtcc_compile(code, W, defines, options.libtcc1_location);

	} else {
#ifdef YEP_IS_WINDOWS
		// Windows users don't have C compilers laying around
		dylib = nullptr;
		printf("Binary translation not supported on Windows\n");
		printf("The translation filename is %s\n", filename.c_str());
#else
		extern void* compile(const std::string& code, int arch, const std::unordered_map<std::string, std::string>& defines, const std::string&);
		extern bool mingw_compile(const std::string& code, int arch, const std::unordered_map<std::string, std::string>& defines, const std::string&, const MachineMingWTranslationOptions&);

		// If the binary translation has already been loaded, we can skip compilation
		if (exec.is_binary_translated()) {
			dylib = exec.binary_translation_so();
		} else {
			dylib = compile(code, W, defines, filename);
		}

		// Optionally produce a mingw PE-dll for Windows
		// This is a secondary binary that can be loaded on Windows machines.
		if (options.mingw_options) {
			const uint32_t hash = exec.translation_hash();
			const std::string mingw_filename = MachineMingWTranslationOptions::filename(
				options.mingw_options->mingw_cross_prefix, hash, options.mingw_options->mingw_cross_suffix);
			mingw_compile(code, W, defines, mingw_filename, *options.mingw_options);
		}
#endif
	}
	if (options.translate_timing) {
		TIME_POINT(t10);
		printf(">> Code compilation took %.2f ms\n", nanodiff(t9, t10) / 1e6);
	}

	// Check compilation result
	if (dylib == nullptr) {
		return;
	}

	if (!exec.is_binary_translated()) {
		this->activate_dylib(options, exec, dylib);
	}

	if constexpr (!libtcc_enabled) {
		if (!options.translation_cache) {
			// Delete the program if the shared ELF is unwanted
			unlink(filename.c_str());
		}
	}
	if (options.translate_timing) {
		TIME_POINT(t12);
		printf(">> Binary translation totals %.2f ms\n", nanodiff(t0, t12) / 1e6);
	}
}

template <int W>
void CPU<W>::activate_dylib(const MachineOptions<W>& options, DecodedExecuteSegment<W>& exec, void* dylib) const
{
	TIME_POINT(t11);

	if (!initialize_translated_segment(exec, dylib))
	{
		if constexpr (!libtcc_enabled) {
			// only warn when translation is not already disabled
			if (getenv("NO_TRANSLATE") == nullptr) {
				fprintf(stderr, "libriscv: Could not find dylib init function\n");
			}
		}
		dylib_close(dylib);
		exec.set_binary_translated(nullptr);
		return;
	}

	// Map all the functions to instruction handlers
	uint32_t* no_mappings = (uint32_t *)dylib_lookup(dylib, "no_mappings");
	struct Mapping {
		address_t addr;
		bintr_block_func<W> handler;
	};
	Mapping* mappings = (Mapping *)dylib_lookup(dylib, "mappings");

	if (no_mappings == nullptr || mappings == nullptr || *no_mappings > 500000UL) {
		dylib_close(dylib);
		exec.set_binary_translated(nullptr);
		throw MachineException(INVALID_PROGRAM, "Invalid mappings in binary translation program");
	}

	// After this, we should automatically close the dylib on destruction
	exec.set_binary_translated(dylib);

	// Apply mappings to decoder cache
	const auto nmappings = *no_mappings;
	exec.reserve_mappings(nmappings);
	for (size_t i = 0; i < nmappings; i++) {
		exec.add_mapping(mappings[i].handler);
		const auto addr = mappings[i].addr;
		if (exec.is_within(addr)) {
			auto& entry = decoder_entry_at(exec, addr);
			if (mappings[i].handler != nullptr) {
				entry.instr = i;
				entry.set_bytecode(CPU<W>::computed_index_for(RV32_INSTR_BLOCK_END));
			} else {
				entry.set_bytecode(0x0); /* Invalid opcode */
			}
		} else {
			throw MachineException(INVALID_PROGRAM, "Translation mapping outside execute area", addr);
		}
	}

	if (options.translate_timing) {
		TIME_POINT(t12);
		printf(">> Binary translation activation %ld ns\n", nanodiff(t11, t12));
	}
}

template <int W>
bool CPU<W>::initialize_translated_segment(DecodedExecuteSegment<W>&, void* dylib) const
{
	// NOTE: At some point this must be able to duplicate the dylib
	// in order to be able to share execute segments across machines.

	auto* ptr = dylib_lookup(dylib, "init"); // init() function
	if (ptr == nullptr) {
		return false;
	}

	// Map the API callback table
	auto func = (void (*)(const CallbackTable<W>&, void*)) ptr;
	func(CallbackTable<W>{
		.mem_read = [] (CPU<W>& cpu, address_type<W> addr) -> const void* {
			return cpu.machine().memory.cached_readable_page(addr << 12, 1).buffer8.data();
		},
		.mem_write = [] (CPU<W>& cpu, address_type<W> addr) -> void* {
			return cpu.machine().memory.cached_writable_page(addr << 12).buffer8.data();
		},
		.vec_load = [] (CPU<W>& cpu, int vd, address_type<W> addr) {
#ifdef RISCV_EXT_VECTOR
			auto& rvv = cpu.registers().rvv();
			rvv.get(vd) = cpu.machine().memory.template read<VectorLane> (addr);
#else
			(void)cpu; (void)addr; (void)vd;
#endif
		},
		.vec_store = [] (CPU<W>& cpu, address_type<W> addr, int vd) {
#ifdef RISCV_EXT_VECTOR
			auto& rvv = cpu.registers().rvv();
			cpu.machine().memory.template write<VectorLane> (addr, rvv.get(vd));
#else
			(void)cpu; (void)addr; (void)vd;
#endif
		},
		.syscalls = machine().syscall_handlers.data(),
		.unknown_syscall = [] (CPU<W>& cpu, address_type<W> sysno) {
			cpu.machine().on_unhandled_syscall(cpu.machine(), sysno);
		},
		.system = [] (CPU<W>& cpu, uint32_t instr) {
			cpu.machine().system(rv32i_instruction{instr});
		},
		.execute = [] (CPU<W>& cpu, uint32_t instr) {
			const rv32i_instruction rvi{instr};
			cpu.decode(rvi).handler(cpu, rvi);
		},
		.trigger_exception = [] (CPU<W>& cpu, int e) {
			cpu.trigger_exception(e);
		},
		.trace = [] (CPU<W>& cpu, const char* msg, address_type<W> addr, uint32_t instr) {
			(void)cpu;
			printf("f %s pc 0x%lX instr %08X\n", msg, (long)addr, instr);
		},
		.sqrtf32 = [] (float f) -> float {
			return std::sqrt(f);
		},
		.sqrtf64 = [] (double d) -> double {
			return std::sqrt(d);
		},
		.clz = [] (uint32_t x) -> int {
			return std::countl_zero(x);
		},
		.clzl = [] (uint64_t x) -> int {
			return std::countl_zero(x);
		},
		.ctz = [] (uint32_t x) -> int {
			return std::countr_zero(x);
		},
		.ctzl = [] (uint64_t x) -> int {
			return std::countr_zero(x);
		},
		.cpop = [] (uint32_t x) -> int {
			return std::popcount(x);
		},
		.cpopl = [] (uint64_t x) -> int {
			return std::popcount(x);
		},
	},
	m_machine.memory.memory_arena_ptr());

	return true;
}

#ifdef RISCV_32I
	template void CPU<4>::try_translate(const MachineOptions<4>&, const std::string&, DecodedExecuteSegment<4>&, address_t, address_t) const;
	template int CPU<4>::load_translation(const MachineOptions<4>&, std::string*, DecodedExecuteSegment<4>&) const;
	template bool CPU<4>::initialize_translated_segment(DecodedExecuteSegment<4>&, void* dylib) const;
	template void CPU<4>::activate_dylib(const MachineOptions<4>&, DecodedExecuteSegment<4>&, void*) const;
#endif
#ifdef RISCV_64I
	template void CPU<8>::try_translate(const MachineOptions<8>&, const std::string&, DecodedExecuteSegment<8>&, address_t, address_t) const;
	template int CPU<8>::load_translation(const MachineOptions<8>&, std::string*, DecodedExecuteSegment<8>&) const;
	template bool CPU<8>::initialize_translated_segment(DecodedExecuteSegment<8>&, void* dylib) const;
	template void CPU<8>::activate_dylib(const MachineOptions<8>&, DecodedExecuteSegment<8>&, void*) const;
#endif

	timespec time_now()
	{
		timespec t;
#ifdef YEP_IS_WINDOWS
		std::chrono::nanoseconds ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch());
		t.tv_sec  = ns.count() / 1000000000;
		t.tv_nsec = ns.count() % 1000000000;
#else
		clock_gettime(CLOCK_MONOTONIC, &t);
#endif
		return t;
	}
	long nanodiff(timespec start_time, timespec end_time)
	{
		return (end_time.tv_sec - start_time.tv_sec) * (long)1e9 + (end_time.tv_nsec - start_time.tv_nsec);
	}
} // riscv
