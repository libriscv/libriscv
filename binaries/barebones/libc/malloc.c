//
// As written by Snaipe @ https://github.com/Snaipe/malloc
//
#include <unistd.h>
#include <errno.h>
extern void* sbrk(intptr_t);

static inline size_t word_align(size_t size) {
    return size + ((sizeof(size_t) - 1) & ~(sizeof(size_t) - 1));
}

struct chunk {
    struct chunk *next, *prev;
    size_t        size;
    int           free;
    void         *data;
};

typedef struct chunk *Chunk;

static void *malloc_base() {
    static Chunk b = NULL;
    if (!b) {
        b = sbrk(word_align(sizeof(struct chunk)));
        if (b == (void*) -1) {
            _exit(127);
        }
        b->next = NULL;
        b->prev = NULL;
        b->size = 0;
        b->free = 0;
        b->data = NULL;
    }
    return b;
}

Chunk malloc_chunk_find(size_t s, Chunk *heap) {
    Chunk c = malloc_base();
    for (; c && (!c->free || c->size < s); *heap = c, c = c->next);
    return c;
}

void malloc_merge_next(Chunk c) {
    c->size = c->size + c->next->size + sizeof(struct chunk);
    c->next = c->next->next;
    if (c->next) {
        c->next->prev = c;
    }
}

void malloc_split_next(Chunk c, size_t size) {
    Chunk newc = (Chunk)((char*) c + size);
    newc->prev = c;
    newc->next = c->next;
    newc->size = c->size - size;
    newc->free = 1;
    newc->data = newc + 1;
    if (c->next) {
        c->next->prev = newc;
    }
    c->next = newc;
    c->size = size - sizeof(struct chunk);
}

void *malloc(size_t size) {
    if (!size) return NULL;
    size_t length = word_align(size + sizeof(struct chunk));
    Chunk prev = NULL;
    Chunk c = malloc_chunk_find(size, &prev);
    if (!c) {
        Chunk newc = sbrk(length);
        if (newc == (void*) -1) {
            return NULL;
        }
        newc->next = NULL;
        newc->prev = prev;
        newc->size = length - sizeof(struct chunk);
        newc->data = newc + 1;
        prev->next = newc;
        c = newc;
    } else if (length + sizeof(size_t) < c->size) {
        malloc_split_next(c, length);
    }
    c->free = 0;
    return c->data;
}

void free(void *ptr) {
    if (!ptr || ptr < malloc_base() || ptr > sbrk(0)) return;
    Chunk c = (Chunk) ptr - 1;
    if (c->data != ptr) return;
    c->free = 1;

    if (c->next && c->next->free) {
        malloc_merge_next(c);
    }
    if (c->prev->free) {
        malloc_merge_next(c = c->prev);
    }
    if (!c->next) {
        c->prev->next = NULL;
        sbrk(- c->size - sizeof(struct chunk));
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t length = nmemb * size;
    void *ptr = malloc(length);
    if (ptr) {
        char *dst = ptr;
        for (size_t i = 0; i < length; *dst = 0, ++dst, ++i);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    void *newptr = malloc(size);
    if (newptr && ptr && ptr >= malloc_base() && ptr <= sbrk(0)) {
        Chunk c = (Chunk) ptr - 1;
        if (c->data == ptr) {
            size_t length = c->size > size ? size : c->size;
            char *dst = newptr, *src = ptr;
            for (size_t i = 0; i < length; *dst = *src, ++src, ++dst, ++i);
            free(ptr);
        }
    }
    return newptr;
}
