//
// C++ Header-Only Separate Address-Space Allocator
// by fwsGonzo, originally based on allocator written in C by Snaipe
//
#pragma once
#include <cstddef>
#include <deque>
#include <vector>

namespace sas_alloc
{
struct Arena;

struct Chunk
{
	using PointerType = uint32_t;

	Chunk() = default;
	Chunk(Chunk* n, Chunk* p, size_t s, bool f, PointerType d)
		: next(n), prev(p), size(s), free(f), data(d) {}

    Chunk* next = nullptr;
	Chunk* prev = nullptr;
    size_t size = 0;
    bool   free = false;
    PointerType data = 0;

	Chunk* find(PointerType ptr);
	Chunk* find_free(size_t size);
	void   merge_next(Arena&);
	void   split_next(Arena&, size_t size);
};

struct Arena
{
	using PointerType = Chunk::PointerType;
	Arena(PointerType base, PointerType end);

	PointerType malloc(size_t size);
	signed int  free(PointerType);

	size_t bytes_free() const;
	size_t bytes_used() const;
	size_t chunks_used() const noexcept { return m_chunks.size(); }

	inline Chunk& base_chunk() {
	    return m_base_chunk;
	}
	template <typename... Args>
	Chunk* new_chunk(Args&&... args);
	void   free_chunk(Chunk*);
	Chunk* find_chunk(PointerType ptr);

private:
	inline size_t word_align(size_t size) {
	    return (size + (sizeof(size_t) - 1)) & ~(sizeof(size_t) - 1);
	}
	void foreach(std::function<void(const Chunk&)>) const;

	std::deque<Chunk>   m_chunks;
	std::vector<Chunk*> m_free_chunks;
	Chunk  m_base_chunk;
	Chunk* last_chunk = &m_base_chunk;
};

// find exact free chunk that matches ptr
inline Chunk* Chunk::find(PointerType ptr)
{
    Chunk* ch = this;
    while (ch != nullptr) {
		if (!ch->free && ch->data == ptr)
			return ch;
		ch = ch->next;
	}
    return nullptr;
}
// find free chunk that has at least given size
inline Chunk* Chunk::find_free(size_t size)
{
    Chunk* ch = this;
	while (ch != nullptr) {
		if (ch->free && ch->size >= size)
			return ch;
		ch = ch->next;
	}
    return nullptr;
}
// merge this and next into this chunk
inline void Chunk::merge_next(Arena& arena)
{
	Chunk* freech = this->next;
    this->size += freech->size;
    this->next = freech->next;
    if (this->next) {
        this->next->prev = this;
    }
	arena.free_chunk(freech);
}

inline void Chunk::split_next(Arena& arena, size_t size)
{
	Chunk* newch = arena.new_chunk(
		this->next,
		this,
		this->size - size,
		true,
		this->data + (PointerType) size
	);
    if (this->next) {
        this->next->prev = newch;
    }
    this->next = newch;
    this->size = size;
}

template <typename... Args>
inline Chunk* Arena::new_chunk(Args&&... args)
{
	if (UNLIKELY(m_free_chunks.empty())) {
		return &m_chunks.emplace_back(std::forward<Args>(args)...);
	}
	else {
		auto* chunk = m_free_chunks.back();
		m_free_chunks.pop_back();
		return new (chunk) Chunk {std::forward<Args>(args)...};
	}
}
inline void Arena::free_chunk(Chunk* chunk)
{
	m_free_chunks.push_back(chunk);
}
inline Chunk* Arena::find_chunk(PointerType ptr)
{
	for (auto& chunk : m_chunks) {
		if (!chunk.free && chunk.data == ptr)
			return &chunk;
	}
	return nullptr;
}

inline Arena::PointerType Arena::malloc(size_t size)
{
    const size_t length = word_align(size);
    Chunk* ch = base_chunk().find_free(size);

    if (ch != nullptr) {
        ch->split_next(*this, length);
		ch->free = false;
		return ch->data;
    }
	return 0;
}

inline int Arena::free(PointerType ptr)
{
    Chunk* ch = base_chunk().find(ptr);
    if (UNLIKELY(ch == nullptr))
		return -1;

    ch->free = true;
	// merge chunks ahead and behind us
    if (ch->next && ch->next->free) {
		if (ch->next == last_chunk)
			last_chunk = ch;
        ch->merge_next(*this);
    }
    if (ch->prev && ch->prev->free) {
		ch = ch->prev;
		if (ch->next == last_chunk)
			last_chunk = ch;
        ch->merge_next(*this);
    }
	return 0;
}

inline Arena::Arena(PointerType arena_base, PointerType arena_end)
{
	m_base_chunk.size = arena_end - arena_base;
	m_base_chunk.data = arena_base;
	m_base_chunk.free = true;
}

inline void Arena::foreach(std::function<void(const Chunk&)> callback) const
{
	const Chunk* ch = &this->m_base_chunk;
    while (ch != nullptr) {
		callback(*ch);
		ch = ch->next;
	}
}

inline size_t Arena::bytes_free() const
{
	size_t size = 0;
	foreach([&size] (const Chunk& chunk) {
		if (chunk.free) size += chunk.size;
	});
	return size;
}
inline size_t Arena::bytes_used() const
{
	size_t size = 0;
	foreach([&size] (const Chunk& chunk) {
		if (!chunk.free) size += chunk.size;
	});
	return size;
}

} // namespace foreign_heap
