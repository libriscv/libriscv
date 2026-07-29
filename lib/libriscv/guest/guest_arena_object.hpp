#pragma once
#include "guest_common.hpp"
#include <string_view>
#include <unordered_map>
#include <vector>

namespace riscv {

namespace detail {
	/// @brief True when object.set(machine, self, arg) is valid, which is how
	/// a variant is given a new alternative.
	template <int W, typename T, typename Arg, typename = void>
	struct has_guest_set : std::false_type {};
	template <int W, typename T, typename Arg>
	struct has_guest_set<W, T, Arg, std::void_t<decltype(std::declval<T&>().set(
		std::declval<Machine<W>&>(), std::declval<address_type<W>>(), std::declval<const Arg&>()))>>
		: std::true_type {};

	/// @brief True when object.set_string(machine, self, data, len) is valid
	template <int W, typename T, typename = void>
	struct has_guest_set_string : std::false_type {};
	template <int W, typename T>
	struct has_guest_set_string<W, T, std::void_t<decltype(std::declval<T&>().set_string(
		std::declval<Machine<W>&>(), std::declval<address_type<W>>(),
		std::declval<const void*>(), std::declval<std::size_t>()))>>
		: std::true_type {};

	/// @brief True when object.assign(machine, host_vector) is valid
	template <int W, typename T, typename U, typename = void>
	struct has_guest_vector_assign : std::false_type {};
	template <int W, typename T, typename U>
	struct has_guest_vector_assign<W, T, U, std::void_t<decltype(std::declval<T&>().assign(
		std::declval<Machine<W>&>(), std::declval<const std::vector<U>&>()))>>
		: std::true_type {};

	/// @brief True when object.assign(machine, self, host_map) is valid
	template <int W, typename T, typename Map, typename = void>
	struct has_guest_map_assign : std::false_type {};
	template <int W, typename T, typename Map>
	struct has_guest_map_assign<W, T, Map, std::void_t<decltype(std::declval<T&>().assign(
		std::declval<Machine<W>&>(), std::declval<address_type<W>>(), std::declval<const Map&>()))>>
		: std::true_type {};
} // detail

/// @brief Owns a T in the guest arena, and frees both the object and every
/// guest allocation it holds when it goes out of scope. Works with any guest
/// container: what the constructor and the assignments do is decided by the
/// traits in guest_common.hpp, not by a list of known types.
template <int W, typename T>
struct ScopedArenaObject {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	template <typename... Args>
	ScopedArenaObject(machine_t& machine, Args&&... args)
		: m_machine(&machine)
	{
		this->m_addr = m_machine->arena().malloc(sizeof(T));
		if (this->m_addr == 0) {
			throw std::bad_alloc();
		}
		this->m_ptr = m_machine->memory.template memarray<T>(this->m_addr, 1);
		if constexpr (guest_object_needs_self_address<W, T>::value) {
			// Containers that place their contents inline, or that point back
			// into themselves, are built at their final address right away
			new (m_ptr) T(machine, this->m_addr, std::forward<Args>(args)...);
		} else if constexpr (is_guest_datatype<W, T>::value) {
			new (m_ptr) T(machine, std::forward<Args>(args)...);
			relocate_guest_object<W>(machine, *m_ptr, this->m_addr);
		} else {
			// Construct the object in place (as if trivially constructible)
			new (m_ptr) T{std::forward<Args>(args)...};
		}
	}

	~ScopedArenaObject() {
		this->free_standard_types();
		m_machine->arena().free(this->m_addr);
	}

	T& operator*() { return *m_ptr; }
	T* operator->() { return m_ptr; }

	gaddr_t address() const { return m_addr; }

	ScopedArenaObject& operator=(const ScopedArenaObject&) = delete;

	ScopedArenaObject& operator=(const T& other) {
		// It's not possible for m_addr to be 0 here, as it would have thrown in the constructor
		this->free_standard_types();
		this->allocate_if_null();
		this->m_ptr = m_machine->memory.template memarray<T>(this->m_addr, 1);
		// A shallow copy, which transfers ownership of the guest allocations
		new (m_ptr) T(other);
		relocate_guest_object<W>(*m_machine, *m_ptr, this->m_addr);
		return *this;
	}

	// Assign any alternative value to a variant-like container, ie. one that
	// selects what to store from the type of the value (see GuestStdVariant)
	template <typename Arg, typename Tp = T>
	std::enable_if_t<detail::has_guest_set<W, Tp, Arg>::value
		&& !std::is_same_v<std::decay_t<Arg>, Tp>, ScopedArenaObject&>
	operator=(const Arg& other) {
		this->allocate_if_null();
		this->m_ptr->set(*m_machine, this->m_addr, other);
		return *this;
	}

	// Assign to a string container, C++ or Rust
	template <typename Tp = T>
	std::enable_if_t<detail::has_guest_set_string<W, Tp>::value, ScopedArenaObject&>
	operator=(std::string_view other) {
		this->allocate_if_null();
		this->m_ptr->set_string(*m_machine, this->m_addr, other.data(), other.size());
		return *this;
	}

	// Assign to a vector container, C++ or Rust
	template <typename U, typename Tp = T>
	std::enable_if_t<detail::has_guest_vector_assign<W, Tp, U>::value, ScopedArenaObject&>
	operator=(const std::vector<U>& other) {
		this->allocate_if_null();
		this->m_ptr->assign(*m_machine, other);
		return *this;
	}

	// Assign to a map container
	template <typename HK, typename HV, typename Tp = T>
	std::enable_if_t<detail::has_guest_map_assign<W, Tp, std::unordered_map<HK, HV>>::value, ScopedArenaObject&>
	operator=(const std::unordered_map<HK, HV>& other) {
		this->allocate_if_null();
		this->m_ptr->assign(*m_machine, this->m_addr, other);
		return *this;
	}

	ScopedArenaObject& operator=(ScopedArenaObject&& other) {
		this->free_standard_types();
		this->m_machine = other.m_machine;
		this->m_addr = other.m_addr;
		this->m_ptr = other.m_ptr;
		other.m_addr = 0;
		other.m_ptr = nullptr;
		return *this;
	}

private:
	void allocate_if_null() {
		if (this->m_addr == 0) {
			this->m_addr = m_machine->arena().malloc(sizeof(T));
			if (this->m_addr == 0) {
				throw std::bad_alloc();
			}
			this->m_ptr = m_machine->memory.template memarray<T>(this->m_addr, 1);
		}
	}
	void free_standard_types() {
		if constexpr (is_guest_datatype<W, T>::value) {
			if (this->m_ptr) {
				this->m_ptr->free(*this->m_machine);
			}
		}
	}

	T*      m_ptr  = nullptr;
	gaddr_t m_addr = 0;
	machine_t* m_machine;
};

template <int W, typename T>
struct is_scoped_guest_object : std::false_type {};

template <int W, typename T>
struct is_scoped_guest_object<W, ScopedArenaObject<W, T>> : std::true_type {};

/// @brief The type that a ScopedArenaObject holds, or the type itself
template <int W, typename T>
struct scoped_guest_object { using type = T; };

template <int W, typename T>
struct scoped_guest_object<W, ScopedArenaObject<W, T>> { using type = T; };

template <int W, typename T>
using scoped_guest_object_t = typename scoped_guest_object<W, T>::type;

} // riscv
