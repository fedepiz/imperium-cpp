#pragma once
#include <string.h>

#include "core.hpp"
#include "arena.hpp"

// Arena-backed growable array. ZII: the all-zero Vec is a valid empty vec —
// safe to iterate, index-check, and clear; pushing needs a wired arena
// (asserted). Nothing is freed on growth: when the block is the arena's most
// recent allocation it is extended in place, otherwise a fresh block is
// allocated and the old one stays behind until the arena is reset — bulk
// lifetime as usual.

namespace vec {

template <typename T> struct Vec {
    arena::Arena* arena;
    usize         len;
    usize         capacity;
    T*            data;

    T* begin() { return this->data; }
    T* end() { return this->data + this->len; }

    const T* begin() const { return this->data; }
    const T* end() const { return this->data + this->len; }

    T& operator[](usize idx) {
        ASSERT(idx < this->len);
        return this->data[idx];
    }

    const T& operator[](usize idx) const {
        ASSERT(idx < this->len);
        return this->data[idx];
    }
};

// Internal — callers go through push. A null or exhausted arena is a
// budgeting bug: no null checks, growing on one faults right here.
template <typename T> fn void grow(Vec<T>* vec, usize new_capacity) {
    arena::Arena* a = vec->arena;
    // In-place extension: sizeof(T) is a multiple of alignof(T), so when our
    // block ends at the arena watermark the next allocation starts exactly
    // there and the new capacity is contiguous with the existing elements.
    u8* tail = (u8*)(vec->data + vec->capacity);
    if (vec->data && tail == a->base + a->used) {
        arena::allocate_raw(a, (new_capacity - vec->capacity) * sizeof(T), alignof(T));
        vec->capacity = new_capacity;
        return;
    }
    T* new_data = arena::allocate<T>(a, new_capacity);
    if (vec->len) memcpy(new_data, vec->data, vec->len * sizeof(T));
    vec->data     = new_data;
    vec->capacity = new_capacity;
}

template <typename T> fn Vec<T> make_vec(arena::Arena* arena, usize capacity) {
    Vec<T> result   = {};
    result.arena    = arena;
    result.capacity = capacity;
    result.data     = arena::allocate<T>(arena, capacity);
    if (!result.data) result.capacity = 0; // ZII: full/zero arena -> valid empty vec
    return result;
}

template <typename T> fn Vec<T> make_vec(arena::Arena* arena, Slice<T> slice) {
    Vec<T> result = make_vec<T>(arena, slice.len);
    if (result.capacity < slice.len) return result; // allocation failed — empty vec
    if (slice.len) memcpy(result.data, slice.data, slice.len * sizeof(T));
    result.len = slice.len;
    return result;
}

template <typename T> fn void push(Vec<T>* vec, T value) {
    if (vec->len == vec->capacity) grow(vec, vec->capacity ? vec->capacity * 2 : 8);
    vec->data[vec->len] = value;
    vec->len += 1;
}

template <typename T> fn void push_all(Vec<T>* vec, Slice<T> items) {
    usize needed = vec->len + items.len;
    if (needed > vec->capacity) {
        usize new_capacity = vec->capacity ? vec->capacity * 2 : 8;
        while (new_capacity < needed) new_capacity *= 2;
        grow(vec, new_capacity);
    }
    if (items.len) memcpy(vec->data + vec->len, items.data, items.len * sizeof(T));
    vec->len = needed;
}

// String is not Slice<char> (its bytes are const); char builders still append
// it — the source is only read.
fn void push_all(Vec<char>* vec, String items) {
    push_all(vec, Slice<char>{items.len, (char*)items.data});
}

// View of the current contents; grows/pushes invalidate it (the vec may
// relocate), so take it when the vec is done being built.
template <typename T> fn Slice<T> slice(const Vec<T> vec) {
    return {vec.len, vec.data};
}

template <typename T> fn void pop(Vec<T>* vec) {
    ASSERT(vec->len > 0);
    vec->len -= 1;
}

template <typename T> fn void clear(Vec<T>* vec) {
    vec->len = 0;
}

} // namespace vec
