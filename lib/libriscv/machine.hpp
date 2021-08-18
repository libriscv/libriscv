#pragma once
#include "cpu.hpp"
#include "memory.hpp"
#include "riscvbase.hpp"
#include <array>

namespace riscv
{
	static constexpr int RISCV32 = 4;
	static constexpr int RISCV64 = 8;

	template <int W>
	struct Machine
	{
		using syscall_t = Function<void(Machine&)>;
		using syscall_fptr_t = void(*)(Machine&);
		using address_t = address_type<W>; // one unsigned memory address

		// see common.hpp for MachineOptions
		Machine(std::string_view binary, MachineOptions<W> = {});
		Machine(const std::vector<uint8_t>& bin, MachineOptions<W> = {});
		Machine(const Machine&, MachineOptions<W> = {}); //<- Fork
		~Machine();

		// Simulate a RISC-V machine until @max_instructions have been
		// executed, or the machine has been stopped.
		// NOTE: if @max_instructions is 0, then run until stop
		template <bool Throw = false>
		void simulate(uint64_t max_instructions = 0);

		void stop(bool v = true) noexcept;
		bool stopped() const noexcept;
		void reset();

		uint64_t instruction_counter() const noexcept { return m_counter; }
		void     increment_counter(uint64_t val) noexcept { m_counter += val; }
		void     reset_instruction_counter() noexcept { m_counter = 0; }

		CPU<W>    cpu;
		Memory<W> memory;

		// Copy data into the guests memory (without page protections)
		address_t copy_to_guest(address_t dst, const void* buf, size_t length);
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

		// Calls into the virtual machine, returning the value returned from
		// @function_name, which must be visible in the ELF symbol tables.
		// the function must use the C ABI calling convention.
		// The value of machine.stopped() should be false if the machine
		// reached max instructions without completing the function call.
		// Supports integers, floating-point values and strings.
		// Passing 0 to max instructions will disable the limit, and potentially
		// run forever.
		// NOTE: relies on an exit function to stop execution after returning.
		// _exit must call the exit (93) system call and not call destructors,
		// which is the norm.
		template<uint64_t MAXI = 0, bool Throw = true, typename... Args> constexpr
		address_t vmcall(const char* func_name, Args&&... args);

		template<uint64_t MAXI = 0, bool Throw = true, typename... Args> constexpr
		address_t vmcall(address_t func_addr, Args&&... args);

		// Saves and restores registers before calling
		template<uint64_t MAXI = 0, bool Throw = true, bool StoreRegs = true, typename... Args>
		address_t preempt(const char* func_name, Args&&... args);

		template<uint64_t MAXI = 0, bool Throw = true, bool StoreRegs = true, typename... Args>
		address_t preempt(address_t func_addr, Args&&... args);

		// Sets up a function call only, executes no instructions.
		// Supports integers, floating-point values and strings.
		// Strings will be put on stack, which is not restored automatically.
		template<typename... Args> constexpr
		void setup_call(address_t call_addr, Args&&... args);

		// returns the address of a symbol in the ELF symtab, or zero
		address_t address_of(const char* name) const;
		address_t address_of(const std::string& name) const;

		// Call a function when the machine gets destroyed
		void add_destructor_callback(Function<void()> callback) const;

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

		// Call an installed system call handler
		void system_call(size_t);
		void unchecked_system_call(size_t);
		void ebreak();
		static void install_syscall_handler(size_t, const syscall_t&);
		static void install_syscall_handlers(std::initializer_list<std::pair<size_t, syscall_t>>);

		static inline std::array<syscall_t, RISCV_SYSCALLS_MAX>
			syscall_handlers = {};
		static inline Function<void(Machine&, int)>
			on_unhandled_syscall = [] (Machine<W>&, int) {};

		// Execute CSRs
		void system(union rv32i_instruction);

		static inline Function<void(Machine&, int, int, int)>
			on_unhandled_csr = [] (Machine<W>&, int, int, int) {};

		// Optional custom native-performance arena
		const Arena& arena() const noexcept { return *m_arena; }
		Arena& arena() noexcept { return *m_arena; }
		void setup_native_heap(size_t sysnum, uint64_t addr, size_t size);
		// Optional custom memory-related system calls
		void setup_native_memory(size_t sysnum, bool safe = true);

		// Optional posix and custom threads implementations
		const MultiThreading<W>& threads() const noexcept { return *m_mt; }
		MultiThreading<W>& threads() noexcept { return *m_mt; }
		void setup_posix_threads();
		void setup_native_threads(const size_t syscall_base);

		// Realign the stack pointer, to make sure that function calls succeed
		void realign_stack();

		// Returns true if the guest environment contains native code
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
		static long unknown_syscall_handler(Machine<W>&);
		template<typename... Args, std::size_t... indices>
		auto resolve_args(std::index_sequence<indices...>) const;
		void setup_native_heap_internal(const size_t);

		bool         m_stopped = false;
		uint64_t     m_counter = 0;
		mutable std::vector<Function<void()>> m_destructor_callbacks;
		void* m_userdata = nullptr;
		std::unique_ptr<Arena> m_arena;
		std::unique_ptr<MultiThreading<W>> m_mt;
		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
	};

#include "machine_inline.hpp"
}
