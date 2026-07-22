#pragma once
#include "core.hpp"
#include "arena.hpp"

// Chunked list: a chain of geometrically growing chunks (64, 128, 256, ...),
// each holding a contiguous run of elements. The collector companion to
// vec::Vec — pushes never relocate, so pointers into the list stay valid for
// the arena's lifetime, and iteration is mostly sequential within a chunk.
// Also serves as a queue (push + pop_front) and a stack (push + pop_back).
// No random indexing.
//
// Nothing is ever freed: chunks spent by pop_front and emptied by pop_back
// are abandoned to the arena, reclaimed when the arena resets.
//
// ZII: the all-zero List is a valid empty list — safe to iterate, query, and
// clear; pushing needs a wired arena (make_list).
//
// Invariant kept by every operation: the chain contains no empty chunks —
// the first chunk has elements past `head`, middle chunks are full, and the
// last chunk is unlinked rather than left at zero. The iterator's simplicity
// depends on this.

namespace list {

template <typename T> struct Chunk {
    Chunk<T>* next;
    usize     len; // used slots; only the first chunk (via head) and the last may be partial
    usize     cap;
    T*        values;
};

// Iterator for range-for: the chunk-hop is what it exists to hide.
// Zero Iter == end.
template <typename T> struct Iter {
    Chunk<T>* chunk;
    usize     index;

    T&   operator*() const { return chunk->values[index]; }
    bool operator!=(Iter other) const { return chunk != other.chunk || index != other.index; }
    void operator++() {
        index += 1;
        if (index == chunk->len) {
            chunk = chunk->next;
            index = 0;
        }
    }
};

template <typename T> struct List {
    arena::Arena* arena;
    Chunk<T>*     first;
    Chunk<T>*     last;
    usize         head; // consumed prefix of the first chunk (pop_front cursor)
    usize         len;  // total live elements

    Iter<T> begin() { return {this->first, this->head}; }
    Iter<T> end() { return {0, 0}; }
};

constexpr usize FIRST_CHUNK_CAP = 64;

template <typename T> fn List<T> make_list(arena::Arena* arena) {
    List<T> result = {};
    result.arena   = arena;
    return result;
}

// Returns the slot the value was stored in — stable for the arena's lifetime.
template <typename T> fn T* push(List<T>* list, T value) {
    Chunk<T>* last = list->last;
    if (!last || last->len == last->cap) {
        usize     cap   = last ? last->cap * 2 : FIRST_CHUNK_CAP;
        Chunk<T>* chunk = arena::allocate<Chunk<T>>(list->arena);
        chunk->cap      = cap;
        chunk->values   = arena::allocate<T>(list->arena, cap);
        if (last) {
            last->next = chunk;
        } else {
            list->first = chunk;
        }
        list->last = chunk;
        last       = chunk;
    }
    T* slot = &last->values[last->len];
    *slot   = value;
    last->len += 1;
    list->len += 1;
    return slot;
}

// Oldest element, or null when empty.
template <typename T> fn T* front(List<T>* list) {
    return list->first ? &list->first->values[list->head] : 0;
}

// Newest element, or null when empty.
template <typename T> fn T* back(List<T>* list) {
    return list->last ? &list->last->values[list->last->len - 1] : 0;
}

template <typename T> fn void pop_front(List<T>* list) {
    ASSERT(list->len > 0);
    list->head += 1;
    list->len -= 1;
    if (list->head == list->first->len) {
        list->first = list->first->next; // spent chunk abandoned to the arena
        list->head  = 0;
        if (!list->first) list->last = 0;
    }
}

template <typename T> fn void pop_back(List<T>* list) {
    ASSERT(list->len > 0);
    Chunk<T>* last = list->last;
    last->len -= 1;
    list->len -= 1;
    bool empty = last == list->first ? last->len == list->head : last->len == 0;
    if (!empty) return;
    if (last == list->first) {
        list->first = 0;
        list->last  = 0;
        list->head  = 0;
    } else {
        // Unlink the emptied tail chunk; the chain is short (doubling sizes),
        // so the walk to its predecessor is a few hops.
        Chunk<T>* chunk = list->first;
        while (chunk->next != last) chunk = chunk->next;
        chunk->next = 0;
        list->last  = chunk;
    }
}

// Drops all elements; chunks are abandoned to the arena, the wiring stays.
template <typename T> fn void clear(List<T>* list) {
    list->first = 0;
    list->last  = 0;
    list->head  = 0;
    list->len   = 0;
}
} // namespace list
