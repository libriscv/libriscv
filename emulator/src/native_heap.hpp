//
// C++ Header-Only Separate Address-Space Allocator
// by fwsGonzo, based on original allocator written in C by Snaipe
//
#pragma once
#include <unistd.h>
#include <errno.h>

namespace sas_alloc
{
struct Chunk
{
	using PointerType = uint32_t;

    Chunk* next = nullptr;
	Chunk* prev = nullptr;
    size_t size = 0;
    bool   free = false;
    PointerType data = 0;

	Chunk* find(PointerType ptr);
	Chunk* find_free(size_t size);
	void   merge_next();
	void   split_next(size_t size);
};

struct Arena
{
	using PointerType = Chunk::PointerType;
	Arena(PointerType base, PointerType end);
	~Arena();

	PointerType malloc(size_t size);
	signed int  free(PointerType);

	inline Chunk& base_chunk() {
		static Chunk bc;
	    return bc;
	}

	size_t      total_chunks = 0;

private:
	PointerType increment(size_t size);
	void        decrement(size_t size);
	inline size_t word_align(size_t size) {
	    return size + ((sizeof(size_t) - 1) & ~(sizeof(size_t) - 1));
	}

	PointerType arena_base;
	PointerType arena_current;
	PointerType arena_end;
	Chunk*      last_chunk = nullptr;
};

// find exact free chunk that matches ptr
inline Chunk* Chunk::find(PointerType ptr)
{
    Chunk* ch = this;
    while (ch != nullptr && !ch->free) {
		if (ch->data == ptr) return ch;
		ch = ch->next;
	}
    return nullptr;
}
// find free chunk that has at least given size
inline Chunk* Chunk::find_free(size_t size)
{
    Chunk* ch = this;
	while (ch != nullptr && ch->free) {
		if (ch->size >= size) return ch;
		ch = ch->next;
	}
    return nullptr;
}
// merge this and next into this chunk
inline void Chunk::merge_next()
{
	auto* freech = this->next;
    this->size += freech->size;
    this->next = freech->next;
    if (this->next) {
        this->next->prev = this;
    }
	delete freech;
}

inline void Chunk::split_next(size_t size)
{
	Chunk* newch = new Chunk({
		.next = this->next,
		.prev = this,
		.size = this->size - size,
		.free = true,
		.data = this->data + (PointerType) size
	});
    if (this->next) {
        this->next->prev = newch;
    }
    this->next = newch;
    this->size = size;
}

inline Arena::PointerType Arena::malloc(size_t size)
{
    if (!size) return 0;
    size_t length = word_align(size);
    Chunk* ch = base_chunk().find_free(size);
    if (ch == nullptr)
	{
		Chunk::PointerType data = this->increment(length);
		if (data == 0) return 0;

        Chunk* newch = new Chunk({
			.next = nullptr,
			.prev = this->last_chunk,
			.size = length,
			.free = false,
			.data = data
		});
        this->last_chunk->next = newch;
		this->last_chunk = newch;
		this->total_chunks++;
		return newch->data;
    } else if (length < ch->size) {
        ch->split_next(length);
		this->total_chunks++;
    }
    ch->free = false;
    return ch->data;
}

inline int Arena::free(PointerType ptr)
{
    if (ptr == 0 || ptr < arena_base || ptr >= arena_end)
		return -1;

    Chunk* ch = base_chunk().find(ptr);
    if (ch->data != ptr)
		return -1;

    ch->free = true;
	// merge chunks ahead and behind us
    if (ch->next && ch->next->free) {
		if (ch->next == last_chunk) last_chunk = ch;
        ch->merge_next();
		this->total_chunks --;
    }
    if (ch->prev->free) {
		ch = ch->prev;
		if (ch->next == last_chunk) last_chunk = ch;
        ch->merge_next();
		this->total_chunks --;
    }
	// if we are the last chunk, give back memory
    if (!ch->next) {
        ch->prev->next = nullptr;
		this->decrement(ch->size);
		delete ch;
		this->total_chunks --;
    }
	return 0;
}

inline Arena::PointerType Arena::increment(size_t size)
{
	if (arena_end - arena_current >= size) {
		PointerType ptr = arena_current;
		arena_current += size;
		return ptr;
	}
	return 0;
}
inline void Arena::decrement(size_t size)
{
	arena_current -= size;
	assert(arena_current >= arena_base);
}

inline Arena::Arena(PointerType base, PointerType end)
	: arena_base(base), arena_current(base), arena_end(end)
{
	this->last_chunk = &base_chunk();
}
inline Arena::~Arena()
{
	Chunk* ch = base_chunk().next;
	while (ch != nullptr) {
		auto* next = ch->next;
		delete ch;
		ch = next;
		this->total_chunks --;
	}
	assert(total_chunks == 0);
}

} // namespace foreign_heap
