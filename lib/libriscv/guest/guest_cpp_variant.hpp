#pragma once
#include "guest_arena_object.hpp"
#include "guest_cpp_string.hpp"
#include <algorithm>
#include <tuple>
#include <variant>

namespace riscv {

// View into libstdc++ and LLVM libc++ std::variant<Types...> (same layout)
//
// A variant is a union of every alternative, followed by the index of the
// alternative that is currently active. An index of npos means that the
// variant holds nothing at all (std::variant_npos, "valueless by exception").
//
// NOTE: The alternatives that own guest memory need to know the address of
// the variant itself in guest memory, and it is passed to the operations
// that can allocate as the self argument. The storage of the alternative
// begins at the very start of the variant, so it is the same address.
template <int W, typename... Types>
struct GuestStdVariant
{
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	static constexpr std::size_t alternatives = sizeof...(Types);
	static_assert(alternatives > 0, "Guest std::variant needs at least one alternative");

	/// @brief The type that the guest stores the index in (see __select_index)
	using index_type = std::conditional_t<(alternatives <= 255), uint8_t, uint16_t>;
	/// @brief The index of a variant that holds nothing (std::variant_npos)
	static constexpr index_type npos = index_type(~index_type(0));

	template <std::size_t I>
	using alternative_t = std::tuple_element_t<I, std::tuple<Types...>>;

	static constexpr std::size_t union_align = std::max({alignof(Types)...});
	/// @brief The size of the union, rounded up to its alignment
	static constexpr std::size_t union_size =
		((std::max({sizeof(Types)...}) + union_align - 1) / union_align) * union_align;

	alignas(union_align) uint8_t storage[union_size]; // _Variadic_union _M_u
	index_type index_of_alternative;                  // __index_type _M_index

	/// @brief Create a variant that holds a zeroed first alternative, like a
	/// default-constructed std::variant. NOTE: When the first alternative
	/// owns guest memory, the variant is not usable by the guest until it
	/// has been given its own guest address using move().
	constexpr GuestStdVariant() noexcept
		: storage{}, index_of_alternative(0) {}

	/// @brief Create a variant at the guest address self, holding a
	/// value-initialized first alternative.
	GuestStdVariant(machine_t& machine, gaddr_t self)
		: storage{}, index_of_alternative(npos)
	{
		this->template emplace<alternative_t<0>>(machine, self);
	}

	/// @brief Create a variant at the guest address self, holding the
	/// alternative that matches the given host value.
	template <typename Arg>
	GuestStdVariant(machine_t& machine, gaddr_t self, const Arg& value)
		: storage{}, index_of_alternative(npos)
	{
		this->set(machine, self, value);
	}

	// Copying is intentionally shallow/fast, like the other guest containers.
	// A shallow copy shares the guest allocations with the original, and it
	// must be given its own address with move() before the guest can use it.
	GuestStdVariant(const GuestStdVariant& other) = default;
	GuestStdVariant& operator=(const GuestStdVariant& other) = default;

	/// @brief The index of the active alternative, or std::variant_npos.
	std::size_t index() const noexcept {
		return this->index_of_alternative == npos
			? std::size_t(-1) : std::size_t(this->index_of_alternative);
	}
	bool valueless_by_exception() const noexcept {
		return this->index_of_alternative == npos;
	}

	/// @brief The index of the alternative T, or alternatives when not found.
	template <typename T>
	static constexpr std::size_t index_of() noexcept {
		constexpr bool matches[alternatives] { std::is_same_v<T, Types>... };
		std::size_t result = alternatives;
		for (std::size_t i = alternatives; i > 0; i--)
			if (matches[i - 1]) result = i - 1;
		return result;
	}

	template <typename T>
	bool holds_alternative() const noexcept {
		static_assert(index_of<T>() < alternatives, "GuestStdVariant: Not an alternative");
		return this->index_of_alternative == index_type(index_of<T>());
	}

	/// @brief Access the alternative T, or throw when it is not active.
	/// @return A reference into guest memory.
	template <typename T>
	T& get() {
		if (!this->template holds_alternative<T>())
			throw std::runtime_error("Guest std::variant does not hold the given alternative");
		return this->template alternative_ref<T>();
	}
	template <typename T>
	const T& get() const {
		if (!this->template holds_alternative<T>())
			throw std::runtime_error("Guest std::variant does not hold the given alternative");
		return this->template alternative_ref<T>();
	}

	/// @brief Access the alternative T, or null when it is not active.
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

	/// @brief Access the alternative at index I, or throw when not active.
	template <std::size_t I>
	alternative_t<I>& get_index() {
		return this->template get<alternative_t<I>>();
	}
	template <std::size_t I>
	const alternative_t<I>& get_index() const {
		return this->template get<alternative_t<I>>();
	}

	/// @brief Replace the value with a newly constructed alternative T.
	/// @param self The address of this variant in guest memory.
	/// @return A reference to the new alternative, in guest memory.
	template <typename T, typename... Args>
	T& emplace(machine_t& machine, gaddr_t self, Args&&... args)
	{
		constexpr std::size_t I = index_of<T>();
		static_assert(I < alternatives, "GuestStdVariant: Not an alternative");
		this->free(machine);

		T& dst = this->template alternative_ref<T>();
		if constexpr (guest_object_needs_self_address<W, T>::value) {
			// These need to know their own address in guest memory
			new (&dst) T(machine, self, std::forward<Args>(args)...);
		} else if constexpr (is_guest_datatype<W, T>::value) {
			new (&dst) T(machine, std::forward<Args>(args)...);
		} else {
			new (&dst) T(std::forward<Args>(args)...);
		}
		this->index_of_alternative = index_type(I);
		return dst;
	}

	/// @brief Replace the value with the alternative that matches the given
	/// host value. The alternative is selected like the converting
	/// constructor of std::variant: an exact match when there is one, and
	/// otherwise the single alternative that the value converts to.
	/// @param self The address of this variant in guest memory.
	template <typename Arg>
	auto& set(machine_t& machine, gaddr_t self, const Arg& value)
	{
		constexpr std::size_t I = index_for_arg<Arg>();
		static_assert(I < alternatives,
			"GuestStdVariant: No unique alternative matches the given value. "
			"Use emplace<T>() to select the alternative explicitly.");
		using T = alternative_t<I>;
		this->free(machine);

		T& dst = this->template alternative_ref<T>();
		construct_guest_object<W, T>(machine, self, dst, value);
		this->index_of_alternative = index_type(I);
		return dst;
	}

	/// @brief Replace the value with the active alternative of a host
	/// std::variant, converting each alternative one by one.
	template <typename... HostTypes>
	void set(machine_t& machine, gaddr_t self, const std::variant<HostTypes...>& value)
	{
		if (value.valueless_by_exception()) {
			this->free(machine);
			return;
		}
		std::visit([&] (const auto& alt) {
			this->set(machine, self, alt);
		}, value);
	}

	/// @brief Invoke the callback with a reference to the active alternative.
	/// Does nothing when the variant is valueless.
	/// @return True when the callback was invoked.
	template <typename F>
	bool visit(F&& callback) {
		return this->visit_impl(std::index_sequence_for<Types...>{}, callback);
	}
	template <typename F>
	bool visit(F&& callback) const {
		return this->visit_impl(std::index_sequence_for<Types...>{}, callback);
	}

	using host_variant_t = std::variant<guest_host_type_t<W, Types>...>;

	/// @brief Copy the active alternative to the host. Nested guest
	/// containers are not supported here, use visit() for those.
	host_variant_t to_variant(const machine_t& machine) const
	{
		static_assert((is_host_convertible_guest_object<W, Types>::value && ...),
			"Guest std::variant: Nested guest containers must be read using visit()");
		host_variant_t result;
		const bool valid = this->visit([&] (const auto& alt) {
			result = guest_object_to_host<W>(machine, alt);
		});
		if (!valid)
			throw std::runtime_error("Guest std::variant is valueless");
		return result;
	}

	/// @brief Destroy the active alternative, leaving the variant valueless.
	void free(machine_t& machine)
	{
		this->visit([&] (auto& alt) {
			free_guest_object<W>(machine, alt);
		});
		this->index_of_alternative = npos;
	}

	/// @brief Tell the variant that it now lives at the guest address self.
	void move(machine_t& machine, gaddr_t self)
	{
		this->visit([&] (auto& alt) {
			relocate_guest_object<W>(machine, alt, self);
		});
	}

private:
	template <typename T>
	T& alternative_ref() noexcept {
		return *reinterpret_cast<T*>(&this->storage[0]);
	}
	template <typename T>
	const T& alternative_ref() const noexcept {
		return *reinterpret_cast<const T*>(&this->storage[0]);
	}

	template <std::size_t... I, typename F>
	bool visit_impl(std::index_sequence<I...>, F& callback) {
		return (... || (this->index_of_alternative == index_type(I)
			? (callback(this->template alternative_ref<alternative_t<I>>()), true) : false));
	}
	template <std::size_t... I, typename F>
	bool visit_impl(std::index_sequence<I...>, F& callback) const {
		return (... || (this->index_of_alternative == index_type(I)
			? (callback(this->template alternative_ref<alternative_t<I>>()), true) : false));
	}

	template <typename U>
	struct one_element_array { U value[1]; };

	/// @brief True when the array declaration "U x[] = { a }" is valid, which
	/// is how the converting constructor of std::variant builds the set of
	/// alternatives to choose between. It rejects narrowing conversions.
	template <typename A, typename U, typename = void>
	struct is_array_initializable : std::false_type {};
	template <typename A, typename U>
	struct is_array_initializable<A, U,
		std::void_t<decltype(one_element_array<U>{{std::declval<const A&>()}})>>
		: std::true_type {};

	/// @brief True when a host value can be converted to the alternative U.
	/// The array form alone would also accept the aggregate initialization of
	/// an alternative from a single value, eg. a glm::vec3 from a float, which
	/// the overload resolution of std::variant rejects.
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

	/// @brief The index of the only alternative that matches, or alternatives
	/// when there is no match, or more than one.
	static constexpr std::size_t only_match(const bool (&matches)[alternatives]) noexcept
	{
		std::size_t result = alternatives;
		std::size_t count = 0;
		for (std::size_t i = alternatives; i > 0; i--) {
			if (matches[i - 1]) { result = i - 1; count += 1; }
		}
		return count == 1 ? result : alternatives;
	}

	/// @brief The alternative that a host value should be converted to
	template <typename Arg>
	static constexpr std::size_t index_for_arg() noexcept
	{
		using A = std::decay_t<Arg>;
		constexpr bool is_string_like = is_stdstring<A>::value
			|| is_string<A>::value || std::is_same_v<A, std::string_view>;

		if constexpr (index_of<A>() < alternatives) {
			// An exact match always wins
			return index_of<A>();
		} else if constexpr (is_string_like) {
			// The only string alternative, if there is exactly one
			constexpr bool matches[alternatives] { is_guest_string<W, Types>::value... };
			return only_match(matches);
		} else if constexpr (is_stdvector<A>::value) {
			// The only vector alternative, if there is exactly one
			constexpr bool matches[alternatives] { is_guest_vector<W, Types>::value... };
			return only_match(matches);
		} else if constexpr (is_host_map<A>::value) {
			// The only map alternative, if there is exactly one
			constexpr bool matches[alternatives] { is_guest_map<W, Types>::value... };
			return only_match(matches);
		} else {
			// The only alternative that the value converts to
			constexpr bool matches[alternatives] { is_alternative_convertible<A, Types>::value... };
			return only_match(matches);
		}
	}
};

// Verify the variant layout against libstdc++ (verified with GCC 12 and 14)
template <int W>
struct GuestStdVariantLayout {
	template <typename... Types>
	using variant_type = GuestStdVariant<W, Types...>;
	using string_type = GuestStdString<W>;
	// A variant is the union of its alternatives, followed by the index
	static_assert(sizeof(variant_type<uint8_t>) == 2, "variant<char>");
	static_assert(sizeof(variant_type<int32_t>) == 8, "variant<int>");
	static_assert(sizeof(variant_type<bool, int32_t, double>) == 16, "variant<bool, int, double>");
	static_assert(alignof(variant_type<bool, int32_t, double>) == 8, "variant<bool, int, double>");
	static_assert(sizeof(variant_type<bool, string_type>) == sizeof(string_type) + alignof(string_type),
		"variant<bool, std::string>");
	// The layout must match the host std::variant, which has the same ABI
	static_assert(sizeof(variant_type<bool, int32_t, double>) == sizeof(std::variant<bool, int32_t, double>),
		"variant<bool, int, double> matches the host");
	static_assert(sizeof(variant_type<uint8_t, uint16_t, float>) == sizeof(std::variant<uint8_t, uint16_t, float>),
		"variant<char, short, float> matches the host");
	static_assert(variant_type<int32_t>::npos == 0xFF, "variant_npos is all ones");
	static_assert(std::is_standard_layout_v<variant_type<bool, int32_t, double>>, "Standard layout");
};
template struct GuestStdVariantLayout<4>;
template struct GuestStdVariantLayout<8>;

/// @brief A guest std::variant that lives in the arena, and which frees
/// itself (and its active alternative) at the end of the scope.
template <int W, typename... Types>
using ScopedGuestStdVariant = ScopedArenaObject<W, GuestStdVariant<W, Types...>>;

template <int W, typename T>
struct is_scoped_guest_stdvariant : std::false_type {};

template <int W, typename... Types>
struct is_scoped_guest_stdvariant<W, ScopedArenaObject<W, GuestStdVariant<W, Types...>>> : std::true_type {};

} // riscv
