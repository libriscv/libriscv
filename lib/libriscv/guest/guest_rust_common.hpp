#pragma once
#include "guest_common.hpp"
#include <string>

// Forward declarations and type traits of the guest-side Rust containers.
//
// Rust does not stabilize the memory layout of its own collections: the
// compiler is free to order the fields of a #[repr(Rust)] type as it likes,
// and it does reorder these. What is written below is what the compiler
// actually emits, verified by transmuting the collections to [usize; N] in
// a riscv64gc-unknown-linux-gnu program and running it (rustc 1.93):
//
//   Vec<T>  = { cap: usize, ptr: *mut T, len: usize }
//   String  = Vec<u8> of UTF-8 bytes, ie. the same three words
//   &[T]    = { ptr: *const T, len: usize }
//   &str    = &[u8] of UTF-8 bytes, ie. the same two words
//
// NOTE: The capacity comes *first* in a Vec, ahead of the pointer, even
// though RawVecInner declares the pointer first. Re-run the check above
// when a new Rust release starts producing something else - a slice is the
// only one of the four whose order the language guarantees.
//
// Unlike the C++ containers there is no small-string optimization and no
// terminating zero, and none of them point back into themselves. That makes
// them cheaper to move around, but it also means that the *bytes* of a Rust
// string are the whole string: a length of zero is an empty string even when
// the pointer is not null (see GuestRustString::DANGLING).

namespace riscv {

template <int W> struct GuestRustString;
template <int W> struct GuestRustStr;
template <int W, typename T> struct GuestRustVec;
template <int W, typename T> struct GuestRustSlice;
template <int W, typename T> struct GuestRustBox;
template <int W, typename T> struct GuestRustBoxedSlice;
template <int W> struct GuestRustBoxedStr;
template <int W, typename Tag, typename... Types> struct GuestRustEnum;
template <int W, typename Value> struct GuestRustAttr;
template <int W, typename Value> struct GuestRustAttributes;

template <int W, typename T>
struct is_guest_ruststring : std::false_type {};

template <int W>
struct is_guest_ruststring<W, GuestRustString<W>> : std::true_type {};

template <int W, typename T>
struct is_guest_rustvec : std::false_type {};

template <int W, typename T>
struct is_guest_rustvec<W, GuestRustVec<W, T>> : std::true_type {};

template <int W, typename T>
struct is_guest_rustbox : std::false_type {};

template <int W, typename T>
struct is_guest_rustbox<W, GuestRustBox<W, T>> : std::true_type {};

template <int W, typename T>
struct is_guest_rustenum : std::false_type {};

template <int W, typename Tag, typename... Types>
struct is_guest_rustenum<W, GuestRustEnum<W, Tag, Types...>> : std::true_type {};

template <int W, typename T>
struct is_guest_rustattributes : std::false_type {};

template <int W, typename Value>
struct is_guest_rustattributes<W, GuestRustAttributes<W, Value>> : std::true_type {};

// Register the containers with the generic machinery in guest_common.hpp.
// None of them are self-referencing, and none of them need to know their own
// address in guest memory, which is why only the ownership trait is set. That
// is a property of the language, not of these mirrors: a Rust move is always a
// memcpy, so nothing may point back into itself.

template <int W>
struct is_guest_datatype<W, GuestRustString<W>> : std::true_type {};
template <int W, typename T>
struct is_guest_datatype<W, GuestRustVec<W, T>> : std::true_type {};
template <int W, typename T>
struct is_guest_datatype<W, GuestRustBox<W, T>> : std::true_type {};
template <int W, typename T>
struct is_guest_datatype<W, GuestRustBoxedSlice<W, T>> : std::true_type {};
template <int W>
struct is_guest_datatype<W, GuestRustBoxedStr<W>> : std::true_type {};
template <int W, typename Tag, typename... Types>
struct is_guest_datatype<W, GuestRustEnum<W, Tag, Types...>> : std::true_type {};
template <int W, typename Value>
struct is_guest_datatype<W, GuestRustAttr<W, Value>> : std::true_type {};
template <int W, typename Value>
struct is_guest_datatype<W, GuestRustAttributes<W, Value>> : std::true_type {};

template <int W>
struct is_guest_string<W, GuestRustString<W>> : std::true_type {};
template <int W>
struct is_guest_string<W, GuestRustBoxedStr<W>> : std::true_type {};
template <int W, typename T>
struct is_guest_vector<W, GuestRustVec<W, T>> : std::true_type {};
template <int W, typename T>
struct is_guest_vector<W, GuestRustBoxedSlice<W, T>> : std::true_type {};
template <int W, typename Value>
struct is_guest_map<W, GuestRustAttributes<W, Value>> : std::true_type {};

/// @brief A guest Rust String is copied to the host as a std::string
template <int W>
struct guest_host_type<W, GuestRustString<W>> { using type = std::string; };
/// @brief And so is a Box<str>
template <int W>
struct guest_host_type<W, GuestRustBoxedStr<W>> { using type = std::string; };

/// @brief The pointer that Rust uses for a collection that has not allocated
/// anything yet: a dangling but correctly aligned address. A null pointer is
/// undefined behavior, as the collections hold a NonNull<T>.
template <typename T>
struct GuestRustDangling {
	static constexpr std::size_t value = guest_alignof_v<T>;
};

/// @brief The smallest allocation that Rust's RawVec makes, in elements. It
/// avoids a run of tiny reallocations for the first few pushes.
template <typename T>
struct GuestRustMinCapacity {
	static constexpr std::size_t value =
		sizeof(T) == 1 ? 8 : (sizeof(T) <= 1024 ? 4 : 1);
};

} // riscv
