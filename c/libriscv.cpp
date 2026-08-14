#include "libriscv.h"

#include <libriscv/util/load_binary_file.hpp>
#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>
using namespace riscv;
static const std::vector<std::string> env = {"LC_CTYPE=C", "LC_ALL=C", "USER=groot"};

#define RISCV_ARCH  RISCV64
#define MACHINE(x) ((Machine<RISCV_ARCH> *)x)
#define CONST_MACHINE(x) ((const Machine<RISCV_ARCH> *)x)
#define ERROR_CALLBACK(m, type, msg, data) \
	if (auto *usr = m->template get_userdata<UserData> (); usr->error != nullptr) \
		usr->error(usr->opaque, type, msg, data);
#define USERDATA(m) (*m->template get_userdata<UserData> ())

static std::vector<std::string> fill(unsigned count, const char* const* args) {
	std::vector<std::string> v;
	v.reserve(count);
	for (unsigned i = 0; i < count; i++)
		v.push_back(args[i]);
	return v;
}

struct UserData {
	riscv_error_func_t error = nullptr;
	riscv_stdout_func_t stdout = nullptr;
	void *opaque = nullptr;
	std::vector<std::string> allowed_files;
	const Machine<RISCV_ARCH>* fork_parent = nullptr;
	unsigned arena_syscall_base = 0;
	uint64_t arena_total_size = 0;
	size_t max_owned_pages = 0;
	size_t owned_page_count = 0;
};

static void setup_printer(Machine<RISCV_ARCH>* m)
{
	m->set_printer([] (auto& m, const char* data, size_t size) {
		auto& userdata = (*m.template get_userdata<UserData> ());
		if (userdata.stdout)
			userdata.stdout(userdata.opaque, data, size);
		else
			printf("%.*s", (int)size, data);
	});
}

static PageAttributes convert_to_cpp(const RISCVPageAttributes& c) {
	PageAttributes attr;
	attr.read       = c.read;
	attr.write      = c.write;
	attr.exec       = c.exec;
	attr.is_cow     = c.is_cow;
	attr.non_owning = c.non_owning;
	attr.dont_fork  = c.dont_fork;
	attr.user_defined = c.user_defined;
	return attr;
}

static RISCVPageAttributes convert_to_c(const PageAttributes& attr) {
	RISCVPageAttributes c;
	c.read         = attr.read;
	c.write        = attr.write;
	c.exec         = attr.exec;
	c.is_cow       = attr.is_cow;
	c.non_owning   = attr.non_owning;
	c.dont_fork    = attr.dont_fork;
	c.user_defined = attr.user_defined;
	return c;
}

#ifdef RISCV_VIRTUAL_PAGING
struct ParentPageInfo {
	PageData* data;
	PageAttributes attr;
};

// Non-mutating parent page lookup. Never creates pages on the parent.
// For pages not in the page table, returns arena data or the static cow_page.
static ParentPageInfo get_parent_page_readonly(
	const Machine<RISCV_ARCH>& parent, uint64_t pageno)
{
	auto& mem = parent.memory;
	auto it = mem.pages().find(pageno);
	if (it != mem.pages().end())
		return {it->second.m_page.get(), it->second.attr};
	if (mem.uses_flat_memory_arena()
		&& pageno < mem.memory_arena_size() / Page::size()) {
		auto* arena = static_cast<PageData*>(mem.memory_arena_ptr());
		return {&arena[pageno], {.read = true, .write = true}};
	}
	return {Page::cow_page().m_page.get(), Page::cow_page().attr};
}
#endif // RISCV_VIRTUAL_PAGING

extern "C"
void libriscv_set_defaults(RISCVOptions *options)
{
	MachineOptions<RISCV_ARCH> mo;

	options->max_memory = mo.memory_max;
	options->stack_size = mo.stack_size;
	options->strict_sandbox = true;
	options->argc = 0;
	options->argv = nullptr;
	options->error = nullptr;
	options->stdout = nullptr;
	options->opaque = nullptr;
	options->use_memory_arena = mo.use_memory_arena ? 1 : 0;
	options->use_shared_execute_segments = mo.use_shared_execute_segments ? 1 : 0;
	options->default_exit_function = nullptr;
	options->load_program = mo.load_program ? 1 : 0;
	options->protect_segments = mo.protect_segments ? 1 : 0;
	options->native_syscall_base = 0;
	options->arena_size = 8ULL << 20;
}

extern "C"
RISCVMachine *libriscv_new(const void *elf_prog, unsigned elf_length, const RISCVOptions *options)
{
	MachineOptions<RISCV_ARCH> mo {
		.memory_max = options->max_memory,
		.stack_size = options->stack_size,
		.load_program = (bool)options->load_program,
		.protect_segments = (bool)options->protect_segments,
		.use_memory_arena = (bool)options->use_memory_arena,
		.use_shared_execute_segments = (bool)options->use_shared_execute_segments,
		.default_exit_function = "fast_exit",
	};
	if (options->default_exit_function)
		mo.default_exit_function = options->default_exit_function;

	UserData *u = nullptr;
	try {
		auto view = std::string_view{(const char *)elf_prog, size_t(elf_length)};

		auto* m = new Machine<RISCV_ARCH> { view, mo };
		u = new UserData {
			.error = options->error, .stdout = options->stdout, .opaque = options->opaque
		};
		m->set_userdata(u);
		setup_printer(m);
		Machine<RISCV_ARCH>::on_unhandled_syscall = [] (auto& m, size_t num) {
			ERROR_CALLBACK((&m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, "Unknown system call", num);
		};

		std::vector<std::string> args;
		if (options->argc > 0) {
			args = fill(options->argc, options->argv);
		} else {
			args.push_back("./program"); // We need at least one argument
		}
		m->setup_linux(args, env);
		m->setup_linux_syscalls();
		m->setup_posix_threads();
		if (options->native_syscall_base > 0) {
			const unsigned base = options->native_syscall_base;
			Machine<RISCV_ARCH>::setup_native_memory(base + 5);
			m->setup_native_heap(base, m->memory.mmap_allocate(options->arena_size), options->arena_size);
			u->arena_syscall_base = base;
			u->arena_total_size = options->arena_size;
		}
		m->fds().permit_filesystem = !options->strict_sandbox;
		m->fds().permit_sockets = !options->strict_sandbox;
		// TODO: File permissions
		if (!options->strict_sandbox) {
			if (m->memory.is_dynamic_executable()) {
				// Since it's dynamic, the first argument (the program) is the dynamic linker
				// we'll treat the first argument as the program path, and automatically allow it
				USERDATA(m).allowed_files.push_back(args.at(0));
			}
			m->fds().filter_open = [=] (void* user, std::string& path) {
				(void) user;
				if (path == "/dev/urandom")
					return true;
				if (path == "/program") { // Fake program path
					path = args.at(0); // Sneakily open the real program instead
					return true;
				}

				// Paths that are allowed to be opened
				static const std::string sandbox_libdir  = "/lib/riscv64-linux-gnu/";
				// The real path to the libraries (on the host system)
				static const std::string real_libdir = "/usr/riscv64-linux-gnu/lib/";
				// The dynamic linker and libraries we allow
				auto& allowed_files = USERDATA(m).allowed_files;

				if (path.find(sandbox_libdir) == 0) {
					// Find the library name
					auto lib = path.substr(sandbox_libdir.size());
					for (const std::string& allowed_lib : allowed_files) {
						if (lib == allowed_lib) {
							// Construct new path
							path = real_libdir + path.substr(sandbox_libdir.size());
							return true;
						}
					}
				}

				if (m->memory.is_dynamic_executable() && args.size() > 1 && path == args.at(1)) {
					return true;
				}

				for (const auto& allowed : allowed_files) {
					if (path == allowed) {
						return true;
					}
				}
				return false;
			};
		}

		return (RISCVMachine *)m;
	}
	catch (const MachineException& me)
	{
		if (options->error)
			options->error(options->opaque, RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		delete u;
		return NULL;
	}
	catch (const std::exception& e)
	{
		if (options->error)
			options->error(options->opaque, RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
		delete u;
		return NULL;
	}
}

extern "C"
int libriscv_delete(RISCVMachine *m)
{
	try {
		delete MACHINE(m)->get_userdata<UserData> ();
		delete MACHINE(m);
		return 0;
	}
	catch (...)
	{
		return -1;
	}
}

extern "C"
int libriscv_run(RISCVMachine *m, uint64_t instruction_limit)
{
	try {
		auto& machine = *MACHINE(m);
		if (instruction_limit == 0) {
			machine.cpu.simulate_inaccurate(machine.cpu.pc());
			return machine.instruction_limit_reached() ? -RISCV_ERROR_TYPE_MACHINE_TIMEOUT : 0;
		}
		else {
			return machine.simulate<false>(instruction_limit) ? 0 : -RISCV_ERROR_TYPE_MACHINE_TIMEOUT;
		}
	} catch (const MachineTimeoutException& tmo) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_TIMEOUT, tmo.what(), tmo.data());
		return RISCV_ERROR_TYPE_MACHINE_TIMEOUT;
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		return RISCV_ERROR_TYPE_MACHINE_EXCEPTION;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
		return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
	}
}
extern "C"
int64_t libriscv_step_one(RISCVMachine *m, int verbose)
{
	std::string instr;
	std::string state;
	char buffer[1024];
	try {
		auto& machine = *MACHINE(m);
		instr = machine.cpu.current_instruction_to_string();
		const auto pc = machine.cpu.pc();
		if (verbose) {
			auto callsite = machine.memory.lookup(pc);
			snprintf(buffer, sizeof(buffer), ">> 0x%lX [%s] %s", pc, callsite.name.c_str(), instr.c_str());
			if (MACHINE(m)->get_userdata<UserData> ()->stdout)
				MACHINE(m)->get_userdata<UserData> ()->stdout(MACHINE(m)->get_userdata<UserData> ()->opaque, buffer, strlen(buffer));
			else
				printf("%s\n", buffer);

			const auto& regs = machine.cpu.registers();
			state = regs.to_string();
			if (MACHINE(m)->get_userdata<UserData> ()->stdout)
				MACHINE(m)->get_userdata<UserData> ()->stdout(MACHINE(m)->get_userdata<UserData> ()->opaque, state.c_str(), state.size());
			else
				printf("%s\n", state.c_str());
		}
		machine.cpu.step_one(true);
		return machine.stopped() ? 0 : machine.instruction_counter();
	} catch (const MachineTimeoutException& tmo) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_TIMEOUT, tmo.what(), tmo.data());
		return RISCV_ERROR_TYPE_MACHINE_TIMEOUT;
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		return RISCV_ERROR_TYPE_MACHINE_EXCEPTION;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
		return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
	}
}
extern "C"
const char * libriscv_strerror(int return_value)
{
	switch (return_value) {
	case 0:
		return "No error";
	case RISCV_ERROR_TYPE_MACHINE_TIMEOUT:
		return "Timed out";
	case RISCV_ERROR_TYPE_MACHINE_EXCEPTION:
		return "Machine exception";
	case RISCV_ERROR_TYPE_GENERAL_EXCEPTION:
		return "General exception";
	default:
		return "Unknown error";
	}
}
extern "C"
void libriscv_stop(RISCVMachine *m)
{
	MACHINE(m)->stop();
}

extern "C"
int64_t libriscv_return_value(RISCVMachine *m)
{
	return MACHINE(m)->return_value();
}

extern "C"
uint64_t libriscv_instruction_counter(RISCVMachine *m)
{
	return MACHINE(m)->instruction_counter();
}
extern "C"
uint64_t * libriscv_max_counter_pointer(RISCVMachine *m)
{
	return &MACHINE(m)->get_counters().second;
}

extern "C"
int libriscv_instruction_limit_reached(RISCVMachine *m)
{
	return MACHINE(m)->instruction_limit_reached();
}

extern "C"
uint64_t libriscv_address_of(RISCVMachine *m, const char *name)
{
	try {
		return ((Machine<RISCV_ARCH> *)m)->address_of(name);
	}
	catch (...) {
		return 0x0;
	}
}

extern "C"
void * libriscv_opaque(RISCVMachine *m)
{
	return MACHINE(m)->get_userdata<UserData> ()->opaque;
}

extern "C"
void libriscv_allow_file(RISCVMachine *m, const char *path)
{
	USERDATA(MACHINE(m)).allowed_files.push_back(path);
}

extern "C"
int libriscv_set_syscall_handler(unsigned idx, riscv_syscall_handler_t handler)
{
	try {
		Machine<RISCV_ARCH>::syscall_handlers.at(idx) = Machine<RISCV_ARCH>::syscall_t(handler);
		return 0;
	}
	catch (...) {
		return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
	}
}

extern "C"
void libriscv_set_result_register(RISCVMachine *m, int64_t value)
{
	MACHINE(m)->set_result(value);
}
extern "C"
RISCVRegisters * libriscv_get_registers(RISCVMachine *m)
{
	return (RISCVRegisters *)&MACHINE(m)->cpu.registers();
}
extern "C"
int libriscv_jump(RISCVMachine *m, uint64_t address)
{
	try {
		MACHINE(m)->cpu.jump(address);
		return 0;
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		return RISCV_ERROR_TYPE_MACHINE_EXCEPTION;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
	}
	return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
}
extern "C"
int libriscv_setup_vmcall(RISCVMachine *m, uint64_t address)
{
	try {
		auto* machine = MACHINE(m);
		machine->cpu.reset_stack_pointer();
		machine->setup_call();
		machine->cpu.jump(address);
		return 0;
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		return RISCV_ERROR_TYPE_MACHINE_EXCEPTION;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
	}
	return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
}

extern "C"
int libriscv_copy_to_guest(RISCVMachine *m, uint64_t dst, const void *src, unsigned len)
{
	try {
		MACHINE(m)->copy_to_guest(dst, src, len);
		return 0;
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		return RISCV_ERROR_TYPE_MACHINE_EXCEPTION;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
	}
	return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
}
extern "C"
int libriscv_copy_from_guest(RISCVMachine *m, void* dst, uint64_t src, unsigned len)
{
	try {
		MACHINE(m)->copy_from_guest(dst, src, len);
		return 0;
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		return RISCV_ERROR_TYPE_MACHINE_EXCEPTION;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
	}
	return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
}

extern "C"
const char * libriscv_memstring(RISCVMachine *m, uint64_t src, unsigned maxlen, unsigned* length)
{
	if (length == nullptr)
		return nullptr;
	char *result = nullptr;

	try {
		const auto view = MACHINE(m)->memory.memstring_view(src, maxlen);
		*length = view.size();
		return view.data();
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
	}

	if (result)
		std::free(result);
	*length = 0;
	return nullptr;
}

extern "C"
const char * libriscv_memview(RISCVMachine *m, uint64_t src, unsigned length)
{
	try {
		auto buffer = MACHINE(m)->memory.memview(src, length);
		return buffer.data();
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
	}
	return nullptr;
}

extern "C"
char * libriscv_writable_memview(RISCVMachine *m, uint64_t src, unsigned length)
{
	try {
		auto buffer = MACHINE(m)->memory.writable_memview(src, length);
		return (char *)buffer.data();
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
	}
	return nullptr;
}

extern "C"
void libriscv_trigger_exception(RISCVMachine *m, unsigned exception, uint64_t data)
{
	MACHINE(m)->cpu.trigger_exception(exception, data);
}

extern "C"
int libriscv_load_binary_file(const char *filename, char **data)
{
	if (filename == NULL || data == NULL) {
		return -1;
	}

	std::string filename_cpp(filename);
	std::vector<uint8_t> loaded_file = load_binary_file(filename_cpp);
	size_t size = loaded_file.size() * sizeof(char);

	*data = (char *) malloc(size);
	if (*data == nullptr) {
		return -1;
	}

	std::copy(loaded_file.begin(), loaded_file.end(), *data);

	return size;
}


/*** Fast-fork API ***/

#ifdef RISCV_VIRTUAL_PAGING

extern "C"
RISCVMachine *libriscv_fast_fork(const RISCVMachine *parent, RISCVOptions *opts)
{
	auto* pm = CONST_MACHINE(parent);
	UserData *u = nullptr;
	try {
		const uint64_t fork_memory_max = (opts && opts->max_memory > 0)
			? opts->max_memory : pm->memory.memory_arena_size();
		auto* m = new Machine<RISCV_ARCH>(*pm, MachineOptions<RISCV_ARCH>{
			.memory_max = fork_memory_max,
			.minimal_fork = true,
			.use_memory_arena = false,
			.default_exit_function = "fast_exit",
#ifdef RISCV_BINARY_TRANSLATION
			.translate_enabled = true,
			.translation_use_arena = false,
#endif
		});

		u = new UserData {
			.error = opts ? opts->error : nullptr,
			.stdout = opts ? opts->stdout : nullptr,
			.opaque = opts ? opts->opaque : nullptr,
		};
		u->fork_parent = pm;
		u->max_owned_pages = fork_memory_max / Page::size();
		m->set_userdata(u);
		m->memory.set_stack_initial(m->cpu.reg(2) & ~0xFULL); // Align stack pointer to 16 bytes
		setup_printer(m);

		// Default page fault: heap-allocated pages with limit
		m->memory.set_page_fault_handler(
			[](auto& mem, size_t pageno, bool init) -> Page& {
				auto& ud = USERDATA((&mem.machine()));
				if (ud.max_owned_pages > 0
					&& ud.owned_page_count >= ud.max_owned_pages)
					throw MachineException(OUT_OF_MEMORY,
						"Fork page limit reached", pageno * Page::size());
				ud.owned_page_count++;
				return mem.allocate_page(pageno,
					init ? PageData::INITIALIZED : PageData::UNINITIALIZED);
			});

		// Default page read: install parent page as non-owning CoW
		m->memory.set_page_readf_handler(
			[](const Memory<RISCV_ARCH>& mem, auto pageno) -> const Page& {
				auto* ud = mem.machine().template get_userdata<UserData>();
				auto info = get_parent_page_readonly(*ud->fork_parent, pageno);
				info.attr.non_owning = true;
				info.attr.is_cow = info.attr.write;
				info.attr.write = false;
				return const_cast<Memory<RISCV_ARCH>&>(mem)
					.allocate_page(pageno, info.attr, info.data);
			});

		// Default page write: make writable copy with limit
		m->memory.set_page_write_handler(
			[](auto& mem, auto pageno, Page& page) {
				auto& ud = USERDATA((&mem.machine()));
				if (ud.max_owned_pages > 0
					&& ud.owned_page_count >= ud.max_owned_pages)
					throw MachineException(OUT_OF_MEMORY,
						"Fork write page limit reached", pageno * Page::size());
				page.make_writable();
				ud.owned_page_count++;
			});

		// Set up arena at parent's high watermark
		if (pm->has_arena()) {
			const auto& parent_ud = *pm->template get_userdata<UserData>();
			const auto watermark = pm->arena().high_watermark();
			const auto arena_base = pm->memory.heap_address();
			const auto total_size = parent_ud.arena_total_size;
			const auto syscall_base = parent_ud.arena_syscall_base;
			if (total_size > 0 && syscall_base > 0) {
				const auto remaining = total_size - (watermark - arena_base);
				m->setup_native_heap(syscall_base, watermark, remaining);

				m->arena().on_unknown_free(
					[](auto, auto*) -> int { return 0; });

				const Arena* src_arena = &pm->arena();
				m->arena().on_unknown_realloc(
					[src_arena](auto ptr, auto newsize) -> Arena::ReallocResult {
						const size_t old_len = src_arena->size(ptr);
						return {0, old_len}; // Caller must malloc separately
					});
				// Fix: the on_unknown_realloc needs access to the fork's arena
				// Re-install with a lambda that captures the fork machine
				auto* fork_machine = m;
				m->arena().on_unknown_realloc(
					[src_arena, fork_machine](auto ptr, auto newsize) -> Arena::ReallocResult {
						const size_t old_len = src_arena->size(ptr);
						const auto new_ptr = fork_machine->arena().malloc(newsize);
						return {new_ptr, old_len};
					});
			}
		}

		return (RISCVMachine *)m;
	}
	catch (const MachineException& me)
	{
		if (opts && opts->error)
			opts->error(opts->opaque, RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		delete u;
		return NULL;
	}
	catch (const std::exception& e)
	{
		if (opts && opts->error)
			opts->error(opts->opaque, RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
		delete u;
		return NULL;
	}
}

extern "C"
int libriscv_is_forked(const RISCVMachine *m)
{
	return CONST_MACHINE(m)->is_forked() ? 1 : 0;
}

extern "C"
const void *libriscv_get_parent_page_data(
	const RISCVMachine *parent, uint64_t pageno, RISCVPageAttributes *attr_out)
{
	try {
		auto info = get_parent_page_readonly(*CONST_MACHINE(parent), pageno);
		if (attr_out)
			*attr_out = convert_to_c(info.attr);
		return info.data;
	}
	catch (...) {
		return NULL;
	}
}

#else // RISCV_VIRTUAL_PAGING

extern "C"
RISCVMachine *libriscv_fast_fork(const RISCVMachine *, RISCVOptions *opts)
{
	if (opts && opts->error)
		opts->error(opts->opaque, RISCV_ERROR_TYPE_GENERAL_EXCEPTION,
			"Forking requires enabling virtual paging", 0);
	return NULL;
}

extern "C"
int libriscv_is_forked(const RISCVMachine *)
{
	return 0;
}

extern "C"
const void *libriscv_get_parent_page_data(
	const RISCVMachine *, uint64_t, RISCVPageAttributes *)
{
	return NULL;
}

#endif // RISCV_VIRTUAL_PAGING


/*** Arena management ***/

extern "C"
int libriscv_setup_arena(
	RISCVMachine *m, unsigned syscall_base, uint64_t addr, uint64_t size)
{
	try {
		MACHINE(m)->setup_native_heap(syscall_base, addr, size);
		auto& ud = USERDATA(MACHINE(m));
		ud.arena_syscall_base = syscall_base;
		ud.arena_total_size = size;
		return 0;
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		return RISCV_ERROR_TYPE_MACHINE_EXCEPTION;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
		return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
	}
}

extern "C"
int libriscv_has_arena(const RISCVMachine *m)
{
	return CONST_MACHINE(m)->has_arena() ? 1 : 0;
}

extern "C"
uint64_t libriscv_arena_malloc(RISCVMachine *m, uint64_t size)
{
	try {
		return MACHINE(m)->arena().malloc(size);
	} catch (...) {
		return 0;
	}
}

extern "C"
int libriscv_arena_free(RISCVMachine *m, uint64_t ptr)
{
	try {
		return MACHINE(m)->arena().free(ptr);
	} catch (...) {
		return -1;
	}
}

extern "C"
RISCVReallocResult libriscv_arena_realloc(
	RISCVMachine *m, uint64_t ptr, uint64_t new_size)
{
	try {
		auto [new_ptr, old_size] = MACHINE(m)->arena().realloc(ptr, new_size);
		return {new_ptr, old_size};
	} catch (...) {
		return {0, 0};
	}
}

extern "C"
uint64_t libriscv_arena_size(RISCVMachine *m, uint64_t ptr)
{
	try {
		return MACHINE(m)->arena().size(ptr);
	} catch (...) {
		return 0;
	}
}

extern "C"
uint64_t libriscv_arena_high_watermark(const RISCVMachine *m)
{
	try {
		return CONST_MACHINE(m)->arena().high_watermark();
	} catch (...) {
		return 0;
	}
}

extern "C"
void libriscv_arena_set_unknown_free(
	RISCVMachine *m, riscv_arena_unknown_free_t handler, void *user)
{
	try {
		MACHINE(m)->arena().on_unknown_free(
			[handler, user](auto ptr, auto*) -> int {
				return handler(ptr, user);
			});
	} catch (...) {}
}

extern "C"
void libriscv_arena_set_unknown_realloc(
	RISCVMachine *m, riscv_arena_unknown_realloc_t handler, void *user)
{
	try {
		MACHINE(m)->arena().on_unknown_realloc(
			[handler, user](auto ptr, auto newsize) -> Arena::ReallocResult {
				auto result = handler(ptr, newsize, user);
				return {(Arena::PointerType)result.ptr, result.old_size};
			});
	} catch (...) {}
}

extern "C"
int libriscv_transfer_arena(RISCVMachine *dst, const RISCVMachine *src)
{
	try {
		MACHINE(dst)->transfer_arena_from(*CONST_MACHINE(src));
		return 0;
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(dst), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		return RISCV_ERROR_TYPE_MACHINE_EXCEPTION;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(dst), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
		return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
	}
}


/*** Memory address queries ***/

extern "C"
uint64_t libriscv_heap_address(const RISCVMachine *m)
{
	return CONST_MACHINE(m)->memory.heap_address();
}

extern "C"
uint64_t libriscv_mmap_allocate(RISCVMachine *m, uint64_t bytes)
{
	try {
		return MACHINE(m)->memory.mmap_allocate(bytes);
	} catch (...) {
		return 0;
	}
}

extern "C"
uint64_t libriscv_stack_initial(const RISCVMachine *m)
{
	return CONST_MACHINE(m)->memory.stack_initial();
}

extern "C"
uint64_t libriscv_owned_pages_active(const RISCVMachine *m)
{
	return CONST_MACHINE(m)->memory.owned_pages_active();
}


/*** Shared / non-owned memory ***/

extern "C"
int libriscv_insert_non_owned_memory(
	RISCVMachine *m, uint64_t dst, void *src, uint64_t size,
	const RISCVPageAttributes *attr)
{
#ifndef RISCV_VIRTUAL_PAGING
	(void)dst; (void)src; (void)size; (void)attr;
	ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION,
		"Non-owned memory requires enabling virtual paging", 0);
	return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
#else
	try {
		PageAttributes cpp_attr;
		if (attr) {
			cpp_attr = convert_to_cpp(*attr);
		}
		MACHINE(m)->memory.insert_non_owned_memory(dst, src, size, cpp_attr);
		return 0;
	} catch (const MachineException& me) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_MACHINE_EXCEPTION, me.what(), me.data());
		return RISCV_ERROR_TYPE_MACHINE_EXCEPTION;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
		return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
	}
#endif // RISCV_VIRTUAL_PAGING
}


/*** Setup helpers ***/

extern "C"
int libriscv_setup_linux_syscalls(RISCVMachine *m, int filesystem, int sockets)
{
	try {
		MACHINE(m)->setup_linux_syscalls((bool)filesystem, (bool)sockets);
		return 0;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
		return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
	}
}

extern "C"
int libriscv_setup_posix_threads(RISCVMachine *m)
{
	try {
		MACHINE(m)->setup_posix_threads();
		return 0;
	} catch (const std::exception& e) {
		ERROR_CALLBACK(MACHINE(m), RISCV_ERROR_TYPE_GENERAL_EXCEPTION, e.what(), 0);
		return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
	}
}

extern "C"
int libriscv_setup_native_memory(unsigned syscall_base)
{
	try {
		Machine<RISCV_ARCH>::setup_native_memory(syscall_base);
		return 0;
	} catch (...) {
		return RISCV_ERROR_TYPE_GENERAL_EXCEPTION;
	}
}
