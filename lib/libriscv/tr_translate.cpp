#if __has_include(<bit>)
# include <bit>
# if defined(__cpp_lib_bitops)
#  define RISCV_HAS_BITOPS
# endif
#endif
#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#if defined(__MINGW32__) || defined(__MINGW64__) || defined(_MSC_VER)
# define YEP_IS_WINDOWS 1
# include "win32/dlfcn.h"
# ifdef _MSC_VER
#  define access _access
#  define unlink _unlink
extern "C" int access(const char* path, int mode);
extern "C" int unlink(const char* path);
#  define R_OK   4       /* Test for read permission.  */
# else // _MSC_VER
#  include <unistd.h>
# endif
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "internal_common.hpp"
#include "safe_instr_loader.hpp"
#include "threaded_bytecodes.hpp"
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

	static inline timespec time_now();
	static inline long nanodiff(timespec, timespec);
	#define TIME_POINT(x) \
		[[maybe_unused]] timespec x;  \
		if (options.translate_timing) { \
			asm("" : : : "memory");   \
			x = time_now();           \
			asm("" : : : "memory");   \
		}
	extern void  dylib_close(void* dylib, bool is_libtcc);
	extern void* dylib_lookup(void* dylib, const char*, bool is_libtcc);

	template <int W>
	using binary_translation_init_func = void (*)(const CallbackTable<W>&, int32_t, int32_t, int32_t);
	template <int W>
	static CallbackTable<W> create_bintr_callback_table(DecodedExecuteSegment<W>&);

	// Translations that are embeddable in the binary will be added as a source
	// file directly in the project, which allows it to run global constructors.
	// The constructor will register the translation with the binary translator,
	// and we can check against this list when loading translations.
	template <int W>
	struct Mapping {
		address_type<W> addr;
		unsigned mapping_index;
	};

	// This implementation is designed to make sure it's not a global constructor
	// instead it will get zeroed from BSS
	static constexpr size_t MAX_EMBEDDED = 12;
	template <int W>
	struct EmbeddedTranslation {
		uint32_t    hash = 0;
		uint32_t    nmappings = 0;
		uint32_t    nhandlers = 0;
		const Mapping<W>* mappings = nullptr;
		const bintr_block_func<W>* handlers = nullptr;
		// NOTE: Pointer to the init function of the translation
		binary_translation_init_func<W> init_func = nullptr;
	};
	template <int W>
	struct EmbeddedTranslations {
		std::array<EmbeddedTranslation<W>, MAX_EMBEDDED> translations;
		size_t count = 0;
	};
	template <int W>
	static EmbeddedTranslations<W> registered_embedded_translations;

	template <int W>
	static EmbeddedTranslation<W>* find_embedded_translation_by_hash(uint32_t hash)
	{
		for (size_t i = 0; i < registered_embedded_translations<W>.count; ++i) {
			auto& translation = registered_embedded_translations<W>.translations[i];
			if (translation.hash == hash) {
				return &translation;
			}
		}
		return nullptr;
	}

	template <int W>
	static void register_translation(uint32_t hash, const Mapping<W>* mappings, uint32_t nmappings,
		const bintr_block_func<W>* handlers, uint32_t nhandlers, binary_translation_init_func<W> init_func)
	{
		static std::mutex translation_mutex;
		std::scoped_lock lock(translation_mutex);

		EmbeddedTranslations<W>& translations = registered_embedded_translations<W>;
		EmbeddedTranslation<W>* existing = find_embedded_translation_by_hash<W>(hash);
		if (existing == nullptr) {
			if (translations.count >= MAX_EMBEDDED) {
				throw MachineException(INVALID_PROGRAM, "Too many embedded translations", MAX_EMBEDDED);
			}
			// We allow overwriting existing translations with the same hash
			existing = &translations.translations[translations.count++];
		}
		existing->hash = hash;
		existing->nmappings = nmappings;
		existing->mappings  = mappings;
		existing->nhandlers = nhandlers;
		existing->handlers  = handlers;
		existing->init_func = init_func;

		if (getenv("VERBOSE")) {
			printf("libriscv: Registered embedded translation for hash %08X, %u/%u mappings\n",
				hash, nhandlers, nmappings);
		}
	}

	static std::string defines_to_string(const std::unordered_map<std::string, std::string>& cflags)
	{
		std::vector<std::string> cflags_str;
		for (auto pair : cflags) {
			cflags_str.push_back(" -D" + pair.first + "=" + pair.second);
		}
		// Sort the flags to make sure the hash is consistent
		std::sort(cflags_str.begin(), cflags_str.end());

		std::string defstr;
		for (auto& str : cflags_str) {
			defstr += str;
		}
		return defstr;
	}

template <int W>
inline uint32_t opcode(const TransInstr<W>& ti) {
	return rv32i_instruction{ti.instr}.opcode();
}

template <int W>
inline DecoderData<W>& decoder_entry_at(DecoderData<W>* cache, address_type<W> addr) {
	return cache[addr / DecoderCache<W>::DIVISOR];
}

template <int W>
static std::unordered_map<std::string, std::string> create_defines_for(const Machine<W>& machine, const MachineOptions<W>& options)
{
	// Some executables are loaded at high-memory addresses, which is outside of the memory arena.
	size_t arena_end                   = machine.memory.memory_arena_size();
	address_type<W> initial_rodata_end = machine.memory.initial_rodata_end();
	if (!options.translation_use_arena) {
		initial_rodata_end = 0;
		arena_end = 0x1000;
	}

	std::unordered_map<std::string, std::string> defines;
	defines.emplace("RISCV_TRANSLATION_DYLIB", std::to_string(W));
	defines.emplace("RISCV_MAX_SYSCALLS", std::to_string(RISCV_SYSCALLS_MAX));
	defines.emplace("RISCV_MACHINE_ALIGNMENT", std::to_string(RISCV_MACHINE_ALIGNMENT));
	if constexpr (W == 16) {
		defines.emplace("RISCV_ARENA_END", std::to_string(uint64_t(arena_end)));
		defines.emplace("RISCV_ARENA_ROEND", std::to_string(uint64_t(initial_rodata_end)));
	} else {
		defines.emplace("RISCV_ARENA_END", std::to_string(arena_end));
		defines.emplace("RISCV_ARENA_ROEND", std::to_string(initial_rodata_end));
	}
	if constexpr (atomics_enabled) {
		defines.emplace("RISCV_EXT_A", "1");
	}
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
	if constexpr (encompassing_Nbit_arena != 0) {
		defines.emplace("RISCV_NBIT_UNBOUNDED", std::to_string(encompassing_Nbit_arena));
	}
	return defines;
}

template <int W>
int CPU<W>::load_translation(const MachineOptions<W>& options,
	std::string* filename, DecodedExecuteSegment<W>& exec) const
{
	// Disable translator by setting options.translate_enabled to false
	// or by setting max blocks to zero.
	if (0 == options.translate_blocks_max || (!options.translate_enabled && !options.translate_enable_embedded)) {
		if (options.verbose_loader) {
			printf("libriscv: Binary translation disabled\n");
		}
		exec.set_binary_translated(nullptr, false);
		return -1;
	}
	if (exec.is_binary_translated()) {
		throw MachineException(ILLEGAL_OPERATION, "Execute segment already binary translated");
	}

	// Checksum the execute segment, ...
	TIME_POINT(t5);
	const std::string cflags = defines_to_string(create_defines_for(machine(), options));
	extern std::string compile_command(int arch, const std::string& cflags);
	uint32_t checksum = exec.crc32c_hash();
	if (UNLIKELY(checksum == 0)) {
		throw MachineException(INVALID_PROGRAM, "Invalid execute segment hash for translation");
	}
	// Also add the compiler flags to the checksum
	checksum = crc32c(checksum, cflags.c_str(), cflags.size());
	exec.set_translation_hash(checksum);

	if (options.translate_timing) {
		TIME_POINT(t6);
		printf(">> Execute segment 0x%X hashing took %ld ns\n", checksum, nanodiff(t5, t6));
	}

	// Check if translation is registered
	if (options.translate_enable_embedded)
	{
		TIME_POINT(t6);

		for (size_t i = 0; i < registered_embedded_translations<W>.count; i++)
		{
			auto& translation = registered_embedded_translations<W>.translations[i];
			if (translation.hash == checksum)
			{
				// Initialize the translation
				Machine<W>& m = const_cast<Machine<W>&> (machine());
				auto counters = m.get_counters();

				const int32_t ins_counter_offset = uintptr_t(&counters.first) - uintptr_t(&m);
				const int32_t max_counter_offset = uintptr_t(&counters.second) - uintptr_t(&m);
				const int32_t arena_offset = uintptr_t(&machine().memory.memory_arena_ptr_ref()) - uintptr_t(&m);

				translation.init_func(create_bintr_callback_table(exec),
					arena_offset, ins_counter_offset, max_counter_offset);

				if (options.verbose_loader) {
					printf("libriscv: Found embedded translation for hash %08X, %u/%u mappings\n",
						checksum, translation.nhandlers, translation.nmappings);
				}

				auto& mappings = exec.create_mappings(translation.nhandlers);
				std::copy(translation.handlers, translation.handlers + translation.nhandlers, mappings.begin());

				const uint8_t bytecode = RV32I_BC_TRANSLATOR;
				for (unsigned i = 0; i < translation.nmappings; i++) {
					const auto& mapping = translation.mappings[i];

					auto& entry = decoder_entry_at(exec.decoder_cache(), mapping.addr);
					entry.set_bytecode(bytecode);
					entry.set_invalid_handler();
					entry.instr = mapping.mapping_index;
				}
				if (options.translate_timing) {
					TIME_POINT(t7);
					printf(">> Activating embedded code took %ld ns\n", nanodiff(t6, t7));
				}
				return 0;
			}
		}
		if (options.verbose_loader) {
			printf("libriscv: No embedded translation found for hash %08X\n", checksum);
		}

		// If we are only looking for embedded translations,
		// check if we should emit embeddable code and then return.
		if (!options.translate_enabled) {
			for (auto& cc : options.cross_compile) {
				if (std::holds_alternative<MachineTranslationEmbeddableCodeOptions>(cc))
					return 1; // We must compile embeddable source code
			}
			// No need to compile anything
			return -1;
		}
	}

	if (!options.translate_enabled)
		return -1;

	char filebuffer[512];
	int len = snprintf(filebuffer, sizeof(filebuffer),
		"%s%08X%s", options.translation_prefix.c_str(), checksum, options.translation_suffix.c_str());
	if (len <= 0)
		return -1;

	void* dylib = nullptr;
	{
		TIME_POINT(t7);
		// Probably not needed, but on Windows there might be some issues
		// with the emulated dlopen() functionality. Let's serialize it.
		static std::mutex dlopen_mutex;
		std::lock_guard<std::mutex> lock(dlopen_mutex);
		dylib = dlopen(filebuffer, RTLD_LAZY);
		if (options.translate_timing) {
			TIME_POINT(t8);
			printf(">> dlopen took %ld ns\n", nanodiff(t7, t8));
		}
	}
	bool must_compile = dylib == nullptr;

	// JIT-compilation with libtcc is secondary to high-performance
	// pre-compiled translations. If no embedded translation is found,
	// and no shared library is found we may JIT-compile the translation.
	if constexpr (libtcc_enabled) {
		if (must_compile)
			return 1;
	}

#ifndef _MSC_VER
	// If cross compilation is enabled, we should check if all results exist
	for (auto& cc : options.cross_compile) {
		if (std::holds_alternative<MachineTranslationCrossOptions>(cc))
		{
			auto& mingw = std::get<MachineTranslationCrossOptions>(cc);
			const uint32_t hash = checksum;
			const std::string cross_filename = options.translation_filename(
				mingw.cross_prefix, hash, mingw.cross_suffix);
			if (access(cross_filename.c_str(), R_OK) != 0) {
				must_compile = true;
				break; // We must compile at least one of the cross-compiled binaries
			}
		} else if (std::holds_alternative<MachineTranslationEmbeddableCodeOptions>(cc)) {
			must_compile = true;
			break; // We must compile embeddable source code
		} else {
			throw MachineException(INVALID_PROGRAM, "Invalid cross-compile option");
		}
	}
#endif

	// We must compile ourselves
	if (dylib == nullptr) {
		if (filename) *filename = std::string(filebuffer);
		return 1;
	}

	this->activate_dylib(options, exec, dylib, machine(), false, false);

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
static void record_return_location(std::unordered_map<address_type<W>, address_type<W>>& single_return_locations, address_type<W> caller, address_type<W> callee)
{
	auto it = single_return_locations.find(callee);
	if (it != single_return_locations.end()) {
		// We already have a return location, disable it by setting it to zero
		// This means JALR cannot predict the return location
		it->second = 0;
	} else {
		// Record the return location
		// This means JALR can predict the return location
		single_return_locations.emplace(callee, caller);
	}
}

template <int W>
void CPU<W>::binary_translate(const MachineOptions<W>& options, DecodedExecuteSegment<W>& exec,
	TransOutput<W>& output) const
{
	// Run with VERBOSE=1 to see command and output
	const bool verbose = options.verbose_loader;
	const bool trace_instructions = options.translate_trace;
	bool is_libtcc = libtcc_enabled;
	if constexpr (libtcc_enabled) {
		// If there is an embeddable code option, we can't enable libtcc
		for (auto& cc : options.cross_compile) {
			if (std::holds_alternative<MachineTranslationEmbeddableCodeOptions>(cc)) {
				is_libtcc = false;
				break;
			}
		}
	}

	const address_t basepc    = exec.exec_begin();
	const address_t endbasepc = exec.exec_end();
	const uintptr_t arena_ponter_ref = (uintptr_t)machine().memory.memory_arena_ptr_ref();
	const address_t arena_roend = machine().memory.initial_rodata_end();
	const address_t arena_size  = machine().memory.memory_arena_size();

	address_t gp = 0;
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

		if constexpr (compressed_enabled)
			pc += instruction.length();
		else
			pc += 4;
	} // iterator
	if (options.translate_timing) {
		TIME_POINT(t1);
		printf(">> GP scan took %ld ns, GP=0x%lX\n", nanodiff(output.t0, t1), (long)gp);
	}
} // SCAN_FOR_GP

	// EBREAK locations
	std::unordered_set<address_type<W>> ebreak_locations;
	for (auto& loc : options.ebreak_locations) {
		address_t addr = 0;
		if (std::holds_alternative<address_type<W>>(loc))
			addr = std::get<address_type<W>>(loc);
		else
			addr = machine().address_of(std::get<std::string>(loc));
		if (addr >= basepc && addr < endbasepc) {
			ebreak_locations.insert(addr);
			if (verbose) {
				printf("libriscv: Binary translator added ebreak location at 0x%lX\n", (long)addr);
			}
		}
	}

	// Code block and loop detection
	TIME_POINT(t2);
	static constexpr size_t ITS_TIME_TO_SPLIT = (libtcc_enabled) ? 150'000 : 1'250;
	size_t icounter = 0;
	std::unordered_set<address_type<W>> global_jump_locations;
	std::unordered_map<address_type<W>, address_type<W>> single_return_locations;
	std::vector<TransInfo<W>> blocks;

	// Insert the ELF entry point as the first global jump location
	const auto elf_entry = machine().memory.start_address();
	if (elf_entry >= basepc && elf_entry < endbasepc)
		global_jump_locations.insert(elf_entry);
	// Speculate that the first instruction is a jump target
	global_jump_locations.insert(exec.exec_begin());

	for (auto address : options.translator_jump_hints) {
		if (address >= basepc && address < endbasepc) {
			global_jump_locations.insert(address);
		}
	}

	for (address_t pc = basepc; pc < endbasepc && icounter < options.translate_instr_max; )
	{
		const auto block = pc;
		std::size_t block_insns = 0;

		for (; pc < endbasepc; ) {
			const rv32i_instruction instruction
				= read_instruction(exec.exec_data(), pc, endbasepc);
			if constexpr (compressed_enabled)
				pc += instruction.length();
			else
				pc += 4;
			block_insns++;

			// JALR and STOP are show-stoppers / code-block enders
			if (block_insns >= ITS_TIME_TO_SPLIT && is_stopping_instruction(instruction)) {
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

				// Record return location for JALR prediction when rd != 0
				if (instruction.opcode() == RV32I_JAL && instruction.Jtype.rd != 0) {
					record_return_location<W>(single_return_locations, pc + instruction.length(), location);
					global_jump_locations.insert(pc + instruction.length());
				}

				if (location >= block && location < block_end)
					jump_locations.insert(location);
			}
			// loop detection (negative branch offsets)
			else if (is_branch) {
				// only accept branches relative to current block
				if (location >= block && location < block_end)
					jump_locations.insert(location);
				else
					global_jump_locations.insert(location);
			}

			// Add instruction to block
			block_instructions.push_back(instruction);
			if constexpr (compressed_enabled)
				pc += instruction.length();
			else
				pc += 4;
		} // process block

		// Process block and add it for emission
		const size_t length = block_instructions.size();
		if (length > 0 && icounter + length < options.translate_instr_max)
		{
			if constexpr (VERBOSE_BLOCKS) {
				printf("Block found at %#lX -> %#lX. Length: %zu\n", long(block), long(block_end), length);
				for (auto loc : jump_locations)
					printf("-> Jump to %#lX\n", long(loc));
			}

			blocks.push_back({
				std::move(block_instructions),
				block, block_end,
				basepc, endbasepc,
				gp,
				is_libtcc,
				trace_instructions,
				options.translate_ignore_instruction_limit,
				options.use_shared_execute_segments,
				options.translate_use_register_caching,
				options.translate_automatic_nbit_address_space,
				std::move(jump_locations),
				std::move(single_return_locations),
				nullptr, // blocks
				&ebreak_locations,
				global_jump_locations,
				// Memory arena
				arena_ponter_ref,
				arena_roend,
				arena_size
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
	auto& dlmappings = output.mappings;
	extern const std::string bintr_code;
	output.code = std::make_shared<std::string>(bintr_code);

	for (auto& block : blocks)
	{
		block.blocks = &blocks;
		auto result = emit(*output.code, block);

		for (auto& mapping : result) {
			dlmappings.push_back(std::move(mapping));
		}
	}
	// Append all instruction handler -> dl function mappings
	// to the footer used by shared libraries
	auto& footer = output.footer;
	footer += "VISIBLE const uint32_t no_mappings = "
		+ std::to_string(dlmappings.size()) + ";\n";
	footer += R"V0G0N(
struct Mapping {
	addr_t   addr;
	unsigned mapping_index;
};
VISIBLE const struct Mapping mappings[] = {
)V0G0N";

	std::unordered_map<std::string, unsigned> mapping_indices;
	std::vector<const std::string*> handlers;
	handlers.reserve(blocks.size());

	for (const auto& mapping : dlmappings)
	{
		// Create map of unique mappings
		unsigned mapping_index = 0;
		auto it = mapping_indices.find(mapping.symbol);
		if (it == mapping_indices.end()) {
			mapping_index = handlers.size();
			mapping_indices.emplace(mapping.symbol, mapping_index);
			handlers.push_back(&mapping.symbol);
		} else {
			mapping_index = it->second;
		}

		char buffer[128];
		snprintf(buffer, sizeof(buffer), 
			"{0x%lX, %u},\n",
			(long)mapping.addr, mapping_index);
		footer.append(buffer);
	}
	footer += "};\nVISIBLE const uint32_t no_handlers = "
		+ std::to_string(mapping_indices.size()) + ";\n"
		+ "VISIBLE const void* unique_mappings[] = {\n";

	// Create array of unique mappings
	if (handlers.size() != blocks.size())
		throw MachineException(INVALID_PROGRAM, "Mismatch in unique mappings");
	for (auto* handler : handlers) {
		footer += "    " + *handler + ",\n";
	}
	footer += "};\n";

	if (options.translate_timing) {
		TIME_POINT(t4);
		printf(">> Code generation took %ld ns\n", nanodiff(t3, t4));
	}

	if (verbose) {
		printf("libriscv: Emitted %zu accelerated instructions, %zu blocks and %zu functions. GP=0x%lX\n",
			icounter, blocks.size(), dlmappings.size(), (long) gp);
	}
}

template <int W>
void CPU<W>::produce_embeddable_code(const MachineOptions<W>& options, DecodedExecuteSegment<W>& exec,
	const TransOutput<W>& output, const MachineTranslationEmbeddableCodeOptions& embed)
{
	const uint32_t hash = exec.translation_hash();
	const std::string& embed_filename = options.translation_filename(
		embed.prefix, hash, embed.suffix);

	std::stringstream embed_code;
	embed_code << "#define EMBEDDABLE_CODE 1\n"; // Mark as embeddable variant
	for (auto& def : output.defines) {
		embed_code << "#define " << def.first << " " << def.second << "\n";
	}
	embed_code << *output.code;
	// Construct a footer that self-registers the translation
	const std::string reg_func = "libriscv_register_translation" + std::to_string(W);
	embed_code << R"V0G0N(
struct Mappings {
	addr_t   addr;
	unsigned mapping_index;
};
typedef ReturnValues (*bintr_func)(CPU*, uint64_t, uint64_t, addr_t);
# ifdef __cplusplus
#define EXTERN_C extern "C"
# else
#define EXTERN_C extern
# endif
#ifndef CALLBACK_INIT
EXTERN_C void libriscv_register_translation4(uint32_t hash, const struct Mappings* mappings, uint32_t nmappings, const bintr_func* handlers, uint32_t nhandlers, void*);
EXTERN_C void libriscv_register_translation8(uint32_t hash, const struct Mappings* mappings, uint32_t nmappings, const bintr_func* handlers, uint32_t nhandlers, void*);
#define REGISTRATION_ATTR  __attribute__((constructor, used))
#else
typedef void (*RegistrationFunction) (uint32_t hash, const struct Mappings* mappings, uint32_t nmappings, const bintr_func* handlers, uint32_t nhandlers, void*);
static RegistrationFunction libriscv_register_translation4;
static RegistrationFunction libriscv_register_translation8;
#define REGISTRATION_ATTR /* */
#endif
static REGISTRATION_ATTR void register_translation() {
	static const struct Mappings mappings[] = {
)V0G0N";

	std::unordered_map<std::string, unsigned> mapping_indices;
	std::vector<const std::string*> handlers;

	for (const auto& mapping : output.mappings)
	{
		// Create map of unique mappings
		unsigned mapping_index = 0;
		auto it = mapping_indices.find(mapping.symbol);
		if (it == mapping_indices.end()) {
			mapping_index = handlers.size();
			mapping_indices.emplace(mapping.symbol, mapping_index);
			handlers.push_back(&mapping.symbol);
		} else {
			mapping_index = it->second;
		}

		char buffer[128];
		snprintf(buffer, sizeof(buffer), 
			"{0x%lX, %u},\n",
			(long)mapping.addr, mapping_index);
		embed_code << buffer;
	}
	embed_code << "    };\n"
		"static bintr_func unique_mappings[] = {\n";
	for (auto* handler : handlers) {
		embed_code << "    " << *handler << ",\n";
	}
	embed_code << "};\n"
		"    " << reg_func << "(" << hash << ", mappings, " << output.mappings.size()
		<< ", unique_mappings, " << mapping_indices.size() << ", (void*)&init);\n";
	embed_code << R"V0G0N(}
#ifdef CALLBACK_INIT
EXTERN_C __attribute__((used, visibility("default"))) void libriscv_init_with_callback4(RegistrationFunction regfunc) {
	libriscv_register_translation4 = regfunc;
	register_translation();
}
EXTERN_C __attribute__((used, visibility("default"))) void libriscv_init_with_callback8(RegistrationFunction regfunc) {
	libriscv_register_translation8 = regfunc;
	register_translation();
}
#endif
)V0G0N";

	if (embed.result_c99 == nullptr) {
		// Write the embeddable code to a file
		std::ofstream embed_file;
		embed_file.open(embed_filename, std::ios::out | std::ios::trunc);
		if (!embed_file.is_open()) {
			throw MachineException(INVALID_PROGRAM, "Failed to open embeddable code file");
		}
		embed_file << embed_code.str();
	} else {
		// Return the embeddable code as a string
		*embed.result_c99 = embed_code.str();
	}
}

template <int W>
void CPU<W>::try_translate(const MachineOptions<W>& options, const std::string& filename,
	std::shared_ptr<DecodedExecuteSegment<W>>& shared_segment) const
{
	// Check if compiling new translations is enabled
	if (!options.translate_invoke_compiler) {
		// Check if there are any embeddable code options
		bool has_embeddable = false;
		for (auto& cc : options.cross_compile) {
			if (std::holds_alternative<MachineTranslationEmbeddableCodeOptions>(cc)) {
				// We must compile embeddable source code
				has_embeddable = true;
				break;
			}
		}
		if (!has_embeddable)
			return; // No need to compile anything
	}

	TransOutput<W> output;
	TIME_POINT(t0);
	output.t0 = t0;

	output.defines = create_defines_for(machine(), options);
	const bool live_patch = options.translate_background_callback != nullptr;

	// Compilation step
	std::function<void()> compilation_step =
	[this, options, output = std::move(output), filename, live_patch, shared_segment = shared_segment] () mutable
	{
		auto* exec = shared_segment.get();
		// The mutex is already locked when we enter this function
		// but we need to unlock it when we are done, or an exception is thrown.
		struct Unlock {
			Unlock(DecodedExecuteSegment<W>& exec) : m_exec(exec) {}
			~Unlock() { m_exec.background_compilation_mutex().unlock(); }
		private:
			DecodedExecuteSegment<W>& m_exec;
		} unlock(*exec);

		this->binary_translate(options, *exec, output);

		//printf("*** Compiling translation from 0x%lX to 0x%lX ***\n",
		//	long(shared_segment->exec_begin()), long(shared_segment->exec_end()));

		for (auto& cc : options.cross_compile)
		{
			if (std::holds_alternative<MachineTranslationEmbeddableCodeOptions>(cc))
			{
				auto& embed = std::get<MachineTranslationEmbeddableCodeOptions>(cc);
				produce_embeddable_code(options, *shared_segment, output, embed);
			}
		}

		void* dylib = nullptr;
		// Final shared library loadable code w/footer
		const std::string shared_library_code = *output.code + output.footer;

		TIME_POINT(t9);
		// If translate_invoke_compiler is disabled, do not compile
		// This allows for producing embeddable code without invoking the compiler
		if (libtcc_enabled && options.translate_invoke_compiler) {
			extern void* libtcc_compile(const std::string&, int arch, const std::unordered_map<std::string, std::string>& defines, const std::string&);
			// XXX: Debugging: write the compiled code to a file
			if constexpr (false) {
				std::ofstream ofs("libtcc_output.c", std::ios::out | std::ios::trunc);
				if (ofs.is_open()) {
					ofs << shared_library_code;
					ofs.close();
				} else {
					fprintf(stderr, "libriscv: Failed to write libtcc output to file\n");
				}
			}
			dylib = libtcc_compile(shared_library_code, W, output.defines, "");
		} else if (options.translate_invoke_compiler) {
			extern void* compile(const std::string&, int arch, const std::string& cflags, const std::string&);
			extern bool mingw_compile(const std::string&, int arch, const std::string& cflags, const std::string&, const MachineTranslationCrossOptions&);
			const std::string cflags = defines_to_string(output.defines);

			// If the binary translation has already been loaded, we can skip compilation
			if (exec->is_binary_translated()) {
				dylib = exec->binary_translation_so();
			} else {
				dylib = compile(shared_library_code, W, cflags, filename);
			}

			// Optionally produce cross-compiled binaries
			for (auto& cc : options.cross_compile)
			{
				if (std::holds_alternative<MachineTranslationCrossOptions>(cc))
				{
	#ifndef _MSC_VER
					auto& mingw = std::get<MachineTranslationCrossOptions>(cc);
					const uint32_t hash = exec->translation_hash();
					const std::string cross_filename = options.translation_filename(
						mingw.cross_prefix, hash, mingw.cross_suffix);
					mingw_compile(shared_library_code, W, cflags, cross_filename, mingw);
	#endif
				}
			}
		}

		if (options.translate_timing) {
			TIME_POINT(t10);
			printf(">> Code compilation took %.2f ms\n", nanodiff(t9, t10) / 1e6);
		}

		// Check compilation result
		if (dylib != nullptr) {
			if (!exec->is_binary_translated()) {
				activate_dylib(options, *exec, dylib, machine(), libtcc_enabled, live_patch);
			}

			if constexpr (!libtcc_enabled) {
				if (!options.translation_cache) {
					// Delete the shared object if it is unwanted
					unlink(filename.c_str());
				}
			}
		}

		if (options.translate_timing) {
			TIME_POINT(t12);
			printf(">> Binary translation totals %.2f ms\n", nanodiff(output.t0, t12) / 1e6);
		}
	};

	shared_segment->background_compilation_mutex().lock();
	if (options.translate_background_callback) {
		shared_segment->set_background_compiling(true);
		// User-provided callback for background compilation
		options.translate_background_callback(compilation_step);
	} else {
		// Synchronous compilation
		compilation_step();
	}
}

template <int W>
void CPU<W>::activate_dylib(const MachineOptions<W>& options, DecodedExecuteSegment<W>& exec, void* dylib, const Machine<W>& machine, bool is_libtcc, bool live_patch)
{
	TIME_POINT(t11);

	if (!initialize_translated_segment(exec, dylib, machine, is_libtcc))
	{
		if constexpr (!libtcc_enabled) {
			// only warn when translation is not already disabled
			if (options.verbose_loader) {
				fprintf(stderr, "libriscv: Could not find dylib init function\n");
			}
		}
		if (dylib != nullptr) {
			dylib_close(dylib, is_libtcc);
		}
		exec.set_binary_translated(nullptr, false);
		exec.set_background_compiling(false);
		return;
	}

	// Map all the functions to instruction handlers
	const uint32_t* no_mappings = (const uint32_t *)dylib_lookup(dylib, "no_mappings", is_libtcc);
	const auto* mappings = (const Mapping<W> *)dylib_lookup(dylib, "mappings", is_libtcc);
	const uint32_t* no_handlers = (const uint32_t *)dylib_lookup(dylib, "no_handlers", is_libtcc);
	const auto* handlers = (const bintr_block_func<W> *)dylib_lookup(dylib, "unique_mappings", is_libtcc);

	if (no_mappings == nullptr || mappings == nullptr || *no_mappings > 500000UL) {
		dylib_close(dylib, is_libtcc);
		exec.set_binary_translated(nullptr, false);
		throw MachineException(INVALID_PROGRAM, "Invalid mappings in binary translation program");
	}

	// After this, we should automatically close the dylib on destruction
	exec.set_binary_translated(dylib, is_libtcc);

	// Helper to rebuild decoder blocks
	std::unique_ptr<DecoderCache<W>[]> patched_decoder_cache = nullptr;
	DecoderData<W>* patched_decoder = nullptr;
	DecoderData<W>* decoder_begin   = nullptr;
	std::vector<DecoderData<W>*> livepatch_bintr;
	if (live_patch) {
		patched_decoder_cache = std::make_unique<DecoderCache<W>[]>(exec.decoder_cache_size());
		// Copy the decoder cache to the patched decoder cache
		std::memcpy(patched_decoder_cache.get(), exec.decoder_cache_base(), exec.decoder_cache_size() * sizeof(DecoderCache<W>));
		// A horrible calculation to find the patched decoder
		patched_decoder = patched_decoder_cache[0].get_base() - exec.pagedata_base() / DecoderCache<W>::DIVISOR;
		decoder_begin = &decoder_entry_at(patched_decoder, exec.exec_begin());
		// Pre-allocate the livepatch_bintr vector
		livepatch_bintr.reserve(*no_mappings);
	}
	std::unordered_map<bintr_block_func<W>, unsigned> block_indices;
	const unsigned nmappings = *no_mappings;
	const unsigned unique_mappings = *no_handlers;
	static constexpr bool enable_live_patching = false;

	// Create N+1 mappings, where the last one is a catch-all for invalid mappings
	auto& exec_mappings = exec.create_mappings(unique_mappings + 1);
	std::copy(handlers, handlers + unique_mappings, exec_mappings.begin());
	exec.set_mapping(unique_mappings, [] (CPU<W>&, uint64_t, uint64_t, address_t) -> bintr_block_returns<W> {
		throw MachineException(INVALID_PROGRAM, "Translation mapping outside execute area");
	});

	// Apply mappings to decoder cache
	// NOTE: It is possible to optimize this by applying from the end towards the beginning
	// and for each mapping, move on to the next mapping when that mappings address is reached.
	// This way, we can avoid the O(n) search for each mapping, and instead do it in O(1).
	for (unsigned i = 0; i < nmappings; i++)
	{
		const unsigned mapping_index = mappings[i].mapping_index;
		const auto addr = mappings[i].addr;

		if (exec.is_within(addr)) {
			auto* handler = handlers[mapping_index];
			if (handler != nullptr)
			{
				if (live_patch) {
					// NOTE: If we don't use the patched decoder here, entries
					// will trample each other in the patched decoder cache.
					auto& entry = decoder_entry_at(patched_decoder, addr);
					// If the entry is already the last one in the block,
					// we can skip the processing entirely.
					if (entry.block_bytes() == 0) {
						entry.set_bytecode(RV32I_BC_TRANSLATOR);
						entry.set_invalid_handler();
						entry.instr = mapping_index;
					#ifdef RISCV_EXT_C
						entry.icount = 0;
					#endif
						entry.idxend = 0;
						if constexpr (enable_live_patching) {
							auto& original_entry = decoder_entry_at(exec.decoder_cache(), addr);
							livepatch_bintr.push_back(&original_entry);
						}
						continue;
					}
					// The code below doesn't work, so we skip it
					continue;

					// 1. The last instruction will be the current entry
					// 2. Later instructions will work as normal
					// 3. Look back to find the beginning of the block
					auto* last    = &entry;
					auto* current = &entry;
					auto last_block_bytes = entry.block_bytes();
					while (current > decoder_begin && (current-1)->block_bytes() > last_block_bytes) {
						current--;
						last_block_bytes = current->block_bytes();
					}
					int block_bytes = last_block_bytes - entry.block_bytes();

					const auto block_begin_addr = addr - block_bytes;
					if (block_begin_addr < exec.exec_begin() || block_begin_addr >= exec.exec_end()) {
						if (options.verbose_loader)
						fprintf(stderr, "libriscv: Patched address 0x%lX outside execute area 0x%lX-0x%lX\n",
							(long)block_begin_addr, (long)exec.exec_begin(), (long)exec.exec_end());
						throw MachineException(INVALID_PROGRAM, "Translation mapping outside execute area");
					}

					// 4. Correct block_bytes() for all entries in the block
					auto patched_addr = block_begin_addr;
					if (current + block_bytes / (compressed_enabled ? 2 : 4) != last) {
						throw MachineException(INVALID_PROGRAM, "Translation mapping block bytes mismatch");
					}
					for (auto* dd = current; dd < last; dd++) {
						// Get the patched decoder entry
						auto& p = decoder_entry_at(patched_decoder, patched_addr);
					#ifdef RISCV_EXT_C
						p.icount = last - dd; // This is inexact, but works for now
						p.idxend = block_bytes / 2;
					#else
						p.idxend = last - dd;
					#endif
						patched_addr += (compressed_enabled ? 2 : 4);
						block_bytes -= (compressed_enabled ? 2 : 4);
					}
					if (compressed_enabled && block_bytes != 0) {
						if (options.verbose_loader)
							fprintf(stderr, "libriscv: Patched block bytes mismatch at 0x%lX: %u != 0\n",
								(long)block_begin_addr, block_bytes);
						throw MachineException(INVALID_PROGRAM, "Translation mapping block bytes mismatch");
					}

					// 5. The last instruction will be replaced with a binary translation
					// function, which will be the last instruction in the block.
					entry.set_bytecode(RV32I_BC_TRANSLATOR);
					entry.set_invalid_handler();
					entry.instr  = mapping_index;
					entry.idxend = 0;
				#ifdef RISCV_EXT_C
					entry.icount = 0;
				#endif
					if constexpr (enable_live_patching) {
						auto& original_entry = decoder_entry_at(exec.decoder_cache(), addr);
						livepatch_bintr.push_back(&original_entry);
					}
				} else {
					// Normal block-end hint that will be transformed into a translation
					// bytecode if it passes a few more checks, later.
					auto& entry = decoder_entry_at(exec.decoder_cache(), addr);
					entry.set_bytecode(RV32I_BC_TRANSLATOR);
					entry.set_invalid_handler();
					entry.instr = mapping_index;
				}
			} else {
				auto& entry = decoder_entry_at(exec.decoder_cache(), addr);
				entry.set_bytecode(0x0); /* Invalid opcode */
			}
		} else if (options.verbose_loader) {
			fprintf(stderr, "libriscv: Translation mapping 0x%lX outside execute area 0x%lX-0x%lX\n",
				(long)addr, (long)exec.exec_begin(), (long)exec.exec_end());
		}
	}

	if (live_patch) {
		// Move the patched decoder cache to the execute segment
		exec.set_patched_decoder_cache(std::move(patched_decoder_cache), patched_decoder);
		// Set regular decoder cache to the patched decoder cache
		exec.set_decoder(patched_decoder);

		if constexpr (enable_live_patching)
		{
			// Memory fence to ensure that the patched decoder is visible to all threads
#ifndef __COSMOCC__
			std::atomic_thread_fence(std::memory_order_seq_cst);
#endif

			// Atomically set a livepatch bytecode for each instruction that is patched
			// It will swap out the current decoder with the patched one, and then continue.
			for (auto* dd : livepatch_bintr) {
				dd->set_atomic_bytecode_and_handler(RV32I_BC_LIVEPATCH, 0);
			}
		}
		exec.set_background_compiling(false);
	}

	if (options.translate_timing) {
		TIME_POINT(t12);
		printf(">> Binary translation activation %ld ns\n", nanodiff(t11, t12));
	}
	if (options.verbose_loader) {
		printf("libriscv: Activated %s binary translation with hash 0x%X, %u/%u mappings%s\n",
			is_libtcc ? "libtcc" : "full",
			exec.translation_hash(),
			unique_mappings, nmappings,
			live_patch ? ", live-patching enabled" : "");
	}
}

template <int W>
CallbackTable<W> create_bintr_callback_table(DecodedExecuteSegment<W>&)
{
	return CallbackTable<W>{
		.mem_read = [] (CPU<W>& cpu, address_type<W> addr, unsigned size) -> address_type<W> {
			if constexpr (libtcc_enabled) {
				try {
					switch (size) {
					case 1: return cpu.machine().memory.template read<uint8_t>(addr);
					case 2: return cpu.machine().memory.template read<uint16_t>(addr);
					case 4: return cpu.machine().memory.template read<uint32_t>(addr);
					case 8: return cpu.machine().memory.template read<uint64_t>(addr);
					default: throw MachineException(ILLEGAL_OPERATION, "Invalid memory read size", size);
					}
				} catch (...) {
					cpu.set_current_exception(std::current_exception());
					cpu.machine().stop();
					return 0;
				}
			} else {
				switch (size) {
				case 1: return cpu.machine().memory.template read<uint8_t>(addr);
				case 2: return cpu.machine().memory.template read<uint16_t>(addr);
				case 4: return cpu.machine().memory.template read<uint32_t>(addr);
				case 8: return cpu.machine().memory.template read<uint64_t>(addr);
				default: throw MachineException(ILLEGAL_OPERATION, "Invalid memory read size", size);
				}
			}
		},
		.mem_write = [] (CPU<W>& cpu, address_type<W> addr, address_type<W> value, unsigned size) -> void {
			if constexpr (libtcc_enabled) {
				try {
					switch (size) {
					case 1: cpu.machine().memory.template write<uint8_t>(addr, value); break;
					case 2: cpu.machine().memory.template write<uint16_t>(addr, value); break;
					case 4: cpu.machine().memory.template write<uint32_t>(addr, value); break;
					case 8: cpu.machine().memory.template write<uint64_t>(addr, value); break;
					default: throw MachineException(ILLEGAL_OPERATION, "Invalid memory write size", size);
					}
				} catch (...) {
					cpu.set_current_exception(std::current_exception());
					cpu.machine().stop();
				}
			} else {
				switch (size) {
				case 1: cpu.machine().memory.template write<uint8_t>(addr, value); break;
				case 2: cpu.machine().memory.template write<uint16_t>(addr, value); break;
				case 4: cpu.machine().memory.template write<uint32_t>(addr, value); break;
				case 8: cpu.machine().memory.template write<uint64_t>(addr, value); break;
				default: throw MachineException(ILLEGAL_OPERATION, "Invalid memory write size", size);
				}
			}
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
		.syscalls = Machine<W>::syscall_handlers.data(),
		.system_call = [] (CPU<W>& cpu, int sysno) -> int {
			try {
				const auto current_tp = cpu.reg(REG_TP);
				const auto current_pc = cpu.registers().pc;
				cpu.machine().system_call(sysno);
				return cpu.registers().pc != current_pc || cpu.reg(REG_TP) != current_tp || cpu.machine().stopped();
			} catch (...) {
#ifdef RISCV_LIBTCC
				cpu.set_current_exception(std::current_exception());
				cpu.machine().stop();
				return false;
#else
				throw;
#endif
			}
		},
		.unknown_syscall = [] (CPU<W>& cpu, address_type<W> sysno) {
			cpu.machine().on_unhandled_syscall(cpu.machine(), sysno);
		},
		.system = [] (CPU<W>& cpu, uint32_t instr) -> int {
			try {
				cpu.machine().system(rv32i_instruction{instr});
				return 0;
			} catch (...) {
#ifdef RISCV_LIBTCC
				cpu.set_current_exception(std::current_exception());
				return 1; // Indicate that an exception occurred
#else
				throw; // Re-throw the exception
#endif
			}
		},
		.execute = [] (CPU<W>& cpu, uint32_t instr) -> unsigned {
			const rv32i_instruction rvi{instr};
			if constexpr (libtcc_enabled) {
				try {
					cpu.decode(rvi).handler(cpu, rvi);
					return 0;
				} catch (...) {
#ifdef RISCV_LIBTCC
					cpu.set_current_exception(std::current_exception());
					return 1;
#else
					throw;
#endif
				}
			} else {
				auto* handler = cpu.decode(rvi).handler;
				handler(cpu, rvi);
				return DecoderData<W>::handler_index_for(handler);
			}
		},
		.execute_handler = [] (CPU<W>& cpu, unsigned index, uint32_t instr) -> unsigned {
			const rv32i_instruction rvi{instr};
			try {
				DecoderData<W>::get_handlers()[index](cpu, rvi);
				return 0;
			} catch (...) {
#ifdef RISCV_LIBTCC
				cpu.set_current_exception(std::current_exception());
				return 1;
#else
				throw;
#endif
			}
		},
		.handlers = (void (**)(CPU<W>&, uint32_t)) DecoderData<W>::get_handlers(),
		.trigger_exception = [] (CPU<W>& cpu, address_type<W> pc, int e) {
			cpu.registers().pc = pc; // XXX: Set PC to the failing instruction (?)
#ifdef RISCV_LIBTCC
			if (libtcc_enabled && cpu.current_execute_segment().is_libtcc())
			{
				// If we're using libtcc, we can't throw C++ exceptions because
				// there's no unwinding support. But we can mark an exception
				// in the CPU state and return back to dispatch.
				try {
					cpu.trigger_exception(e);
				} catch (...) {
					cpu.set_current_exception(std::current_exception());
					// Trigger a slow-path in dispatch (which will check for exceptions)
					cpu.machine().stop();
				}
				return;
			}
#endif
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
#ifdef RISCV_HAS_BITOPS
			return std::countl_zero(x);
#else
			return x ? __builtin_clz(x) : 32;
#endif
		},
		.clzl = [] (uint64_t x) -> int {
#ifdef RISCV_HAS_BITOPS
			return std::countl_zero(x);
#else
			return x ? __builtin_clzl(x) : 64;
#endif
		},
		.ctz = [] (uint32_t x) -> int {
#ifdef RISCV_HAS_BITOPS
			return std::countr_zero(x);
#else
			return x ? __builtin_ctz(x) : 0;
#endif
		},
		.ctzl = [] (uint64_t x) -> int {
#ifdef RISCV_HAS_BITOPS
			return std::countr_zero(x);
#else
			return x ? __builtin_ctzl(x) : 0;
#endif
		},
		.cpop = [] (uint32_t x) -> int {
#ifdef RISCV_HAS_BITOPS
			return std::popcount(x);
#else
			return __builtin_popcount(x);
#endif
		},
		.cpopl = [] (uint64_t x) -> int {
#ifdef RISCV_HAS_BITOPS
			return std::popcount(x);
#else
			return __builtin_popcountl(x);
#endif
		},
	};
}

template <int W>
bool CPU<W>::initialize_translated_segment(DecodedExecuteSegment<W>& exec, void* dylib, const Machine<W>& machine, bool is_libtcc)
{
	// NOTE: At some point this must be able to duplicate the dylib
	// in order to be able to share execute segments across machines.

	auto* ptr = dylib_lookup(dylib, "init", is_libtcc); // init() function
	if (ptr == nullptr) {
		return false;
	}

	// Map the API callback table
	auto counters = const_cast<Machine<W>&> (machine).get_counters();
	auto func = (binary_translation_init_func<W>) ptr;

	const int32_t ins_counter_offset = uintptr_t(&counters.first) - uintptr_t(&machine);
	const int32_t max_counter_offset = uintptr_t(&counters.second) - uintptr_t(&machine);
	const int32_t arena_offset = uintptr_t(&machine.memory.memory_arena_ptr_ref()) - uintptr_t(&machine);

	func(create_bintr_callback_table<W>(exec), arena_offset, ins_counter_offset, max_counter_offset);

	return true;
}

template <int W>
std::string MachineOptions<W>::translation_filename(const std::string& prefix, uint32_t hash, const std::string& suffix)
{
	char buffer[256];
	const int len = snprintf(buffer, sizeof(buffer), "%s%08X%s",
		prefix.c_str(), hash, suffix.c_str());
	return std::string(buffer, len);
}

#ifdef RISCV_32I
	template void CPU<4>::try_translate(const MachineOptions<4>&, const std::string&, std::shared_ptr<DecodedExecuteSegment<4>>&) const;
	template int CPU<4>::load_translation(const MachineOptions<4>&, std::string*, DecodedExecuteSegment<4>&) const;
	template std::string MachineOptions<4>::translation_filename(const std::string&, uint32_t, const std::string&);
#endif
#ifdef RISCV_64I
	template void CPU<8>::try_translate(const MachineOptions<8>&, const std::string&, std::shared_ptr<DecodedExecuteSegment<8>>&) const;
	template int CPU<8>::load_translation(const MachineOptions<8>&, std::string*, DecodedExecuteSegment<8>&) const;
	template std::string MachineOptions<8>::translation_filename(const std::string&, uint32_t, const std::string&);
#endif
#ifdef RISCV_128I
	template void CPU<16>::try_translate(const MachineOptions<16>&, const std::string&, std::shared_ptr<DecodedExecuteSegment<16>>&) const;
	template int CPU<16>::load_translation(const MachineOptions<16>&, std::string*, DecodedExecuteSegment<16>&) const;
	template std::string MachineOptions<16>::translation_filename(const std::string&, uint32_t, const std::string&);
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

extern "C" {
	void RISCV_EXPORT libriscv_register_translation4(uint32_t hash, const riscv::Mapping<4>* mappings, uint32_t nmappings,
		const riscv::bintr_block_func<4>* handlers, uint32_t nhandlers, void* init_func_ptr)
	{
#ifdef RISCV_32I
		riscv::register_translation<4>(hash, mappings, nmappings, handlers, nhandlers, (riscv::binary_translation_init_func<4>)init_func_ptr);
#else
		(void)hash; (void)mappings; (void)nmappings; (void)handlers; (void)nhandlers; (void)init_func_ptr;
		fprintf(stderr, "libriscv: Warning: libriscv_register_translation4 called on 64-bit build\n");
#endif
	}
	void RISCV_EXPORT libriscv_register_translation8(uint32_t hash, const riscv::Mapping<8>* mappings, uint32_t nmappings,
		const riscv::bintr_block_func<8>* handlers, uint32_t nhandlers, void* init_func_ptr)
	{
#ifdef RISCV_64I
		riscv::register_translation<8>(hash, mappings, nmappings, handlers, nhandlers, (riscv::binary_translation_init_func<8>)init_func_ptr);
#else
		(void)hash; (void)mappings; (void)nmappings; (void)handlers; (void)nhandlers; (void)init_func_ptr;
		fprintf(stderr, "libriscv: Warning: libriscv_register_translation8 called on 32-bit build\n");
#endif
	}
}
