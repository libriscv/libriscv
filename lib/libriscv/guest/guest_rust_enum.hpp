#pragma once
#include "guest_arena_object.hpp"
#include "guest_rust_string.hpp"
#include <cstddef>
#include <tuple>
#include <variant>

// View into a Rust enum with fields, declared #[repr(C, uN)] by the guest.
//
// The Rust answer to std::variant. Unlike the C++ one the layout is not
// discovered but *requested*: repr(C, uN) is specified as a tag followed by the
// union of the variants. A plain #[repr(Rust)] enum is not mirrorable at all -
// the compiler reorders the fields and hides the tag in a niche of one of them.
// It does: { i64, String } comes out three words long, with the tag stuffed
// into the String's capacity field.
//
//   #[repr(C, u64)]                  GuestRustEnum<W, uint64_t,
//   pub enum Value {                     std::monostate,   // Nil
//       Nil,                             int64_t,          // Int(i64)
//       Int(i64),                        double,           // Float(f64)
//       Float(f64),                      GuestRustString<W>>
//       Str(String),
//   }
//
// The tag comes *first*, unlike a C++ std::variant, and discriminants are the
// declaration order 0..n - an enum with explicit discriminants is not mirrored.
// Unit variants (`Nil`) are spelled std::monostate.
//
// WHY uN SHOULD BE AS WIDE AS THE PAYLOAD ALIGNMENT
//
// RFC 2195 specifies repr(C, uN) as a union of repr(C) structs that each begin
// with the tag, putting a bool payload at offset 1 behind a u8 tag. rustc
// instead emits a tag followed by one uniformly aligned payload, putting it at
// offset 8. The readings only differ when the tag is narrower than the payload
// alignment, so this mirror requires sizeof(Tag) >= alignof(payload) and the
// ambiguity cannot arise. For pointer-sized variants that means u64 on a 64-bit
// guest and u32 on a 32-bit one, at no cost: the padding after a u8 tag would
// have been there anyway.

namespace riscv {

template <int W, typename Tag, typename... Types>
struct GuestRustEnum
{
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	using tag_type = Tag;
	/// @brief Names the enum itself, so a type deriving from it can be checked
	/// against it (see RISCV_REGISTER_GUEST_RUST_ENUM)
	using this_enum_type = GuestRustEnum<W, Tag, Types...>;

	static constexpr std::size_t alternatives = sizeof...(Types);
	static_assert(alternatives > 0, "A guest Rust enum needs at least one variant");
	static_assert(std::is_unsigned_v<Tag> && std::is_integral_v<Tag>,
		"The tag of a #[repr(C, uN)] enum is an unsigned integer");

	template <std::size_t I>
	using alternative_t = std::tuple_element_t<I, std::tuple<Types...>>;

	/// @brief The alignment of the widest variant, which the payload as a whole
	/// is aligned to
	static constexpr std::size_t payload_align = std::max({guest_alignof_v<Types>...});
	/// @brief The size of the payload, rounded up to its alignment
	static constexpr std::size_t payload_size =
		((std::max({sizeof(Types)...}) + payload_align - 1) / payload_align) * payload_align;

	static_assert(sizeof(Tag) >= payload_align,
		"The tag of a guest Rust enum must be at least as wide as the alignment "
		"of its widest variant, so that the payload offset is unambiguous. Declare "
		"the guest enum as #[repr(C, u64)] (or u32/u16 to match) instead of u8.");

	/// @brief No Rust container points back into itself, which is why nothing
	/// here takes a self address.
	static_assert(!(guest_object_needs_self_address<W, Types>::value || ...),
		"A guest Rust enum variant must not need its own guest address. The C++ "
		"containers (GuestStdString, GuestStdUnorderedMap) do, and cannot be used "
		"as a variant of a Rust enum.");

	alignas(guest_alignof_v<Tag>) Tag tag;        // the discriminant
	alignas(payload_align) uint8_t payload[payload_size];

	/// @brief An enum holding a default-constructed first variant. It allocates
	/// nothing, which is only correct when that variant is a unit
	/// (std::monostate) or an empty container.
	constexpr GuestRustEnum() noexcept
		: tag(0), payload{} {}

	/// @brief Create an enum holding the variant that matches a host value
	template <typename Arg>
	GuestRustEnum(machine_t& machine, const Arg& value)
		: tag(0), payload{}
	{
		this->set(machine, value);
	}
	/// @brief Overload ignoring the enum's own guest address, which it does not need
	template <typename Arg>
	GuestRustEnum(machine_t& machine, gaddr_t /*self*/, const Arg& value)
		: GuestRustEnum(machine, value) {}

	// Copying is shallow, like the other guest containers: the copy shares the
	// allocations of the active variant, and only one of them may free them.
	GuestRustEnum(const GuestRustEnum& other) = default;
	GuestRustEnum& operator=(const GuestRustEnum& other) = default;

	/// @brief The discriminant, ie. the index of the active variant.
	std::size_t index() const noexcept { return std::size_t(this->tag); }

	/// @brief True when the discriminant names a variant that exists. A Rust
	/// enum always holds one of its variants, so this is false only for memory
	/// the guest never initialized, or that it is lying about.
	bool valid() const noexcept { return std::size_t(this->tag) < alternatives; }

	/// @brief Which variant a host value belongs to (guest_common.hpp)
	using selector = detail::guest_alternatives<W, Types...>;

	/// @brief The index of the variant T, or alternatives when not found.
	template <typename T>
	static constexpr std::size_t index_of() noexcept {
		return selector::template index_of<T>();
	}

	template <typename T>
	bool holds_alternative() const noexcept {
		static_assert(index_of<T>() < alternatives, "GuestRustEnum: Not a variant");
		return this->tag == Tag(index_of<T>());
	}

	/// @brief Access the variant T, or throw when it is not the active one.
	/// @return A reference into guest memory.
	template <typename T>
	T& get() {
		if (!this->template holds_alternative<T>())
			throw std::runtime_error("Guest Rust enum does not hold the given variant");
		return this->template alternative_ref<T>();
	}
	template <typename T>
	const T& get() const {
		if (!this->template holds_alternative<T>())
			throw std::runtime_error("Guest Rust enum does not hold the given variant");
		return this->template alternative_ref<T>();
	}

	/// @brief Access the variant T, or null when it is not the active one.
	template <typename T>
	T* get_if() noexcept {
		if (!this->template holds_alternative<T>())
			return nullptr;
		return &this->template alternative_ref<T>();
	}
	template <typename T>
	const T* get_if() const noexcept {
		if (!this->template holds_alternative<T>())
			return nullptr;
		return &this->template alternative_ref<T>();
	}

	/// @brief Access the variant at index I, or throw when not active.
	template <std::size_t I>
	alternative_t<I>& get_index() {
		return this->template get<alternative_t<I>>();
	}
	template <std::size_t I>
	const alternative_t<I>& get_index() const {
		return this->template get<alternative_t<I>>();
	}

	/// @brief Replace the value with a newly constructed variant T.
	/// @return A reference to the new variant, in guest memory.
	template <typename T, typename... Args>
	T& emplace(machine_t& machine, Args&&... args)
	{
		constexpr std::size_t I = index_of<T>();
		static_assert(I < alternatives, "GuestRustEnum: Not a variant");
		// free() rather than destroy_active(), so a construction that throws
		// leaves a valid enum behind instead of a freed payload with the old
		// tag still pointing at it
		this->free(machine);

		T& dst = this->template alternative_ref<T>();
		if constexpr (is_guest_datatype<W, T>::value)
			new (&dst) T(machine, std::forward<Args>(args)...);
		else
			new (&dst) T(std::forward<Args>(args)...);
		this->tag = Tag(I);
		return dst;
	}

	/// @brief Replace the value with the variant matching the given host value,
	/// chosen the way std::variant's converting constructor does: an exact
	/// match when there is one, otherwise the single variant it converts to.
	template <typename Arg>
	auto& set(machine_t& machine, const Arg& value)
	{
		constexpr std::size_t I = selector::template index_for_arg<Arg>();
		static_assert(I < alternatives,
			"GuestRustEnum: No unique variant matches the given value. "
			"Use emplace<T>() to select the variant explicitly.");
		using T = alternative_t<I>;
		this->free(machine);

		T& dst = this->template alternative_ref<T>();
		// No variant needs its own address (see the assert above), so the self
		// argument here is never used
		construct_guest_object<W, T>(machine, gaddr_t(0), dst, value);
		this->tag = Tag(I);
		return dst;
	}
	/// @brief Overload ignoring the enum's own guest address, so that it can be
	/// used wherever a C++ variant can.
	template <typename Arg>
	auto& set(machine_t& machine, gaddr_t /*self*/, const Arg& value) {
		return this->set(machine, value);
	}

	/// @brief Replace the value with the active alternative of a host
	/// std::variant, converting one alternative at a time.
	template <typename... HostTypes>
	void set(machine_t& machine, const std::variant<HostTypes...>& value)
	{
		std::visit([&] (const auto& alt) {
			this->set(machine, alt);
		}, value);
	}

	/// @brief Invoke the callback with a reference to the active variant.
	/// @return True when it was invoked, ie. when the discriminant is in range.
	template <typename F>
	bool visit(F&& callback) {
		return this->visit_impl(std::index_sequence_for<Types...>{}, callback);
	}
	template <typename F>
	bool visit(F&& callback) const {
		return this->visit_impl(std::index_sequence_for<Types...>{}, callback);
	}

	using host_variant_t = std::variant<guest_host_type_t<W, Types>...>;

	/// @brief Copy the active variant to the host. Nested guest containers
	/// are not supported here, use visit() for those.
	host_variant_t to_variant(const machine_t& machine) const
	{
		static_assert((is_host_convertible_guest_object<W, Types>::value && ...),
			"Guest Rust enum: Nested guest containers must be read using visit()");
		host_variant_t result;
		const bool ok = this->visit([&] (const auto& alt) {
			result = guest_object_to_host<W>(machine, alt);
		});
		if (!ok)
			throw std::runtime_error("Guest Rust enum has an out-of-range discriminant");
		return result;
	}

	/// @brief Drop the active variant and everything it owns, recursively.
	///
	/// A Rust enum has no valueless state, so what is left behind is the first
	/// variant in its default state - which allocates nothing, and which the
	/// guest can safely drop. Freeing twice is a no-op, not a double free.
	void free(machine_t& machine)
	{
		using First = alternative_t<0>;
		static_assert(std::is_default_constructible_v<First>,
			"The first variant of a guest Rust enum must be default constructible, "
			"as it is what free() leaves behind. Declare a unit variant first "
			"(std::monostate here, `Nil` in the guest).");
		this->destroy_active(machine);
		new (&this->template alternative_ref<First>()) First();
		this->tag = 0;
	}

private:
	template <typename T>
	T& alternative_ref() noexcept {
		return *reinterpret_cast<T*>(&this->payload[0]);
	}
	template <typename T>
	const T& alternative_ref() const noexcept {
		return *reinterpret_cast<const T*>(&this->payload[0]);
	}

	/// @brief Release the guest allocations of the active variant, leaving the
	/// tag pointing at storage that is no longer valid: every caller assigns a
	/// new variant right afterwards.
	void destroy_active(machine_t& machine) {
		this->visit([&] (auto& alt) {
			free_guest_object<W>(machine, alt);
		});
	}

	template <std::size_t... I, typename F>
	bool visit_impl(std::index_sequence<I...>, F& callback) {
		return (... || (this->tag == Tag(I)
			? (callback(this->template alternative_ref<alternative_t<I>>()), true) : false));
	}
	template <std::size_t... I, typename F>
	bool visit_impl(std::index_sequence<I...>, F& callback) const {
		return (... || (this->tag == Tag(I)
			? (callback(this->template alternative_ref<alternative_t<I>>()), true) : false));
	}
};

// The layout of a #[repr(C, uN)] enum: the tag first, then the payload at the
// first offset that satisfies the alignment of the widest variant.
template <int W>
struct GuestRustEnumLayout {
	/// @brief The tag a pointer-sized payload wants: u64 on a 64-bit guest, u32
	/// on a 32-bit one. Wider only adds tail padding; narrower is rejected by
	/// the static_assert above.
	using tag_t = riscv::address_type<W>;
	using string_type = GuestRustString<W>;

	using nil_int_t = GuestRustEnum<W, tag_t, std::monostate, tag_t>;
	using nil_str_t = GuestRustEnum<W, tag_t, std::monostate, string_type>;
	using narrow_t  = GuestRustEnum<W, uint16_t, std::monostate, uint16_t>;
	/// @brief An f64 is 8-aligned on every guest, so this one needs a u64 tag
	/// even on a 32-bit guest
	using nil_f64_t = GuestRustEnum<W, uint64_t, std::monostate, double, string_type>;

	static_assert(offsetof(nil_int_t, payload) == sizeof(tag_t), "The payload follows the tag");
	static_assert(sizeof(nil_int_t) == 2 * sizeof(tag_t), "enum { Nil, Int(usize) }");
	static_assert(alignof(nil_int_t) == sizeof(tag_t), "Aligned like its payload");
	static_assert(sizeof(nil_str_t) == sizeof(tag_t) + sizeof(string_type),
		"enum { Nil, Str(String) } is the tag plus the widest variant");
	// A narrow tag is wide enough for a payload of the same alignment
	static_assert(offsetof(narrow_t, payload) == 2, "A u16 tag with u16 variants");
	static_assert(sizeof(narrow_t) == 4, "enum { Nil, Small(u16) }");
	// The payload is padded up to its own alignment, which makes the enum the
	// same size whether or not the f64 variant is the widest one
	static_assert(offsetof(nil_f64_t, payload) == 8, "A u64 tag ahead of an f64 payload");
	static_assert(sizeof(nil_f64_t) == 8 + ((sizeof(string_type) + 7) / 8) * 8,
		"enum { Nil, Float(f64), Str(String) }");
	static_assert(std::is_standard_layout_v<nil_int_t>, "Standard layout");
};
template struct GuestRustEnumLayout<4>;
template struct GuestRustEnumLayout<8>;

/// @brief A guest Rust enum that lives in the arena, and which frees itself
/// (and its active variant) at the end of the scope.
template <int W, typename Tag, typename... Types>
using ScopedGuestRustEnum = ScopedArenaObject<W, GuestRustEnum<W, Tag, Types...>>;

} // riscv

/// @brief Register a value type that derives from GuestRustEnum.
///
/// A tree type has to name itself - a group variant is a Box<Attributes>, and
/// Attributes is a Vec of entries holding the value type - and only a class can
/// be forward declared, not an alias. So the value type is a struct deriving
/// from the enum, and this line tells guest_common.hpp that it owns guest
/// memory just like the enum it derives from:
///
///     struct GValue;
///     using GAttrs = riscv::GuestRustAttributes<RISCV64, GValue>;
///     struct GValue : riscv::GuestRustEnum<RISCV64, uint64_t,
///         std::monostate, int64_t, riscv::GuestRustString<RISCV64>,
///         riscv::GuestRustBox<RISCV64, GAttrs>>
///     {
///         using GuestRustEnum::GuestRustEnum;
///     };
///     RISCV_REGISTER_GUEST_RUST_ENUM(RISCV64, GValue);
///
/// Use it at namespace scope, outside of namespace riscv.
#define RISCV_REGISTER_GUEST_RUST_ENUM(WIDTH, TYPE)                        \
	namespace riscv {                                                      \
		template <> struct is_guest_datatype<WIDTH, TYPE>                  \
			: std::true_type {};                                           \
		template <> struct is_guest_rustenum<WIDTH, TYPE>                  \
			: std::true_type {};                                           \
	} /* riscv */                                                          \
	static_assert(sizeof(TYPE) == sizeof(TYPE::this_enum_type),            \
		#TYPE " must add no members to the guest Rust enum it derives from")
