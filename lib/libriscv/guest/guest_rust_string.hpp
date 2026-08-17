#pragma once
#include "guest_arena_object.hpp"
#include "guest_rust_common.hpp"
#include <algorithm>
#include <string>
#include <string_view>

namespace riscv {

/// @brief A borrowed Rust &str: a pointer and a length, without ownership.
///
/// NOTE: This is the layout of a &str *stored in memory*, eg. as a field of
/// a #[repr(C)] struct. As a function argument the RISC-V ABI hands Rust the
/// two halves in separate registers instead, so a &str argument is passed as
/// two integer arguments: the address and the length.
template <int W>
struct alignas(guest_word_align<W>) GuestRustStr {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	gaddr_t ptr;
	gaddr_t len;

	constexpr GuestRustStr() noexcept
		: ptr(GuestRustDangling<char>::value), len(0) {}
	constexpr GuestRustStr(gaddr_t ptr, gaddr_t len) noexcept
		: ptr(ptr), len(len) {}

	bool empty() const noexcept { return this->len == 0; }
	std::size_t size() const noexcept { return this->len; }
	gaddr_t address() const noexcept { return this->ptr; }

	std::string_view to_view(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->len > max_len)
			throw std::runtime_error("Guest Rust &str too large (len > max_len)");
		if (this->len == 0)
			return std::string_view();
		return machine.memory.memview(this->ptr, this->len);
	}
	std::string to_string(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		const auto view = this->to_view(machine, max_len);
		return std::string(view.data(), view.size());
	}
};

/// @brief View into Rust's alloc::string::String, which is a Vec<u8> that
/// holds UTF-8 bytes: { capacity, ptr, len }. See guest_rust_common.hpp for
/// why the capacity comes first.
///
/// A Rust string is not zero-terminated, has no small-string optimization,
/// and never points back into itself, so it can be moved around freely.
///
/// NOTE: Rust code is allowed to assume that the bytes are valid UTF-8, and
/// it is undefined behavior in the guest when they are not. Everything that
/// writes into a string here passes the bytes through unchanged: use
/// is_valid_utf8() when the source is not already known to be text.
template <int W>
struct alignas(guest_word_align<W>) GuestRustString {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	/// @brief The pointer of a string that has not allocated anything.
	/// Rust holds a NonNull<u8>, so it must never be zero.
	static constexpr gaddr_t DANGLING = GuestRustDangling<char>::value;
	/// @brief The smallest allocation that Rust's RawVec makes for bytes
	static constexpr std::size_t MIN_CAPACITY = GuestRustMinCapacity<char>::value;

	gaddr_t cap;
	gaddr_t ptr;
	gaddr_t len;

	constexpr GuestRustString() noexcept
		: cap(0), ptr(DANGLING), len(0) {}

	GuestRustString(machine_t& machine, std::string_view str = "")
		: cap(0), ptr(DANGLING), len(0)
	{
		this->set_string(machine, str);
	}
	/// @brief Overload that ignores the guest address of the string itself,
	/// which a Rust string does not need. It exists so that a Rust string can
	/// be used wherever a C++ one can, eg. as an element of a container.
	GuestRustString(machine_t& machine, gaddr_t /*self*/, std::string_view str = "")
		: GuestRustString(machine, str) {}

	// Copying is intentionally shallow/fast, like the C++ containers: the
	// copy shares the guest allocation with the original, and only one of
	// them may free it.
	GuestRustString(const GuestRustString& other) = default;
	GuestRustString& operator=(const GuestRustString& other) = default;

	GuestRustString(GuestRustString&& other) noexcept
		: cap(other.cap), ptr(other.ptr), len(other.len)
	{
		other.cap = 0;
		other.ptr = DANGLING;
		other.len = 0;
	}

	bool empty() const noexcept { return this->len == 0; }
	std::size_t size() const noexcept { return this->len; }
	std::size_t length() const noexcept { return this->len; }
	std::size_t capacity() const noexcept { return this->cap; }
	/// @brief The guest address of the bytes, which is only meaningful
	/// when the string has a capacity.
	gaddr_t data() const noexcept { return this->ptr; }

	/// @brief Borrow the string as a Rust &str
	GuestRustStr<W> as_str() const noexcept {
		return GuestRustStr<W>(this->ptr, this->len);
	}

	/// @brief View the bytes of the string in guest memory.
	std::string_view to_view(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->len > max_len)
			throw std::runtime_error("Guest Rust String too large (len > max_len)");
		if (this->len == 0)
			return std::string_view();
		if (this->len > this->cap)
			throw std::runtime_error("Guest Rust String has len > capacity");
		return machine.memory.memview(this->ptr, this->len);
	}

	std::string to_string(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		const auto view = this->to_view(machine, max_len);
		return std::string(view.data(), view.size());
	}

	/// @brief Replace the contents of the string, reusing the allocation
	/// when it is already big enough.
	void set_string(machine_t& machine, const void* str, std::size_t len)
	{
		this->reserve(machine, len);
		if (len != 0)
			std::memcpy(machine.memory.template memarray<char>(this->ptr, len), str, len);
		this->len = len;
	}
	void set_string(machine_t& machine, std::string_view str) {
		this->set_string(machine, str.data(), str.size());
	}
	/// @brief Overload that ignores the guest address of the string itself,
	/// matching GuestStdString::set_string().
	void set_string(machine_t& machine, gaddr_t /*self*/, const void* str, std::size_t len) {
		this->set_string(machine, str, len);
	}
	void set_string(machine_t& machine, gaddr_t /*self*/, std::string_view str) {
		this->set_string(machine, str.data(), str.size());
	}

	/// @brief Append bytes to the end of the string (Rust's push_str).
	void append(machine_t& machine, const void* str, std::size_t len)
	{
		if (len == 0)
			return;
		this->reserve(machine, std::size_t(this->len) + len);
		std::memcpy(machine.memory.template memarray<char>(this->ptr + this->len, len), str, len);
		this->len += len;
	}
	void append(machine_t& machine, std::string_view str) {
		this->append(machine, str.data(), str.size());
	}

	/// @brief Drop the characters, keeping the allocation (Rust's clear).
	void clear(machine_t&) noexcept {
		this->len = 0;
	}

	/// @brief Make room for at least the given number of bytes in total.
	/// Grows the same way Rust's RawVec does, so that a string built here
	/// and one built by the guest end up with the same capacity.
	void reserve(machine_t& machine, std::size_t capacity)
	{
		if (capacity <= this->cap)
			return;
		const std::size_t new_cap = std::max(std::max(capacity,
			std::size_t(this->cap) * 2), MIN_CAPACITY);

		const gaddr_t new_ptr = machine.arena().malloc(new_cap);
		if (new_ptr == 0)
			throw std::bad_alloc();
		if (this->len != 0) {
			machine.memory.memcpy(new_ptr, machine, this->ptr, this->len);
		}
		if (this->cap != 0)
			machine.arena().free(this->ptr);
		this->ptr = new_ptr;
		this->cap = new_cap;
	}

	/// @brief Release the guest allocation. A freed string is left in the
	/// state of a newly created Rust String::new().
	void free(machine_t& machine)
	{
		if (this->cap != 0)
			machine.arena().free(this->ptr);
		this->ptr = DANGLING;
		this->cap = 0;
		this->len = 0;
	}

	/// @brief True when the bytes form a valid UTF-8 sequence, which is what
	/// Rust requires of every str. Mirrors core::str::from_utf8().
	static bool is_valid_utf8(std::string_view bytes) noexcept
	{
		const unsigned char* p = (const unsigned char *)bytes.data();
		const unsigned char* const end = p + bytes.size();
		while (p < end) {
			const unsigned c = *p;
			std::size_t extra;
			unsigned lower = 0x80, upper = 0xBF; // the usual continuation range
			if (c < 0x80) { p += 1; continue; }
			else if (c >= 0xC2 && c <= 0xDF) extra = 1;
			else if (c == 0xE0) { extra = 2; lower = 0xA0; }
			else if (c >= 0xE1 && c <= 0xEC) extra = 2;
			else if (c == 0xED) { extra = 2; upper = 0x9F; } // no surrogates
			else if (c >= 0xEE && c <= 0xEF) extra = 2;
			else if (c == 0xF0) { extra = 3; lower = 0x90; }
			else if (c >= 0xF1 && c <= 0xF3) extra = 3;
			else if (c == 0xF4) { extra = 3; upper = 0x8F; } // max U+10FFFF
			else return false;

			if (std::size_t(end - p) <= extra)
				return false;
			// The first continuation byte can have a narrower range, which is
			// what rejects the overlong encodings and the surrogate halves
			if (p[1] < lower || p[1] > upper)
				return false;
			for (std::size_t i = 2; i <= extra; i++)
				if (p[i] < 0x80 || p[i] > 0xBF)
					return false;
			p += extra + 1;
		}
		return true;
	}
};

// A Rust String and a Vec are three machine words, and a &str is two
static_assert(sizeof(GuestRustString<4>) == 12, "Rust String is 3 words");
static_assert(sizeof(GuestRustString<8>) == 24, "Rust String is 3 words");
static_assert(sizeof(GuestRustStr<4>) == 8, "Rust &str is 2 words");
static_assert(sizeof(GuestRustStr<8>) == 16, "Rust &str is 2 words");
static_assert(std::is_standard_layout_v<GuestRustString<8>>, "Standard layout");
static_assert(std::is_standard_layout_v<GuestRustStr<8>>, "Standard layout");
// A 64-bit guest word is 8-aligned even on a host that would align it to 4
static_assert(alignof(GuestRustString<8>) == 8, "Aligned like a guest word");
static_assert(alignof(GuestRustStr<8>) == 8, "Aligned like a guest word");

/// @brief A guest Rust String that lives in the arena, and which frees
/// itself (and its bytes) at the end of the scope.
template <int W>
using ScopedGuestRustString = ScopedArenaObject<W, GuestRustString<W>>;

} // riscv
