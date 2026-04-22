#include "script.hpp"
#include <fstream>
#include <libriscv/native_heap.hpp>
#include <libriscv/rv32i_instr.hpp>
#include <libriscv/util/crc32.hpp>

std::map<uint32_t, Script::HostFunction> Script::s_host_functions;

static std::vector<uint8_t> load_file(const std::string& filename)
{
	std::ifstream stream(filename, std::ios::in | std::ios::binary);
	if (!stream)
		throw std::runtime_error("Could not open file: " + filename);
	return {
		std::istreambuf_iterator<char>(stream),
		std::istreambuf_iterator<char>()
	};
}

static std::string single_spaced(std::string s)
{
	size_t pos = 0;
	while ((pos = s.find("  ", pos)) != std::string::npos)
		s.replace(pos, 2, " ");
	return s;
}

Script::Script(const std::string& name, const std::string& filename)
	: m_binary(load_file(filename)), m_name(name)
{
	static bool init = false;
	if (!init) {
		init = true;
		Script::setup_dispatch();
		Script::register_host_functions();
	}
	this->reset();
	this->initialize();
}

Script::~Script() {}

void Script::reset()
{
	riscv::MachineOptions<MARCH> options {
		.memory_max = MAX_MEMORY,
		.stack_size = STACK_SIZE,
		.use_memory_arena = true,
		.default_exit_function = "fast_exit",
		.translate_enabled = false,
	};
	m_machine = std::make_unique<machine_t>(m_binary, options);
	this->machine_setup();
	machine().setup_linux({name()}, {"LC_CTYPE=C", "LC_ALL=C", "USER=groot"});
}

void Script::initialize()
{
	this->resolve_host_functions(true);
	try {
		machine().simulate(MAX_BOOT_INSTR);
	} catch (riscv::MachineTimeoutException& me) {
		fprintf(stderr, ">>> Instruction limit reached on %s\n", name().c_str());
		throw;
	} catch (riscv::MachineException& me) {
		fprintf(stderr, ">>> Machine exception %d: %s (data: 0x%lx)\n",
			me.type(), me.what(), (long)me.data());
		throw;
	} catch (std::exception& e) {
		fprintf(stderr, ">>> Exception: %s\n", e.what());
		throw;
	}
	printf(">>> %s initialized.\n", name().c_str());
}

void Script::machine_setup()
{
	machine().set_userdata<Script>(this);
	machine().set_printer(
		(machine_t::printer_func)[](const machine_t&, const char* p, size_t len) {
			printf("%.*s", (int)len, p);
		});
	machine().on_unhandled_syscall = [](machine_t& machine, size_t num) {
		auto& script = *machine.get_userdata<Script>();
		fprintf(stderr, "%s: Unhandled system call: %zu\n", script.name().c_str(), num);
	};

	m_heap_area = machine().memory.mmap_allocate(MAX_HEAP);
	machine().setup_linux_syscalls(false, false);
	machine().setup_native_heap(HEAP_SYSCALLS_BASE, m_heap_area, MAX_HEAP);
	machine().setup_native_memory(MEMORY_SYSCALLS_BASE);
}

// --- Generated host function dispatch ---

void Script::set_host_function(
	std::string name, std::string signature, ghandler_t handler)
{
	signature = single_spaced(signature);
	const uint32_t hash = riscv::crc32(signature.c_str());

	auto it = s_host_functions.find(hash);
	if (it != s_host_functions.end() && it->second.name != name)
		throw std::runtime_error(
			"CRC32 hash collision: " + name + " vs " + it->second.name);

	s_host_functions[hash] = {
		std::move(name), std::move(signature), std::move(handler)};
}

std::size_t Script::host_function_count() noexcept
{
	return s_host_functions.size();
}

void Script::setup_dispatch()
{
	using CPU = riscv::CPU<MARCH>;
	using Instruction = riscv::Instruction<MARCH>;

	static const Instruction unchecked_hostcall {
		[](CPU& cpu, riscv::rv32i_instruction instr) {
			auto& script = *cpu.machine().template get_userdata<Script>();
			auto idx = (unsigned)instr.Itype.imm;
			if (idx < script.m_host_function_array.size())
				script.m_host_function_array[idx](script);
			else
				throw std::runtime_error("Host function index out of range");
		}, nullptr};

	CPU::on_unimplemented_instruction
		= [](riscv::rv32i_instruction instr) -> const Instruction& {
		if (instr.opcode() == 0b1011011
			&& instr.Itype.rs1 == 0
			&& instr.Itype.rd == 0)
		{
			if ((unsigned)instr.Itype.imm < Script::host_function_count())
				return unchecked_hostcall;
		}
		return CPU::get_unimplemented_instruction();
	};
}

void Script::resolve_host_functions(bool initialization, bool client_side)
{
	auto table_addr = machine().address_of("dyncall_table");
	if (table_addr == 0x0)
		throw std::runtime_error(name() + ": dyncall_table not found in guest ELF");

	const uint32_t count = machine().memory.template read<uint32_t>(table_addr);
	if (count > 2048)
		throw std::runtime_error(name() + ": dyncall_table has bogus entry count");

	auto entries = machine().memory.template memspan<const HostFunctionDesc>(
		table_addr + 4, count);

	m_host_function_array.clear();
	m_host_function_array.reserve(count);
	std::size_t unimplemented = 0;

	for (unsigned i = 0; i < count; i++) {
		auto& entry = entries[i];
		auto func_name = machine().memory.memstring(entry.strname);

		if (entry.initialization_only && !initialization) {
			m_host_function_array.push_back([func_name](Script&) {
				throw std::runtime_error("Init-only host function '" + func_name + "' called at runtime");
			});
			continue;
		}
		if (entry.client_side_only && !client_side) {
			m_host_function_array.push_back([func_name](Script&) {
				throw std::runtime_error("Client-only host function '" + func_name + "' called on server");
			});
			continue;
		}
		if (entry.server_side_only && client_side) {
			m_host_function_array.push_back([func_name](Script&) {
				throw std::runtime_error("Server-only host function '" + func_name + "' called on client");
			});
			continue;
		}

		auto it = s_host_functions.find(entry.hash);
		if (it != s_host_functions.end()) {
			m_host_function_array.push_back(it->second.func);
		} else {
			fprintf(stderr, "WARNING: Unimplemented host function '%s' (hash %08x)\n",
				func_name.c_str(), entry.hash);
			m_host_function_array.push_back([func_name](Script&) {
				throw std::runtime_error("Unimplemented host function: " + func_name);
			});
			unimplemented++;
		}
	}

	fprintf(stderr, "Resolved %u host functions for '%s' (%zu unimplemented)\n",
		count, name().c_str(), unimplemented);
}


Script::gaddr_t Script::address_of(const std::string& name) const
{
	auto it = m_lookup_cache.find(name);
	if (it != m_lookup_cache.end())
		return it->second;
	const auto addr = machine().address_of(name.c_str());
	m_lookup_cache.try_emplace(name, addr);
	return addr;
}

std::string Script::symbol_name(gaddr_t address) const
{
	auto callsite = machine().memory.lookup(address);
	return callsite.name;
}

Script::gaddr_t Script::guest_alloc(gaddr_t bytes)
{
	return machine().arena().malloc(bytes);
}

bool Script::guest_free(gaddr_t addr)
{
	return machine().arena().free(addr) == 0x0;
}

void Script::handle_exception(gaddr_t address)
{
	auto callsite = machine().memory.lookup(address);
	fprintf(stderr, "Exception at %s (0x%lx)\n", callsite.name.c_str(), (long)callsite.address);
	try { throw; }
	catch (const riscv::MachineTimeoutException& e) {
		this->handle_timeout(address);
	}
	catch (const riscv::MachineException& e) {
		fprintf(stderr, "Machine exception: %s (data: 0x%lx)\n", e.what(), (long)e.data());
	}
	catch (const std::exception& e) {
		fprintf(stderr, "Exception: %s\n", e.what());
	}
}

void Script::handle_timeout(gaddr_t address)
{
	auto callsite = machine().memory.lookup(address);
	fprintf(stderr, "Timeout at %s (0x%lx)\n", callsite.name.c_str(), (long)callsite.address);
}

void Script::max_depth_exceeded(gaddr_t address)
{
	auto callsite = machine().memory.lookup(address);
	fprintf(stderr, "Max call depth at %s (0x%lx)\n", callsite.name.c_str(), (long)callsite.address);
}
