//
// C++ Header-Only Separate Address-Space Allocator
// by fwsGonzo, originally based on allocator written in C by Snaipe
//
#pragma once
#include "common.hpp"
#include "util/function.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace riscv
{
struct Arena;

struct ArenaChunk
{
	using PointerType = uint32_t;
	static constexpr uint32_t NO_CHUNK = UINT32_MAX;

	ArenaChunk() = default;
	ArenaChunk(uint32_t n, uint32_t p, uint32_t s, bool f, PointerType d)
		: next(n), prev(p), size(s), data(d), free(f) {}

	uint32_t next = NO_CHUNK;
	uint32_t prev = NO_CHUNK;
	uint32_t size = 0;
	PointerType data = 0;
	uint32_t fl_next = NO_CHUNK;
	uint32_t fl_prev = NO_CHUNK;
	bool free = false;
};

struct Arena
{
	static constexpr size_t ALIGNMENT = 16u;
	static constexpr unsigned SL_BITS = 4u;
	static constexpr unsigned SL_COUNT = 1u << SL_BITS;
	static constexpr unsigned SMALL_LOG = SL_BITS + 4u;
	static constexpr size_t SMALL_SIZE = size_t{1} << SMALL_LOG;
	static constexpr unsigned DEFAULT_MIN_CHUNKS = 16'384u;
	static constexpr unsigned DEFAULT_MAX_CHUNKS = 1u << 20;
	static constexpr unsigned SEQ_PROBE_LIMIT = 8u;
	using PointerType = ArenaChunk::PointerType;
	using ReallocResult = std::tuple<PointerType, size_t>;
	using unknown_realloc_func_t = Function<ReallocResult(PointerType, size_t)>;
	using unknown_free_func_t = Function<int(PointerType, ArenaChunk*)>;

	/// Construct an arena for [base, end). An exhausted fork may use an empty range.
	/// Metadata remains host-side.
	/// Both addresses must be 16-byte aligned and the range must fit in uint32_t.
	Arena(PointerType base, PointerType end) : Arena(uint64_t(base), uint64_t(end), 0) {}
	Arena(uint64_t base, uint64_t end, unsigned max_chunks);
	Arena(const Arena& other);

	PointerType malloc(size_t size);
	ReallocResult realloc(PointerType old, size_t size);
	size_t size(PointerType src, bool allow_free = false) const;
	signed int free(PointerType);
	PointerType seq_alloc_aligned(size_t size, size_t alignment,
		bool arena_is_flat = riscv::flat_readwrite_arena);

	/// O(1) accounting used directly by the meminfo syscall.
	size_t bytes_free() const noexcept { return m_bytes_free; }
	size_t bytes_used() const noexcept { return m_total - m_bytes_free; }
	size_t chunks_used() const noexcept { return m_slab_top; }
	size_t live_chunks() const noexcept { return m_live_chunks; }
	unsigned max_chunks() const noexcept { return m_max_chunks; }
	size_t metadata_bytes() const noexcept {
		return m_chunk_slab.capacity() * sizeof(ArenaChunk)
			+ m_used_chunk_table.capacity() * sizeof(UsedSlot) + sizeof(m_bins);
	}

	/// O(1): immediate coalescing means a free tail begins at the watermark.
	PointerType high_watermark() const noexcept {
		const auto& tail = slab(m_tail);
		return tail.free ? tail.data : tail.data + tail.size;
	}

	/// Set the host-controlled metadata cap. This never eagerly grows the slab.
	/// Lowering below chunks_used() is permitted and prevents further slab growth.
	void set_max_chunks(unsigned new_max);

	unsigned allocation_counter() const noexcept { return m_allocation_counter; }
	unsigned deallocation_counter() const noexcept { return m_deallocation_counter; }
	void transfer(Arena& dest) const;

	void on_unknown_free(unknown_free_func_t func) { m_free_unknown_chunk = std::move(func); }
	void on_unknown_realloc(unknown_realloc_func_t func) { m_realloc_unknown_chunk = std::move(func); }

	ArenaChunk& base_chunk() { return m_chunk_slab[0]; }
	const ArenaChunk& base_chunk() const { return m_chunk_slab[0]; }
	ArenaChunk& slab(uint32_t idx) { return m_chunk_slab[idx]; }
	const ArenaChunk& slab(uint32_t idx) const { return m_chunk_slab[idx]; }

	uint32_t new_chunk(uint32_t next, uint32_t prev, size_t size, bool free, PointerType data);
	void free_chunk(uint32_t idx);

	static size_t word_align(size_t size) noexcept {
		if (UNLIKELY(size > SIZE_MAX - (ALIGNMENT - 1))) return size;
		return (size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
	}
	static size_t fixup_size(size_t size) noexcept {
		return std::max(ALIGNMENT, word_align(size));
	}

	/// Full O(n) integrity check for tests/debugging. No allocator syscall calls it.
	bool validate(std::string* error = nullptr) const;

	uint64_t search_steps() const noexcept { return m_search_steps; }
	uint32_t max_probe() const noexcept { return m_max_probe; }
	uint64_t sequential_fallbacks() const noexcept { return m_seq_fallbacks; }
	void reset_search_steps() const noexcept { m_search_steps = 0; }

private:
	struct UsedSlot {
		uint32_t key = 0;
		uint32_t chunk = ArenaChunk::NO_CHUNK;
	};

	static unsigned first_set(uint32_t value) noexcept { return __builtin_ctz(value); }
	static unsigned floor_log2(uint32_t value) noexcept { return 31u - __builtin_clz(value); }
	static void mapping_insert(uint32_t size, unsigned& fl, unsigned& sl) noexcept;
	static bool mapping_search(size_t size, unsigned& fl, unsigned& sl) noexcept;

	void initialize_bins() noexcept;
	void bin_insert(uint32_t idx) noexcept;
	void bin_remove(uint32_t idx) noexcept;
	uint32_t bin_search(size_t size) const noexcept;
	bool first_suitable_bin(size_t size, unsigned& fl, unsigned& sl) const noexcept;
	bool next_suitable_bin(unsigned& fl, unsigned& sl) const noexcept;

	static size_t table_capacity_for(unsigned max_chunks);
	static uint32_t hash_key(uint32_t key) noexcept { return key * 2654435769u; }
	uint32_t pointer_key(PointerType ptr) const noexcept { return (ptr - m_base) >> 4u; }
	uint32_t begin_find_used(PointerType ptr) const noexcept;
	void table_insert(PointerType ptr, uint32_t idx);
	bool table_erase(PointerType ptr) noexcept;
	void rebuild_table(size_t capacity);

	void ensure_chunk_capacity(unsigned count) const;
	void internal_free(uint32_t idx);
	void merge_next_unbinned(uint32_t idx);
	void split_next(uint32_t idx, size_t size);
	void subsume_next(uint32_t idx, size_t newlen);
	bool page_candidate(uint32_t idx, size_t objectsize) const noexcept;
	uint32_t seq_find_candidate(size_t objectsize) const noexcept;

	void debug_step(uint64_t count = 1) const noexcept {
#ifdef RISCV_ARENA_DEBUG
		m_search_steps += count;
#else
		(void)count;
#endif
	}
	void debug_probe(uint32_t count) const noexcept {
#ifdef RISCV_ARENA_DEBUG
		m_max_probe = std::max(m_max_probe, count);
		m_search_steps += count;
#else
		(void)count;
#endif
	}

	std::vector<ArenaChunk> m_chunk_slab;
	uint32_t m_slab_top = 0;
	uint32_t m_slab_free = ArenaChunk::NO_CHUNK;
	uint32_t m_slab_free_count = 0;
	uint32_t m_tail = 0;

	std::array<std::array<uint32_t, SL_COUNT>, 32> m_bins;
	uint32_t m_fl_bitmap = 0;
	std::array<uint16_t, 32> m_sl_bitmap {};

	std::vector<UsedSlot> m_used_chunk_table;
	size_t m_live_chunks = 0;
	PointerType m_base = 0;
	uint32_t m_total = 0;
	size_t m_bytes_free = 0;

	unsigned m_max_chunks = DEFAULT_MIN_CHUNKS;
	unsigned m_allocation_counter = 0;
	unsigned m_deallocation_counter = 0;

	mutable uint64_t m_search_steps = 0;
	mutable uint32_t m_max_probe = 0;
	mutable uint64_t m_seq_fallbacks = 0;

	unknown_free_func_t m_free_unknown_chunk = [] (auto, auto*) { return -1; };
	unknown_realloc_func_t m_realloc_unknown_chunk = [] (auto, auto) { return ReallocResult{0, 0}; };
};

// ---------------------------------------------------------------------------
// Size-class index
// ---------------------------------------------------------------------------

inline void Arena::mapping_insert(uint32_t size, unsigned& fl, unsigned& sl) noexcept
{
	if (size < SMALL_SIZE) {
		fl = 0;
		sl = size / ALIGNMENT;
		return;
	}
	fl = floor_log2(size);
	sl = (size >> (fl - SL_BITS)) & (SL_COUNT - 1u);
}

inline bool Arena::mapping_search(size_t size, unsigned& fl, unsigned& sl) noexcept
{
	if (size < SMALL_SIZE) {
		fl = 0;
		sl = unsigned(size / ALIGNMENT);
		return sl < SL_COUNT;
	}
	if (size > UINT32_MAX) return false;
	const unsigned base_fl = floor_log2(uint32_t(size));
	const uint64_t step = uint64_t{1} << (base_fl - SL_BITS);
	const uint64_t rounded = uint64_t(size) + step - 1u;
	if (rounded > UINT32_MAX) return false;
	mapping_insert(uint32_t(rounded), fl, sl);
	return true;
}

inline void Arena::initialize_bins() noexcept
{
	for (auto& level : m_bins) level.fill(ArenaChunk::NO_CHUNK);
	m_fl_bitmap = 0;
	m_sl_bitmap.fill(0);
}

inline void Arena::bin_insert(uint32_t idx) noexcept
{
	auto& chunk = slab(idx);
	assert(chunk.free);
	unsigned fl, sl;
	mapping_insert(chunk.size, fl, sl);
	chunk.fl_prev = ArenaChunk::NO_CHUNK;
	chunk.fl_next = m_bins[fl][sl];
	if (chunk.fl_next != ArenaChunk::NO_CHUNK)
		slab(chunk.fl_next).fl_prev = idx;
	m_bins[fl][sl] = idx;
	m_sl_bitmap[fl] |= uint16_t(1u << sl);
	m_fl_bitmap |= uint32_t(1u << fl);
	m_bytes_free += chunk.size;
}

inline void Arena::bin_remove(uint32_t idx) noexcept
{
	auto& chunk = slab(idx);
	assert(chunk.free);
	unsigned fl, sl;
	mapping_insert(chunk.size, fl, sl);
	if (chunk.fl_prev != ArenaChunk::NO_CHUNK)
		slab(chunk.fl_prev).fl_next = chunk.fl_next;
	else
		m_bins[fl][sl] = chunk.fl_next;
	if (chunk.fl_next != ArenaChunk::NO_CHUNK)
		slab(chunk.fl_next).fl_prev = chunk.fl_prev;
	chunk.fl_next = chunk.fl_prev = ArenaChunk::NO_CHUNK;
	if (m_bins[fl][sl] == ArenaChunk::NO_CHUNK) {
		m_sl_bitmap[fl] &= uint16_t(~(1u << sl));
		if (m_sl_bitmap[fl] == 0)
			m_fl_bitmap &= ~(uint32_t(1u << fl));
	}
	assert(m_bytes_free >= chunk.size);
	m_bytes_free -= chunk.size;
}

inline bool Arena::first_suitable_bin(size_t size, unsigned& fl, unsigned& sl) const noexcept
{
	if (!mapping_search(size, fl, sl)) return false;
	uint32_t sl_map = uint32_t(m_sl_bitmap[fl]) & (0xffffu << sl);
	if (sl_map != 0) {
		sl = first_set(sl_map);
		return true;
	}
	const uint32_t fl_map = fl == 31 ? 0 : m_fl_bitmap & (~0u << (fl + 1u));
	if (fl_map == 0) return false;
	fl = first_set(fl_map);
	sl = first_set(m_sl_bitmap[fl]);
	return true;
}

inline bool Arena::next_suitable_bin(unsigned& fl, unsigned& sl) const noexcept
{
	uint32_t sl_map = sl == 15 ? 0 : uint32_t(m_sl_bitmap[fl]) & (0xffffu << (sl + 1u));
	if (sl_map != 0) {
		sl = first_set(sl_map);
		return true;
	}
	const uint32_t fl_map = fl == 31 ? 0 : m_fl_bitmap & (~0u << (fl + 1u));
	if (fl_map == 0) return false;
	fl = first_set(fl_map);
	sl = first_set(m_sl_bitmap[fl]);
	return true;
}

inline uint32_t Arena::bin_search(size_t size) const noexcept
{
	if (size <= UINT32_MAX) {
		unsigned current_fl, current_sl;
		mapping_insert(uint32_t(size), current_fl, current_sl);
		for (uint32_t idx = m_bins[current_fl][current_sl];
			 idx != ArenaChunk::NO_CHUNK; idx = slab(idx).fl_next) {
			debug_step();
			if (slab(idx).size >= size) return idx;
		}
	}
	unsigned fl, sl;
	debug_step();
	if (!first_suitable_bin(size, fl, sl)) return ArenaChunk::NO_CHUNK;
	return m_bins[fl][sl];
}

// ---------------------------------------------------------------------------
// Bounded flat used-pointer table
// ---------------------------------------------------------------------------

inline size_t Arena::table_capacity_for(unsigned max_chunks)
{
	const size_t wanted = std::max<size_t>(2, size_t(max_chunks) * 2u);
	size_t capacity = 1;
	while (capacity < wanted) {
		if (capacity > SIZE_MAX / 2)
			throw MachineException(INVALID_PROGRAM, "Native heap chunk cap is too large", max_chunks);
		capacity <<= 1u;
	}
	return capacity;
}

inline uint32_t Arena::begin_find_used(PointerType ptr) const noexcept
{
	if (ptr < m_base || uint64_t(ptr) >= uint64_t(m_base) + m_total ||
		(ptr - m_base) % ALIGNMENT != 0 || m_used_chunk_table.empty())
		return ArenaChunk::NO_CHUNK;
	const uint32_t key = pointer_key(ptr);
	const size_t mask = m_used_chunk_table.size() - 1u;
	size_t slot = hash_key(key) & mask;
	uint32_t probes = 0;
	while (probes++ < m_used_chunk_table.size()) {
		const auto& entry = m_used_chunk_table[slot];
		if (entry.chunk == ArenaChunk::NO_CHUNK) {
			debug_probe(probes);
			return ArenaChunk::NO_CHUNK;
		}
		if (entry.key == key) {
			debug_probe(probes);
			return entry.chunk;
		}
		slot = (slot + 1u) & mask;
	}
	debug_probe(probes);
	return ArenaChunk::NO_CHUNK;
}

inline void Arena::table_insert(PointerType ptr, uint32_t idx)
{
	if ((m_live_chunks + 1u) * 2u > m_used_chunk_table.size())
		throw MachineException(INVALID_PROGRAM, "Native heap pointer table exhausted", m_live_chunks);
	const uint32_t key = pointer_key(ptr);
	const size_t mask = m_used_chunk_table.size() - 1u;
	size_t slot = hash_key(key) & mask;
	uint32_t probes = 1;
	while (m_used_chunk_table[slot].chunk != ArenaChunk::NO_CHUNK) {
		if (m_used_chunk_table[slot].key == key) {
			m_used_chunk_table[slot].chunk = idx;
			debug_probe(probes);
			return;
		}
		slot = (slot + 1u) & mask;
		++probes;
	}
	m_used_chunk_table[slot] = UsedSlot{key, idx};
	++m_live_chunks;
	debug_probe(probes);
}

inline bool Arena::table_erase(PointerType ptr) noexcept
{
	if (ptr < m_base || m_used_chunk_table.empty()) return false;
	const uint32_t key = pointer_key(ptr);
	const size_t mask = m_used_chunk_table.size() - 1u;
	size_t hole = hash_key(key) & mask;
	uint32_t probes = 1;
	while (m_used_chunk_table[hole].chunk != ArenaChunk::NO_CHUNK &&
		m_used_chunk_table[hole].key != key) {
		hole = (hole + 1u) & mask;
		++probes;
	}
	if (m_used_chunk_table[hole].chunk == ArenaChunk::NO_CHUNK) {
		debug_probe(probes);
		return false;
	}

	for (size_t scan = (hole + 1u) & mask;
		 m_used_chunk_table[scan].chunk != ArenaChunk::NO_CHUNK;
		 scan = (scan + 1u) & mask) {
		++probes;
		const size_t home = hash_key(m_used_chunk_table[scan].key) & mask;
		const size_t scan_distance = (scan - home) & mask;
		const size_t hole_distance = (hole - home) & mask;
		if (hole_distance < scan_distance) {
			m_used_chunk_table[hole] = m_used_chunk_table[scan];
			hole = scan;
		}
	}
	m_used_chunk_table[hole] = UsedSlot{};
	--m_live_chunks;
	debug_probe(probes);
	return true;
}

inline void Arena::rebuild_table(size_t capacity)
{
	if (capacity < m_live_chunks * 2u) return;
	std::vector<UsedSlot> old = std::move(m_used_chunk_table);
	m_used_chunk_table.assign(capacity, UsedSlot{});
	m_live_chunks = 0;
	for (const auto& entry : old) {
		if (entry.chunk != ArenaChunk::NO_CHUNK)
			table_insert(slab(entry.chunk).data, entry.chunk);
	}
}

// ---------------------------------------------------------------------------
// Slab and chunk operations
// ---------------------------------------------------------------------------

inline void Arena::ensure_chunk_capacity(unsigned count) const
{
	const uint64_t available = uint64_t(m_slab_free_count)
		+ (m_slab_top < m_max_chunks ? uint64_t(m_max_chunks - m_slab_top) : 0u);
	if (UNLIKELY(count > available))
		throw MachineException(INVALID_PROGRAM, "Too many arena chunks", m_max_chunks);
}

inline uint32_t Arena::new_chunk(uint32_t next, uint32_t prev, size_t size, bool is_free, PointerType data)
{
	assert(size <= UINT32_MAX);
	uint32_t idx;
	if (m_slab_free != ArenaChunk::NO_CHUNK) {
		idx = m_slab_free;
		m_slab_free = m_chunk_slab[idx].next;
		--m_slab_free_count;
	} else {
		ensure_chunk_capacity(1);
		idx = m_slab_top;
		if (idx >= m_chunk_slab.size()) {
			const size_t doubled = std::max<size_t>(8, m_chunk_slab.size() * 2u);
			m_chunk_slab.resize(std::min<size_t>(m_max_chunks, std::max<size_t>(idx + 1u, doubled)));
		}
		++m_slab_top;
	}
	m_chunk_slab[idx] = ArenaChunk{next, prev, uint32_t(size), is_free, data};
	return idx;
}

inline void Arena::free_chunk(uint32_t idx)
{
	m_chunk_slab[idx] = ArenaChunk{};
	m_chunk_slab[idx].next = m_slab_free;
	m_slab_free = idx;
	++m_slab_free_count;
}

inline void Arena::merge_next_unbinned(uint32_t idx)
{
	const uint32_t nidx = slab(idx).next;
	assert(nidx != ArenaChunk::NO_CHUNK);
	const uint32_t next = slab(nidx).next;
	const uint32_t combined = slab(idx).size + slab(nidx).size;
	slab(idx).size = combined;
	slab(idx).next = next;
	if (next != ArenaChunk::NO_CHUNK) slab(next).prev = idx;
	if (m_tail == nidx) m_tail = idx;
	free_chunk(nidx);
}

inline void Arena::split_next(uint32_t idx, size_t size)
{
	const uint32_t old_size = slab(idx).size;
	if (old_size > size) {
		const uint32_t old_next = slab(idx).next;
		const PointerType new_data = slab(idx).data + PointerType(size);
		const uint32_t new_idx = new_chunk(old_next, idx, old_size - size, true, new_data);
		// new_chunk may grow the vector, so every slab reference is re-fetched.
		if (old_next != ArenaChunk::NO_CHUNK) slab(old_next).prev = new_idx;
		slab(idx).next = new_idx;
		slab(idx).size = uint32_t(size);
		if (m_tail == idx) m_tail = new_idx;
		bin_insert(new_idx);
	} else {
		slab(idx).size = uint32_t(size);
	}
}

inline void Arena::subsume_next(uint32_t idx, size_t newlen)
{
	assert(slab(idx).size < newlen);
	const uint32_t nidx = slab(idx).next;
	assert(nidx != ArenaChunk::NO_CHUNK && slab(nidx).free);
	if (uint64_t(slab(idx).size) + slab(nidx).size < newlen) return;

	bin_remove(nidx);
	const uint32_t amount = uint32_t(newlen - slab(idx).size);
	const uint32_t remainder = slab(nidx).size - amount;
	slab(idx).size = uint32_t(newlen);
	if (remainder == 0) {
		const uint32_t next = slab(nidx).next;
		slab(idx).next = next;
		if (next != ArenaChunk::NO_CHUNK) slab(next).prev = idx;
		if (m_tail == nidx) m_tail = idx;
		free_chunk(nidx);
	} else {
		slab(nidx).size = remainder;
		slab(nidx).data += amount;
		bin_insert(nidx);
	}
}

inline void Arena::internal_free(uint32_t idx)
{
	++m_deallocation_counter;
	const PointerType data = slab(idx).data;
	const bool erased = table_erase(data);
	(void)erased;
	assert(erased);
	slab(idx).free = true;

	if (slab(idx).next != ArenaChunk::NO_CHUNK && slab(slab(idx).next).free) {
		bin_remove(slab(idx).next);
		merge_next_unbinned(idx);
	}
	if (slab(idx).prev != ArenaChunk::NO_CHUNK && slab(slab(idx).prev).free) {
		const uint32_t prev = slab(idx).prev;
		bin_remove(prev);
		merge_next_unbinned(prev);
		idx = prev;
	}
	bin_insert(idx);
}

// ---------------------------------------------------------------------------
// Public allocator operations
// ---------------------------------------------------------------------------

inline Arena::PointerType Arena::malloc(size_t size)
{
	const size_t length = fixup_size(size);
	++m_allocation_counter;
	if (UNLIKELY(length > m_total)) return 0;
	const uint32_t idx = bin_search(length);
	if (idx == ArenaChunk::NO_CHUNK) return 0;
	ensure_chunk_capacity(slab(idx).size > length ? 1u : 0u);
	bin_remove(idx);
	split_next(idx, length);
	slab(idx).free = false;
	table_insert(slab(idx).data, idx);
	return slab(idx).data;
}

inline bool Arena::page_candidate(uint32_t idx, size_t objectsize) const noexcept
{
	const auto& chunk = slab(idx);
	const uint64_t last = uint64_t(chunk.data) + objectsize - 1u;
	if ((chunk.data & ~(RISCV_PAGE_SIZE - 1u)) == (last & ~(RISCV_PAGE_SIZE - 1u)))
		return true;
	const uint64_t boundary = (uint64_t(chunk.data) + RISCV_PAGE_SIZE - 1u) & ~(uint64_t(RISCV_PAGE_SIZE) - 1u);
	const uint64_t prefix = boundary - chunk.data;
	return prefix <= chunk.size && uint64_t(chunk.size) - prefix >= objectsize;
}

inline uint32_t Arena::seq_find_candidate(size_t objectsize) const noexcept
{
	unsigned fl, sl;
	if (!first_suitable_bin(objectsize, fl, sl)) return ArenaChunk::NO_CHUNK;
	unsigned examined = 0;
	do {
		for (uint32_t idx = m_bins[fl][sl]; idx != ArenaChunk::NO_CHUNK; idx = slab(idx).fl_next) {
			debug_step();
			if (page_candidate(idx, objectsize)) return idx;
			if (++examined == SEQ_PROBE_LIMIT) goto fallback;
		}
	} while (next_suitable_bin(fl, sl));
	return ArenaChunk::NO_CHUNK;

fallback:
#ifdef RISCV_ARENA_DEBUG
	++m_seq_fallbacks;
#endif
	if (objectsize > SIZE_MAX - RISCV_PAGE_SIZE) return ArenaChunk::NO_CHUNK;
	return bin_search(objectsize + RISCV_PAGE_SIZE);
}

inline Arena::PointerType Arena::seq_alloc_aligned(size_t size, size_t alignment, bool arena_is_flat)
{
	(void)alignment;
	if (arena_is_flat) return malloc(size);
	const size_t objectsize = fixup_size(size);
	++m_allocation_counter;
	if (objectsize > RISCV_PAGE_SIZE)
		throw MachineException(INVALID_PROGRAM, "Requested sequential allocation too large", objectsize);
	if (UNLIKELY(objectsize > m_total)) return 0;
	uint32_t idx = seq_find_candidate(objectsize);
	if (idx == ArenaChunk::NO_CHUNK) return 0;

	const auto& candidate = slab(idx);
	const uint64_t last = uint64_t(candidate.data) + objectsize - 1u;
	const bool crosses = (candidate.data & ~(RISCV_PAGE_SIZE - 1u)) !=
		(last & ~(uint64_t(RISCV_PAGE_SIZE) - 1u));
	const size_t prefix = crosses
		? ((uint64_t(candidate.data) + RISCV_PAGE_SIZE - 1u)
			& ~(uint64_t(RISCV_PAGE_SIZE) - 1u)) - candidate.data
		: 0u;
	const unsigned splits = (crosses ? 1u : 0u)
		+ (candidate.size - prefix > objectsize ? 1u : 0u);
	ensure_chunk_capacity(splits);
	bin_remove(idx);
	if (crosses) {
		split_next(idx, prefix);
		bin_insert(idx);
		idx = slab(idx).next;
		bin_remove(idx);
	}
	split_next(idx, objectsize);
	slab(idx).free = false;
	table_insert(slab(idx).data, idx);
	return slab(idx).data;
}

inline Arena::ReallocResult Arena::realloc(PointerType ptr, size_t newsize)
{
	if (ptr == 0) return {malloc(newsize), 0};
	const size_t length = fixup_size(newsize);
	if (UNLIKELY(length > m_total)) return {0, 0};
	const uint32_t idx = begin_find_used(ptr);
	if (UNLIKELY(idx == ArenaChunk::NO_CHUNK))
		return m_realloc_unknown_chunk(ptr, newsize);
	if (slab(idx).size >= length) return {slab(idx).data, 0};
	const size_t old_len = slab(idx).size;
	if (slab(idx).next != ArenaChunk::NO_CHUNK && slab(slab(idx).next).free) {
		subsume_next(idx, length);
		if (slab(idx).size >= length) return {slab(idx).data, 0};
	}
	const PointerType newptr = malloc(length);
	if (newptr != 0) {
		internal_free(idx);
		return {newptr, old_len};
	}
	return {0, 0};
}

inline size_t Arena::size(PointerType ptr, bool allow_free) const
{
	const uint32_t idx = begin_find_used(ptr);
	if (UNLIKELY(idx == ArenaChunk::NO_CHUNK)) return 0;
	const auto& chunk = slab(idx);
	if (chunk.free && !allow_free) return 0;
	return chunk.size;
}

inline int Arena::free(PointerType ptr)
{
	const uint32_t idx = begin_find_used(ptr);
	if (UNLIKELY(idx == ArenaChunk::NO_CHUNK)) return m_free_unknown_chunk(ptr, nullptr);
	if (UNLIKELY(slab(idx).free)) return m_free_unknown_chunk(ptr, &slab(idx));
	internal_free(idx);
	return 0;
}

// ---------------------------------------------------------------------------
// Construction, configuration and transfer
// ---------------------------------------------------------------------------

inline Arena::Arena(uint64_t arena_base, uint64_t arena_end, unsigned max_chunks)
{
	if (arena_end < arena_base || arena_end > UINT32_MAX ||
		arena_base > UINT32_MAX || (arena_base % ALIGNMENT) != 0 ||
		(arena_end % ALIGNMENT) != 0 || arena_end - arena_base > UINT32_MAX)
		throw MachineException(INVALID_PROGRAM, "Invalid native heap address range", arena_base);
	m_base = PointerType(arena_base);
	m_total = uint32_t(arena_end - arena_base);
	m_max_chunks = max_chunks != 0 ? max_chunks : unsigned(std::clamp<uint64_t>(
		m_total / 64u, DEFAULT_MIN_CHUNKS, DEFAULT_MAX_CHUNKS));
	initialize_bins();
	m_chunk_slab.resize(std::min<unsigned>(8u, m_max_chunks));
	if (m_chunk_slab.empty()) m_chunk_slab.resize(1);
	m_chunk_slab[0] = ArenaChunk{ArenaChunk::NO_CHUNK, ArenaChunk::NO_CHUNK,
		m_total, true, m_base};
	m_slab_top = 1;
	m_tail = 0;
	m_used_chunk_table.assign(table_capacity_for(m_max_chunks), UsedSlot{});
	bin_insert(0);
}

inline Arena::Arena(const Arena& other)
{
	other.transfer(*this);
}

inline void Arena::set_max_chunks(unsigned new_max)
{
	const size_t new_capacity = table_capacity_for(new_max);
	if (new_capacity != m_used_chunk_table.size() && new_capacity >= m_live_chunks * 2u)
		rebuild_table(new_capacity);
	m_max_chunks = new_max;
}

inline void Arena::transfer(Arena& dest) const
{
	std::vector<ArenaChunk> slab_copy(m_chunk_slab.begin(), m_chunk_slab.begin() + m_slab_top);
	std::vector<UsedSlot> table_copy(m_used_chunk_table);
	dest.m_chunk_slab.swap(slab_copy);
	dest.m_slab_top = m_slab_top;
	dest.m_slab_free = m_slab_free;
	dest.m_slab_free_count = m_slab_free_count;
	dest.m_tail = m_tail;
	dest.m_bins = m_bins;
	dest.m_fl_bitmap = m_fl_bitmap;
	dest.m_sl_bitmap = m_sl_bitmap;
	dest.m_used_chunk_table.swap(table_copy);
	dest.m_live_chunks = m_live_chunks;
	dest.m_base = m_base;
	dest.m_total = m_total;
	dest.m_bytes_free = m_bytes_free;
	dest.m_max_chunks = m_max_chunks;
	dest.m_allocation_counter = m_allocation_counter;
	dest.m_deallocation_counter = m_deallocation_counter;
	dest.m_search_steps = m_search_steps;
	dest.m_max_probe = m_max_probe;
	dest.m_seq_fallbacks = m_seq_fallbacks;
}

// ---------------------------------------------------------------------------
// Debug integrity checker
// ---------------------------------------------------------------------------

inline bool Arena::validate(std::string* error) const
{
	auto fail = [&] (const char* message) {
		if (error) *error = message;
		return false;
	};
	if (m_slab_top == 0 || m_slab_top > m_chunk_slab.size())
		return fail("invalid slab bounds");
	std::vector<uint8_t> address_seen(m_slab_top, 0), bin_seen(m_slab_top, 0), free_seen(m_slab_top, 0);
	uint64_t cursor = m_base;
	uint64_t recomputed_free = 0;
	size_t used_count = 0;
	uint32_t previous = ArenaChunk::NO_CHUNK;
	uint32_t idx = 0;
	uint32_t recomputed_tail = 0;
	PointerType recomputed_hwm = m_base;
	while (idx != ArenaChunk::NO_CHUNK) {
		if (idx >= m_slab_top || address_seen[idx]) return fail("address list cycle or invalid index");
		address_seen[idx] = 1;
		const auto& chunk = slab(idx);
		if (chunk.prev != previous) return fail("broken prev link");
		if (chunk.data != cursor) return fail("address list gap or overlap");
		if ((chunk.size < ALIGNMENT && (chunk.size != 0 || m_total != 0)) ||
			chunk.size % ALIGNMENT || chunk.data % ALIGNMENT)
			return fail("misaligned chunk");
		cursor += chunk.size;
		if (cursor > uint64_t(m_base) + m_total) return fail("chunk exceeds arena");
		if (chunk.free) {
			recomputed_free += chunk.size;
			if (chunk.next != ArenaChunk::NO_CHUNK && slab(chunk.next).free)
				return fail("adjacent free chunks");
		} else {
			++used_count;
			recomputed_hwm = chunk.data + chunk.size;
			if (begin_find_used(chunk.data) != idx) return fail("used table missing chunk");
		}
		previous = idx;
		recomputed_tail = idx;
		idx = chunk.next;
	}
	if (cursor != uint64_t(m_base) + m_total) return fail("arena not fully covered");
	if (recomputed_tail != m_tail) return fail("tail mismatch");
	if (recomputed_free != m_bytes_free) return fail("free-byte cache mismatch");
	if (recomputed_hwm != high_watermark()) return fail("watermark cache mismatch");

	uint32_t expected_fl = 0;
	std::array<uint16_t, 32> expected_sl {};
	for (unsigned fl = 0; fl < 32; ++fl) {
		for (unsigned sl = 0; sl < SL_COUNT; ++sl) {
			uint32_t prev = ArenaChunk::NO_CHUNK;
			for (uint32_t b = m_bins[fl][sl]; b != ArenaChunk::NO_CHUNK; b = slab(b).fl_next) {
				if (b >= m_slab_top || bin_seen[b]) return fail("free-bin cycle or duplicate");
				bin_seen[b] = 1;
				const auto& chunk = slab(b);
				if (!chunk.free || chunk.fl_prev != prev) return fail("invalid free-bin link");
				unsigned actual_fl, actual_sl;
				mapping_insert(chunk.size, actual_fl, actual_sl);
				if (actual_fl != fl || actual_sl != sl) return fail("chunk in wrong size class");
				prev = b;
			}
			if (m_bins[fl][sl] != ArenaChunk::NO_CHUNK) {
				expected_fl |= 1u << fl;
				expected_sl[fl] |= uint16_t(1u << sl);
			}
		}
	}
	if (expected_fl != m_fl_bitmap || expected_sl != m_sl_bitmap) return fail("bitmap mismatch");
	for (uint32_t i = 0; i < m_slab_top; ++i)
		if (address_seen[i] && slab(i).free != bool(bin_seen[i])) return fail("free chunk/bin mismatch");

	size_t table_count = 0;
	const size_t mask = m_used_chunk_table.size() - 1u;
	for (size_t slot = 0; slot < m_used_chunk_table.size(); ++slot) {
		const auto& entry = m_used_chunk_table[slot];
		if (entry.chunk == ArenaChunk::NO_CHUNK) continue;
		++table_count;
		if (entry.chunk >= m_slab_top || !address_seen[entry.chunk] || slab(entry.chunk).free)
			return fail("table points to invalid chunk");
		if (entry.key != pointer_key(slab(entry.chunk).data)) return fail("table key mismatch");
		const size_t home = hash_key(entry.key) & mask;
		for (size_t probe = home; probe != slot; probe = (probe + 1u) & mask)
			if (m_used_chunk_table[probe].chunk == ArenaChunk::NO_CHUNK)
				return fail("broken linear-probing chain");
	}
	if (table_count != used_count || table_count != m_live_chunks) return fail("table live-count mismatch");

	uint32_t free_count = 0;
	for (uint32_t f = m_slab_free; f != ArenaChunk::NO_CHUNK; f = slab(f).next) {
		if (f >= m_slab_top || free_seen[f]) return fail("slab free-list cycle or invalid index");
		if (address_seen[f]) return fail("active chunk on slab free list");
		free_seen[f] = 1;
		++free_count;
	}
	if (free_count != m_slab_free_count) return fail("slab free-count mismatch");
	return true;
}

} // namespace riscv
