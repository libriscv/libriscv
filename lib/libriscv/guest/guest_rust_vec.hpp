#pragma once
#include "guest_arena_object.hpp"
#include "guest_rust_string.hpp"
#include <algorithm>
#include <vector>

namespace riscv {

/// @brief A borrowed Rust slice &[T]: a pointer and an element count.
///
/// NOTE: Like GuestRustStr this is the layout of a slice *stored in memory*.
/// As a function argument the two halves go in separate registers.
template <int W, typename T>
struct alignas(guest_word_align<W>) GuestRustSlice {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	gaddr_t ptr;
	gaddr_t len;

	constexpr GuestRustSlice() noexcept
		: ptr(GuestRustDangling<T>::value), len(0) {}
	constexpr GuestRustSlice(gaddr_t ptr, gaddr_t len) noexcept
		: ptr(ptr), len(len) {}

	bool empty() const noexcept { return this->len == 0; }
	std::size_t size() const noexcept { return this->len; }
	gaddr_t address() const noexcept { return this->ptr; }

	const T* as_array(const machine_t& machine, std::size_t max_bytes = 16UL << 20) const {
		if (this->len * sizeof(T) > max_bytes)
			throw std::runtime_error("Guest Rust slice is larger than max_bytes");
		return machine.memory.template memarray<T>(this->ptr, this->len);
	}
};

/// @brief View into Rust's alloc::vec::Vec<T>: { capacity, ptr, len }. See
/// guest_rust_common.hpp for why the capacity comes first.
///
/// A Rust vector never points back into itself, and its elements are moved
/// with a plain memcpy by the guest, so it can be relocated freely. The
/// elements themselves may be other guest containers, eg. a Vec<String>.
///
/// NOTE: An empty vector still has a non-null (dangling) pointer, because
/// Rust holds a NonNull<T>. Only a non-zero capacity means an allocation.
template <int W, typename T>
struct alignas(guest_word_align<W>) GuestRustVec {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	using value_type = T;

	/// @brief The pointer of a vector that has not allocated anything
	static constexpr gaddr_t DANGLING = GuestRustDangling<T>::value;
	/// @brief The smallest allocation that Rust's RawVec makes for T
	static constexpr std::size_t MIN_CAPACITY = GuestRustMinCapacity<T>::value;

	gaddr_t cap;
	gaddr_t ptr;
	gaddr_t len;

	constexpr GuestRustVec() noexcept
		: cap(0), ptr(DANGLING), len(0) {}

	/// @brief Create a vector of value-initialized elements
	GuestRustVec(machine_t& machine, std::size_t elements)
		: cap(0), ptr(DANGLING), len(0)
	{
		this->resize(machine, elements);
	}
	GuestRustVec(machine_t& machine, const std::vector<T>& vec = {})
		: cap(0), ptr(DANGLING), len(0)
	{
		if (!vec.empty())
			this->assign(machine, vec);
	}
	/// @brief Create a Vec<String> from a vector of host strings
	GuestRustVec(machine_t& machine, const std::vector<std::string>& vec)
		: cap(0), ptr(DANGLING), len(0)
	{
		static_assert(is_guest_ruststring<W, T>::value,
			"GuestRustVec<T> must be a vector of GuestRustString<W>");
		this->reserve(machine, vec.size());
		for (const auto& str : vec)
			this->push_back(machine, std::string_view(str));
	}
	/// @brief Overload that ignores the guest address of the vector itself,
	/// which a Rust vector does not need.
	template <typename Arg>
	GuestRustVec(machine_t& machine, gaddr_t /*self*/, const Arg& arg)
		: GuestRustVec(machine, arg) {}

	// Copying is intentionally shallow/fast, like the C++ containers
	GuestRustVec(const GuestRustVec& other) = default;
	GuestRustVec& operator=(const GuestRustVec& other) = default;

	GuestRustVec(GuestRustVec&& other) noexcept
		: cap(other.cap), ptr(other.ptr), len(other.len)
	{
		other.cap = 0;
		other.ptr = DANGLING;
		other.len = 0;
	}

	bool empty() const noexcept { return this->len == 0; }
	std::size_t size() const noexcept { return this->len; }
	std::size_t capacity() const noexcept { return this->cap; }
	gaddr_t data() const noexcept { return this->ptr; }

	std::size_t size_bytes() const noexcept { return this->len * sizeof(T); }
	std::size_t capacity_bytes() const noexcept { return this->cap * sizeof(T); }

	/// @brief Borrow the whole vector as a Rust slice
	GuestRustSlice<W, T> as_slice() const noexcept {
		return GuestRustSlice<W, T>(this->ptr, this->len);
	}

	gaddr_t address_at(std::size_t index) const {
		if (index >= this->len)
			throw std::out_of_range("Guest Rust Vec index out of range");
		return this->ptr + index * sizeof(T);
	}

	T& at(machine_t& machine, std::size_t index, std::size_t max_bytes = 16UL << 20) {
		if (index >= this->len)
			throw std::out_of_range("Guest Rust Vec index out of range");
		return this->as_array(machine, max_bytes)[index];
	}
	const T& at(const machine_t& machine, std::size_t index, std::size_t max_bytes = 16UL << 20) const {
		if (index >= this->len)
			throw std::out_of_range("Guest Rust Vec index out of range");
		return this->as_array(machine, max_bytes)[index];
	}

	T *as_array(const machine_t& machine, std::size_t max_bytes = 16UL << 20) {
		if (size_bytes() > max_bytes)
			throw std::runtime_error("Guest Rust Vec has size > max_bytes");
		return machine.memory.template memarray<T>(this->ptr, this->len);
	}
	const T *as_array(const machine_t& machine, std::size_t max_bytes = 16UL << 20) const {
		if (size_bytes() > max_bytes)
			throw std::runtime_error("Guest Rust Vec has size > max_bytes");
		return machine.memory.template memarray<T>(this->ptr, this->len);
	}

#if RISCV_SPAN_AVAILABLE
	std::span<T> to_span(const machine_t& machine) {
		return std::span<T>(as_array(machine), this->len);
	}
	std::span<const T> to_span(const machine_t& machine) const {
		return std::span<const T>(as_array(machine), this->len);
	}
#endif

	// Iterators
	auto begin(machine_t& machine) { return as_array(machine); }
	auto end(machine_t& machine) { return as_array(machine) + this->len; }

	void push_back(machine_t& machine, const T& value) {
		this->reserve(machine, std::size_t(this->len) + 1);
		T* array = machine.memory.template memarray<T>(this->ptr, this->len + 1);
		new (&array[this->len]) T(value);
		this->len += 1;
	}
	void push_back(machine_t& machine, T&& value) {
		this->reserve(machine, std::size_t(this->len) + 1);
		T* array = machine.memory.template memarray<T>(this->ptr, this->len + 1);
		new (&array[this->len]) T(std::move(value));
		this->len += 1;
	}
	/// @brief Specialization for a Vec<String>
	void push_back(machine_t& machine, std::string_view value) {
		static_assert(is_guest_ruststring<W, T>::value,
			"GuestRustVec: T must be a GuestRustString<W>");
		this->reserve(machine, std::size_t(this->len) + 1);
		T str(machine, value);
		T* array = machine.memory.template memarray<T>(this->ptr, this->len + 1);
		new (&array[this->len]) T(str);
		this->len += 1;
	}

	void pop_back(machine_t& machine) {
		if (this->len == 0)
			throw std::out_of_range("Guest Rust Vec is empty");
		this->free_element(machine, this->len - 1);
		this->len -= 1;
	}

	/// @brief Drop every element, keeping the allocation (Rust's clear).
	void clear(machine_t& machine) {
		for (std::size_t i = 0; i < this->len; i++)
			this->free_element(machine, i);
		this->len = 0;
	}

	void resize(machine_t& machine, std::size_t new_size)
	{
		if (new_size < this->len) {
			for (std::size_t i = new_size; i < this->len; i++)
				this->free_element(machine, i);
		} else if (new_size > this->len) {
			this->reserve(machine, new_size);
			T* array = machine.memory.template memarray<T>(this->ptr, new_size);
			for (std::size_t i = this->len; i < new_size; i++)
				new (&array[i]) T();
		}
		this->len = new_size;
	}

	/// @brief Make room for at least the given number of elements in total.
	/// Grows the same way Rust's RawVec does.
	void reserve(machine_t& machine, std::size_t elements)
	{
		if (elements <= this->cap)
			return;
		const std::size_t new_cap = std::max(std::max(elements,
			std::size_t(this->cap) * 2), MIN_CAPACITY);

		const gaddr_t new_ptr = machine.arena().malloc(new_cap * sizeof(T));
		if (new_ptr == 0)
			throw std::bad_alloc();
		if (this->len != 0)
			machine.memory.memcpy(new_ptr, machine, this->ptr, this->len * sizeof(T));
		if (this->cap != 0)
			machine.arena().free(this->ptr);
		this->ptr = new_ptr;
		this->cap = new_cap;
	}

	void assign(machine_t& machine, const T* values, std::size_t count)
	{
		this->clear(machine);
		this->reserve(machine, count);
		if (count != 0) {
			T* array = machine.memory.template memarray<T>(this->ptr, count);
			std::copy(values, values + count, array);
		}
		this->len = count;
	}
	void assign(machine_t& machine, const std::vector<T>& vec) {
		this->assign(machine, vec.data(), vec.size());
	}
	/// @brief Replace the contents with a vector of host strings
	void assign(machine_t& machine, const std::vector<std::string>& vec)
	{
		static_assert(is_guest_ruststring<W, T>::value,
			"GuestRustVec<T> must be a vector of GuestRustString<W>");
		this->free(machine);
		this->reserve(machine, vec.size());
		for (const auto& str : vec)
			this->push_back(machine, std::string_view(str));
	}

	/// @brief Replace the contents of the vector with the given array, by
	/// assuming ownership of the array memory (which must have been
	/// allocated in the guest arena and properly initialized).
	void assume_ownership(machine_t& machine, gaddr_t array, std::size_t count)
	{
		this->free(machine);
		this->ptr = array;
		this->cap = count;
		this->len = count;
	}

	std::vector<T> to_vector(const machine_t& machine) const {
		if (this->len > this->cap)
			throw std::runtime_error("Guest Rust Vec has len > capacity");
		const T* array = machine.memory.template memarray<T>(this->ptr, this->len);
		return std::vector<T>(&array[0], &array[this->len]);
	}

	/// @brief Copy a Vec<String> to the host
	std::vector<std::string> to_string_vector(const machine_t& machine) const {
		static_assert(is_guest_ruststring<W, T>::value,
			"GuestRustVec: T must be a GuestRustString<W>");
		std::vector<std::string> vec;
		const T* array = machine.memory.template memarray<T>(this->ptr, this->len);
		vec.reserve(this->len);
		for (std::size_t i = 0; i < this->len; i++)
			vec.push_back(array[i].to_string(machine));
		return vec;
	}

	/// @brief Drop every element and release the allocation. A freed vector
	/// is left in the state of a newly created Rust Vec::new().
	void free(machine_t& machine)
	{
		this->clear(machine);
		if (this->cap != 0)
			machine.arena().free(this->ptr);
		this->ptr = DANGLING;
		this->cap = 0;
	}

private:
	void free_element(machine_t& machine, std::size_t index) {
		free_guest_object<W>(machine, this->at(machine, index));
	}
};

// A Rust Vec is three machine words, and a slice is two
static_assert(sizeof(GuestRustVec<4, int>) == 12, "Rust Vec is 3 words");
static_assert(sizeof(GuestRustVec<8, int>) == 24, "Rust Vec is 3 words");
static_assert(sizeof(GuestRustSlice<4, int>) == 8, "Rust &[T] is 2 words");
static_assert(sizeof(GuestRustSlice<8, int>) == 16, "Rust &[T] is 2 words");
static_assert(std::is_standard_layout_v<GuestRustVec<8, int>>, "Standard layout");
static_assert(alignof(GuestRustVec<8, int>) == 8, "Aligned like a guest word");
static_assert(alignof(GuestRustSlice<8, int>) == 8, "Aligned like a guest word");

/// @brief A guest Rust Vec that lives in the arena, and which frees itself
/// (and its elements) at the end of the scope.
template <int W, typename T>
using ScopedGuestRustVec = ScopedArenaObject<W, GuestRustVec<W, T>>;

template <int W, typename T>
struct is_scoped_guest_rustvec : std::false_type {};

template <int W, typename T>
struct is_scoped_guest_rustvec<W, ScopedArenaObject<W, GuestRustVec<W, T>>> : std::true_type {};

} // riscv
