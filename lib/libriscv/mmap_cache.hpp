#include <array>
#include <cstdint>
#include <vector>
#include "types.hpp"

namespace riscv
{
	template <int W>
	struct MMapCache
	{
		using address_t = address_type<W>;

		struct Range {
			address_t addr = 0x0;
			address_t size = 0u;

			constexpr bool empty() const noexcept { return size == 0u; }
			// Invalidate if one of the ranges is in the other (both ways!)
			constexpr bool within(address_t mem, address_t memsize) const noexcept {
				return ((this->addr >= mem) && (this->addr + this->size <= mem + memsize))
					|| ((mem >= this->addr) && (mem + memsize <= this->addr + this->size));
			}
			constexpr bool equals(address_t mem, address_t memsize) const noexcept {
				return (this->addr == mem) && (this->addr + this->size == mem + memsize);
			}
		};

		Range find(address_t size)
		{
			auto it = m_lines.begin();
			while (it != m_lines.end())
			{
				auto& r = *it;
				if (!r.empty())
				{
					if (r.size >= size) {
						const Range result { r.addr, size };
						if (r.size > size) {
							r.addr += size;
							r.size -= size;
						} else {
							m_lines.erase(it);
						}
						return result;
					}
				}
				++it;
			}
			return Range{};
		}

		// Reserve an exact range out of the free list, keeping whatever is
		// left over on either side. Returns false when the range is not
		// entirely free, in which case nothing is modified.
		bool carve(address_t addr, address_t size)
		{
			if (addr + size < addr)
				return false;
			for (auto it = m_lines.begin(); it != m_lines.end(); ++it)
			{
				auto& r = *it;
				if (r.empty() || addr < r.addr || addr + size > r.addr + r.size)
					continue;
				const address_t tail_addr = addr + size;
				const address_t tail_size = (r.addr + r.size) - tail_addr;
				if (addr == r.addr) {
					// Carved from the front
					if (tail_size == 0) m_lines.erase(it);
					else { r.addr = tail_addr; r.size = tail_size; }
				} else {
					// Carved from the back or the middle
					r.size = addr - r.addr;
					if (tail_size != 0)
						m_lines.insert(it + 1, Range{tail_addr, tail_size});
				}
				return true;
			}
			return false;
		}

		void invalidate(address_t addr, address_t size)
		{
			auto it = m_lines.begin();
			while (it != m_lines.end())
			{
				const auto r = *it;
				if (r.within(addr, size))
				{
					bool equals = r.equals(addr, size);
					it = m_lines.erase(it);
					if (equals) return;
				}
				else ++it;
			}
		}

		void insert(address_t addr, address_t size)
		{
			/* Extend the back range? */
			if (!m_lines.empty()) {
				if (m_lines.back().addr + m_lines.back().size == addr) {
					m_lines.back().size += size;
					return;
				}
			}

			m_lines.push_back({addr, size});
		}

	private:
		std::vector<Range> m_lines {};
	};

} // riscv
