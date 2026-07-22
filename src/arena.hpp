#pragma once
#include <string.h>

#include "core.hpp"

// Basic arena implementation, relying on the virtual memory committing
// interface: reserve() claims address space once, allocation commits pages on
// demand as `used` grows, and freeing is bulk only — reset() rewinds,
// release() returns the whole range. All allocated memory is zeroed (ZII).
#include "vmem.hpp"

namespace arena {

struct Arena {
    u8*   base; // null = empty/failed arena; every function is safe on it (ZII)
    usize size; // reserved capacity, COMMIT_CHUNK-aligned
    usize used;
    usize committed; // bytes backed by read/write pages, COMMIT_CHUNK-aligned
};

// NOT POD — has a real destructor: captures the arena's watermark on
// construction and restores it at scope exit, so everything allocated
// inside the scope is reclaimed even on early return. Stack-local only —
// never store, serialize, or arena-allocate one. Copies are deleted: a
// copy's destructor would restore the watermark early, clobbering later
// allocations.
struct ScratchArena {
    Arena* arena;
    usize  saved_used;

    ScratchArena() = default; // all-zero state stays valid (ZII): destructor no-ops
    ScratchArena(Arena* arena) : arena{arena}, saved_used{arena->used} {}
    ~ScratchArena() {
        if (arena) arena->used = saved_used;
    }
    ScratchArena(const ScratchArena&)            = delete;
    ScratchArena& operator=(const ScratchArena&) = delete;
};

namespace {

// A multiple of every page size we run on (4K, and 16K on Apple silicon), so
// commit ranges stay page-aligned without querying the OS.
constexpr usize COMMIT_CHUNK = 64 * 1024;

fn usize align_up(usize value, usize align) { return (value + align - 1) & ~(align - 1); }

} // namespace

fn void reserve(Arena* arena, usize capacity) {
    *arena   = {};
    capacity = align_up(capacity, COMMIT_CHUNK);
    u8* base = vmem::reserve(capacity);
    if (!base) return; // ZII: arena stays zero — allocating on it returns null
    arena->base = base;
    arena->size = capacity;
}

fn void release(Arena* arena) {
    if (arena->base) vmem::release(arena->base, arena->size);
    *arena = {};
}

fn u8* allocate_raw(Arena* arena, usize length, usize align) {
    ASSERT(align != 0 && (align & (align - 1)) == 0);
    if (length == 0) return 0;
    usize offset = align_up(arena->used, align);
    if (offset > arena->size || length > arena->size - offset) return 0; // ZII: no space -> null

    usize new_used = offset + length;
    if (new_used > arena->committed) {
        usize commit_end = align_up(new_used, COMMIT_CHUNK);
        vmem::commit(arena->base + arena->committed, commit_end - arena->committed);
        arena->committed = commit_end;
    }

    u8* result  = arena->base + offset;
    arena->used = new_used;
    // Fresh pages arrive zeroed, but ranges reused after reset()/temp_end() don't.
    memset(result, 0, length);
    return result;
}

fn void reset(Arena* arena) {
    arena->used = 0; // pages stay committed for reuse
}

template <typename T> fn T* allocate(Arena* arena, usize count = 1) {
    return (T*)allocate_raw(arena, sizeof(T) * count, alignof(T));
}

template <typename T> fn Slice<T> make_slice(Arena* arena, usize count) {
    return {count, allocate<T>(arena, count)};
}

template <typename T> fn Slice<T> clone_slice(Arena* arena, Slice<T> slice) {
    auto out = make_slice<T>(arena, slice.len);
    for (usize i = 0; i < slice.len; ++i) {
        out.data[i] = slice.data[i];
    }
    return out;
}

// Copy a view into the arena. Strings are immutable, so this is the one way
// to make one whose bytes the arena owns; builders that need to write bytes
// use make_slice<char> and convert on the way out.
fn String clone_string(Arena* arena, String s) {
    if (s.len == 0) return {};
    Slice<char> copy = make_slice<char>(arena, s.len);
    if (!copy.data) return {}; // ZII: arena full -> empty string
    memcpy(copy.data, s.data, s.len);
    return copy;
}

} // namespace arena
