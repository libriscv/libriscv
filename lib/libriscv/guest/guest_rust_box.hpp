#pragma once
#include "guest_arena_object.hpp"
#include "guest_rust_vec.hpp"
#include <algorithm>
#include <vector>

// Rust's owning pointers: Box<T>, Box<[T]> and Box<str>.
//
// Box is the one Rust smart pointer whose layout the language guarantees: for
// a sized T it is a single non-null pointer, ABI-compatible with *mut T and
// valid through extern "C". The two unsized forms are the same fat pointers as
// a slice and a &str, ie. { ptr, len }, with ownership.
//
// Box::from_raw() on a host allocation is sound, not merely convenient: the
// arena is the guest's global allocator, so the block really did come from the
// allocator that will release it. The arena is 16-byte aligned and does not
// need the size back, so the Layout that Rust passes to dealloc() is ignored.
//
// NOTE: Rust holds a NonNull inside a Box, so a null box is not a Box<T> at
// all - it is Option<Box<T>>::None, the one Option whose null representation
// the language documents. A default-constructed or freed box is exactly that,
// and must not be handed to the guest as a Box<T>.

namespace riscv {

/// @brief View into Rust's alloc::boxed::Box<T> for a sized T: one pointer to
/// a single T in the guest heap.
///
/// T may be incomplete, which is what lets a tree type refer to itself:
/// declare the value type, alias the box, then define the value type. Only the
/// operations that touch the pointee need T to be complete.
template <int W, typename T>
struct alignas(guest_word_align<W>) GuestRustBox {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	using element_type = T;

	gaddr_t ptr;

	/// @brief An empty box, ie. Option<Box<T>>::None.
	constexpr GuestRustBox() noexcept : ptr(0) {}
	constexpr explicit GuestRustBox(gaddr_t ptr) noexcept : ptr(ptr) {}

	/// @brief Box::new() of a value-initialized T
	explicit GuestRustBox(machine_t& machine)
		: ptr(0)
	{
		this->emplace(machine);
	}
	/// @brief Box::new() of a T built from a host value
	template <typename Arg>
	GuestRustBox(machine_t& machine, const Arg& value)
		: ptr(0)
	{
		this->emplace(machine, value);
	}
	/// @brief Overload ignoring the box's own guest address, which it does not need
	template <typename Arg>
	GuestRustBox(machine_t& machine, gaddr_t /*self*/, const Arg& value)
		: GuestRustBox(machine, value) {}

	// Copying is shallow, like the other guest containers: the copy shares the
	// allocation, and only one of them may free it.
	GuestRustBox(const GuestRustBox& other) = default;
	GuestRustBox& operator=(const GuestRustBox& other) = default;

	GuestRustBox(GuestRustBox&& other) noexcept : ptr(other.ptr) {
		other.ptr = 0;
	}

	/// @brief True when the box holds nothing (Option<Box<T>>::None)
	bool empty() const noexcept { return this->ptr == 0; }
	explicit operator bool() const noexcept { return this->ptr != 0; }
	gaddr_t address() const noexcept { return this->ptr; }

	/// @brief The boxed value in guest memory. Throws when the box is empty.
	T& get(const machine_t& machine) const {
		if (this->ptr == 0)
			throw std::runtime_error("Guest Rust Box is empty (None)");
		return *machine.memory.template memarray<T>(this->ptr, 1);
	}
	/// @brief The boxed value, or null when the box is empty.
	T* get_if(const machine_t& machine) const {
		if (this->ptr == 0)
			return nullptr;
		return machine.memory.template memarray<T>(this->ptr, 1);
	}

	/// @brief Replace the contents with a newly constructed T (Box::new).
	/// @return A reference to the new value, in guest memory.
	template <typename... Args>
	T& emplace(machine_t& machine, Args&&... args)
	{
		this->free(machine);
		const gaddr_t addr = machine.arena().malloc(sizeof(T));
		if (addr == 0)
			throw std::bad_alloc();
		T* dst = machine.memory.template memarray<T>(addr, 1);

		if constexpr (guest_object_needs_self_address<W, T>::value) {
			// Built at its final address right away, as it points into itself
			new (dst) T(machine, addr, std::forward<Args>(args)...);
		} else if constexpr (is_guest_datatype<W, T>::value) {
			new (dst) T(machine, std::forward<Args>(args)...);
			relocate_guest_object<W>(machine, *dst, addr);
		} else {
			new (dst) T(std::forward<Args>(args)...);
		}
		this->ptr = addr;
		return *dst;
	}

	/// @brief Take ownership of a T that already lives in the guest heap
	/// (Box::from_raw). The address must have come from the arena.
	void assume_ownership(machine_t& machine, gaddr_t addr)
	{
		this->free(machine);
		this->ptr = addr;
	}

	/// @brief Give up ownership without freeing anything (Box::into_raw). The
	/// caller owns the allocation from here on.
	gaddr_t release() noexcept {
		const gaddr_t addr = this->ptr;
		this->ptr = 0;
		return addr;
	}

	/// @brief Copy the boxed value to the host, when it is a value or a
	/// container that knows how to become one (see guest_host_type).
	guest_host_type_t<W, T> to_host(const machine_t& machine) const {
		return guest_object_to_host<W>(machine, this->get(machine));
	}

	/// @brief Drop the boxed value and release its allocation, the way the
	/// guest's drop glue would: recursively, so a whole tree goes with it.
	void free(machine_t& machine)
	{
		if (this->ptr != 0) {
			free_guest_object<W>(machine, this->get(machine));
			machine.arena().free(this->ptr);
			this->ptr = 0;
		}
	}
};

/// @brief View into Rust's Box<[T]>: an owned { ptr, len }, ie. a Vec<T> that
/// gave up its capacity. One allocation of exactly len elements.
template <int W, typename T>
struct alignas(guest_word_align<W>) GuestRustBoxedSlice {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	using value_type = T;

	/// @brief The pointer of an empty boxed slice: Rust holds a NonNull<T>, and
	/// an empty slice never allocates.
	static constexpr gaddr_t DANGLING = GuestRustDangling<T>::value;

	gaddr_t ptr;
	gaddr_t len;

	constexpr GuestRustBoxedSlice() noexcept
		: ptr(DANGLING), len(0) {}

	/// @brief Allocate a boxed slice of value-initialized elements
	GuestRustBoxedSlice(machine_t& machine, std::size_t elements)
		: ptr(DANGLING), len(0)
	{
		this->allocate(machine, elements);
		T* array = machine.memory.template memarray<T>(this->ptr, elements);
		for (std::size_t i = 0; i < elements; i++)
			new (&array[i]) T();
	}
	/// @brief Allocate a boxed slice holding a copy of a host vector
	GuestRustBoxedSlice(machine_t& machine, const std::vector<T>& vec)
		: ptr(DANGLING), len(0)
	{
		this->assign(machine, vec.data(), vec.size());
	}
	template <typename Arg>
	GuestRustBoxedSlice(machine_t& machine, gaddr_t /*self*/, const Arg& arg)
		: GuestRustBoxedSlice(machine, arg) {}

	GuestRustBoxedSlice(const GuestRustBoxedSlice& other) = default;
	GuestRustBoxedSlice& operator=(const GuestRustBoxedSlice& other) = default;

	GuestRustBoxedSlice(GuestRustBoxedSlice&& other) noexcept
		: ptr(other.ptr), len(other.len)
	{
		other.ptr = DANGLING;
		other.len = 0;
	}

	bool empty() const noexcept { return this->len == 0; }
	std::size_t size() const noexcept { return this->len; }
	gaddr_t data() const noexcept { return this->ptr; }
	std::size_t size_bytes() const noexcept { return this->len * sizeof(T); }

	/// @brief Borrow the whole thing as a Rust slice
	GuestRustSlice<W, T> as_slice() const noexcept {
		return GuestRustSlice<W, T>(this->ptr, this->len);
	}

	T* as_array(const machine_t& machine, std::size_t max_bytes = 16UL << 20) const {
		if (this->size_bytes() > max_bytes)
			throw std::runtime_error("Guest Rust Box<[T]> has size > max_bytes");
		return machine.memory.template memarray<T>(this->ptr, this->len);
	}

	T& at(const machine_t& machine, std::size_t index) const {
		if (index >= this->len)
			throw std::out_of_range("Guest Rust Box<[T]> index out of range");
		return this->as_array(machine)[index];
	}

	/// @brief Replace the contents, reallocating to exactly count elements
	void assign(machine_t& machine, const T* values, std::size_t count)
	{
		this->free(machine);
		if (count == 0)
			return;
		this->allocate(machine, count);
		std::copy(values, values + count, machine.memory.template memarray<T>(this->ptr, count));
	}
	void assign(machine_t& machine, const std::vector<T>& vec) {
		this->assign(machine, vec.data(), vec.size());
	}

	/// @brief Take ownership of an array that already lives in the guest heap
	void assume_ownership(machine_t& machine, gaddr_t array, std::size_t count)
	{
		this->free(machine);
		this->ptr = array;
		this->len = count;
	}

	std::vector<T> to_vector(const machine_t& machine) const {
		const T* array = this->as_array(machine);
		return std::vector<T>(&array[0], &array[this->len]);
	}

	/// @brief Drop every element and release the allocation.
	///
	/// The pointer, not the length, says whether there is anything to release:
	/// a boxed slice has no capacity word, and a zero-length allocation is
	/// reachable through assume_ownership() and the element-count constructor.
	void free(machine_t& machine)
	{
		if (this->ptr != DANGLING) {
			if (this->len != 0) {
				T* array = machine.memory.template memarray<T>(this->ptr, this->len);
				for (std::size_t i = 0; i < this->len; i++)
					free_guest_object<W>(machine, array[i]);
			}
			machine.arena().free(this->ptr);
		}
		this->ptr = DANGLING;
		this->len = 0;
	}

private:
	void allocate(machine_t& machine, std::size_t count)
	{
		const gaddr_t addr = machine.arena().malloc(count * sizeof(T));
		if (addr == 0)
			throw std::bad_alloc();
		this->ptr = addr;
		this->len = count;
	}
};

/// @brief View into Rust's Box<str>: an owned { ptr, len } of UTF-8 bytes. A
/// String without the capacity word, for a string that never grows again.
template <int W>
struct alignas(guest_word_align<W>) GuestRustBoxedStr {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	static constexpr gaddr_t DANGLING = GuestRustDangling<char>::value;

	gaddr_t ptr;
	gaddr_t len;

	constexpr GuestRustBoxedStr() noexcept
		: ptr(DANGLING), len(0) {}

	GuestRustBoxedStr(machine_t& machine, std::string_view str = "")
		: ptr(DANGLING), len(0)
	{
		this->set_string(machine, str);
	}
	GuestRustBoxedStr(machine_t& machine, gaddr_t /*self*/, std::string_view str = "")
		: GuestRustBoxedStr(machine, str) {}

	GuestRustBoxedStr(const GuestRustBoxedStr& other) = default;
	GuestRustBoxedStr& operator=(const GuestRustBoxedStr& other) = default;

	GuestRustBoxedStr(GuestRustBoxedStr&& other) noexcept
		: ptr(other.ptr), len(other.len)
	{
		other.ptr = DANGLING;
		other.len = 0;
	}

	bool empty() const noexcept { return this->len == 0; }
	std::size_t size() const noexcept { return this->len; }
	gaddr_t data() const noexcept { return this->ptr; }

	/// @brief Borrow as a Rust &str
	GuestRustStr<W> as_str() const noexcept {
		return GuestRustStr<W>(this->ptr, this->len);
	}

	std::string_view to_view(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->len > max_len)
			throw std::runtime_error("Guest Rust Box<str> too large (len > max_len)");
		if (this->len == 0)
			return std::string_view();
		return machine.memory.memview(this->ptr, this->len);
	}
	std::string to_string(const machine_t& machine, std::size_t max_len = 16UL << 20) const {
		const auto view = this->to_view(machine, max_len);
		return std::string(view.data(), view.size());
	}

	/// @brief Replace the contents, reallocating to exactly the given size: a
	/// boxed str has no spare capacity to reuse.
	void set_string(machine_t& machine, const void* str, std::size_t size)
	{
		this->free(machine);
		if (size == 0)
			return;
		const gaddr_t addr = machine.arena().malloc(size);
		if (addr == 0)
			throw std::bad_alloc();
		std::memcpy(machine.memory.template memarray<char>(addr, size), str, size);
		this->ptr = addr;
		this->len = size;
	}
	void set_string(machine_t& machine, std::string_view str) {
		this->set_string(machine, str.data(), str.size());
	}
	void set_string(machine_t& machine, gaddr_t /*self*/, const void* str, std::size_t size) {
		this->set_string(machine, str, size);
	}
	void set_string(machine_t& machine, gaddr_t /*self*/, std::string_view str) {
		this->set_string(machine, str.data(), str.size());
	}

	void free(machine_t& machine)
	{
		if (this->len != 0)
			machine.arena().free(this->ptr);
		this->ptr = DANGLING;
		this->len = 0;
	}
};

// A Box<T> is one machine word, and the two unsized forms are two
static_assert(sizeof(GuestRustBox<4, int>) == 4, "Rust Box<T> is 1 word");
static_assert(sizeof(GuestRustBox<8, int>) == 8, "Rust Box<T> is 1 word");
static_assert(sizeof(GuestRustBoxedSlice<4, int>) == 8, "Rust Box<[T]> is 2 words");
static_assert(sizeof(GuestRustBoxedSlice<8, int>) == 16, "Rust Box<[T]> is 2 words");
static_assert(sizeof(GuestRustBoxedStr<4>) == 8, "Rust Box<str> is 2 words");
static_assert(sizeof(GuestRustBoxedStr<8>) == 16, "Rust Box<str> is 2 words");
static_assert(std::is_standard_layout_v<GuestRustBox<8, int>>, "Standard layout");
static_assert(std::is_standard_layout_v<GuestRustBoxedSlice<8, int>>, "Standard layout");
static_assert(std::is_standard_layout_v<GuestRustBoxedStr<8>>, "Standard layout");
static_assert(alignof(GuestRustBox<8, int>) == 8, "Aligned like a guest word");
static_assert(alignof(GuestRustBoxedSlice<8, int>) == 8, "Aligned like a guest word");
static_assert(alignof(GuestRustBoxedStr<8>) == 8, "Aligned like a guest word");

/// @brief A guest Rust Box that lives in the arena, and which frees itself
/// (and the value it points at) at the end of the scope.
template <int W, typename T>
using ScopedGuestRustBox = ScopedArenaObject<W, GuestRustBox<W, T>>;
template <int W, typename T>
using ScopedGuestRustBoxedSlice = ScopedArenaObject<W, GuestRustBoxedSlice<W, T>>;
template <int W>
using ScopedGuestRustBoxedStr = ScopedArenaObject<W, GuestRustBoxedStr<W>>;

} // riscv
