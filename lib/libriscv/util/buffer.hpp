#pragma once
#include <cstddef>
#include <string>

/**
 * Container that is designed to hold pointers to guest data, which can
 * be sequentialized in various ways.
**/

namespace riscv
{
	struct Buffer
	{
		auto    first() const { return m_data[0]; }
		auto*   c_str() const noexcept { return first().first; }
		size_t  size() const noexcept { return m_len; }
		uint32_t used() const noexcept { return m_idx; }

		size_t copy_to(char* dst, size_t dstlen);
		void   foreach(std::function<void(const char*, size_t)> cb);
		std::string to_string() const;

		Buffer() = default;
		void append_page(const char* data, size_t len);

	private:
		std::array<std::pair<const char*, size_t>, 4> m_data = {};
		uint32_t m_len  = 0; /* Total length */
		uint32_t m_idx  = 0; /* Current array index */
	};

	inline size_t Buffer::copy_to(char* dst, size_t maxlen)
	{
		size_t len = 0;
		for (const auto& entry : m_data) {
			if (entry.second == 0) break;
			if (UNLIKELY(len + entry.second > maxlen)) break;
			std::copy(entry.first, entry.first + entry.second, &dst[len]);
			len += entry.second;
		}
		return len;
	}

	inline void Buffer::foreach(std::function<void(const char*, size_t)> cb)
	{
		for (const auto& entry : m_data) {
			if (entry.second == 0) break;
			cb(entry.first, entry.second);
		}
	}

	inline void Buffer::append_page(const char* buffer, size_t len)
	{
		assert(len <= Page::size());
		assert(m_idx < m_data.size());
		m_len += len;
		m_data.at(m_idx ++) = {buffer, len};
	}

	inline std::string Buffer::to_string() const
	{
		std::string result;
		result.reserve(this->m_len);
		for (const auto& entry : m_data) {
			if (entry.second == 0) break;
			result.append(entry.first, entry.first + entry.second);
		}
		return result;
	}

}
