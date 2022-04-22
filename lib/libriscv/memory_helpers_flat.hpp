#pragma once

template <int W>
inline void Memory<W>::fault_if_unreadable(address_t addr, size_t size) const
{
	if (UNLIKELY(addr < 0x1000 || addr + size >= m_memsize)) protection_fault(addr);
}

template <int W>
inline void Memory<W>::fault_if_readonly(address_t addr, size_t size) const
{
	if (UNLIKELY(addr + size >= m_memsize || addr < 0x1000)) protection_fault(addr);
	if (UNLIKELY(is_executable(addr, size))) protection_fault(addr);
}

template <int W> inline
void Memory<W>::memzero(address_t dst, size_t len)
{
	fault_if_readonly(dst, len);
	std::memset(&m_memdata[dst], 0, len);
}

template <int W> inline
void Memory<W>::memset(address_t dst, uint8_t value, size_t len)
{
	fault_if_readonly(dst, len);
	std::memset(&m_memdata[dst], value, len);
}

template <int W> inline
void Memory<W>::memcpy(address_t dst, const void* vsrc, size_t len)
{
	fault_if_readonly(dst, len);
	std::memcpy(&m_memdata[dst], vsrc, len);
}

template <int W> inline
void Memory<W>::memcpy_unsafe(address_t dst, const void* vsrc, size_t len)
{
	fault_if_unreadable(dst, len);
	std::memcpy(&m_memdata[dst], vsrc, len);
}

template <int W> inline
void Memory<W>::memcpy_out(void* vdst, address_t src, size_t len) const
{
	fault_if_unreadable(src, len);
	std::memcpy(vdst, &m_memdata[src], len);
}

template <int W>
template <typename T> inline
void Memory<W>::foreach_helper(T& mem, address_t addr, size_t len,
	std::function<void(T&, address_t, const uint8_t*, size_t)> callback)
{
	uint8_t* buffer = (uint8_t*) __builtin_alloca(len);
	mem.memcpy_out(buffer, addr, len);
	callback(mem, addr, buffer, len);
}
template <int W>
template <typename T> inline
void Memory<W>::memview_helper(T& mem, address_t addr, size_t len,
	std::function<void(T&, const uint8_t*, size_t)> callback)
{
	uint8_t* buffer = (uint8_t*) __builtin_alloca(len);
	mem.memcpy_out(buffer, addr, len);
	callback(mem, buffer, len);
}

template <int W> inline
void Memory<W>::foreach(address_t addr, size_t len,
	std::function<void(const Memory<W>&, address_t, const uint8_t*, size_t)> callback) const
{
	foreach_helper(*this, addr, len, std::move(callback));
}
template <int W> inline
void Memory<W>::foreach(address_t addr, size_t len,
	std::function<void(Memory<W>&, address_t, const uint8_t*, size_t)> callback)
{
	foreach_helper(*this, addr, len, std::move(callback));
}
template <int W> inline
void Memory<W>::memview(address_t addr, size_t len,
	std::function<void(const Memory<W>&, const uint8_t*, size_t)> callback) const
{
	memview_helper(*this, addr, len, std::move(callback));
}
template <int W> inline
void Memory<W>::memview(address_t addr, size_t len,
	std::function<void(Memory<W>&, const uint8_t*, size_t)> callback)
{
	memview_helper(*this, addr, len, std::move(callback));
}
template <int W>
template <typename T> inline
void Memory<W>::memview(address_t addr, std::function<void(const T&)> callback) const
{
	T object;
	memcpy_out(&object, addr, sizeof(object));
	callback(object);
}

template <int W> inline
std::string Memory<W>::memstring(address_t addr, const size_t max_len) const
{
	fault_if_unreadable(addr, max_len);
	return std::string((const char *)&m_memdata[addr]);
}

template <int W> inline
riscv::Buffer Memory<W>::rvbuffer(address_t addr,
	const size_t datalen, const size_t maxlen) const
{
	if (UNLIKELY(datalen + 1 >= maxlen))
		protection_fault(addr);
	fault_if_unreadable(addr, datalen);
	riscv::Buffer result;
	result.append_page((const char *)&m_memdata[addr], datalen);
	return result;
}

template <int W> inline
size_t Memory<W>::strlen(address_t addr, size_t maxlen) const
{
	fault_if_unreadable(addr, maxlen);
	return ::strnlen((const char *)&m_memdata[addr], maxlen);
}

template <int W> inline
int Memory<W>::memcmp(address_t p1, address_t p2, size_t len) const
{
	fault_if_unreadable(p1, len);
	fault_if_unreadable(p2, len);

	const auto* s1 = &m_memdata[p1];
	const auto* s2 = &m_memdata[p2];

	return std::memcmp(s1, s2, len);
}
template <int W> inline
int Memory<W>::memcmp(const void* ptr1, address_t p2, size_t len) const
{
	fault_if_unreadable(p2, len);

	const auto* s1 = (const char*) ptr1;
	const auto* s2 = &m_memdata[p2];

	return std::memcmp(s1, s2, len);
}

template <int W> inline
void Memory<W>::memcpy(
	address_t dst, Machine<W>& srcm, address_t src, address_t len)
{
	srcm.memory.fault_if_unreadable(src, len);
	this->fault_if_readonly(dst, len);
	std::memcpy(&m_memdata[dst], &srcm.memory.m_memdata[src], len);
}

template <int W> inline
size_t Memory<W>::gather_buffers_from_range(
	size_t cnt, vBuffer buffers[], address_t addr, size_t len)
{
	fault_if_unreadable(addr, len);
	if (cnt > 0) {
		buffers[0].ptr = (char *)&m_memdata[addr];
		buffers[0].len = len;
		return 1;
	}
	return 0;
}

template <int W> inline void
Memory<W>::invalidate_reset_cache() const
{
}
