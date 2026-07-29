#pragma once
#include "../native_heap.hpp" // arena()
#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

// The machinery that every guest container is built on, regardless of which
// language runtime it belongs to. It knows nothing about the containers
// themselves: a container registers what it needs by specializing the traits
// below, and the dispatch functions pick the right operation from there.
//
// Adding a container to this framework means, at most:
//   1. is_guest_datatype        - it owns guest memory, free() releases it
//   2. is_self_referencing_guest_object - it points back into itself, move()
//   3. guest_object_needs_self_address  - its constructors take (machine, self, ...)
// Everything else - vectors of it, variants of it, map keys and values of it,
// ScopedArenaObject of it - then works without any further changes.

namespace riscv {

// View a guest memory location as a reference to a C++ object
template <int W, typename T>
struct GuestRef {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	const gaddr_t ptr;

	constexpr GuestRef() noexcept : ptr(0) {}
	GuestRef(gaddr_t ptr) : ptr(ptr) {}

	/// @brief Get the address of the reference.
	/// @return The address of the reference.
	gaddr_t address() const noexcept { return ptr; }

	/// @brief Check if the reference is valid.
	/// @return True if the reference is not null.
	operator bool() const noexcept { return ptr == 0; }

	const T& get(machine_t& machine) const noexcept {
		// This function cannot silently fail, as it will
		// throw an exception if the location is invalid or misaligned.
		return *machine.memory.template memarray<T>(this->ptr, 1);
	}
	T& get(machine_t& machine) noexcept {
		// This function cannot silently fail, as it will
		// throw an exception if the location is invalid or misaligned.
		return *machine.memory.template memarray<T>(this->ptr, 1);
	}
};

/// @brief True for a guest container that owns guest memory, and which must
/// be released with free(machine) instead of a destructor. Specialize this
/// for every new container that allocates from the guest arena.
template <int W, typename T>
struct is_guest_datatype : std::false_type {};

/// @brief Backwards-compatible name from when only the C++ standard
/// containers were supported.
template <int W, typename T>
using is_guest_stdtype = is_guest_datatype<W, T>;

/// @brief True when an object contains pointers back into itself, and it
/// must be told when it is moved to another address in guest memory.
/// Such a container implements either move(addr) or move(machine, addr).
template <int W, typename T>
struct is_self_referencing_guest_object : std::false_type {};

/// @brief True when the constructors of a container take the guest address
/// of the container itself, directly after the machine: T(machine, self, ...).
/// The containers that point back into themselves need it in order to be
/// usable by the guest, and so do the ones that place their contents inline.
template <int W, typename T>
struct guest_object_needs_self_address : std::false_type {};

/// @brief True for a guest container that holds a run of characters, eg. a
/// C++ std::string or a Rust String. Used to select the string alternative
/// of a variant, and the like.
template <int W, typename T>
struct is_guest_string : std::false_type {};

/// @brief True for a guest container that holds a contiguous array
template <int W, typename T>
struct is_guest_vector : std::false_type {};

/// @brief True for a guest container that maps keys to values
template <int W, typename T>
struct is_guest_map : std::false_type {};

/// @brief The host type that a guest value is copied to, eg. a std::string
/// for the guest string types. Plain values are copied as they are.
template <int W, typename T>
struct guest_host_type { using type = T; };

template <int W, typename T>
using guest_host_type_t = typename guest_host_type<W, T>::type;

namespace detail {
	/// @brief True when object.move(machine, addr) is valid
	template <int W, typename T, typename = void>
	struct has_relocate_with_machine : std::false_type {};
	template <int W, typename T>
	struct has_relocate_with_machine<W, T, std::void_t<decltype(
		std::declval<T&>().move(std::declval<Machine<W>&>(), std::declval<address_type<W>>()))>>
		: std::true_type {};

	/// @brief True when object.move(addr) is valid
	template <int W, typename T, typename = void>
	struct has_relocate : std::false_type {};
	template <int W, typename T>
	struct has_relocate<W, T, std::void_t<decltype(
		std::declval<T&>().move(std::declval<address_type<W>>()))>>
		: std::true_type {};

	/// @brief True when object.to_string(machine) is valid
	template <int W, typename T, typename = void>
	struct has_to_string : std::false_type {};
	template <int W, typename T>
	struct has_to_string<W, T, std::void_t<decltype(
		std::declval<const T&>().to_string(std::declval<const Machine<W>&>()))>>
		: std::true_type {};
} // detail

/// @brief True when a guest value can be copied to the host on its own: it
/// is either a plain value, or a container that knows how to become one.
/// The nested containers that are not, must be read element by element.
template <int W, typename T>
struct is_host_convertible_guest_object : std::bool_constant<
	!is_guest_datatype<W, T>::value || detail::has_to_string<W, T>::value> {};

/// @brief Copy a guest value to the host (see guest_host_type).
template <int W, typename T>
inline guest_host_type_t<W, T> guest_object_to_host(const Machine<W>& machine, const T& value)
{
	if constexpr (detail::has_to_string<W, T>::value)
		return value.to_string(machine);
	else {
		(void)machine;
		return value;
	}
}

/// @brief Destroy an object in guest memory, freeing any guest allocations
/// that it owns. Trivial types are simply destructed.
template <int W, typename T>
inline void free_guest_object(Machine<W>& machine, T& object)
{
	if constexpr (is_guest_datatype<W, T>::value)
		object.free(machine);
	else
		object.~T();
}

/// @brief Tell an object that it now lives at the given guest address,
/// which the objects that point back into themselves need to know.
template <int W, typename T>
inline void relocate_guest_object(Machine<W>& machine, T& object, address_type<W> addr)
{
	if constexpr (is_self_referencing_guest_object<W, T>::value) {
		// A container that needs the machine in order to reach its own guest
		// memory takes it, and the rest only need to know the new address.
		if constexpr (detail::has_relocate_with_machine<W, T>::value)
			object.move(machine, addr);
		else {
			static_assert(detail::has_relocate<W, T>::value,
				"A self-referencing guest object must implement move(addr) or move(machine, addr)");
			(void)machine;
			object.move(addr);
		}
	} else {
		(void)machine; (void)object; (void)addr;
	}
}

/// @brief Construct an object of type T at the guest address addr, from a
/// host-side value. A value of the same type is shallow-copied, which
/// transfers the ownership of its guest allocations.
template <int W, typename T, typename Arg>
inline void construct_guest_object(Machine<W>& machine, address_type<W> addr, T& dst, const Arg& src)
{
	if constexpr (std::is_same_v<T, std::decay_t<Arg>>) {
		// Shallow copy, which transfers ownership of guest allocations
		new (&dst) T(src);
		relocate_guest_object<W>(machine, dst, addr);
	} else if constexpr (is_guest_datatype<W, T>::value) {
		if constexpr (guest_object_needs_self_address<W, T>::value)
			new (&dst) T(machine, addr, src);
		else
			new (&dst) T(machine, src);
	} else {
		(void)machine; (void)addr;
		new (&dst) T(static_cast<T>(src));
	}
}

} // riscv
