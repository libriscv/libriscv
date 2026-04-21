#pragma once
#include <functional>
#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

struct Script
{
	static constexpr int MARCH = 8; // 64-bit RISC-V

	using gaddr_t   = riscv::address_type<MARCH>;
	using sgaddr_t  = riscv::signed_address_type<MARCH>;
	using machine_t = riscv::Machine<MARCH>;
	using ghandler_t = std::function<void(Script&)>;

	static constexpr gaddr_t  MAX_MEMORY     = 16ULL << 20;   // 16 MB
	static constexpr gaddr_t  STACK_SIZE     = 2ULL << 20;    // 2 MB
	static constexpr gaddr_t  MAX_HEAP       = 8ULL << 20;    // 8 MB
	static constexpr uint64_t MAX_BOOT_INSTR = 32'000'000ULL;
	static constexpr uint64_t MAX_CALL_INSTR = 32'000'000ULL;
	static constexpr uint8_t  MAX_CALL_DEPTH = 8;

	static constexpr int HEAP_SYSCALLS_BASE   = 490;
	static constexpr int MEMORY_SYSCALLS_BASE = 495;

	struct HostFunctionDesc {
		uint32_t hash;
		uint32_t reserved;
		uint32_t strname;
		bool initialization_only;
		bool client_side_only;
		bool server_side_only;
		bool padding;
	};
	static_assert(sizeof(HostFunctionDesc) == 16);

	struct HostFunction {
		std::string name;
		std::string signature;
		ghandler_t  func;
	};

	Script(const std::string& name, const std::string& filename);
	~Script();

	template <typename... Args>
	std::optional<sgaddr_t> call(const std::string& func, Args&&... args);

	template <typename... Args>
	std::optional<sgaddr_t> call(gaddr_t addr, Args&&... args);

	gaddr_t address_of(const std::string& name) const;
	std::string symbol_name(gaddr_t address) const;

	auto& machine() { return *m_machine; }
	const auto& machine() const { return *m_machine; }
	const auto& name() const noexcept { return m_name; }

	gaddr_t guest_alloc(gaddr_t bytes);
	bool guest_free(gaddr_t addr);

	static void set_host_function(
		std::string name, std::string signature, ghandler_t handler);
	static std::size_t host_function_count() noexcept;
	static void setup_dispatch();

	void resolve_host_functions(bool initialization, bool client_side = false);

	std::vector<ghandler_t> m_host_function_array;
	Script* m_peer = nullptr;
	void set_peer(Script* peer) { m_peer = peer; }

private:
	void reset();
	void initialize();
	void machine_setup();
	static void register_host_functions();
	void handle_exception(gaddr_t);
	void handle_timeout(gaddr_t);
	void max_depth_exceeded(gaddr_t);

	static std::map<uint32_t, HostFunction> s_host_functions;

	std::unique_ptr<machine_t> m_machine;
	std::vector<uint8_t> m_binary;
	gaddr_t m_heap_area = 0;
	std::string m_name;
	uint8_t m_call_depth = 0;
	mutable std::unordered_map<std::string, gaddr_t> m_lookup_cache;
};

struct ScriptDepthMeter {
	ScriptDepthMeter(uint8_t& val) : m_val(++val) {}
	~ScriptDepthMeter() { m_val--; }
	uint8_t get() const noexcept { return m_val; }
	bool is_one() const noexcept { return m_val == 1; }
private:
	uint8_t& m_val;
};

// --- Inline call implementation ---

template <typename... Args>
inline std::optional<Script::sgaddr_t> Script::call(gaddr_t address, Args&&... args)
{
	ScriptDepthMeter meter(this->m_call_depth);
	try {
		if (meter.is_one())
			return { machine().template vmcall<MAX_CALL_INSTR>(
				address, std::forward<Args>(args)...) };
		else if (meter.get() < MAX_CALL_DEPTH)
			return { machine().preempt(MAX_CALL_INSTR,
				address, std::forward<Args>(args)...) };
		else
			this->max_depth_exceeded(address);
	} catch (const std::exception& e) {
		this->handle_exception(address);
	}
	return std::nullopt;
}

template <typename... Args>
inline std::optional<Script::sgaddr_t> Script::call(const std::string& func, Args&&... args)
{
	const auto address = this->address_of(func);
	if (address == 0x0) {
		fprintf(stderr, "Script::call(): Could not find '%s'\n", func.c_str());
		return std::nullopt;
	}
	return this->call(address, std::forward<Args>(args)...);
}

// --- Event: type-safe wrapper for calling guest functions ---

template <typename F = void()>
struct Event
{
	Event() = default;
	Event(Script& s, const std::string& func)
		: m_script(&s), m_addr(s.address_of(func)) {}
	Event(Script& s, Script::gaddr_t address)
		: m_script(&s), m_addr(address) {}

	bool is_callable() const noexcept {
		return m_script != nullptr && m_addr != 0x0;
	}

	template <typename... Args>
	auto operator()(Args&&... args) {
		return call(std::forward<Args>(args)...);
	}

	template <typename... Args>
	auto call(Args&&... args)
	{
		static_assert(std::is_invocable_v<F, Args...>);
		using Ret = decltype((F*){}(args...));

		if (is_callable()) {
			if (auto res = m_script->call(m_addr, std::forward<Args>(args)...)) {
				if constexpr (std::is_same_v<void, Ret>)
					return true;
				else
					return std::optional<Ret>(static_cast<Ret>(res.value()));
			}
		}
		if constexpr (std::is_same_v<void, Ret>)
			return false;
		else
			return std::optional<Ret>{std::nullopt};
	}

private:
	Script* m_script = nullptr;
	Script::gaddr_t m_addr = 0;
};
