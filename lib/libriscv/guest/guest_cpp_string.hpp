#pragma once
#include "guest_arena_object.hpp"
#include "guest_cpp_common.hpp"
#include <string>
#include <string_view>

namespace riscv {

// View into libstdc++'s std::string
template <int W>
struct alignas(guest_word_align<W>) GuestStdString {
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

	/// @brief View the characters, wherever they live.
	/// @note ptr (_M_p) decides, not the size: libstdc++ points it at the inline
	/// buffer for a short string and at a heap block otherwise, and a string that
	/// was allocated big and then shrunk keeps its heap block. The inline union of
	/// such a string holds the capacity, not characters.
	/// A null ptr is a host-side object with no guest address yet (see set_string),
	/// which is the only case where the characters are inline in this struct.
	std::string_view to_view(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 16MB)");
		if (this->size == 0)
			return std::string_view();
		if (this->ptr == 0)
			return std::string_view(data, size);
		return machine.memory.memview(ptr, size);
	}

	std::string to_string(const machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		const auto view = this->to_view(machine, max_len);
		return std::string(view.data(), view.size());
	}

	void set_string(machine_t& machine, gaddr_t self, const void* str, std::size_t len, bool use_memarray = true)
	{
		this->free(machine);

		if (len <= SSO)
		{
			// Leave ptr null when there is no guest address yet, instead of pointing
			// it into the guest's null page, so that to_view() can tell an unplaced
			// host-side string apart. move() fills it in once there is an address.
			this->ptr = (self != 0) ? self + offsetof(GuestStdString, data) : 0;
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

	/// @note Only for strings built by set_string(), which never over-allocates.
	/// A guest string shrunk below SSO is still on the heap, and re-pointing it at
	/// the inline buffer would lose it.
	void move(gaddr_t self)
	{
		if (size <= SSO) {
			this->ptr = self + offsetof(GuestStdString, data);
		}
	}

	/// @note Same caveat as move(): frees only what set_string() allocated. A
	/// guest string shrunk below SSO owns a heap block this will not release.
	void free(machine_t& machine)
	{
		if (size > SSO) {
			machine.arena().free(ptr);
		}
		this->ptr = 0;
		this->size = 0;
	}
};

// A guest std::string is two machine words plus the 16-byte inline buffer,
// and it is aligned like a machine word
static_assert(sizeof(GuestStdString<4>) == 24, "std::string is 24 bytes on a 32-bit guest");
static_assert(sizeof(GuestStdString<8>) == 32, "std::string is 32 bytes on a 64-bit guest");
static_assert(alignof(GuestStdString<8>) == 8, "Aligned like a guest word");
static_assert(std::is_standard_layout_v<GuestStdString<8>>, "Standard layout");

/// @brief A guest std::string that lives in the arena, and which frees
/// itself (and its characters) at the end of the scope.
template <int W>
using ScopedGuestStdString = ScopedArenaObject<W, GuestStdString<W>>;

} // riscv
