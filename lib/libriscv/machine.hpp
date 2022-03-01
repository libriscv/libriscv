#pragma once
#include "cpu.hpp"
#include "memory.hpp"
#include "riscvbase.hpp"
#include "posix_filedesc.hpp"
#include "signals.hpp"
#include <array>

namespace riscv
{
	static constexpr int RISCV32  = 4;
	static constexpr int RISCV64  = 8;
	static constexpr int RISCV128 = 16;

	// Machine is a RISC-V emulator. The W template parameter is
	// used to determine the bit-architecture, like so:
	// 32-bit:  Machine<RISCV32>, 64-bit:  Machine<RISCV64>
	// 128-bit: Machine<RISCV128>
	//
	// It is instantiated with an ELF binary that contains the
	// *statically* built RISC-V program to run:
	//
	//  std::vector<uint8_t> mybinary = load_file("riscv_program.elf");
	//  Machine<RISCV64> machine { mybinary };
	//
	template <int W>
	struct Machine
	{
		using syscall_t = void(*)(Machine&);
		using address_t = address_type<W>; // one unsigned memory address
		using printer_func = std::function<void(const char*, size_t)>;

		// See common.hpp for MachineOptions
		Machine(std::string_view binary, const MachineOptions<W>& = {});
		Machine(const std::vector<uint8_t>& bin, const MachineOptions<W>& = {});
		Machine(const Machine&, const MachineOptions<W>& = {}); //<- Fork
		~Machine();

		// Simulate a RISC-V machine until @max_instructions have been
		// executed, or the machine has been stopped.
		template <bool Throw = true>
		void simulate(uint64_t max_instructions = UINT64_MAX);

		void stop() noexcept;
		bool stopped() const noexcept;
		void reset();

		uint64_t instruction_counter() const noexcept { return m_counter; }
		void     set_instruction_counter(uint64_t val) noexcept { m_counter = val; }
		void     increment_counter(uint64_t val) noexcept { m_counter += val; }
		void     reset_instruction_counter() noexcept { m_counter = 0; }
		uint64_t max_instructions() const noexcept { return m_max_counter; }
		void     set_max_instructions(uint64_t val) noexcept { m_max_counter = val; }

		CPU<W>    cpu;
		Memory<W> memory;

		// Copy data into the guests memory (*without* page protections)
		void copy_to_guest(address_t dst, const void* buf, size_t len);
		// Copy data from the guests memory (*with* page protections)
		void copy_from_guest(void* dst, address_t buf, size_t len);
		// Push something onto the stack, and move the stack pointer
		address_t stack_push(const void* data, size_t length);
		address_t stack_push(const std::string& string);
		template <typename T>
		address_t stack_push(const T& pod_type);

		// Push all strings on stack and then create a mini-argv on SP
		void setup_argv(const std::vector<std::string>& args, const std::vector<std::string>& env = {});
		// Full Linux-compatible stack with program headers
		void setup_linux(const std::vector<std::string>& args, const std::vector<std::string>& env = {});

		// Retrieve arguments during a system call
		template <typename T>
		inline T sysarg(int arg) const;

		// Retrieve all arguments by given types during a system call
		template <typename... Args>
		inline auto sysargs() const;

		// Set the result of a system or function call
		// Only supports primitive types like integers and floats
		template <typename... Args>
		inline void set_result(Args... args);

		// Forward the result of a C library function call that
		// returns 0 or positive on success, and -1 on failure. errno
		// will be passed on to the guest on failure.
		void set_result_or_error(int);

		// A shortcut to getting a return or exit value
		template <typename T>
		inline T return_value() const { return sysarg<T> (0); }

		// Calls into the virtual machine, returning the value returned from
		// @function_name, which must be visible in the ELF symbol tables.
		// the function must use the C ABI calling convention.
		template<uint64_t MAXI = UINT64_MAX, bool Throw = true, typename... Args> constexpr
		address_t vmcall(const char* func_name, Args&&... args);

		template<uint64_t MAXI = UINT64_MAX, bool Throw = true, typename... Args> constexpr
		address_t vmcall(address_t func_addr, Args&&... args);

		// Saves and restores registers while calling given function
		template<uint64_t MAXI = UINT64_MAX, bool Throw = true, bool StoreRegs = true, typename... Args>
		address_t preempt(const char* func_name, Args&&... args);

		template<uint64_t MAXI = UINT64_MAX, bool Throw = true, bool StoreRegs = true, typename... Args>
		address_t preempt(address_t func_addr, Args&&... args);

		// Sets up a function call only, executes no instructions.
		// Supports integers, floating-point values and strings.
		// Strings will be put on stack, which is not restored automatically.
		template<typename... Args> constexpr
		void setup_call(address_t call_addr, Args&&... args);

		// multiprocess() executes a single function with many machines,
		// each of which uses memory pages from this machine. Using this
		// we can partition workloads and work on them concurrently.
		bool is_multiprocessing() const noexcept;
		bool multiprocess(unsigned cpus, uint64_t maxi, address_t stack, address_t stksize, bool fork = false);
		void multiprocess_wait();

		// Returns the address of a symbol in the ELF symtab, or zero
		address_t address_of(const char* name) const;
		address_t address_of(const std::string& name) const;

#ifdef RISCV_DEBUG
		// Immediately block execution, print registers and current instruction.
		void print_and_pause();
		bool verbose_instructions = false;
		bool verbose_jumps     = false;
		bool verbose_registers = false;
		bool verbose_fp_registers = false;
#else
		static constexpr bool verbose_instructions = false;
		static constexpr bool verbose_jumps     = false;
		static constexpr bool verbose_registers = false;
#endif

		// Custom user pointer
		template <typename T> void set_userdata(T* data) { m_userdata = data; }
		template <typename T> T* get_userdata() { return static_cast<T*> (m_userdata); }

		// Stdout, stderr
		void print(const char*, size_t) const;
		auto& get_printer() const noexcept { return m_printer; }
		void set_printer(printer_func pf = m_default_printer) { m_printer = std::move(pf); }
		// Stdin
		void stdin(const char*, size_t) const;
		auto& get_stdin() const noexcept { return m_stdin; }
		void set_stdin(printer_func sin) { m_stdin = std::move(sin); }

		// Call an installed system call handler
		void system_call(size_t);
		void unchecked_system_call(size_t);
		void ebreak();
		static void install_syscall_handler(size_t, syscall_t);
		static void install_syscall_handlers(std::initializer_list<std::pair<size_t, syscall_t>>);

		static constexpr auto initialize_syscalls() noexcept {
			std::array<syscall_t, RISCV_SYSCALLS_MAX> arr;
			for (auto& h : arr) h = unknown_syscall_handler;
			return arr;
		}
		static inline std::array<syscall_t, RISCV_SYSCALLS_MAX>
			syscall_handlers = initialize_syscalls();
		static inline void (*on_unhandled_syscall) (Machine&, int)
			= [] (Machine<W>&, int) {};

		// Execute CSRs
		void system(union rv32i_instruction);

		static inline void (*on_unhandled_csr) (Machine&, int, int, int)
			= [] (Machine<W>&, int, int, int) {};

		// Optional custom native-performance arena
		const Arena& arena() const;
		Arena& arena();
		void setup_native_heap(size_t sysnum, uint64_t addr, size_t size);
		// Optional custom memory-related system calls
		void setup_native_memory(size_t sysnum, bool safe = true);

		// System calls, files and threads implementations
		bool has_file_descriptors() const noexcept { return m_fds != nullptr; }
		// The "minimum": lseek, read, write, exit (provided for example usage)
		void setup_minimal_syscalls();
		// Enough to run minimal newlib programs
		void setup_newlib_syscalls();
		// Set up every supported system call, emulating Linux
		void setup_linux_syscalls(bool filesystem = true, bool sockets = true);
		void setup_posix_threads();
		void setup_native_threads(const size_t syscall_base);
		// Threads: Access to thread internal structures
		const MultiThreading<W>& threads() const noexcept { return *m_mt; }
		MultiThreading<W>& threads() noexcept { return *m_mt; }
		int gettid() const;
		// FileDescriptors: Access to translation between guest fds
		// and real system fds. The destructor also closes all opened files.
		const FileDescriptors& fds() const;
		FileDescriptors& fds();
		// Multiprocessing structure, lazily created
		Multiprocessing<W>& smp();
		// Signal structure, lazily created
		Signals<W>& signals() {
			if (m_signals == nullptr) m_signals.reset(new Signals<W>);
			return *m_signals;
		}
		SignalAction<W>& sigaction(int sig) { return signals().signals.at(sig-1); }

		// Realign the stack pointer, to make sure that function calls succeed
		void realign_stack();

		// Returns true if the Machine has loaded native code
		// generated from binary translation.
		bool is_binary_translated() const { return memory.is_binary_translated(); }

		// Serializes all the machine state + a tiny header to @vec
		void serialize_to(std::vector<uint8_t>& vec);
		// Returns the machine to a previously stored state
		// NOTE: All previous memory traps are lost, syscall handlers,
		// destructor callbacks are kept. Page fault handler and
		// symbol lookup cache is also kept. Returns 0 on success.
		int deserialize_from(const std::vector<uint8_t>&);

	private:
		static void unknown_syscall_handler(Machine<W>&);
		template<typename... Args, std::size_t... indices>
		auto resolve_args(std::index_sequence<indices...>) const;
		void setup_native_heap_internal(const size_t);
		void timeout_exception(uint64_t);

		uint64_t     m_counter = 0;
		uint64_t     m_max_counter = 0;
		void*        m_userdata = nullptr;
		printer_func m_printer = m_default_printer;
		printer_func m_stdin = [] (const char*, size_t) {};
		std::unique_ptr<Arena> m_arena;
		std::unique_ptr<MultiThreading<W>> m_mt;
		std::unique_ptr<FileDescriptors> m_fds;
		std::unique_ptr<Multiprocessing<W>> m_smp = nullptr;
		std::unique_ptr<Signals<W>> m_signals = nullptr;
		const unsigned m_multiprocessing_workers;
		static_assert((W == 4 || W == 8 || W == 16), "Must be either 32-bit, 64-bit or 128-bit ISA");
		static printer_func m_default_printer;
	};

#include "machine_inline.hpp"
}
