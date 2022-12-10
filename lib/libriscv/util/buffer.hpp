#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/**
 * Container that is designed to hold pointers to guest data, which can
 * be sequentialized in various ways.
**/

namespace riscv
{
	struct Buffer
	{
		bool is_sequential() const noexcept { return m_data.size() == 1; }
		const auto& first() const { return m_data[0]; }
		const char* c_str() const noexcept { return first().first; }
		const char* data() const noexcept { return first().first; }
		size_t      size() const noexcept { return m_len; }

		auto strview() const noexcept { return std::string_view{c_str(), size()}; }

		size_t copy_to(char* dst, size_t dstlen) const;
		void   copy_to(std::vector<uint8_t>&) const;
		void   foreach(std::function<void(const char*, size_t)> cb);
		std::string to_string() const;

		Buffer() = default;
		void append_page(const char* data, size_t len);

	private:
		std::vector<std::pair<const char*, size_t>> m_data;
		size_t m_len  = 0; /* Total length */
	};

	inline size_t Buffer::copy_to(char* dst, size_t maxlen) const
	{
		size_t len = 0;
		for (const auto& entry : m_data) {
			if (UNLIKELY(len + entry.second > maxlen)) break;
			std::copy(entry.first, entry.first + entry.second, &dst[len]);
			len += entry.second;
		}
		return len;
	}
	inline void Buffer::copy_to(std::vector<uint8_t>& vec) const
	{
		for (const auto& entry : m_data) {
			vec.insert(vec.end(), entry.first, entry.first + entry.second);
		}
	}

	inline void Buffer::foreach(std::function<void(const char*, size_t)> cb)
	{
		for (const auto& entry : m_data) {
			cb(entry.first, entry.second);
		}
	}

	inline void Buffer::append_page(const char* buffer, size_t len)
	{
		assert(len <= Page::size());
		// In some cases we can continue the last entry
		if (!m_data.empty()) {
			auto& last = m_data.back();
			if (last.first + last.second == buffer) {
				last.second += len;
				m_len += len;
				return;
			}
		}
		// Otherwise, append new entry
		m_len += len;
		m_data.emplace_back(buffer, len);
	}

	inline std::string Buffer::to_string() const
	{
		if (is_sequential()) {
			return std::string(c_str(), size());
		}
		std::string result;
		result.reserve(this->m_len);
		for (const auto& entry : m_data) {
			result.append(entry.first, entry.first + entry.second);
		}
		return result;
	}
}
