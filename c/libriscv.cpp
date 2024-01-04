#include "libriscv.h"

#include <libriscv/machine.hpp>

using namespace riscv;
#define RISCV_ARCH  RISCV64
#define MACHINE(x) ((Machine<RISCV_ARCH> *)x)
#define ERROR_CALLBACK(m, type, msg, data) \
	if (auto *usr = m->get_userdata<UserData> (); usr->error != nullptr) \
		usr->error(usr->opaque, type, msg, data);

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
};

extern "C"
void libriscv_set_defaults(RISCVOptions *options)
{
	MachineOptions<RISCV_ARCH> mo;

	options->max_memory = mo.memory_max;
	options->stack_size = mo.stack_size;
	options->strict_sandbox = true;
	options->argc = 0;
}

extern "C"
RISCVMachine *libriscv_new(const void *elf_prog, unsigned elf_length, RISCVOptions *options)
{
	MachineOptions<RISCV_ARCH> mo {
		.memory_max = options->max_memory,
		.stack_size = options->stack_size,
	};
	UserData *u = nullptr;
	try {
		auto view = std::string_view{(const char *)elf_prog, size_t(elf_length)};

		auto* m = new Machine<RISCV_ARCH> { view, mo };
		u = new UserData {
			.error = options->error, .stdout = options->stdout, .opaque = options->opaque
		};
		m->set_userdata(u);
		m->set_printer([] (auto& m, const char* data, size_t size) {
			auto& userdata = (*m.template get_userdata<UserData> ());
			if (userdata.stdout)
				userdata.stdout(userdata.opaque, data, size);
			else
				printf("%.*s", (int)size, data);
		});

		if (options->argc > 0) {
			std::vector<std::string> args = fill(options->argc, options->argv);
			std::vector<std::string> env = {"LC_CTYPE=C", "LC_ALL=C", "USER=groot"};

			m->setup_linux_syscalls();
			m->setup_posix_threads();
			m->setup_linux(args, env);
			m->fds().permit_filesystem = !options->strict_sandbox;
			m->fds().permit_sockets = !options->strict_sandbox;
			// TODO: File permissions
		}

		return (RISCVMachine *)m;
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
		return MACHINE(m)->simulate<false>(instruction_limit) ? 0 : -RISCV_ERROR_TYPE_MACHINE_TIMEOUT;
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
char * libriscv_memstring(RISCVMachine *m, uint64_t src, unsigned maxlen, unsigned* length)
{
	if (length == nullptr)
		return nullptr;
	char *result = nullptr;

	try {
		const unsigned len = MACHINE(m)->memory.strlen(src, maxlen);
		result = (char *)std::malloc(len);
		if (result != nullptr) {
			MACHINE(m)->copy_from_guest(result, src, len);
			*length = len;
		}
		return result;
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
		auto buffer = MACHINE(m)->memory.rvbuffer(src, length);
		if (buffer.is_sequential()) {
			return buffer.data();
		}
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
