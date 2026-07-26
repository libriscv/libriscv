#pragma once
#include "native_heap.hpp" // arena()
#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

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

// View into libstdc++'s std::string
template <int W>
struct GuestStdString {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	static constexpr std::size_t SSO = 15;

	gaddr_t ptr;
	gaddr_t size;
	union {
		char data[SSO + 1];
		gaddr_t capacity;
	};

	constexpr GuestStdString() noexcept : ptr(0), size(0), capacity(0) {}
	GuestStdString(machine_t& machine, std::string_view str = "")
		: ptr(0), size(0), capacity(0)
	{
		this->set_string(machine, 0, str);
	}
	GuestStdString(machine_t& machine, gaddr_t self, std::string_view str = "")
		: ptr(0), size(0), capacity(0)
	{
		this->set_string(machine, self, str);
	}

	bool empty() const noexcept { return size == 0; }

	std::string to_string(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->size <= SSO)
			return std::string(data, size);
		else if (this->size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 16MB)");
		// Copy the string from guest memory
		const auto view = machine.memory.memview(ptr, size);
		return std::string(view.data(), view.size());
	}

	std::string_view to_view(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->size <= SSO)
			return std::string_view(data, size);
		else if (this->size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 16MB)");
		// View the string from guest memory
		return machine.memory.memview(ptr, size);
	}

	void set_string(machine_t& machine, gaddr_t self, const void* str, std::size_t len, bool use_memarray = true)
	{
		this->free(machine);

		if (len <= SSO)
		{
			this->ptr = self + offsetof(GuestStdString, data);
			this->size = len;
			std::memcpy(this->data, str, len);
			this->data[len] = '\0';
		}
		else
		{
			this->ptr = machine.arena().malloc(len+1);
			this->size = len;
			this->capacity = len;
			if (use_memarray)
			{
				char* dst = machine.memory.template memarray<char>(this->ptr, len + 1);
				std::memcpy(dst, str, len);
				dst[len] = '\0';
			}
			else
			{
				machine.memory.memcpy(this->ptr, str, len);
				machine.memory.template write<uint8_t>(this->ptr + len, 0);
			}
		}
	}
	void set_string(machine_t& machine, gaddr_t self, std::string_view str)
	{
		this->set_string(machine, self, str.data(), str.size());
	}

	void move(gaddr_t self)
	{
		if (size <= SSO) {
			this->ptr = self + offsetof(GuestStdString, data);
		}
	}

	void free(machine_t& machine)
	{
		if (size > SSO) {
			machine.arena().free(ptr);
		}
		this->ptr = 0;
		this->size = 0;
	}
};

template <int W, typename T> struct GuestStdVector;
template <int W, typename K> struct GuestStdHash;
/// @brief See GuestStdUnorderedMap below. CacheHashCode must match the guest,
/// which stores the hash code in every node only for slow hash functions. It
/// defaults to what std::hash<K> would give, so a map that uses a custom hash
/// function has to say so: GuestStdUnorderedMap<W, K, V, false>.
template <int W, typename K, typename V,
	bool CacheHashCode = GuestStdHash<W, K>::cache_hash_code>
struct GuestStdUnorderedMap;
template <int W, typename... Types> struct GuestStdVariant;

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

/// @brief True for the guest-side standard containers that own guest
/// memory, and which must be freed with free() instead of a destructor.
template <int W, typename T>
struct is_guest_stdtype : std::bool_constant<
	std::is_same_v<T, GuestStdString<W>> ||
	is_guest_stdvector<W, T>::value ||
	is_guest_stdunordered_map<W, T>::value ||
	is_guest_stdvariant<W, T>::value> {};

/// @brief True when an object contains pointers back into itself, and it
/// must be told when it is moved to another address in guest memory.
template <int W, typename T>
struct is_self_referencing_guest_object : std::bool_constant<
	std::is_same_v<T, GuestStdString<W>> ||
	is_guest_stdunordered_map<W, T>::value> {};

/// @brief A variant is self-referencing when any of its alternatives are.
template <int W, typename... Types>
struct is_self_referencing_guest_object<W, GuestStdVariant<W, Types...>>
	: std::bool_constant<(is_self_referencing_guest_object<W, Types>::value || ...)> {};

/// @brief Destroy an object in guest memory, freeing any guest allocations
/// that it owns. Trivial types are simply destructed.
template <int W, typename T>
inline void free_guest_object(Machine<W>& machine, T& object)
{
	if constexpr (is_guest_stdtype<W, T>::value)
		object.free(machine);
	else
		object.~T();
}

/// @brief Tell an object that it now lives at the given guest address,
/// which the objects that point back into themselves need to know.
template <int W, typename T>
inline void relocate_guest_object(Machine<W>& machine, T& object, address_type<W> addr)
{
	if constexpr (std::is_same_v<T, GuestStdString<W>>)
		object.move(addr);
	else if constexpr (is_guest_stdunordered_map<W, T>::value || is_guest_stdvariant<W, T>::value)
		object.move(machine, addr);
	else {
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
	} else if constexpr (std::is_same_v<T, GuestStdString<W>>) {
		new (&dst) T(machine, addr, std::string_view(src));
	} else if constexpr (is_guest_stdvector<W, T>::value) {
		new (&dst) T(machine, src);
	} else if constexpr (is_guest_stdunordered_map<W, T>::value
		|| is_guest_stdvariant<W, T>::value) {
		new (&dst) T(machine, addr, src);
	} else {
		(void)machine; (void)addr;
		new (&dst) T(static_cast<T>(src));
	}
}

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
		if constexpr (is_self_referencing_guest_object<W, T>::value
			|| is_guest_stdvariant<W, T>::value) {
			// These need to know their own address in guest memory
			new (&dst) T(machine, self, std::forward<Args>(args)...);
		} else if constexpr (is_guest_stdtype<W, T>::value) {
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

	using host_variant_t = std::variant<
		std::conditional_t<std::is_same_v<Types, GuestStdString<W>>, std::string, Types>...>;

	/// @brief Copy the active alternative to the host. Nested guest
	/// containers are not supported here, use visit() for those.
	host_variant_t to_variant(const machine_t& machine) const
	{
		static_assert(!(is_guest_stdvector<W, Types>::value || ...)
			&& !(is_guest_stdunordered_map<W, Types>::value || ...)
			&& !(is_guest_stdvariant<W, Types>::value || ...),
			"Guest std::variant: Nested guest containers must be read using visit()");
		host_variant_t result;
		const bool valid = this->visit([&] (const auto& alt) {
			using T = std::decay_t<decltype(alt)>;
			if constexpr (std::is_same_v<T, GuestStdString<W>>)
				result = alt.to_string(machine);
			else {
				(void)machine;
				result = alt;
			}
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
		} else if constexpr (is_string_like && index_of<GuestStdString<W>>() < alternatives) {
			return index_of<GuestStdString<W>>();
		} else if constexpr (is_stdvector<A>::value) {
			// The only std::vector alternative, if there is exactly one
			constexpr bool matches[alternatives] { is_guest_stdvector<W, Types>::value... };
			return only_match(matches);
		} else if constexpr (is_host_map<A>::value) {
			// The only std::unordered_map alternative, if there is exactly one
			constexpr bool matches[alternatives] { is_guest_stdunordered_map<W, Types>::value... };
			return only_match(matches);
		} else {
			// The only alternative that the value converts to
			constexpr bool matches[alternatives] { is_alternative_convertible<A, Types>::value... };
			return only_match(matches);
		}
	}
};

// View into libstdc++ and LLVM libc++ std::vector (same layout)
template <int W, typename T>
struct GuestStdVector {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	gaddr_t ptr_begin;
	gaddr_t ptr_end;
	gaddr_t ptr_capacity;

	constexpr GuestStdVector() noexcept : ptr_begin(0), ptr_end(0), ptr_capacity(0) {}

	GuestStdVector(machine_t& machine, std::size_t elements)
		: ptr_begin(0), ptr_end(0), ptr_capacity(0)
	{
		auto [array, self] = this->alloc(machine, elements);
		(void)self;
		for (std::size_t i = 0; i < elements; i++) {
			new (&array[i]) T();
			this->relocate_element(machine, array, i);
		}
		// Set new end only after all elements are constructed
		this->ptr_end = this->ptr_begin + elements * sizeof(T);
	}

	GuestStdVector(machine_t& machine, const std::vector<std::string>& vec)
		: ptr_begin(0), ptr_end(0), ptr_capacity(0)
	{
		static_assert(std::is_same_v<T, GuestStdString<W>>, "GuestStdVector<T> must be a vector of GuestStdString<W>");
		if (vec.empty())
			return;

		// Specialization for std::vector<std::string>
		auto [array, self] = this->alloc(machine, vec.size());
		(void)self;
		for (std::size_t i = 0; i < vec.size(); i++) {
			T* str = new (&array[i]) T(machine, vec[i]);
			str->move(this->ptr_begin + i * sizeof(T));
		}
		// Set new end only after all elements are constructed
		this->ptr_end = this->ptr_begin + vec.size() * sizeof(T);
	}
	GuestStdVector(machine_t& machine, const std::vector<T>& vec = {})
		: ptr_begin(0), ptr_end(0), ptr_capacity(0)
	{
		if (!vec.empty())
			this->assign(machine, vec);
	}
	template <typename... Args>
	GuestStdVector(machine_t& machine, const std::array<T, sizeof...(Args)>& arr)
		: GuestStdVector(machine, std::vector<T> {arr.begin(), arr.end()})
	{
	}

	GuestStdVector(GuestStdVector&& other) noexcept
		: ptr_begin(other.ptr_begin), ptr_end(other.ptr_end), ptr_capacity(other.ptr_capacity)
	{
		other.ptr_begin = 0;
		other.ptr_end = 0;
		other.ptr_capacity = 0;
	}

	// Copying is intentionally shallow/fast, in order to avoid copying/duplication
	// Use the std::move if you need proper semantics
	GuestStdVector(const GuestStdVector& other) = default;
	GuestStdVector& operator=(const GuestStdVector& other) = default;

	gaddr_t data() const noexcept { return ptr_begin; }

	std::size_t size() const noexcept {
		return size_bytes() / sizeof(T);
	}
	bool empty() const noexcept {
		return size() == 0;
	}

	std::size_t capacity() const noexcept {
		return capacity_bytes() / sizeof(T);
	}

	T& at(machine_t& machine, std::size_t index, std::size_t max_bytes = 16UL << 20) {
		if (index >= size())
			throw std::out_of_range("Guest std::vector index out of range");
		return as_array(machine, max_bytes)[index];
	}
	const T& at(machine_t& machine, std::size_t index, std::size_t max_bytes = 16UL << 20) const {
		if (index >= size())
			throw std::out_of_range("Guest std::vector index out of range");
		return as_array(machine, max_bytes)[index];
	}

	void push_back(machine_t& machine, T&& value) {
		if (size_bytes() >= capacity_bytes())
			this->increase_capacity(machine);
		T* array = machine.memory.template memarray<T>(this->data(), size() + 1);
		new (&array[size()]) T(std::move(value));
		this->relocate_element(machine, array, size());
		this->ptr_end += sizeof(T);
	}
	void push_back(machine_t& machine, const T& value) {
		if (size_bytes() >= capacity_bytes())
			this->increase_capacity(machine);
		T* array = machine.memory.template memarray<T>(this->data(), size() + 1);
		new (&array[size()]) T(value);
		this->relocate_element(machine, array, size());
		this->ptr_end += sizeof(T);
	}

	// Specialization for std::string and std::string_view
	void push_back(machine_t& machine, std::string_view value) {
		static_assert(std::is_same_v<T, GuestStdString<W>>, "GuestStdVector: T must be a GuestStdString<W>");
		if (size_bytes() >= capacity_bytes())
			this->increase_capacity(machine);
		T* array = machine.memory.template memarray<T>(this->data(), size() + 1);
		const gaddr_t address = this->ptr_begin + size() * sizeof(T);
		new (&array[size()]) T(machine, address, value);
		this->ptr_end += sizeof(T);
	}
	// Specialization for std::vector<U>
	template <typename U>
	void push_back(machine_t& machine, const std::vector<U>& value) {
		static_assert(is_guest_stdvector<W, T>::value, "GuestStdVector: T must be a GuestStdVector itself");
		this->push_back(machine, GuestStdVector<W, U>(machine, value));
	}

	void pop_back(machine_t& machine) {
		if (size() == 0)
			throw std::out_of_range("Guest std::vector is empty");
		this->free_element(machine, size() - 1);
		this->ptr_end -= sizeof(T);
	}

	void append(machine_t& machine, const T* values, std::size_t count) {
		if (size_bytes() + count * sizeof(T) > capacity_bytes())
			this->reserve(machine, size() + count);
		T* array = machine.memory.template memarray<T>(this->data(), size() + count);
		for (std::size_t i = 0; i < count; i++) {
			new (&array[size() + i]) T(values[i]);
			this->relocate_element(machine, array, size() + i);
		}
		this->ptr_end += count * sizeof(T);
	}

	void clear(machine_t& machine) {
		for (std::size_t i = 0; i < size(); i++)
			this->free_element(machine, i);
		this->ptr_end = this->ptr_begin;
	}

	gaddr_t address_at(std::size_t index) const {
		if (index >= size())
			throw std::out_of_range("Guest std::vector index out of range");
		return ptr_begin + index * sizeof(T);
	}

	T *as_array(const machine_t& machine, std::size_t max_bytes = 16UL << 20) {
		if (size_bytes() > max_bytes)
			throw std::runtime_error("Guest std::vector has size > max_bytes");
		return machine.memory.template memarray<T>(data(), size());
	}
	const T *as_array(const machine_t& machine, std::size_t max_bytes = 16UL << 20) const {
		if (size_bytes() > max_bytes)
			throw std::runtime_error("Guest std::vector has size > max_bytes");
		return machine.memory.template memarray<T>(data(), size());
	}

#if RISCV_SPAN_AVAILABLE
	std::span<T> to_span(const machine_t& machine) {
		return std::span<T>(as_array(machine), size());
	}
	std::span<const T> to_span(const machine_t& machine) const {
		return std::span<const T>(as_array(machine), size());
	}
#endif

	// Iterators
	auto begin(machine_t& machine) { return as_array(machine); }
	auto end(machine_t& machine) { return as_array(machine) + size(); }

	std::vector<T> to_vector(const machine_t& machine) const {
		if (size_bytes() > capacity_bytes())
			throw std::runtime_error("Guest std::vector has size > capacity");
		// Copy the vector from guest memory
		const size_t elements = size();
		const T *array = machine.memory.template memarray<T>(data(), elements);
		return std::vector<T>(&array[0], &array[elements]);
	}

	/// @brief Specialization for std::string
	/// @param machine The RISC-V machine
	/// @return A vector of strings
	std::vector<std::string> to_string_vector(const machine_t& machine) const {
		if constexpr (std::is_same_v<T, GuestStdString<W>>) {
			std::vector<std::string> vec;
			const size_t elements = size();
			const T *array = machine.memory.template memarray<T>(data(), elements);
			vec.reserve(elements);
			for (std::size_t i = 0; i < elements; i++)
				vec.push_back(array[i].to_string(machine));
			return vec;
		} else {
			throw std::runtime_error("GuestStdVector: T must be a GuestStdString<W>");
		}
	}

	void assign(machine_t& machine, const std::vector<std::string>& vec)
	{
		static_assert(std::is_same_v<T, GuestStdString<W>>, "GuestStdVector<T> must be a vector of GuestStdString<W>");
		this->free(machine);
		if (vec.empty())
			return;

		// Specialization for std::vector<std::string>
		auto [array, self] = this->alloc(machine, vec.size());
		(void)self;
		for (std::size_t i = 0; i < vec.size(); i++) {
			T* str = new (&array[i]) T(machine, vec[i]);
			str->move(this->ptr_begin + i * sizeof(T));
		}
		// Set new end only after all elements are constructed
		this->ptr_end = this->ptr_begin + vec.size() * sizeof(T);
	}

	void assign(machine_t& machine, const std::vector<T>& vec)
	{
		auto [array, self] = alloc(machine, vec.size());
		(void)self;
		std::copy(vec.begin(), vec.end(), array);
		for (std::size_t i = 0; i < vec.size(); i++)
			this->relocate_element(machine, array, i);
		this->ptr_end = this->ptr_begin + vec.size() * sizeof(T);
	}

	void assign(machine_t& machine, const T* values, std::size_t count)
	{
		auto [array, self] = alloc(machine, count);
		(void)self;
		std::copy(values, values + count, array);
		for (std::size_t i = 0; i < count; i++)
			this->relocate_element(machine, array, i);
		this->ptr_end = this->ptr_begin + count * sizeof(T);
	}

	/// @brief Replace the contents of the vector with the given array,
	/// by assuming ownership of the array memory (which must have been
	/// allocated with guest_alloc and *must* be properly initialized
	/// with the given values).
	void assume_ownership(machine_t& machine, gaddr_t array, std::size_t count)
	{
		this->free(machine);
		this->ptr_begin = array;
		this->ptr_end = array + count * sizeof(T);
		this->ptr_capacity = this->ptr_end;
	}

	void resize(machine_t& machine, std::size_t new_size)
	{
		if (new_size < size()) {
			for (std::size_t i = new_size; i < size(); i++)
				this->free_element(machine, i);
			this->ptr_end = this->ptr_begin + new_size * sizeof(T);
		} else if (new_size > size()) {
			if (new_size > capacity())
				this->reserve(machine, new_size);
			T* array = machine.memory.template memarray<T>(this->data(), new_size);
			for (std::size_t i = size(); i < new_size; i++) {
				new (&array[i]) T();
				this->relocate_element(machine, array, i);
			}
			this->ptr_end = this->ptr_begin + new_size * sizeof(T);
		}
	}

	void reserve(machine_t& machine, std::size_t elements)
	{
		if (elements <= capacity())
			return;

		GuestStdVector<W, T> old_vec(std::move(*this));
		// Allocate new memory
		auto [array, self] = this->alloc(machine, elements);
		(void)self;
		if (!old_vec.empty()) {
			std::copy(old_vec.as_array(machine), old_vec.as_array(machine) + old_vec.size(), array);
			// Free the old vector manually (as we don't want to call the destructor(s))
			machine.arena().free(old_vec.ptr_begin);
		}
		this->ptr_end = this->ptr_begin + old_vec.size() * sizeof(T);
		// Adjust the elements that point back into themselves (std::string
		// small-string optimization, std::unordered_map bucket pointers)
		if constexpr (element_is_self_referencing) {
			T* array = machine.memory.template memarray<T>(this->data(), size());
			for (std::size_t i = 0; i < size(); i++)
				this->relocate_element(machine, array, i);
		}
	}

	void free(machine_t& machine) {
		if (this->ptr_begin != 0) {
			for (std::size_t i = 0; i < size(); i++)
				this->free_element(machine, i);
			machine.arena().free(this->data());
			this->ptr_begin = 0;
			this->ptr_end = 0;
			this->ptr_capacity = 0;
		}
	}

	std::size_t size_bytes() const noexcept { return ptr_end - ptr_begin; }
	std::size_t capacity_bytes() const noexcept { return ptr_capacity - ptr_begin; }

private:
	void increase_capacity(machine_t& machine) {
		this->reserve(machine, capacity() * 2 + 4);
	}

	std::tuple<T *, gaddr_t> alloc(machine_t& machine, std::size_t elements) {
		this->free(machine);

		this->ptr_begin = machine.arena().malloc(elements * sizeof(T));
		this->ptr_end = this->ptr_begin;
		this->ptr_capacity = this->ptr_begin + elements * sizeof(T);
		return { machine.memory.template memarray<T>(this->data(), elements), this->data() };
	}

	/// @brief True when the elements contain pointers back into themselves,
	/// and must be told when they are moved to another address.
	static constexpr bool element_is_self_referencing =
		is_self_referencing_guest_object<W, T>::value;

	/// @brief Tell an element that it now lives at its current address
	void relocate_element(machine_t& machine, T* array, std::size_t index) {
		relocate_guest_object<W>(machine, array[index],
			this->ptr_begin + index * sizeof(T));
	}

	void free_element(machine_t& machine, std::size_t index) {
		free_guest_object<W>(machine, this->at(machine, index));
	}
};

// Replica of libstdc++'s std::_Hash_bytes (a MurmurHash2 variant), which is
// what std::hash uses for strings and floating-point values. The width of the
// guests std::size_t decides which variant is used, so both are implemented.
template <int W>
struct GuestStdHashBytes
{
	using ghash_t = riscv::address_type<W>; // The guests std::size_t
	static_assert(W == 4 || W == 8, "Guest std::hash requires a 32- or 64-bit guest");

	/// @brief The seed that libstdc++ uses for all of its byte hashing
	static constexpr ghash_t SEED = ghash_t(0xc70f6907UL);

	static ghash_t hash(const void* data, std::size_t len, ghash_t seed = SEED) noexcept
	{
		const char* buf = (const char *)data;
		if constexpr (W == 8) {
			static constexpr uint64_t mul =
				(uint64_t(0xc6a4a793) << 32) + uint64_t(0x5bd1e995);
			// The main loop consumes the input 8 bytes at a time
			const std::size_t aligned_len = len & ~std::size_t(0x7);
			const char* const end = buf + aligned_len;
			uint64_t hash = seed ^ (len * mul);
			for (const char* p = buf; p != end; p += 8)
				hash = (hash ^ (shift_mix(load_word<uint64_t>(p) * mul) * mul)) * mul;
			if ((len & 0x7) != 0)
				hash = (hash ^ load_bytes(end, len - aligned_len)) * mul;
			hash = shift_mix(hash) * mul;
			return shift_mix(hash);
		} else {
			static constexpr uint32_t m = 0x5bd1e995;
			uint32_t hash = uint32_t(seed) ^ uint32_t(len);
			// Mix 4 bytes at a time into the hash
			while (len >= 4) {
				uint32_t k = load_word<uint32_t>(buf);
				k *= m;
				k ^= k >> 24;
				k *= m;
				hash *= m;
				hash ^= k;
				buf += 4;
				len -= 4;
			}
			// Handle the last few bytes of the input
			switch (len) {
			case 3: hash ^= uint32_t((unsigned char)buf[2]) << 16; [[fallthrough]];
			case 2: hash ^= uint32_t((unsigned char)buf[1]) << 8;  [[fallthrough]];
			case 1: hash ^= uint32_t((unsigned char)buf[0]);
				hash *= m;
			}
			// Murmur's mix function
			hash ^= hash >> 13;
			hash *= m;
			hash ^= hash >> 15;
			return hash;
		}
	}

private:
	static uint64_t shift_mix(uint64_t value) noexcept { return value ^ (value >> 47); }

	template <typename T>
	static T load_word(const char* p) noexcept {
		T value;
		std::memcpy(&value, p, sizeof(value));
		return value;
	}
	static uint64_t load_bytes(const char* p, std::size_t n) noexcept {
		uint64_t result = 0;
		while (n > 0)
			result = (result << 8) + (unsigned char)p[--n];
		return result;
	}
};

/// @brief Replica of the guests std::hash<K> and std::equal_to<K>, used to
/// find the bucket of a key. Specialize this in order to support other key
/// types than integers, enums, floats and std::string.
template <int W, typename K>
struct GuestStdHash
{
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	/// @brief libstdc++ stores the hash code in every node when the hash
	/// function is not considered fast (see __is_fast_hash), which changes
	/// the node layout. This must match the guest exactly. It is only the
	/// default for GuestStdUnorderedMap: a map that uses a hash function
	/// other than std::hash<K> overrides it with the CacheHashCode argument.
	static constexpr bool cache_hash_code = false;

	static gaddr_t hash(const machine_t&, const K& key)
	{
		if constexpr (std::is_integral_v<K> || std::is_enum_v<K>) {
			// std::hash of an integral type is the identity function,
			// truncated to the width of the guests std::size_t
			return static_cast<gaddr_t>(key);
		} else if constexpr (std::is_same_v<K, float> || std::is_same_v<K, double>) {
			// 0.0 and -0.0 both hash to zero
			return key != K(0.0) ? GuestStdHashBytes<W>::hash(&key, sizeof(key)) : 0;
		} else {
			static_assert(std::is_integral_v<K>,
				"GuestStdHash: Unsupported key type. Please specialize riscv::GuestStdHash<W, K>");
			return 0;
		}
	}
	static bool equals(const machine_t&, const K& a, const K& b) {
		return a == b;
	}
};

// std::hash<std::string> is not a "fast" hash, and so libstdc++ stores the
// hash code of every key in the node it belongs to.
template <int W>
struct GuestStdHash<W, GuestStdString<W>>
{
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	static constexpr bool cache_hash_code = true;

	static gaddr_t hash(const machine_t& machine, const GuestStdString<W>& key) {
		const auto view = key.to_view(machine);
		return GuestStdHashBytes<W>::hash(view.data(), view.size());
	}
	static gaddr_t hash(const machine_t&, std::string_view key) {
		return GuestStdHashBytes<W>::hash(key.data(), key.size());
	}
	static bool equals(const machine_t& machine, const GuestStdString<W>& a, const GuestStdString<W>& b) {
		return a.to_view(machine) == b.to_view(machine);
	}
	static bool equals(const machine_t& machine, const GuestStdString<W>& a, std::string_view b) {
		return a.to_view(machine) == b;
	}
};

// The bucket counts used by libstdc++'s _Prime_rehash_policy. The list is
// truncated: beyond the last entry we simply stop growing the bucket array.
struct GuestStdHashPolicy {
	static constexpr unsigned char fast_bkt[] =
		{ 2, 2, 2, 3, 5, 5, 7, 7, 11, 11, 11, 11, 13, 13 };
	static constexpr uint32_t primes[] = {
		2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67,
		71, 73, 79, 83, 89, 97, 103, 109, 113, 127, 137, 139, 149, 157, 167, 179,
		193, 199, 211, 227, 241, 257, 277, 293, 313, 337, 359, 383, 409, 439, 467,
		503, 541, 577, 619, 661, 709, 761, 823, 887, 953, 1031, 1109, 1193, 1289,
		1381, 1493, 1613, 1741, 1879, 2029, 2179, 2357, 2549, 2753, 2971, 3209,
		3469, 3739, 4027, 4349, 4703, 5087, 5503, 5953, 6427, 6949, 7517, 8123,
		8783, 9497, 10273, 11113, 12011, 12983, 14033, 15173, 16411, 17749, 19183,
		20753, 22447, 24281, 26267, 28411, 30727, 33223, 35933, 38873, 42043,
		45481, 49201, 53201, 57557, 62233, 67307, 72817, 78779, 85229, 92203,
		99733, 107897, 116731, 126271, 136607, 147793, 159871, 172933, 187091,
		202409, 218971, 236897, 256279, 277261, 299951, 324503, 351061, 379787,
		410857, 444487, 480881, 520241, 562841, 608903, 658753, 712697, 771049,
		834181, 902483, 976369, 1056323, 1142821, 1236397, 1337629, 1447153,
		1565659, 1693859, 1832561, 1982627, 2144977, 2320627, 2510653, 2716249,
		2938679, 3179303, 3439651, 3721303, 4026031, 4355707, 4712381, 5098259,
		5515729, 5967347, 6456007, 6984629, 7556579, 8175383, 8844859, 9569143,
		10352717, 11200489, 12117689, 13109983, 14183539, 15345007, 16601593,
		17961079
	};
	static constexpr std::size_t prime_count = sizeof(primes) / sizeof(primes[0]);
};

/// @brief Mirrors std::pair<const K, V>, the value_type of std::unordered_map
template <typename K, typename V>
struct GuestStdPair {
	K first;
	V second;
};

/// @brief Mirrors libstdc++'s __detail::_Hash_node, a node in the singly-
/// linked list of every element in an unordered container. The hash code is
/// only stored in the node for slow hash functions, eg. std::hash<std::string>.
template <int W, typename K, typename V, bool CacheHashCode>
struct GuestHashNode;

template <int W, typename K, typename V>
struct GuestHashNode<W, K, V, false> {
	riscv::address_type<W> next; // _Hash_node_base::_M_nxt
	GuestStdPair<K, V> value;    // _Hash_node_value_base::_M_storage
};

template <int W, typename K, typename V>
struct GuestHashNode<W, K, V, true> {
	riscv::address_type<W> next;
	GuestStdPair<K, V> value;
	riscv::address_type<W> hash_code; // _Hash_node_code_cache::_M_hash_code
};

// View into libstdc++'s std::unordered_map<K, V>
//
// The map consists of an array of buckets, and a singly-linked list that
// holds every element in the map. A bucket does not point at its first
// element, but at the element *before* it, which is why the map has a
// before_begin member that acts as the head of the node list. As a result,
// a non-empty map contains a pointer back into itself, and it must be told
// when it is moved to another address (see move()).
//
// NOTE: The map operations that can allocate need to know the address of the
// map itself in guest memory, and it is passed to them as the self argument.
//
// NOTE: libstdc++ decides the node layout from the hash function of the map:
// the hash code of every key is stored in its node unless __is_fast_hash<H>,
// which is true for every hash function except the ones the standard library
// marks as slow (std::hash<std::string> among them). A map that uses a custom
// hash function - even one that just forwards to std::hash - therefore does
// *not* store hash codes, and must be given CacheHashCode = false in order to
// match the guest. Getting it wrong shifts every key and value in the node.
template <int W, typename K, typename V, bool CacheHashCode>
struct GuestStdUnorderedMap
{
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	using key_type = K;
	using mapped_type = V;
	using hasher = GuestStdHash<W, K>;
	/// @brief True when the guest stores the hash code of each key in its node
	static constexpr bool cache_hash_code = CacheHashCode;
	using node_type  = GuestHashNode<W, K, V, cache_hash_code>;
	using value_type = GuestStdPair<K, V>;

	// The libstdc++ _Hashtable layout (7 machine words)
	gaddr_t buckets;       // __node_base_ptr* _M_buckets
	gaddr_t bucket_cnt;    // size_type      _M_bucket_count
	gaddr_t before_begin;  // __node_base    _M_before_begin (a lone _M_nxt)
	gaddr_t element_cnt;   // size_type      _M_element_count
	float   max_load;      // _Prime_rehash_policy::_M_max_load_factor
	gaddr_t next_resize;   // _Prime_rehash_policy::_M_next_resize
	gaddr_t single_bucket; // __node_base_ptr _M_single_bucket

	// Offsets of the members that the map itself points at
	static constexpr std::size_t BEFORE_BEGIN_OFF  = 2 * sizeof(gaddr_t);
	static constexpr std::size_t SINGLE_BUCKET_OFF = 6 * sizeof(gaddr_t);
	// Offsets of the key and the value inside a node
	static constexpr std::size_t KEY_OFF =
		offsetof(node_type, value) + offsetof(value_type, first);
	static constexpr std::size_t VALUE_OFF =
		offsetof(node_type, value) + offsetof(value_type, second);

	/// @brief Create an empty map. NOTE: It is not usable by the guest until
	/// it has been given its own guest address using move().
	constexpr GuestStdUnorderedMap() noexcept
		: buckets(0), bucket_cnt(1), before_begin(0), element_cnt(0),
		  max_load(1.0f), next_resize(0), single_bucket(0) {}

	/// @brief Create an empty map that lives at the guest address self.
	GuestStdUnorderedMap(machine_t& machine, gaddr_t self)
		: GuestStdUnorderedMap()
	{
		this->move(machine, self);
	}

	/// @brief Create a map at the guest address self, filled with the
	/// key/value pairs of a host container, eg. a std::unordered_map.
	template <typename Container>
	GuestStdUnorderedMap(machine_t& machine, gaddr_t self, const Container& container)
		: GuestStdUnorderedMap()
	{
		this->move(machine, self);
		this->assign(machine, self, container);
	}

	// Copying is intentionally shallow/fast, like GuestStdVector. A shallow
	// copy shares the nodes with the original, and it must be given its own
	// address with move() before the guest can use it.
	GuestStdUnorderedMap(const GuestStdUnorderedMap& other) = default;
	GuestStdUnorderedMap& operator=(const GuestStdUnorderedMap& other) = default;

	std::size_t size() const noexcept { return this->element_cnt; }
	bool empty() const noexcept { return this->element_cnt == 0; }
	std::size_t bucket_count() const noexcept { return this->bucket_cnt; }
	float max_load_factor() const noexcept { return this->max_load; }
	float load_factor() const noexcept {
		return float(this->element_cnt) / float(this->bucket_cnt);
	}

	void set_max_load_factor(float mlf) {
		this->max_load = mlf;
		if (this->bucket_cnt > 1)
			this->next_resize = floor_size(this->bucket_cnt * double(mlf));
	}

	/// @brief Find the value of the given key.
	/// @return A pointer into guest memory, or null when not found.
	template <typename KeyArg>
	V* find(const machine_t& machine, const KeyArg& key) const {
		const gaddr_t addr = this->find_node(machine, key);
		if (addr == 0)
			return nullptr;
		return &this->node_at(machine, addr).value.second;
	}

	/// @brief Find the value of the given key, or throw std::out_of_range.
	template <typename KeyArg>
	V& at(const machine_t& machine, const KeyArg& key) const {
		if (V* value = this->find(machine, key))
			return *value;
		throw std::out_of_range("Guest std::unordered_map: Key not found");
	}

	template <typename KeyArg>
	bool contains(const machine_t& machine, const KeyArg& key) const {
		return this->find_node(machine, key) != 0;
	}
	template <typename KeyArg>
	std::size_t count(const machine_t& machine, const KeyArg& key) const {
		return this->contains(machine, key) ? 1u : 0u;
	}

	/// @brief Insert a key/value pair, overwriting the value of an existing key.
	/// @param self The address of this map in guest memory.
	/// @return The guest address of the node that holds the pair.
	template <typename KeyArg, typename ValueArg>
	gaddr_t insert_or_assign(machine_t& machine, gaddr_t self,
		const KeyArg& key, const ValueArg& value)
	{
		const gaddr_t code = hasher::hash(machine, key);
		if (this->element_cnt != 0) {
			const gaddr_t prev =
				this->find_before_node(machine, this->bucket_index(code), key, code);
			if (prev != 0) {
				const gaddr_t addr = this->next_ref(machine, prev);
				free_member(machine, this->node_at(machine, addr).value.second);
				store_member<V>(machine, addr + VALUE_OFF, value);
				return addr;
			}
		}
		// Grow the bucket array first, exactly like libstdc++ does
		this->maybe_rehash(machine, self, 1);

		const gaddr_t addr = machine.arena().malloc(sizeof(node_type));
		if (addr == 0)
			throw std::bad_alloc();
		node_type& node = this->node_at(machine, addr);
		node.next = 0;
		if constexpr (cache_hash_code)
			node.hash_code = code;
		store_member<K>(machine, addr + KEY_OFF, key);
		store_member<V>(machine, addr + VALUE_OFF, value);

		this->insert_bucket_begin(machine, self, this->bucket_index(code), addr);
		this->element_cnt += 1;
		return addr;
	}

	/// @brief Insert a key/value pair, unless the key already exists.
	/// @return The guest address of the node that holds the pair.
	template <typename KeyArg, typename ValueArg>
	gaddr_t try_emplace(machine_t& machine, gaddr_t self,
		const KeyArg& key, const ValueArg& value)
	{
		if (const gaddr_t addr = this->find_node(machine, key); addr != 0)
			return addr;
		return this->insert_or_assign(machine, self, key, value);
	}

	/// @brief Remove the given key from the map.
	/// @return True when an element was removed.
	template <typename KeyArg>
	bool erase(machine_t& machine, gaddr_t self, const KeyArg& key)
	{
		if (this->element_cnt == 0)
			return false;
		const gaddr_t code = hasher::hash(machine, key);
		const std::size_t bkt = this->bucket_index(code);
		const gaddr_t prev = this->find_before_node(machine, bkt, key, code);
		if (prev == 0)
			return false;

		const gaddr_t addr = this->next_ref(machine, prev);
		node_type& node = this->node_at(machine, addr);
		const gaddr_t next = node.next;
		// The bucket of the next node decides whether this bucket becomes empty
		const std::size_t next_bkt = next != 0
			? this->bucket_index(this->node_hash(machine, this->node_at(machine, next)))
			: bkt;

		free_member(machine, node.value.first);
		free_member(machine, node.value.second);
		machine.arena().free(addr);
		this->element_cnt -= 1;

		gaddr_t* bkts = this->bucket_array(machine);
		if (prev == bkts[bkt])
			this->remove_bucket_begin(machine, self, bkt, next, next_bkt);
		else if (next_bkt != bkt)
			bkts[next_bkt] = prev;
		this->next_ref(machine, prev) = next;
		return true;
	}

	/// @brief Remove every element, keeping the bucket array.
	void clear(machine_t& machine)
	{
		gaddr_t addr = this->before_begin;
		while (addr != 0) {
			node_type& node = this->node_at(machine, addr);
			const gaddr_t next = node.next;
			free_member(machine, node.value.first);
			free_member(machine, node.value.second);
			machine.arena().free(addr);
			addr = next;
		}
		this->before_begin = 0;
		this->element_cnt = 0;
		if (this->buckets != 0)
			std::memset(this->bucket_array(machine), 0, this->bucket_cnt * sizeof(gaddr_t));
	}

	/// @brief Free every element and the bucket array. The map is detached
	/// from guest memory afterwards: use move() to make it usable again.
	void free(machine_t& machine)
	{
		this->clear(machine);
		if (this->bucket_cnt != 1 && this->buckets != 0)
			machine.arena().free(this->buckets);
		this->buckets = 0;
		this->bucket_cnt = 1;
		this->single_bucket = 0;
		this->next_resize = 0;
	}

	/// @brief Tell the map that it now lives at the guest address self,
	/// updating the pointers that point back into the map itself.
	void move(machine_t& machine, gaddr_t self)
	{
		if (this->bucket_cnt == 1) {
			// libstdc++ uses the single_bucket member as a 1-element bucket array
			this->buckets = self + SINGLE_BUCKET_OFF;
		}
		if (this->before_begin != 0) {
			// Exactly one bucket points at the before_begin member: the bucket
			// that holds the first node of the global list of nodes.
			gaddr_t* bkts = this->bucket_array(machine);
			const std::size_t bkt = this->bucket_index(
				this->node_hash(machine, this->node_at(machine, this->before_begin)));
			bkts[bkt] = self + BEFORE_BEGIN_OFF;
		}
	}

	/// @brief Replace the contents with the key/value pairs of a host
	/// container, eg. a std::unordered_map or a std::vector of pairs.
	template <typename Container>
	void assign(machine_t& machine, gaddr_t self, const Container& container)
	{
		this->clear(machine);
		this->reserve(machine, self, container.size());
		for (const auto& entry : container)
			this->insert_or_assign(machine, self, entry.first, entry.second);
	}

	/// @brief Visit every element in the map, in an unspecified order. The
	/// callback is invoked as callback(const K& key, V& value).
	template <typename F>
	void for_each(const machine_t& machine, F&& callback) const
	{
		gaddr_t addr = this->before_begin;
		while (addr != 0) {
			node_type& node = this->node_at(machine, addr);
			addr = node.next;
			callback(const_cast<const K&>(node.value.first), node.value.second);
		}
	}

	using host_key_type =
		std::conditional_t<std::is_same_v<K, GuestStdString<W>>, std::string, K>;
	using host_mapped_type =
		std::conditional_t<std::is_same_v<V, GuestStdString<W>>, std::string, V>;

	/// @brief Copy the whole map to the host. Nested guest containers are not
	/// supported here, use for_each() for those.
	std::unordered_map<host_key_type, host_mapped_type> to_map(const machine_t& machine) const
	{
		static_assert(!is_guest_stdvector<W, V>::value && !is_guest_stdunordered_map<W, V>::value
			&& !is_guest_stdvariant<W, V>::value,
			"Guest std::unordered_map: Nested guest containers must be read using for_each()");
		std::unordered_map<host_key_type, host_mapped_type> result;
		result.reserve(this->size());
		this->for_each(machine, [&] (const K& key, const V& value) {
			result.emplace(to_host<K, host_key_type>(machine, key),
				to_host<V, host_mapped_type>(machine, value));
		});
		return result;
	}

	/// @brief Set the bucket count to at least count, rehashing the elements.
	/// Mirrors std::unordered_map::rehash().
	void rehash(machine_t& machine, gaddr_t self, std::size_t count)
	{
		const gaddr_t saved_resize = this->next_resize;
		std::size_t buckets =
			std::max(this->bkt_for_elements(std::size_t(this->element_cnt) + 1), count);
		buckets = this->next_bucket_count(buckets);
		if (buckets != this->bucket_cnt)
			this->rehash_buckets(machine, self, buckets);
		else // No rehash: keep the resize threshold consistent with the state
			this->next_resize = saved_resize;
	}

	/// @brief Make room for count elements without rehashing later.
	/// Mirrors std::unordered_map::reserve().
	void reserve(machine_t& machine, gaddr_t self, std::size_t count)
	{
		this->rehash(machine, self, this->bkt_for_elements(count));
	}

	/// @brief The guest address of the node of the given key, or zero.
	template <typename KeyArg>
	gaddr_t find_node(const machine_t& machine, const KeyArg& key) const
	{
		if (this->element_cnt == 0)
			return 0;
		const gaddr_t code = hasher::hash(machine, key);
		const gaddr_t prev =
			this->find_before_node(machine, this->bucket_index(code), key, code);
		return prev != 0 ? this->next_ref(machine, prev) : 0;
	}

	/// @brief View a node in guest memory, given its address.
	node_type& node_at(const machine_t& machine, gaddr_t addr) const {
		return *machine.memory.template memarray<node_type>(addr, 1);
	}

	/// @brief The bucket array in guest memory. When there is only a single
	/// bucket it is the single_bucket member of the map itself.
	gaddr_t *bucket_array(const machine_t& machine) const {
		if (this->buckets == 0)
			throw std::runtime_error("Guest std::unordered_map has no address in guest memory (see move())");
		return machine.memory.template memarray<gaddr_t>(this->buckets, this->bucket_cnt);
	}

	/// @brief The bucket that a hash code belongs to (__detail::_Mod_range_hashing)
	std::size_t bucket_index(gaddr_t code) const {
		// A guest-provided map always has at least one bucket
		if (this->bucket_cnt == 0)
			throw std::runtime_error("Guest std::unordered_map has zero buckets");
		return code % this->bucket_cnt;
	}

	/// @brief The hash code of the key of a node, cached or recomputed.
	gaddr_t node_hash(const machine_t& machine, const node_type& node) const {
		if constexpr (cache_hash_code)
			return node.hash_code;
		else
			return hasher::hash(machine, node.value.first);
	}

	/// @brief The _M_nxt member of a node (or of before_begin), which is
	/// always the first word of a node base.
	gaddr_t& next_ref(const machine_t& machine, gaddr_t node_base) const {
		return *machine.memory.template memarray<gaddr_t>(node_base, 1);
	}

	/// @brief The address of the node *before* the node of the given key, as
	/// stored in the buckets. Mirrors _M_find_before_node().
	template <typename KeyArg>
	gaddr_t find_before_node(const machine_t& machine, std::size_t bkt,
		const KeyArg& key, gaddr_t code) const
	{
		gaddr_t* bkts = this->bucket_array(machine);
		gaddr_t prev = bkts[bkt];
		if (prev == 0)
			return 0;
		for (gaddr_t addr = this->next_ref(machine, prev); addr != 0; ) {
			node_type& node = this->node_at(machine, addr);
			if (this->node_equals(machine, node, key, code))
				return prev;
			// Stop at the end of the list, or when leaving this bucket
			if (node.next == 0)
				break;
			if (this->bucket_index(this->node_hash(machine, this->node_at(machine, node.next))) != bkt)
				break;
			prev = addr;
			addr = node.next;
		}
		return 0;
	}

	template <typename KeyArg>
	bool node_equals(const machine_t& machine, const node_type& node,
		const KeyArg& key, gaddr_t code) const
	{
		if constexpr (cache_hash_code) {
			if (node.hash_code != code)
				return false;
		} else {
			(void)code;
		}
		return hasher::equals(machine, node.value.first, key);
	}

	/// @brief Grow the bucket array when inserting n_ins elements would
	/// exceed the load factor. Mirrors _Prime_rehash_policy::_M_need_rehash().
	/// @return True when the map was rehashed.
	bool maybe_rehash(machine_t& machine, gaddr_t self, std::size_t n_ins)
	{
		if (std::size_t(this->element_cnt) + n_ins <= this->next_resize)
			return false;
		// A zero resize threshold means that no buckets have been allocated
		// yet, in which case libstdc++ starts out with at least 11 buckets
		const std::size_t elements = std::max(std::size_t(this->element_cnt) + n_ins,
			this->next_resize == 0 ? std::size_t(11) : std::size_t(0));
		const double min_buckets = elements / double(this->max_load);
		if (min_buckets >= double(this->bucket_cnt)) {
			const std::size_t count = this->next_bucket_count(
				std::max(floor_size(min_buckets) + 1, std::size_t(this->bucket_cnt) * 2));
			this->rehash_buckets(machine, self, count);
			return true;
		}
		this->next_resize = floor_size(this->bucket_cnt * double(this->max_load));
		return false;
	}

	/// @brief The bucket count that libstdc++ would choose for count buckets,
	/// which also updates the resize threshold. Mirrors _M_next_bkt().
	std::size_t next_bucket_count(std::size_t count)
	{
		using policy = GuestStdHashPolicy;
		if (count < sizeof(policy::fast_bkt)) {
			if (count == 0) {
				// Keep the threshold at zero, so that the next insertion allocates
				return 1;
			}
			this->next_resize = floor_size(policy::fast_bkt[count] * double(this->max_load));
			return policy::fast_bkt[count];
		}
		// The last prime is never selected by the search, just like in libstdc++
		const uint32_t* const last = policy::primes + policy::prime_count - 1;
		const uint32_t* const it = std::lower_bound(policy::primes + 6, last, count);
		if (it == last) {
			// Never rehash again, as we have reached the biggest bucket count
			this->next_resize = std::size_t(-1);
			return std::max(count, std::size_t(*last));
		}
		this->next_resize = floor_size(*it * double(this->max_load));
		return *it;
	}

	/// @brief The bucket count needed to hold count elements
	std::size_t bkt_for_elements(std::size_t count) const {
		return ceil_size(count / double(this->max_load));
	}

	std::size_t size_bytes() const noexcept {
		return this->bucket_cnt * sizeof(gaddr_t) + this->element_cnt * sizeof(node_type);
	}

	/// @brief Move every node into a newly allocated bucket array.
	/// Mirrors _M_rehash_aux() for containers with unique keys.
	void rehash_buckets(machine_t& machine, gaddr_t self, std::size_t count)
	{
		const gaddr_t bbegin = self + BEFORE_BEGIN_OFF;
		gaddr_t new_buckets;
		if (count == 1) {
			this->single_bucket = 0;
			new_buckets = self + SINGLE_BUCKET_OFF;
		} else {
			new_buckets = machine.arena().malloc(count * sizeof(gaddr_t));
			if (new_buckets == 0)
				throw std::bad_alloc();
			std::memset(machine.memory.template memarray<gaddr_t>(new_buckets, count),
				0, count * sizeof(gaddr_t));
		}
		gaddr_t* bkts = machine.memory.template memarray<gaddr_t>(new_buckets, count);

		// Re-insert every node, which also reverses the order of the list
		gaddr_t addr = this->before_begin;
		this->before_begin = 0;
		std::size_t bbegin_bkt = 0;
		while (addr != 0) {
			node_type& node = this->node_at(machine, addr);
			const gaddr_t next = node.next;
			const std::size_t bkt = this->node_hash(machine, node) % count;
			if (bkts[bkt] == 0) {
				node.next = this->before_begin;
				this->before_begin = addr;
				bkts[bkt] = bbegin;
				if (node.next != 0)
					bkts[bbegin_bkt] = addr;
				bbegin_bkt = bkt;
			} else {
				gaddr_t& prev_next = this->next_ref(machine, bkts[bkt]);
				node.next = prev_next;
				prev_next = addr;
			}
			addr = next;
		}

		if (this->bucket_cnt != 1 && this->buckets != 0)
			machine.arena().free(this->buckets);
		this->bucket_cnt = count;
		this->buckets = new_buckets;
	}

	/// @brief Insert a node at the beginning of a bucket.
	/// Mirrors _M_insert_bucket_begin().
	void insert_bucket_begin(machine_t& machine, gaddr_t self, std::size_t bkt, gaddr_t addr)
	{
		gaddr_t* bkts = this->bucket_array(machine);
		if (bkts[bkt] != 0) {
			// The bucket is not empty: insert after the node before the bucket
			gaddr_t& prev_next = this->next_ref(machine, bkts[bkt]);
			this->node_at(machine, addr).next = prev_next;
			prev_next = addr;
		} else {
			// The bucket is empty: the node becomes the first node of the
			// list, and the bucket points at our before_begin member
			node_type& node = this->node_at(machine, addr);
			node.next = this->before_begin;
			this->before_begin = addr;
			if (node.next != 0) {
				// Update the bucket that pointed at before_begin
				const std::size_t old_bkt = this->bucket_index(
					this->node_hash(machine, this->node_at(machine, node.next)));
				bkts[old_bkt] = addr;
			}
			bkts[bkt] = self + BEFORE_BEGIN_OFF;
		}
	}

	/// @brief Unlink the first node of a bucket. Mirrors _M_remove_bucket_begin().
	void remove_bucket_begin(machine_t& machine, gaddr_t self, std::size_t bkt,
		gaddr_t next, std::size_t next_bkt)
	{
		if (next == 0 || next_bkt != bkt) {
			// The bucket is now empty
			gaddr_t* bkts = this->bucket_array(machine);
			if (next != 0)
				bkts[next_bkt] = bkts[bkt];
			if (bkts[bkt] == self + BEFORE_BEGIN_OFF)
				this->before_begin = next;
			bkts[bkt] = 0;
		}
	}

	/// @brief Construct a key or a value of a node in guest memory
	template <typename T, typename Arg>
	static void store_member(machine_t& machine, gaddr_t addr, const Arg& src)
	{
		T& dst = *machine.memory.template memarray<T>(addr, 1);
		construct_guest_object<W, T>(machine, addr, dst, src);
	}

	/// @brief Free a key or a value of a node
	template <typename T>
	static void free_member(machine_t& machine, T& member) {
		free_guest_object<W>(machine, member);
	}

	/// @brief Tell a key or a value that it has moved to another address
	template <typename T>
	static void relocate_member(machine_t& machine, T& member, gaddr_t addr) {
		relocate_guest_object<W>(machine, member, addr);
	}

private:
	template <typename T, typename HostT>
	static HostT to_host(const machine_t& machine, const T& value) {
		if constexpr (std::is_same_v<T, GuestStdString<W>>)
			return value.to_string(machine);
		else
			return value;
	}
	// std::floor and std::ceil for non-negative values
	static std::size_t floor_size(double value) noexcept {
		return value > 0.0 ? std::size_t(value) : 0u;
	}
	static std::size_t ceil_size(double value) noexcept {
		const std::size_t result = floor_size(value);
		return double(result) < value ? result + 1 : result;
	}
};

// Verify the layouts against libstdc++ (verified with GCC 12 and 14)
template <int W>
struct GuestStdUnorderedMapLayout {
	using map_type = GuestStdUnorderedMap<W, int, int>;
	static constexpr std::size_t WORD = sizeof(riscv::address_type<W>);
	static_assert(sizeof(map_type) == 7 * WORD, "The guest std::unordered_map is 7 words");
	static_assert(offsetof(map_type, buckets) == 0 * WORD, "_M_buckets");
	static_assert(offsetof(map_type, bucket_cnt) == 1 * WORD, "_M_bucket_count");
	static_assert(offsetof(map_type, before_begin) == 2 * WORD, "_M_before_begin");
	static_assert(offsetof(map_type, element_cnt) == 3 * WORD, "_M_element_count");
	static_assert(offsetof(map_type, max_load) == 4 * WORD, "_M_max_load_factor");
	static_assert(offsetof(map_type, next_resize) == 5 * WORD, "_M_next_resize");
	static_assert(offsetof(map_type, single_bucket) == 6 * WORD, "_M_single_bucket");
};
template struct GuestStdUnorderedMapLayout<4>;
template struct GuestStdUnorderedMapLayout<8>;

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

// The node sizes, as measured with RISC-V GCC 12 and 14
static_assert(sizeof(GuestHashNode<4, int, int, false>) == 12, "_Hash_node<pair<int, int>>");
static_assert(sizeof(GuestHashNode<8, int, int, false>) == 16, "_Hash_node<pair<int, int>>");
static_assert(sizeof(GuestHashNode<4, GuestStdString<4>, int, true>) == 36, "_Hash_node<pair<string, int>>");
static_assert(sizeof(GuestHashNode<8, GuestStdString<8>, int, true>) == 56, "_Hash_node<pair<string, int>>");
// A string-keyed map with a custom hash function has no cached hash code
static_assert(sizeof(GuestHashNode<4, GuestStdString<4>, int, false>) == 32, "_Hash_node<pair<string, int>>");
static_assert(sizeof(GuestHashNode<8, GuestStdString<8>, int, false>) == 48, "_Hash_node<pair<string, int>>");
static_assert(sizeof(GuestHashNode<4, GuestStdString<4>, GuestStdString<4>, true>) == 56, "_Hash_node<pair<string, string>>");
static_assert(sizeof(GuestHashNode<8, GuestStdString<8>, GuestStdString<8>, true>) == 80, "_Hash_node<pair<string, string>>");

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
		// Adjust the SSO pointer if the object is a std::string
		if constexpr (std::is_same_v<T, GuestStdString<W>>) {
			new (m_ptr) T(machine, std::forward<Args>(args)...);
			this->m_ptr->move(this->m_addr);
		} else if constexpr (is_guest_stdvector<W, T>::value) {
			new (m_ptr) T(machine, std::forward<Args>(args)...);
		} else if constexpr (is_guest_stdunordered_map<W, T>::value
			|| is_guest_stdvariant<W, T>::value) {
			// The map and the variant need their own address in guest memory
			new (m_ptr) T(machine, this->m_addr, std::forward<Args>(args)...);
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

	// Special case for std::variant: assign any alternative value
	template <typename Arg, typename Tp = T>
	std::enable_if_t<is_guest_stdvariant<W, Tp>::value
		&& !std::is_same_v<std::decay_t<Arg>, Tp>, ScopedArenaObject&>
	operator=(const Arg& other) {
		this->allocate_if_null();
		this->m_ptr->set(*m_machine, this->m_addr, other);
		return *this;
	}

	// Special case for std::string (a variant uses the assignment below)
	template <typename Tp = T>
	std::enable_if_t<!is_guest_stdvariant<W, Tp>::value, ScopedArenaObject&>
	operator=(std::string_view other) {
		static_assert(std::is_same_v<T, GuestStdString<W>>, "ScopedArenaObject<T> must be a GuestStdString<W>");
		this->allocate_if_null();
		this->m_ptr->set_string(*m_machine, this->m_addr, other.data(), other.size());
		return *this;
	}

	// Special case for std::vector (a variant uses the assignment below)
	template <typename U, typename Tp = T>
	std::enable_if_t<!is_guest_stdvariant<W, Tp>::value, ScopedArenaObject&>
	operator=(const std::vector<U>& other) {
		static_assert(std::is_same_v<T, GuestStdVector<W, U>>, "ScopedArenaObject<T> must be a GuestStdVector<W, U>");
		this->allocate_if_null();
		this->m_ptr->assign(*m_machine, other);
		return *this;
	}

	// Special case for std::unordered_map (a variant uses the assignment below)
	template <typename HK, typename HV, typename Tp = T>
	std::enable_if_t<!is_guest_stdvariant<W, Tp>::value, ScopedArenaObject&>
	operator=(const std::unordered_map<HK, HV>& other) {
		static_assert(is_guest_stdunordered_map<W, T>::value,
			"ScopedArenaObject<T> must be a GuestStdUnorderedMap<W, K, V>");
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
		if constexpr (is_guest_stdtype<W, T>::value) {
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

template <int W, typename T>
struct is_scoped_guest_stdvector : std::false_type {};

template <int W, typename T>
struct is_scoped_guest_stdvector<W, ScopedArenaObject<W, GuestStdVector<W, T>>> : std::true_type {};

template <int W, typename T>
struct is_scoped_guest_stdunordered_map : std::false_type {};

template <int W, typename K, typename V, bool CacheHashCode>
struct is_scoped_guest_stdunordered_map<W, ScopedArenaObject<W, GuestStdUnorderedMap<W, K, V, CacheHashCode>>> : std::true_type {};

template <int W, typename T>
struct is_scoped_guest_stdvariant : std::false_type {};

template <int W, typename... Types>
struct is_scoped_guest_stdvariant<W, ScopedArenaObject<W, GuestStdVariant<W, Types...>>> : std::true_type {};

template <int W, typename T>
using ScopedGuestStdVector = ScopedArenaObject<W, GuestStdVector<W, T>>;
template <int W>
using ScopedGuestStdString = ScopedArenaObject<W, GuestStdString<W>>;
template <int W, typename K, typename V,
	bool CacheHashCode = GuestStdHash<W, K>::cache_hash_code>
using ScopedGuestStdUnorderedMap = ScopedArenaObject<W, GuestStdUnorderedMap<W, K, V, CacheHashCode>>;
template <int W, typename... Types>
using ScopedGuestStdVariant = ScopedArenaObject<W, GuestStdVariant<W, Types...>>;

} // namespace riscv
