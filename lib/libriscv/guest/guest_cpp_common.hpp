#pragma once
#include "guest_common.hpp"
#include <string>

// Forward declarations and the type traits of the guest-side C++ standard
// containers. They are gathered here because the containers refer to each
// other: a variant can hold a map, and a map can hold a variant, so neither
// header can be the one that declares both.

namespace riscv {

template <int W> struct GuestStdString;
template <int W, typename T> struct GuestStdVector;
template <int W, typename K> struct GuestStdHash;
/// @brief See GuestStdUnorderedMap. CacheHashCode must match the guest,
/// which stores the hash code in every node only for slow hash functions. It
/// defaults to what std::hash<K> would give, so a map that uses a custom hash
/// function has to say so: GuestStdUnorderedMap<W, K, V, false>.
template <int W, typename K, typename V,
	bool CacheHashCode = GuestStdHash<W, K>::cache_hash_code>
struct GuestStdUnorderedMap;
template <int W, typename... Types> struct GuestStdVariant;

template <int W, typename T>
struct is_guest_stdstring : std::false_type {};

template <int W>
struct is_guest_stdstring<W, GuestStdString<W>> : std::true_type {};

template <int W, typename T>
struct is_guest_stdvector : std::false_type {};

template <int W, typename T>
struct is_guest_stdvector<W, GuestStdVector<W, T>> : std::true_type {};

template <int W, typename T>
struct is_guest_stdunordered_map : std::false_type {};

template <int W, typename K, typename V, bool CacheHashCode>
struct is_guest_stdunordered_map<W, GuestStdUnorderedMap<W, K, V, CacheHashCode>> : std::true_type {};

template <int W, typename T>
struct is_guest_stdvariant : std::false_type {};

template <int W, typename... Types>
struct is_guest_stdvariant<W, GuestStdVariant<W, Types...>> : std::true_type {};

// Register the containers with the generic machinery in guest_common.hpp

template <int W>
struct is_guest_datatype<W, GuestStdString<W>> : std::true_type {};
template <int W, typename T>
struct is_guest_datatype<W, GuestStdVector<W, T>> : std::true_type {};
template <int W, typename K, typename V, bool CacheHashCode>
struct is_guest_datatype<W, GuestStdUnorderedMap<W, K, V, CacheHashCode>> : std::true_type {};
template <int W, typename... Types>
struct is_guest_datatype<W, GuestStdVariant<W, Types...>> : std::true_type {};

/// @brief A std::string points at its own inline buffer when it is short
/// (the small-string optimization), and a map points at its before_begin
/// member from one of its buckets.
template <int W>
struct is_self_referencing_guest_object<W, GuestStdString<W>> : std::true_type {};
template <int W, typename K, typename V, bool CacheHashCode>
struct is_self_referencing_guest_object<W, GuestStdUnorderedMap<W, K, V, CacheHashCode>> : std::true_type {};
/// @brief A variant is self-referencing when any of its alternatives are.
template <int W, typename... Types>
struct is_self_referencing_guest_object<W, GuestStdVariant<W, Types...>>
	: std::bool_constant<(is_self_referencing_guest_object<W, Types>::value || ...)> {};

/// @brief The string, the map and the variant are all constructed as
/// T(machine, self, ...), as they need their own address in guest memory.
/// The vector does not: its elements live in a separate allocation.
template <int W>
struct guest_object_needs_self_address<W, GuestStdString<W>> : std::true_type {};
template <int W, typename K, typename V, bool CacheHashCode>
struct guest_object_needs_self_address<W, GuestStdUnorderedMap<W, K, V, CacheHashCode>> : std::true_type {};
template <int W, typename... Types>
struct guest_object_needs_self_address<W, GuestStdVariant<W, Types...>> : std::true_type {};

// The language-neutral categories, used to pick eg. the string alternative
// of a variant when a host string is assigned to it.

template <int W>
struct is_guest_string<W, GuestStdString<W>> : std::true_type {};
template <int W, typename T>
struct is_guest_vector<W, GuestStdVector<W, T>> : std::true_type {};
template <int W, typename K, typename V, bool CacheHashCode>
struct is_guest_map<W, GuestStdUnorderedMap<W, K, V, CacheHashCode>> : std::true_type {};

/// @brief A guest std::string is copied to the host as a std::string
template <int W>
struct guest_host_type<W, GuestStdString<W>> { using type = std::string; };

} // riscv
