#pragma once
#include "cpu.hpp"
#include "memory.hpp"
#include "riscvbase.hpp"
#include "posix/filedesc.hpp"
#include "posix/signals.hpp"
#include <array>
#include <string_view>

namespace riscv
{
	static constexpr int RISCV32  = 4; /* 32-bits CPU */
	static constexpr int RISCV64  = 8; /* 64-bits CPU */
	static constexpr int RISCV128 = 16; /* 128-bits CPU */

	// Machine is a RISC-V emulator. The W template parameter is
	// used to determine the bit-architecture, like so:
	// 32-bit:  Machine<RISCV32>, 64-bit:  Machine<RISCV64>
	// 128-bit: Machine<RISCV128>
	//
	// It is instantiated with an ELF binary that contains the
	// loaded RISC-V program to run:
	//
	//  std::vector<uint8_t> mybinary = load_file("riscv_program.elf");
	//  Machine<RISCV64> machine { mybinary };
	//  machine.setup_linux_syscalls();
	//  machine.setup_linux({"program", "arg0"}, {"LC_ALL=C"});
	//
	template <int W>
	struct Machine
	{
		using syscall_t = void(*)(Machine&);
		using address_t = address_type<W>; // one unsigned memory address
		using printer_func = void(*)(const Machine&, const char*, size_t);
		using stdin_func = long(*)(const Machine&, char*, size_t);
		using rdtime_func = uint64_t(*)(const Machine&);

		// See common.hpp for MachineOptions
		// The machine takes the binary as a const reference and does not
		// own it, instead the binary data must be kept alive with the machine
		// and not moved or reallocated.
		Machine(std::string_view binary, const MachineOptions<W>& = {});
		Machine(const std::vector<uint8_t>& bin, const MachineOptions<W>& = {});
		// Create empty Machine.
		Machine(const MachineOptions<W>& = {});
		// The forking constructor creates a new machine based on @main,
		// and loans all memory using Copy-on-Write mechanisms. Additionally,
		// all cached structures like execute segment, and the instruction cache
		// is also loaned. The main machine must not be destroyed or (in most cases)
		// modified while the fork is running. Forks consume very little resources.
		Machine(const Machine& main, const MachineOptions<W>& = {});
		~Machine();

		// Simulate RISC-V starting from the current address, and stopping when
		// at most @max_instructions have been executed. If Throw == true,
		// the machine will throw a MachineTimeoutException if it hits the
		// given instruction limit, but not if stopped normally.
		template <bool Throw = true>
		void simulate(uint64_t max_instructions = UINT64_MAX, uint64_t counter = 0u);

		// Resume simulation by extending the max instructions counter by
		// the given amount, and the simulating RISC-V as if calling
		// simulate(). The instruction counter will not be reset.
		template <bool Throw = true>
		void resume(uint64_t max_instructions);

		// Sets the max instructions counter to zero, which effectively
		// causes the machine to stop. instruction_limit_reached() will return
		// false indicating that the machine did not stop because an instruction
		// limit was reached, and instead stopped naturally.
		void stop() noexcept;
		// Returns true if the machine is stopped, including when the
		// instruction limit was reached.
		bool stopped() const noexcept;
		// This function returns true only when a simulation ended caused by
		// reaching the instruction limit. It will not be true if the machine
		// stopped normally. See: machine.stopped() for that.
		bool instruction_limit_reached() const noexcept;

		// Returns the precise number of instructions executed. Should only
		// be called after simulation ends, or inside a system call handler.
		uint64_t instruction_counter() const noexcept { return m_counter; }
		void     set_instruction_counter(uint64_t val) noexcept { m_counter = val; }
		void     increment_counter(uint64_t val) noexcept { m_counter += val; }
		void     reset_instruction_counter() noexcept { m_counter = 0; }
		void     penalize(uint64_t val) noexcept;
		uint64_t max_instructions() const noexcept { return m_max_counter; }
		void     set_max_instructions(uint64_t val) noexcept { m_max_counter = val; }

		CPU<W>    cpu;
		Memory<W> memory;

		// Copy data into the guests memory (*with* page protections).
		void copy_to_guest(address_t dst, const void* buf, size_t len);
		// Copy data from the guests memory (*with* page protections).
		void copy_from_guest(void* dst, address_t buf, size_t len);

		// Push all strings on stack and then create a mini-argv on SP.
		void setup_argv(const std::vector<std::string>& args, const std::vector<std::string>& env = {});
		// Full Linux-compatible stack with program headers.
		void setup_linux(const std::vector<std::string>& args, const std::vector<std::string>& env = {});

		// Retrieve arguments during a system call
		// Example: auto arg0 = machine.sysarg <int> (0);
		template <typename T = address_t>
		inline T sysarg(int arg) const;
		// Retrieve all arguments by given types during a system call
		// Example: auto [a, b] = machine.sysargs <int, address_type<W>> ();
		template <typename... Args>
		inline auto sysargs() const;

		// Set the result of a system or function call
		// Only supports primitive types like integers and floats
		// Example: machine.set_result <int, int, float> (123, 456, 789.0f);
		template <typename... Args>
		inline void set_result(Args... args);

		// Forward the result of a C library function call that
		// returns 0 or positive on success, and -1 on failure. errno
		// will be passed on to the guest on failure.
		void set_result_or_error(int);

		// A shortcut to getting a return or exit value by interpreting A0
		template <typename T = address_t>
		inline T return_value() const { return sysarg<T> (0); }

		// Calls into the virtual machine, returning the value returned from
		// @function_name, which must be visible in the ELF symbol tables.
		// The function must use the C ABI calling convention.
		template<uint64_t MAXI = UINT64_MAX, bool Throw = true, typename... Args> constexpr
		address_t vmcall(const char* func_name, Args&&... args);

		template<uint64_t MAXI = UINT64_MAX, bool Throw = true, typename... Args> constexpr
		address_t vmcall(address_t func_addr, Args&&... args);

		// Saves and restores registers while calling given function
		// Uses resume() to execute the other function, continuing instruction counting.
		template<bool Throw = true, bool StoreRegs = true, typename... Args>
		address_t preempt(uint64_t max_instr, const char* func_name, Args&&... args);

		template<bool Throw = true, bool StoreRegs = true, typename... Args>
		address_t preempt(uint64_t max_instr, address_t func_addr, Args&&... args);

		// Sets up a function call only, executes no instructions.
		// Supports integers, floating-point values and strings.
		// Strings will be put on stack, which is not restored automatically.
		template<typename... Args> constexpr
		void setup_call(address_t call_addr, Args&&... args);

		// Returns the address of a symbol in the ELF symtab, or zero
		address_t address_of(std::string_view name) const;

		// Custom user pointer
		template <typename T> void set_userdata(T* data) { m_userdata = data; }
		template <typename T> T* get_userdata() const noexcept { return static_cast<T*> (m_userdata); }

		// Stdout, stderr (for when the guest wants to write)
		void print(const char*, size_t) const;
		auto& get_printer() const noexcept { return m_printer; }
		void set_printer(printer_func pf = default_printer) const { m_printer = pf; }
		// Stdin (for when the guest wants to read)
		long stdin_read(char*, size_t) const;
		auto& get_stdin() const noexcept { return m_stdin; }
		void set_stdin(stdin_func sin = default_stdin) const { m_stdin = sin; }
		// Debug printer (for when the machine wants to inform)
		void debug_print(const char*, size_t) const;
		auto& get_debug_printer() const noexcept { return m_debug_printer; }
		void set_debug_printer(printer_func pf = default_printer) const { m_debug_printer = pf; }
		// Monotonic time function (used by RDTIME and RDTIMEH)
		uint64_t rdtime() const { return m_rdtime(*this); }
		auto& get_rdtime() const noexcept { return m_rdtime; }
		void set_rdtime(rdtime_func tf = default_rdtime) const { m_rdtime = tf; }

		// Push something onto the stack, moving the current stack pointer.
		address_t stack_push(const void* data, size_t length);
		address_t stack_push(const std::string& string);
		template <typename T>
		address_t stack_push(const T& pod_type);
		// Realign the stack pointer, to make sure that function calls succeed
		void realign_stack();

		// Invoke an installed system call handler at the given index (system call number).
		void system_call(size_t);
		// Invoke the EBREAK system function
		void ebreak();
		static void install_syscall_handler(size_t, syscall_t);
		static void install_syscall_handlers(std::initializer_list<std::pair<size_t, syscall_t>>);

		static void unknown_syscall_handler(Machine<W>&);
		static constexpr auto initialize_syscalls() noexcept {
			std::array<syscall_t, RISCV_SYSCALLS_MAX> arr;
			for (auto& h : arr) h = unknown_syscall_handler;
			return arr;
		}
		// A fixed-size array of system call handlers
		static inline std::array<syscall_t, RISCV_SYSCALLS_MAX>
			syscall_handlers = initialize_syscalls();
		// Callback for unimplemented system calls (default: see machine.cpp)
		static void default_unknown_syscall_no(Machine&, size_t);
		static inline void (*on_unhandled_syscall) (Machine&, size_t) = default_unknown_syscall_no;

		// Execute CSRs
		void system(union rv32i_instruction);
		// User callback for unhandled CSRs
		static inline void (*on_unhandled_csr) (Machine&, int, int, int)
			= [] (Machine<W>&, int, int, int) {};

		// multiprocess() executes a single function with many machines,
		// each of which uses memory pages from this machine. Using this
		// we can partition workloads and work on them concurrently.
		bool is_multiprocessing() const noexcept;
		bool multiprocess(unsigned cpus, uint64_t maxi, address_t stack, address_t stksize,
			std::function<void(Machine&)> per_machine_setup_cb = nullptr);
		uint32_t multiprocess_wait();

		// Returns true if this machine is forked from another, and thus
		// dependent on the original machine to function properly.
		bool is_forked() const noexcept { return memory.is_forked(); }

		// Optional custom native-performance arena
		const Arena& arena() const;
		Arena& arena();
		void setup_native_heap(size_t sysnum, uint64_t addr, size_t size);
		void transfer_arena_from(const Machine& other);
		// Optional custom memory-related system calls
		static void setup_native_memory(size_t sysnum);

		// System calls, files and threads implementations
		bool has_file_descriptors() const noexcept { return m_fds != nullptr; }
		// The "minimum": lseek, read, write, exit (provided for example usage)
		static void setup_minimal_syscalls();
		// Enough to run minimal newlib programs
		static void setup_newlib_syscalls();
		// Set up every supported system call, emulating Linux
		void setup_linux_syscalls(bool filesystem = true, bool sockets = true);
		void setup_posix_threads();
		void setup_native_threads(const size_t syscall_base);
		// Threads: Access to thread internal structures
		const MultiThreading<W>& threads() const;
		MultiThreading<W>& threads();
		bool has_threads() const noexcept { return this->m_mt != nullptr; }
		int gettid() const;
		// FileDescriptors: Access to translation between guest fds
		// and real system fds. The destructor also closes all opened files.
		const FileDescriptors& fds() const;
		FileDescriptors& fds();
		// Multiprocessing structure, lazily created
		Multiprocessing<W>& smp(unsigned workers = 4);
		// Signal structure, lazily created
		Signals<W>& signals();
		SignalAction<W>& sigaction(int sig) { return signals().get(sig); }

		// Resets the machine to the initial state. It is, however, not a
		// reliable way to reset complex machines with all kinds of features
		// attached to it, and should almost never be used. It is recommended
		// to create a new machine instead, or rely on forking to facilitate
		// quickly creating and destroying a machine.
		void reset();

		// Serializes the current machine state to @vec
		size_t serialize_to(std::vector<uint8_t>& vec) const;
		// Returns the machine to a previously stored state
		// NOTE: All previous memory traps are lost, syscall handlers,
		// destructor callbacks are kept. Page fault handler and
		// symbol lookup cache is also kept. Returns 0 on success.
		int deserialize_from(const std::vector<uint8_t>&);

		std::pair<uint64_t&, uint64_t&> get_counters() noexcept { return {m_counter, m_max_counter}; }
	private:
		template<typename... Args, std::size_t... indices>
		auto resolve_args(std::index_sequence<indices...>) const;
		static void setup_native_heap_internal(const size_t);
		void timeout_exception(uint64_t);

		uint64_t     m_counter = 0;
		uint64_t     m_max_counter = 0;
		mutable void*        m_userdata = nullptr;
		mutable printer_func m_printer = default_printer;
		mutable printer_func m_debug_printer = default_printer;
		mutable stdin_func   m_stdin = default_stdin;
		mutable rdtime_func  m_rdtime = default_rdtime;
		std::unique_ptr<Arena> m_arena;
		std::unique_ptr<MultiThreading<W>> m_mt = nullptr;
		std::unique_ptr<FileDescriptors> m_fds = nullptr;
		std::unique_ptr<Multiprocessing<W>> m_smp = nullptr;
		std::unique_ptr<Signals<W>> m_signals = nullptr;
		static_assert((W == 4 || W == 8 || W == 16), "Must be either 32-bit, 64-bit or 128-bit ISA");
		static void default_printer(const Machine&, const char*, size_t);
		static long default_stdin(const Machine&, char*, size_t);
		static uint64_t default_rdtime(const Machine&);
	};

#include "machine_inline.hpp"
}
