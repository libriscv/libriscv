#pragma once
#include <cstddef>
#include <string>

/**
 * Container that is designed to hold a guest zero-terminated string.
 * It will throw a protection fault if the string is not terminated.
**/

namespace riscv
{
	struct String
	{
		auto* c_str() const noexcept { return m_ptr; }
		size_t size() const noexcept { return m_len; }

		std::string to_string() const;

		String() = default;
		String(const char* p, size_t s, bool h = false)
			: m_ptr(p), m_len(s), m_heap(h) {}
		String(String&& other);
		String& operator= (String&& other);
		~String();

	private:
		const char* m_ptr = nullptr;
		size_t m_len  = 0;
		bool   m_heap = false;
	};

	inline String::~String()
	{
		if (m_heap) delete m_ptr;
	}
	inline String::String(String&& other)
		: m_ptr(other.m_ptr), m_len(other.m_len), m_heap(other.m_heap)
	{
		other.m_ptr  = nullptr;
		other.m_len  = 0;
		other.m_heap = false;
	}
	inline String& String::operator= (String&& other)
	{
		this->m_ptr  = other.m_ptr;
		this->m_len  = other.m_len;
		this->m_heap = other.m_heap;
		other.m_ptr  = nullptr;
		other.m_len  = 0;
		other.m_heap = false;
		return *this;
	}

	inline std::string String::to_string() const
	{
		return std::string(m_ptr, m_ptr + m_len);
	}

}
