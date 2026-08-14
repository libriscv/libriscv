//
// C++ Header-Only Separate Address-Space Allocator
// by fwsGonzo, originally based on allocator written in C by Snaipe
//
#pragma once
#include "common.hpp"
#include <cstddef>
#include <cassert>
#include <unordered_map>
#include <vector>
#include "util/function.hpp"

namespace riscv
{
struct Arena;

struct ArenaChunk
{
	using PointerType = uint32_t;
	static constexpr uint32_t NO_CHUNK = UINT32_MAX;

	ArenaChunk() = default;
	ArenaChunk(uint32_t n, uint32_t p, size_t s, bool f, PointerType d)
		: next(n), prev(p), size(s), free(f), data(d) {}

	uint32_t next = NO_CHUNK;
	uint32_t prev = NO_CHUNK;
	size_t size = 0;
	bool   free = false;
	PointerType data = 0;
};

struct Arena
{
	static constexpr size_t ALIGNMENT = 16u;
	using PointerType = ArenaChunk::PointerType;
	using ReallocResult = std::tuple<PointerType, size_t>;
	using unknown_realloc_func_t = Function<ReallocResult(PointerType, size_t)>;
	using unknown_free_func_t = Function<int(PointerType, ArenaChunk *)>;

	/// @brief Construct an arena that manages allocations for a given memory range.
	/// @param base The base (lowest) guest address owned by this arena.
	/// @param end  One-past-the-end guest address; the initial free chunk spans [base, end).
	/// @note The slab vector is pre-allocated to @p m_max_chunks entries at construction time.
	Arena(PointerType base, PointerType end);

	/// @brief Copy-construct by transferring all allocations from @p other.
	/// @param other Source arena; it is left unchanged so multiple destinations can be seeded.
	/// @note Equivalent to calling `other.transfer(*this)`.
	Arena(const Arena& other);

	/// @brief Allocate a region of guest memory.
	/// @param size Requested allocation size in bytes (rounded up to 16-byte alignment).
	/// @return Guest address of the allocated region, or 0 on failure.
	/// @note The returned memory is not zeroed. Lookup for subsequent free/realloc is O(1).
	PointerType   malloc(size_t size);

	/// @brief Resize an existing allocation.
	/// @param old     Guest address previously returned by malloc() or seq_alloc_aligned().
	///                Passing 0 behaves like malloc(@p size).
	/// @param size    New desired size in bytes.
	/// @return {new_ptr, bytes_to_copy}: if new_ptr == old_ptr the block was grown in-place
	///         and no copy is needed (bytes_to_copy == 0); otherwise the caller must copy
	///         @p bytes_to_copy bytes from the old address to new_ptr, then the old block is freed.
	///         Returns {0, 0} if the reallocation failed.
	/// @note If @p old is not found in this arena the registered on_unknown_realloc() callback
	///       is invoked (used by the fast-fork path to delegate to the parent arena).
	ReallocResult realloc(PointerType old, size_t size);

	/// @brief Query the allocated size of a guest pointer.
	/// @param src        Guest address to look up.
	/// @param allow_free If true, report the size even for free chunks (useful for debugging).
	/// @return The chunk size in bytes, or 0 if @p src is not a live allocation.
	size_t        size(PointerType src, bool allow_free = false) const;

	/// @brief Free a previous allocation.
	/// @param ptr Guest address to free.
	/// @return 0 on success, -1 if @p ptr is not found in this arena.
	/// @note Adjacent free chunks are coalesced immediately. If @p ptr is not found the
	///       registered on_unknown_free() callback is invoked.
	signed int    free(PointerType);

	/// @brief Allocate a region that does not straddle a page boundary.
	/// @param size          Requested size in bytes.
	/// @param alignment     Desired alignment (currently unused; 16-byte alignment is always applied).
	/// @param arena_is_flat When true (flat arena mode) this delegates straight to malloc().
	///                      When false the allocator skips candidates that cross a page boundary,
	///                      splitting the chunk at the boundary as needed.
	/// @return Guest address of the allocated region, or 0 on failure.
	/// @throws MachineException if @p size exceeds RISCV_PAGE_SIZE or if the chunk limit is hit.
	PointerType seq_alloc_aligned(size_t size, size_t alignment, bool arena_is_flat = riscv::flat_readwrite_arena);

	/// @brief Total bytes currently held in free chunks.
	size_t bytes_free() const;

	/// @brief Total bytes currently held in live (non-free) chunks.
	size_t bytes_used() const;

	/// @brief Number of slab slots consumed (live + recycled-but-not-yet-reused).
	size_t chunks_used() const noexcept { return m_slab_top; }

	/// @brief Highest guest address covered by any live (non-free) allocation.
	/// @details Call once after the master VM is fully initialised and cache the result.
	///          Each fork uses the cached watermark as the base of its fresh arena so that
	///          no chunk-list copy is needed at fork time.  Runs in O(n) over all chunks.
	PointerType high_watermark() const {
		PointerType hwm = slab(0).data;
		uint32_t idx = 0;
		while (idx != ArenaChunk::NO_CHUNK) {
			const auto& ch = m_chunk_slab[idx];
			if (!ch.free)
				hwm = std::max(hwm, ch.data + (PointerType)ch.size);
			idx = ch.next;
		}
		return hwm;
	}

	/// @brief Override the maximum number of slab slots (default: 4000).
	/// Grows the slab storage to match: new_chunk() indexes the slab directly
	/// up to this limit, so the vector must always hold m_max_chunks entries.
	/// @param new_max New cap; must be set before the first allocation.
	void set_max_chunks(unsigned new_max)
	{
		this->m_max_chunks = new_max;
		if (m_chunk_slab.size() < new_max)
			m_chunk_slab.resize(new_max);
	}

	/// @brief Total number of successful malloc() / seq_alloc_aligned() calls since construction.
	unsigned allocation_counter()   const noexcept { return m_allocation_counter; }

	/// @brief Total number of successful free() calls since construction.
	unsigned deallocation_counter() const noexcept { return m_deallocation_counter; }

	/// @brief Deep-copy all arena state into @p dest, overwriting it.
	/// @param dest Destination arena; any prior state is replaced.
	/// @note The source arena is unchanged; used by the copy constructor and the fast-fork path.
	void transfer(Arena& dest) const;

	/// @brief Register a fallback for free() on pointers not owned by this arena.
	/// @param func Callable receiving the unknown pointer and a (possibly null) ArenaChunk hint;
	///             must return 0 on success or -1 on failure.
	/// @note Used by the fast-fork path so forks can delegate pre-fork allocations to the parent.
	void on_unknown_free(unknown_free_func_t func) {
		m_free_unknown_chunk = std::move(func);
	}

	/// @brief Register a fallback for realloc() on pointers not owned by this arena.
	/// @param func Callable receiving the unknown pointer and the new size;
	///             must return a ReallocResult (same semantics as realloc()).
	/// @note Used by the fast-fork path so forks can delegate pre-fork allocations to the parent.
	void on_unknown_realloc(unknown_realloc_func_t func) {
		m_realloc_unknown_chunk = std::move(func);
	}

	/** Internal usage **/
	ArenaChunk&       base_chunk()       { return m_chunk_slab[0]; }
	const ArenaChunk& base_chunk() const { return m_chunk_slab[0]; }

	ArenaChunk&       slab(uint32_t idx)       { return m_chunk_slab[idx]; }
	const ArenaChunk& slab(uint32_t idx) const { return m_chunk_slab[idx]; }

	uint32_t new_chunk(uint32_t next, uint32_t prev, size_t sz, bool f, PointerType d);
	void     free_chunk(uint32_t idx);

	static size_t word_align(size_t size) {
		// Adding the alignment must not be allowed to overflow
		if (UNLIKELY(size > SIZE_MAX - (ALIGNMENT-1)))
			return size;
		return (size + (ALIGNMENT-1)) & ~(ALIGNMENT-1);
	}
	static size_t fixup_size(size_t size) {
		return std::max(ALIGNMENT, word_align(size));
	}

private:
	uint32_t begin_find_used(PointerType ptr) const;
	uint32_t find_free(size_t size) const;

	void internal_free(uint32_t idx);
	void merge_next(uint32_t idx);
	void split_next(uint32_t idx, size_t size);
	void subsume_next(uint32_t idx, size_t newlen);

	std::vector<ArenaChunk> m_chunk_slab;
	uint32_t m_slab_top  = 0;
	uint32_t m_slab_free = ArenaChunk::NO_CHUNK;

	std::unordered_map<PointerType, uint32_t> m_used_chunk_map;

	unsigned m_max_chunks = 4'000u;
	unsigned m_allocation_counter   = 0u;
	unsigned m_deallocation_counter = 0u;

	unknown_free_func_t m_free_unknown_chunk
		= [] (auto, auto*) { return -1; };
	unknown_realloc_func_t m_realloc_unknown_chunk
		= [] (auto, auto) { return ReallocResult{0, 0}; };
};

// ---------------------------------------------------------------------------
// Slab management
// ---------------------------------------------------------------------------

inline uint32_t Arena::new_chunk(uint32_t next, uint32_t prev, size_t sz, bool f, PointerType d)
{
	uint32_t idx;
	if (m_slab_free != ArenaChunk::NO_CHUNK) {
		idx = m_slab_free;
		m_slab_free = m_chunk_slab[idx].next;
	} else {
		if (UNLIKELY(m_slab_top >= m_max_chunks))
			throw MachineException(INVALID_PROGRAM, "Too many arena chunks", m_max_chunks);
		idx = m_slab_top++;
	}
	m_chunk_slab[idx] = ArenaChunk{next, prev, sz, f, d};
	return idx;
}

inline void Arena::free_chunk(uint32_t idx)
{
	m_chunk_slab[idx].next = m_slab_free;
	m_slab_free = idx;
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

inline uint32_t Arena::begin_find_used(PointerType ptr) const
{
	auto it = m_used_chunk_map.find(ptr);
	if (it != m_used_chunk_map.end())
		return it->second;
	return ArenaChunk::NO_CHUNK;
}

inline uint32_t Arena::find_free(size_t size) const
{
	uint32_t idx = 0; // always start from slot 0 (head of linked list)
	while (idx != ArenaChunk::NO_CHUNK) {
		const auto& ch = m_chunk_slab[idx];
		if (ch.free && ch.size >= size)
			return idx;
		idx = ch.next;
	}
	return ArenaChunk::NO_CHUNK;
}

// ---------------------------------------------------------------------------
// Chunk operations
// ---------------------------------------------------------------------------

inline void Arena::merge_next(uint32_t idx)
{
	auto& ch  = slab(idx);
	uint32_t nidx = ch.next;
	auto& nch = slab(nidx);
	ch.size  += nch.size;
	ch.next   = nch.next;
	if (ch.next != ArenaChunk::NO_CHUNK)
		slab(ch.next).prev = idx;
	free_chunk(nidx);
}

inline void Arena::split_next(uint32_t idx, size_t size)
{
	auto& ch = slab(idx);
	if (ch.size > size) {
		uint32_t newIdx = new_chunk(
			ch.next, idx,
			ch.size - size, true,
			ch.data + (PointerType)size);
		if (ch.next != ArenaChunk::NO_CHUNK)
			slab(ch.next).prev = newIdx;
		ch.next = newIdx;
	}
	// Exact fit: neighbors already point correctly to idx; no surgery needed.
	slab(idx).size = size;
}

inline void Arena::subsume_next(uint32_t idx, size_t newlen)
{
	auto& ch  = slab(idx);
	assert(ch.size < newlen);
	uint32_t nidx = ch.next;
	assert(nidx != ArenaChunk::NO_CHUNK);
	auto& nch = slab(nidx);

	if (ch.size + nch.size < newlen)
		return;

	const size_t subsume = newlen - ch.size;
	nch.size -= subsume;
	nch.data += (PointerType)subsume;
	ch.size   = newlen;

	if (nch.size == 0) {
		ch.next = nch.next;
		if (ch.next != ArenaChunk::NO_CHUNK)
			slab(ch.next).prev = idx;
		free_chunk(nidx);
	}
}

inline void Arena::internal_free(uint32_t idx)
{
	this->m_deallocation_counter++;
	auto& ch = slab(idx);
	m_used_chunk_map.erase(ch.data);
	ch.free = true;

	if (ch.next != ArenaChunk::NO_CHUNK && slab(ch.next).free)
		merge_next(idx);
	if (ch.prev != ArenaChunk::NO_CHUNK && slab(ch.prev).free) {
		uint32_t pidx = slab(idx).prev;
		merge_next(pidx);
	}
}

// ---------------------------------------------------------------------------
// Public allocator operations
// ---------------------------------------------------------------------------

inline Arena::PointerType Arena::malloc(size_t size)
{
	const size_t length = fixup_size(size);
	uint32_t idx = find_free(length);
	this->m_allocation_counter++;

	if (idx != ArenaChunk::NO_CHUNK) {
		split_next(idx, length);
		auto& ch = slab(idx);
		ch.free = false;
		m_used_chunk_map.insert_or_assign(ch.data, idx);
		return ch.data;
	}
	return 0;
}

inline Arena::PointerType Arena::seq_alloc_aligned(size_t size, size_t alignment, bool arena_is_flat)
{
	(void)alignment;

	if (arena_is_flat)
		return malloc(size);

	const size_t objectsize = fixup_size(size);
	this->m_allocation_counter++;
	if (objectsize > RISCV_PAGE_SIZE)
		throw MachineException(INVALID_PROGRAM, "Requested sequential allocation too large", objectsize);

	uint32_t idx = 0;
restart:
	while (idx != ArenaChunk::NO_CHUNK) {
		const auto& ch = m_chunk_slab[idx];
		if (ch.free && ch.size >= objectsize) break;
		idx = ch.next;
	}

	if (idx != ArenaChunk::NO_CHUNK) {
		auto& ch = slab(idx);
		if ((ch.data & ~(RISCV_PAGE_SIZE-1)) !=
			((ch.data + objectsize - 1) & ~(RISCV_PAGE_SIZE-1)))
		{
			const PointerType boundary = (ch.data + objectsize - 1) & ~(RISCV_PAGE_SIZE-1);
			if (boundary < ch.data)
				throw MachineException(INVALID_PROGRAM, "Page boundary overflow", boundary);
			const size_t remaining = boundary - ch.data;
			if (ch.size - remaining < objectsize) {
				if (ch.next == ArenaChunk::NO_CHUNK) return 0;
				idx = ch.next;
				goto restart;
			}
			split_next(idx, remaining);
			idx = slab(idx).next;
		}

		split_next(idx, objectsize);
		auto& ch2 = slab(idx);
		ch2.free = false;
		m_used_chunk_map.insert_or_assign(ch2.data, idx);
		return ch2.data;
	}
	return 0;
}

inline Arena::ReallocResult Arena::realloc(PointerType ptr, size_t newsize)
{
	if (ptr == 0x0)
		return {malloc(newsize), 0};

	uint32_t idx = this->begin_find_used(ptr);
	if (UNLIKELY(idx == ArenaChunk::NO_CHUNK))
		return m_realloc_unknown_chunk(ptr, newsize);

	newsize = fixup_size(newsize);
	if (slab(idx).size >= newsize)
		return {slab(idx).data, 0};

	const size_t old_len = slab(idx).size;

	if (slab(idx).next != ArenaChunk::NO_CHUNK && slab(slab(idx).next).free) {
		subsume_next(idx, newsize);
		if (slab(idx).size >= newsize)
			return {slab(idx).data, 0};
	}

	PointerType newptr = malloc(newsize);
	if (newptr != 0x0) {
		internal_free(idx);
		return {newptr, old_len};
	}
	return {0x0, 0x0};
}

inline size_t Arena::size(PointerType ptr, bool allow_free) const
{
	uint32_t idx = this->begin_find_used(ptr);
	if (UNLIKELY(idx == ArenaChunk::NO_CHUNK))
		return 0;
	const auto& ch = slab(idx);
	if (ch.free && !allow_free)
		return 0;
	return ch.size;
}

inline int Arena::free(PointerType ptr)
{
	uint32_t idx = this->begin_find_used(ptr);
	if (UNLIKELY(idx == ArenaChunk::NO_CHUNK))
		return m_free_unknown_chunk(ptr, nullptr);
	if (UNLIKELY(slab(idx).free))
		return m_free_unknown_chunk(ptr, &slab(idx));

	this->internal_free(idx);
	return 0;
}

// ---------------------------------------------------------------------------
// Construction / transfer
// ---------------------------------------------------------------------------

inline Arena::Arena(PointerType arena_base, PointerType arena_end)
{
	m_chunk_slab.resize(m_max_chunks);
	m_chunk_slab[0] = ArenaChunk{ArenaChunk::NO_CHUNK, ArenaChunk::NO_CHUNK,
	                              (size_t)(arena_end - arena_base), true, arena_base};
	m_slab_top = 1;
}

inline Arena::Arena(const Arena& other)
{
	other.transfer(*this);
}

inline void Arena::transfer(Arena& dest) const
{
	dest.m_chunk_slab         = m_chunk_slab;
	dest.m_slab_top           = m_slab_top;
	dest.m_slab_free          = m_slab_free;
	dest.m_used_chunk_map     = m_used_chunk_map;
	dest.m_max_chunks         = m_max_chunks;
	dest.m_allocation_counter   = m_allocation_counter;
	dest.m_deallocation_counter = m_deallocation_counter;
}

inline size_t Arena::bytes_free() const
{
	size_t total = 0;
	uint32_t idx = 0;
	while (idx != ArenaChunk::NO_CHUNK) {
		const auto& ch = m_chunk_slab[idx];
		if (ch.free) total += ch.size;
		idx = ch.next;
	}
	return total;
}

inline size_t Arena::bytes_used() const
{
	size_t total = 0;
	uint32_t idx = 0;
	while (idx != ArenaChunk::NO_CHUNK) {
		const auto& ch = m_chunk_slab[idx];
		if (!ch.free) total += ch.size;
		idx = ch.next;
	}
	return total;
}

} // namespace riscv
