#pragma once
/**
 * A minimal embedding of libriscv: one VM running a static Rust binary, a native
 * heap that the guest's #[global_allocator] allocates out of, and three
 * syscall-based host functions.
 *
 * The native heap is what makes the zero-copy path possible at all: the arena
 * the host allocates from is the arena the guest frees to, so a tree the host
 * builds in guest memory can be owned -- and dropped -- by the guest.
 */
#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>
#include <memory>
#include <string>
#include <vector>

struct Script
{
	static constexpr int MARCH = 8; // 64-bit RISC-V

	using gaddr_t   = riscv::address_type<MARCH>;
	using sgaddr_t  = riscv::signed_address_type<MARCH>;
	using machine_t = riscv::Machine<MARCH>;

	static constexpr gaddr_t  MAX_MEMORY     = 64ULL << 20;
	static constexpr gaddr_t  STACK_SIZE     = 1ULL << 20;
	static constexpr gaddr_t  MAX_HEAP       = 48ULL << 20;
	static constexpr uint64_t MAX_BOOT_INSTR = 64'000'000ULL;
	static constexpr uint64_t MAX_CALL_INSTR = 2'000'000'000ULL;

	// Host functions, as syscall numbers. The native heap owns 490..503, and
	// guest/src/env.rs allocates through it.
	static constexpr size_t SYSCALL_OUT_FLAT = 480;
	static constexpr size_t SYSCALL_OUT_OBJ  = 481;
	static constexpr size_t SYSCALL_PRINT    = 482;
	static constexpr size_t HEAP_SYSCALLS_BASE = 490;

	Script(const std::string& name, const std::string& filename);

	/// @brief Call a guest function. The instruction counter is reset by every
	/// vmcall, so instructions() afterwards is the cost of this call.
	template <typename... Args>
	sgaddr_t call(const std::string& func, Args&&... args);
	template <typename... Args>
	sgaddr_t call(gaddr_t addr, Args&&... args);

	gaddr_t address_of(const std::string& name) const;

	auto& machine() { return *m_machine; }
	const auto& machine() const { return *m_machine; }
	const auto& name() const noexcept { return m_name; }

	gaddr_t guest_alloc(gaddr_t bytes);
	bool guest_free(gaddr_t addr);

	uint64_t instructions() const noexcept { return m_machine->instruction_counter(); }
	unsigned allocations() const noexcept { return m_machine->arena().allocation_counter(); }
	unsigned deallocations() const noexcept { return m_machine->arena().deallocation_counter(); }

private:
	void machine_setup();
	static void install_host_functions();

	std::vector<uint8_t> m_binary;
	std::unique_ptr<machine_t> m_machine;
	std::string m_name;
	gaddr_t m_heap_area = 0;
};

template <typename... Args>
inline Script::sgaddr_t Script::call(gaddr_t address, Args&&... args)
{
	return machine().vmcall<MAX_CALL_INSTR>(address, std::forward<Args>(args)...);
}

template <typename... Args>
inline Script::sgaddr_t Script::call(const std::string& func, Args&&... args)
{
	const auto address = this->address_of(func);
	if (address == 0x0)
		throw std::runtime_error("Script::call(): no such guest function: " + func);
	return this->call(address, std::forward<Args>(args)...);
}
