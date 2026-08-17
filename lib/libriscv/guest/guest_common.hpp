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

/// @brief The alignment that the *guest* gives a type, which is not always the
/// one the host gives it: a RISC-V guest aligns every scalar to its own size,
/// while a 32-bit x86 host aligns uint64_t and double to 4 bytes. A mirror that
/// asked the host would then put a 64-bit member at the wrong offset, and be
/// wrong about its own size, so the mirrors ask here instead - and declare
/// themselves alignas() the answer, so that alignof() agrees with the guest for
/// every mirror built out of other mirrors.
template <typename T, typename = void>
struct guest_alignof {
	static constexpr std::size_t value = alignof(T);
};
template <typename T>
struct guest_alignof<T, std::enable_if_t<
	(std::is_arithmetic_v<T> || std::is_enum_v<T>) && sizeof(T) <= 8>>
{
	static constexpr std::size_t value = sizeof(T);
};

template <typename T>
inline constexpr std::size_t guest_alignof_v = guest_alignof<T>::value;

/// @brief The alignment of a guest machine word: 4 on a 32-bit guest and 8 on
/// a 64-bit one, whatever the host makes of address_type<W>.
template <int W>
inline constexpr std::size_t guest_word_align = guest_alignof_v<address_type<W>>;

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

	/// @brief Picks which alternative of a sum type a host value belongs to.
	/// Shared by GuestStdVariant and GuestRustEnum, which select the same way
	/// and only differ in where they keep the tag.
	template <int W, typename... Types>
	struct guest_alternatives
	{
		static constexpr std::size_t count = sizeof...(Types);

		/// @brief The index of the alternative T, or count when not found.
		template <typename T>
		static constexpr std::size_t index_of() noexcept
		{
			constexpr bool matches[count] { std::is_same_v<T, Types>... };
			std::size_t result = count;
			for (std::size_t i = count; i > 0; i--)
				if (matches[i - 1]) result = i - 1;
			return result;
		}

		/// @brief The alternative that a host value should be converted to, or
		/// count when there is no match, or more than one. Mirrors the converting
		/// constructor of std::variant, except that the container categories
		/// (string, vector, map) take precedence over plain conversions, so that
		/// eg. a host std::string picks the one string alternative rather than
		/// matching every alternative it converts to.
		template <typename Arg>
		static constexpr std::size_t index_for_arg() noexcept
		{
			using A = std::decay_t<Arg>;
			constexpr bool is_string_like = is_stdstring<A>::value
				|| is_string<A>::value || std::is_same_v<A, std::string_view>;

			if constexpr (index_of<A>() < count) {
				// An exact match always wins
				return index_of<A>();
			} else if constexpr (is_string_like) {
				// The only string alternative, if there is exactly one
				constexpr bool matches[count] { is_guest_string<W, Types>::value... };
				return only_match(matches);
			} else if constexpr (is_stdvector<A>::value) {
				// The only vector alternative, if there is exactly one
				constexpr bool matches[count] { is_guest_vector<W, Types>::value... };
				return only_match(matches);
			} else if constexpr (is_host_map<A>::value) {
				// The only map alternative, if there is exactly one
				constexpr bool matches[count] { is_guest_map<W, Types>::value... };
				return only_match(matches);
			} else {
				// The only alternative that the value converts to
				constexpr bool matches[count] { is_alternative_convertible<A, Types>::value... };
				return only_match(matches);
			}
		}

	private:
		template <typename U>
		struct one_element_array { U value[1]; };

		/// @brief True when the declaration "U x[] = { a }" is valid, which is how
		/// std::variant builds its overload set. It rejects narrowing.
		template <typename A, typename U, typename = void>
		struct is_array_initializable : std::false_type {};
		template <typename A, typename U>
		struct is_array_initializable<A, U,
			std::void_t<decltype(one_element_array<U>{{std::declval<const A&>()}})>>
			: std::true_type {};

		/// @brief True when a host value can be converted to the alternative U.
		/// The array form alone would also accept aggregate initialization from a
		/// single value, eg. a glm::vec3 from a float, which std::variant rejects.
		template <typename A, typename U>
		struct is_alternative_convertible : std::bool_constant<
			is_array_initializable<A, U>::value && std::is_convertible_v<const A&, U>> {};

		/// @brief True for a host container with keys and mapped values,
		/// eg. a std::unordered_map or a std::map.
		template <typename A, typename = void>
		struct is_host_map : std::false_type {};
		template <typename A>
		struct is_host_map<A, std::void_t<typename A::key_type, typename A::mapped_type>>
			: std::true_type {};

		/// @brief The index of the only alternative that matches, or count
		/// when there is no match, or more than one.
		static constexpr std::size_t only_match(const bool (&matches)[count]) noexcept
		{
			std::size_t result = count;
			std::size_t found = 0;
			for (std::size_t i = count; i > 0; i--) {
				if (matches[i - 1]) { result = i - 1; found += 1; }
			}
			return found == 1 ? result : count;
		}
	};
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
